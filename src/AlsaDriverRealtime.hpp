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

// Realtime-safe building blocks used by the ALSA audio thread. Kept apart from
// AlsaDriver.cpp so that they can be unit-tested without audio hardware.
//
// Everything marked "audio thread" is wait-free: no allocation, no locks, no
// logging, no syscalls. Everything marked "service thread" is meant to be called
// from a non-realtime thread, which does the reporting.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>
#include "MidiEvent.hpp"

namespace pipedal
{

    // Fixed-capacity per-cycle store for incoming MIDI events.
    //
    // Capacity (events and data bytes) is fixed at construction. When either runs
    // out, further events in the cycle are dropped and counted; the store never grows.
    class RealtimeMidiEventBuffer
    {
    public:
        RealtimeMidiEventBuffer(size_t maxEvents, size_t memoryBytes)
        {
            events.resize(maxEvents);
            memory.resize(memoryBytes);
        }

        // Audio thread: start a new cycle. Call once per cycle, never mid-cycle.
        void Clear() noexcept
        {
            eventCount = 0;
            memoryIndex = 0;
        }

        // Audio thread. Returns false (and counts a drop) if the event does not fit.
        bool Add(uint32_t frame, const MidiTimestamp &timeStamp, const uint8_t *data, size_t size) noexcept
        {
            if (eventCount >= events.size() || size > memory.size() - memoryIndex)
            {
                droppedEvents.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            uint8_t *buffer = memory.data() + memoryIndex;
            std::memcpy(buffer, data, size);
            memoryIndex += size;

            MidiEvent *pEvent = events.data() + eventCount++;
            pEvent->timeStamp = timeStamp;
            pEvent->frame = frame;
            pEvent->size = (uint32_t)size;
            pEvent->buffer = buffer;
            return true;
        }

        size_t Count() const noexcept { return eventCount; }
        MidiEvent *Events() noexcept { return events.data(); }
        size_t EventCapacity() const noexcept { return events.size(); }
        size_t MemoryCapacity() const noexcept { return memory.size(); }

        uint64_t DroppedEvents() const noexcept { return droppedEvents.load(std::memory_order_relaxed); }
        // Service thread: drops since the previous call.
        uint64_t TakeDroppedEvents() noexcept { return droppedEvents.exchange(0, std::memory_order_relaxed); }

    private:
        std::vector<MidiEvent> events;
        std::vector<uint8_t> memory;
        size_t eventCount = 0;
        size_t memoryIndex = 0;
        std::atomic<uint64_t> droppedEvents{0};
    };

    // Drain every pending message from a MIDI source into the cycle's event buffer,
    // all stamped with the same frame. Called once per audio cycle, before the PCM
    // read. Excess messages are still read from the source (so they don't arrive a
    // cycle late in a burst) but dropped and counted.
    //
    // TSource needs: bool ReadMessage(TMessage &message, int timeoutMs), with
    // TMessage exposing data/size/realtime_sec/realtime_nsec (AlsaMidiMessage).
    // Not noexcept: an exception from the source propagates to the audio thread's
    // handler (audio stops) rather than terminating the process. AlsaSequencer's
    // non-blocking ReadMessage skips malformed events instead of throwing.
    template <typename TSource, typename TMessage>
    size_t DrainMidiInput(TSource &source, TMessage &message, RealtimeMidiEventBuffer &buffer, uint32_t frame)
    {
        size_t added = 0;
        while (source.ReadMessage(message, 0))
        {
            size_t messageSize = message.size;
            if (messageSize == 0)
            {
                continue;
            }
            // for now, prevent META event messages from propagating.
            if (message.data[0] == 0xFF && messageSize > 1)
            {
                continue;
            }
            if (buffer.Add(
                    frame,
                    MidiTimestamp(message.realtime_sec, message.realtime_nsec),
                    message.data,
                    messageSize))
            {
                ++added;
            }
        }
        return added;
    }

    struct XrunCounts
    {
        uint64_t input = 0;
        uint64_t output = 0;
        uint64_t failedRecoveries = 0;
        // Device restarts performed on the audio thread itself, because no service thread was running.
        uint64_t audioThreadRestarts = 0;

        bool Any() const { return input != 0 || output != 0 || failedRecoveries != 0 || audioThreadRestarts != 0; }
    };

    // Xrun bookkeeping. The audio thread counts; a service thread reports.
    class RealtimeXrunCounter
    {
    public:
        // Audio thread.
        void OnInputXrun() noexcept { input.fetch_add(1, std::memory_order_relaxed); }
        void OnOutputXrun() noexcept { output.fetch_add(1, std::memory_order_relaxed); }
        void OnFailedRecovery() noexcept { failedRecoveries.fetch_add(1, std::memory_order_relaxed); }
        void OnAudioThreadRestart() noexcept { audioThreadRestarts.fetch_add(1, std::memory_order_relaxed); }

        // Service thread: counts since the previous call.
        XrunCounts Take() noexcept
        {
            XrunCounts result;
            result.input = input.exchange(0, std::memory_order_relaxed);
            result.output = output.exchange(0, std::memory_order_relaxed);
            result.failedRecoveries = failedRecoveries.exchange(0, std::memory_order_relaxed);
            result.audioThreadRestarts = audioThreadRestarts.exchange(0, std::memory_order_relaxed);
            return result;
        }

    private:
        std::atomic<uint64_t> input{0};
        std::atomic<uint64_t> output{0};
        std::atomic<uint64_t> failedRecoveries{0};
        std::atomic<uint64_t> audioThreadRestarts{0};
    };

    // Length of one audio period, for polling loops. 1ms if the format is not known yet;
    // never less than 100us.
    inline std::chrono::microseconds RealtimePeriodDuration(uint32_t bufferSize, uint32_t sampleRate) noexcept
    {
        if (sampleRate == 0 || bufferSize == 0)
        {
            return std::chrono::milliseconds(1);
        }
        auto us = (int64_t)bufferSize * 1000000 / (int64_t)sampleRate;
        if (us < 100)
        {
            us = 100;
        }
        return std::chrono::microseconds(us);
    }

    // Why the audio thread gave up on in-place recovery, kept for the service thread to
    // log. The text lives in a fixed buffer: setting it copies (and truncates), never
    // allocates. The error code is kept as a number and turned into text (snd_strerror)
    // by the service thread.
    //
    // One audio-thread writer, one service-thread reader. A small state machine makes the
    // hand-off race-free: a Set() that finds the reader mid-copy is dropped rather than
    // overwriting the text under it.
    class RealtimeErrorMessage
    {
    public:
        static constexpr size_t CAPACITY = 256;

        // Audio thread. Concatenates the (non-null) parts. Returns false if the reader
        // was copying out a previous message, in which case this one is dropped.
        bool Set(int errorCode, const char *part1, const char *part2 = nullptr, const char *part3 = nullptr, const char *part4 = nullptr) noexcept
        {
            State current = state.load(std::memory_order_acquire);
            while (true)
            {
                if (current == State::Reading)
                {
                    return false;
                }
                if (state.compare_exchange_weak(current, State::Writing, std::memory_order_acquire))
                {
                    break;
                }
            }
            size_t length = 0;
            Append(length, part1);
            Append(length, part2);
            Append(length, part3);
            Append(length, part4);
            text[length] = '\0';
            this->errorCode = errorCode;
            state.store(State::Ready, std::memory_order_release);
            return true;
        }

        // Service thread. Copies out and clears a pending message. Returns false if there is none.
        // `out` must hold CAPACITY bytes.
        bool Take(char *out, int *errorCodeOut) noexcept
        {
            State expected = State::Ready;
            if (!state.compare_exchange_strong(expected, State::Reading, std::memory_order_acquire))
            {
                return false;
            }
            std::memcpy(out, text, CAPACITY);
            *errorCodeOut = errorCode;
            state.store(State::Empty, std::memory_order_release);
            return true;
        }

    private:
        enum class State : int
        {
            Empty,
            Writing,
            Ready,
            Reading,
        };

        void Append(size_t &length, const char *part) noexcept
        {
            if (!part)
            {
                return;
            }
            while (*part != '\0' && length < CAPACITY - 1)
            {
                text[length++] = *part++;
            }
        }

        std::atomic<State> state{State::Empty};
        int errorCode = 0;
        char text[CAPACITY] = {};
    };

    // Hand-off for a device restart that the audio thread needs but must not perform
    // itself. The audio thread requests and then waits (it has no audio to process:
    // the device is gone); a non-realtime service thread claims the request, does the
    // restart, and publishes the outcome. Release/acquire ordering on the state makes
    // the service thread's writes to the driver visible to the audio thread.
    //
    // The audio thread waits for a bounded time only (WaitForDeferredRestart). If it
    // gives up while the service thread is still working, the request is marked
    // Abandoned; the service thread sees that, stops retrying, and returns the state
    // to Idle itself.
    class DeferredRestart
    {
    public:
        enum class State : int
        {
            Idle,
            Requested,
            Running,
            Succeeded,
            Failed,
            Abandoned,
        };

        // Audio thread.
        void Request() noexcept { state.store(State::Requested, std::memory_order_release); }
        // Audio thread: acknowledge a finished restart.
        void Reset() noexcept { state.store(State::Idle, std::memory_order_release); }
        // Audio thread: take back a request no service thread has claimed yet.
        // Returns false if a service thread already claimed it (wait for it instead).
        bool Withdraw() noexcept
        {
            State expected = State::Requested;
            return state.compare_exchange_strong(expected, State::Idle, std::memory_order_acq_rel);
        }
        // Audio thread: stop waiting. An unclaimed request is withdrawn (-> Idle); a running
        // one is marked Abandoned, for the service thread to clean up. If the outcome was
        // published in the meantime, it is left in place and returned (Succeeded/Failed):
        // the caller acts on it as usual.
        State Abandon() noexcept
        {
            State current = state.load(std::memory_order_acquire);
            while (true)
            {
                State next;
                switch (current)
                {
                case State::Requested:
                    next = State::Idle;
                    break;
                case State::Running:
                    next = State::Abandoned;
                    break;
                default:
                    return current;
                }
                if (state.compare_exchange_weak(current, next, std::memory_order_acq_rel))
                {
                    return next;
                }
            }
        }

        State Get() const noexcept { return state.load(std::memory_order_acquire); }

        // Service thread: claim a pending request. Returns false if there is none.
        bool TryBegin() noexcept
        {
            State expected = State::Requested;
            return state.compare_exchange_strong(expected, State::Running, std::memory_order_acq_rel);
        }
        // Service thread: true if the audio thread stopped waiting for the claimed request.
        bool IsAbandoned() const noexcept { return Get() == State::Abandoned; }
        // Service thread: publish the outcome of a claimed request. Returns false if the
        // audio thread abandoned it; the state then goes back to Idle and nobody is told.
        bool Complete(bool succeeded) noexcept
        {
            State expected = State::Running;
            if (state.compare_exchange_strong(expected, succeeded ? State::Succeeded : State::Failed, std::memory_order_acq_rel))
            {
                return true;
            }
            ReleaseAbandoned();
            return false;
        }
        // Service thread: give up a claimed request the audio thread abandoned (-> Idle).
        // Returns false (and changes nothing) if it was not abandoned.
        bool ReleaseAbandoned() noexcept
        {
            State expected = State::Abandoned;
            return state.compare_exchange_strong(expected, State::Idle, std::memory_order_acq_rel);
        }

    private:
        std::atomic<State> state{State::Idle};
    };

    enum class DeferredRestartResult
    {
        Restarted,  // The device was reopened: carry on processing.
        Failed,     // The service thread gave up on the device.
        TimedOut,   // No outcome within the time limit. The request was withdrawn or abandoned.
        Terminated, // Shutting down. The request was withdrawn or abandoned.
    };

    // Audio thread: wait for the outcome of a requested restart (DeferredRestart::Request()),
    // for at most `timeout`. Polls every `pollInterval`; while it waits it calls
    // `whileWaiting()` once per poll (the driver uses it to keep host commands moving),
    // and stops early when `terminating()` returns true. The state is back to Idle on
    // return, except after TimedOut/Terminated with a restart still running: that one is
    // Abandoned, and the service thread returns it to Idle.
    //
    // No allocation, no locks: steady_clock::now() and sleep_for() only, plus whatever the
    // callbacks do.
    template <typename TerminatingFn, typename WhileWaitingFn>
    DeferredRestartResult WaitForDeferredRestart(
        DeferredRestart &restart,
        std::chrono::steady_clock::duration timeout,
        std::chrono::steady_clock::duration pollInterval,
        TerminatingFn &&terminating,
        WhileWaitingFn &&whileWaiting)
    {
        using State = DeferredRestart::State;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (true)
        {
            State state = restart.Get();
            bool giveUp = false;
            DeferredRestartResult giveUpResult = DeferredRestartResult::TimedOut;
            if (state != State::Succeeded && state != State::Failed)
            {
                if (terminating())
                {
                    giveUp = true;
                    giveUpResult = DeferredRestartResult::Terminated;
                }
                else if (std::chrono::steady_clock::now() >= deadline)
                {
                    giveUp = true;
                }
                if (giveUp)
                {
                    state = restart.Abandon();
                }
            }
            switch (state)
            {
            case State::Succeeded:
                restart.Reset();
                return DeferredRestartResult::Restarted;
            case State::Failed:
                restart.Reset();
                return DeferredRestartResult::Failed;
            default:
                break;
            }
            if (giveUp)
            {
                return giveUpResult;
            }
            whileWaiting();
            std::this_thread::sleep_for(pollInterval);
        }
    }
}
