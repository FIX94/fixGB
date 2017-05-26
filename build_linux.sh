#!/bin/sh
gcc main.c mbc.c apu.c audio.c alhelpers.c cpu.c mem.c ppu.c input.c -DFREEGLUT_STATIC -lglut -lopenal -lGL -lGLU -lm -Wall -Wextra -O3 -msse -mfpmath=sse -ffast-math -s -o fixGB
