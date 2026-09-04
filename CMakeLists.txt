cmake_minimum_required(VERSION 3.16)
project(riscv_emu CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# An emulator's inner loop runs billions of times when booting an OS, so an
# unoptimised build is the difference between a boot that takes seconds and one
# that takes minutes. Default to Release when the user has not chosen.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

add_compile_options(-Wall -Wextra)

# The emulator core, built as a library so that both the executable and the
# tests can link against exactly the same code.
add_library(riscv_core STATIC
  src/bus.cpp
  src/cpu.cpp
  src/decoder.cpp
  src/dram.cpp
  src/trap.cpp
)
target_include_directories(riscv_core PUBLIC include)

add_executable(riscv_emu src/main.cpp)
target_link_libraries(riscv_emu PRIVATE riscv_core)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()

set(RISCV_EMU_TESTS
  test_decoder
  test_bus
  test_cpu
)

foreach(test_name IN LISTS RISCV_EMU_TESTS)
  add_executable(${test_name} tests/${test_name}.cpp)
  target_link_libraries(${test_name} PRIVATE riscv_core)
  target_include_directories(${test_name} PRIVATE tests)
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
