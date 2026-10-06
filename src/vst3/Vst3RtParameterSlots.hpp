/*
 * MIT License
 *
 * Copyright (c) Robin E.R. Davies
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#pragma once

// Lock-free plumbing used by the VST3 host to move parameter values between
// the audio thread and a non-realtime thread. Deliberately free of VST3 SDK
// dependencies so that it can be unit-tested with ENABLE_VST3=0.

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <memory>
#include <semaphore.h>
#include <system_error>

namespace pipedal
{
    // One "latest value" slot per parameter.
    //
    // Set() may be called from any number of threads, including the audio
    // thread: it is two atomic stores, never allocates, never locks. Values are
    // coalesced (last writer wins), which is the right semantics for parameter
    // values and means the structure can never overflow, unlike a FIFO.
    //
    // Take()/ForEachPending()/Clear() are consumers. Each pending flag is
    // claimed with an atomic exchange, so concurrent consumers are safe: every
    // pending value goes to exactly one of them (this is what lets a non-RT
    // thread Clear() slots that the audio thread drains). A Set() that races
    // with a consumer is never lost: the consumer clears the flag *before*
    // reading the value, so a store that lands after the read re-raises the
    // flag and is picked up on the next pass. At worst a value is delivered
    // twice.
    // True if RtParameterSlots<T> can be instantiated on this target.
    // std::atomic<double> is not lock-free everywhere (e.g. armv6, which has no
    // 64-bit exclusive load/store), so code that is built unconditionally must
    // check this before naming RtParameterSlots<double>.
    template <typename T>
    inline constexpr bool RtParameterSlotsSupported = std::atomic<T>::is_always_lock_free;

    template <typename T>
    class RtParameterSlots
    {
    public:
        static_assert(RtParameterSlotsSupported<T>, "RtParameterSlots requires lock-free atomics.");

        RtParameterSlots() = default;
        explicit RtParameterSlots(size_t size) { Resize(size); }

        RtParameterSlots(const RtParameterSlots &) = delete;
        RtParameterSlots &operator=(const RtParameterSlots &) = delete;

        // Not thread-safe. Call before any producer or consumer runs.
        void Resize(size_t size)
        {
            slots.reset(size == 0 ? nullptr : new Slot[size]);
            this->size = size;
            anyPending.store(false, std::memory_order_relaxed);
        }

        size_t Size() const { return size; }

        // RT-safe, multi-producer.
        void Set(size_t index, T value)
        {
            if (index >= size)
                return;
            Slot &slot = slots[index];
            slot.value.store(value, std::memory_order_relaxed);
            slot.pending.store(true, std::memory_order_release);
            // seq_cst (here, in ForEachPending, and in RtWakeSemaphore) so that
            // "Set(); Post();" racing with "Wait(); ForEachPending();" can never
            // leave a value parked with no wake-up outstanding.
            anyPending.store(true);
        }

        bool HasPending() const { return anyPending.load(); }

        // Consumer. Returns true and the latest value if the slot was set
        // since the last Take().
        bool Take(size_t index, T *value)
        {
            if (index >= size)
                return false;
            Slot &slot = slots[index];
            if (!slot.pending.exchange(false, std::memory_order_acq_rel))
                return false;
            *value = slot.value.load(std::memory_order_relaxed);
            return true;
        }

        // Consumer. Calls fn(index, value) for every slot set since the
        // last pass. RT-safe as long as fn is.
        template <typename FN>
        size_t ForEachPending(FN &&fn)
        {
            if (!anyPending.exchange(false))
                return 0;
            return ScanPending(fn);
        }

        // Consumer. Like ForEachPending(), but scans every slot even when the
        // summary flag is clear. For a consumer nested inside another
        // consumer's ForEachPending() callback: the outer pass has already
        // claimed the summary flag, so a nested ForEachPending() returns 0
        // although the slots the outer pass has not reached yet are still
        // pending. Slots claimed here are skipped by the outer pass (each
        // pending flag goes to exactly one consumer). O(Size()) per call.
        template <typename FN>
        size_t ForEachPendingScanAll(FN &&fn)
        {
            anyPending.exchange(false);
            return ScanPending(fn);
        }

        // Consumer. Discards everything pending.
        void Clear()
        {
            ForEachPending([](size_t, T) {});
        }

    private:
        template <typename FN>
        size_t ScanPending(FN &fn)
        {
            size_t count = 0;
            for (size_t i = 0; i < size; ++i)
            {
                T value;
                if (Take(i, &value))
                {
                    fn(i, value);
                    ++count;
                }
            }
            return count;
        }

        struct Slot
        {
            std::atomic<T> value{};
            std::atomic<bool> pending{false};
        };
        std::unique_ptr<Slot[]> slots;
        size_t size = 0;
        std::atomic<bool> anyPending{false};
    };

    // Wakes a non-realtime worker from the audio thread. sem_post() is
    // lock-free in glibc (an atomic increment, plus a non-blocking futex wake
    // only when a waiter is parked), so it is safe to call on the audio thread,
    // unlike std::condition_variable::notify_*, which needs a mutex to be
    // race-free.
    class RtWakeSemaphore
    {
    public:
        RtWakeSemaphore()
        {
            if (sem_init(&sem, 0, 0) != 0)
            {
                throw std::system_error(errno, std::generic_category(), "sem_init failed");
            }
        }
        ~RtWakeSemaphore() { sem_destroy(&sem); }
        RtWakeSemaphore(const RtWakeSemaphore &) = delete;
        RtWakeSemaphore &operator=(const RtWakeSemaphore &) = delete;

        // RT-safe. Posts coalesce: while a wake-up is outstanding, further
        // Post()s are a single atomic exchange and no syscall.
        void Post()
        {
            if (!signalled.exchange(true))
            {
                sem_post(&sem);
            }
        }

        // Non-RT. Blocks until posted. The caller drains its state after this
        // returns; a Post() that lands after the flag is cleared here produces
        // another wake-up, so nothing is missed.
        void Wait()
        {
            while (sem_wait(&sem) != 0 && errno == EINTR)
            {
            }
            signalled.store(false);
        }

    private:
        sem_t sem;
        std::atomic<bool> signalled{false};
    };
}
