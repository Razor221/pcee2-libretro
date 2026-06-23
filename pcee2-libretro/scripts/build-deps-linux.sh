#!/usr/bin/env bash
# Builds the dependencies that aren't packaged by Ubuntu into a local prefix
# (static, PIC), for building the PCEE2 libretro core.
# Usage: build-deps-linux.sh <install-prefix>

set -e

if [ "$#" -ne 1 ]; then
	echo "Usage: $0 <install-prefix>"
	exit 1
fi

PREFIX=$(realpath "$1")
NPROCS="$(getconf _NPROCESSORS_ONLN)"

SDL=release-3.4.10
FREETYPE=VER-2-14-3
PLUTOVG=v1.3.2
PLUTOSVG=v0.0.7
RAPIDYAML=v0.12.1

mkdir -p deps-build
cd deps-build

clone() {
	[ -d "$2" ] || git clone --depth 1 --branch "$3" --recursive "$1" "$2"
}

clone https://github.com/libsdl-org/SDL sdl3 "$SDL"
cmake -S sdl3 -B sdl3/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="$PREFIX" -DSDL_SHARED=ON -DSDL_STATIC=OFF \
	-DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
cmake --build sdl3/build --parallel "$NPROCS"
cmake --install sdl3/build

# FreeType: the libretro build image ships an old FreeType (e.g. 2.8.1), but
# pcee2 needs >= 2.10 (COLRv0 emoji) and plutosvg's FreeType integration needs
# the OT-SVG API from >= 2.12. Build a current one into the prefix so both
# plutosvg and the core find it (via CMAKE_PREFIX_PATH=$CI_PROJECT_DIR/deps).
clone https://github.com/freetype/freetype freetype "$FREETYPE"
cmake -S freetype -B freetype/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
	-DBUILD_SHARED_LIBS=OFF -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON \
	-DFT_DISABLE_PNG=ON -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON
cmake --build freetype/build --parallel "$NPROCS"
cmake --install freetype/build

clone https://github.com/sammycage/plutovg plutovg "$PLUTOVG"
cmake -S plutovg -B plutovg/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
	-DBUILD_SHARED_LIBS=OFF -DPLUTOVG_BUILD_EXAMPLES=OFF
cmake --build plutovg/build --parallel "$NPROCS"
cmake --install plutovg/build

clone https://github.com/sammycage/plutosvg plutosvg "$PLUTOSVG"
cmake -S plutosvg -B plutosvg/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_PREFIX_PATH="$PREFIX" \
	-DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF \
	-DPLUTOSVG_ENABLE_FREETYPE=ON -DPLUTOSVG_BUILD_EXAMPLES=OFF
cmake --build plutosvg/build --parallel "$NPROCS"
cmake --install plutosvg/build

clone https://github.com/biojppm/rapidyaml rapidyaml "$RAPIDYAML"
cmake -S rapidyaml -B rapidyaml/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
	-DBUILD_SHARED_LIBS=OFF
cmake --build rapidyaml/build --parallel "$NPROCS"
cmake --install rapidyaml/build

clone https://github.com/ianlancetaylor/libbacktrace libbacktrace master
(cd libbacktrace && ./configure --prefix="$PREFIX" --with-pic && make -j"$NPROCS" && make install)

# shaderc: static combined, linked straight into the core. Distro
# libshaderc_combined.a packages aren't actually self-contained (Ubuntu's
# expects the system glslang), so build the real thing from source.
SHADERC=v2026.2
clone https://github.com/google/shaderc shaderc "$SHADERC"
(cd shaderc && python3 utils/git-sync-deps)
cmake -S shaderc -B shaderc/b -G Ninja -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
	-DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
	-DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON -DSHADERC_SKIP_COPYRIGHT_CHECK=ON
cmake --build shaderc/b --parallel "$NPROCS" --target shaderc_combined
mkdir -p "$PREFIX/lib" "$PREFIX/include"
cp shaderc/b/libshaderc/libshaderc_combined.a "$PREFIX/lib/"
cp -r shaderc/libshaderc/include/shaderc "$PREFIX/include/"

echo "Dependencies installed to $PREFIX"
