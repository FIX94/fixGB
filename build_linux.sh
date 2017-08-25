#!/bin/sh
gcc -DZIPSUPPORT main.c mbc.c apu.c audio.c alhelpers.c cpu.c mem.c ppu.c input.c unzip/*.c -DFREEGLUT_STATIC -lglut -lopenal -lGL -lGLU -lm -lz -Wall -Wextra -O3 -flto -s -o fixGB
