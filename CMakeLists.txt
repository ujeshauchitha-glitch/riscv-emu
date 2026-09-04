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
  test_rv64i
)

foreach(test_name IN LISTS RISCV_EMU_TESTS)
  add_executable(${test_name} tests/${test_name}.cpp)
  target_link_libraries(${test_name} PRIVATE riscv_core)
  target_include_directories(${test_name} PRIVATE tests)
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# ---------------------------------------------------------------------------
# Bare-metal self-test.
#
# Assembled with the real GNU assembler when a RISC-V toolchain is available,
# which validates the decoder against an independent implementation rather than
# against the encoders in tests/machine.hpp. Skipped silently when the toolchain
# is absent, so the build never depends on it.
#
# Install with: apt-get install gcc-riscv64-unknown-elf
# ---------------------------------------------------------------------------
find_program(RISCV_AS      NAMES riscv64-unknown-elf-as riscv64-linux-gnu-as)
find_program(RISCV_LD      NAMES riscv64-unknown-elf-ld riscv64-linux-gnu-ld)
find_program(RISCV_OBJCOPY NAMES riscv64-unknown-elf-objcopy riscv64-linux-gnu-objcopy)

if(RISCV_AS AND RISCV_LD AND RISCV_OBJCOPY)
  set(SELFTEST_SRC ${CMAKE_CURRENT_SOURCE_DIR}/examples/rv64i_selftest.S)
  set(SELFTEST_LD  ${CMAKE_CURRENT_SOURCE_DIR}/examples/link.ld)
  set(SELFTEST_BIN ${CMAKE_CURRENT_BINARY_DIR}/rv64i_selftest.bin)

  add_custom_command(
    OUTPUT ${SELFTEST_BIN}
    COMMAND ${RISCV_AS} -march=rv64i -mabi=lp64 -o rv64i_selftest.o ${SELFTEST_SRC}
    COMMAND ${RISCV_LD} --no-warn-rwx-segments -T ${SELFTEST_LD} -o rv64i_selftest.elf rv64i_selftest.o
    COMMAND ${RISCV_OBJCOPY} -O binary rv64i_selftest.elf ${SELFTEST_BIN}
    DEPENDS ${SELFTEST_SRC} ${SELFTEST_LD}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Assembling RV64I self-test"
  )
  add_custom_target(selftest_bin ALL DEPENDS ${SELFTEST_BIN})

  # a0 accumulates one bit per sub-test; 0x3fff means all 14 passed.
  add_test(NAME rv64i_selftest
    COMMAND ${CMAKE_COMMAND}
      -DEMU=$<TARGET_FILE:riscv_emu>
      -DBIN=${SELFTEST_BIN}
      -DEXPECT=3fff
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_selftest.cmake
  )
else()
  message(STATUS "RISC-V toolchain not found; skipping the bare-metal self-test")
endif()
