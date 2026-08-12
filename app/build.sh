#!/usr/bin/env bash
# Builds the standalone GD Level Player app (fetch-by-ID + minimalist playback,
# powered by gdsim). Run from anywhere; paths are relative to this script.
set -e
cd "$(dirname "$0")"

RAYLIB_DIR="vendor/raylib"
MINIZ_DIR="vendor/miniz"

g++ -std=c++23 -O2 -DNDEBUG \
    -I "$RAYLIB_DIR/include" -I "$MINIZ_DIR" -I ../src \
    src/main.cpp src/network.cpp src/leveldata.cpp src/render.cpp src/exportdialog.cpp \
    ../src/sim/*.cpp \
    "$MINIZ_DIR/miniz.c" "$MINIZ_DIR/miniz_tinfl.c" "$MINIZ_DIR/miniz_tdef.c" "$MINIZ_DIR/miniz_zip.c" \
    -L "$RAYLIB_DIR/lib" -lraylib \
    -lopengl32 -lgdi32 -lwinmm -lwinhttp -lcomdlg32 \
    -static -static-libstdc++ -static-libgcc -mwindows \
    -o gdplayer.exe

echo "built: app/gdplayer.exe"
