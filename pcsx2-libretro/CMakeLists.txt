add_library(pcsx2-libretro SHARED)

set_target_properties(pcsx2-libretro PROPERTIES
	OUTPUT_NAME "pcee2_libretro"
	PREFIX ""
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
	LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if (PACKAGE_MODE)
	install(TARGETS pcsx2-libretro DESTINATION ${CMAKE_INSTALL_LIBDIR})
else()
	install(TARGETS pcsx2-libretro DESTINATION ${CMAKE_SOURCE_DIR}/bin)
endif()

target_sources(pcsx2-libretro PRIVATE
	Libretro.cpp
	libretro.h
)

target_include_directories(pcsx2-libretro PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}"
	"${CMAKE_BINARY_DIR}/common/include"
	"${CMAKE_SOURCE_DIR}/pcsx2"
)

target_link_libraries(pcsx2-libretro PRIVATE
	PCSX2_FLAGS
	PCSX2
)
