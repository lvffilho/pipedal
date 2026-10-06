// Copyright (c) Robin E.R. Davies
// MIT license; see the license text at the top of other source files.

#pragma once

#include <bit>
#include <cstdint>
#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

namespace pipedal
{
    // True for NaN and +/-Inf. Uses the integer representation, so that it
    // survives -ffast-math (-ffinite-math-only), which may optimise std::isnan away.
    inline bool IsNonFiniteSample(float v)
    {
        return (std::bit_cast<uint32_t>(v) & 0x7FFFFFFFu) >= 0x7F800000u;
    }

    // NaN/Inf -> 0; otherwise clamped to [-1,1].
    inline float ClampOutputSample(float v)
    {
        if (IsNonFiniteSample(v))
            return 0.0f;
        if (v > 1.0f)
            return 1.0f;
        if (v < -1.0f)
            return -1.0f;
        return v;
    }

    // For float output formats: NaN/Inf -> 0, range preserved.
    inline float SanitizeFloatOutputSample(float v)
    {
        return IsNonFiniteSample(v) ? 0.0f : v;
    }

    // Set flush-to-zero / denormals-are-zero on the calling thread.
    inline void EnableFlushToZero()
    {
#if defined(__x86_64__) || defined(__i386__)
        _mm_setcsr(_mm_getcsr() | 0x8040);
#elif defined(__aarch64__)
        uint64_t fpcr;
        asm volatile("mrs %0, fpcr" : "=r"(fpcr));
        fpcr |= (1ULL << 24); // FZ
        asm volatile("msr fpcr, %0" : : "r"(fpcr));
#endif
    }
}
