# cmake -DSRC=<file> -DDST=<file> -P CopyIfExists.cmake
#
# MSVC builds of zlib, libpng and libjpeg-turbo name their static libraries
# differently depending on the version and on which options were on, and
# find_package() looks for the plain name. Copying the one that exists is what
# build-deps-windows.bat does with `if exist ... copy`; `cmake -E copy` would
# fail on the name that did not get built, so it cannot be used directly.

if(EXISTS "${SRC}")
	configure_file("${SRC}" "${DST}" COPYONLY)
endif()
