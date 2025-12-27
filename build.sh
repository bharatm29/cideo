#!/bin/env bash
set -x

gcc -o main main.c -Wall -Wextra -lraylib -lm -I/usr/include/ffmpeg -lavcodec -lavformat -lavutil -lswscale -lswresample
