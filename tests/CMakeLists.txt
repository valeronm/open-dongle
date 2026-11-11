cmake_minimum_required(VERSION 3.20)
project(open_dongle_tests C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../src)
enable_testing()

function(suite name)
  add_executable(test_${name} test_${name}.c ${ARGN})
  add_test(NAME ${name} COMMAND test_${name})
endfunction()

suite(nuphy ../src/nuphy.c)
suite(reports ../src/reports.c ../src/nuphy.c)
suite(chanpick ../src/chanpick.c)
suite(noise ../src/noise.c)
suite(link ../src/link.c ../src/nuphy.c)
suite(diagproto ../src/diagproto.c)
target_compile_definitions(test_diagproto PRIVATE
  FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tool/fixtures")
suite(history ../src/history.c)
