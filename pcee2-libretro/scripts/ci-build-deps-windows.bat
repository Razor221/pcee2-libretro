@echo off
rem CI helper: enter a Visual Studio x64 developer environment, then build the
rem libretro dependencies with build-deps-windows.bat.
rem
rem Why this wrapper exists: the libretro MSVC template runs VsDevCmd.bat through
rem PowerShell's Invoke-Expression, which configures a *child* process only, so
rem cl/ninja/cmake never reach the `cmd /c` that runs the deps script. The VS
rem CMake generator used by the template's build step does not care (it locates
rem the toolchain itself), but build-deps-windows.bat uses -G Ninja and needs a
rem real developer prompt. Doing the vcvars call and the deps build inside one
rem cmd process is exactly what the GitHub Actions Windows job does.
setlocal

if "%~1"=="" (
  echo Usage: %0 ^<install-prefix^>
  exit /b 1
)

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=vswhere"

set "VSINSTPATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VSINSTPATH=%%i"
if not defined VSINSTPATH (
  echo ERROR: could not locate a Visual Studio installation via vswhere
  exit /b 1
)

call "%VSINSTPATH%\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1

rem VS ships cmake and ninja with the CMake component; add them if the developer
rem prompt did not already put them on PATH.
set "VSCMAKE=%VSINSTPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake"
where cmake >nul 2>nul || set "PATH=%VSCMAKE%\CMake\bin;%PATH%"
where ninja >nul 2>nul || set "PATH=%VSCMAKE%\Ninja;%PATH%"

rem Fail loudly and early if the toolchain is still incomplete — otherwise the
rem first dependency dies with an unhelpful CMake error.
where cl || (echo ERROR: cl.exe not on PATH after vcvars64 & exit /b 1)
where cmake || (echo ERROR: cmake not on PATH & exit /b 1)
where ninja || (echo ERROR: ninja not on PATH & exit /b 1)

call "%~dp0build-deps-windows.bat" "%~1" || exit /b 1
