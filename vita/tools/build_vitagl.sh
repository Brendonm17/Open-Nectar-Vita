#!/bin/bash
# Builds and installs the vitaGL fork this port needs.
# usage: bash vita/tools/build_vitagl.sh [path-to-vitaGL-checkout]
set -e

export VITASDK="${VITASDK:-/usr/local/vitasdk}"
export PATH="$VITASDK/bin:$PATH"

REPO="https://github.com/Brendonm17/vitaGL.git"
BRANCH="async-compressed-tex-prep"
SRC="${1:-$HOME/.cache/nectar_vita/vitaGL}"

if [ ! -d "$SRC/.git" ]; then
    git clone --branch "$BRANCH" "$REPO" "$SRC"
fi
cd "$SRC"

HDR="source/shaders/glsl_translator_hdr.h"
if grep -q 'return M1 \* M2' "$HDR" 2>/dev/null; then
    sed -i 's/return M1 \* M2/return M2 \* M1/g' "$HDR"
fi
if ! grep -q 'DISABLE_ADVANCED_SHADER_CACHE' Makefile; then
    sed -i '/^CFLAGS  = -g -Wl,-q/a CFLAGS += -DDISABLE_ADVANCED_SHADER_CACHE' Makefile
fi

FLAGS="HAVE_GLSL_TEXTURE_SIZE=1 NO_DEBUG=1 \
BUFFERS_SPEEDHACK=1 SAMPLERS_SPEEDHACK=1 PRIMITIVES_SPEEDHACK=1 \
TEXTURES_SPEEDHACK=1 DRAW_STATE_CACHE=1 PHYCONT_ON_DEMAND=1 \
UNIFORMS_SPEEDHACK=1 MATH_SPEEDHACK=1 NO_TEX_COMBINER=1 \
INDICES_DRAW_SPEEDHACK=1 NO_DMAC=1"

make clean >/dev/null 2>&1 || true
make -j"$(nproc)" $FLAGS
make $FLAGS install
