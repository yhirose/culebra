# Publish a freshly built vendored dependency (SDL3, raylib) from its staging
# install dir into the shared deps cache. Invoked as an ExternalProject step:
#   cmake -DSTAGE=<staging install dir> -DDEST=<cache dir> -DPROBE=<file> -P publish_dep.cmake
#
# The rename is what makes the cache safe to share between build dirs and
# worktrees: the destination appears atomically, complete, so a concurrent cold
# build elsewhere either doesn't see it yet (and builds its own copy) or sees a
# finished tree — never a half-written archive.
#
# PROBE is the artifact whose presence means "a complete tree is already here"
# — the static library the consumer links. The test cannot be the directory:
# Ninja creates the directories of a rule's declared byproducts before running
# it, so DEST already exists, empty, the first time this step runs under that
# generator. Testing the directory therefore made a cold Ninja build publish
# nothing at all and leave an empty shell behind, which the dependent build
# then failed to find its dependency in.

if(NOT STAGE OR NOT DEST OR NOT PROBE)
  message(FATAL_ERROR "publish_dep: STAGE, DEST and PROBE are required")
endif()

# Someone else won the race (or this build dir is reconfiguring over a hit):
# their tree is as good as ours, so drop ours and use theirs.
if(EXISTS "${PROBE}")
  file(REMOVE_RECURSE "${STAGE}")
  return()
endif()

# Nothing complete is at DEST, so anything there is the empty shell above (or
# the remains of a build that died mid-publish). Clear it, so the rename below
# stays the atomic swap it is meant to be instead of failing or merging into a
# directory that already exists.
file(REMOVE_RECURSE "${DEST}")

get_filename_component(_parent "${DEST}" DIRECTORY)
file(MAKE_DIRECTORY "${_parent}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E rename "${STAGE}" "${DEST}"
                RESULT_VARIABLE _rc ERROR_QUIET)
if(NOT _rc EQUAL 0)
  # rename can't cross filesystems, and loses the race if another build landed
  # first. Copy in that case, unless the winner already put a tree there.
  if(NOT EXISTS "${PROBE}")
    file(COPY "${STAGE}/" DESTINATION "${DEST}")
  endif()
  file(REMOVE_RECURSE "${STAGE}")
endif()
