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

#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <ctime>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <initializer_list>
#include <stdexcept>
#include <semaphore.h>
#include "PiPedalException.hpp"

#ifndef NO_MLOCK
#include <sys/mman.h>
#endif /* NO_MLOCK */

namespace pipedal
{

    enum class RingBufferStatus
    {
        Ready,
        TimedOut,
        Closed,
        Kicked // kickReader() was called; there may be no data.
    };

    // One piece of a message written with RingBuffer::writeSegments().
    struct RingBufferSegment
    {
        size_t size;
        const void *data;
    };

    /**
     * @brief Lock-free single-consumer ring buffer for variable-sized messages.
     *
     * Exactly one thread reads. The reader never takes a lock.
     *
     * MULTI_WRITER == false: exactly one thread writes, and the writer never takes a lock.
     *
     * MULTI_WRITER == true: writers serialise among themselves on writeMutex. The reader never
     * touches writeMutex, so a realtime reader can poll a ring that several non-realtime threads
     * write to. (A realtime thread must not write to a MULTI_WRITER ring.)
     *
     * SEMAPHORE_READER == true: the reader may block (readWait*) until data arrives. The writer
     * wakes it with sem_post(), which takes no lock. A post is only made when the reader has
     * re-armed the wake-up flag since the previous post, so a burst of writes costs at most one
     * post per reader wake-up, and sem_post only makes a futex syscall when the reader is
     * actually blocked.
     *
     * Per-re-arm alone does not bound posts per audio cycle: a reader that drains and re-arms
     * between two writes of the same cycle earns a second post. A single (realtime) writer can
     * therefore bracket each audio cycle with beginWakeBatch()/endWakeBatch(): writes inside the
     * batch only note that a wake is owed, and endWakeBatch() makes the (re-arm-gated) post once.
     * That gives at most one post per reader per audio cycle; the reader sees the cycle's data at
     * most one cycle later than an immediate post would have delivered it. Writes outside a batch
     * post immediately, as above.
     *
     * Memory ordering: each position is stored only by its owner (writePosition by the writer,
     * readPosition by the reader) with release semantics, and loaded by the other side with
     * acquire semantics. The writer's release store of writePosition publishes the message bytes
     * to the reader; the reader's release store of readPosition tells the writer that the bytes
     * it consumed may be overwritten.
     *
     * Capacity is size-1 bytes, so that an empty ring can be distinguished from a full one.
     * Messages may wrap around the end of the buffer.
     */
    template <bool MULTI_WRITER = false, bool SEMAPHORE_READER = false>
    class RingBuffer
    {
        char *buffer;
        bool mlocked = false;
        size_t ringBufferSize;
        size_t ringBufferMask;

        // Positions are kept in [0, ringBufferSize). Separate cache lines avoid false sharing
        // between the reader and the writer.
        alignas(64) std::atomic<size_t> readPosition{0};
        alignas(64) std::atomic<size_t> writePosition{0};

        // MULTI_WRITER only. Never taken by the reader.
        std::mutex writeMutex;

        std::atomic<bool> is_open{true};

        // SEMAPHORE_READER only.
        // wakePending == true means "a post has been made since the reader last re-armed".
        std::atomic<bool> wakePending{false};
        sem_t readSemaphore;
        std::atomic<uint64_t> postCount{0}; // test hook.
        std::atomic<bool> kickRequested{false};

        // SEMAPHORE_READER, single writer only. Owned by the writer thread.
        bool wakeBatchActive = false;
        bool wakeBatchDirty = false;

        static size_t nextPowerOfTwo(size_t size)
        {
            size_t v = 1;
            while (v < size)
            {
                v *= 2;
            }
            return v;
        }

    public:
        RingBuffer(size_t ringBufferSize = 65536, bool mLock = true)
        {
            this->ringBufferSize = ringBufferSize = nextPowerOfTwo(ringBufferSize);
            ringBufferMask = ringBufferSize - 1;
            buffer = new char[ringBufferSize];
            if (sem_init(&readSemaphore, 0, 0) != 0)
            {
                delete[] buffer;
                throw PiPedalStateException("RingBuffer: sem_init failed.");
            }

#ifndef NO_MLOCK
            if (mLock)
            {
                if (mlock(buffer, ringBufferSize))
                {
                    sem_destroy(&readSemaphore);
                    delete[] buffer;
                    throw PiPedalStateException("Mlock failed.");
                }
                this->mlocked = true;
            }
#endif
        }
        RingBuffer(const RingBuffer &) = delete;
        RingBuffer &operator=(const RingBuffer &) = delete;

        ~RingBuffer()
        {
#ifndef NO_MLOCK
            if (this->mlocked)
            {
                munlock(buffer, ringBufferSize);
            }
#endif
            sem_destroy(&readSemaphore);
            delete[] buffer;
        }

        // Empties and re-opens the ring. Only call while no other thread is using it.
        void reset()
        {
            this->readPosition.store(0, std::memory_order_seq_cst);
            this->writePosition.store(0, std::memory_order_seq_cst);
            this->is_open.store(true, std::memory_order_seq_cst);
            this->wakePending.store(false, std::memory_order_seq_cst);
            this->wakeBatchActive = false;
            this->wakeBatchDirty = false;
            this->kickRequested.store(false, std::memory_order_seq_cst);
            while (sem_trywait(&readSemaphore) == 0)
            {
            }
        }

        // Releases a blocked reader; subsequent waits return Closed (or Ready while data remains).
        void close()
        {
            if constexpr (SEMAPHORE_READER)
            {
                this->is_open.store(false, std::memory_order_seq_cst);
                sem_post(&readSemaphore);
            }
        }

        template <class Rep, class Period>
        RingBufferStatus readWait_for(const std::chrono::duration<Rep, Period> &timeout)
        {
            static_assert(SEMAPHORE_READER, "SEMAPHORE_READER is not set to true.");
            Deadline deadline = deadlineFromDuration(timeout);
            return waitFor(deadline, [this]()
                           { return isReadReady_(); });
        }

        template <class Clock, class Duration>
        RingBufferStatus readWait_until(const std::chrono::time_point<Clock, Duration> &time_point)
        {
            static_assert(SEMAPHORE_READER, "SEMAPHORE_READER is not set to true.");
            Deadline deadline = deadlineFromTimePoint(time_point);
            return waitFor(deadline, [this]()
                           { return isReadReady_(); });
        }

        // Wait until at least `size` bytes are readable.
        template <class Clock, class Duration>
        RingBufferStatus readWait_until(size_t size, const std::chrono::time_point<Clock, Duration> &time_point)
        {
            static_assert(SEMAPHORE_READER, "SEMAPHORE_READER is not set to true.");
            Deadline deadline = deadlineFromTimePoint(time_point);
            return waitFor(deadline, [this, size]()
                           { return readSpace() >= size; });
        }

        // Wait (without timeout) for a complete packet. Returns false if the ring was closed.
        bool readWait()
        {
            static_assert(SEMAPHORE_READER, "SEMAPHORE_READER is not set to true.");
            Deadline deadline{true, {}};
            return waitFor(deadline, [this]()
                           { return isReadReady_(); }) == RingBufferStatus::Ready;
        }

        size_t writeSpace()
        {
            size_t r = readPosition.load(std::memory_order_acquire);
            size_t w = writePosition.load(std::memory_order_acquire);
            return freeSpace(r, w);
        }

        size_t readSpace()
        {
            size_t w = writePosition.load(std::memory_order_acquire);
            size_t r = readPosition.load(std::memory_order_acquire);
            return usedSpace(r, w);
        }

        // Write a message made of several pieces, atomically: the reader sees all of it or none of it,
        // and a SEMAPHORE_READER is woken (at most) once.
        bool writeSegments(std::initializer_list<RingBufferSegment> segments)
        {
            return writeSegments(segments.begin(), segments.size());
        }

        bool writeSegments(const RingBufferSegment *segments, size_t count)
        {
            if constexpr (MULTI_WRITER)
            {
                std::lock_guard writeLock{writeMutex};
                return writeSegments_(segments, count);
            }
            else
            {
                return writeSegments_(segments, count);
            }
        }

        bool write(size_t bytes, const uint8_t *data)
        {
            RingBufferSegment segment{bytes, data};
            return writeSegments(&segment, 1);
        }

        // Write two disjoint areas of memory atomically, as: data[bytes], (size_t)bytes2, data2[bytes2].
        bool write(size_t bytes, const uint8_t *data, size_t bytes2, const uint8_t *data2)
        {
            RingBufferSegment segments[3]{
                {bytes, data},
                {sizeof(bytes2), &bytes2},
                {bytes2, data2}};
            return writeSegments(segments, 3);
        }

        size_t read_packet(size_t maxSize, void *data)
        {
            size_t packet_size;
            if (!read(sizeof(packet_size), (uint8_t *)&packet_size))
            {
                throw std::runtime_error("RingBuffer::read_packet: failed to read packet size.");
            }
            if (packet_size > maxSize)
            {
                throw std::runtime_error("RingBuffer::read_packet: packet size too large.");
            }
            if (!read(packet_size, (uint8_t *)data))
            {
                throw std::runtime_error("RingBuffer::read_packet: failed to read packet data.");
            }
            return packet_size;
        }

        // Reader only. Returns false (and consumes nothing) if fewer than `bytes` bytes are available.
        bool read(size_t bytes, uint8_t *data)
        {
            size_t r = readPosition.load(std::memory_order_relaxed);
            size_t w = writePosition.load(std::memory_order_acquire);
            if (usedSpace(r, w) < bytes)
                return false;
            copyOut(r, data, bytes);
            readPosition.store((r + bytes) & ringBufferMask, std::memory_order_release);
            return true;
        }

        // True if a complete packet (uint32_t length prefix + body) is readable, or the ring is closed.
        bool isReadReady()
        {
            if (isReadReady_())
                return true;
            return !this->is_open.load(std::memory_order_acquire);
        }
        bool isReadReady(size_t size)
        {
            return readSpace() >= size;
        }

        // Writer only (single-writer SEMAPHORE_READER rings). Defers reader wake-ups until
        // endWakeBatch(), which posts at most once. Lock-free and allocation-free.
        void beginWakeBatch()
        {
            static_assert(SEMAPHORE_READER && !MULTI_WRITER, "Wake batches need a single writer and a semaphore reader.");
            wakeBatchActive = true;
        }
        void endWakeBatch()
        {
            static_assert(SEMAPHORE_READER && !MULTI_WRITER, "Wake batches need a single writer and a semaphore reader.");
            wakeBatchActive = false;
            if (wakeBatchDirty)
            {
                wakeBatchDirty = false;
                wakeReader();
            }
        }

        // Any thread (non-realtime callers, or the writer outside a wake batch). Makes a blocked
        // (or the next) readWait* return Kicked even if no data has arrived, so the reader can do
        // other work, e.g. retry messages it is waiting to send. Lock-free; at most one post per
        // reader re-arm, like a write.
        void kickReader()
        {
            static_assert(SEMAPHORE_READER, "SEMAPHORE_READER is not set to true.");
            kickRequested.store(true, std::memory_order_seq_cst);
            wakeReader();
        }

        // Test hook: number of sem_post() calls made to wake the reader (excludes close()).
        uint64_t wakePostCountForTest() const
        {
            return postCount.load(std::memory_order_relaxed);
        }

        // Test hook: the reader semaphore's count.
        int semaphoreValueForTest()
        {
            int value = 0;
            sem_getvalue(&readSemaphore, &value);
            return value;
        }

    private:
        struct Deadline
        {
            bool infinite = false;
            std::chrono::steady_clock::time_point time;
        };

        // Waits longer than this are treated as "forever" (avoids overflow with time_point::max()).
        static constexpr std::chrono::hours MAX_FINITE_WAIT{24 * 365};

        // Range checks are done in floating-point seconds: converting duration::min()/max()
        // (or a time_point near them) to a finer integer unit, or subtracting now() from
        // one, overflows. Only an in-range value is converted to steady_clock units.
        using FloatSeconds = std::chrono::duration<double>;

        template <class Rep, class Period>
        static Deadline deadlineFromDuration(const std::chrono::duration<Rep, Period> &timeout)
        {
            FloatSeconds seconds = std::chrono::duration_cast<FloatSeconds>(timeout);
            if (!(seconds <= FloatSeconds(MAX_FINITE_WAIT))) // also catches NaN.
            {
                return Deadline{true, {}};
            }
            auto now = std::chrono::steady_clock::now();
            if (seconds <= FloatSeconds::zero())
            {
                return Deadline{false, now}; // already expired.
            }
            return Deadline{false, now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(seconds)};
        }

        template <class Clock, class Duration>
        static Deadline deadlineFromTimePoint(const std::chrono::time_point<Clock, Duration> &time_point)
        {
            auto now = Clock::now();
            FloatSeconds remaining =
                std::chrono::duration_cast<FloatSeconds>(time_point.time_since_epoch()) -
                std::chrono::duration_cast<FloatSeconds>(now.time_since_epoch());
            if constexpr (std::is_same_v<Clock, std::chrono::steady_clock>)
            {
                if (!(remaining <= FloatSeconds(MAX_FINITE_WAIT)))
                    return Deadline{true, {}};
                if (remaining <= FloatSeconds::zero())
                    return Deadline{false, now}; // already expired.
                // In range (within a year of now): exact conversion, no overflow.
                return Deadline{false, std::chrono::time_point_cast<std::chrono::steady_clock::duration>(time_point)};
            }
            else
            {
                return deadlineFromDuration(remaining);
            }
        }

        size_t freeSpace(size_t r, size_t w) const
        {
            return (r - 1 - w) & ringBufferMask;
        }
        size_t usedSpace(size_t r, size_t w) const
        {
            return (w - r) & ringBufferMask;
        }

        void copyIn(size_t index, const void *data, size_t bytes)
        {
            if (bytes == 0)
            {
                return; // empty segments may have a null data pointer (memcpy(dst, nullptr, 0) is UB).
            }
            const char *src = (const char *)data;
            size_t first = std::min(bytes, ringBufferSize - index);
            memcpy(buffer + index, src, first);
            if (bytes > first)
            {
                memcpy(buffer, src + first, bytes - first);
            }
        }
        void copyOut(size_t index, void *data, size_t bytes) const
        {
            if (bytes == 0)
            {
                return;
            }
            char *dst = (char *)data;
            size_t first = std::min(bytes, ringBufferSize - index);
            memcpy(dst, buffer + index, first);
            if (bytes > first)
            {
                memcpy(dst + first, buffer, bytes - first);
            }
        }

        bool writeSegments_(const RingBufferSegment *segments, size_t count)
        {
            size_t total = 0;
            for (size_t i = 0; i < count; ++i)
            {
                total += segments[i].size;
            }
            // writePosition is only stored by the (serialised) writer, so relaxed is enough here.
            size_t w = writePosition.load(std::memory_order_relaxed);
            // acquire: the reader has finished copying out the bytes it released.
            size_t r = readPosition.load(std::memory_order_acquire);
            if (total > freeSpace(r, w))
            {
                return false;
            }
            if (total == 0)
            {
                return true;
            }
            for (size_t i = 0; i < count; ++i)
            {
                copyIn(w, segments[i].data, segments[i].size);
                w = (w + segments[i].size) & ringBufferMask;
            }
            // release: publishes the bytes copied above.
            writePosition.store(w, std::memory_order_release);

            if constexpr (SEMAPHORE_READER)
            {
                if (!MULTI_WRITER && wakeBatchActive)
                {
                    wakeBatchDirty = true; // posted (at most once) by endWakeBatch().
                }
                else
                {
                    wakeReader();
                }
            }
            return true;
        }

        // Writer side. Lock-free; at most one sem_post per reader re-arm.
        void wakeReader()
        {
            // Pairs with the fence in waitFor(): either the reader sees our writePosition store,
            // or we see its wakePending=false store (or a later one) and post.
            std::atomic_thread_fence(std::memory_order_seq_cst);
            if (!wakePending.load(std::memory_order_relaxed))
            {
                if (!wakePending.exchange(true, std::memory_order_acq_rel))
                {
                    postCount.fetch_add(1, std::memory_order_relaxed);
                    sem_post(&readSemaphore);
                }
            }
        }

        // Reader side.
        template <typename READY>
        RingBufferStatus waitFor(const Deadline &deadline, READY isReady)
        {
            while (true)
            {
                // Re-arm: the next write after this point will post.
                wakePending.store(false, std::memory_order_release);
                std::atomic_thread_fence(std::memory_order_seq_cst);

                // Data takes priority over a kick: a kick that arrives while data is ready stays
                // in kickRequested and is reported as Kicked by the next wait (once the reader has
                // drained what's ready). Harmless: the caller just goes round its loop once more.
                if (isReady())
                {
                    drainStalePost();
                    return RingBufferStatus::Ready;
                }
                if (!is_open.load(std::memory_order_acquire))
                {
                    return RingBufferStatus::Closed;
                }
                // Checked after the re-arm + fence, like the data: a kick either is seen here or posts.
                if (kickRequested.exchange(false, std::memory_order_seq_cst))
                {
                    drainStalePost();
                    return RingBufferStatus::Kicked;
                }
                if (!semWait(deadline))
                {
                    // timed out. Report data that arrived at the last moment.
                    if (isReady())
                    {
                        drainStalePost();
                        return RingBufferStatus::Ready;
                    }
                    if (!is_open.load(std::memory_order_acquire))
                        return RingBufferStatus::Closed;
                    return RingBufferStatus::TimedOut;
                }
                // Woken (possibly by a stale post); re-check.
            }
        }

        // Reader side, returning Ready without having waited on the semaphore. A writer
        // may have posted after the re-arm above (its data is what made us ready); left
        // alone, that post would make the next wait return at once for nothing, and a
        // reader that keeps finding data without blocking would let the count grow
        // without bound. Consume it. If the writer has set wakePending but not yet
        // posted, the post lands later and is drained by the next Ready, so the count
        // stays bounded. Nothing is lost: every wait re-checks isReady()/is_open before
        // blocking.
        void drainStalePost()
        {
            while (sem_trywait(&readSemaphore) != 0 && errno == EINTR)
            {
            }
        }

        // returns false on timeout.
        bool semWait(const Deadline &deadline)
        {
            if (deadline.infinite)
            {
                while (sem_wait(&readSemaphore) != 0)
                {
                    if (errno != EINTR)
                        throw PiPedalStateException("RingBuffer: sem_wait failed.");
                }
                return true;
            }
            // std::chrono::steady_clock is CLOCK_MONOTONIC on Linux.
            auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline.time.time_since_epoch()).count();
            if (ns < 0)
                ns = 0;
            struct timespec ts;
            ts.tv_sec = (time_t)(ns / 1000000000);
            ts.tv_nsec = (long)(ns % 1000000000);
            while (sem_clockwait(&readSemaphore, CLOCK_MONOTONIC, &ts) != 0)
            {
                if (errno == ETIMEDOUT)
                    return false;
                if (errno != EINTR)
                    throw PiPedalStateException("RingBuffer: sem_clockwait failed.");
            }
            return true;
        }

        uint32_t peekSize(size_t readIndex) const
        {
            uint32_t result;
            copyOut(readIndex, &result, sizeof(result));
            return result;
        }
        // Reader side: is a complete packet (uint32_t length prefix + body) available?
        bool isReadReady_()
        {
            size_t r = readPosition.load(std::memory_order_relaxed);
            size_t w = writePosition.load(std::memory_order_acquire);
            size_t available = usedSpace(r, w);
            if (available < sizeof(uint32_t))
                return false;
            uint32_t packetSize = peekSize(r);
            return (size_t)packetSize + sizeof(uint32_t) <= available;
        }
    };

};
