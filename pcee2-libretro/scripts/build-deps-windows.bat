@echo off
rem Builds static dependencies for the PCEE2 libretro core into %1.
rem Run from a VS x64 developer prompt (cl/ninja/cmake on PATH).
rem All libraries including shaderc_combined are built static.
setlocal enabledelayedexpansion

if "%~1"=="" (
  echo Usage: %0 ^<install-prefix^>
  exit /b 1
)
set "INSTALLDIR=%~1"
mkdir "%INSTALLDIR%" 2>nul

set ZLIB=v1.3.2
set LIBPNG=v1.6.58
set LIBJPEGTURBO=3.1.4.1
set WEBP=v1.6.0
set LZ4=v1.10.0
set ZSTD=v1.5.7
set FREETYPE=VER-2-14-3
set SDL=release-3.4.10
set PLUTOVG=v1.3.2
set PLUTOSVG=v0.0.7
set RAPIDYAML=v0.12.1
set SHADERC=v2026.2

set "COMMON=-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=%INSTALLDIR% -DCMAKE_PREFIX_PATH=%INSTALLDIR% -DBUILD_SHARED_LIBS=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -G Ninja"

mkdir deps-build 2>nul
cd deps-build || exit /b 1

echo === zlib %ZLIB% ===
if not exist zlib git clone --depth 1 -b %ZLIB% https://github.com/madler/zlib || exit /b 1
cmake -S zlib -B zlib\b %COMMON% -DZLIB_BUILD_SHARED=OFF -DZLIB_BUILD_STATIC=ON -DZLIB_BUILD_TESTING=OFF -DZLIB_BUILD_MINIZIP=OFF || exit /b 1
cmake --build zlib\b --target install || exit /b 1
if exist "%INSTALLDIR%\lib\zlibstatic.lib" copy /y "%INSTALLDIR%\lib\zlibstatic.lib" "%INSTALLDIR%\lib\zlib.lib"
if exist "%INSTALLDIR%\lib\zs.lib" copy /y "%INSTALLDIR%\lib\zs.lib" "%INSTALLDIR%\lib\zlib.lib"

echo === libpng %LIBPNG% ===
if not exist libpng git clone --depth 1 -b %LIBPNG% https://github.com/pnggroup/libpng || exit /b 1
cmake -S libpng -B libpng\b %COMMON% -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF -DPNG_TOOLS=OFF -DPNG_FRAMEWORK=OFF || exit /b 1
cmake --build libpng\b --target install || exit /b 1
if exist "%INSTALLDIR%\lib\libpng16_static.lib" copy /y "%INSTALLDIR%\lib\libpng16_static.lib" "%INSTALLDIR%\lib\libpng16.lib"

echo === libjpeg-turbo %LIBJPEGTURBO% ===
if not exist libjpeg-turbo git clone --depth 1 -b %LIBJPEGTURBO% https://github.com/libjpeg-turbo/libjpeg-turbo || exit /b 1
cmake -S libjpeg-turbo -B libjpeg-turbo\b %COMMON% -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_SIMD=OFF -DWITH_TURBOJPEG=OFF || exit /b 1
cmake --build libjpeg-turbo\b --target install || exit /b 1
if exist "%INSTALLDIR%\lib\jpeg-static.lib" copy /y "%INSTALLDIR%\lib\jpeg-static.lib" "%INSTALLDIR%\lib\jpeg.lib"

echo === libwebp %WEBP% ===
if not exist libwebp git clone --depth 1 -b %WEBP% https://github.com/webmproject/libwebp || exit /b 1
cmake -S libwebp -B libwebp\b %COMMON% -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF || exit /b 1
cmake --build libwebp\b --target install || exit /b 1
rem merge sharpyuv into libwebp so module-style find_package links cleanly
lib.exe /OUT:"%INSTALLDIR%\lib\webp_merged.lib" "%INSTALLDIR%\lib\libwebp.lib" "%INSTALLDIR%\lib\libsharpyuv.lib" || exit /b 1
copy /y "%INSTALLDIR%\lib\webp_merged.lib" "%INSTALLDIR%\lib\libwebp.lib"
del "%INSTALLDIR%\lib\webp_merged.lib"

echo === lz4 %LZ4% ===
if not exist lz4 git clone --depth 1 -b %LZ4% https://github.com/lz4/lz4 || exit /b 1
cmake -S lz4\build\cmake -B lz4\b %COMMON% -DLZ4_BUILD_CLI=OFF -DLZ4_BUILD_LEGACY_LZ4C=OFF || exit /b 1
cmake --build lz4\b --target install || exit /b 1

echo === zstd %ZSTD% ===
if not exist zstd git clone --depth 1 -b %ZSTD% https://github.com/facebook/zstd || exit /b 1
cmake -S zstd\build\cmake -B zstd\b %COMMON% -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF || exit /b 1
cmake --build zstd\b --target install || exit /b 1

echo === freetype %FREETYPE% ===
if not exist freetype git clone --depth 1 -b %FREETYPE% https://github.com/freetype/freetype || exit /b 1
cmake -S freetype -B freetype\b %COMMON% -DFT_REQUIRE_ZLIB=TRUE -DFT_REQUIRE_PNG=TRUE -DFT_DISABLE_BZIP2=TRUE -DFT_DISABLE_BROTLI=TRUE -DFT_DISABLE_HARFBUZZ=TRUE || exit /b 1
cmake --build freetype\b --target install || exit /b 1

echo === SDL %SDL% ===
if not exist SDL git clone --depth 1 -b %SDL% https://github.com/libsdl-org/SDL || exit /b 1
cmake -S SDL -B SDL\b %COMMON% -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF || exit /b 1
cmake --build SDL\b --target install || exit /b 1

echo === plutovg %PLUTOVG% ===
if not exist plutovg git clone --depth 1 -b %PLUTOVG% https://github.com/sammycage/plutovg || exit /b 1
cmake -S plutovg -B plutovg\b %COMMON% -DPLUTOVG_BUILD_EXAMPLES=OFF || exit /b 1
cmake --build plutovg\b --target install || exit /b 1

echo === plutosvg %PLUTOSVG% ===
if not exist plutosvg git clone --depth 1 -b %PLUTOSVG% https://github.com/sammycage/plutosvg || exit /b 1
cmake -S plutosvg -B plutosvg\b %COMMON% -DPLUTOSVG_ENABLE_FREETYPE=ON -DPLUTOSVG_BUILD_EXAMPLES=OFF || exit /b 1
cmake --build plutosvg\b --target install || exit /b 1

echo === rapidyaml %RAPIDYAML% ===
if not exist rapidyaml git clone --depth 1 -b %RAPIDYAML% --recursive https://github.com/biojppm/rapidyaml || exit /b 1
cmake -S rapidyaml -B rapidyaml\b %COMMON% || exit /b 1
cmake --build rapidyaml\b --target install || exit /b 1

echo === shaderc %SHADERC% (static combined, linked into the core) ===
if not exist shaderc git clone --depth 1 -b %SHADERC% https://github.com/google/shaderc || exit /b 1
cd shaderc
python utils\git-sync-deps || exit /b 1
cd ..
cmake -S shaderc -B shaderc\b -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=%INSTALLDIR% -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -G Ninja -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON -DSHADERC_SKIP_COPYRIGHT_CHECK=ON -DSHADERC_ENABLE_SHARED_CRT=ON || exit /b 1
cmake --build shaderc\b --target shaderc_combined || exit /b 1
copy /y shaderc\b\libshaderc\shaderc_combined.lib "%INSTALLDIR%\lib\shaderc_combined.lib" || exit /b 1
xcopy /e /i /y shaderc\libshaderc\include\shaderc "%INSTALLDIR%\include\shaderc" || exit /b 1

echo Dependencies installed to %INSTALLDIR%
exit /b 0
