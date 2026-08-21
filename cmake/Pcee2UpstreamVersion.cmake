# Overrides the version PCSX2's own git description produced with the upstream
# PCSX2 release this core is merged up to, recorded in
# pcee2-libretro/upstream.version. Without this the version comes from
# `git describe` against pcee2's own tags, which says nothing about which PCSX2
# the emulation code actually is.
#
# Only the human-facing version is replaced: PCSX2_GIT_HASH and PCSX2_GIT_DATE
# keep pointing at the pcee2 commit that was built.

function(apply_pcee2_upstream_version)
	set(version_file "${PROJECT_SOURCE_DIR}/pcee2-libretro/upstream.version")
	if(NOT EXISTS "${version_file}")
		message(WARNING "pcee2: ${version_file} is missing; falling back to the git description.")
		return()
	endif()

	file(STRINGS "${version_file}" version_lines REGEX "^PCSX2_(VERSION|COMMIT)=")
	foreach(line IN LISTS version_lines)
		if(line MATCHES "^PCSX2_VERSION=(.+)$")
			set(upstream_version "${CMAKE_MATCH_1}")
		elseif(line MATCHES "^PCSX2_COMMIT=(.+)$")
			set(upstream_commit "${CMAKE_MATCH_1}")
		endif()
	endforeach()

	if(NOT upstream_version)
		message(WARNING "pcee2: no PCSX2_VERSION in ${version_file}; falling back to the git description.")
		return()
	endif()

	# Catch a forgotten bump: the recorded commit must be part of this tree's
	# history. Skipped for shallow clones, which cannot answer the question.
	if(GIT_FOUND AND upstream_commit AND EXISTS "${PROJECT_SOURCE_DIR}/.git")
		execute_process(WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
			COMMAND ${GIT_EXECUTABLE} rev-parse --is-shallow-repository
			OUTPUT_VARIABLE is_shallow OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
		if(NOT is_shallow STREQUAL "true")
			execute_process(WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
				COMMAND ${GIT_EXECUTABLE} merge-base --is-ancestor ${upstream_commit} HEAD
				RESULT_VARIABLE ancestor_result ERROR_QUIET)
			if(NOT ancestor_result EQUAL 0)
				message(WARNING "pcee2: ${upstream_version} (${upstream_commit}) is not an ancestor of HEAD - "
					"pcee2-libretro/upstream.version is stale or the upstream merge is missing.")
			endif()
		endif()
	endif()

	message(STATUS "pcee2: reporting the upstream PCSX2 version ${upstream_version}.")
	set(PCSX2_GIT_TAG "${upstream_version}" PARENT_SCOPE)
	set(PCSX2_GIT_REV "${upstream_version}" PARENT_SCOPE)
endfunction()
