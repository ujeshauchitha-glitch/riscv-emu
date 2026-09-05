#pragma once

#include <cstdio>
#include <cstdint>
#include <string>

// ---------------------------------------------------------------------------
// A deliberately tiny test harness.
//
// No external framework: the project has no dependencies and it is nice to keep
// it that way while the codebase is small. From phase 5 onward the real
// correctness signal comes from the official riscv-tests suite; these unit
// tests cover the plumbing that riscv-tests cannot reach (bus decoding, bounds
// checking) and give fast feedback while developing.
// ---------------------------------------------------------------------------

namespace testutil {

inline int g_failures = 0;
inline int g_checks   = 0;

inline void report_failure(const char* file, int line, const std::string& msg) {
    ++g_failures;
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, msg.c_str());
}

inline int summary(const char* suite) {
    if (g_failures == 0) {
        std::fprintf(stderr, "PASS %s (%d checks)\n", suite, g_checks);
        return 0;
    }
    std::fprintf(stderr, "FAIL %s (%d/%d checks failed)\n", suite, g_failures, g_checks);
    return 1;
}

inline std::string hex(std::uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(v));
    return buf;
}

}  // namespace testutil

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++testutil::g_checks;                                              \
        if (!(cond)) {                                                     \
            testutil::report_failure(__FILE__, __LINE__, "CHECK(" #cond ")"); \
        }                                                                  \
    } while (0)

// Compares as signed 64-bit so that negative immediates print readably.
#define CHECK_EQ(actual, expected)                                              \
    do {                                                                        \
        ++testutil::g_checks;                                                   \
        const long long a_ = static_cast<long long>(actual);                    \
        const long long e_ = static_cast<long long>(expected);                  \
        if (a_ != e_) {                                                         \
            char buf_[256];                                                     \
            std::snprintf(buf_, sizeof(buf_),                                   \
                          "%s == %s : got %lld (0x%llx), expected %lld (0x%llx)", \
                          #actual, #expected, a_,                               \
                          static_cast<unsigned long long>(a_), e_,              \
                          static_cast<unsigned long long>(e_));                 \
            testutil::report_failure(__FILE__, __LINE__, buf_);                 \
        }                                                                       \
    } while (0)

#define CHECK_EQ_U(actual, expected)                                            \
    do {                                                                        \
        ++testutil::g_checks;                                                   \
        const unsigned long long a_ = static_cast<unsigned long long>(actual);  \
        const unsigned long long e_ = static_cast<unsigned long long>(expected);\
        if (a_ != e_) {                                                         \
            char buf_[256];                                                     \
            std::snprintf(buf_, sizeof(buf_),                                   \
                          "%s == %s : got 0x%llx, expected 0x%llx",             \
                          #actual, #expected, a_, e_);                          \
            testutil::report_failure(__FILE__, __LINE__, buf_);                 \
        }                                                                       \
    } while (0)
