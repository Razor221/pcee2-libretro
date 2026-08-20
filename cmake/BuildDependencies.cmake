# Builds deps/ into a prefix inside the build directory and points the rest of
# the configure at it. This has to happen while configuring, not while building:
# SearchForStuff's find_package() calls run at configure time, so the libraries
# have to be installed by then.
#
# Enable with -DPCEE2_BUILD_DEPS=ON. Off by default, so the CI jobs keep taking
# the shell scripts they already know.

set(PCEE2_DEPS_PREFIX "${CMAKE_BINARY_DIR}/deps" CACHE PATH
	"Where -DPCEE2_BUILD_DEPS=ON installs the dependencies it builds")
set(PCEE2_DEPS_ARGS "" CACHE STRING
	"Extra -D arguments for the dependency build (semicolon separated)")
set(PCEE2_DEPS_JOBS "" CACHE STRING
	"Parallel jobs to build each dependency with (default: as many as there are cores)")

set(_deps_build "${CMAKE_BINARY_DIR}/deps-build")

# Pass the toolchain along, or a cross build would compile its dependencies for
# the host and only find out at link time.
set(_deps_args
	-DCMAKE_INSTALL_PREFIX=${PCEE2_DEPS_PREFIX}
	-DPCEE2_SDL_STATIC=${PCEE2_SDL_STATIC}
	-DPCEE2_BUILD_PNG_ZSTD=${PCEE2_BUILD_PNG_ZSTD}
	-DPCEE2_BUILD_JPEG=${PCEE2_BUILD_JPEG}
)
if(PCEE2_DEPS_JOBS)
	list(APPEND _deps_args -DNPROCS=${PCEE2_DEPS_JOBS})
endif()
foreach(_var CMAKE_TOOLCHAIN_FILE CMAKE_C_COMPILER CMAKE_CXX_COMPILER CMAKE_C_COMPILER_LAUNCHER
		CMAKE_CXX_COMPILER_LAUNCHER CMAKE_SYSROOT CMAKE_OSX_DEPLOYMENT_TARGET CMAKE_OSX_ARCHITECTURES
		ANDROID_ABI ANDROID_PLATFORM)
	if(DEFINED ${_var})
		list(APPEND _deps_args -D${_var}=${${_var}})
	endif()
endforeach()
list(APPEND _deps_args ${PCEE2_DEPS_ARGS})

message(STATUS "Building dependencies into ${PCEE2_DEPS_PREFIX} (this takes a while the first time)")

execute_process(
	COMMAND ${CMAKE_COMMAND} -S "${CMAKE_SOURCE_DIR}/pcee2-libretro/deps" -B "${_deps_build}"
		-G "${CMAKE_GENERATOR}" ${_deps_args}
	RESULT_VARIABLE _deps_result)
if(NOT _deps_result EQUAL 0)
	message(FATAL_ERROR "Configuring the dependency build failed (${_deps_result})")
endif()

execute_process(
	COMMAND ${CMAKE_COMMAND} --build "${_deps_build}"
	RESULT_VARIABLE _deps_result)
if(NOT _deps_result EQUAL 0)
	message(FATAL_ERROR "Building the dependencies failed (${_deps_result})")
endif()

# Ahead of the system copies, since the reason for building a dependency here is
# that the system one is missing or too old.
list(PREPEND CMAKE_PREFIX_PATH "${PCEE2_DEPS_PREFIX}")
if(CMAKE_FIND_ROOT_PATH)
	# Under a cross-compiling toolchain, ONLY/BOTH keeps find_package() out of
	# anything not named here.
	list(PREPEND CMAKE_FIND_ROOT_PATH "${PCEE2_DEPS_PREFIX}")
endif()

# FindShaderc looks for the shared library; what gets built here is the combined
# static archive. SHADERC_STATIC goes with it - without that the core would be
# built to load a shaderc shared library at runtime, and there is none to load.
foreach(_combined libshaderc_combined.a shaderc_combined.lib)
	if(NOT SHADERC_LIBRARY AND EXISTS "${PCEE2_DEPS_PREFIX}/lib/${_combined}")
		set(SHADERC_LIBRARY "${PCEE2_DEPS_PREFIX}/lib/${_combined}" CACHE FILEPATH "" FORCE)
		set(SHADERC_STATIC ON CACHE BOOL "" FORCE)
	endif()
endforeach()
