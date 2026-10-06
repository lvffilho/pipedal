#include "pch.h"
#include "catch.hpp"
#include "DeferredMidi.hpp"
#include "ZeroInputMix.hpp"
#include <vector>

using namespace pipedal;
using R = DeferredMidiReplayResult;

TEST_CASE("deferred midi round trip", "[deferred_midi]")
{
    DeferredMidiBuffer<64> b;
    uint8_t m1[] = {0xB0, 7, 100};
    uint8_t m2[] = {0xC0, 5};
    uint8_t m3[] = {0x90, 60, 127};
    REQUIRE(b.Push(m1, 3));
    REQUIRE(b.Push(m2, 2));
    REQUIRE(b.Push(m3, 3));
    std::vector<std::vector<uint8_t>> got;
    b.Replay([&](const uint8_t *p, size_t n)
             { got.emplace_back(p, p + n); return R::Continue; });
    REQUIRE(got.size() == 3);
    CHECK(got[0] == std::vector<uint8_t>{0xB0, 7, 100});
    CHECK(got[1] == std::vector<uint8_t>{0xC0, 5});
    CHECK(got[2] == std::vector<uint8_t>{0x90, 60, 127});
    CHECK(b.count == 0);
}

TEST_CASE("deferred midi snapshot interruption keeps tail", "[deferred_midi]")
{
    DeferredMidiBuffer<64> b;
    uint8_t m1[] = {0xB0, 1, 2};
    uint8_t m2[] = {0xB0, 3, 4};
    uint8_t m3[] = {0xC0, 9};
    b.Push(m1, 3);
    b.Push(m2, 3);
    b.Push(m3, 2);
    int calls = 0;
    b.Replay([&](const uint8_t *, size_t)
             { return ++calls == 1 ? R::SnapshotPending : R::Continue; });
    CHECK(calls == 1);
    CHECK(b.count == 4 + 3);
    std::vector<std::vector<uint8_t>> got;
    b.Replay([&](const uint8_t *p, size_t n)
             { got.emplace_back(p, p + n); return R::Continue; });
    REQUIRE(got.size() == 2);
    CHECK(got[0] == std::vector<uint8_t>{0xB0, 3, 4});
    CHECK(got[1] == std::vector<uint8_t>{0xC0, 9});
    CHECK(b.count == 0);
}

TEST_CASE("deferred midi program change stops replay", "[deferred_midi]")
{
    DeferredMidiBuffer<64> b;
    uint8_t m1[] = {0xB0, 1, 2};
    b.Push(m1, 3);
    b.Push(m1, 3);
    int calls = 0;
    b.Replay([&](const uint8_t *, size_t)
             { ++calls; return R::ProgramChangePending; });
    CHECK(calls == 1);
    CHECK(b.count != 0);
}

TEST_CASE("deferred midi rejects oversize", "[deferred_midi]")
{
    DeferredMidiBuffer<8> b;
    uint8_t m[] = {1, 2, 3};
    CHECK(b.Push(m, 3));
    CHECK(b.Push(m, 3)); // exact fit: 4 + 4 == 8
    CHECK_FALSE(b.Push(m, 1));
    CHECK_FALSE(b.Push(m, 0));
}

TEST_CASE("deferred midi size limit is 127 bytes", "[deferred_midi]")
{
    DeferredMidiBuffer<512> b;
    std::vector<uint8_t> big(200, 0x42);
    CHECK(b.Push(big.data(), 127));
    CHECK(b.count == 128);
    CHECK_FALSE(b.Push(big.data(), 128));
    CHECK_FALSE(b.Push(big.data(), 200));
    CHECK(b.count == 128); // rejected pushes leave the buffer untouched
}

TEST_CASE("deferred midi capacity safety contract", "[deferred_midi]")
{
    // Storage is never overrun and accepted entries survive intact.
    constexpr size_t B = 32;
    DeferredMidiBuffer<B> b;
    std::vector<uint8_t> accepted;
    for (uint8_t i = 0; i < 255; ++i)
    {
        uint8_t m = i;
        if (!b.Push(&m, 1))
            break;
        accepted.push_back(m);
    }
    REQUIRE(accepted.size() > 0);
    REQUIRE(accepted.size() < 255); // a rejection happened
    CHECK(b.count <= B);
    CHECK(b.count == accepted.size() * 2);

    // Clearly over remaining capacity is rejected and changes nothing.
    size_t before = b.count;
    uint8_t big[B] = {};
    CHECK_FALSE(b.Push(big, B));
    CHECK(b.count == before);

    std::vector<uint8_t> got;
    b.Replay([&](const uint8_t *p, size_t n)
             { REQUIRE(n == 1); got.push_back(p[0]); return R::Continue; });
    CHECK(got == accepted);

    // Exact fit: an entry of size+1 == remaining bytes is accepted, and one
    // byte more is rejected.
    DeferredMidiBuffer<B> c;
    REQUIRE(c.Push(big, 5));
    size_t remaining = B - c.count;
    CHECK_FALSE(c.Push(big, remaining)); // needs remaining+1 bytes
    CHECK(c.Push(big, remaining - 1));
    CHECK(c.count == B);
    CHECK_FALSE(c.Push(big, 1));
}

TEST_CASE("deferred midi program change replay with Clear in callback", "[deferred_midi]")
{
    DeferredMidiBuffer<64> b;
    uint8_t pc[] = {0xC0, 5};
    uint8_t cc[] = {0xB0, 7, 100};
    b.Push(pc, 2);
    b.Push(cc, 3);
    int calls = 0;
    b.Replay([&](const uint8_t *, size_t)
             { ++calls; b.Clear(); return R::ProgramChangePending; });
    CHECK(calls == 1);
    CHECK(b.count == 0);

    // Buffer is reusable after the callback cleared it.
    REQUIRE(b.Push(cc, 3));
    std::vector<std::vector<uint8_t>> got;
    b.Replay([&](const uint8_t *p, size_t n)
             { got.emplace_back(p, p + n); return R::Continue; });
    REQUIRE(got.size() == 1);
    CHECK(got[0] == std::vector<uint8_t>{0xB0, 7, 100});

    // Clear() with Continue also ends replay without visiting the tail.
    b.Push(pc, 2);
    b.Push(cc, 3);
    calls = 0;
    b.Replay([&](const uint8_t *, size_t)
             { ++calls; b.Clear(); return R::Continue; });
    CHECK(calls == 1);
    CHECK(b.count == 0);
}

TEST_CASE("zero input mix levels", "[zero_input_mix]")
{
    auto a = ComputeZeroInputMixLevels(0.0f);
    CHECK(a.pluginLevel == 0.0f);
    CHECK(a.inputLevel == 1.0f);
    auto b = ComputeZeroInputMixLevels(0.5f);
    CHECK(b.pluginLevel == 1.0f);
    CHECK(b.inputLevel == 1.0f);
    auto c = ComputeZeroInputMixLevels(1.0f);
    CHECK(c.pluginLevel == 1.0f);
    CHECK(c.inputLevel == 0.0f);
}
