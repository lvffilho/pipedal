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
#include "RingBuffer.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

using namespace pipedal;
using namespace std::chrono_literals;

namespace
{
    // Message layout used by the tests:
    //   uint32_t length  (number of bytes that follow; compatible with isReadReady()/readWait() peeking)
    //   uint32_t producer
    //   uint64_t sequence
    //   uint8_t  payload[length - 12]  (payload[i] = (uint8_t)(sequence * 31 + i + producer))
    constexpr size_t HEADER_EXTRA = sizeof(uint32_t) + sizeof(uint64_t);

    size_t payloadSize(uint64_t sequence)
    {
        // 0..96 bytes, deterministic, with odd sizes so messages straddle the wrap point.
        return (size_t)((sequence * 7919u) % 97u);
    }

    std::vector<uint8_t> makeMessage(uint32_t producer, uint64_t sequence)
    {
        size_t payload = payloadSize(sequence);
        uint32_t length = (uint32_t)(HEADER_EXTRA + payload);
        std::vector<uint8_t> message(sizeof(uint32_t) + length);
        uint8_t *p = message.data();
        memcpy(p, &length, sizeof(length));
        p += sizeof(length);
        memcpy(p, &producer, sizeof(producer));
        p += sizeof(producer);
        memcpy(p, &sequence, sizeof(sequence));
        p += sizeof(sequence);
        for (size_t i = 0; i < payload; ++i)
        {
            p[i] = (uint8_t)(sequence * 31 + i + producer);
        }
        return message;
    }

    // Reads one complete message. Returns false on a content error.
    template <typename RING>
    bool readAndVerifyMessage(RING &ring, uint32_t *producerOut, uint64_t *sequenceOut)
    {
        uint32_t length = 0;
        if (!ring.read(sizeof(length), (uint8_t *)&length))
            return false;
        if (length < HEADER_EXTRA || length > HEADER_EXTRA + 97)
            return false;
        uint8_t body[HEADER_EXTRA + 97];
        if (!ring.read(length, body))
            return false;
        uint32_t producer;
        uint64_t sequence;
        memcpy(&producer, body, sizeof(producer));
        memcpy(&sequence, body + sizeof(producer), sizeof(sequence));
        size_t payload = length - HEADER_EXTRA;
        if (payload != payloadSize(sequence))
            return false;
        const uint8_t *p = body + HEADER_EXTRA;
        for (size_t i = 0; i < payload; ++i)
        {
            if (p[i] != (uint8_t)(sequence * 31 + i + producer))
                return false;
        }
        *producerOut = producer;
        *sequenceOut = sequence;
        return true;
    }

    template <typename RING>
    void writeBlocking(RING &ring, const std::vector<uint8_t> &message, const std::atomic<bool> &abort)
    {
        while (!ring.write(message.size(), (uint8_t *)message.data()))
        {
            if (abort)
                return;
            std::this_thread::yield();
        }
    }
}

TEST_CASE("ring buffer basic round trip", "[ring_buffer]")
{
    RingBuffer<false, false> ring(1024, false);
    REQUIRE(ring.readSpace() == 0);
    REQUIRE(ring.writeSpace() == 1023);

    uint8_t data[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    REQUIRE(ring.write(sizeof(data), data));
    REQUIRE(ring.readSpace() == sizeof(data));
    REQUIRE(ring.writeSpace() == 1023 - sizeof(data));

    uint8_t out[10] = {};
    REQUIRE(ring.read(sizeof(out), out));
    REQUIRE(memcmp(data, out, sizeof(data)) == 0);
    REQUIRE(ring.readSpace() == 0);

    // Reading more than is available fails without consuming anything.
    REQUIRE(ring.write(3, data));
    REQUIRE_FALSE(ring.read(4, out));
    REQUIRE(ring.readSpace() == 3);
}

TEST_CASE("ring buffer wrap-around preserves content", "[ring_buffer]")
{
    // 64-byte ring and 13-byte messages: almost every few messages straddle the end of the buffer.
    RingBuffer<false, false> ring(64, false);
    auto fill = [](uint8_t *message, size_t size, int iteration)
    {
        for (size_t i = 0; i < size; ++i)
        {
            message[i] = (uint8_t)(iteration * 13 + i);
        }
    };
    uint8_t message[13];
    uint8_t expected[13];
    uint8_t out[13];
    for (int iteration = 0; iteration < 10000; ++iteration)
    {
        fill(message, sizeof(message), iteration);
        REQUIRE(ring.write(sizeof(message), message));
        if (iteration & 1)
        {
            // keep a second message in flight half of the time. Verify both: the one
            // written on the previous iteration, then this one.
            fill(expected, sizeof(expected), iteration - 1);
            REQUIRE(ring.read(sizeof(out), out));
            REQUIRE(memcmp(expected, out, sizeof(out)) == 0);
            REQUIRE(ring.read(sizeof(out), out));
            REQUIRE(memcmp(message, out, sizeof(out)) == 0);
        }
        else
        {
            REQUIRE(ring.readSpace() == sizeof(message));
        }
    }
    REQUIRE(ring.readSpace() == 0);
}

TEST_CASE("ring buffer two-part write layout", "[ring_buffer]")
{
    RingBuffer<true, false> ring(64, false);
    uint8_t first[5] = {1, 2, 3, 4, 5};
    uint8_t second[20];
    for (size_t i = 0; i < sizeof(second); ++i)
        second[i] = (uint8_t)(100 + i);

    // Run a few times so the two-part write also wraps.
    for (int iteration = 0; iteration < 50; ++iteration)
    {
        REQUIRE(ring.write(sizeof(first), first, sizeof(second), second));
        REQUIRE(ring.readSpace() == sizeof(first) + sizeof(size_t) + sizeof(second));

        uint8_t outFirst[5];
        size_t length = 0;
        uint8_t outSecond[20];
        REQUIRE(ring.read(sizeof(outFirst), outFirst));
        REQUIRE(ring.read(sizeof(length), (uint8_t *)&length));
        REQUIRE(length == sizeof(second));
        REQUIRE(ring.read(length, outSecond));
        REQUIRE(memcmp(first, outFirst, sizeof(first)) == 0);
        REQUIRE(memcmp(second, outSecond, sizeof(second)) == 0);
    }
}

TEST_CASE("ring buffer full-buffer behaviour", "[ring_buffer]")
{
    RingBuffer<false, false> ring(64, false);
    REQUIRE(ring.writeSpace() == 63);

    uint8_t data[64];
    for (size_t i = 0; i < sizeof(data); ++i)
        data[i] = (uint8_t)i;

    // A write larger than the free space fails and leaves the ring untouched.
    REQUIRE_FALSE(ring.write(64, data));
    REQUIRE(ring.readSpace() == 0);
    REQUIRE(ring.writeSpace() == 63);

    // Fill with 7-byte messages until a write is refused.
    size_t written = 0;
    while (ring.write(7, data))
    {
        written += 7;
        REQUIRE(written <= 63);
    }
    REQUIRE(ring.readSpace() == written);
    REQUIRE(ring.writeSpace() == 63 - written);

    // A refused write must not have changed anything.
    REQUIRE_FALSE(ring.write(7, data));
    REQUIRE(ring.readSpace() == written);

    // Draining one message makes room for exactly one more.
    uint8_t out[7];
    REQUIRE(ring.read(7, out));
    REQUIRE(memcmp(out, data, 7) == 0);
    REQUIRE(ring.write(7, data));
    REQUIRE(ring.readSpace() == written);
}

TEST_CASE("ring buffer SPSC stress", "[ring_buffer]")
{
    constexpr uint64_t MESSAGES = 1000000;
    RingBuffer<false, true> ring(4096, false);

    std::atomic<bool> failed{false};
    std::thread producer([&]()
                         {
        for (uint64_t i = 0; i < MESSAGES && !failed; ++i)
        {
            writeBlocking(ring, makeMessage(0, i), failed);
        } });

    uint64_t expected = 0;
    while (expected < MESSAGES)
    {
        // No REQUIRE while the producer is joinable: a failing REQUIRE throws, and
        // destroying a joinable std::thread terminates the test run. Record and break.
        RingBufferStatus status = ring.readWait_for(1s);
        if (status != RingBufferStatus::Ready)
        {
            failed = true; // producer stalled, a wake-up was lost, or a spurious Closed.
            break;
        }
        while (ring.isReadReady() && expected < MESSAGES)
        {
            uint32_t producerId;
            uint64_t sequence;
            if (!readAndVerifyMessage(ring, &producerId, &sequence) || producerId != 0 || sequence != expected)
            {
                failed = true;
                break;
            }
            ++expected;
        }
        if (failed)
            break;
    }
    producer.join();
    REQUIRE_FALSE(failed);
    REQUIRE(expected == MESSAGES);
    REQUIRE(ring.readSpace() == 0);
}

TEST_CASE("ring buffer multi-writer stress with polling reader", "[ring_buffer]")
{
    constexpr uint32_t PRODUCERS = 3;
    constexpr uint64_t MESSAGES_PER_PRODUCER = 200000;
    RingBuffer<true, false> ring(4096, false);

    std::atomic<bool> failed{false};
    std::vector<std::thread> producers;
    for (uint32_t p = 0; p < PRODUCERS; ++p)
    {
        producers.emplace_back([&ring, &failed, p]()
                               {
            for (uint64_t i = 0; i < MESSAGES_PER_PRODUCER && !failed; ++i)
            {
                writeBlocking(ring, makeMessage(p, i), failed);
            } });
    }

    std::vector<uint64_t> nextSequence(PRODUCERS, 0);
    uint64_t total = 0;
    auto deadline = std::chrono::steady_clock::now() + 120s;
    while (total < PRODUCERS * MESSAGES_PER_PRODUCER && !failed)
    {
        if (!ring.isReadReady())
        {
            if (std::chrono::steady_clock::now() > deadline)
            {
                failed = true;
                break;
            }
            std::this_thread::yield();
            continue;
        }
        uint32_t producerId;
        uint64_t sequence;
        if (!readAndVerifyMessage(ring, &producerId, &sequence) || producerId >= PRODUCERS || nextSequence[producerId] != sequence)
        {
            failed = true;
            break;
        }
        ++nextSequence[producerId];
        ++total;
    }
    for (auto &t : producers)
        t.join();
    REQUIRE_FALSE(failed);
    REQUIRE(total == PRODUCERS * MESSAGES_PER_PRODUCER);
}

TEST_CASE("ring buffer reader times out", "[ring_buffer]")
{
    RingBuffer<false, true> ring(1024, false);

    auto start = std::chrono::steady_clock::now();
    REQUIRE(ring.readWait_for(50ms) == RingBufferStatus::TimedOut);
    auto elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE(elapsed >= 45ms);
    REQUIRE(elapsed < 2s);

    start = std::chrono::steady_clock::now();
    REQUIRE(ring.readWait_until(sizeof(uint64_t), start + 50ms) == RingBufferStatus::TimedOut);
    elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE(elapsed >= 45ms);
    REQUIRE(elapsed < 2s);

    // a deadline in the past returns immediately.
    REQUIRE(ring.readWait_until(std::chrono::steady_clock::now() - 1s) == RingBufferStatus::TimedOut);

    // Ready immediately when data is already present.
    uint64_t value = 42;
    REQUIRE(ring.write(sizeof(value), (uint8_t *)&value));
    REQUIRE(ring.readWait_until(sizeof(value), std::chrono::steady_clock::now() + 10s) == RingBufferStatus::Ready);
    // but not for more than is available.
    REQUIRE(ring.readWait_until(sizeof(value) + 1, std::chrono::steady_clock::now() + 20ms) == RingBufferStatus::TimedOut);
}

namespace
{
    // Runs `write` on another thread once the reader has announced that it is about to
    // block (and a little later, so that it very likely has). The outcome does not depend
    // on that timing: a reader that has not blocked yet must still see the data, since
    // every wait re-checks before blocking.
    class DeferredWriter
    {
    public:
        template <typename FN>
        DeferredWriter(FN &&write)
            : thread([this, write]()
                     {
                while (!readerWaiting.load())
                {
                    std::this_thread::yield();
                }
                std::this_thread::sleep_for(5ms);
                result = write(); })
        {
        }
        // A REQUIRE that throws before Join() must neither leave the writer spinning
        // nor destroy a joinable thread (std::terminate).
        ~DeferredWriter()
        {
            readerWaiting = true;
            if (thread.joinable())
            {
                thread.join();
            }
        }
        // Reader: call immediately before the blocking wait.
        void ReaderAboutToWait() { readerWaiting = true; }
        // Joins; returns the write's result.
        bool Join()
        {
            thread.join();
            return result;
        }

    private:
        std::atomic<bool> readerWaiting{false};
        std::atomic<bool> result{false};
        std::thread thread;
    };
}

TEST_CASE("ring buffer writer wakes blocked reader", "[ring_buffer]")
{
    RingBuffer<false, true> ring(1024, false);

    for (int iteration = 0; iteration < 20; ++iteration)
    {
        DeferredWriter writer([&ring]()
                              {
            auto message = makeMessage(0, 5);
            return ring.write(message.size(), message.data()); });

        auto start = std::chrono::steady_clock::now();
        writer.ReaderAboutToWait();
        RingBufferStatus status = ring.readWait_for(10s);
        auto elapsed = std::chrono::steady_clock::now() - start;
        REQUIRE(writer.Join());
        REQUIRE(status == RingBufferStatus::Ready);
        REQUIRE(elapsed < 5s);

        uint32_t producerId;
        uint64_t sequence;
        REQUIRE(readAndVerifyMessage(ring, &producerId, &sequence));
        REQUIRE(sequence == 5);
    }

    // readWait_until(size, ...) is woken too.
    {
        DeferredWriter writer([&ring]()
                              {
            uint64_t v = 7;
            return ring.write(sizeof(v), (uint8_t*)&v); });
        auto start = std::chrono::steady_clock::now();
        writer.ReaderAboutToWait();
        RingBufferStatus status = ring.readWait_until(sizeof(uint64_t), start + 10s);
        auto elapsed = std::chrono::steady_clock::now() - start;
        REQUIRE(writer.Join());
        REQUIRE(status == RingBufferStatus::Ready);
        REQUIRE(elapsed < 5s);
    }

    // readWait() (no timeout) is woken too.
    uint64_t v;
    REQUIRE(ring.read(sizeof(v), (uint8_t *)&v));
    {
        DeferredWriter writer([&ring]()
                              {
            auto message = makeMessage(0, 9);
            return ring.write(message.size(), message.data()); });
        writer.ReaderAboutToWait();
        bool ready = ring.readWait();
        REQUIRE(writer.Join());
        REQUIRE(ready);
    }
}

TEST_CASE("ring buffer close wakes blocked reader", "[ring_buffer]")
{
    RingBuffer<false, true> ring(1024, false);

    std::thread closer([&ring]()
                       {
        std::this_thread::sleep_for(20ms);
        ring.close(); });
    auto start = std::chrono::steady_clock::now();
    RingBufferStatus status = ring.readWait_for(10s);
    auto elapsed = std::chrono::steady_clock::now() - start;
    closer.join();
    REQUIRE(status == RingBufferStatus::Closed);
    REQUIRE(elapsed < 5s);

    // once closed, waits return immediately.
    REQUIRE(ring.readWait() == false);
    REQUIRE(ring.readWait_until(sizeof(uint64_t), std::chrono::steady_clock::now() + 10s) == RingBufferStatus::Closed);
    REQUIRE(ring.isReadReady()); // closed reports "ready" so that pollers notice.

    // reset() re-opens.
    ring.reset();
    REQUIRE(ring.readWait_for(10ms) == RingBufferStatus::TimedOut);

    // blocked readWait() is released by close().
    std::thread closer2([&ring]()
                        {
        std::this_thread::sleep_for(20ms);
        ring.close(); });
    bool readResult = ring.readWait();
    closer2.join();
    REQUIRE(readResult == false);
}
TEST_CASE("ring buffer write may use the whole free space", "[ring_buffer]")
{
    RingBuffer<false, false> ring(64, false);
    uint8_t data[63];
    for (size_t i = 0; i < sizeof(data); ++i)
        data[i] = (uint8_t)(i * 3);
    REQUIRE(ring.write(sizeof(data), data));
    REQUIRE(ring.writeSpace() == 0);
    REQUIRE_FALSE(ring.write(1, data));
    uint8_t out[63];
    REQUIRE(ring.read(sizeof(out), out));
    REQUIRE(memcmp(data, out, sizeof(data)) == 0);

    RingBuffer<true, false> ring2(64, false);
    REQUIRE(ring2.write(10, data, 63 - 10 - sizeof(size_t), data));
    REQUIRE(ring2.writeSpace() == 0);
}

TEST_CASE("ring buffer segmented write is a single message", "[ring_buffer]")
{
    RingBuffer<false, true> ring(64, false);
    uint32_t length = 12;
    uint64_t pointerValue = 0x0123456789ABCDEFull;
    uint8_t payload[4] = {9, 8, 7, 6};
    for (int iteration = 0; iteration < 40; ++iteration)
    {
        REQUIRE(ring.writeSegments({{sizeof(length), &length},
                                    {sizeof(pointerValue), &pointerValue},
                                    {sizeof(payload), payload}}));
        REQUIRE(ring.isReadReady());
        uint32_t outLength;
        uint64_t outPointer;
        uint8_t outPayload[4];
        REQUIRE(ring.read(sizeof(outLength), (uint8_t *)&outLength));
        REQUIRE(ring.read(sizeof(outPointer), (uint8_t *)&outPointer));
        REQUIRE(ring.read(sizeof(outPayload), outPayload));
        REQUIRE(outLength == length);
        REQUIRE(outPointer == pointerValue);
        REQUIRE(memcmp(outPayload, payload, sizeof(payload)) == 0);
    }
    // all-or-nothing when there is no room.
    uint8_t big[60] = {};
    REQUIRE_FALSE(ring.writeSegments({{sizeof(big), big}, {sizeof(payload), payload}}));
    REQUIRE(ring.readSpace() == 0);
}


TEST_CASE("ring buffer Ready consumes the wake-up post", "[ring_buffer]")
{
    // A reader that always finds data waiting never blocks on the semaphore. Each write
    // still posts (the reader re-armed), so without draining, the count would grow by one
    // per message.
    RingBuffer<false, true> ring(1024, false);
    for (uint64_t i = 0; i < 1000; ++i)
    {
        auto message = makeMessage(0, i);
        REQUIRE(ring.write(message.size(), message.data()));
        REQUIRE(ring.readWait_for(1s) == RingBufferStatus::Ready);
        uint32_t producerId;
        uint64_t sequence;
        REQUIRE(readAndVerifyMessage(ring, &producerId, &sequence));
        REQUIRE(sequence == i);
        REQUIRE(ring.semaphoreValueForTest() == 0);
    }
    // Same through readWait() and readWait_until(size, ...).
    for (uint64_t i = 0; i < 100; ++i)
    {
        auto message = makeMessage(0, i);
        REQUIRE(ring.write(message.size(), message.data()));
        REQUIRE(ring.readWait());
        REQUIRE(ring.readWait_until(message.size(), std::chrono::steady_clock::now() + 1s) == RingBufferStatus::Ready);
        uint32_t producerId;
        uint64_t sequence;
        REQUIRE(readAndVerifyMessage(ring, &producerId, &sequence));
        REQUIRE(ring.semaphoreValueForTest() == 0);
    }
    // And the reader still times out properly afterwards (no stale post to wake it early).
    auto start = std::chrono::steady_clock::now();
    REQUIRE(ring.readWait_for(30ms) == RingBufferStatus::TimedOut);
    REQUIRE(std::chrono::steady_clock::now() - start >= 25ms);
}

TEST_CASE("ring buffer extreme deadlines", "[ring_buffer]")
{
    using steady = std::chrono::steady_clock;
    using system = std::chrono::system_clock;
    RingBuffer<false, true> ring(1024, false);

    // Already-expired extremes time out immediately (overflow used to turn some of them into "forever").
    auto start = steady::now();
    REQUIRE(ring.readWait_until(steady::time_point::min()) == RingBufferStatus::TimedOut);
    REQUIRE(ring.readWait_until(system::time_point::min()) == RingBufferStatus::TimedOut);
    REQUIRE(ring.readWait_until(std::chrono::time_point<steady, std::chrono::hours>::min()) == RingBufferStatus::TimedOut);
    REQUIRE(ring.readWait_until(sizeof(uint64_t), steady::time_point::min()) == RingBufferStatus::TimedOut);
    REQUIRE(ring.readWait_for(std::chrono::nanoseconds::min()) == RingBufferStatus::TimedOut);
    REQUIRE(ring.readWait_for(std::chrono::hours::min()) == RingBufferStatus::TimedOut);
    REQUIRE(ring.readWait_for(-1s) == RingBufferStatus::TimedOut);
    REQUIRE(steady::now() - start < 2s);

    // Far-future extremes wait "forever": Ready as soon as data is there...
    uint64_t value = 42;
    REQUIRE(ring.write(sizeof(value), (uint8_t *)&value));
    REQUIRE(ring.readWait_until(sizeof(value), steady::time_point::max()) == RingBufferStatus::Ready);
    REQUIRE(ring.readWait_until(sizeof(value), system::time_point::max()) == RingBufferStatus::Ready);
    REQUIRE(ring.readWait_until(sizeof(value), std::chrono::time_point<steady, std::chrono::hours>::max()) == RingBufferStatus::Ready);
    REQUIRE(ring.read(sizeof(value), (uint8_t *)&value));

    // ...and are released by a writer (or close) when blocked.
    {
        DeferredWriter writer([&ring]()
                              {
            auto message = makeMessage(0, 3);
            return ring.write(message.size(), message.data()); });
        writer.ReaderAboutToWait();
        RingBufferStatus status = ring.readWait_until(steady::time_point::max());
        REQUIRE(writer.Join());
        REQUIRE(status == RingBufferStatus::Ready);
        uint32_t producerId;
        uint64_t sequence;
        REQUIRE(readAndVerifyMessage(ring, &producerId, &sequence));
    }
    {
        DeferredWriter writer([&ring]()
                              {
            auto message = makeMessage(0, 4);
            return ring.write(message.size(), message.data()); });
        writer.ReaderAboutToWait();
        RingBufferStatus status = ring.readWait_for(std::chrono::hours::max());
        REQUIRE(writer.Join());
        REQUIRE(status == RingBufferStatus::Ready);
        uint32_t producerId;
        uint64_t sequence;
        REQUIRE(readAndVerifyMessage(ring, &producerId, &sequence));
    }
    {
        DeferredWriter closer([&ring]()
                              {
            ring.close();
            return true; });
        closer.ReaderAboutToWait();
        RingBufferStatus status = ring.readWait_until(system::time_point::max());
        REQUIRE(closer.Join());
        REQUIRE(status == RingBufferStatus::Closed);
    }
}

TEST_CASE("ring buffer wake batch posts at most once per cycle", "[ring_buffer]")
{
    // A reader that drains and re-arms between two writes of the same cycle: without a batch
    // that earns a second post; within a batch, the cycle makes exactly one, at its end.
    for (bool batched : {false, true})
    {
        RingBuffer<false, true> ring(1024, false);
        if (batched)
        {
            ring.beginWakeBatch();
        }
        for (uint64_t i = 0; i < 3; ++i)
        {
            auto message = makeMessage(0, i);
            REQUIRE(ring.write(message.size(), message.data()));
            uint32_t producerId;
            uint64_t sequence;
            REQUIRE(readAndVerifyMessage(ring, &producerId, &sequence));
            REQUIRE(sequence == i);
            // re-arm (the ring is empty again, so this times out at once).
            REQUIRE(ring.readWait_for(0ms) == RingBufferStatus::TimedOut);
        }
        if (batched)
        {
            REQUIRE(ring.wakePostCountForTest() == 0);
            auto message = makeMessage(0, 3);
            REQUIRE(ring.write(message.size(), message.data()));
            ring.endWakeBatch();
            REQUIRE(ring.wakePostCountForTest() == 1);
            // the deferred post wakes the reader.
            REQUIRE(ring.readWait_for(1s) == RingBufferStatus::Ready);
            // an empty batch posts nothing.
            ring.beginWakeBatch();
            uint32_t producerId;
            uint64_t sequence;
            REQUIRE(readAndVerifyMessage(ring, &producerId, &sequence));
            ring.endWakeBatch();
            REQUIRE(ring.wakePostCountForTest() == 1);
        }
        else
        {
            REQUIRE(ring.wakePostCountForTest() == 3);
        }
    }
}

TEST_CASE("ring buffer wake batch with a blocking reader", "[ring_buffer]")
{
    // The audio-thread pattern: each cycle writes several messages inside a batch while a
    // service thread blocks/drains. Every message arrives, in order, with <= 1 post per cycle.
    RingBuffer<false, true> ring(64 * 1024, false);
    constexpr uint64_t CYCLES = 300;
    constexpr uint64_t MESSAGES_PER_CYCLE = 8;
    constexpr uint64_t TOTAL = CYCLES * MESSAGES_PER_CYCLE;
    std::atomic<bool> failed{false};
    std::atomic<bool> abort{false};

    std::thread reader([&]()
                       {
        uint64_t expected = 0;
        while (expected < TOTAL)
        {
            auto status = ring.readWait_for(5s);
            if (status != RingBufferStatus::Ready)
            {
                failed = true;
                return;
            }
            while (ring.isReadReady())
            {
                uint32_t producerId;
                uint64_t sequence;
                if (!readAndVerifyMessage(ring, &producerId, &sequence) || sequence != expected)
                {
                    failed = true;
                    return;
                }
                ++expected;
            }
        } });

    uint64_t sequence = 0;
    for (uint64_t cycle = 0; cycle < CYCLES; ++cycle)
    {
        ring.beginWakeBatch();
        for (uint64_t i = 0; i < MESSAGES_PER_CYCLE; ++i)
        {
            writeBlocking(ring, makeMessage(0, sequence++), abort);
            std::this_thread::yield(); // let the reader drain and re-arm mid-cycle.
        }
        ring.endWakeBatch();
    }
    reader.join();
    REQUIRE_FALSE(failed);
    REQUIRE(ring.wakePostCountForTest() <= CYCLES);
}

TEST_CASE("ring buffer kick wakes the reader without data", "[ring_buffer]")
{
    RingBuffer<false, true> ring(1024, false);

    // a kick before the wait returns at once.
    ring.kickReader();
    REQUIRE(ring.readWait_for(10s) == RingBufferStatus::Kicked);
    // and is consumed.
    REQUIRE(ring.readWait_for(20ms) == RingBufferStatus::TimedOut);

    // a blocked reader is woken.
    {
        DeferredWriter kicker([&ring]()
                              { ring.kickReader(); return true; });
        auto start = std::chrono::steady_clock::now();
        kicker.ReaderAboutToWait();
        REQUIRE(ring.readWait_for(10s) == RingBufferStatus::Kicked);
        REQUIRE(kicker.Join());
        REQUIRE(std::chrono::steady_clock::now() - start < 5s);
        REQUIRE(ring.semaphoreValueForTest() == 0);
    }

    // data wins over a kick.
    uint64_t v = 1;
    REQUIRE(ring.write(sizeof(v), (uint8_t *)&v));
    ring.kickReader();
    REQUIRE(ring.readWait_until(sizeof(v), std::chrono::steady_clock::now() + 1s) == RingBufferStatus::Ready);
}
