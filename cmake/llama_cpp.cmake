# The llama.cpp release the Llama namespace is built against, and the SHA-256 of
# its source archive (https://github.com/ggml-org/llama.cpp/archive/refs/tags/<version>.tar.gz).
#
# vendor/cpp-llamalib carries the tag of the same version: its releases are
# named for the llama.cpp release they were built and tested against.
# tools/checks/check_llama_versions.sh keeps the two together, so a bump is made
# in both places in one commit, or `just test-dev` says which side is behind.
set(CULEBRA_LLAMA_CPP_VERSION v0.8.0)
set(CULEBRA_LLAMA_CPP_SHA256
    51b1e025d7b12e83fa55736e65bc254acd9d23e49f546be7531368fef82210ee)
