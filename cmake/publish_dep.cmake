# Publish a freshly built vendored dependency (SDL3, raylib) from its staging
# install dir into the shared deps cache. Invoked as an ExternalProject step:
#   cmake -DSTAGE=<staging install dir> -DDEST=<cache dir> -P publish_dep.cmake
#
# The rename is what makes the cache safe to share between build dirs and
# worktrees: the destination appears atomically, complete, so a concurrent cold
# build elsewhere either doesn't see it yet (and builds its own copy) or sees a
# finished tree — never a half-written archive.

if(NOT STAGE OR NOT DEST)
  message(FATAL_ERROR "publish_dep: STAGE and DEST are required")
endif()

# Someone else won the race (or this build dir is reconfiguring over a hit):
# their tree is as good as ours, so drop ours and use theirs.
if(EXISTS "${DEST}")
  file(REMOVE_RECURSE "${STAGE}")
  return()
endif()

get_filename_component(_parent "${DEST}" DIRECTORY)
file(MAKE_DIRECTORY "${_parent}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E rename "${STAGE}" "${DEST}"
                RESULT_VARIABLE _rc ERROR_QUIET)
if(NOT _rc EQUAL 0)
  # rename can't cross filesystems, and loses the race if another build landed
  # first. Copy in that case, unless the winner already put a tree there.
  if(NOT EXISTS "${DEST}")
    file(COPY "${STAGE}/" DESTINATION "${DEST}")
  endif()
  file(REMOVE_RECURSE "${STAGE}")
endif()
