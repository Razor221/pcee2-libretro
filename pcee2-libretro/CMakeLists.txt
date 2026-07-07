add_library(pcee2_libretro SHARED)

set_target_properties(pcee2_libretro PROPERTIES
	OUTPUT_NAME "pcee2_libretro"
	PREFIX ""
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
	LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if (PACKAGE_MODE)
	install(TARGETS pcee2_libretro DESTINATION ${CMAKE_INSTALL_LIBDIR})
else()
	install(TARGETS pcee2_libretro DESTINATION ${CMAKE_SOURCE_DIR}/bin)
endif()

# The libretro buildbot expects the .so directly in the build dir root (it does
# `mv $BUILD_DIR/pcee2_libretro.so ...`), while the GitHub/standalone packaging
# uses bin/. Copy it to the build root too so both layouts work.
add_custom_command(TARGET pcee2_libretro POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy
		$<TARGET_FILE:pcee2_libretro> ${CMAKE_BINARY_DIR}/pcee2_libretro.so)

target_sources(pcee2_libretro PRIVATE
	Libretro.cpp
	libretro.h
)

target_include_directories(pcee2_libretro PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}"
	"${CMAKE_BINARY_DIR}/common/include"
	"${CMAKE_SOURCE_DIR}/pcsx2"
)

target_link_libraries(pcee2_libretro PRIVATE
	PCSX2_FLAGS
	PCSX2
)

# undefined symbols in a core otherwise surface only when the frontend loads
# it ("symbol lookup error" at runtime) - fail the link instead
if(UNIX AND NOT APPLE)
	target_link_options(pcee2_libretro PRIVATE "-Wl,--no-undefined")
endif()
