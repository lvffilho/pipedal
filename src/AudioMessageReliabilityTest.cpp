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

// Host <-> audio thread message delivery when a ring is full (AudioHost.cpp).

#include "pch.h"
#include "catch.hpp"
#include "RingBuffer.hpp"
#include "RingBufferReader.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

using namespace pipedal;

namespace
{
    // Reads one host->audio message the way ProcessInputCommands() does. Returns false if the ring is empty.
    bool ReadHostMessage(RealtimeRingBufferReader &reader, RingBufferCommand *command, int64_t *pointerOrValue)
    {
        if (reader.readSpace() <= sizeof(RingBufferCommand))
        {
            return false;
        }
        reader.read(command);
        if (*command == RingBufferCommand::SetValue)
        {
            SetControlValueBody body;
            reader.readComplete(&body);
            *pointerOrValue = (int64_t)body.value;
        }
        else
        {
            void *pointer;
            reader.readComplete(&pointer);
            *pointerOrValue = (int64_t)(intptr_t)pointer;
        }
        return true;
    }

    // Reads one audio->host message carrying a pointer-sized body.
    bool ReadRealtimeMessage(HostRingBufferReader &reader, RingBufferCommand *command, int64_t *pointerOrValue)
    {
        if (reader.readSpace() <= sizeof(RingBufferCommand))
        {
            return false;
        }
        reader.read(command);
        reader.read(pointerOrValue);
        return true;
    }

    struct Counted
    {
        static int deleted;
        int id;
        Counted(int id) : id(id) {}
        ~Counted() { ++deleted; }
    };
    int Counted::deleted = 0;
}

TEST_CASE("Host writer queues messages that find the ring full, in order", "[audio_messages]")
{
    RingBuffer<true, false> ring(256, false);
    HostRingBufferWriter writer(&ring);
    RealtimeRingBufferReader reader(&ring);

    // ReplaceEffect, then control values (that use the new effect's indices) until the ring is full.
    writer.ReplaceEffect((Lv2Pedalboard *)(intptr_t)0x1000);
    int nextValue = 0;
    while (!writer.HasPending())
    {
        writer.SetControlValue(0, 0, (float)nextValue++);
        REQUIRE(nextValue < 1000);
    }
    // the ring is full; more ReplaceEffect + values all queue behind.
    writer.ReplaceEffect((Lv2Pedalboard *)(intptr_t)0x2000);
    int valuesAfterSecondEffect = 50;
    for (int i = 0; i < valuesAfterSecondEffect; ++i)
    {
        writer.SetControlValue(0, 0, (float)nextValue++);
    }
    REQUIRE(writer.DroppedWrites() == 0);
    REQUIRE(writer.TakeDeferredWrites() == writer.PendingCount());

    // The audio thread drains; the service thread retries. Everything arrives exactly once, in order.
    std::vector<std::pair<RingBufferCommand, int64_t>> received;
    for (int iteration = 0; iteration < 100; ++iteration)
    {
        RingBufferCommand command;
        int64_t value;
        while (ReadHostMessage(reader, &command, &value))
        {
            received.push_back({command, value});
        }
        if (writer.RetryPending() && reader.readSpace() == 0)
        {
            break;
        }
    }
    REQUIRE_FALSE(writer.HasPending());
    REQUIRE(received.size() == (size_t)nextValue + 2);
    REQUIRE(received[0].first == RingBufferCommand::ReplaceEffect);
    REQUIRE(received[0].second == 0x1000);
    int expectedValue = 0;
    bool sawSecondEffect = false;
    for (size_t i = 1; i < received.size(); ++i)
    {
        if (received[i].first == RingBufferCommand::ReplaceEffect)
        {
            REQUIRE_FALSE(sawSecondEffect);
            REQUIRE(received[i].second == 0x2000);
            REQUIRE(expectedValue == nextValue - valuesAfterSecondEffect);
            sawSecondEffect = true;
            continue;
        }
        REQUIRE(received[i].first == RingBufferCommand::SetValue);
        REQUIRE(received[i].second == expectedValue++);
    }
    REQUIRE(sawSecondEffect);
    REQUIRE(expectedValue == nextValue);
}

TEST_CASE("Host writer flushes queued messages before a new write", "[audio_messages]")
{
    RingBuffer<true, false> ring(256, false);
    HostRingBufferWriter writer(&ring);
    RealtimeRingBufferReader reader(&ring);

    int nextValue = 0;
    while (!writer.HasPending())
    {
        writer.SetControlValue(0, 0, (float)nextValue++);
    }
    // Make room, then write without calling RetryPending(): the queued message goes first.
    RingBufferCommand command;
    int64_t value;
    REQUIRE(ReadHostMessage(reader, &command, &value));
    REQUIRE(ReadHostMessage(reader, &command, &value));
    REQUIRE(ReadHostMessage(reader, &command, &value));
    writer.SetControlValue(0, 0, (float)nextValue++);
    std::vector<int64_t> values;
    while (ReadHostMessage(reader, &command, &value))
    {
        values.push_back(value);
    }
    for (size_t i = 1; i < values.size(); ++i)
    {
        REQUIRE(values[i] == values[i - 1] + 1);
    }
    REQUIRE(writer.RetryPending());
    REQUIRE(values.back() == nextValue - 1);
}

TEST_CASE("Host writer caps the queue but never drops resource messages", "[audio_messages]")
{
    RingBuffer<true, false> ring(256, false);
    HostRingBufferWriter writer(&ring);

    while (!writer.HasPending())
    {
        writer.SetControlValue(0, 0, 1.0f);
    }
    while (writer.PendingCount() < HostRingBufferWriter::MAX_PENDING_MESSAGES)
    {
        writer.SetControlValue(0, 0, 1.0f);
    }
    REQUIRE(writer.DroppedWrites() == 0);
    // Past the cap: control values are dropped and counted ...
    writer.SetControlValue(0, 0, 1.0f);
    REQUIRE(writer.DroppedWrites() == 1);
    // ... but objects handed to the audio thread, and acks, still queue.
    writer.SetVuSubscriptions((RealtimeVuBuffers *)(intptr_t)0x3000);
    writer.AckVuUpdate();
    REQUIRE(writer.DroppedWrites() == 1);
    REQUIRE(writer.PendingCount() == HostRingBufferWriter::MAX_PENDING_MESSAGES + 2);

    // On close, the host gets every queued message back to release what it carries.
    size_t messages = 0;
    std::vector<int64_t> vuConfigs;
    writer.ClearPending([&](RingBufferCommand command, const uint8_t *body, size_t size)
                        {
        ++messages;
        if (command == RingBufferCommand::SetVuSubscriptions)
        {
            REQUIRE(size == sizeof(void *));
            int64_t pointer;
            memcpy(&pointer, body, sizeof(pointer));
            vuConfigs.push_back(pointer);
        } });
    REQUIRE(messages == HostRingBufferWriter::MAX_PENDING_MESSAGES + 2);
    REQUIRE(vuConfigs == std::vector<int64_t>{0x3000});
    REQUIRE_FALSE(writer.HasPending());
}

TEST_CASE("Host writer keeps order with concurrent writers and a polling reader", "[audio_messages]")
{
    // Several non-realtime writers, a small ring, and a slow reader: each writer's messages arrive
    // complete and in that writer's order, with nothing dropped.
    RingBuffer<true, false> ring(512, false);
    HostRingBufferWriter writer(&ring);
    RealtimeRingBufferReader reader(&ring);
    constexpr int WRITERS = 4;
    constexpr int PER_WRITER = 1000; // total stays under MAX_PENDING_MESSAGES, so nothing may drop.
    std::atomic<int> writersDone{0};

    std::vector<std::thread> threads;
    for (int w = 0; w < WRITERS; ++w)
    {
        threads.emplace_back([&, w]()
                             {
            for (int i = 0; i < PER_WRITER; ++i)
            {
                writer.SetControlValue(w, 0, (float)i);
            }
            ++writersDone; });
    }
    std::thread service([&]()
                        {
        while (writersDone.load() < WRITERS || writer.HasPending())
        {
            writer.RetryPending();
            std::this_thread::yield();
        } });

    int next[WRITERS] = {};
    int total = 0;
    bool ok = true;
    while (total < WRITERS * PER_WRITER && ok)
    {
        if (reader.readSpace() <= sizeof(RingBufferCommand))
        {
            std::this_thread::yield();
            continue;
        }
        RingBufferCommand command;
        reader.read(&command);
        SetControlValueBody body;
        reader.readComplete(&body);
        ok = command == RingBufferCommand::SetValue && body.effectIndex >= 0 && body.effectIndex < WRITERS &&
             (int)body.value == next[body.effectIndex];
        if (ok)
        {
            ++next[body.effectIndex];
            ++total;
        }
    }
    for (auto &t : threads)
    {
        t.join();
    }
    service.join();
    REQUIRE(ok);
    REQUIRE(total == WRITERS * PER_WRITER);
    REQUIRE(writer.DroppedWrites() == 0);
}

TEST_CASE("Realtime releases that find the ring full are retried, in order", "[audio_messages][rt_hygiene]")
{
    RingBuffer<false, true> ring(256, false);
    RealtimeRingBufferWriter writer(&ring);
    HostRingBufferReader reader(&ring);

    int64_t unused = 0;
    int filler = 0;
    while (writer.write(RingBufferCommand::Lv2StateChanged, unused))
    {
        ++filler;
    }
    REQUIRE(writer.TakeDroppedWrites() == 1);

    writer.EffectReplaced((Lv2Pedalboard *)(intptr_t)0x10);
    writer.FreeVuSubscriptions((RealtimeVuBuffers *)(intptr_t)0x20);
    writer.FreeMonitorPortSubscriptions((RealtimeMonitorPortSubscriptions *)(intptr_t)0x30);
    writer.FreeSnapshot((IndexedSnapshot *)(intptr_t)0x40);
    REQUIRE(writer.DeferredReleaseCount() == 4);
    REQUIRE(writer.DroppedWrites() == 0);
    REQUIRE(writer.TakeLostReleases() == 0);

    // Room for two: the next cycle's retry sends the oldest two.
    RingBufferCommand command;
    int64_t value;
    for (int i = 0; i < filler; ++i)
    {
        REQUIRE(ReadRealtimeMessage(reader, &command, &value));
        REQUIRE(command == RingBufferCommand::Lv2StateChanged);
        if (i == 1)
        {
            break;
        }
    }
    writer.RetryReleases();
    REQUIRE(writer.DeferredReleaseCount() == 2);

    // A new release while older ones wait goes behind them.
    writer.EffectReplaced((Lv2Pedalboard *)(intptr_t)0x50);
    REQUIRE(writer.DeferredReleaseCount() == 3);

    std::vector<std::pair<RingBufferCommand, int64_t>> received;
    for (int iteration = 0; iteration < 10; ++iteration)
    {
        while (ReadRealtimeMessage(reader, &command, &value))
        {
            if (command != RingBufferCommand::Lv2StateChanged)
            {
                received.push_back({command, value});
            }
        }
        writer.RetryReleases();
    }
    REQUIRE(writer.DeferredReleaseCount() == 0);
    std::vector<std::pair<RingBufferCommand, int64_t>> expected{
        {RingBufferCommand::EffectReplaced, 0x10},
        {RingBufferCommand::FreeVuSubscriptions, 0x20},
        {RingBufferCommand::FreeMonitorPortSubscription, 0x30},
        {RingBufferCommand::FreeSnapshot, 0x40},
        {RingBufferCommand::EffectReplaced, 0x50},
    };
    REQUIRE(received == expected);
}

TEST_CASE("Realtime release queue overflow is counted, not grown", "[audio_messages][rt_hygiene]")
{
    RingBuffer<false, true> ring(256, false);
    RealtimeRingBufferWriter writer(&ring);

    int64_t unused = 0;
    while (writer.write(RingBufferCommand::Lv2StateChanged, unused))
    {
    }
    // the last entry is kept for parameter request completions.
    for (size_t i = 0; i < RealtimeRingBufferWriter::MAX_DEFERRED_RELEASES - 1; ++i)
    {
        writer.FreeVuSubscriptions((RealtimeVuBuffers *)(intptr_t)(0x100 + i));
    }
    REQUIRE(writer.DeferredReleaseCount() == RealtimeRingBufferWriter::MAX_DEFERRED_RELEASES - 1);
    writer.FreeVuSubscriptions((RealtimeVuBuffers *)(intptr_t)0x999);
    REQUIRE(writer.DeferredReleaseCount() == RealtimeRingBufferWriter::MAX_DEFERRED_RELEASES - 1);
    REQUIRE(writer.TakeLostReleases() == 1);
    REQUIRE(writer.TakeLostReleases() == 0);

    writer.Reset();
    REQUIRE(writer.DeferredReleaseCount() == 0);
}

TEST_CASE("In-order release list reclaims earlier objects whose release was lost", "[audio_messages]")
{
    Counted::deleted = 0;
    {
        InOrderReleaseList<Counted> list;
        Counted *a = new Counted(1);
        Counted *b = new Counted(2);
        Counted *c = new Counted(3);
        Counted *d = new Counted(4);
        list.Add(a);
        list.Add(b);
        list.Add(nullptr); // ignored.
        list.Add(c);
        list.Add(d);
        REQUIRE(list.size() == 4);

        // a's release was lost; b's release covers it.
        REQUIRE(list.Release(b) == 2);
        REQUIRE(Counted::deleted == 2);
        REQUIRE(list.size() == 2);

        // a late (or duplicate) release of an already-reclaimed object does nothing.
        REQUIRE(list.Release(a) == 0);
        REQUIRE(list.Release(b) == 0);
        REQUIRE(Counted::deleted == 2);

        REQUIRE(list.Release(c) == 1);
        REQUIRE(Counted::deleted == 3);
        // d is still held by the audio thread; the destructor (or DeleteAll on close) reclaims it.
    }
    REQUIRE(Counted::deleted == 4);
}

namespace
{
    bool WriteControlValue(HostRingBufferWriter &writer, int effectIndex, int controlIndex, float value)
    {
        SetControlValueBody body;
        body.effectIndex = effectIndex;
        body.controlIndex = controlIndex;
        body.value = value;
        return writer.write(RingBufferCommand::SetValue, body);
    }
}

TEST_CASE("Host writer above the cap replaces the oldest queued value of the same control", "[audio_messages]")
{
    RingBuffer<true, false> ring(256, false);
    HostRingBufferWriter writer(&ring);

    while (!writer.HasPending())
    {
        writer.SetControlValue(9, 9, 0.0f);
    }
    // Queue: value for control (1,2) before a ReplaceEffect, (1,2) and (3,4) after it, then filler.
    writer.SetControlValue(1, 2, 100.0f);
    writer.ReplaceEffect((Lv2Pedalboard *)(intptr_t)0x1000);
    writer.SetControlValue(1, 2, 200.0f);
    writer.SetControlValue(3, 4, 300.0f);
    writer.SetControlValue(1, 2, 250.0f);
    while (writer.PendingCount() < HostRingBufferWriter::MAX_PENDING_MESSAGES)
    {
        writer.SetControlValue(9, 9, 0.0f);
    }
    REQUIRE(writer.DroppedWrites() == 0);

    // (1,2): the oldest queued value after the ReplaceEffect (200) is superseded; the new one goes last.
    REQUIRE(WriteControlValue(writer, 1, 2, 201.0f));
    REQUIRE(writer.PendingCount() == HostRingBufferWriter::MAX_PENDING_MESSAGES);
    REQUIRE(writer.DroppedWrites() == 1);
    // (5,6) has no queued value: the new value is dropped.
    REQUIRE_FALSE(WriteControlValue(writer, 5, 6, 1.0f));
    REQUIRE(writer.DroppedWrites() == 2);
    // Bypass and volume are never dropped.
    writer.SetBypass(1, true);
    writer.SetInputVolume(-3.0f);
    writer.SetOutputVolume(-6.0f);
    writer.SetSuspendBypassedPlugins(true);
    REQUIRE(writer.DroppedWrites() == 2);
    REQUIRE(writer.PendingCount() == HostRingBufferWriter::MAX_PENDING_MESSAGES + 4);

    std::vector<std::pair<RingBufferCommand, float>> interesting;
    writer.ClearPending([&](RingBufferCommand command, const uint8_t *body, size_t size)
                        {
        if (command == RingBufferCommand::SetValue)
        {
            SetControlValueBody value;
            memcpy(&value, body, sizeof(value));
            if (value.effectIndex != 9)
            {
                interesting.push_back({command, value.value});
            }
        }
        else
        {
            interesting.push_back({command, 0.0f});
        } });
    std::vector<std::pair<RingBufferCommand, float>> expected{
        {RingBufferCommand::SetValue, 100.0f}, // before the ReplaceEffect (other effect indices): kept.
        {RingBufferCommand::ReplaceEffect, 0.0f},
        {RingBufferCommand::SetValue, 300.0f},
        {RingBufferCommand::SetValue, 250.0f},
        {RingBufferCommand::SetValue, 201.0f},
        {RingBufferCommand::SetBypass, 0.0f},
        {RingBufferCommand::SetInputVolume, 0.0f},
        {RingBufferCommand::SetOutputVolume, 0.0f},
        {RingBufferCommand::SetSuspendBypassedPlugins, 0.0f},
    };
    REQUIRE(interesting == expected);
}

TEST_CASE("Host writer wakes the service thread when a message is first deferred", "[audio_messages]")
{
    // rtsvc's loop: a long wait while nothing is pending, ~10 ms waits while something is.
    RingBuffer<true, false> inputRing(256, false);
    RingBuffer<false, true> outputRing(256, false);
    HostRingBufferWriter writer(&inputRing);
    RealtimeRingBufferReader reader(&inputRing);
    writer.SetOnPendingStarted([&outputRing]()
                               { outputRing.kickReader(); });

    std::thread service([&]()
                        {
        while (true)
        {
            auto wakeTime = std::chrono::steady_clock::now() +
                (writer.HasPending() ? std::chrono::milliseconds(10) : std::chrono::milliseconds(30000));
            if (outputRing.readWait_until(wakeTime) == RingBufferStatus::Closed)
            {
                return;
            }
            if (writer.HasPending())
            {
                writer.RetryPending();
            }
        } });
    // Let the service thread settle into its long wait.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    int nextValue = 0;
    while (!writer.HasPending())
    {
        writer.SetControlValue(0, 0, (float)nextValue++);
    }
    int deferredValue = nextValue - 1;

    // The audio thread drains the ring; no other traffic follows.
    RingBufferCommand command;
    int64_t value;
    while (ReadHostMessage(reader, &command, &value))
    {
    }
    auto start = std::chrono::steady_clock::now();
    bool delivered = false;
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(5))
    {
        if (ReadHostMessage(reader, &command, &value))
        {
            delivered = value == deferredValue;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    auto elapsed = std::chrono::steady_clock::now() - start;
    outputRing.close();
    service.join();
    REQUIRE(delivered);
    REQUIRE(elapsed < std::chrono::milliseconds(100));
}

TEST_CASE("Monitor port subscriptions own their callbacks", "[audio_messages]")
{
    auto token = std::make_shared<int>(0);
    {
        auto *subscriptions = new RealtimeMonitorPortSubscriptions();
        for (int i = 0; i < 5; ++i) // vector growth copies the elements.
        {
            RealtimeMonitorPortSubscription subscription;
            subscription.subscriptionHandle = i;
            subscription.callbackPtr = new PortMonitorCallback([token](int64_t, float) {});
            subscriptions->subscriptions.push_back(subscription);
        }
        REQUIRE(token.use_count() == 6);
        delete subscriptions; // each callback deleted exactly once.
    }
    REQUIRE(token.use_count() == 1);
}
