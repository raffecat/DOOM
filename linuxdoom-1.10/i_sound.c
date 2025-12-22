// Emacs style mode select   -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// The source is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// FITNESS FOR A PARTICULAR PURPOSE. See the DOOM Source Code License
// for more details.
//
// $Log:$
//
// DESCRIPTION:
//	System interface for sound.
//
//-----------------------------------------------------------------------------

static const char __attribute__((unused))
rcsid[] = "$Id: i_unix.c,v 1.5 1997/02/03 22:45:10 b1 Exp $";

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include <math.h>

#include <sys/time.h>
#include <sys/types.h>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "z_zone.h"

#include "i_system.h"
#include "i_sound.h"
#include "m_argv.h"
#include "m_misc.h"
#include "w_wad.h"

#include "doomdef.h"
#include "i_device.h"

#include "musplayer.h"

#if defined(OPL_NUKED)
#include "opl3.h"
#else
void adlib_init(uint32_t samplerate);
void adlib_getsample(int16_t* sndptr, intptr_t numsamples);
#endif


// The number of internal mixing channels,
//  the samples calculated for each mixing step,
//  the size of the 16bit, 2 hardware channel (stereo)
//  mixing buffer, and the samplerate of the raw data.

// Number of mixer channels.
#define NUM_CHANNELS		8
// Power of two >= the number of mixer channels (bitmasks)
#define NUM_CHANNELS_POW2	8
// Number of output channels. 2 for Stereo (OPL3 requires 2)
#define MIX_CHANNELS            2

// 11025  22050  44100  (49716)
#define SAMPLERATE		22050

// 140 Hz music tick rate
// 22050 / 140 = 157.5 (ideal chunk size) [192]
// 24000 / 140 = 171.4                    [256]
// 44100 / 140 = 315.0                    [384]
#define CHUNK_SIZE              192

// Sfx step shift (rate divider) 0=11025 1=22050 2=44100
#define STEP_SHIFT              1


// --------------------------------------------------------------------------
// MIXER THREAD BEGINS

// All code must LOCK `sfx_mutex` to access the data in this zone.

static mutex_t sfx_mutex = {0};

static void* sfx_data[NUMSFX] = {0};
static int sfx_length[NUMSFX] = {0};

// Basically, samples from all active internal channels
//  are modifed and added, and stored in the buffer
//  that is submitted to the audio device.

static size_t           mix_samplecount = 0;

// The channel step amount...
static unsigned int	channelstep[NUM_CHANNELS];
// ... and a 0.16 bit remainder of last step.
static unsigned int	channelstepremainder[NUM_CHANNELS];

// The channel data pointers, start and end.
static unsigned char*	channels[NUM_CHANNELS];
static unsigned char*	channelsend[NUM_CHANNELS];

// Time/gametic that the channel started playing,
//  used to determine oldest, which automatically
//  has lowest priority.
// In case number of active sounds exceeds
//  available channels.
static int		channelstart[NUM_CHANNELS];

// The channel handle, assigned when the sound starts.
//  used to stop/modify the sound.
static unsigned int 	channelhandles[NUM_CHANNELS];

// SFX id of the playing sound effect.
// Used to catch duplicates (like chainsaw).
static int		channelids[NUM_CHANNELS];			

// Pitch to stepping lookup, unused.
static int		steptable[256];

// Volume lookups.
static int		vol_lookup[128*256];

// Hardware left and right channel volume lookup.
static int*		channelleftvol_lookup[NUM_CHANNELS];
static int*		channelrightvol_lookup[NUM_CHANNELS];

static unsigned int     nexthandle = 0;


// Music stuff.

// OPL3 generates a stereo pair for each sample.
static int16_t*         music_buffer = 0; // dynamic size

// Derived 0-127 volume used in mixing.
static int 		music_volume = 127;

// Time tracking for the music player, 140 ticks per second.
static int      	music_lasttic = 0;

// Game has started the music playing.
static int 		music_playing = 0;

// Game has paused music (network stall?)
static int 		music_paused = 0;

#if defined(OPL_NUKED)
static opl3_chip music_opl3 = {0};

// On the mixer thread.
void adlib_write(int reg, int val) {
    OPL3_WriteReg(&music_opl3, reg, val);
}
#endif


// On the mixer thread with LOCK held.
static void mix_music(int musvol, int sample_count) {
	// advance the score, update adlib state.
	int thistic = I_GetSoundTime(); // in 1/140ths (140 Hz)
	int numtics = thistic - music_lasttic;
	music_lasttic = thistic;
	if (numtics > 0) {
		music_playing = musplay_update(numtics);
	}
	// pull some samples from OPL.
	if (musvol) {
#if defined(OPL_NUKED)
		OPL3_GenerateStream(&music_opl3, music_mixbuffer, sample_count);
#else
		adlib_getsample(music_buffer, sample_count);
#endif
	}
}

//
// This function loops all active (internal) sound
//  channels, retrieves a given number of samples
//  from the raw sound data, modifies it according
//  to the current (internal) channel parameters,
//  mixes the per channel samples into the global
//  mixbuffer, clamping it to the allowed range,
//  and sets up everything for transferring the
//  contents of the mixbuffer to the (two)
//  hardware channels (left and right, that is).
//
// This function currently supports only 16bit.
//
// On the mixer thread, called by Audio device. Acquires LOCK.
//
static void mix_next_chunk( void* userdata, uint8_t* buffer, int buffer_size )
{
  // Mix current sound data.
  // Data, from raw sound, for right and left.
  register unsigned int	sample;
  register int		dl;
  register int		dr;
  
  // Pointers in global mixbuffer, left, right, end.
  signed short*		leftout;
  signed short*		rightout;
  signed short*		leftend;
  // Step in mixbuffer, left and right, thus two.
  int			step;

  // Mixing channel index.
  int			chan;

  // Music stuff.
  int                   musvol;
  int                   music_on;
  int16_t*              musicsample;
  int			clipped;
    
  int16_t*              mixbuffer = (int16_t*)buffer;

    Mutex_Lock(&sfx_mutex);

    musvol = music_volume;
    music_on = music_playing && !music_paused;
    musicsample = music_buffer;
    clipped = 0;

    if (music_on) {
	mix_music(musvol, mix_samplecount);
    }

    // Left and right channel
    //  are in global mixbuffer, alternating.
    leftout = mixbuffer;
    rightout = mixbuffer+1;
    step = 2;

    // Determine end, for left channel only
    //  (right channel is implicit).
    leftend = mixbuffer + mix_samplecount*step;

    // Mix sounds into the mixing buffer.
    // Loop over step*samplecount,
    //  that is 512 values for two channels.
    while (leftout != leftend)
    {
	// Reset left/right value. 
	dl = 0;
	dr = 0;

	// Love thy L2 chache - made this a loop.
	// Now more channels could be set at compile time
	//  as well. Thus loop those  channels.
	for ( chan = 0; chan < NUM_CHANNELS; chan++ )
	{
	    // Check channel, if active.
	    if (channels[ chan ])
	    {
		// Get the raw data from the channel. 
		sample = *channels[ chan ];
		// Add left and right part
		//  for this channel (sound)
		//  to the current data.
		// Adjust volume accordingly.
		dl += channelleftvol_lookup[ chan ][sample];
		dr += channelrightvol_lookup[ chan ][sample];
		// Apply pitch step to offset, 16.16 fixed point.
		channelstepremainder[ chan ] += channelstep[ chan ];
		// Advance by integer part in high 16 bits.
		channels[ chan ] += channelstepremainder[ chan ] >> 16;
		// Keep remainder in low 16 bits.
		channelstepremainder[ chan ] &= 65536-1;

		// Check whether we are done.
		if (channels[ chan ] >= channelsend[ chan ])
		    channels[ chan ] = 0;
	    }
	}

	if (music_on && musvol) {
		int sample = ((*musicsample++) * musvol) >> 7;  // left channel
		dl += sample;
#if defined(OPLTYPE_IS_OPL3) || defined(OPL_NUKED)
		// OPL3 generates a stereo pair for each sample.
		dr += ((*musicsample++) * musvol) >> 7;  // right channel
#else
		dr += sample;
#endif
	}

	// Clamp to range. Left hardware channel.
	// Has been char instead of short.
	// if (dl > 127) *leftout = 127;
	// else if (dl < -128) *leftout = -128;
	// else *leftout = dl;

	if (dl > 0x7fff) {
	    *leftout = 0x7fff;
	    clipped++;
	} else if (dl < -0x8000) {
	    *leftout = -0x8000;
	    clipped++;
	} else {
	    *leftout = dl;
	}

	// Same for right hardware channel.
	if (dr > 0x7fff) {
	    *rightout = 0x7fff;
	    clipped++;
	} else if (dr < -0x8000) {
	    *rightout = -0x8000;
	    clipped++;
	} else {
	    *rightout = dr;
	}

	// Increment current pointers in mixbuffer.
	leftout += step;
	rightout += step;
    }

    Mutex_Unlock(&sfx_mutex);

//     if (clipped > 0) {
// 	printf("[MIXER] clipped %d\n", clipped);
//     }

    // UNUSED
    buffer_size = 0;
}

// --------------------------------------------------------------------------
// MIXER THREAD ENDS



//
// This function loads the sound data from the WAD lump,
//  for single sound.
// On the main thread.
//
static void*
getsfx
( char*         sfxname,
  int*          len )
{
    unsigned char*      sfx;
    unsigned char*      paddedsfx;
    int                 i;
    int                 size;
    int                 paddedsize;
    char                name[20];
    int                 sfxlump;

    
    // Get the sound data from the WAD, allocate lump
    //  in zone memory.
    sprintf(name, "ds%s", sfxname);

    // Now, there is a severe problem with the
    //  sound handling, in it is not (yet/anymore)
    //  gamemode aware. That means, sounds from
    //  DOOM II will be requested even with DOOM
    //  shareware.
    // The sound list is wired into sounds.c,
    //  which sets the external variable.
    // I do not do runtime patches to that
    //  variable. Instead, we will use a
    //  default sound for replacement.
    if ( W_CheckNumForName(name) == -1 )
      sfxlump = W_GetNumForName("dspistol");
    else
      sfxlump = W_GetNumForName(name);
    
    size = W_LumpLength( sfxlump );

    // Debug.
    // fprintf( stderr, "." );
    //fprintf( stderr, " -loading  %s (lump %d, %d bytes)\n",
    //	     sfxname, sfxlump, size );
    //fflush( stderr );
    
    sfx = (unsigned char*)W_CacheLumpNum( sfxlump, PU_STATIC );

    // Pads the sound effect out to the mixing buffer size.
    // The original realloc would interfere with zone memory.
    paddedsize = ((size-8 + (mix_samplecount-1)) / mix_samplecount) * mix_samplecount;

    // Allocate from zone memory.
    paddedsfx = (unsigned char*)Z_Malloc( paddedsize+8, PU_STATIC, 0 );
    // ddt: (unsigned char *) realloc(sfx, paddedsize+8);
    // This should interfere with zone memory handling,
    //  which does not kick in in the soundserver.

    // Now copy and pad.
    memcpy(  paddedsfx, sfx, size );
    for (i=size ; i<paddedsize+8 ; i++)
        paddedsfx[i] = 128;

    // Remove the cached lump.
    Z_Free( sfx );
    
    // Preserve padded length.
    *len = paddedsize;

    // Return allocated padded data.
    return (void *) (paddedsfx + 8);
}


//
// This function adds a sound to the
//  list of currently active sounds,
//  which is maintained as a given number
//  (eight, usually) of internal channels.
// Returns a handle.
// On the main thread with LOCK held.
//
static int addsfx_with_lock
( int		sfxid,
  int		volume,
  int		step,
  int		seperation )
{
    int		i;
    int         handle;
    
    int		oldest = gametic;
    int		oldestnum = 0;
    int		slot;

    int		rightvol;
    int		leftvol;

    // Chainsaw troubles.
    // Play these sound effects only one at a time.
    if ( sfxid == sfx_sawup
	 || sfxid == sfx_sawidl
	 || sfxid == sfx_sawful
	 || sfxid == sfx_sawhit
	 || sfxid == sfx_stnmov
	 || sfxid == sfx_pistol	 )
    {
	// Loop all channels, check.
	for (i=0 ; i<NUM_CHANNELS ; i++)
	{
	    // Active, and using the same SFX?
	    if ( (channels[i])
		 && (channelids[i] == sfxid) )
	    {
		// Reset.
		channels[i] = 0;
		// We are sure that iff,
		//  there will only be one.
		break;
	    }
	}
    }

    // Loop all channels to find oldest SFX.
    for (i=0; (i<NUM_CHANNELS) && (channels[i]); i++)
    {
	if (channelstart[i] < oldest)
	{
	    oldestnum = i;
	    oldest = channelstart[i];
	}
    }

    // Tales from the cryptic.
    // If we reached the end, all channels were playing, oldestnum is the oldest.
    // If not, we found a channel that wasn't in use and stopped early.
    if (i == NUM_CHANNELS)
	slot = oldestnum;
    else
	slot = i;

    // Okay, in the less recent channel,
    //  we will handle the new SFX.
    // Set pointer to raw data.
    channels[slot] = (unsigned char *) sfx_data[sfxid];
    // Set pointer to end of raw data.
    channelsend[slot] = channels[slot] + sfx_length[sfxid];

    // Set stepping (pitch)
    channelstep[slot] = step;
    // Initial offset in sample.
    channelstepremainder[slot] = 0;
    // Should be gametic, I presume.
    channelstart[slot] = gametic;

    // Separation, that is, orientation/stereo.
    //  range is: 1 - 256
    seperation += 1;

    // Per left/right channel.
    //  x^2 seperation,
    //  adjust volume properly.
    leftvol =
	volume - ((volume*seperation*seperation) >> 16); ///(256*256);
    seperation = seperation - 257;
    rightvol =
	volume - ((volume*seperation*seperation) >> 16);	

    // Sanity check, clamp volume.
    if (rightvol < 0 || rightvol > 127)
	I_Error("rightvol out of bounds");
    
    if (leftvol < 0 || leftvol > 127)
	I_Error("leftvol out of bounds");
    
    // Get the proper lookup table piece
    //  for this volume level.
    channelleftvol_lookup[slot] = &vol_lookup[leftvol*256];
    channelrightvol_lookup[slot] = &vol_lookup[rightvol*256];

    // Preserve sound SFX id,
    //  e.g. for avoiding duplicates of chainsaw.
    channelids[slot] = sfxid;

    // Handle is next handle number combined with slot index.
    channelhandles[slot] = nexthandle;
    handle = nexthandle | (unsigned int)slot;
    nexthandle += NUM_CHANNELS_POW2; // inc high bits above slot.
    return handle;
}


//
// SFX API
// Note: this was called by S_Init.
// However, whatever they did in the
// old DPMS based DOS version, this
// were simply dummies in the Linux
// version.
// On the main thread, before mixer thread starts. STARTS mixer.
//
void I_SetChannels()
{
  // Init internal lookups (raw data, mixing buffer, channels).
  // This function sets up internal lookups used during
  //  the mixing process. 
  int		i;
  int		j;
    
  int*	steptablemid = steptable + 128;

  // Okay, reset internal mixing channels to zero.
  for (i=0; i<NUM_CHANNELS; i++)
  {
    channels[i] = 0;
  }

  // This table provides step widths for pitch parameters.
  // I fail to see that this is currently used.
  for (i=-128 ; i<128 ; i++)
    steptablemid[i] = (int)(pow(2.0, (i/64.0))*65536.0);
  
  
  // Generates volume lookup tables
  //  which also turn the unsigned samples
  //  into signed samples.
  for (i=0 ; i<128 ; i++)
    for (j=0 ; j<256 ; j++)
      vol_lookup[i*256+j] = (i*(j-128)*256)/127;

  // Find the GENMIDI lump and register instruments.
  int op2lump = W_CheckNumForName("GENMIDI.OP2");
  if ( op2lump == -1 )
    op2lump = W_GetNumForName("GENMIDI");
  char *op2 = W_CacheLumpNum( op2lump, PU_STATIC );
  musplay_op2bank(op2+8); // skip "#OPL_II#" to get BYTE[175][36] instrument data
  Z_Free( op2 );

#if defined(OPL_NUKED)
	OPL3_Reset(&music_opl3, SAMPLERATE);
#else
	adlib_init(SAMPLERATE);
#endif

  // Start the mixer.
  Audio_Start(ddev_sound);
}



// Set sound effects mixer volume.
// On the main thread (API)
void I_SetSfxVolume(int volume) // 0-127
{
  // Identical to DOS.
  // Basically, this should propagate
  //  the menu/config file setting
  //  to the state variable used in
  //  the mixing.
  // This is handled via I_StartSound and I_UpdateSoundParams.
}


// MUSIC API. Some code from DOS version.
// On the main thread. Acquires LOCK.
void I_SetMusicVolume(int volume) // 0-127
{
    if (volume < 0 || volume > 127)
	I_Error("Attempt to set music volume at %d", volume);

  // apply log-scaling to the requested volume.
  // we can't use vol_lookup with 16-bit samples so this will do.
  volume += 7; // a bit of boost at max volume
  volume = (volume * volume) >> 7;

  Mutex_Lock(&sfx_mutex);
  music_volume = volume;
  Mutex_Unlock(&sfx_mutex);
}


//
// Retrieve the raw data lump index
//  for a given SFX name.
// On the main thread.
//
int I_GetSfxLumpNum(sfxinfo_t* sfx)
{
    char namebuf[9];
    sprintf(namebuf, "ds%s", sfx->name);
    return W_GetNumForName(namebuf);
}


//
// Starting a sound means adding it
//  to the current list of active sounds
//  in the internal channels.
// As the SFX info struct contains
//  e.g. a pointer to the raw data,
//  it is ignored.
// As our sound handling does not handle
//  priority, it is ignored.
// Pitching (that is, increased speed of playback)
//  is set, but currently not used by mixing.
//
// On the main thread. Acquires LOCK.
//
int
I_StartSound
( int		id,
  int		vol,
  int		sep,
  int		pitch,
  int		priority )
{
  int           handle;

	Mutex_Lock(&sfx_mutex);

	// Returns a handle, later used for I_UpdateSoundParams
	// Assumes volume in 0..127
	handle = addsfx_with_lock( id, vol, steptable[pitch], sep );

	// fprintf( stderr, "/handle is %d\n", id );

	Mutex_Unlock(&sfx_mutex);

  // UNUSED
  priority = 0;
    
  return handle;
}


// On the main thread. Acquires LOCK.
void I_StopSound (int handle)
{
  unsigned int h = handle; // modern UB.

  // You need the handle returned by StartSound.
  // Would be looping all channels,
  //  tracking down the handle,
  //  an setting the channel to zero.
  
  int slot = h & (NUM_CHANNELS_POW2-1);

  Mutex_Lock(&sfx_mutex);

  // Check if the slot is still playing the same handle.
  if (channels[slot] && channelhandles[slot] == (h & ~(NUM_CHANNELS_POW2-1))) {
	// Reset.
	channels[slot] = 0;
  }

  Mutex_Unlock(&sfx_mutex);
}


// On the main thread. Acquires LOCK.
int I_SoundIsPlaying(int handle)
{
  int playing = 0;
  unsigned int h = handle; // modern UB.

  int slot = h & (NUM_CHANNELS_POW2-1);

  Mutex_Lock(&sfx_mutex);

  // Check if the slot is still playing the same handle.
  if (channels[slot] && channelhandles[slot] == (h & ~(NUM_CHANNELS_POW2-1))) {
	playing = 1;
  }

  Mutex_Unlock(&sfx_mutex);

  return playing;
}


// Not used.
void I_UpdateSound( void )
{
  // Moved to mixer thread.
}


// 
// This would be used to write out the mixbuffer
//  during each game loop update.
// Updates sound buffer and audio device at runtime. 
// Mixing now done synchronous, and
//  only output be done asynchronous?
//
void
I_SubmitSound(void)
{
  // Moved to mixer thread.
}


// On the main thread. Acquires LOCK.
void
I_UpdateSoundParams
( int	handle,
  int	volume,
  int	seperation,
  int	pitch)
{
  int		rightvol;
  int		leftvol;

  unsigned int h = handle; // modern UB.

  // Use the handle to identify
  //  on which channel the sound might be active,
  //  and reset the channel parameters.

  int slot = h & (NUM_CHANNELS_POW2-1);

  Mutex_Lock(&sfx_mutex);

  // Check if the slot is still playing the same handle.
  if (channels[slot] && channelhandles[slot] == (h & ~(NUM_CHANNELS_POW2-1))) {
	
    // Set stepping (pitch)
    channelstep[slot] = steptable[pitch] >> STEP_SHIFT;

    // Separation, that is, orientation/stereo.
    //  range is: 1 - 256
    seperation += 1;

    // Per left/right channel.
    //  x^2 seperation,
    //  adjust volume properly.
    leftvol =
	volume - ((volume*seperation*seperation) >> 16); ///(256*256);
    seperation = seperation - 257;
    rightvol =
	volume - ((volume*seperation*seperation) >> 16);	

    // Sanity check, clamp volume.
    if (rightvol < 0 || rightvol > 127)
	I_Error("rightvol out of bounds");
    
    if (leftvol < 0 || leftvol > 127)
	I_Error("leftvol out of bounds");
    
    // Get the proper lookup table piece
    //  for this volume level.
    channelleftvol_lookup[slot] = &vol_lookup[leftvol*256];
    channelrightvol_lookup[slot] = &vol_lookup[rightvol*256];

  }

   Mutex_Unlock(&sfx_mutex);
}




// On the main thread. STOPS MIXER.
void I_ShutdownSound(void)
{    
  // Wait till all pending sounds are finished.
  // int done = 0;
  // int i;
  

  // FIXME (below).
  fprintf( stderr, "I_ShutdownSound: NOT finishing pending sounds\n");
  fflush( stderr );
  
  // while ( !done )
  // {
  //   for( i=0 ; i<8 && !channels[i] ; i++);
  //   
  //   // FIXME. No proper channel output.
  //   //if (i==8)
  //   done=1;
  // }

  // Stop the audio mixer and mixer thread.  
  Audio_Stop(ddev_sound);

  // Release the Audio device.
  System_DropCapability(ddev_sound);

  // Done.
  return;
}


// On the main thread, before mixer starts.
void
I_InitSound()
{ 
  int i;
  
  // Secure and configure sound device first.
  fprintf( stderr, "I_InitSound: ");

  Mutex_Init(&sfx_mutex);

  Audio_CreateStream(ddev_sound, mix_next_chunk, Audio_Fmt_S16, MIX_CHANNELS, SAMPLERATE, CHUNK_SIZE);
  mix_samplecount = Audio_SampleCount(ddev_sound);
  music_buffer = Buffer_Create(ddev_musicbuf, mix_samplecount*sizeof(int16_t)*2, 0);

  // Initialize external data (all sounds) at start, keep static.
  fprintf( stderr, "I_InitSound: samplecount = %d\n", (int)mix_samplecount);
  
  for (i=1 ; i<NUMSFX ; i++)
  { 
    // Alias? Example is the chaingun sound linked to pistol.
    if (!S_sfx[i].link)
    {
      // Load data from WAD file.
      S_sfx[i].data = sfx_data[i] = getsfx( S_sfx[i].name, &sfx_length[i] );
    }	
    else
    {
      // Previously loaded already?
      S_sfx[i].data = sfx_data[i] = S_sfx[i].link->data;
      sfx_length[i] = sfx_length[(S_sfx[i].link - S_sfx)/sizeof(sfxinfo_t)];
    }
  }

  fprintf( stderr, " pre-cached all sound data\n");

  // Finished initialization.
  fprintf(stderr, "I_InitSound: sound module ready\n");
}




//
// MUSIC API.
//
void I_InitMusic(void)		{ }
void I_ShutdownMusic(void)	{ }

// Only used here to communicate between I_RegisterSong and I_PlaySong
static void*    last_registered_song = 0;

void I_PlaySong(int handle, int loop)
{
  // UNUSED.
  handle = 0;

  if (last_registered_song) {
	Mutex_Lock(&sfx_mutex);
	musplay_start(last_registered_song, loop);
	music_lasttic = I_GetSoundTime();
	music_playing = 1;
	Mutex_Unlock(&sfx_mutex);
  }
}

void I_PauseSong (int handle)
{
  // UNUSED.
  handle = 0;

  Mutex_Lock(&sfx_mutex);
  music_paused = 1;
  Mutex_Unlock(&sfx_mutex);
}

void I_ResumeSong (int handle)
{
  // UNUSED.
  handle = 0;

  Mutex_Lock(&sfx_mutex);
  music_paused = 0;
  Mutex_Unlock(&sfx_mutex);
}

void I_StopSong(int handle)
{
  // UNUSED.
  handle = 0;

  Mutex_Lock(&sfx_mutex);
  music_playing = 0;
  musplay_stop();
  Mutex_Unlock(&sfx_mutex);
}

void I_UnRegisterSong(int handle)
{
  // UNUSED.
  handle = 0;
}

int I_RegisterSong(void* data)
{
  // Always registered just before I_PlaySong.
  // Always unregistered just after I_StopSong.
  // Music lump data. Returns handle.
  last_registered_song = data;
  return 1;
}

// Is the song playing?
int I_QrySongPlaying(int handle)
{
  int playing = 0;

  // UNUSED.
  handle = 0;

  Mutex_Lock(&sfx_mutex);
  playing = music_playing;
  Mutex_Lock(&sfx_mutex);

  return playing;
}
