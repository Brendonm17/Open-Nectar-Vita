#!/bin/bash
# Builds nectar_vita.vpk.
# usage: bash vita/build.sh [clean]
# Extra CMake options: CMAKE_ARGS="-DNECTAR_LOGGING=OFF" bash vita/build.sh
set -e

export VITASDK="${VITASDK:-/usr/local/vitasdk}"
export PATH="$VITASDK/bin:$PATH"

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$SRC/vita/build"

# WSL on a Windows drive: build from a Linux-side copy.
if [[ "$SRC" == /mnt/* ]]; then
    WORK="${NECTAR_WORK_DIR:-$HOME/.cache/nectar_vita}"
    mkdir -p "$WORK"
    rsync -a --delete --exclude='.git/' --exclude='build*/' --exclude='orig/' --exclude='*.vpk' "$SRC/" "$WORK/src/"
    BUILD="$WORK/build"
    CMAKE_SRC="$WORK/src/vita"
else
    BUILD="$OUT"
    CMAKE_SRC="$SRC/vita"
fi

[ "$1" = "clean" ] && rm -rf "$BUILD"

if [ ! -f "$BUILD/CMakeCache.txt" ]; then
    GEN="Unix Makefiles"
    command -v ninja >/dev/null && GEN="Ninja"
    cmake -S "$CMAKE_SRC" -B "$BUILD" -G "$GEN" \
          -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
          -DCMAKE_BUILD_TYPE=Release $CMAKE_ARGS
fi
cmake --build "$BUILD" -j"$(nproc)"

if [ "$BUILD" != "$OUT" ]; then
    mkdir -p "$OUT"
    cp "$BUILD/nectar_vita.vpk" "$BUILD/eboot.bin" "$OUT/"
fi
echo "VPK: $OUT/nectar_vita.vpk"
