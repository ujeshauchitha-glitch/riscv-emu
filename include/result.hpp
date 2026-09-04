#pragma once

#include "trap.hpp"

// ---------------------------------------------------------------------------
// Result / Status: how we report traps.
//
// Design note. Almost every memory access and every instruction can fail, and
// "failure" means "the guest takes a trap" — which in a running OS is completely
// routine. Page faults are how demand paging works; ECALL is how every system
// call is made. These are *not* exceptional conditions, they are the normal
// control flow of an operating system, and they happen constantly.
//
// So we do not use C++ exceptions for them. Instead every fallible operation
// returns a value that the caller must inspect:
//
//   Result<u64>  - a load: either a value, or a Trap
//   Status       - a store or an instruction: either OK, or a Trap
//
// This is the same shape as Rust's Result<T, E>, which is what most RISC-V
// emulators use. It keeps the trap path visible in the type signature and
// avoids the cost of unwinding on what is effectively a hot path.
//
// (std::expected would be the natural fit but it is C++23; we are on C++20, so
// this is a small hand-rolled stand-in.)
// ---------------------------------------------------------------------------

template <typename T>
struct Result {
    bool ok    = true;
    T    value = T{};
    Trap trap  = {};

    static Result good(T v) {
        Result r;
        r.ok    = true;
        r.value = v;
        return r;
    }

    static Result bad(Trap t) {
        Result r;
        r.ok   = false;
        r.trap = t;
        return r;
    }

    static Result bad(Exception cause, u64 tval = 0) {
        return bad(Trap{cause, tval});
    }

    explicit operator bool() const { return ok; }
};

// The void-returning counterpart, for operations that either succeed or trap.
struct Status {
    bool ok   = true;
    Trap trap = {};

    static Status good() { return Status{}; }

    static Status bad(Trap t) {
        Status s;
        s.ok   = false;
        s.trap = t;
        return s;
    }

    static Status bad(Exception cause, u64 tval = 0) {
        return bad(Trap{cause, tval});
    }

    explicit operator bool() const { return ok; }
};
