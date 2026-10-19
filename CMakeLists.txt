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

# -Wnon-virtual-dtor and -Woverloaded-virtual matter here because Device is a
# polymorphic base that every peripheral derives from; -Wshadow catches the
# classic emulator bug of a local quietly hiding a member with the same name.
add_compile_options(
  -Wall
  -Wextra
  -Wshadow
  -Wnon-virtual-dtor
  -Woverloaded-virtual
  -Wnull-dereference
)

# On MinGW, link the C++ runtime statically.
#
# Otherwise the executables load libstdc++-6.dll and libgcc_s_seh-1.dll from
# whatever directory happens to be first on PATH, which on a machine with more
# than one MinGW/MSYS2 installed is routinely a *different version* from the
# compiler that built them. The failure is a program that exits instantly with
# 0xc0000139 (entry point not found) and prints nothing - or, under a test
# runner, appears to hang on the loader's error dialog. A static runtime also
# means riscv_emu.exe runs on a machine with no MinGW at all.
if(MINGW)
  add_link_options(-static)
endif()

# The emulator core, built as a library so that both the executable and the
# tests can link against exactly the same code.
add_library(riscv_core STATIC
  src/bus.cpp
  src/cpu.cpp
  src/cpu_bitmanip.cpp
  src/clint.cpp
  src/csr.cpp
  src/elf_loader.cpp
  src/fpu.cpp
  src/fdt.cpp
  src/sbi.cpp
  src/mmu.cpp
  src/plic.cpp
  src/platform.cpp
  src/decoder.cpp
  src/dram.cpp
  src/syscon.cpp
  src/uart.cpp
  src/virtio_mmio.cpp
  src/virtio.cpp
  src/virtio_net.cpp
  src/rtc.cpp
  src/hostnet.cpp
  src/usernet.cpp
  src/gdbstub.cpp
  src/trap.cpp
)
target_include_directories(riscv_core PUBLIC include)

# Sockets live in a separate library on Windows.
if(WIN32)
  target_link_libraries(riscv_core PUBLIC ws2_32 iphlpapi)
endif()

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
  test_devices
  test_supervisor
  test_interrupt_devices
  test_compressed
  test_float
  test_firmware
  test_backends
  test_smp
  test_virtio
  test_usernet
  test_gdbstub
)

# The bit-manipulation extensions are checked against an independent model
# (tests/gen_bitmanip_vectors.py): tens of thousands of random and edge-case
# vectors, generated at build time. The hand-written cases in the same program
# still run without Python.
set(BITMANIP_VECTORS ${CMAKE_BINARY_DIR}/bitmanip_vectors.txt)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
  add_custom_command(
    OUTPUT ${BITMANIP_VECTORS}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/gen_bitmanip_vectors.py ${BITMANIP_VECTORS}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/gen_bitmanip_vectors.py
    COMMENT "Generating bit-manipulation test vectors")
  add_custom_target(bitmanip_vectors ALL DEPENDS ${BITMANIP_VECTORS})
  set(BITMANIP_ARGS ${BITMANIP_VECTORS})
else()
  message(STATUS "Python 3 not found; the bit-manipulation differential test will be skipped")
  set(BITMANIP_ARGS "")
endif()
add_executable(test_bitmanip tests/test_bitmanip.cpp)
target_link_libraries(test_bitmanip PRIVATE riscv_core)
target_include_directories(test_bitmanip PRIVATE tests)
add_test(NAME test_bitmanip COMMAND test_bitmanip ${BITMANIP_ARGS})
if(Python3_FOUND)
  add_dependencies(test_bitmanip bitmanip_vectors)
endif()

# A throughput benchmark, built but deliberately not registered as a test: it
# measures speed, it does not check correctness, and a timing threshold in CI
# would be flaky. Run ./bench_cpu by hand around a change to the execution loop.
add_executable(bench_cpu tests/bench_cpu.cpp)
target_link_libraries(bench_cpu PRIVATE riscv_core)
target_include_directories(bench_cpu PRIVATE tests)

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
# Install with:
#   Debian/Ubuntu  apt install gcc-riscv64-unknown-elf
#   Fedora         dnf install gcc-riscv64-elf binutils-riscv64-elf
#   Arch           pacman -S riscv64-elf-gcc riscv64-elf-binutils
# ---------------------------------------------------------------------------
# Toolchain prefixes differ by distribution: Debian and Ubuntu ship
# riscv64-unknown-elf-, Fedora ships riscv64-elf-, and a Linux-target
# cross-compiler (riscv64-linux-gnu-) assembles bare-metal code perfectly well
# too. All three are accepted.
find_program(RISCV_AS      NAMES riscv64-unknown-elf-as riscv64-elf-as riscv-none-elf-as riscv64-linux-gnu-as)
find_program(RISCV_LD      NAMES riscv64-unknown-elf-ld riscv64-elf-ld riscv-none-elf-ld riscv64-linux-gnu-ld)
find_program(RISCV_OBJCOPY NAMES riscv64-unknown-elf-objcopy riscv64-elf-objcopy riscv-none-elf-objcopy riscv64-linux-gnu-objcopy)

if(RISCV_AS AND RISCV_LD AND RISCV_OBJCOPY)
  set(SELFTEST_LD ${CMAKE_CURRENT_SOURCE_DIR}/examples/link.ld)

  # Each entry is  <name>:<march>:<expected a0 in hex>[:<expected console text>].
  # a0 accumulates one bit per passing sub-test, so a clear bit in a failure
  # message names exactly which check did not pass.
  set(SELFTESTS
    "rv64i_selftest:rv64i:3fff"
    "trap_selftest:rv64i_zicsr:fff"
    "muldiv_atomic_selftest:rv64ima_zicsr:7fff"
    "device_selftest:rv64ima_zicsr:1f:uart works"
  )

  set(SELFTEST_OUTPUTS "")
  foreach(entry IN LISTS SELFTESTS)
    string(REPLACE ":" ";" parts "${entry}")
    list(GET parts 0 st_name)
    list(GET parts 1 st_arch)
    list(GET parts 2 st_expect)
    list(LENGTH parts st_nparts)
    set(st_output "")
    if(st_nparts GREATER 3)
      list(GET parts 3 st_output)
    endif()

    set(st_src ${CMAKE_CURRENT_SOURCE_DIR}/examples/${st_name}.S)
    set(st_bin ${CMAKE_CURRENT_BINARY_DIR}/${st_name}.bin)

    add_custom_command(
      OUTPUT ${st_bin}
      COMMAND ${RISCV_AS} -march=${st_arch} -mabi=lp64 -o ${st_name}.o ${st_src}
      COMMAND ${RISCV_LD} -m elf64lriscv --no-warn-rwx-segments -T ${SELFTEST_LD} -o ${st_name}.elf ${st_name}.o
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
        -DEXPECT_OUTPUT=${st_output}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_selftest.cmake
    )
  endforeach()

  add_custom_target(selftest_bins ALL DEPENDS ${SELFTEST_OUTPUTS})
else()
  message(STATUS "RISC-V toolchain not found; skipping the bare-metal self-test")
endif()
