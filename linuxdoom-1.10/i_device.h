#include "platform.h"

// DOOM device capabilities
enum DOOM_devices {
    ddev_sys      = 0,
    ddev_fb       = 2,
    ddev_sound    = 3,
    ddev_input    = 4,
    ddev_palette  = 5,
    ddev_savebuf  = 8,
    ddev_mixbuf   = 9,
    ddev_musicbuf = 10,
    ddev_main_q   = 11,
    ddev_sound_q  = 12,
};

// DOOM IO Area Map (4K pages)
enum DOOM_IO_map {
    dio_main_q = 0,
    dio_sound_q = 1,
    dio_sbuf_1 = 2,
    dio_sbuf_2 = 3,
    dio_fb_1   = 4,
    dio_fb_2   = 5,
};
