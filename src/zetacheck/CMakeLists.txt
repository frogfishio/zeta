cmake_minimum_required(VERSION 3.20)

add_executable(zetacheck
  zetacheck.c
)

target_compile_definitions(zetacheck PRIVATE SIR_VERSION="${SIR_VERSION}")

target_compile_options(zetacheck PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -Werror
)

