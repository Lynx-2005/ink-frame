#pragma once
// Hard test checks: CMake defines NDEBUG in RelWithDebInfo/Release, which
// compiles out assert(). Our regression proofs must fail loudly in every
// build type, so tests use CHECK, never assert.
#include <cstdio>

#define INK_CHECK(c)                                                           \
    do {                                                                       \
        if (!(c)) {                                                            \
            std::printf("CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #c);    \
            return 1;                                                          \
        }                                                                      \
    } while (0)
