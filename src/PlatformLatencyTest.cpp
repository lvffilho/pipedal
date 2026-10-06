// Copyright (c) Robin E.R. Davies
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of
// this software and associated documentation files (the "Software"), to deal in
// the Software without restriction, including without limitation the rights to
// use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
// the Software, and to permit persons to whom the Software is furnished to do so,
// subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
// FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
// IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
// CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


#include "pch.h"
#include "catch.hpp"
#include "AudioSampleClamp.hpp"
#include "CpuGovernor.hpp"
#include "CpuDmaLatency.hpp"
#include <cmath>
#include <limits>
#include <unistd.h>
#include <cstdio>
#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

using namespace pipedal;

TEST_CASE("Governor list parsing", "[platform_latency]")
{
    auto v = ParseGovernorList("performance powersave\n");
    REQUIRE(v == std::vector<std::string>{"performance", "powersave"});

    v = ParseGovernorList("conservative ondemand userspace powersave performance schedutil ");
    REQUIRE(v.size() == 6);
    REQUIRE(v[5] == "schedutil");

    // reject odd tokens
    v = ParseGovernorList("performance ../evil Bad;x powersave aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    REQUIRE(v == std::vector<std::string>{"performance", "powersave"});

    REQUIRE(ParseGovernorList("").empty());
    REQUIRE(ParseGovernorList("  \n").empty());
}

TEST_CASE("Output sample sanitising survives fast-math", "[platform_latency]")
{
    volatile float nan = std::numeric_limits<float>::quiet_NaN();
    float inf = std::numeric_limits<float>::infinity();
    float vals[] = {nan, -nan, inf, -inf, 1e30f, -1e30f, 2.0f, -2.0f, 0.5f, -0.5f, 1.0f};
    float expected[] = {0, 0, 0, 0, 1, -1, 1, -1, 0.5f, -0.5f, 1.0f};
    for (size_t i = 0; i < sizeof(vals) / sizeof(vals[0]); ++i)
    {
        float c = ClampOutputSample(vals[i]);
        REQUIRE(c == expected[i]);
        // same conversions as the CopyPlayback* integer formats.
        int32_t i32 = (int32_t)((double)std::numeric_limits<int32_t>::max() * c);
        int16_t i16 = (int16_t)((float)std::numeric_limits<int16_t>::max() * c);
        int32_t i24 = (int32_t)(8388607.0 * c);
        if (expected[i] == 0)
        {
            REQUIRE(i32 == 0);
            REQUIRE(i16 == 0);
            REQUIRE(i24 == 0);
        }
        else
        {
            REQUIRE(i32 != std::numeric_limits<int32_t>::min());
            REQUIRE(i16 != std::numeric_limits<int16_t>::min());
        }
    }
    // float formats keep range but zero non-finite values.
    REQUIRE(SanitizeFloatOutputSample(nan) == 0.0f);
    REQUIRE(SanitizeFloatOutputSample(inf) == 0.0f);
    REQUIRE(SanitizeFloatOutputSample(-inf) == 0.0f);
    REQUIRE(SanitizeFloatOutputSample(3.0f) == 3.0f);
}

TEST_CASE("FTZ/DAZ can be enabled", "[platform_latency]")
{
    EnableFlushToZero();
#if defined(__x86_64__)
    REQUIRE((_mm_getcsr() & 0x8040) == 0x8040);
#endif
}

TEST_CASE("CpuDmaLatency tolerates unopenable path and holds/releases fd", "[platform_latency]")
{
    CpuDmaLatency h;
    h.Hold("/nonexistent/cpu_dma_latency");
    REQUIRE(!h.IsHeld());
    h.Release();

    char tmpl[] = "/tmp/cpudmaXXXXXX";
    int fd = mkstemp(tmpl);
    REQUIRE(fd != -1);
    close(fd);
    h.Hold(tmpl);
    REQUIRE(h.IsHeld());
    h.Release();
    REQUIRE(!h.IsHeld());
    unlink(tmpl);
}

TEST_CASE("Governor validation", "[platform_latency]")
{
    std::vector<std::string> available{"performance", "powersave"};
    REQUIRE(IsValidGovernor("performance", available));
    REQUIRE(!IsValidGovernor("ondemand", available));
    REQUIRE(!IsValidGovernor("", available));
    REQUIRE(!IsValidGovernor("performance\n", available));
    REQUIRE(!IsValidGovernor("performance", {}));
}

TEST_CASE("Persisted governor falls back to the running governor", "[platform_latency]")
{
    std::vector<std::string> available{"performance", "ondemand", "schedutil"};
    // Available: kept.
    REQUIRE(ResolvePersistedGovernor("ondemand", available, "schedutil", true) == "ondemand");
    // Not available (e.g. settings from another device): replaced by the running governor.
    REQUIRE(ResolvePersistedGovernor("powersave", available, "schedutil", true) == "schedutil");
    REQUIRE(ResolvePersistedGovernor("", available, "performance", true) == "performance");
    // No usable replacement: unchanged.
    REQUIRE(ResolvePersistedGovernor("powersave", available, "", true) == "powersave");
    REQUIRE(ResolvePersistedGovernor("powersave", available, "userspace", true) == "powersave");
    // No cpufreq at all: unchanged.
    REQUIRE(ResolvePersistedGovernor("powersave", {}, "", true) == "powersave");
    // List is the hard-coded fallback (sysfs list missing/empty): never repaired.
    REQUIRE(ResolvePersistedGovernor("schedutil", {"performance", "ondemand", "powersave"}, "performance", false) == "schedutil");
}
