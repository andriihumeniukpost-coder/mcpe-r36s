#!/bin/sh
# stb_image (public domain) decodes the game's PNG textures.
set -e
cd "$(dirname "$0")/.."
[ -f include/stb_image.h ] || curl -fsSL -o include/stb_image.h \
  https://raw.githubusercontent.com/nothings/stb/master/stb_image.h
