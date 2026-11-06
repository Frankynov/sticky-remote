#!/bin/sh
# Builds the command-line engine (engine/build/sim): the firmware's UI code
# (firmware/remote_ui.h) compiled for this computer, plus fallback glyphs.
# The web wizard's preview scripts use it.
set -e
cd "$(dirname "$0")/.."
mkdir -p engine/build
python3 engine/make_fonts.py
clang -O2 -c engine/core/qrcodegen/qrcodegen.c -o engine/build/qrcodegen.o
clang++ -std=c++17 -O2 -Wall -Wno-unused-function \
  -I engine/stubs -I firmware -I engine/core/include -I engine/core/qrcodegen \
  engine/core/sticky_core.cpp engine/sim.cpp engine/build/qrcodegen.o -o engine/build/sim
echo "built engine/build/sim"
