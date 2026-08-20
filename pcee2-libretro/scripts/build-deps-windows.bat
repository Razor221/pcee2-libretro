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

rem Revisions live in deps.versions, shared with the other platform scripts and
rem with deps/CMakeLists.txt, so the paths cannot drift apart. Plain KEY=value
rem lines; findstr picks those out and leaves the comments alone.
for /f "usebackq tokens=1,2 delims==" %%a in (`findstr /r "^[A-Z0-9_][A-Z0-9_]*=" "%~dp0deps.versions"`) do set "%%a=%%b"

rem The core links the CRT statically so it loads without the VC++
rem redistributable installed, so every dependency has to use /MT as well -
rem mixing runtimes in one binary is a link error at best.
rem
rem CMAKE_MSVC_RUNTIME_LIBRARY only takes effect under CMP0091 NEW, and
rem CMAKE_POLICY_VERSION_MINIMUM below pins the policy floor at 3.5 for the
rem projects that ask for an ancient CMake (rapidyaml is one), which puts that
rem policy back to OLD and silently leaves them on /MD. Set the policy itself.
set "COMMON=-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=%INSTALLDIR% -DCMAKE_PREFIX_PATH=%INSTALLDIR% -DBUILD_SHARED_LIBS=OFF -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DCMAKE_POLICY_DEFAULT_CMP0091=NEW -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -G Ninja"

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

echo === libjpeg-turbo %JPEGTURBO% ===
if not exist libjpeg-turbo git clone --depth 1 -b %JPEGTURBO% https://github.com/libjpeg-turbo/libjpeg-turbo || exit /b 1
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

echo === DirectX-Headers %DXHEADERS% ===
if not exist DirectX-Headers git clone --depth 1 -b %DXHEADERS% https://github.com/microsoft/DirectX-Headers || exit /b 1
cmake -S DirectX-Headers -B DirectX-Headers\b %COMMON% -DDXHEADERS_BUILD_TEST=OFF -DDXHEADERS_BUILD_GOOGLE_TEST=OFF -DDXHEADERS_INSTALL=ON || exit /b 1
cmake --build DirectX-Headers\b --target install || exit /b 1

echo === shaderc %SHADERC% (static combined, linked into the core) ===
call :ensure_python || exit /b 1
if not exist shaderc git clone --depth 1 -b %SHADERC% https://github.com/google/shaderc || exit /b 1
cd shaderc
"%PYTHON%" utils\git-sync-deps || exit /b 1
cd ..
rem git-sync-deps has just put DEPS' revision in place; move it forward. The
rem reason for the override is recorded in deps.versions.
for /f "delims=" %%h in ('git -C shaderc\third_party\glslang rev-parse HEAD') do set "GLSLANG_HEAD=%%h"
if not "!GLSLANG_HEAD!"=="%GLSLANG%" (
  git -C shaderc\third_party\glslang fetch --depth 1 origin %GLSLANG% || exit /b 1
  git -C shaderc\third_party\glslang checkout --detach FETCH_HEAD || exit /b 1
)
rem SPIRV-Tools and glslang generate sources with Python scripts of their own, so
rem hand them the interpreter we resolved instead of letting find_package() guess
rem (PYTHON_EXECUTABLE covers the older FindPythonInterp path they still use).
cmake -S shaderc -B shaderc\b -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=%INSTALLDIR% -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DCMAKE_POLICY_DEFAULT_CMP0091=NEW -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -G Ninja -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON -DSHADERC_SKIP_COPYRIGHT_CHECK=ON -DSHADERC_ENABLE_SHARED_CRT=OFF "-DPython3_EXECUTABLE=%PYTHON%" "-DPYTHON_EXECUTABLE=%PYTHON%" || exit /b 1
cmake --build shaderc\b --target shaderc_combined || exit /b 1
copy /y shaderc\b\libshaderc\shaderc_combined.lib "%INSTALLDIR%\lib\shaderc_combined.lib" || exit /b 1
xcopy /e /i /y shaderc\libshaderc\include\shaderc "%INSTALLDIR%\include\shaderc" || exit /b 1

echo Dependencies installed to %INSTALLDIR%
exit /b 0

rem ---------------------------------------------------------------------------
rem Resolve a Python 3 interpreter into %PYTHON% (full path) and put it on PATH.
rem
rem shaderc's utils\git-sync-deps is a Python script, and SPIRV-Tools / glslang
rem run generator scripts from their CMake builds, so Python is needed for the
rem whole shaderc step. GitHub's windows-2025 image ships Python, but the
rem libretro buildbot's MSVC runner does not ("'python' is not recognized"), so
rem fall back to the official python.org NuGet package, which is a plain zip with
rem a normal Lib\ layout and needs no installer or admin rights.
rem ---------------------------------------------------------------------------
:ensure_python
if defined PYTHON exit /b 0
for %%p in (python.exe python3.exe) do (
  if not defined PYTHON (
    for /f "delims=" %%q in ('where %%p 2^>nul') do (
      if not defined PYTHON (
        "%%q" -c "import sys; sys.exit(0 if sys.version_info[0] == 3 else 1)" >nul 2>nul && set "PYTHON=%%q"
      )
    )
  )
)
if not defined PYTHON (
  for /f "delims=" %%q in ('py -3 -c "import sys; print(sys.executable)" 2^>nul') do set "PYTHON=%%q"
)
if defined PYTHON (
  echo Using Python at !PYTHON!
  exit /b 0
)

set "PYVER=3.12.10"
set "PYROOT=%CD%\python-%PYVER%"
echo === no Python found, bootstrapping CPython %PYVER% ===
if not exist "!PYROOT!\tools\python.exe" (
  if exist python.nupkg del /q python.nupkg
  rem curl.exe ships with Windows 10/Server 2019 and up; fall back to PowerShell.
  curl.exe -sSL -o python.nupkg "https://globalcdn.nuget.org/packages/python.%PYVER%.nupkg"
  if not exist python.nupkg (
    powershell -NoProfile -Command "Invoke-WebRequest -UseBasicParsing -Uri 'https://globalcdn.nuget.org/packages/python.%PYVER%.nupkg' -OutFile 'python.nupkg'"
    if not exist python.nupkg exit /b 1
  )
  mkdir "!PYROOT!" 2>nul
  rem A .nupkg is a plain zip; bsdtar reads it, Expand-Archive is the fallback.
  tar -xf python.nupkg -C "!PYROOT!" 2>nul
  if not exist "!PYROOT!\tools\python.exe" (
    powershell -NoProfile -Command "Expand-Archive -Force -Path 'python.nupkg' -DestinationPath '!PYROOT!'"
  )
)
if not exist "!PYROOT!\tools\python.exe" (
  echo ERROR: failed to bootstrap Python
  exit /b 1
)
rem FindPython3 and scripts invoking a bare `python3` both need to resolve here.
if not exist "!PYROOT!\tools\python3.exe" copy /y "!PYROOT!\tools\python.exe" "!PYROOT!\tools\python3.exe" >nul
set "PATH=!PYROOT!\tools;!PATH!"
set "PYTHON=!PYROOT!\tools\python.exe"
"!PYTHON!" --version || exit /b 1
exit /b 0
