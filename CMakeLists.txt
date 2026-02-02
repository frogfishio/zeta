cmake_minimum_required(VERSION 3.20)

project(sir LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

enable_testing()

add_subdirectory(src/sircc)

option(SIR_ENABLE_SIRC "Build the experimental .sir text parser (flex/bison)" OFF)
if(SIR_ENABLE_SIRC)
  add_subdirectory(src/sirc)
endif()
