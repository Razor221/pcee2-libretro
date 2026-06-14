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
PLUTOVG=v1.3.2
PLUTOSVG=v0.0.7
RAPIDYAML=v0.12.1

mkdir -p deps-build
cd deps-build

clone() {
	[ -d "$2" ] || git clone --depth 1 --branch "$3" --recursive "$1" "$2"
}

# Set PCEE2_SDL_STATIC=1 to build SDL3 as a static lib instead of shared. With a
# static-only install, SDL3's CMake config makes SDL3::SDL3 (what the core links)
# resolve to the static archive, so the resulting core is self-contained and needs
# no libSDL3.so.0 at runtime — useful for minimal/uncommon ARM distros.
if [ "${PCEE2_SDL_STATIC:-0}" = "1" ]; then
	SDL_LIB_FLAGS="-DSDL_SHARED=OFF -DSDL_STATIC=ON -DCMAKE_POSITION_INDEPENDENT_CODE=ON"
else
	SDL_LIB_FLAGS="-DSDL_SHARED=ON -DSDL_STATIC=OFF"
fi
clone https://github.com/libsdl-org/SDL sdl3 "$SDL"
cmake -S sdl3 -B sdl3/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="$PREFIX" $SDL_LIB_FLAGS \
	-DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
cmake --build sdl3/build --parallel "$NPROCS"
cmake --install sdl3/build

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

# Some distros (notably Debian Bookworm, the glibc target for Raspberry Pi OS)
# ship libpng / libzstd a hair older than PCSX2's find_package() minimums
# (PNG >= 1.6.40, Zstd >= 1.5.5). Bookworm has 1.6.39 / 1.5.4. Build newer ones
# static into the prefix so they're found before the system copies and get
# embedded into the core (no runtime dependency on the Pi's older .so either).
# Set PCEE2_BUILD_PNG_ZSTD=1 to enable; off by default so the Ubuntu jobs, whose
# system libs already satisfy the minimums, keep using those.
if [ "${PCEE2_BUILD_PNG_ZSTD:-0}" = "1" ]; then
	clone https://github.com/pnggroup/libpng libpng v1.6.43
	cmake -S libpng -B libpng/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
		-DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF -DPNG_TOOLS=OFF
	cmake --build libpng/build --parallel "$NPROCS"
	cmake --install libpng/build

	clone https://github.com/facebook/zstd zstd v1.5.6
	cmake -S zstd/build/cmake -B zstd/b -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
		-DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON \
		-DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF
	cmake --build zstd/b --parallel "$NPROCS"
	cmake --install zstd/b
fi

echo "Dependencies installed to $PREFIX"
