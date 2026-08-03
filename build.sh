#!/bin/bash
# Builds the overlay. Run from Git Bash on Windows with CMake and Visual Studio
# installed. The resulting exe has no runtime dependencies.
set -e

if [ ! -d "build" ]; then
    cmake -S . -B build/
fi

cmake --build build --config Release

echo
echo "built: build/Release/rline.exe (+ lap.csv beside it)"
