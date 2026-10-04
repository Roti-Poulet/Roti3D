@echo off
x86_64-w64-mingw32-gcc -O2 -std=gnu11 -mwindows main.c -o engine.exe -ld3d9 -lgdi32 -lcomctl32 -luser32 -ladvapi32
