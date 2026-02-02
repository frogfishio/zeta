cmake_minimum_required(VERSION 3.20)

find_package(BISON REQUIRED)
find_package(FLEX REQUIRED)

BISON_TARGET(SIR_PARSER ${CMAKE_CURRENT_LIST_DIR}/sir.y ${CMAKE_CURRENT_BINARY_DIR}/sir.tab.c
  DEFINES_FILE ${CMAKE_CURRENT_BINARY_DIR}/sir.tab.h
)
FLEX_TARGET(SIR_LEXER ${CMAKE_CURRENT_LIST_DIR}/sir.l ${CMAKE_CURRENT_BINARY_DIR}/sir.lex.c)
ADD_FLEX_BISON_DEPENDENCY(SIR_LEXER SIR_PARSER)

add_executable(sirc
  main.c
  ${BISON_SIR_PARSER_OUTPUTS}
  ${FLEX_SIR_LEXER_OUTPUTS}
)

target_include_directories(sirc PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

target_compile_options(sirc PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -Werror
)

