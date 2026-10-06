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
#include "PendingMidiAcks.hpp"
#include "PedalboardBuilder.hpp"
#include "RingBuffer.hpp"
#include "AlsaDriverRealtime.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

using namespace pipedal;
using namespace std::chrono_literals;
using Kind = PendingMidiAckTracker::Kind;

TEST_CASE("Pending MIDI acks: no build outstanding -> immediate ack", "[final_review_fixes]")
{
    PendingMidiAckTracker tracker;
    // snapshot fast path, request error, or no-op: nothing was submitted.
    REQUIRE(tracker.Defer(Kind::Snapshot, 7, false, 3));
    REQUIRE(tracker.Defer(Kind::Program, 8, false, 3));
    REQUIRE(tracker.Empty());
}

TEST_CASE("Pending MIDI acks: ack only after the build is installed", "[final_review_fixes]")
{
    PendingMidiAckTracker tracker;
    REQUIRE_FALSE(tracker.Defer(Kind::Program, 1, true, 5));
    REQUIRE(tracker.Size() == 1);

    // a stale (borrowed) build of an older generation is installed while generation 5 is still pending:
    // the ack keeps waiting.
    REQUIRE(tracker.OnInstalled(4, false).empty());
    REQUIRE(tracker.Size() == 1);

    // generation 5 is installed.
    auto acks = tracker.OnInstalled(5, false);
    REQUIRE(acks.size() == 1);
    CHECK(acks[0].kind == Kind::Program);
    CHECK(acks[0].requestId == 1);
    REQUIRE(tracker.Empty());
}

TEST_CASE("Pending MIDI acks: every terminal path releases the ack", "[final_review_fixes]")
{
    SECTION("stale discard with a newer build outstanding waits for that build")
    {
        PendingMidiAckTracker tracker;
        tracker.Defer(Kind::Program, 1, true, 2);
        REQUIRE(tracker.OnBuildTerminated(false).empty()); // generation 3 still outstanding.
        auto acks = tracker.OnInstalled(3, true);
        REQUIRE(acks.size() == 1);
        CHECK(acks[0].requestId == 1);
    }
    SECTION("stale discard leaving the builder idle")
    {
        PendingMidiAckTracker tracker;
        tracker.Defer(Kind::Snapshot, 4, true, 2);
        auto acks = tracker.OnBuildTerminated(true);
        REQUIRE(acks.size() == 1);
        CHECK(acks[0].kind == Kind::Snapshot);
        REQUIRE(tracker.Empty());
    }
    SECTION("installing an older generation while idle releases everything")
    {
        PendingMidiAckTracker tracker;
        tracker.Defer(Kind::Program, 1, true, 9);
        REQUIRE(tracker.OnInstalled(8, true).size() == 1);
    }
    SECTION("audio restart / close")
    {
        PendingMidiAckTracker tracker;
        tracker.Defer(Kind::Program, 1, true, 2);
        tracker.Defer(Kind::Snapshot, 2, true, 3);
        REQUIRE(tracker.TakeAll().size() == 2);
        REQUIRE(tracker.Empty());
    }
}

namespace
{
    // Mirrors PiPedalModel's use of the tracker with a real LatestOnlyBuilder: the "ring" records what the
    // host writes to the realtime thread, in order.
    class FakeModel
    {
    public:
        std::recursive_mutex mutex;
        std::vector<std::string> ring;
        PendingMidiAckTracker acks;
        std::mutex gateMutex;
        std::condition_variable gateCv;
        bool gateOpen = true;
        bool failBuild = false;
        bool rejectInstall = false;

        LatestOnlyBuilder<int, int> builder{
            [this](int &request)
            {
                std::unique_lock<std::mutex> lock(gateMutex);
                gateCv.wait(lock, [this]() { return gateOpen; });
                if (failBuild)
                {
                    throw std::runtime_error("build failed");
                }
                return request;
            },
            [this](uint64_t generation, int &request, int &)
            {
                std::lock_guard<std::recursive_mutex> lock(mutex);
                bool installed = !rejectInstall && builder.IsCurrent(generation);
                if (installed)
                {
                    ring.push_back("replace:" + std::to_string(request));
                }
                builder.InstallCompleted();
                bool idle = builder.IsIdle();
                Send(installed ? acks.OnInstalled(generation, idle) : acks.OnBuildTerminated(idle));
            },
            [this](const std::exception &)
            {
                std::lock_guard<std::recursive_mutex> lock(mutex);
                builder.InstallCompleted();
                Send(acks.OnBuildTerminated(builder.IsIdle()));
            }};

        void Send(const std::vector<PendingMidiAckTracker::Ack> &list)
        {
            for (auto &ack : list)
            {
                ring.push_back("ack:" + std::to_string(ack.requestId));
            }
        }
        void SetGate(bool open)
        {
            std::lock_guard<std::mutex> lock(gateMutex);
            gateOpen = open;
            gateCv.notify_all();
        }
        // A MIDI request; submitBuild = false models the snapshot fast path / an error.
        void MidiRequest(int64_t requestId, bool submitBuild, int pedalboard)
        {
            std::lock_guard<std::recursive_mutex> lock(mutex);
            if (submitBuild)
            {
                builder.Submit(int(pedalboard));
            }
            if (acks.Defer(Kind::Program, requestId, !builder.IsIdle(), builder.CurrentGeneration()))
            {
                Send({PendingMidiAckTracker::Ack{Kind::Program, requestId, 0}});
            }
        }
        std::vector<std::string> Ring()
        {
            std::lock_guard<std::recursive_mutex> lock(mutex);
            return ring;
        }
    };
}

TEST_CASE("Pending MIDI acks: ack follows the new pedalboard in ring order", "[final_review_fixes]")
{
    FakeModel model;
    model.SetGate(false);
    model.MidiRequest(1, true, 100);
    std::this_thread::sleep_for(20ms);
    REQUIRE(model.Ring().empty()); // no ack while the build is outstanding.
    model.SetGate(true);
    REQUIRE(model.builder.WaitForIdle(5s));
    REQUIRE(model.Ring() == std::vector<std::string>{"replace:100", "ack:1"});
    model.builder.Close();
}

TEST_CASE("Pending MIDI acks: no build -> immediate ack", "[final_review_fixes]")
{
    FakeModel model;
    model.MidiRequest(1, false, 0);
    REQUIRE(model.Ring() == std::vector<std::string>{"ack:1"});
    model.builder.Close();
}

TEST_CASE("Pending MIDI acks: build failure and rejected install still ack", "[final_review_fixes]")
{
    {
        FakeModel model;
        model.failBuild = true;
        model.MidiRequest(1, true, 100);
        REQUIRE(model.builder.WaitForIdle(5s));
        REQUIRE(model.Ring() == std::vector<std::string>{"ack:1"});
        model.builder.Close();
    }
    {
        FakeModel model;
        model.rejectInstall = true; // e.g. audio epoch / configuration mismatch.
        model.MidiRequest(2, true, 100);
        REQUIRE(model.builder.WaitForIdle(5s));
        REQUIRE(model.Ring() == std::vector<std::string>{"ack:2"});
        model.builder.Close();
    }
}

TEST_CASE("Pending MIDI acks: superseded build acks after the newest install", "[final_review_fixes]")
{
    FakeModel model;
    model.SetGate(false);
    model.MidiRequest(1, true, 100);
    std::this_thread::sleep_for(20ms); // let the first build start (blocked on the gate).
    model.MidiRequest(2, true, 200);   // supersedes it.
    model.SetGate(true);
    REQUIRE(model.builder.WaitForIdle(5s));
    auto ring = model.Ring();
    // the stale build is discarded; both acks follow the install of the newest pedalboard.
    REQUIRE(ring.size() == 3);
    CHECK(ring[0] == "replace:200");
    CHECK(ring[1] == "ack:1");
    CHECK(ring[2] == "ack:2");
    model.builder.Close();
}

TEST_CASE("LatestOnlyBuilder reports non-std exceptions", "[final_review_fixes]")
{
    std::mutex m;
    std::condition_variable cv;
    bool reported = false;
    LatestOnlyBuilder<int, int> builder(
        [](int &) -> int
        { throw 42; },
        [](uint64_t, int &, int &) {},
        [&](const std::exception &)
        {
            std::lock_guard<std::mutex> lock(m);
            reported = true;
            cv.notify_all();
        });
    builder.Submit(1);
    std::unique_lock<std::mutex> lock(m);
    REQUIRE(cv.wait_for(lock, 5s, [&]() { return reported; }));
    lock.unlock();
    builder.Close();
}

TEST_CASE("Xrun counter counts audio-thread restarts", "[final_review_fixes]")
{
    RealtimeXrunCounter counter;
    counter.OnAudioThreadRestart();
    XrunCounts counts = counter.Take();
    REQUIRE(counts.Any());
    REQUIRE(counts.audioThreadRestarts == 1);
    REQUIRE_FALSE(counter.Take().Any());
}

TEST_CASE("RingBuffer accepts empty segments with null data", "[final_review_fixes]")
{
    RingBuffer<false, false> ring(64, false);
    uint32_t value = 0x12345678;
    REQUIRE(ring.writeSegments({{0, nullptr}, {sizeof(value), &value}, {0, nullptr}}));
    uint32_t out = 0;
    REQUIRE(ring.read(sizeof(out), (uint8_t *)&out));
    REQUIRE(out == value);
}
