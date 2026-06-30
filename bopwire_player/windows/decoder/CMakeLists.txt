cmake_minimum_required(VERSION 3.14)
project(mc_decoder LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(mc_decoder SHARED mc_decoder.cpp)
target_compile_definitions(mc_decoder PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  UNICODE _UNICODE)
target_link_libraries(mc_decoder PRIVATE
  mfplat mfreadwrite mfuuid propsys ole32)

set_target_properties(mc_decoder PROPERTIES
  OUTPUT_NAME mc_decoder
  PREFIX ""
  WINDOWS_EXPORT_ALL_SYMBOLS OFF
)
