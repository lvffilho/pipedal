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

#include <cstdint>
#include <vector>

namespace pipedal
{
    /**
     * @brief Holds back acknowledgements of realtime MIDI program/snapshot requests until the pedalboard
     * built for them is running.
     *
     * The audio thread defers MIDI messages (DeferredMidiBuffer) from a program/snapshot request until the
     * request is acknowledged, and then replays them against the running pedalboard. Since pedalboards are
     * built asynchronously, an ack sent as soon as the request is handled would reach the audio thread
     * before the new pedalboard (ReplaceEffect), and the deferred messages would be applied to the outgoing
     * pedalboard. So while a pedalboard build is outstanding, acks are recorded here, and sent when the build
     * is installed (after audioHost->SetPedalboard(), so they follow the ReplaceEffect in the host->realtime
     * ring), or when the builder ends up idle by any other path (stale discard, failure), or unconditionally
     * on audio restart / close, so that the audio thread never defers forever.
     *
     * Duplicate or late acks are harmless: the audio thread only clears its pending flag for the id of its
     * latest request.
     *
     * Not thread-safe: all calls are made under PiPedalModel::mutex.
     */
    class PendingMidiAckTracker
    {
    public:
        enum class Kind
        {
            Program,  // AckMidiProgramRequest (program, bank, next/prev preset/bank/snapshot)
            Snapshot, // AckSnapshotRequest (direct snapshot selection)
        };
        struct Ack
        {
            Kind kind;
            int64_t requestId;
            uint64_t generation; // the builder generation that must be installed before the ack is sent.
        };

        // Called once the request has been handled. If no build is outstanding (no build was triggered,
        // or the request failed), returns true: ack immediately. Otherwise the ack is held until the
        // build of `currentGeneration` (or a later one) terminates, and returns false.
        bool Defer(Kind kind, int64_t requestId, bool buildOutstanding, uint64_t currentGeneration)
        {
            if (!buildOutstanding)
            {
                return true;
            }
            pending.push_back(Ack{kind, requestId, currentGeneration});
            return false;
        }

        // A build of `installedGeneration` was installed. Returns the acks it satisfies (send them after
        // audioHost->SetPedalboard()). If the builder is also idle, everything is released.
        std::vector<Ack> OnInstalled(uint64_t installedGeneration, bool builderIdle)
        {
            std::vector<Ack> result;
            std::vector<Ack> remaining;
            for (const Ack &ack : pending)
            {
                if (builderIdle || ack.generation <= installedGeneration)
                {
                    result.push_back(ack);
                }
                else
                {
                    remaining.push_back(ack);
                }
            }
            pending = std::move(remaining);
            return result;
        }

        // A build terminated without being installed (stale discard, failure). If a newer build is still
        // outstanding, its termination will release the acks; otherwise release them all now.
        std::vector<Ack> OnBuildTerminated(bool builderIdle)
        {
            if (!builderIdle)
            {
                return {};
            }
            return TakeAll();
        }

        // Audio restart, close: release everything.
        std::vector<Ack> TakeAll()
        {
            std::vector<Ack> result = std::move(pending);
            pending.clear();
            return result;
        }

        bool Empty() const { return pending.empty(); }
        size_t Size() const { return pending.size(); }

    private:
        std::vector<Ack> pending;
    };
}
