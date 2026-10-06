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

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

namespace pipedal
{

    /**
     * @brief Builds expensive objects on a dedicated (non-realtime) thread, installing only the latest one.
     *
     * Used by PiPedalModel to instantiate Lv2Pedalboards (plugin instantiation, NAM model / IR loading)
     * without holding the model mutex.
     *
     * Threading contract:
     *
     * - Submit() records a request and returns its generation number. Each Submit()/Invalidate()
     *   bumps the generation, so earlier generations become stale. At most one request is pending at
     *   any time: a request that has not started building yet is simply replaced (coalescing).
     *
     * - The builder thread calls build(request) with no builder lock held, then calls
     *   install(generation, request, result) with no builder lock held, for every successful build,
     *   including stale ones (so that the installer can finish any bookkeeping for them). The installer typically takes the owner's
     *   own lock and checks IsCurrent(generation) under that lock to decide whether to install, because
     *   a newer Submit()/Invalidate() can happen at any time before that. Results the installer does not
     *   keep are destroyed on the builder thread.
     *
     * - Close() discards any pending request, waits for an in-flight build and its install call to
     *   finish, and joins the thread. IsCurrent() returns false once Close() has started. After Close()
     *   returns, install is never called again. Close() must NOT be called while holding a lock that the
     *   installer takes (deadlock), and must not be called from the install callback.
     *
     * - Submit(), Invalidate(), IsCurrent() and IsIdle() only take the builder's internal mutex briefly,
     *   so they may be called while holding the owner's lock.
     */
    template <typename REQUEST, typename RESULT>
    class LatestOnlyBuilder
    {
    public:
        using BuildFunction = std::function<RESULT(REQUEST &request)>;
        using InstallFunction = std::function<void(uint64_t generation, REQUEST &request, RESULT &result)>;
        using ErrorFunction = std::function<void(const std::exception &e)>;

        LatestOnlyBuilder(BuildFunction build, InstallFunction install, ErrorFunction onError = nullptr)
            : build(std::move(build)), install(std::move(install)), onError(std::move(onError))
        {
        }
        ~LatestOnlyBuilder()
        {
            Close();
        }
        LatestOnlyBuilder(const LatestOnlyBuilder &) = delete;
        LatestOnlyBuilder &operator=(const LatestOnlyBuilder &) = delete;

        // returns the generation of the request, or 0 if the builder is closed.
        uint64_t Submit(REQUEST &&request)
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (closed)
            {
                return 0;
            }
            uint64_t generation = ++currentGeneration;
            pending.reset(); // destroy any superseded request before replacing it.
            pending.emplace(std::move(request));
            pendingGeneration = generation;
            if (!thread.joinable())
            {
                thread = std::thread([this]()
                                     { ThreadProc(); });
            }
            cv.notify_all();
            return generation;
        }

        // Make all outstanding requests stale, and discard the pending one.
        void Invalidate()
        {
            std::optional<REQUEST> discarded;
            {
                std::lock_guard<std::mutex> lock(mutex);
                ++currentGeneration;
                discarded = std::move(pending);
                pending.reset();
            }
        }

        bool IsCurrent(uint64_t generation) const
        {
            std::lock_guard<std::mutex> lock(mutex);
            return !closed && generation == currentGeneration;
        }
        uint64_t CurrentGeneration() const
        {
            std::lock_guard<std::mutex> lock(mutex);
            return currentGeneration;
        }

        // Optionally called by the installer (from inside install(), typically under the owner's lock)
        // once it has installed or rejected the result, so that IsIdle() is accurate immediately rather
        // than only after install() returns and the builder thread re-acquires its lock.
        void InstallCompleted()
        {
            std::lock_guard<std::mutex> lock(mutex);
            busy = false;
            if (!pending.has_value())
            {
                idleCv.notify_all();
            }
        }

        // true if no request is pending, building, or installing.
        bool IsIdle() const
        {
            std::lock_guard<std::mutex> lock(mutex);
            return !pending.has_value() && !busy;
        }

        // For tests and orderly shutdown. Returns true if the builder became idle before the timeout.
        template <typename Rep, typename Period>
        bool WaitForIdle(const std::chrono::duration<Rep, Period> &timeout)
        {
            std::unique_lock<std::mutex> lock(mutex);
            return idleCv.wait_for(lock, timeout, [this]()
                                   { return !pending.has_value() && !busy; });
        }

        void Close()
        {
            std::optional<REQUEST> discarded;
            std::thread t;
            {
                std::lock_guard<std::mutex> lock(mutex);
                closed = true;
                discarded = std::move(pending);
                pending.reset();
                t = std::move(thread);
                cv.notify_all();
            }
            if (t.joinable())
            {
                if (t.get_id() == std::this_thread::get_id())
                {
                    t.detach(); // not supported, but don't std::terminate.
                }
                else
                {
                    t.join();
                }
            }
        }

    private:
        void ThreadProc()
        {
            std::unique_lock<std::mutex> lock(mutex);
            while (true)
            {
                cv.wait(lock, [this]()
                        { return closed || pending.has_value(); });
                if (closed)
                {
                    break;
                }
                uint64_t generation = pendingGeneration;
                std::optional<REQUEST> request = std::move(pending);
                pending.reset();
                busy = true;
                lock.unlock();
                {
                    std::optional<RESULT> result;
                    try
                    {
                        result.emplace(build(*request));
                        install(generation, *request, *result);
                    }
                    catch (const std::exception &e)
                    {
                        if (onError)
                        {
                            onError(e);
                        }
                    }
                    catch (...)
                    {
                        if (onError)
                        {
                            onError(std::runtime_error("Unknown error."));
                        }
                    }
                    // stale (or installed) results and requests are released here, on the builder thread.
                    result.reset();
                    request.reset();
                }
                lock.lock();
                busy = false;
                idleCv.notify_all();
            }
            busy = false;
            idleCv.notify_all();
        }

        BuildFunction build;
        InstallFunction install;
        ErrorFunction onError;

        mutable std::mutex mutex;
        std::condition_variable cv;
        std::condition_variable idleCv;
        std::thread thread;
        bool closed = false;
        bool busy = false;
        uint64_t currentGeneration = 0;
        uint64_t pendingGeneration = 0;
        std::optional<REQUEST> pending;
    };

    /**
     * @brief Decides whether a pedalboard build may borrow effect instances from the running pedalboard.
     *
     * Borrowing (PluginHost::UpdateLv2PedalboardStructure) matches effects by instance id, which is only
     * meaningful if the running pedalboard belongs to the same lineage as the requested one. That is not
     * the case while a full build (preset/bank load, audio restart) is outstanding: the running pedalboard
     * is then an older, unrelated pedalboard. So a reuse request is downgraded to a full build until the
     * outstanding full build has been installed.
     *
     * Not thread-safe: all calls must be made under the owner's lock.
     */
    class PedalboardBuildModeTracker
    {
    public:
        // returns true if the build may reuse existing effects.
        bool ResolveReuse(bool reuseRequested)
        {
            if (!reuseRequested || fullBuildOutstanding)
            {
                fullBuildOutstanding = true;
                return false;
            }
            return true;
        }
        // A (current) pedalboard was installed. If it was a full build, the running pedalboard is now of the
        // current lineage.
        void OnInstalled(bool wasReuseBuild)
        {
            if (!wasReuseBuild)
            {
                fullBuildOutstanding = false;
            }
            runningPedalboardIntact = true;
        }
        // The running pedalboard was torn down (e.g. audio restart), or a build failed after possibly modifying
        // its effects. The next build must be a full build, and must not reuse its instances.
        void OnRunningPedalboardDiscarded()
        {
            fullBuildOutstanding = true;
            runningPedalboardIntact = false;
        }
        bool FullBuildOutstanding() const { return fullBuildOutstanding; }

        // Whether a full build (e.g. a preset switch) may reuse matching instances of the running pedalboard
        // (MatchReusableInstances in PresetInstanceReuse.hpp). Unlike borrowing by instance id, this doesn't
        // depend on the lineage, only on the running pedalboard being a completely built, installed one.
        bool CanReuseRunningInstances() const { return runningPedalboardIntact; }

    private:
        bool fullBuildOutstanding = true; // nothing installed yet.
        bool runningPedalboardIntact = false; // nothing installed yet.
    };

    /**
     * @brief Whether a built pedalboard may be installed. Pure function; see PiPedalModel::TryInstallBuiltPedalboard.
     */
    struct PedalboardInstallState
    {
        bool reuseBuild = false;           // the build borrowed effects from the running pedalboard.
        bool isCurrent = false;            // the build's generation is the latest one.
        bool audioRestarting = false;      // RestartAudio() is between its start and requesting a fresh build.
        uint64_t buildAudioEpoch = 0;      // audio epoch when the build was requested (full) or borrowed (reuse).
        uint64_t currentAudioEpoch = 0;
        uint64_t buildConfigurationVersion = 0; // plugin host configuration version used by the build.
        uint64_t currentConfigurationVersion = 0;
    };
    inline bool ShouldInstallBuiltPedalboard(const PedalboardInstallState &state)
    {
        // The same rules apply to builds that borrowed effects of the running pedalboard: borrowing only stages
        // its changes (buffers, instance ids, settings) in the new pedalboard until it is swapped in on the audio
        // thread, so a discarded borrowing build leaves the running effects intact, and the newer build that
        // superseded it borrows them again. (state.reuseBuild only selects which epoch buildAudioEpoch holds.)
        return state.isCurrent &&
               !state.audioRestarting &&
               state.buildAudioEpoch == state.currentAudioEpoch &&
               state.buildConfigurationVersion == state.currentConfigurationVersion;
    }
}
