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

# Allow a cross-compiling CMake wrapper (e.g. the MXE x86_64-w64-mingw32.static-cmake
# used by the libretro Windows job) to be substituted for the host cmake. HOST is
# the autoconf target triple for the non-CMake deps (libbacktrace); empty = native.
CMAKE="${CMAKE:-cmake}"
HOST="${HOST:-}"

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
"$CMAKE" -S sdl3 -B sdl3/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="$PREFIX" -DSDL_SHARED=ON -DSDL_STATIC=OFF \
	-DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
"$CMAKE" --build sdl3/build --parallel "$NPROCS"
"$CMAKE" --install sdl3/build

# FreeType: the libretro build image ships an old FreeType (e.g. 2.8.1), but
# pcee2 needs >= 2.10 (COLRv0 emoji) and plutosvg's FreeType integration needs
# the OT-SVG API from >= 2.12. Build a current one into the prefix so both
# plutosvg and the core find it (via CMAKE_PREFIX_PATH=$CI_PROJECT_DIR/deps).
clone https://github.com/freetype/freetype freetype "$FREETYPE"
"$CMAKE" -S freetype -B freetype/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
	-DBUILD_SHARED_LIBS=OFF -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON \
	-DFT_DISABLE_PNG=ON -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON
"$CMAKE" --build freetype/build --parallel "$NPROCS"
"$CMAKE" --install freetype/build

clone https://github.com/sammycage/plutovg plutovg "$PLUTOVG"
"$CMAKE" -S plutovg -B plutovg/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
	-DBUILD_SHARED_LIBS=OFF -DPLUTOVG_BUILD_EXAMPLES=OFF
"$CMAKE" --build plutovg/build --parallel "$NPROCS"
"$CMAKE" --install plutovg/build

clone https://github.com/sammycage/plutosvg plutosvg "$PLUTOSVG"
"$CMAKE" -S plutosvg -B plutosvg/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_PREFIX_PATH="$PREFIX" \
	-DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF \
	-DPLUTOSVG_ENABLE_FREETYPE=ON -DPLUTOSVG_BUILD_EXAMPLES=OFF
"$CMAKE" --build plutosvg/build --parallel "$NPROCS"
"$CMAKE" --install plutosvg/build

clone https://github.com/biojppm/rapidyaml rapidyaml "$RAPIDYAML"
"$CMAKE" -S rapidyaml -B rapidyaml/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
	-DBUILD_SHARED_LIBS=OFF
"$CMAKE" --build rapidyaml/build --parallel "$NPROCS"
"$CMAKE" --install rapidyaml/build

clone https://github.com/ianlancetaylor/libbacktrace libbacktrace master
(cd libbacktrace && ./configure --prefix="$PREFIX" --with-pic ${HOST:+--host="$HOST"} && make -j"$NPROCS" && make install)

# shaderc: static combined, linked straight into the core. Distro
# libshaderc_combined.a packages aren't actually self-contained (Ubuntu's
# expects the system glslang), so build the real thing from source.
SHADERC=v2026.2
clone https://github.com/google/shaderc shaderc "$SHADERC"
(cd shaderc && python3 utils/git-sync-deps)
"$CMAKE" -S shaderc -B shaderc/b -G Ninja -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
	-DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
	-DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON -DSHADERC_SKIP_COPYRIGHT_CHECK=ON
"$CMAKE" --build shaderc/b --parallel "$NPROCS" --target shaderc_combined
mkdir -p "$PREFIX/lib" "$PREFIX/include"
cp shaderc/b/libshaderc/libshaderc_combined.a "$PREFIX/lib/"
cp -r shaderc/libshaderc/include/shaderc "$PREFIX/include/"

# Windows (MinGW) only: the libretro MXE image lacks several system libraries
# that the Linux jobs get from Ubuntu apt (JPEG, Zstd, LZ4, WebP — PNG/ZLIB are
# present in MXE). Build them from source into the prefix for the cross-build.
# Guarded by HOST, which is only set for the Windows cross job, so the Linux
# x64/aarch64 jobs keep using their apt copies and don't pay for these builds.
if [ -n "$HOST" ]; then
	JPEGTURBO=3.1.3
	ZSTD=v1.5.6
	LZ4=v1.10.0
	WEBP=v1.6.0

	clone https://github.com/libjpeg-turbo/libjpeg-turbo libjpeg-turbo "$JPEGTURBO"
	"$CMAKE" -S libjpeg-turbo -B libjpeg-turbo/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
		-DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_SIMD=OFF
	"$CMAKE" --build libjpeg-turbo/build --parallel "$NPROCS"
	"$CMAKE" --install libjpeg-turbo/build

	clone https://github.com/facebook/zstd zstd "$ZSTD"
	"$CMAKE" -S zstd/build/cmake -B zstd/build/cmake/b -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
		-DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON -DZSTD_BUILD_PROGRAMS=OFF
	"$CMAKE" --build zstd/build/cmake/b --parallel "$NPROCS"
	"$CMAKE" --install zstd/build/cmake/b

	clone https://github.com/lz4/lz4 lz4 "$LZ4"
	"$CMAKE" -S lz4/build/cmake -B lz4/build/cmake/b -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
		-DBUILD_SHARED_LIBS=OFF -DLZ4_BUILD_CLI=OFF -DLZ4_BUILD_LEGACY_LZ4C=OFF
	"$CMAKE" --build lz4/build/cmake/b --parallel "$NPROCS"
	"$CMAKE" --install lz4/build/cmake/b

	clone https://github.com/webmproject/libwebp libwebp "$WEBP"
	"$CMAKE" -S libwebp -B libwebp/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
		-DBUILD_SHARED_LIBS=OFF \
		-DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF \
		-DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF \
		-DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF
	"$CMAKE" --build libwebp/build --parallel "$NPROCS"
	"$CMAKE" --install libwebp/build
fi

echo "Dependencies installed to $PREFIX"
