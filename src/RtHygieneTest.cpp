// Copyright (c) Robin E. R. Davies
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
#include "AlsaDriverRealtime.hpp"
#include "AlsaSequencer.hpp"
#include "RingBuffer.hpp"
#include "RingBufferReader.hpp"
#include "PatchPropertyWriter.hpp"

#include <alsa/asoundlib.h>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace pipedal;

namespace
{
    // Stands in for AlsaSequencer: hands out queued messages, non-blocking only.
    class FakeMidiSource
    {
    public:
        std::deque<std::vector<uint8_t>> pending;
        int reads = 0;
        bool sawBlockingRead = false;

        void Push(std::vector<uint8_t> bytes) { pending.push_back(std::move(bytes)); }

        bool ReadMessage(AlsaMidiMessage &message, int timeoutMs)
        {
            ++reads;
            if (timeoutMs != 0)
            {
                sawBlockingRead = true;
            }
            if (pending.empty())
            {
                return false;
            }
            current = std::move(pending.front());
            pending.pop_front();
            message.Set(current.data(), current.size());
            message.realtime_sec = 1;
            message.realtime_nsec = 2;
            return true;
        }

    private:
        std::vector<uint8_t> current;
    };
}

TEST_CASE("MIDI events read twice in a cycle are all kept", "[rt_hygiene]")
{
    RealtimeMidiEventBuffer buffer(16, 1024);
    FakeMidiSource source;
    AlsaMidiMessage message;

    buffer.Clear();
    source.Push({0x90, 60, 100});
    source.Push({0x80, 60, 0});
    REQUIRE(DrainMidiInput(source, message, buffer, 0) == 2);

    // A second read in the same cycle must append, not discard what came before.
    source.Push({0xB0, 7, 64});
    REQUIRE(DrainMidiInput(source, message, buffer, 0) == 1);

    REQUIRE(buffer.Count() == 3);
    MidiEvent *events = buffer.Events();
    REQUIRE(events[0].size == 3);
    REQUIRE(events[0].buffer[0] == 0x90);
    REQUIRE(events[1].buffer[0] == 0x80);
    REQUIRE(events[2].buffer[0] == 0xB0);
    REQUIRE(events[2].buffer[2] == 64);
    REQUIRE(events[0].timeStamp.seconds == 1);
    REQUIRE(events[0].timeStamp.nanoseconds == 2);
    REQUIRE_FALSE(source.sawBlockingRead);

    // The next cycle starts empty.
    buffer.Clear();
    REQUIRE(buffer.Count() == 0);
    REQUIRE(buffer.DroppedEvents() == 0);
}

TEST_CASE("MIDI event overflow drops and counts without growing", "[rt_hygiene]")
{
    RealtimeMidiEventBuffer buffer(4, 1024);
    FakeMidiSource source;
    AlsaMidiMessage message;
    MidiEvent *eventsBefore = buffer.Events();

    buffer.Clear();
    for (int i = 0; i < 10; ++i)
    {
        source.Push({0x90, (uint8_t)i, 100});
    }
    REQUIRE(DrainMidiInput(source, message, buffer, 0) == 4);

    REQUIRE(buffer.Count() == 4);
    REQUIRE(buffer.EventCapacity() == 4);
    REQUIRE(buffer.Events() == eventsBefore); // never reallocated
    REQUIRE(buffer.DroppedEvents() == 6);
    REQUIRE(source.pending.empty()); // excess is drained from the source, not left queued.
    // The first four are the ones kept, in order.
    for (int i = 0; i < 4; ++i)
    {
        REQUIRE(buffer.Events()[i].buffer[1] == (uint8_t)i);
    }

    // Reporting takes the count and resets it.
    REQUIRE(buffer.TakeDroppedEvents() == 6);
    REQUIRE(buffer.DroppedEvents() == 0);
}

TEST_CASE("MIDI data memory overflow drops and counts", "[rt_hygiene]")
{
    RealtimeMidiEventBuffer buffer(100, 8);
    FakeMidiSource source;
    AlsaMidiMessage message;

    buffer.Clear();
    source.Push({0x90, 1, 1});       // 3 bytes
    source.Push({0x90, 2, 2});       // 6 bytes
    source.Push({0x90, 3, 3});       // would need 9: dropped
    source.Push({0xC0, 4});          // 8 bytes: exactly fits
    source.Push({0xC0, 5});          // full: dropped
    REQUIRE(DrainMidiInput(source, message, buffer, 0) == 3);
    REQUIRE(buffer.Count() == 3);
    REQUIRE(buffer.DroppedEvents() == 2);
    REQUIRE(buffer.Events()[2].buffer[0] == 0xC0);
    REQUIRE(buffer.Events()[2].buffer[1] == 4);
}

TEST_CASE("MIDI meta and empty messages are skipped, not counted as drops", "[rt_hygiene]")
{
    RealtimeMidiEventBuffer buffer(4, 64);
    FakeMidiSource source;
    AlsaMidiMessage message;

    buffer.Clear();
    source.Push({});
    source.Push({0xFF, 0x2F, 0x00});
    source.Push({0xF8}); // realtime clock: one byte, kept.
    REQUIRE(DrainMidiInput(source, message, buffer, 7) == 1);
    REQUIRE(buffer.Count() == 1);
    REQUIRE(buffer.Events()[0].frame == 7);
    REQUIRE(buffer.DroppedEvents() == 0);
}

TEST_CASE("Xrun counter counts on the audio thread and reports deltas", "[rt_hygiene]")
{
    RealtimeXrunCounter counter;
    REQUIRE_FALSE(counter.Take().Any());

    counter.OnInputXrun();
    counter.OnInputXrun();
    counter.OnOutputXrun();
    counter.OnFailedRecovery();

    XrunCounts counts = counter.Take();
    REQUIRE(counts.Any());
    REQUIRE(counts.input == 2);
    REQUIRE(counts.output == 1);
    REQUIRE(counts.failedRecoveries == 1);

    // Each report covers only what happened since the previous one.
    REQUIRE_FALSE(counter.Take().Any());

    // Counting from one thread while another reports loses nothing.
    constexpr int N = 100000;
    uint64_t total = 0;
    std::atomic<bool> done{false};
    std::thread audioThread([&]()
                            {
        for (int i = 0; i < N; ++i)
        {
            counter.OnInputXrun();
        }
        done = true; });
    while (!done)
    {
        total += counter.Take().input;
    }
    audioThread.join();
    total += counter.Take().input;
    REQUIRE(total == N);
}

TEST_CASE("Deferred restart hand-off", "[rt_hygiene]")
{
    using State = DeferredRestart::State;
    SECTION("service thread claims, runs and publishes")
    {
        DeferredRestart restart;
        REQUIRE(restart.Get() == State::Idle);
        REQUIRE_FALSE(restart.TryBegin()); // nothing pending

        restart.Request();
        REQUIRE(restart.Get() == State::Requested);
        REQUIRE(restart.TryBegin());
        REQUIRE_FALSE(restart.TryBegin()); // claimed once only
        REQUIRE_FALSE(restart.Withdraw()); // too late to take back
        REQUIRE(restart.Get() == State::Running);
        restart.Complete(true);
        REQUIRE(restart.Get() == State::Succeeded);
        restart.Reset();
        REQUIRE(restart.Get() == State::Idle);
    }
    SECTION("failure is published")
    {
        DeferredRestart restart;
        restart.Request();
        REQUIRE(restart.TryBegin());
        restart.Complete(false);
        REQUIRE(restart.Get() == State::Failed);
    }
    SECTION("unclaimed request can be withdrawn (no service thread)")
    {
        DeferredRestart restart;
        restart.Request();
        REQUIRE(restart.Withdraw());
        REQUIRE(restart.Get() == State::Idle);
        REQUIRE_FALSE(restart.TryBegin());
    }
    SECTION("across threads")
    {
        DeferredRestart restart;
        int sharedDriverState = 0; // written by the service thread, read by the audio thread.
        restart.Request();
        std::thread service([&]()
                            {
            while (!restart.TryBegin())
            {
                std::this_thread::yield();
            }
            sharedDriverState = 42;
            restart.Complete(true); });
        while (restart.Get() != State::Succeeded)
        {
            std::this_thread::yield();
        }
        REQUIRE(sharedDriverState == 42);
        restart.Reset();
        service.join();
    }
    SECTION("abandoning an unclaimed request withdraws it")
    {
        DeferredRestart restart;
        restart.Request();
        REQUIRE(restart.Abandon() == State::Idle);
        REQUIRE(restart.Get() == State::Idle);
        REQUIRE_FALSE(restart.TryBegin());
    }
    SECTION("abandoning a running restart: the service thread finds out and cleans up")
    {
        DeferredRestart restart;
        restart.Request();
        REQUIRE(restart.TryBegin());
        REQUIRE(restart.Abandon() == State::Abandoned);
        REQUIRE(restart.IsAbandoned());
        REQUIRE_FALSE(restart.Complete(true)); // nobody is told
        REQUIRE(restart.Get() == State::Idle);

        restart.Request();
        REQUIRE(restart.TryBegin());
        REQUIRE(restart.Abandon() == State::Abandoned);
        REQUIRE(restart.ReleaseAbandoned()); // between retries
        REQUIRE(restart.Get() == State::Idle);
        REQUIRE_FALSE(restart.ReleaseAbandoned());
    }
    SECTION("Abandon() racing Complete(): exactly one side wins, consistently")
    {
        constexpr int N = 5000;
        int abandonedCount = 0;
        int completedCount = 0;
        for (int i = 0; i < N; ++i)
        {
            DeferredRestart restart;
            restart.Request();
            REQUIRE(restart.TryBegin());
            std::atomic<bool> go{false};
            bool serviceCompleted = false;
            std::thread service([&]()
                                {
                while (!go.load(std::memory_order_acquire))
                {
                }
                serviceCompleted = restart.Complete(true); });
            go.store(true, std::memory_order_release);
            State seen = restart.Abandon();
            service.join();
            if (seen == State::Abandoned)
            {
                // The audio thread won: the service thread was told, and cleaned up.
                REQUIRE_FALSE(serviceCompleted);
                REQUIRE(restart.Get() == State::Idle);
                ++abandonedCount;
            }
            else
            {
                // The service thread won: its outcome stands, for the audio thread to act on.
                REQUIRE(seen == State::Succeeded);
                REQUIRE(serviceCompleted);
                REQUIRE(restart.Get() == State::Succeeded);
                ++completedCount;
            }
        }
        REQUIRE(abandonedCount + completedCount == N);
    }
    SECTION("abandoning after the outcome was published leaves the outcome")
    {
        DeferredRestart restart;
        restart.Request();
        REQUIRE(restart.TryBegin());
        REQUIRE(restart.Complete(false));
        REQUIRE(restart.Abandon() == State::Failed);
        REQUIRE(restart.Get() == State::Failed);
        REQUIRE_FALSE(restart.ReleaseAbandoned());
    }
}

TEST_CASE("Bounded wait for a deferred restart", "[rt_hygiene]")
{
    using State = DeferredRestart::State;
    using namespace std::chrono_literals;
    auto never = []()
    { return false; };

    SECTION("restart succeeds; host commands are processed while waiting")
    {
        DeferredRestart restart;
        restart.Request();
        std::atomic<int> waits{0};
        std::thread service([&]()
                            {
            while (!restart.TryBegin())
            {
                std::this_thread::yield();
            }
            while (waits.load() < 3) // the audio thread keeps processing commands meanwhile
            {
                std::this_thread::yield();
            }
            restart.Complete(true); });
        auto result = WaitForDeferredRestart(restart, 10s, 1ms, never, [&]()
                                             { ++waits; });
        service.join();
        REQUIRE(result == DeferredRestartResult::Restarted);
        REQUIRE(waits.load() >= 3);
        REQUIRE(restart.Get() == State::Idle);
    }
    SECTION("restart fails")
    {
        DeferredRestart restart;
        restart.Request();
        REQUIRE(restart.TryBegin());
        REQUIRE(restart.Complete(false));
        auto result = WaitForDeferredRestart(restart, 10s, 1ms, never, []() {});
        REQUIRE(result == DeferredRestartResult::Failed);
        REQUIRE(restart.Get() == State::Idle);
    }
    SECTION("nobody claims the request: times out and withdraws it")
    {
        DeferredRestart restart;
        restart.Request();
        int waits = 0;
        auto start = std::chrono::steady_clock::now();
        auto result = WaitForDeferredRestart(restart, 30ms, 1ms, never, [&]()
                                             { ++waits; });
        auto elapsed = std::chrono::steady_clock::now() - start;
        REQUIRE(result == DeferredRestartResult::TimedOut);
        REQUIRE(elapsed >= 30ms);
        REQUIRE(elapsed < 5s);
        REQUIRE(waits > 0);
        REQUIRE(restart.Get() == State::Idle);
        REQUIRE_FALSE(restart.TryBegin()); // a late service thread finds nothing to do
    }
    SECTION("restart still running at the deadline: abandoned, and the late outcome is dropped")
    {
        DeferredRestart restart;
        restart.Request();
        REQUIRE(restart.TryBegin());
        auto result = WaitForDeferredRestart(restart, 20ms, 1ms, never, []() {});
        REQUIRE(result == DeferredRestartResult::TimedOut);
        REQUIRE(restart.Get() == State::Abandoned);
        REQUIRE_FALSE(restart.Complete(true));
        REQUIRE(restart.Get() == State::Idle);
    }
    SECTION("shutdown stops the wait")
    {
        DeferredRestart restart;
        restart.Request();
        int polls = 0;
        auto result = WaitForDeferredRestart(restart, 10s, 1ms, [&]()
                                             { return ++polls > 2; }, []() {});
        REQUIRE(result == DeferredRestartResult::Terminated);
        REQUIRE(restart.Get() == State::Idle);
    }
    SECTION("an outcome already published wins over shutdown")
    {
        DeferredRestart restart;
        restart.Request();
        REQUIRE(restart.TryBegin());
        REQUIRE(restart.Complete(true));
        auto result = WaitForDeferredRestart(restart, 0s, 1ms, []()
                                             { return true; }, []() {});
        REQUIRE(result == DeferredRestartResult::Restarted);
        REQUIRE(restart.Get() == State::Idle);
    }
}

TEST_CASE("Realtime ring writer counts drops instead of logging", "[rt_hygiene]")
{
    RingBuffer<false, true> ring(256, false);
    RealtimeRingBufferWriter writer(&ring);

    int written = 0;
    int64_t value = 0;
    while (writer.write(RingBufferCommand::Lv2StateChanged, value))
    {
        ++written;
        REQUIRE(written < 1000);
    }
    REQUIRE(written > 0);
    REQUIRE(writer.DroppedWrites() == 1);
    REQUIRE_FALSE(writer.AlsaRestartRequested());
    REQUIRE(writer.DroppedWrites() == 2);
    REQUIRE(writer.TakeDroppedWrites() == 2);
    REQUIRE(writer.DroppedWrites() == 0);
}

TEST_CASE("Patch property writer reserves its full capacity up front", "[rt_hygiene]")
{
    constexpr size_t capacity = 16 * 1024;
    PatchPropertyWriter writer(1, 2, capacity);
    REQUIRE(writer.Capacity() == capacity);
    auto *buffer = writer.AquireWriteBuffer();
    REQUIRE(buffer->memory.capacity() >= capacity);
    const uint8_t *before = buffer->memory.data();
    buffer->memory.resize(capacity); // what the audio thread does: must not reallocate.
    REQUIRE(buffer->memory.data() == before);

    PatchPropertyWriter defaultWriter(1, 2);
    REQUIRE(defaultWriter.Capacity() == PatchPropertyWriter::DEFAULT_CAPACITY);
}

TEST_CASE("Sequencer event decoding skips malformed events instead of throwing", "[rt_hygiene]")
{
    AlsaMidiMessage message;

    SECTION("note on")
    {
        snd_seq_event_t event;
        memset(&event, 0, sizeof(event));
        event.type = SND_SEQ_EVENT_NOTEON;
        event.data.note.channel = 2;
        event.data.note.note = 60;
        event.data.note.velocity = 100;
        event.time.time.tv_sec = 5;
        REQUIRE(DecodeAlsaSequencerEvent(&event, -1, message) == AlsaSequencerDecodeResult::Message);
        REQUIRE(message.size == 3);
        REQUIRE(message.data[0] == 0x92);
        REQUIRE(message.data[1] == 60);
        REQUIRE(message.realtime_sec == 5);
        // channel filter
        REQUIRE(DecodeAlsaSequencerEvent(&event, 3, message) == AlsaSequencerDecodeResult::Skip);
    }
    SECTION("SysEx continuation chunk (does not start with 0xF0)")
    {
        std::vector<uint8_t> chunk(64, 0x11);
        snd_seq_event_t event;
        memset(&event, 0, sizeof(event));
        event.type = SND_SEQ_EVENT_SYSEX;
        event.data.ext.len = (unsigned int)chunk.size();
        event.data.ext.ptr = chunk.data();
        AlsaSequencerDecodeResult result = AlsaSequencerDecodeResult::Message;
        REQUIRE_NOTHROW(result = DecodeAlsaSequencerEvent(&event, -1, message));
        REQUIRE(result == AlsaSequencerDecodeResult::Malformed);
    }
    SECTION("empty SysEx")
    {
        snd_seq_event_t event;
        memset(&event, 0, sizeof(event));
        event.type = SND_SEQ_EVENT_SYSEX;
        event.data.ext.len = 0;
        event.data.ext.ptr = nullptr;
        REQUIRE(DecodeAlsaSequencerEvent(&event, -1, message) == AlsaSequencerDecodeResult::Malformed);
    }
    SECTION("large well-formed SysEx references the event data")
    {
        std::vector<uint8_t> sysex(64, 0x22);
        sysex[0] = 0xF0;
        sysex.back() = 0xF7;
        snd_seq_event_t event;
        memset(&event, 0, sizeof(event));
        event.type = SND_SEQ_EVENT_SYSEX;
        event.data.ext.len = (unsigned int)sysex.size();
        event.data.ext.ptr = sysex.data();
        REQUIRE(DecodeAlsaSequencerEvent(&event, -1, message) == AlsaSequencerDecodeResult::Message);
        REQUIRE(message.size == sysex.size());
        REQUIRE(message.data == sysex.data());
    }
    SECTION("non-MIDI sequencer events are skipped")
    {
        snd_seq_event_t event;
        memset(&event, 0, sizeof(event));
        event.type = SND_SEQ_EVENT_PORT_SUBSCRIBED;
        REQUIRE(DecodeAlsaSequencerEvent(&event, -1, message) == AlsaSequencerDecodeResult::Skip);
    }
}

TEST_CASE("MIDI drain lets source exceptions propagate (no std::terminate)", "[rt_hygiene]")
{
    struct ThrowingSource
    {
        bool ReadMessage(AlsaMidiMessage &, int) { throw std::runtime_error("boom"); }
    } source;
    RealtimeMidiEventBuffer buffer(4, 64);
    AlsaMidiMessage message;
    REQUIRE_THROWS_AS(DrainMidiInput(source, message, buffer, 0), std::runtime_error);
}

TEST_CASE("Realtime error message hand-off keeps the text without allocating", "[rt_hygiene]")
{
    RealtimeErrorMessage message;
    char text[RealtimeErrorMessage::CAPACITY];
    int err = 0;

    // Nothing pending.
    REQUIRE_FALSE(message.Take(text, &err));

    // Parts are concatenated.
    REQUIRE(message.Set(-EPIPE, "Cannot prepare playback stream (", "input", " xrun)."));
    REQUIRE(message.Take(text, &err));
    REQUIRE(std::string(text) == "Cannot prepare playback stream (input xrun).");
    REQUIRE(err == -EPIPE);
    // Taking clears it.
    REQUIRE_FALSE(message.Take(text, &err));

    REQUIRE(message.Set(-EIO, "Cannot refill playback stream (", "output", " xrun): ", "Audio playback failed."));
    REQUIRE(message.Take(text, &err));
    REQUIRE(std::string(text) == "Cannot refill playback stream (output xrun): Audio playback failed.");

    // A later Set replaces an untaken one; null parts are skipped.
    REQUIRE(message.Set(1, "first"));
    REQUIRE(message.Set(2, "second", nullptr, "!"));
    REQUIRE(message.Take(text, &err));
    REQUIRE(std::string(text) == "second!");
    REQUIRE(err == 2);

    // Overlong text is truncated, and stays terminated.
    std::string longText(RealtimeErrorMessage::CAPACITY * 2, 'x');
    REQUIRE(message.Set(0, longText.c_str(), "tail"));
    REQUIRE(message.Take(text, &err));
    REQUIRE(std::string(text) == std::string(RealtimeErrorMessage::CAPACITY - 1, 'x'));
}

TEST_CASE("Realtime error message survives concurrent set and take", "[rt_hygiene]")
{
    RealtimeErrorMessage message;
    std::atomic<bool> done{false};
    std::atomic<bool> torn{false};
    std::atomic<uint64_t> taken{0};
    std::thread reader([&]()
                       {
        char text[RealtimeErrorMessage::CAPACITY];
        int err;
        while (!done.load())
        {
            if (message.Take(text, &err))
            {
                ++taken;
                // Each message is "<n>:<n>" with err == n; a torn copy would not match.
                std::string s = text;
                std::string expected = std::to_string(err) + ":" + std::to_string(err);
                if (s != expected)
                {
                    torn = true;
                }
            }
        } });
    char number[32];
    for (int i = 0; i < 200000; ++i)
    {
        snprintf(number, sizeof(number), "%d", i);
        message.Set(i, number, ":", number);
    }
    // Make sure the reader really overlapped with the writer: let it see at least one
    // message (bounded, so a broken Take() fails rather than hangs).
    auto giveUp = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (taken.load() == 0 && std::chrono::steady_clock::now() < giveUp)
    {
        message.Set(7, "7", ":", "7");
        std::this_thread::yield();
    }
    done = true;
    reader.join();
    REQUIRE(taken > 0);
    REQUIRE_FALSE(torn);
}

TEST_CASE("Period duration snapshot", "[rt_hygiene]")
{
    REQUIRE(RealtimePeriodDuration(0, 48000) == std::chrono::milliseconds(1));
    REQUIRE(RealtimePeriodDuration(64, 0) == std::chrono::milliseconds(1));
    REQUIRE(RealtimePeriodDuration(48, 48000) == std::chrono::milliseconds(1));
    REQUIRE(RealtimePeriodDuration(128, 48000) == std::chrono::microseconds(2666));
    REQUIRE(RealtimePeriodDuration(1, 48000) == std::chrono::microseconds(100)); // floor.
}
