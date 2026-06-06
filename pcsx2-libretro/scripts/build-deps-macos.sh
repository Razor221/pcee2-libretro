#!/usr/bin/env bash
# Builds static x86_64 dependencies for the PCEE2 libretro core into $1.
# Everything is static except MoltenVK (prebuilt, dlopen'd Vulkan
# driver), the only dylib.
set -e

if [ "$#" -ne 1 ]; then
	echo "Usage: $0 <install-prefix>"
	exit 1
fi

PREFIX=$(python3 -c "import os,sys;print(os.path.realpath(sys.argv[1]))" "$1")
mkdir -p "$PREFIX"
NPROCS="$(getconf _NPROCESSORS_ONLN)"
export MACOSX_DEPLOYMENT_TARGET=11.0

LIBPNG=v1.6.58
LIBJPEGTURBO=3.1.4.1
WEBP=v1.6.0
LZ4=v1.10.0
ZSTD=v1.5.7
FREETYPE=VER-2-14-3
SDL=release-3.4.10
PLUTOVG=v1.3.2
PLUTOSVG=v0.0.7
RAPIDYAML=v0.12.1
SHADERC=v2026.2
MOLTENVK=v1.4.0

COMMON=(-DCMAKE_BUILD_TYPE=Release "-DCMAKE_INSTALL_PREFIX=$PREFIX" "-DCMAKE_PREFIX_PATH=$PREFIX"
	-DBUILD_SHARED_LIBS=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_OSX_ARCHITECTURES=x86_64 -G Ninja)

mkdir -p deps-build
cd deps-build

clone() {
	[ -d "$2" ] || git clone --depth 1 -b "$3" $4 "https://github.com/$1" "$2"
}

build() {
	local src="$1"; shift
	cmake -S "$src" -B "$src/b" "${COMMON[@]}" "$@"
	cmake --build "$src/b" --parallel "$NPROCS" --target install
}

clone pnggroup/libpng libpng "$LIBPNG"
build libpng -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF -DPNG_TOOLS=OFF -DPNG_FRAMEWORK=OFF

clone libjpeg-turbo/libjpeg-turbo libjpeg-turbo "$LIBJPEGTURBO"
build libjpeg-turbo -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_SIMD=OFF -DWITH_TURBOJPEG=OFF

clone webmproject/libwebp libwebp "$WEBP"
build libwebp -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF \
	-DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF \
	-DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF

clone lz4/lz4 lz4 "$LZ4"
cmake -S lz4/build/cmake -B lz4/b "${COMMON[@]}" -DLZ4_BUILD_CLI=OFF -DLZ4_BUILD_LEGACY_LZ4C=OFF
cmake --build lz4/b --parallel "$NPROCS" --target install

clone facebook/zstd zstd "$ZSTD"
cmake -S zstd/build/cmake -B zstd/b "${COMMON[@]}" -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON \
	-DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF
cmake --build zstd/b --parallel "$NPROCS" --target install

clone freetype/freetype freetype "$FREETYPE"
build freetype -DFT_REQUIRE_ZLIB=TRUE -DFT_REQUIRE_PNG=TRUE -DFT_DISABLE_BZIP2=TRUE \
	-DFT_DISABLE_BROTLI=TRUE -DFT_DISABLE_HARFBUZZ=TRUE

clone libsdl-org/SDL SDL "$SDL"
build SDL -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF

clone sammycage/plutovg plutovg "$PLUTOVG"
build plutovg -DPLUTOVG_BUILD_EXAMPLES=OFF

clone sammycage/plutosvg plutosvg "$PLUTOSVG"
build plutosvg -DPLUTOSVG_ENABLE_FREETYPE=ON -DPLUTOSVG_BUILD_EXAMPLES=OFF

clone biojppm/rapidyaml rapidyaml "$RAPIDYAML" --recursive
build rapidyaml

# shaderc: static combined, linked straight into the core
clone google/shaderc shaderc "$SHADERC"
(cd shaderc && python3 utils/git-sync-deps)
cmake -S shaderc -B shaderc/b -DCMAKE_BUILD_TYPE=Release "-DCMAKE_INSTALL_PREFIX=$PREFIX" \
	-DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_OSX_ARCHITECTURES=x86_64 -G Ninja \
	-DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON -DSHADERC_SKIP_COPYRIGHT_CHECK=ON
cmake --build shaderc/b --parallel "$NPROCS" --target shaderc_combined
mkdir -p "$PREFIX/lib" "$PREFIX/include"
cp shaderc/b/libshaderc/libshaderc_combined.a "$PREFIX/lib/"
cp -r shaderc/libshaderc/include/shaderc "$PREFIX/include/"

# MoltenVK: prebuilt release (dlopen'd Vulkan implementation)
curl -L -o moltenvk.tar "https://github.com/KhronosGroup/MoltenVK/releases/download/$MOLTENVK/MoltenVK-macos.tar"
tar xf moltenvk.tar
cp MoltenVK/MoltenVK/dylib/macOS/libMoltenVK.dylib "$PREFIX/lib/"

echo "Dependencies installed to $PREFIX"
