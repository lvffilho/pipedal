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

#include "PiPedalException.hpp"
#include "Lv2Log.hpp"
#include "VuUpdate.hpp"
#include "AudioHost.hpp"
#include "lv2/atom/atom.h"
#include "RealtimeMidiEventType.hpp"
#include <chrono>
#include <atomic>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <type_traits>
#include <vector>

namespace pipedal
{
    class IndexedSnapshot;

    class MidiNotifyBody
    {
    public:
        MidiNotifyBody() = default;
        MidiNotifyBody(uint8_t cc0, uint8_t cc1, uint8_t cc2)
            : cc0_(cc0), cc1_(cc1), cc2_(cc2)
        {
        }       

        uint8_t cc0_;
        uint8_t cc1_; 
        uint8_t cc2_; 
    };


    enum class RingBufferCommand : int64_t
    {
        Invalid = 0,
        ReplaceEffect,
        EffectReplaced,
        SetValue,
        SetBypass,
        // AudioStopped,
        AudioTerminatedAbnormally, // specifically for an ALSA loss of connection.
        SetVuSubscriptions,
        FreeVuSubscriptions,

        LoadSnapshot,
        FreeSnapshot,

        SendVuUpdate,
        AckVuUpdate,

        SetMonitorPortSubscription,
        FreeMonitorPortSubscription,
        SendMonitorPortUpdate,
        AckMonitorPortUpdate,
        ParameterRequest,
        ParameterRequestComplete,

        MidiValueChanged,

        OnMidiListen,

        AtomOutput,

        MidiProgramChange, // program change requested via midi.
        AckMidiProgramChange,
        AckMidiSnapshotRequest,

        NextMidiProgram,
        NextMidiBank,
        NextMidiSnapshot,

        Lv2StateChanged,
        MaybeLv2StateChanged,
        SetInputVolume,
        SetOutputVolume,
        Lv2ErrorMessage,

        RealtimeMidiEvent,
        RealtimeMidiSnapshotRequest,

        SendPathPropertyBuffer,

        AlsaRestartRequested, // audio thread -> service thread: perform AudioDriver::ServiceRestartRequest().

        SetSuspendBypassedPlugins, // host -> audio thread: "Suspend bypassed plugins" setting.

    };

    struct RealtimePedalboardItemIndex {
        RealtimePedalboardItemIndex()
            : index(-1)
        {
        }
        RealtimePedalboardItemIndex(
            int64_t index)
            : index(index)
        {
        }
        RealtimePedalboardItemIndex(const RealtimePedalboardItemIndex& other) = default;
        RealtimePedalboardItemIndex& operator=(const RealtimePedalboardItemIndex& other) = default;

        int64_t index = -1;
    };

    struct RealtimeMidiEventRequest
    {
        RealtimeMidiEventType eventType;
    };

    struct RealtimeMidiSnapshotRequest
    {
        int32_t snapshotIndex;
        int64_t snapshotRequestId;
    };

    struct RealtimeNextMidiProgramRequest
    {
        int64_t requestId;
        int32_t direction;
    };

    struct RealtimeMidiProgramRequest
    {
        int64_t requestId;
        int8_t bank;
        uint8_t program;
    };

    class RealtimeMonitorPortSubscription
    {
    public:
        // callbackPtr is owned by the containing RealtimeMonitorPortSubscriptions (this struct is
        // copied by value, so it must not delete it itself).
        int64_t subscriptionHandle;
        int instanceIndex = 0;
        int portIndex = 0;
        PortMonitorCallback *callbackPtr = nullptr;
        int sampleRate = 0;
        int samplesToNextCallback = 0;
        bool waitingForAck = false;
        float lastValue = -1E30;
    };

    class RealtimeMonitorPortSubscriptions
    {
    public:
        RealtimeMonitorPortSubscriptions() = default;
        RealtimeMonitorPortSubscriptions(const RealtimeMonitorPortSubscriptions &) = delete;
        RealtimeMonitorPortSubscriptions &operator=(const RealtimeMonitorPortSubscriptions &) = delete;
        // Owns each subscription's callbackPtr. Deleted on a non-realtime thread.
        ~RealtimeMonitorPortSubscriptions()
        {
            for (auto &subscription : subscriptions)
            {
                delete subscription.callbackPtr;
                subscription.callbackPtr = nullptr;
            }
        }
        std::vector<RealtimeMonitorPortSubscription> subscriptions;
    };

    struct RealtimeVuBuffers
    {
        RealtimeVuBuffers()
        {
            Reset();
        }
        bool waitingForAcknowledge = false;

        const std::vector<VuUpdateX> *GetResult(size_t currentSample)
        {
            for (size_t i = 0; i < vuUpdateWorkingData.size(); ++i)
            {
                vuUpdateResponseData[i] = vuUpdateWorkingData[i];
                vuUpdateResponseData[i].sampleTime_ = currentSample;
                vuUpdateWorkingData[i].reset();
            }
            return &vuUpdateResponseData;
        }

        std::vector<RealtimePedalboardItemIndex> enabledIndexes;
        std::vector<VuUpdateX> vuUpdateWorkingData;
        std::vector<VuUpdateX> vuUpdateResponseData;

        void Reset()
        {
            for (int i = 0; i < vuUpdateWorkingData.size(); ++i)
            {
                vuUpdateWorkingData[i].reset();
            }
        }
    };

    class AudioStoppedBody
    {
        bool dummy = true;
    };

    class SetBypassBody
    {
    public:
        int effectIndex;
        bool enabled;
    };

    class SetSuspendBypassedPluginsBody
    {
    public:
        bool value;
    };

    class IEffect;
    class Lv2Pedalboard;

    class MidiValueChangedBody
    {
    public:
        int64_t instanceId;
        int controlIndex;
        float value;
        // The effect that was bound to instanceId on the audio thread when the value changed. The
        // host checks it against the installed pedalboard: until the audio thread swaps a newly
        // installed pedalboard in, the outgoing one may still send messages whose instance id names
        // an unrelated item of the new one. Only compared, never dereferenced.
        IEffect *sourceEffect;
        // The pedalboard that was running. A value from any other than the installed pedalboard is
        // dropped (see IsMidiValueFromInstalledPedalboard()). Only compared, never dereferenced.
        Lv2Pedalboard *sourcePedalboard;
    };

    // Header of an AtomOutput message (followed by size_t length, and the atom).
    class AtomOutputBody
    {
    public:
        uint64_t instanceId;
        IEffect *sourceEffect; // the effect that sent the atom; see MidiValueChangedBody.
    };

    class SetControlValueBody
    {
    public:
        int effectIndex;
        int controlIndex;
        float value;
    };
    class SetVolumeBody
    {
    public:
        float value;
    };

    class Lv2Pedalboard;

    class ReplaceEffectBody
    {
    public:
        Lv2Pedalboard *effect;
    };
    class EffectReplacedBody
    {
    public:
        Lv2Pedalboard *oldEffect;
    };

    template <bool MULTI_WRITE, bool SEMAPHORE_READ>
    class RingBufferReader
    {

    private:
        RingBuffer<MULTI_WRITE, SEMAPHORE_READ> *ringBuffer = nullptr;

    public:
        RingBufferReader()
            : ringBuffer(nullptr)
        {
        }
        RingBufferReader(RingBuffer<MULTI_WRITE, SEMAPHORE_READ> *ringBuffer)
            : ringBuffer(ringBuffer)
        {
        }
        void Reset() { ringBuffer->reset(); }
        // 0 -> ready. -1: timed out. -2: closing.
        template <class Rep, class Period>
        RingBufferStatus wait_for(const std::chrono::duration<Rep, Period> &timeout)
        {
            return ringBuffer->readWait_for(timeout);
        }

        template <typename Clock, typename Duration>
        RingBufferStatus wait_until(std::chrono::time_point<Clock, Duration> &time_point)
        {
            return ringBuffer->readWait_until(time_point);
        }
        template <typename Clock, typename Duration>
        RingBufferStatus wait_until(size_t size, std::chrono::time_point<Clock, Duration> &time_point)
        {
            return ringBuffer->readWait_until(size, time_point);
        }

        bool wait()
        {
            return ringBuffer->readWait();
        }
        size_t readSpace() const
        {
            return ringBuffer->readSpace();
        }
        template <typename T>
        bool read(T *output)
        {
            if (!ringBuffer->read(sizeof(T), (uint8_t *)output))
            {
                throw PiPedalStateException("Ringbuffer read failed. Did you forget to check for space?");
            }
            return true;
        }

        bool read(size_t size, uint8_t *data)
        {
            if (!ringBuffer->read(size, data))
            {
                throw PiPedalStateException("Ringbuffer read failed. Did you forget to check for space?");
            }
            return true;
        }
        template <typename T>
        void readComplete(T *output)
        {
            if (!ringBuffer->read(sizeof(T), (uint8_t *)output))
            {
                throw PiPedalStateException("Ringbuffer read failed. Did you forget to check for space?");
            }
        }
    };

    template <typename T>
    class CommandBuffer
    {

    public:
        CommandBuffer(RingBufferCommand command)
            : command(command)
        {
        }

        CommandBuffer(RingBufferCommand command, const T &value)
            : command(command), value(value)
        {
        }
        RingBufferCommand command;

        size_t size() const { return sizeof(RingBufferCommand) + sizeof(T); }

        T value;
    };

    template <bool MULTI_WRITER, bool SEMAPHORE_READER>
    class RingBufferWriter
    {

    private:
        RingBuffer<MULTI_WRITER, SEMAPHORE_READER> *ringBuffer;

    public:
        RingBufferWriter()
            : ringBuffer(nullptr)
        {
        }
        RingBufferWriter(RingBuffer<MULTI_WRITER, SEMAPHORE_READER> *ringBuffer)
            : ringBuffer(ringBuffer)
        {
        }

        // Host side (MULTI_WRITER): discards any messages still deferred; call ClearPending() first
        // to release the resources they carry. Realtime side: forgets deferred releases (the host
        // reclaims those objects itself when it closes the audio stream).
        // Only call while no other thread is using the ring.
        void Reset()
        {
            ringBuffer->reset();
            if constexpr (MULTI_WRITER)
            {
                std::lock_guard lock(overflow.mutex);
                overflow.messages.clear();
                overflow.count.store(0, std::memory_order_relaxed);
            }
            else
            {
                overflow.count = 0;
            }
        }

        // Number of messages dropped because the ring was full, since the last call
        // to TakeDroppedWrites(). Report these from a non-realtime thread.
        uint64_t DroppedWrites() const { return droppedWrites.load(std::memory_order_relaxed); }
        uint64_t TakeDroppedWrites() { return droppedWrites.exchange(0, std::memory_order_relaxed); }

        // Host side (MULTI_WRITER) only.
        //
        // A message that finds the ring full is not dropped: it is queued here, and every later
        // message queues behind it until the queue drains, so the audio thread still sees all
        // messages in the order they were written (a SetValue never overtakes the ReplaceEffect
        // whose effect indices it uses). The queue is retried on every write and by
        // RetryPending(), which the service thread calls whenever it wakes while messages are
        // pending; the OnPendingStarted callback wakes the service thread when the queue first
        // becomes non-empty. Above MAX_PENDING_MESSAGES queued messages, a control value (the only
        // message that is not must-deliver) displaces the oldest queued value for the same control
        // since the last ReplaceEffect, or is dropped if there is none; either way one drop is counted.
        static constexpr size_t MAX_PENDING_MESSAGES = 4096;

        // Called (outside the queue's lock, on the writing thread) when a write finds the queue
        // empty and leaves it non-empty. Set before the writer is shared between threads.
        void SetOnPendingStarted(std::function<void()> &&callback)
        {
            static_assert(MULTI_WRITER, "Host side only.");
            overflow.onPendingStarted = std::move(callback);
        }

        // Returns true if the pending queue is now empty.
        bool RetryPending()
        {
            static_assert(MULTI_WRITER, "Host side only.");
            std::lock_guard lock(overflow.mutex);
            FlushPending_();
            return overflow.messages.empty();
        }
        bool HasPending() const
        {
            static_assert(MULTI_WRITER, "Host side only.");
            return overflow.count.load(std::memory_order_acquire) != 0;
        }
        size_t PendingCount() const
        {
            static_assert(MULTI_WRITER, "Host side only.");
            return overflow.count.load(std::memory_order_acquire);
        }
        // Number of messages that found the ring full and were queued, since the last call.
        uint64_t TakeDeferredWrites()
        {
            static_assert(MULTI_WRITER, "Host side only.");
            return overflow.deferred.exchange(0, std::memory_order_relaxed);
        }
        // Empties the pending queue, passing each message to `release` (command, body, body size)
        // so the caller can free what it carries. Only call once the audio thread no longer runs.
        template <typename RELEASE>
        void ClearPending(RELEASE release)
        {
            static_assert(MULTI_WRITER, "Host side only.");
            std::deque<std::vector<uint8_t>> messages;
            {
                std::lock_guard lock(overflow.mutex);
                messages.swap(overflow.messages);
                overflow.count.store(0, std::memory_order_release);
            }
            // outside the lock: `release` may write to this writer again.
            for (const auto &message : messages)
            {
                RingBufferCommand command;
                memcpy(&command, message.data(), sizeof(command));
                release(command, message.data() + sizeof(command), message.size() - sizeof(command));
            }
        }

        // Realtime side (single writer) only.
        //
        // Messages that hand an object back to the host for deletion (EffectReplaced,
        // FreeVuSubscriptions, FreeMonitorPortSubscription, FreeSnapshot) or complete parameter
        // requests (ParameterRequestComplete) are not dropped when the ring is full: they wait in a
        // fixed-size array (no allocation) and are retried, in order, by RetryReleases() (called by
        // the audio thread every cycle) and before each later release.
        //
        // Releases use at most MAX_DEFERRED_RELEASES - 1 entries. If those are full, the release is
        // counted in TakeLostReleases(); the host reclaims pedalboards, VU and monitor subscriptions
        // released in order (a release of object N implies all earlier ones were released), and
        // everything else on Close(). Request completions are never lost: a completion that has to
        // wait is appended to the request chain of a completion that is already waiting, or takes
        // the last entry, which is kept for them. Close() completes requests still waiting here
        // (TakeDeferredReleases()) or in the ring.
        static constexpr size_t MAX_DEFERRED_RELEASES = 64;

        void RetryReleases()
        {
            static_assert(!MULTI_WRITER, "Realtime side only.");
            size_t done = 0;
            while (done < overflow.count && TryWriteRelease_(overflow.entries[done].command, overflow.entries[done].pointer))
            {
                ++done;
            }
            if (done != 0)
            {
                for (size_t i = done; i < overflow.count; ++i)
                {
                    overflow.entries[i - done] = overflow.entries[i];
                }
                overflow.count -= done;
            }
        }
        size_t DeferredReleaseCount() const
        {
            static_assert(!MULTI_WRITER, "Realtime side only.");
            return overflow.count;
        }
        uint64_t TakeLostReleases()
        {
            static_assert(!MULTI_WRITER, "Realtime side only.");
            return overflow.lost.exchange(0, std::memory_order_relaxed);
        }
        // Empties the deferred array, passing each entry to `fn` (command, pointer), oldest first.
        // Only call while the audio thread does not run (e.g. from Close(), so that the parameter
        // requests waiting here are completed).
        template <typename FN>
        void TakeDeferredReleases(FN fn)
        {
            static_assert(!MULTI_WRITER, "Realtime side only.");
            size_t count = overflow.count;
            overflow.count = 0;
            for (size_t i = 0; i < count; ++i)
            {
                fn(overflow.entries[i].command, overflow.entries[i].pointer);
            }
        }

        // Host->audio messages that must never be dropped: they carry an object the audio thread
        // takes ownership of (or a request that must complete), or an acknowledgement that the
        // audio thread waits for before sending more.
        static bool IsMustDeliverCommand(RingBufferCommand command)
        {
            switch (command)
            {
            case RingBufferCommand::ReplaceEffect:
            case RingBufferCommand::SetVuSubscriptions:
            case RingBufferCommand::SetMonitorPortSubscription:
            case RingBufferCommand::LoadSnapshot:
            case RingBufferCommand::ParameterRequest:
            case RingBufferCommand::AckVuUpdate:
            case RingBufferCommand::AckMonitorPortUpdate:
            case RingBufferCommand::AckMidiProgramChange:
            case RingBufferCommand::AckMidiSnapshotRequest:
            case RingBufferCommand::SetBypass:
            case RingBufferCommand::SetSuspendBypassedPlugins:
            case RingBufferCommand::SetInputVolume:
            case RingBufferCommand::SetOutputVolume:
                return true;
            default:
                return false;
            }
        }

    private:
        std::atomic<uint64_t> droppedWrites{0};

        struct HostPendingQueue
        {
            std::mutex mutex;
            std::deque<std::vector<uint8_t>> messages; // command + body, as written to the ring.
            std::atomic<size_t> count{0};              // messages.size(), readable without the mutex.
            std::atomic<uint64_t> deferred{0};
            std::function<void()> onPendingStarted;
        };
        struct RealtimeReleaseQueue
        {
            struct Entry
            {
                RingBufferCommand command;
                void *pointer;
            };
            Entry entries[MAX_DEFERRED_RELEASES];
            size_t count = 0;
            std::atomic<uint64_t> lost{0};
            // Last request of the waiting ParameterRequestComplete chain (at most one waits); valid
            // while that entry is in `entries`.
            RealtimePatchPropertyRequest *completionTail = nullptr;
        };
        std::conditional_t<MULTI_WRITER, HostPendingQueue, RealtimeReleaseQueue> overflow;

        void OnWriteFailed()
        {
            // The single-writer flavour is the audio thread's writer (RealtimeRingBufferWriter):
            // no logging there, just count the drop.
            // Multi-writer drops are reported periodically by rtsvc (LogRealtimeStatistics), not per message.
            droppedWrites.fetch_add(1, std::memory_order_relaxed);
        }

        // Host side, overflow.mutex held.
        void FlushPending_()
        {
            while (!overflow.messages.empty())
            {
                auto &message = overflow.messages.front();
                if (!ringBuffer->write(message.size(), message.data()))
                {
                    break;
                }
                overflow.messages.pop_front();
                overflow.count.store(overflow.messages.size(), std::memory_order_release);
            }
        }

        // Host side, overflow.mutex held, queue at MAX_PENDING_MESSAGES. Removes the oldest queued
        // SetValue for the same control since the last queued ReplaceEffect (earlier values use
        // another pedalboard's effect indices) and appends `message`. Returns false if there is none.
        bool CoalesceControlValue_(const std::vector<uint8_t> &message)
        {
            RingBufferCommand command;
            memcpy(&command, message.data(), sizeof(command));
            if (command != RingBufferCommand::SetValue || message.size() < sizeof(command) + sizeof(SetControlValueBody))
            {
                return false;
            }
            SetControlValueBody body;
            memcpy(&body, message.data() + sizeof(command), sizeof(body));
            auto oldest = overflow.messages.end();
            for (auto it = overflow.messages.end(); it != overflow.messages.begin();)
            {
                --it;
                RingBufferCommand queuedCommand;
                memcpy(&queuedCommand, it->data(), sizeof(queuedCommand));
                if (queuedCommand == RingBufferCommand::ReplaceEffect)
                {
                    break;
                }
                if (queuedCommand == RingBufferCommand::SetValue && it->size() == message.size())
                {
                    SetControlValueBody queuedBody;
                    memcpy(&queuedBody, it->data() + sizeof(queuedCommand), sizeof(queuedBody));
                    if (queuedBody.effectIndex == body.effectIndex && queuedBody.controlIndex == body.controlIndex)
                    {
                        oldest = it;
                    }
                }
            }
            if (oldest == overflow.messages.end())
            {
                return false;
            }
            // The control still ends up with the latest value, after every older one.
            overflow.messages.erase(oldest);
            overflow.messages.push_back(message);
            return true;
        }

        bool WriteMessage_(RingBufferCommand command, const RingBufferSegment *segments, size_t count)
        {
            if constexpr (MULTI_WRITER)
            {
                bool pendingStarted = false;
                {
                    std::lock_guard lock(overflow.mutex);
                    FlushPending_();
                    if (overflow.messages.empty() && ringBuffer->writeSegments(segments, count))
                    {
                        return true;
                    }
                    size_t total = 0;
                    for (size_t i = 0; i < count; ++i)
                    {
                        total += segments[i].size;
                    }
                    std::vector<uint8_t> message(total);
                    size_t offset = 0;
                    for (size_t i = 0; i < count; ++i)
                    {
                        if (segments[i].size != 0)
                        {
                            memcpy(message.data() + offset, segments[i].data, segments[i].size);
                        }
                        offset += segments[i].size;
                    }
                    if (overflow.messages.size() >= MAX_PENDING_MESSAGES && !IsMustDeliverCommand(command))
                    {
                        droppedWrites.fetch_add(1, std::memory_order_relaxed); // an older value is superseded, or this one.
                        return CoalesceControlValue_(message);
                    }
                    pendingStarted = overflow.messages.empty();
                    overflow.messages.push_back(std::move(message));
                    overflow.count.store(overflow.messages.size(), std::memory_order_release);
                    overflow.deferred.fetch_add(1, std::memory_order_relaxed);
                }
                if (pendingStarted && overflow.onPendingStarted)
                {
                    overflow.onPendingStarted();
                }
                return true;
            }
            else
            {
                (void)command;
                if (!ringBuffer->writeSegments(segments, count))
                {
                    OnWriteFailed();
                    return false;
                }
                return true;
            }
        }

        // Realtime side. Does not count a drop.
        bool TryWriteRelease_(RingBufferCommand command, void *pointer)
        {
            CommandBuffer<void *> buffer(command, pointer);
            return ringBuffer->write(buffer.size(), (uint8_t *)&buffer);
        }

        // Writes a release message (see MAX_DEFERRED_RELEASES). Realtime-safe on the realtime side.
        bool WriteRelease_(RingBufferCommand command, void *pointer)
        {
            if constexpr (MULTI_WRITER)
            {
                return write(command, pointer);
            }
            else
            {
                RetryReleases();
                if (overflow.count == 0 && TryWriteRelease_(command, pointer))
                {
                    return true;
                }
                if (overflow.count < MAX_DEFERRED_RELEASES - 1) // the last entry is kept for request completions.
                {
                    overflow.entries[overflow.count++] = {command, pointer};
                    return true;
                }
                overflow.lost.fetch_add(1, std::memory_order_relaxed); // reported separately from droppedWrites.
                return false;
            }
        }

        // Realtime side: writes a ParameterRequestComplete, which must never be lost (see
        // MAX_DEFERRED_RELEASES). Allocation-free.
        void WriteRequestCompletion_(RealtimePatchPropertyRequest *pRequest)
        {
            RetryReleases();
            if (overflow.count == 0 && TryWriteRelease_(RingBufferCommand::ParameterRequestComplete, pRequest))
            {
                return;
            }
            // The tail of this chain: only walks this cycle's requests (the waiting chain's tail is kept),
            // which the audio thread has just walked anyway to serve them.
            RealtimePatchPropertyRequest *pTail = pRequest;
            while (pTail->pNext != nullptr)
            {
                pTail = pTail->pNext;
            }
            for (size_t i = 0; i < overflow.count; ++i)
            {
                if (overflow.entries[i].command == RingBufferCommand::ParameterRequestComplete)
                {
                    // Join the waiting chain: the host completes both chains from one message.
                    overflow.completionTail->pNext = pRequest;
                    overflow.completionTail = pTail;
                    return;
                }
            }
            // No completion is waiting, so the last entry is free (releases leave it unused).
            overflow.entries[overflow.count++] = {RingBufferCommand::ParameterRequestComplete, pRequest};
            overflow.completionTail = pTail;
        }

    public:
        // Returns false (and counts the drop) if the message was dropped. On the host side a
        // message that finds the ring full is queued instead (see MAX_PENDING_MESSAGES), and
        // true is returned.
        template <typename T>
        bool write(RingBufferCommand command, const T &value)
        {

            // the goal: to atomically write the command and associated data.
            CommandBuffer<T> buffer(command, value);
            RingBufferSegment segment{buffer.size(), &buffer};
            return WriteMessage_(command, &segment, 1);
        }

        // One message: command, value, (size_t)dataLength, variableData[dataLength].
        template <typename T>
        bool write(RingBufferCommand command, const T &value, size_t dataLength, uint8_t *variableData)
        {

            // the goal: to atomically write the command and associated data.
            CommandBuffer<T> buffer(command, value);
            RingBufferSegment segments[3]{
                {buffer.size(), &buffer},
                {sizeof(dataLength), &dataLength},
                {dataLength, variableData}};
            return WriteMessage_(command, segments, 3);
        }

        // Realtime side: defer reader wake-ups to the end of the audio cycle (at most one post
        // per cycle; see RingBuffer). Use RealtimeWakeBatch rather than calling these directly.
        void BeginWakeBatch() { ringBuffer->beginWakeBatch(); }
        void EndWakeBatch() { ringBuffer->endWakeBatch(); }

        void Lv2StateChanged(uint64_t instanceId)
        {
            write(RingBufferCommand::Lv2StateChanged, instanceId);
        }
        void MaybeLv2StateChanged(uint64_t instanceId)
        {
            write(RingBufferCommand::MaybeLv2StateChanged, instanceId);
        }
        void AtomOutput(uint64_t instanceId, IEffect *sourceEffect, size_t bytes, uint8_t *data)
        {
            AtomOutputBody body{instanceId, sourceEffect};
            write(RingBufferCommand::AtomOutput, body, bytes, data);
        }
        void AtomOutput(uint64_t instanceId, IEffect *sourceEffect, const LV2_Atom *atom)
        {
            AtomOutput(instanceId, sourceEffect, atom->size + sizeof(LV2_Atom), (uint8_t *)atom);
        }
        void ParameterRequest(RealtimePatchPropertyRequest *pRequest)
        {
            write(RingBufferCommand::ParameterRequest, pRequest);
        }
        // Completes a chain of requests (linked through pNext). Never dropped on the realtime side.
        void ParameterRequestComplete(RealtimePatchPropertyRequest *pRequest)
        {
            if constexpr (MULTI_WRITER)
            {
                write(RingBufferCommand::ParameterRequestComplete, pRequest);
            }
            else
            {
                WriteRequestCompletion_(pRequest);
            }
        }
        void MidiValueChanged(int64_t instanceId, int controlIndex, float value, IEffect *sourceEffect, Lv2Pedalboard *sourcePedalboard)
        {
            MidiValueChangedBody body;
            body.instanceId = instanceId;
            body.controlIndex = controlIndex;
            body.value = value;
            body.sourceEffect = sourceEffect;
            body.sourcePedalboard = sourcePedalboard;
            write(RingBufferCommand::MidiValueChanged, body);
        }

        void OnMidiListen(const MidiNotifyBody &body)
        {
            write(RingBufferCommand::OnMidiListen, body);
        }

        /**
         * @brief Notify host of a midi program change request.
         *
         * @param bank MIDI bank number, or -1 for current bank.
         * @param program MIDI program number.
         */
        void OnMidiProgramChange(int64_t _requestId, int8_t _bank, uint8_t _program)
        {
            RealtimeMidiProgramRequest msg{requestId : _requestId, bank : _bank, program : _program};

            write(RingBufferCommand::MidiProgramChange, msg);
        }
        void OnRealtimeMidiEvent(RealtimeMidiEventType eventType)
        {
            RealtimeMidiEventRequest msg{eventType};
            write(RingBufferCommand::RealtimeMidiEvent, msg);
        }
        void OnRealtimeMidiSnapshotRequest(int32_t snapshotIndex, int64_t snapshotRequestId)
        {
            RealtimeMidiSnapshotRequest msg{snapshotIndex, snapshotRequestId};

            write(RingBufferCommand::RealtimeMidiSnapshotRequest, msg);
        }

        void OnNextMidiProgram(int64_t requestId, int32_t direction)
        {
            RealtimeNextMidiProgramRequest msg{requestId : requestId, direction : direction};
            write(RingBufferCommand::NextMidiProgram, msg);
        }
        void OnNextMidiBank(int64_t requestId, int32_t direction)
        {
            RealtimeNextMidiProgramRequest msg{requestId : requestId, direction : direction};
            write(RingBufferCommand::NextMidiBank, msg);
        }
        void OnNextMidiSnapshot(int64_t requestId, int32_t direction)
        {
            RealtimeNextMidiProgramRequest msg{requestId : requestId, direction : direction};
            write(RingBufferCommand::NextMidiSnapshot, msg);
        }

        void SetControlValue(int effectIndex, int controlIndex, float value)
        {
            SetControlValueBody body;
            body.effectIndex = effectIndex;
            body.controlIndex = controlIndex;
            body.value = value;
            write(RingBufferCommand::SetValue, body);
        }
        void SetInputVolume(float value)
        {
            SetVolumeBody body;
            body.value = value;
            write(RingBufferCommand::SetInputVolume, body);
        }
        void SetOutputVolume(float value)
        {
            SetVolumeBody body;
            body.value = value;
            write(RingBufferCommand::SetOutputVolume, body);
        }

        void FreeVuSubscriptions(RealtimeVuBuffers *configuration)
        {
            WriteRelease_(RingBufferCommand::FreeVuSubscriptions, configuration);
        }
        void FreeMonitorPortSubscriptions(RealtimeMonitorPortSubscriptions *subscriptions)
        {
            WriteRelease_(RingBufferCommand::FreeMonitorPortSubscription, subscriptions);
        }

        void SendMonitorPortUpdate(
            PortMonitorCallback *callback,
            int64_t subscriptionHandle,
            float value)
        {
            MonitorPortUpdate body{callback, subscriptionHandle, value};
            write(RingBufferCommand::SendMonitorPortUpdate, body);
        }

        void SendVuUpdate(const std::vector<VuUpdateX> *pUpdates)
        {
            write(RingBufferCommand::SendVuUpdate, pUpdates);
        }
        void AckVuUpdate()
        {
            bool value = true;
            write(RingBufferCommand::AckVuUpdate, value);
        }
        void AckMonitorPortUpdate(int64_t subscriptionHandle)
        {
            // we assume no padding between the command and the data, so we can do an atomic write.

            write(RingBufferCommand::AckMonitorPortUpdate, subscriptionHandle);
        }
        void SetVuSubscriptions(RealtimeVuBuffers *configuration)
        {
            write(RingBufferCommand::SetVuSubscriptions, configuration);
        }
        void LoadSnapshot(IndexedSnapshot *snapshot)
        {
            write(RingBufferCommand::LoadSnapshot, snapshot);
        }

        void AckMidiProgramRequest(int64_t requestId)
        {
            write(RingBufferCommand::AckMidiProgramChange, requestId);
        }
        void AckMidiSnapshotRequest(uint64_t snapshotRequestId)
        {
            write(RingBufferCommand::AckMidiSnapshotRequest, snapshotRequestId);
        }

        void SetMonitorPortSubscriptions(RealtimeMonitorPortSubscriptions *subscriptions)
        {

            write(RingBufferCommand::SetMonitorPortSubscription, subscriptions);
        }

        void SetBypass(int effectIndex, bool enabled)
        {

            SetBypassBody body;
            body.effectIndex = effectIndex;
            body.enabled = enabled;
            write(RingBufferCommand::SetBypass, body);
        }

        void SetSuspendBypassedPlugins(bool value)
        {
            SetSuspendBypassedPluginsBody body;
            body.value = value;
            write(RingBufferCommand::SetSuspendBypassedPlugins, body);
        }

        void ReplaceEffect(Lv2Pedalboard *pedalboard)
        {
            write(RingBufferCommand::ReplaceEffect, pedalboard);
        }

        void EffectReplaced(Lv2Pedalboard *pedalboard)
        {
            WriteRelease_(RingBufferCommand::EffectReplaced, pedalboard);
        }



        void AudioTerminatedAbnormally()
        {
            AudioStoppedBody body;
            write(RingBufferCommand::AudioTerminatedAbnormally, body);
        }
        bool AlsaRestartRequested()
        {
            int64_t unused = 0;
            return write(RingBufferCommand::AlsaRestartRequested, unused);
        }


        void FreeSnapshot(IndexedSnapshot *snapshot)
        {
            WriteRelease_(RingBufferCommand::FreeSnapshot, snapshot);
        }

        void WriteLv2ErrorMessage(int64_t instanceId, const char *message)
        {
            size_t length = strlen(message);
            write(RingBufferCommand::Lv2ErrorMessage, instanceId, length, (uint8_t *)message);
        }
        void SendPathPropertyBuffer(PatchPropertyWriter::Buffer *buffer)
        {
            write(RingBufferCommand::SendPathPropertyBuffer, buffer);
        }
    };

    typedef RingBufferReader<true, false> RealtimeRingBufferReader;
    typedef RingBufferReader<false, true> HostRingBufferReader;
    typedef RingBufferWriter<true, false> HostRingBufferWriter;

    // cures a forward-declaration problem.
    class RealtimeRingBufferWriter : public RingBufferWriter<false, true>
    {
    public:
        RealtimeRingBufferWriter()
        {
        }
        RealtimeRingBufferWriter(RingBuffer<false, true> *ringBuffer)
            : RingBufferWriter<false, true>(ringBuffer)
        {
        }
    };

    // Host side, once the reader thread has stopped (Close()): reads every message left in the
    // audio->host ring, passing each ParameterRequestComplete chain to `complete`, so that no
    // request whose completion was already written is left uncompleted. Everything else is
    // discarded: the objects those messages hand back are reclaimed by Close() through its own
    // lists. Body layouts match the writers above. Stops at an unknown command (the ring is reset
    // afterwards anyway). Returns the number of messages read.
    template <typename COMPLETE>
    size_t DrainRealtimeOutputRing(HostRingBufferReader &reader, COMPLETE complete)
    {
        size_t messages = 0;
        std::vector<uint8_t> body;
        while (reader.readSpace() > sizeof(RingBufferCommand))
        {
            RingBufferCommand command;
            reader.read(&command);
            size_t bodySize = 0;
            bool variableData = false;
            switch (command)
            {
            case RingBufferCommand::ParameterRequestComplete:
            {
                RealtimePatchPropertyRequest *pRequest = nullptr;
                reader.read(&pRequest);
                ++messages;
                complete(pRequest);
                continue;
            }
            case RingBufferCommand::EffectReplaced:
            case RingBufferCommand::FreeVuSubscriptions:
            case RingBufferCommand::FreeMonitorPortSubscription:
            case RingBufferCommand::FreeSnapshot:
            case RingBufferCommand::SendVuUpdate:
            case RingBufferCommand::SendPathPropertyBuffer:
                bodySize = sizeof(void *);
                break;
            case RingBufferCommand::MidiValueChanged:
                bodySize = sizeof(MidiValueChangedBody);
                break;
            case RingBufferCommand::OnMidiListen:
                bodySize = sizeof(MidiNotifyBody);
                break;
            case RingBufferCommand::AtomOutput:
                bodySize = sizeof(AtomOutputBody);
                variableData = true;
                break;
            case RingBufferCommand::Lv2ErrorMessage:
                bodySize = sizeof(int64_t);
                variableData = true;
                break;
            case RingBufferCommand::Lv2StateChanged:
            case RingBufferCommand::MaybeLv2StateChanged:
                bodySize = sizeof(uint64_t);
                break;
            case RingBufferCommand::AlsaRestartRequested:
                bodySize = sizeof(int64_t);
                break;
            case RingBufferCommand::AudioTerminatedAbnormally:
                bodySize = sizeof(AudioStoppedBody);
                break;
            case RingBufferCommand::SendMonitorPortUpdate:
                bodySize = sizeof(MonitorPortUpdate);
                break;
            case RingBufferCommand::MidiProgramChange:
                bodySize = sizeof(RealtimeMidiProgramRequest);
                break;
            case RingBufferCommand::NextMidiProgram:
            case RingBufferCommand::NextMidiBank:
            case RingBufferCommand::NextMidiSnapshot:
                bodySize = sizeof(RealtimeNextMidiProgramRequest);
                break;
            case RingBufferCommand::RealtimeMidiEvent:
                bodySize = sizeof(RealtimeMidiEventRequest);
                break;
            case RingBufferCommand::RealtimeMidiSnapshotRequest:
                bodySize = sizeof(RealtimeMidiSnapshotRequest);
                break;
            default:
                return messages;
            }
            if (reader.readSpace() < bodySize)
            {
                return messages;
            }
            body.resize(bodySize);
            reader.read(bodySize, body.data());
            if (variableData)
            {
                size_t dataLength = 0;
                if (reader.readSpace() < sizeof(dataLength))
                {
                    return messages;
                }
                reader.read(&dataLength);
                if (dataLength != 0)
                {
                    if (reader.readSpace() < dataLength)
                    {
                        return messages;
                    }
                    body.resize(dataLength);
                    reader.read(dataLength, body.data());
                }
            }
            ++messages;
        }
        return messages;
    }

    // Host-side bookkeeping for objects handed to the audio thread one after another, each one
    // replacing the previous (VU configurations, monitor port subscriptions). The audio thread
    // releases them in the order they were sent, so the release of one implies that every
    // earlier one has been released too: a release message for an earlier object that was lost,
    // or is still in flight, is covered by the later one. Not thread-safe; guard externally.
    template <typename T>
    class InOrderReleaseList
    {
    public:
        InOrderReleaseList() = default;
        InOrderReleaseList(const InOrderReleaseList &) = delete;
        InOrderReleaseList &operator=(const InOrderReleaseList &) = delete;
        ~InOrderReleaseList() { DeleteAll(); }

        void Add(T *item)
        {
            if (item != nullptr)
            {
                items.push_back(item);
            }
        }
        // Deletes `item` and everything added before it. Returns the number deleted: 0 if `item`
        // is unknown (already reclaimed by a later release).
        size_t Release(T *item)
        {
            for (size_t i = 0; i < items.size(); ++i)
            {
                if (items[i] == item)
                {
                    for (size_t j = 0; j <= i; ++j)
                    {
                        delete items[j];
                    }
                    items.erase(items.begin(), items.begin() + i + 1);
                    return i + 1;
                }
            }
            return 0;
        }
        void DeleteAll()
        {
            for (T *item : items)
            {
                delete item;
            }
            items.clear();
        }
        size_t size() const { return items.size(); }

    private:
        std::vector<T *> items;
    };

    // Audio thread: brackets one audio cycle, so that the host reader is woken at most once
    // per cycle however many messages the cycle writes. Ends the batch on unwind too.
    // Inside a batch, the reader is not woken until the batch ends: never wait inside one for
    // the reader to act on a message (e.g. AlsaRestartRequested, which is written outside OnProcess).
    class RealtimeWakeBatch
    {
    public:
        RealtimeWakeBatch(RealtimeRingBufferWriter &writer)
            : writer(writer)
        {
            writer.BeginWakeBatch();
        }
        ~RealtimeWakeBatch()
        {
            writer.EndWakeBatch();
        }
        RealtimeWakeBatch(const RealtimeWakeBatch &) = delete;
        RealtimeWakeBatch &operator=(const RealtimeWakeBatch &) = delete;

    private:
        RealtimeRingBufferWriter &writer;
    };

} // namespace