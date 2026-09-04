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
  src/csr.cpp
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
  test_csr
  test_muldiv_atomic
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
  set(SELFTEST_LD ${CMAKE_CURRENT_SOURCE_DIR}/examples/link.ld)

  # Each entry is  <name>:<march>:<expected a0 in hex>.
  # a0 accumulates one bit per passing sub-test, so a clear bit in a failure
  # message names exactly which check did not pass.
  set(SELFTESTS
    "rv64i_selftest:rv64i:3fff"
    "trap_selftest:rv64i_zicsr:fff"
    "muldiv_atomic_selftest:rv64ima_zicsr:7fff"
  )

  set(SELFTEST_OUTPUTS "")
  foreach(entry IN LISTS SELFTESTS)
    string(REPLACE ":" ";" parts "${entry}")
    list(GET parts 0 st_name)
    list(GET parts 1 st_arch)
    list(GET parts 2 st_expect)

    set(st_src ${CMAKE_CURRENT_SOURCE_DIR}/examples/${st_name}.S)
    set(st_bin ${CMAKE_CURRENT_BINARY_DIR}/${st_name}.bin)

    add_custom_command(
      OUTPUT ${st_bin}
      COMMAND ${RISCV_AS} -march=${st_arch} -mabi=lp64 -o ${st_name}.o ${st_src}
      COMMAND ${RISCV_LD} --no-warn-rwx-segments -T ${SELFTEST_LD} -o ${st_name}.elf ${st_name}.o
      COMMAND ${RISCV_OBJCOPY} -O binary ${st_name}.elf ${st_bin}
      DEPENDS ${st_src} ${SELFTEST_LD}
      WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
      COMMENT "Assembling ${st_name}"
    )
    list(APPEND SELFTEST_OUTPUTS ${st_bin})

    add_test(NAME ${st_name}
      COMMAND ${CMAKE_COMMAND}
        -DEMU=$<TARGET_FILE:riscv_emu>
        -DBIN=${st_bin}
        -DEXPECT=${st_expect}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_selftest.cmake
    )
  endforeach()

  add_custom_target(selftest_bins ALL DEPENDS ${SELFTEST_OUTPUTS})
else()
  message(STATUS "RISC-V toolchain not found; skipping the bare-metal self-test")
endif()
