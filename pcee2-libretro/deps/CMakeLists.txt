# Builds the dependencies that distributions either do not package or package
# too old, into CMAKE_INSTALL_PREFIX (static and PIC, so they end up inside the
# core). Same sources, same revisions and same flags as
# scripts/build-deps-linux.sh next door, which stays as the path the CI
# jobs take; this one exists so that a plain "cmake --build" on an unusual
# platform is enough (issue #17).
#
# Not built here: everything a normal distribution does package (zlib, libpng,
# ffmpeg, ...). Those are found the usual way, and building them from source
# would only put a second copy in front of a perfectly good one.

cmake_minimum_required(VERSION 3.16)
project(pcee2-deps NONE)

include(ExternalProject)
include(ProcessorCount)

ProcessorCount(NPROCS)
if(NPROCS EQUAL 0)
	set(NPROCS 1)
endif()

# Revisions come from the file the shell scripts source, so there is one place
# to change them. Plain KEY=value lines, comments start with #.
set(DEPS_VERSIONS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/deps.versions")
if(NOT EXISTS "${DEPS_VERSIONS_FILE}")
	message(FATAL_ERROR "Missing ${DEPS_VERSIONS_FILE}")
endif()
file(STRINGS "${DEPS_VERSIONS_FILE}" _version_lines REGEX "^[A-Z0-9_]+=")
foreach(_line IN LISTS _version_lines)
	string(REGEX MATCH "^([A-Z0-9_]+)=(.*)$" _matched "${_line}")
	set(${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
endforeach()

option(PCEE2_SDL_STATIC "Build SDL3 as a static library rather than a shared one" ON)
option(PCEE2_BUILD_PNG_ZSTD "Build libpng and zstd, for distributions whose copies are too old" OFF)
option(PCEE2_BUILD_JPEG "Build libjpeg-turbo static, so no libjpeg SONAME is baked in" OFF)

# What every dependency gets: the build type, the install prefix, PIC (the core
# is a shared library, so everything inside it has to be), and the toolchain the
# parent build is using. CMAKE_TOOLCHAIN_FILE is how a cross build (webOS, an
# embedded SDK) reaches these as well.
set(COMMON_ARGS
	-DCMAKE_BUILD_TYPE=Release
	-DCMAKE_INSTALL_PREFIX=${CMAKE_INSTALL_PREFIX}
	-DCMAKE_POSITION_INDEPENDENT_CODE=ON
)
foreach(_var CMAKE_TOOLCHAIN_FILE CMAKE_C_COMPILER CMAKE_CXX_COMPILER CMAKE_C_COMPILER_LAUNCHER
		CMAKE_CXX_COMPILER_LAUNCHER CMAKE_SYSROOT CMAKE_OSX_DEPLOYMENT_TARGET CMAKE_OSX_ARCHITECTURES
		ANDROID_ABI ANDROID_PLATFORM)
	if(DEFINED ${_var})
		list(APPEND COMMON_ARGS -D${_var}=${${_var}})
	endif()
endforeach()

# Every revision here is pinned, so there is nothing for the update step to do -
# and disabling it keeps a rebuild off the network entirely. Note that
# UPDATE_DISCONNECTED would not do: it takes the patch step out of the build's
# dependency chain along with the update step, and shaderc needs its patch step.
set(SHALLOW_CLONE GIT_SHALLOW ON GIT_PROGRESS OFF UPDATE_COMMAND "")

# ------------------------------------------------------------------ SDL3
#
# A static-only install makes SDL3::SDL3 - what the core links - resolve to the
# archive, so the core needs no libSDL3.so.0 at runtime. That is the default
# here because a hand-built core on an unusual distribution is exactly the case
# where the shared library will not be there.
if(PCEE2_SDL_STATIC)
	set(SDL_LIB_ARGS -DSDL_SHARED=OFF -DSDL_STATIC=ON)
else()
	set(SDL_LIB_ARGS -DSDL_SHARED=ON -DSDL_STATIC=OFF)
endif()

ExternalProject_Add(sdl3
	GIT_REPOSITORY https://github.com/libsdl-org/SDL
	GIT_TAG ${SDL}
	${SHALLOW_CLONE}
	CMAKE_ARGS ${COMMON_ARGS} ${SDL_LIB_ARGS} -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
	BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --parallel ${NPROCS}
)

# ------------------------------------------------------------------ FreeType
#
# Build images ship FreeType as old as 2.8.1, but the core needs >= 2.10 for
# COLRv0 emoji and plutosvg's FreeType integration needs the OT-SVG API from
# >= 2.12.
ExternalProject_Add(freetype
	GIT_REPOSITORY https://github.com/freetype/freetype
	GIT_TAG ${FREETYPE}
	${SHALLOW_CLONE}
	CMAKE_ARGS ${COMMON_ARGS} -DBUILD_SHARED_LIBS=OFF
		-DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON
		-DFT_DISABLE_PNG=ON -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON
	BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --parallel ${NPROCS}
)

ExternalProject_Add(plutovg
	GIT_REPOSITORY https://github.com/sammycage/plutovg
	GIT_TAG ${PLUTOVG}
	${SHALLOW_CLONE}
	CMAKE_ARGS ${COMMON_ARGS} -DBUILD_SHARED_LIBS=OFF -DPLUTOVG_BUILD_EXAMPLES=OFF
	BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --parallel ${NPROCS}
)

# plutosvg links the FreeType and plutovg installed above, hence the prefix path
# and the ordering.
ExternalProject_Add(plutosvg
	DEPENDS freetype plutovg
	GIT_REPOSITORY https://github.com/sammycage/plutosvg
	GIT_TAG ${PLUTOSVG}
	${SHALLOW_CLONE}
	CMAKE_ARGS ${COMMON_ARGS} -DCMAKE_PREFIX_PATH=${CMAKE_INSTALL_PREFIX}
		-DBUILD_SHARED_LIBS=OFF -DPLUTOSVG_ENABLE_FREETYPE=ON -DPLUTOSVG_BUILD_EXAMPLES=OFF
	BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --parallel ${NPROCS}
)

ExternalProject_Add(rapidyaml
	GIT_REPOSITORY https://github.com/biojppm/rapidyaml
	GIT_TAG ${RAPIDYAML}
	GIT_SUBMODULES_RECURSE ON
	${SHALLOW_CLONE}
	CMAKE_ARGS ${COMMON_ARGS} -DBUILD_SHARED_LIBS=OFF
	BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --parallel ${NPROCS}
)

# ------------------------------------------------------------------ libbacktrace
#
# Autotools, and the only dependency here that is not a CMake project.
if(CMAKE_CROSSCOMPILING AND CMAKE_C_COMPILER_TARGET)
	set(LIBBACKTRACE_HOST --host=${CMAKE_C_COMPILER_TARGET})
else()
	set(LIBBACKTRACE_HOST "")
endif()

ExternalProject_Add(libbacktrace
	GIT_REPOSITORY https://github.com/ianlancetaylor/libbacktrace
	GIT_TAG ${LIBBACKTRACE}
	${SHALLOW_CLONE}
	BUILD_IN_SOURCE ON
	CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=${CMAKE_INSTALL_PREFIX} --with-pic ${LIBBACKTRACE_HOST}
	BUILD_COMMAND make -j${NPROCS}
	INSTALL_COMMAND make install
)

# ------------------------------------------------------------------ shaderc
#
# Distribution libshaderc_combined.a packages are not actually self-contained -
# Ubuntu's expects the system glslang - so the real thing gets built here. Its
# dependencies come from git-sync-deps, after which glslang is moved forward to
# the revision named in deps.versions, for the reason recorded there.
find_package(Python3 COMPONENTS Interpreter REQUIRED)

ExternalProject_Add(shaderc
	GIT_REPOSITORY https://github.com/google/shaderc
	GIT_TAG ${SHADERC}
	${SHALLOW_CLONE}
	# The patch step: it runs on a fresh checkout, after the clone, which is
	# exactly when shaderc's own dependencies have to be fetched.
	PATCH_COMMAND ${Python3_EXECUTABLE} <SOURCE_DIR>/utils/git-sync-deps
	COMMAND ${CMAKE_COMMAND} -DGLSLANG_DIR=<SOURCE_DIR>/third_party/glslang -DGLSLANG_REV=${GLSLANG}
		-P ${CMAKE_CURRENT_SOURCE_DIR}/PinGlslang.cmake
	CMAKE_ARGS ${COMMON_ARGS} -DCMAKE_POLICY_VERSION_MINIMUM=3.5
		-DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON -DSHADERC_SKIP_COPYRIGHT_CHECK=ON
	BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --parallel ${NPROCS} --target shaderc_combined
	# shaderc has no install rule for the combined archive, so place it by hand.
	INSTALL_COMMAND ${CMAKE_COMMAND} -E copy <BINARY_DIR>/libshaderc/libshaderc_combined.a
			${CMAKE_INSTALL_PREFIX}/lib/libshaderc_combined.a
	COMMAND ${CMAKE_COMMAND} -E copy_directory <SOURCE_DIR>/libshaderc/include/shaderc
			${CMAKE_INSTALL_PREFIX}/include/shaderc
)

# ------------------------------------------------------------------ optional
#
# Off by default: these are packaged everywhere, and are only built when the
# distribution's copy is older than what PCSX2 asks for (Debian Bookworm ships
# libpng 1.6.39 and zstd 1.5.4 against minimums of 1.6.40 and 1.5.5), or when a
# libjpeg SONAME would follow the core to a machine that has no such library.
if(PCEE2_BUILD_PNG_ZSTD)
	ExternalProject_Add(libpng
		GIT_REPOSITORY https://github.com/pnggroup/libpng
		GIT_TAG ${LIBPNG_OPTIONAL}
		${SHALLOW_CLONE}
		CMAKE_ARGS ${COMMON_ARGS} -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF -DPNG_TOOLS=OFF
		BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --parallel ${NPROCS}
	)

	ExternalProject_Add(zstd
		GIT_REPOSITORY https://github.com/facebook/zstd
		GIT_TAG ${ZSTD}
		${SHALLOW_CLONE}
		SOURCE_SUBDIR build/cmake
		CMAKE_ARGS ${COMMON_ARGS} -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON
			-DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF
		BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --parallel ${NPROCS}
	)
endif()

if(PCEE2_BUILD_JPEG)
	ExternalProject_Add(libjpeg-turbo
		GIT_REPOSITORY https://github.com/libjpeg-turbo/libjpeg-turbo
		GIT_TAG ${JPEGTURBO}
		${SHALLOW_CLONE}
		CMAKE_ARGS ${COMMON_ARGS} -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TURBOJPEG=OFF
		BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --parallel ${NPROCS}
	)
endif()
