#!/usr/bin/env bash
# release-macos.sh [--clean]
#
# Archivos de macOS de una release de Open Nectar Fusion (ver
# scripts/release/make_release.py). Se ejecuta EN un Mac, con las herramientas
# de línea de órdenes de Xcode (xcode-select --install) y cmake (brew install
# cmake). SDL2 se compila estática aquí: los ejecutables solo dependen de lo
# que trae macOS.
#
# Por defecto salen binarios universales (Apple Silicon e Intel). Para uno
# solo: MACOS_ARCHS=arm64 scripts/release-macos.sh
#
# Las versiones salen de versions.json (raíz del repo).
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${root}/build-release/macos"
out_dir="${root}/release-out/macos"
p1="${root}/games/pikmin1"
p2="${root}/games/pikmin2"

if [[ "$(uname -s)" != "Darwin" ]]; then
    printf 'Este script se ejecuta en macOS.\n' >&2
    exit 1
fi
if [[ "${1:-}" == "--clean" ]]; then
    rm -rf "${build_dir}" "${out_dir}"
fi
command -v cmake >/dev/null 2>&1 || { printf 'Falta cmake (brew install cmake).\n' >&2; exit 1; }
version="$(python3 -c "import json;print(json.load(open('${root}/versions.json'))['release'])")"
jobs="$(sysctl -n hw.ncpu)"
archs="${MACOS_ARCHS:-arm64;x86_64}"
deployment="${MACOS_DEPLOYMENT_TARGET:-11.0}"

# SDL2 estática (fuentes oficiales), universal como el resto.
sdl_version="${SDL_VERSION:-2.32.10}"
if [[ ! -f "${build_dir}/sdl2/lib/libSDL2.a" ]]; then
    printf '[0/4] SDL2 %s estática...\n' "${sdl_version}"
    mkdir -p "${build_dir}"
    if [[ ! -d "${build_dir}/SDL2-${sdl_version}" ]]; then
        curl -fsSL "https://github.com/libsdl-org/SDL/releases/download/release-${sdl_version}/SDL2-${sdl_version}.tar.gz" \
            | tar -xz -C "${build_dir}"
    fi
    cmake -S "${build_dir}/SDL2-${sdl_version}" -B "${build_dir}/sdl2-build" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="${archs}" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="${deployment}" \
        -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF \
        -DCMAKE_INSTALL_PREFIX="${build_dir}/sdl2"
    cmake --build "${build_dir}/sdl2-build" -j"${jobs}"
    cmake --install "${build_dir}/sdl2-build"
fi

# PIKMIN_NATIVE_OPTIMIZE=OFF: nada de -march=native (ver release-windows.sh).
common=(
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_PREFIX_PATH="${build_dir}/sdl2"
    -DCMAKE_OSX_ARCHITECTURES="${archs}"
    -DCMAKE_OSX_DEPLOYMENT_TARGET="${deployment}"
    -DPIKMIN_NATIVE_JAUDIO=ON
    -DPIKMIN_STATIC_RUNTIME=ON
    -DPIKMIN_NATIVE_OPTIMIZE=OFF
)

printf '[1/4] Pikmin 1 USA, el launcher y el editor de mapas...\n'
cmake -S "${p1}" -B "${build_dir}/p1-usa" "${common[@]}" -DOPEN_NECTAR_VERSION="${version}"
cmake --build "${build_dir}/p1-usa" --target pikmin_pc pikmin_launcher nectar_mapmaker -j"${jobs}"

printf '[2/4] Pikmin 1 PAL...\n'
cmake -S "${p1}" -B "${build_dir}/p1-pal" "${common[@]}" -DOPEN_NECTAR_VERSION="${version}" \
    -DPIKMIN_GAME_VERSION=VERSION_GPIP01_00
cmake --build "${build_dir}/p1-pal" --target pikmin_pc -j"${jobs}"

printf '[3/4] Pikmin 2 PAL y USA...\n'
cmake -S "${p2}" -B "${build_dir}/p2" "${common[@]}" -DBUILD_TESTING=OFF
cmake --build "${build_dir}/p2" --target pikmin2_pc -j"${jobs}"
cmake -S "${p2}" -B "${build_dir}/p2-usa" "${common[@]}" -DBUILD_TESTING=OFF \
    -DPIKMIN_GAME_VERSION=VERSION_GPVE01
cmake --build "${build_dir}/p2-usa" --target pikmin2_pc -j"${jobs}"

launcher="${build_dir}/p1-usa/bin/nectar-launcher"
usa="${build_dir}/p1-usa/bin/nectar"
pal="${build_dir}/p1-pal/bin/nectar"
mapmaker="${build_dir}/p1-usa/bin/nectar-mapmaker"
p2eu="${build_dir}/p2/pikmin2_pc"
p2usa="${build_dir}/p2-usa/pikmin2_pc"

printf '[4/4] Montando los archivos de la release...\n'
stripped="${build_dir}/stripped"
rm -rf "${stripped}"
mkdir -p "${stripped}"
cp "${launcher}" "${stripped}/nectar-launcher"
cp "${usa}" "${stripped}/nectar"
cp "${pal}" "${stripped}/nectar-pal"
cp "${mapmaker}" "${stripped}/nectar-mapmaker"
cp "${p2eu}" "${stripped}/pikmin2_pc"
cp "${p2usa}" "${stripped}/pikmin2_pc-usa"
for exe in "${stripped}"/*; do
    strip -x "${exe}"
    # Solo bibliotecas de macOS: cualquier otra (Homebrew) no estará en el Mac
    # de quien lo descargue.
    while read -r lib; do
        case "${lib}" in
            /usr/lib/*|/System/Library/*) ;;
            *) printf '%s depende de %s, que no viene con macOS. Revisa PIKMIN_STATIC_RUNTIME.\n' "${exe}" "${lib}" >&2
               exit 1 ;;
        esac
    done < <(otool -L "${exe}" | tail -n +2 | awk '{print $1}')
    # Firma ad hoc: Apple Silicon no ejecuta binarios sin firmar, y strip
    # invalida la que pone el enlazador.
    codesign --force --sign - "${exe}"
done

python3 "${root}/scripts/release/make_release.py" --os macos --out "${out_dir}" \
    --launcher "${stripped}/nectar-launcher" --p1-usa "${stripped}/nectar" \
    --p1-pal "${stripped}/nectar-pal" --p2 "${stripped}/pikmin2_pc" --p2-usa "${stripped}/pikmin2_pc-usa" \
    --mapmaker "${stripped}/nectar-mapmaker" \
    --extra "${root}/games/pikmin1/packaging/macos/README.txt" \
    --extra "${root}/games/pikmin1/packaging/icon/open_nectar.png"

printf 'Listo: %s\n' "${out_dir}"
