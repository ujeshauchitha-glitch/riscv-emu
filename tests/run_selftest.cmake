# Runs a bare-metal self-test binary under the emulator and checks the value it
# leaves in a0. Invoked by CTest via `cmake -P`.
#
# Expects: EMU (emulator path), BIN (flat binary), EXPECT (hex value, no 0x).

execute_process(
  COMMAND "${EMU}" --dump "${BIN}"
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
  RESULT_VARIABLE rc
)

set(all "${out}${err}")

# The emulator stops on the program's EBREAK, which it reports as a trap, so a
# nonzero exit is expected here. What matters is the breakpoint cause and a0.
if(NOT all MATCHES "breakpoint")
  message(FATAL_ERROR "expected the program to stop on ebreak.\n${all}")
endif()

if(NOT all MATCHES "x10 \\(a0\\)[ \t]*: 0x0*([0-9a-f]+)")
  message(FATAL_ERROR "could not find a0 in the register dump.\n${all}")
endif()

set(got "${CMAKE_MATCH_1}")
if(NOT got STREQUAL "${EXPECT}")
  message(FATAL_ERROR
    "self-test failed: a0 = 0x${got}, expected 0x${EXPECT}.\n"
    "Each bit is one sub-test; a clear bit identifies which one failed.\n${all}")
endif()

message(STATUS "self-test passed: a0 = 0x${got}")
