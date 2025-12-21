#!/usr/bin/env bash

OS_NAME=`uname -s | tr A-Z a-z`
if [ "$OS_NAME" = "linux" ]; then
    SDL_I=`sdl2-config --cflags`
    SDL_L=`sdl2-config --libs`
    if [ "$SDL_I" = "" ]; then
        echo "'sdl2-config --cflags' didn't return anything - is SDL2 installed?)"
        exit 1
    fi
elif [ "$OS_NAME" = "darwin" ]; then
    BREW=`brew --prefix`
    if [ "$BREW" = "" ]; then
        echo "'brew --prefix' didn't return a path - is Homebrew installed?)"
        exit 1
    fi
    SDL=`brew --prefix sdl2`
    if [ "$SDL" = "" ]; then
        echo "'brew --prefix sdl2' didn't return a path - is SDL2 installed?)"
        exit 1
    fi
    SDL_I="-I$SDL/include"
    SDL_L="-L$SDL/lib"
fi

OPL=woody-opl    # use -DOPLTYPE_IS_OPL3 (needs OPL3)
#OPL=Nuked-OPL3  # use -DOPL_NUKED

clang -g -Wall -DNORMALUNIX=1 -DOPLTYPE_IS_OPL3 \
  -Ithirdparty/platform \
  -Ithirdparty/musplayer \
  -Ithirdparty/$OPL \
  -Ilinuxdoom-1.10 \
  "$SDL_I" \
  "$SDL_L" \
  -lSDL2 \
  linuxdoom-1.10/*.c \
  thirdparty/platform/*.c \
  thirdparty/musplayer/*.c \
  thirdparty/$OPL/*.c \
  -o doom
