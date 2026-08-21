# Builds the dependencies that distributions either do not package or package
# too old, into CMAKE_INSTALL_PREFIX (static and PIC, so they end up inside the
# core). Same sources, same revisions and same flags as
# scripts/build-deps-linux.sh, scripts/build-deps-macos.sh and
# scripts/build-deps-windows.bat next door, which stay as the path the CI jobs
# take; this one exists so that a plain "cmake --build" on an unusual platform
# is enough (issue #17).
#
# What gets built depends on the platform. On Linux, everything a normal
# distribution packages (zlib, libpng, ffmpeg, ...) is left to the distribution -
# building it from source would only put a second copy in front of a perfectly
# good one. macOS and Windows have no such packages, so there the image and
# compression libraries are built here too, exactly as the two platform scripts
# do it.

cmake_minimum_required(VERSION 3.16)
# C rather than NONE: MSVC and friends are set when a compiler is enabled, and
# the Windows branches below need to know which one this is. It also gives the
# toolchain a chance to fail here rather than inside the first dependency.
project(pcee2-deps C)

include(ExternalProject)
include(ProcessorCount)

ProcessorCount(HOST_CORES)
if(HOST_CORES EQUAL 0)
	set(HOST_CORES 1)
endif()
# A knob rather than a constant: these are the heaviest C++ builds in the tree
# (shaderc pulls in glslang and SPIRV-Tools), and a small board with 4 cores and
# little RAM does better with fewer.
set(NPROCS ${HOST_CORES} CACHE STRING "Parallel jobs to build each dependency with")

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
option(BUILD_IMAGE_LIBS "Build libwebp, lz4" OFF)

# macOS and Windows ship none of the image and compression libraries the core
# needs, so there they are not optional.
if(APPLE OR WIN32)
	set(BUILD_IMAGE_LIBS ON)
endif()

# What every dependency gets: the build type, the install prefix, PIC (the core
# is a shared library, so everything inside it has to be), and the toolchain the
# parent build is using. CMAKE_TOOLCHAIN_FILE is how a cross build (webOS, an
# embedded SDK) reaches these as well.
set(COMMON_ARGS
	-DCMAKE_BUILD_TYPE=Release
	-DCMAKE_INSTALL_PREFIX=${CMAKE_INSTALL_PREFIX}
	-DCMAKE_PREFIX_PATH=${CMAKE_INSTALL_PREFIX}
	-DCMAKE_POSITION_INDEPENDENT_CODE=ON
)
foreach(_var CMAKE_TOOLCHAIN_FILE CMAKE_C_COMPILER CMAKE_CXX_COMPILER CMAKE_C_COMPILER_LAUNCHER
		CMAKE_CXX_COMPILER_LAUNCHER CMAKE_SYSROOT CMAKE_OSX_DEPLOYMENT_TARGET CMAKE_OSX_ARCHITECTURES
		ANDROID_ABI ANDROID_PLATFORM)
	if(DEFINED ${_var})
		list(APPEND COMMON_ARGS -D${_var}=${${_var}})
	endif()
endforeach()

# The core links the CRT statically so it loads without the VC++ redistributable
# installed, so every dependency has to use /MT as well - mixing runtimes in one
# binary is a link error at best. CMAKE_MSVC_RUNTIME_LIBRARY only takes effect
# under CMP0091 NEW, and CMAKE_POLICY_VERSION_MINIMUM, which the projects asking
# for an ancient CMake need, puts that policy back to OLD and would silently
# leave them on /MD. Set the policy itself.
if(MSVC)
	list(APPEND COMMON_ARGS
		-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
		-DCMAKE_POLICY_DEFAULT_CMP0091=NEW)
endif()

# Every revision here is pinned, so there is nothing for the update step to do -
# and disabling it keeps a rebuild off the network entirely. Note that
# UPDATE_DISCONNECTED would not do: it takes the patch step out of the build's
# dependency chain along with the update step, and shaderc needs its patch step.
set(SHALLOW_CLONE GIT_SHALLOW ON GIT_PROGRESS OFF UPDATE_COMMAND "")

set(COPY_IF_EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/CopyIfExists.cmake")

# ------------------------------------------------------------------ zlib
#
# Windows only: every other platform has one, and on Linux using the system copy
# is the whole point.
if(WIN32)
	ExternalProject_Add(zlib
		GIT_REPOSITORY https://github.com/madler/zlib
		GIT_TAG ${ZLIB}
		${SHALLOW_CLONE}
		CMAKE_ARGS ${COMMON_ARGS} -DZLIB_BUILD_SHARED=OFF -DZLIB_BUILD_STATIC=ON
			-DZLIB_BUILD_TESTING=OFF -DZLIB_BUILD_MINIZIP=OFF
		BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --parallel ${NPROCS}
		# find_package(ZLIB) looks for zlib.lib; the static build produces
		# zlibstatic.lib (or zs.lib on newer revisions).
		INSTALL_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target install
		COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_INSTALL_PREFIX}/lib/zlibstatic.lib
			-DDST=${CMAKE_INSTALL_PREFIX}/lib/zlib.lib -P ${COPY_IF_EXISTS}
		COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_INSTALL_PREFIX}/lib/zs.lib
			-DDST=${CMAKE_INSTALL_PREFIX}/lib/zlib.lib -P ${COPY_IF_EXISTS}
	)
	set(ZLIB_TARGET zlib)
else()
	set(ZLIB_TARGET "")
endif()

# ------------------------------------------------------------------ image and compression
#
# On Linux these are the optional ones: built only when the distribution's copy
# is older than what PCSX2 asks for (Debian Bookworm ships libpng 1.6.39 and
# zstd 1.5.4 against minimums of 1.6.40 and 1.5.5), or when a libjpeg SONAME
# would follow the core to a machine that has no such library. On macOS and
# Windows they are always built.
if(PCEE2_BUILD_PNG_ZSTD OR BUILD_IMAGE_LIBS)
	# The platform builds take the current libpng; the Linux one only needs to
	# clear PCSX2's minimum, and staying near it keeps the surprise small.
	if(BUILD_IMAGE_LIBS)
		set(LIBPNG_REV ${LIBPNG})
	else()
		set(LIBPNG_REV ${LIBPNG_OPTIONAL})
	endif()

	ExternalProject_Add(libpng
		DEPENDS ${ZLIB_TARGET}
		GIT_REPOSITORY https://github.com/pnggroup/libpng
		GIT_TAG ${LIBPNG_REV}
		${SHALLOW_CLONE}
		CMAKE_ARGS ${COMMON_ARGS} -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF
			-DPNG_TOOLS=OFF -DPNG_FRAMEWORK=OFF
		BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --parallel ${NPROCS}
		INSTALL_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target install
		COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_INSTALL_PREFIX}/lib/libpng16_static.lib
			-DDST=${CMAKE_INSTALL_PREFIX}/lib/libpng16.lib -P ${COPY_IF_EXISTS}
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

if(PCEE2_BUILD_JPEG OR BUILD_IMAGE_LIBS)
	ExternalProject_Add(libjpeg-turbo
		GIT_REPOSITORY https://github.com/libjpeg-turbo/libjpeg-turbo
		GIT_TAG ${JPEGTURBO}
		${SHALLOW_CLONE}
		CMAKE_ARGS ${COMMON_ARGS} -DENABLE_SHARED=OFF -DENABLE_STATIC=ON
			-DWITH_SIMD=OFF -DWITH_TURBOJPEG=OFF
		BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --parallel ${NPROCS}
		INSTALL_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target install
		COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_INSTALL_PREFIX}/lib/jpeg-static.lib
			-DDST=${CMAKE_INSTALL_PREFIX}/lib/jpeg.lib -P ${COPY_IF_EXISTS}
	)
endif()

if(BUILD_IMAGE_LIBS)
	# libwebp installs sharpyuv as a second archive, and module-style
	# find_package(WebP) only ever names libwebp, so the two are merged into one
	# archive the way the platform scripts do it.
	if(APPLE)
		find_program(LIBTOOL_EXE libtool)
		if(NOT LIBTOOL_EXE)
			message(FATAL_ERROR "libtool not found; it comes with the Xcode command line tools")
		endif()
		set(WEBP_MERGE_COMMAND
			COMMAND ${LIBTOOL_EXE} -static -o ${CMAKE_INSTALL_PREFIX}/lib/libwebp_merged.a
				${CMAKE_INSTALL_PREFIX}/lib/libwebp.a ${CMAKE_INSTALL_PREFIX}/lib/libsharpyuv.a
			COMMAND ${CMAKE_COMMAND} -E rename ${CMAKE_INSTALL_PREFIX}/lib/libwebp_merged.a
				${CMAKE_INSTALL_PREFIX}/lib/libwebp.a)
	elseif(WIN32)
		find_program(LIB_EXE NAMES lib)
		if(NOT LIB_EXE)
			message(FATAL_ERROR "lib.exe not found; run this from a Visual Studio developer prompt")
		endif()
		set(WEBP_MERGE_COMMAND
			COMMAND ${LIB_EXE} /OUT:${CMAKE_INSTALL_PREFIX}/lib/webp_merged.lib
				${CMAKE_INSTALL_PREFIX}/lib/libwebp.lib ${CMAKE_INSTALL_PREFIX}/lib/libsharpyuv.lib
			COMMAND ${CMAKE_COMMAND} -E rename ${CMAKE_INSTALL_PREFIX}/lib/webp_merged.lib
				${CMAKE_INSTALL_PREFIX}/lib/libwebp.lib)
	endif()

	ExternalProject_Add(libwebp
		GIT_REPOSITORY https://github.com/webmproject/libwebp
		GIT_TAG ${WEBP}
		${SHALLOW_CLONE}
		CMAKE_ARGS ${COMMON_ARGS} -DBUILD_SHARED_LIBS=OFF
			-DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF
			-DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF
			-DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF
		BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --parallel ${NPROCS}
		INSTALL_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target install
		${WEBP_MERGE_COMMAND}
	)

	ExternalProject_Add(lz4
		GIT_REPOSITORY https://github.com/lz4/lz4
		GIT_TAG ${LZ4}
		${SHALLOW_CLONE}
		SOURCE_SUBDIR build/cmake
		CMAKE_ARGS ${COMMON_ARGS} -DBUILD_SHARED_LIBS=OFF
			-DLZ4_BUILD_CLI=OFF -DLZ4_BUILD_LEGACY_LZ4C=OFF
		BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --parallel ${NPROCS}
		INSTALL_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target install
	)
endif()

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
#
# Where zlib and libpng come from this prefix, FreeType is told to require them,
# so a silently featureless build cannot happen; on Linux they are the system
# ones and FreeType does not need them at all.
if(BUILD_IMAGE_LIBS)
	set(FREETYPE_DEPENDS DEPENDS libpng)
	set(FREETYPE_ARGS -DFT_REQUIRE_ZLIB=TRUE -DFT_REQUIRE_PNG=TRUE
		-DFT_DISABLE_BZIP2=TRUE -DFT_DISABLE_BROTLI=TRUE -DFT_DISABLE_HARFBUZZ=TRUE)
else()
	set(FREETYPE_DEPENDS "")
	set(FREETYPE_ARGS -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON
		-DFT_DISABLE_PNG=ON -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON)
endif()

ExternalProject_Add(freetype
	${FREETYPE_DEPENDS}
	GIT_REPOSITORY https://github.com/freetype/freetype
	GIT_TAG ${FREETYPE}
	${SHALLOW_CLONE}
	CMAKE_ARGS ${COMMON_ARGS} -DBUILD_SHARED_LIBS=OFF ${FREETYPE_ARGS}
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
	CMAKE_ARGS ${COMMON_ARGS} -DBUILD_SHARED_LIBS=OFF
		-DPLUTOSVG_ENABLE_FREETYPE=ON -DPLUTOSVG_BUILD_EXAMPLES=OFF
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
# Autotools, and the only dependency here that is not a CMake project. Linux
# only, the same as in the platform scripts: the Windows and macOS builds of the
# core do not use it.
if(UNIX AND NOT APPLE)
	if(CMAKE_CROSSCOMPILING)
		if(CMAKE_C_COMPILER_TARGET)
			set(LIBBACKTRACE_HOST "--host=${CMAKE_C_COMPILER_TARGET}")
		elseif(CMAKE_C_COMPILER)
			execute_process(
				COMMAND ${CMAKE_C_COMPILER} -dumpmachine
				OUTPUT_VARIABLE LIBBACKTRACE_TARGET
				OUTPUT_STRIP_TRAILING_WHITESPACE
			)
			set(LIBBACKTRACE_HOST "--host=${LIBBACKTRACE_TARGET}")
		endif()
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
endif()

# ------------------------------------------------------------------ DirectX-Headers
#
# Windows only, for the D3D12 renderer.
if(WIN32)
	ExternalProject_Add(directx-headers
		GIT_REPOSITORY https://github.com/microsoft/DirectX-Headers
		GIT_TAG ${DXHEADERS}
		${SHALLOW_CLONE}
		CMAKE_ARGS ${COMMON_ARGS} -DBUILD_SHARED_LIBS=OFF -DDXHEADERS_BUILD_TEST=OFF
			-DDXHEADERS_BUILD_GOOGLE_TEST=OFF -DDXHEADERS_INSTALL=ON
		BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --parallel ${NPROCS}
	)
endif()

# ------------------------------------------------------------------ shaderc
#
# Distribution libshaderc_combined.a packages are not actually self-contained -
# Ubuntu's expects the system glslang - so the real thing gets built here. Its
# dependencies come from git-sync-deps, after which glslang is moved forward to
# the revision named in deps.versions, for the reason recorded there.
find_package(Python3 COMPONENTS Interpreter REQUIRED)

# SPIRV-Tools and glslang generate sources with Python scripts of their own, so
# hand them the interpreter found here rather than letting find_package() guess
# (PYTHON_EXECUTABLE covers the older FindPythonInterp path they still use).
set(SHADERC_ARGS
	-DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON -DSHADERC_SKIP_COPYRIGHT_CHECK=ON
	-DPython3_EXECUTABLE=${Python3_EXECUTABLE} -DPYTHON_EXECUTABLE=${Python3_EXECUTABLE})
if(MSVC)
	# The static CRT again, this time through shaderc's own switch.
	list(APPEND SHADERC_ARGS -DSHADERC_ENABLE_SHARED_CRT=OFF)
	set(SHADERC_COMBINED shaderc_combined.lib)
else()
	set(SHADERC_COMBINED libshaderc_combined.a)
endif()

ExternalProject_Add(shaderc
	GIT_REPOSITORY https://github.com/google/shaderc
	GIT_TAG ${SHADERC}
	${SHALLOW_CLONE}
	# The patch step: it runs on a fresh checkout, after the clone, which is
	# exactly when shaderc's own dependencies have to be fetched.
	PATCH_COMMAND ${Python3_EXECUTABLE} <SOURCE_DIR>/utils/git-sync-deps
	COMMAND ${CMAKE_COMMAND} -DGLSLANG_DIR=<SOURCE_DIR>/third_party/glslang -DGLSLANG_REV=${GLSLANG}
		-P ${CMAKE_CURRENT_SOURCE_DIR}/PinGlslang.cmake
	CMAKE_ARGS ${COMMON_ARGS} -DCMAKE_POLICY_VERSION_MINIMUM=3.5 ${SHADERC_ARGS}
	BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --parallel ${NPROCS} --target shaderc_combined
	# shaderc has no install rule for the combined archive, so place it by hand.
	INSTALL_COMMAND ${CMAKE_COMMAND} -E copy <BINARY_DIR>/libshaderc/${SHADERC_COMBINED}
			${CMAKE_INSTALL_PREFIX}/lib/${SHADERC_COMBINED}
	COMMAND ${CMAKE_COMMAND} -E copy_directory <SOURCE_DIR>/libshaderc/include/shaderc
			${CMAKE_INSTALL_PREFIX}/include/shaderc
)

# ------------------------------------------------------------------ MoltenVK
#
# macOS has no Vulkan driver; the core dlopen's this one. Prebuilt, because
# building MoltenVK from source pulls in the whole Vulkan SDK toolchain for a
# library that ships as a single dylib.
if(APPLE)
	ExternalProject_Add(moltenvk
		URL https://github.com/KhronosGroup/MoltenVK/releases/download/${MOLTENVK}/MoltenVK-macos.tar
		DOWNLOAD_NO_PROGRESS ON
		CONFIGURE_COMMAND ""
		BUILD_COMMAND ""
		INSTALL_COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_INSTALL_PREFIX}/lib
		COMMAND ${CMAKE_COMMAND} -E copy <SOURCE_DIR>/MoltenVK/dylib/macOS/libMoltenVK.dylib
			${CMAKE_INSTALL_PREFIX}/lib/libMoltenVK.dylib
	)
endif()
