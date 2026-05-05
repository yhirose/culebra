cmake_minimum_required(VERSION 3.20)
project(culebra)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CULEBRA_ENABLE_JIT "Enable LLVM JIT compilation" OFF)

if(MSVC)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /utf-8")
endif()

include_directories(./include)
include_directories(./vendor/cpp-peglib)
include_directories(./vendor/cpp-linenoise)

if(CULEBRA_ENABLE_JIT)
  find_package(LLVM REQUIRED CONFIG)
  message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
  message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

  include_directories(${LLVM_INCLUDE_DIRS})
  add_definitions(${LLVM_DEFINITIONS})

  llvm_map_components_to_libnames(llvm_libs
    core support orcjit native passes)

  add_compile_definitions(CULEBRA_JIT_ENABLED)
endif()

add_executable(culebra ./src/main.cc)

if(CULEBRA_ENABLE_JIT)
  target_link_libraries(culebra ${llvm_libs})
endif()
