# Idempotent `git apply` for a FetchContent PATCH_COMMAND.
#
# FetchContent re-runs PATCH_COMMAND whenever it re-populates, and a plain
# `git apply` fails the second time because the change is already present. This
# checks for that case first and treats it as success, so a reconfigure is not
# a build failure. Any other failure is still fatal: silently building an
# unpatched Dawn would produce a runtime that reports
# DawnNativeHandlesUnavailable with no explanation.
#
# Run with -DGIT_EXECUTABLE=... -DPATCH_FILE=... and the source tree as the
# working directory, which is what FetchContent supplies.

if(NOT DEFINED GIT_EXECUTABLE OR NOT DEFINED PATCH_FILE)
    message(FATAL_ERROR "AuroraApplyPatch.cmake requires GIT_EXECUTABLE and PATCH_FILE")
endif()

if(NOT EXISTS "${PATCH_FILE}")
    message(FATAL_ERROR "Patch file does not exist: ${PATCH_FILE}")
endif()

# --reverse --check succeeds only when the patch is already applied. The source
# tree is an extracted archive rather than a repository, so -p1 and
# --unsafe-paths are needed for git apply to work outside a work tree.
set(_git_apply ${GIT_EXECUTABLE} apply -p1 --unsafe-paths --directory=.)

execute_process(
    COMMAND ${_git_apply} --reverse --check "${PATCH_FILE}"
    RESULT_VARIABLE _already_applied
    OUTPUT_QUIET ERROR_QUIET)
if(_already_applied EQUAL 0)
    message(STATUS "aurora: Dawn patch is already applied; skipping")
    return()
endif()

execute_process(
    COMMAND ${_git_apply} "${PATCH_FILE}"
    RESULT_VARIABLE _applied
    ERROR_VARIABLE _apply_error)
if(NOT _applied EQUAL 0)
    message(FATAL_ERROR
        "aurora: failed to apply ${PATCH_FILE} to the vendored Dawn tree.\n"
        "This usually means the pinned Dawn version moved and the patch needs "
        "rebasing. Without it, OpenXR Vulkan interop cannot bind to Dawn's "
        "device.\n${_apply_error}")
endif()
message(STATUS "aurora: applied the Dawn Vulkan native-handle patch")
