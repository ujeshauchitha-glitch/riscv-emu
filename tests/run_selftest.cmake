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

# A program stops either on its EBREAK (reported as a trap) or by writing the
# poweroff word to syscon. Anything else - a step-budget exhaustion, an
# unexpected fault - means it did not finish.
if(NOT all MATCHES "breakpoint" AND NOT all MATCHES "powered off")
  message(FATAL_ERROR "program did not stop cleanly.\n${all}")
endif()

# Optional: check what the guest printed to the console.
if(DEFINED EXPECT_OUTPUT AND NOT EXPECT_OUTPUT STREQUAL "")
  if(NOT out MATCHES "${EXPECT_OUTPUT}")
    message(FATAL_ERROR
      "expected console output matching \"${EXPECT_OUTPUT}\".\n${all}")
  endif()
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
