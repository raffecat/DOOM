
OS_NAME := $(shell uname -s | tr A-Z a-z)
ifeq ($(OS_NAME),linux)

$(info Linux build.)
SDL_I := $(shell sdl2-config --cflags)
SDL_L := $(shell sdl2-config --libs)
ifeq ($(SDL_I),)
$(error "sdl2-config --cflags" didn't return anything - is SDL2 installed?)
endif

else ifeq ($(OS_NAME),darwin)

$(info Mac OS build.)
BREW := $(shell brew --prefix)
ifeq ($(BREW),)
$(error "brew --prefix" didn't return a path - is Homebrew installed?)
endif
SDL := $(shell brew --prefix sdl2)
ifeq ($(SDL),)
$(error "brew --prefix sdl2" didn't return a path - is SDL2 installed?)
endif
SDL_I := -I$(SDL)/include
SDL_L := -L$(SDL)/lib

endif


$(error this is a work in progress, just run ./make)


O=build/thirdparty
OD=build/doom

SUBDIRS= linuxdoom-1.10

# Subdir makefile build rule, generates DOOM_OBJS
$(SUBDIRS)/Makefile:
	$(MAKE) -C $@ print-objects > $@

# Trigger the rule above
# include $(SUBDIRS)/Makefile

# Define phony targets for 'all' and 'clean'
.PHONY: all clean mkdirs $(SUBDIRS)

CC= clang  # gcc or g++ or clang
CFLAGS+=-DNORMALUNIX -DOPLTYPE_IS_OPL3
LDFLAGS+=$(SDL_L)
LIBS+=-lm -lSDL2 # -lnsl
BIN=.

# Use a wildcard to find all object files in the subdirectories
OBJS := $(O)/musplayer.o	\
	$(O)/opl.o		\
	$(O)/qrt_system.o	\
	$(O)/qrt_services.o

# Default target: builds all subdirectories
all:	mkdirs $(SUBDIRS) $(BIN)/doom

# Rule to clean all subdirectories
clean:
	rm -rf ./build

$(O)/musplayer.o:	thirdparty/musplayer/musplayer.c
	$(CC) $(CFLAGS) -c $< -o $@

$(O)/opl.o:		thirdparty/woody-opl/opl.c
	$(CC) $(CFLAGS) -c $< -o $@

$(O)/qrt_%.o:		thirdparty/platform/qrt_%.c
	$(CC) $(CFLAGS) $(SDL_I) -c $< -o $@

$(BIN)/doom: $(OBJS) $(DOOM_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $(BIN)/doom $(LIBS)

# Enter each subdirectory and run 'make all'
$(SUBDIRS):
	$(MAKE) -C $@ all O=../$(OD) \
		CFLAGS="$(CFLAGS) -I../thirdparty/platform -I../thirdparty/musplayer"

mkdirs:
	@mkdir -p $(OD)
	@mkdir -p $(O)
