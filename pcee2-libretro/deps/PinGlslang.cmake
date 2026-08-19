# Moves shaderc's glslang checkout to the revision named in deps.versions.
# git-sync-deps has just put DEPS' own revision in place; fetching the bare
# commit keeps this pinned rather than tracking whatever main is today.
#
# Run as: cmake -DGLSLANG_DIR=... -DGLSLANG_REV=... -P PinGlslang.cmake

if(NOT GLSLANG_DIR OR NOT GLSLANG_REV)
	message(FATAL_ERROR "GLSLANG_DIR and GLSLANG_REV are both required")
endif()

find_package(Git REQUIRED)

execute_process(COMMAND ${GIT_EXECUTABLE} -C ${GLSLANG_DIR} rev-parse HEAD
	OUTPUT_VARIABLE current_rev OUTPUT_STRIP_TRAILING_WHITESPACE
	RESULT_VARIABLE rev_parse_result)

if(rev_parse_result EQUAL 0 AND current_rev STREQUAL GLSLANG_REV)
	message(STATUS "glslang is already at ${GLSLANG_REV}")
	return()
endif()

message(STATUS "Moving glslang to ${GLSLANG_REV}")
execute_process(COMMAND ${GIT_EXECUTABLE} -C ${GLSLANG_DIR} fetch --depth 1 origin ${GLSLANG_REV}
	COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND ${GIT_EXECUTABLE} -C ${GLSLANG_DIR} checkout --detach FETCH_HEAD
	COMMAND_ERROR_IS_FATAL ANY)
