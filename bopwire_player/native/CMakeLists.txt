cmake_minimum_required(VERSION 3.16)
project(bopwire_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# librats as a SHARED library (Dart FFI requires a loadable .so/.dll).
set(RATS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(RATS_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(RATS_BINDINGS       ON  CACHE BOOL "" FORCE)
set(RATS_SHARED_LIBRARY ON  CACHE BOOL "" FORCE)
set(RATS_STATIC_LIBRARY OFF CACHE BOOL "" FORCE)
set(RATS_VERSION_OVERRIDE "1.0.0" CACHE STRING "" FORCE)

# Path to the librats source — we share the one already vendored under the
# main bopwire repo so we don't double-maintain the deps tree.
set(LIBRATS_SRC_DIR "${CMAKE_SOURCE_DIR}/../../bopwire/deps/librats"
    CACHE PATH "Path to librats source")

if(NOT EXISTS "${LIBRATS_SRC_DIR}/CMakeLists.txt")
  message(FATAL_ERROR
    "librats source not found at ${LIBRATS_SRC_DIR}. "
    "Pass -DLIBRATS_SRC_DIR=<absolute path> to override.")
endif()

add_subdirectory("${LIBRATS_SRC_DIR}" librats_build)
