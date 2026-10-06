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

// Tests for the lock-free parameter transfer used by the VST3 host
// (Vst3EffectImpl) to keep IEditController calls off the audio thread. The
// header has no VST3 SDK dependency, so these run with ENABLE_VST3=0.

#include "pch.h"
#include "catch.hpp"
#include "vst3/Vst3RtParameterSlots.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace pipedal;

// RtParameterSlots<double> does not compile on targets where
// std::atomic<double> is not lock-free (e.g. armv6). The tests that need it
// live in a partial specialization: only the one selected below has its
// member functions instantiated, so on such targets the double tests compile
// away and report themselves as skipped.
template <bool SUPPORTED, typename D = double>
struct DoubleSlotTests
{
    static void LatestValueOnce() { WARN("Skipped: std::atomic<double> is not lock-free on this target."); }
    static void TwoStagePipeline() { WARN("Skipped: std::atomic<double> is not lock-free on this target."); }
};

template <typename D>
struct DoubleSlotTests<true, D>
{
    static void LatestValueOnce()
    {
        RtParameterSlots<D> slots(4);
        REQUIRE(slots.Size() == 4);
        REQUIRE(!slots.HasPending());

        slots.Set(1, 0.25);
        slots.Set(1, 0.75); // coalesced: last writer wins.
        slots.Set(3, 1.0);
        REQUIRE(slots.HasPending());

        std::vector<std::pair<size_t, double>> seen;
        size_t n = slots.ForEachPending([&](size_t i, double v) { seen.push_back({i, v}); });
        REQUIRE(n == 2);
        REQUIRE(seen.size() == 2);
        REQUIRE(seen[0].first == 1);
        REQUIRE(seen[0].second == 0.75);
        REQUIRE(seen[1].first == 3);
        REQUIRE(seen[1].second == 1.0);

        // Nothing left after a drain.
        REQUIRE(!slots.HasPending());
        REQUIRE(slots.ForEachPending([](size_t, double) {}) == 0);

        double v = -1;
        REQUIRE(!slots.Take(1, &v));
        REQUIRE(v == -1);
    }

    // Mirrors the Vst3EffectImpl pipeline: an "audio" thread produces plain values
    // and wakes a worker; the worker "normalizes" and publishes to a second slot
    // set that the audio thread drains. The final value of every parameter must
    // arrive, and the worker must not miss a wake-up.
    static void TwoStagePipeline()
    {
        constexpr size_t N_PARAMS = 16;
        constexpr int N_ITERATIONS = 20000;

        RtParameterSlots<float> plain(N_PARAMS);
        RtParameterSlots<D> normalized(N_PARAMS);
        RtWakeSemaphore wake;
        std::atomic<bool> stop{false};

        std::thread worker([&]() {
            while (true)
            {
                wake.Wait();
                if (stop.load())
                    break;
                plain.ForEachPending([&](size_t i, float v) {
                    normalized.Set(i, v / 1000.0);
                });
            }
        });

        std::vector<double> received(N_PARAMS, -1);
        std::thread audio([&]() {
            for (int iteration = 0; iteration <= N_ITERATIONS; ++iteration)
            {
                plain.Set(iteration % N_PARAMS, (float)iteration);
                wake.Post();
                normalized.ForEachPending([&](size_t i, double v) { received[i] = v; });
            }
        });
        audio.join();

        // Keep draining (as later audio blocks would) until the final values land.
        std::vector<double> expected(N_PARAMS);
        for (size_t i = 0; i < N_PARAMS; ++i)
        {
            int last = N_ITERATIONS - (int)((N_ITERATIONS - i) % N_PARAMS);
            expected[i] = last / 1000.0;
        }
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (received != expected && std::chrono::steady_clock::now() < deadline)
        {
            normalized.ForEachPending([&](size_t i, double v) { received[i] = v; });
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        stop.store(true);
        wake.Post();
        worker.join();

        REQUIRE(received == expected);
    }
};

using DoubleTests = DoubleSlotTests<RtParameterSlotsSupported<double>>;

TEST_CASE("RtParameterSlots delivers latest value once", "[vst3_rt]")
{
    DoubleTests::LatestValueOnce();
}

TEST_CASE("RtParameterSlots ignores out of range indices", "[vst3_rt]")
{
    RtParameterSlots<float> slots(2);
    slots.Set(2, 1.0f);
    slots.Set(1000, 1.0f);
    REQUIRE(!slots.HasPending());
    float v;
    REQUIRE(!slots.Take(2, &v));

    RtParameterSlots<float> empty;
    empty.Set(0, 1.0f);
    REQUIRE(empty.ForEachPending([](size_t, float) {}) == 0);
}

TEST_CASE("RtParameterSlots Clear discards pending values", "[vst3_rt]")
{
    RtParameterSlots<float> slots(3);
    slots.Set(0, 1.0f);
    slots.Set(2, 2.0f);
    slots.Clear();
    REQUIRE(!slots.HasPending());
    REQUIRE(slots.ForEachPending([](size_t, float) {}) == 0);
    slots.Set(2, 3.0f);
    float v = 0;
    REQUIRE(slots.Take(2, &v));
    REQUIRE(v == 3.0f);
}

TEST_CASE("RtParameterSlots two-stage pipeline does not lose the final value", "[vst3_rt]")
{
    DoubleTests::TwoStagePipeline();
}

TEST_CASE("RtWakeSemaphore coalesces posts", "[vst3_rt]")
{
    RtWakeSemaphore wake;
    for (int i = 0; i < 100; ++i)
    {
        wake.Post();
    }
    wake.Wait(); // one outstanding wake-up...

    // ...and no more: a second Wait would block. Prove it by posting from
    // another thread after a delay and checking we did block until then.
    std::atomic<bool> posted{false};
    std::thread t([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        posted.store(true);
        wake.Post();
    });
    wake.Wait();
    REQUIRE(posted.load());
    t.join();
}

// Models a re-entrant restartComponent() from inside FlushControlChanges():
// the outer pass has claimed the summary flag, so a nested ForEachPending()
// sees nothing, but ForEachPendingScanAll() delivers the slots the outer pass
// hasn't reached yet. Every value is delivered exactly once.
TEST_CASE("RtParameterSlots nested scan delivers slots the outer pass has not reached", "[vst3_rt]")
{
    RtParameterSlots<float> slots(5);
    slots.Set(0, 10.0f);
    slots.Set(2, 12.0f);
    slots.Set(4, 14.0f);

    std::vector<std::pair<size_t, float>> outerSeen;
    std::vector<std::pair<size_t, float>> nestedSeen;
    size_t nestedPlainCount = 99;
    size_t outerCount = slots.ForEachPending([&](size_t i, float v) {
        outerSeen.push_back({i, v});
        if (i == 0)
        {
            nestedPlainCount = slots.ForEachPending([&](size_t, float) {});
            slots.ForEachPendingScanAll([&](size_t j, float w) { nestedSeen.push_back({j, w}); });
        }
    });

    REQUIRE(nestedPlainCount == 0); // the old behaviour: nested flush was a no-op.
    REQUIRE(outerCount == 1);
    REQUIRE(outerSeen == std::vector<std::pair<size_t, float>>{{0, 10.0f}});
    REQUIRE(nestedSeen == std::vector<std::pair<size_t, float>>{{2, 12.0f}, {4, 14.0f}});
    REQUIRE(!slots.HasPending());
    REQUIRE(slots.ForEachPending([](size_t, float) {}) == 0);
}

TEST_CASE("RtParameterSlots scan-all picks up values set during the nested scan", "[vst3_rt]")
{
    RtParameterSlots<float> slots(3);
    slots.Set(1, 1.0f);
    std::vector<size_t> seen;
    slots.ForEachPending([&](size_t i, float) {
        seen.push_back(i);
        // A producer racing with the nested consumer: not lost.
        slots.Set(2, 2.0f);
        REQUIRE(slots.ForEachPendingScanAll([&](size_t j, float) { seen.push_back(j); }) == 1);
        slots.Set(0, 3.0f);
    });
    REQUIRE(seen == std::vector<size_t>{1, 2});
    REQUIRE(slots.HasPending());
    float v = 0;
    REQUIRE(slots.ForEachPending([&](size_t i, float w) { seen.push_back(i); v = w; }) == 1);
    REQUIRE(v == 3.0f);
    REQUIRE(seen == std::vector<size_t>{1, 2, 0});
}
