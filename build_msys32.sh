#!/bin/sh
gcc -DWINDOWS_BUILD -DZIPSUPPORT main.c mbc.c apu.c audio.c alhelpers.c cpu.c mem.c ppu.c input.c unzip/*.c -DFREEGLUT_STATIC -lfreeglut_static -lopenal32 -lopengl32 -lglu32 -lgdi32 -lwinmm -lm -lz -Wall -Wextra -O3 -flto -s -o fixGB
