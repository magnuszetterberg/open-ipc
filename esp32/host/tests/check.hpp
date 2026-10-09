// Minimal test helpers: no framework, so the host tests build with nothing but a compiler.
#pragma once
#include <cstdio>
#include <cstdlib>

static int check_failures = 0;

// Report a failed condition and keep going, so one run shows every failure.
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            check_failures++;                                                          \
        }                                                                              \
    } while (0)

static inline int check_result(const char *name)
{
    std::printf("%s: %s\n", name, check_failures ? "FAILED" : "ok");
    return check_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
