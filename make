#!/usr/bin/env bash

SDL=/opt/homebrew/opt/sdl2
OPL=woody-opl    # use -DOPLTYPE_IS_OPL3 (needs OPL3)
#OPL=Nuked-OPL3  # use -DOPL_NUKED

clang -g -Wall -DNORMALUNIX=1 -DOPLTYPE_IS_OPL3 \
  -Ithirdparty/platform \
  -Ithirdparty/musplayer \
  -Ithirdparty/$OPL \
  -Ilinuxdoom-1.10 \
  -I$SDL/include \
  -L$SDL/lib \
  -lSDL2 \
  linuxdoom-1.10/*.c \
  thirdparty/platform/*.c \
  thirdparty/musplayer/*.c \
  thirdparty/$OPL/*.c \
  -o doom
