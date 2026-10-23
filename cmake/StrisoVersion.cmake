# Firmware version string, as the firmware Makefile and tools/build.sh made it:
#   <commit date YYYY-MM-DD>_<git describe --tags --always --long --dirty>[_<voice>]
# e.g. 2026-10-23_v2.2.4-69-gd9d7b7c_all. The board reports it (USB console 'version', INFO_FW.TXT)
# and tools/striso.py checks it after flashing. The voice suffix is left out for the default
# voice (strisy).
#
# striso_git_version(<out-var>)  sets <out-var> to the date_describe part, from STRISO_GIT_VERSION
# if set (tools/build.sh computes it on the host: inside Docker a worktree's .git link points
# nowhere), else from git, else "unknown".

function(striso_git_version out)
  if(STRISO_GIT_VERSION)
    set(${out} "${STRISO_GIT_VERSION}" PARENT_SCOPE)
    # not remembered: a later configure without it must not report a stale version
    unset(STRISO_GIT_VERSION CACHE)
    return()
  endif()
  find_package(Git QUIET)
  set(v "unknown")
  if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" --no-pager show -s --date=short --format=%ad
                    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" OUTPUT_VARIABLE date
                    OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE rc1 ERROR_QUIET)
    execute_process(COMMAND "${GIT_EXECUTABLE}" --no-pager describe --tags --always --long --dirty
                    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" OUTPUT_VARIABLE desc
                    OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE rc2 ERROR_QUIET)
    if(rc1 EQUAL 0 AND rc2 EQUAL 0)
      set(v "${date}_${desc}")
    endif()
  endif()
  set(${out} "${v}" PARENT_SCOPE)
endfunction()
