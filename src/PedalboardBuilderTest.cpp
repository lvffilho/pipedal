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
#include "PedalboardBuilder.hpp"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace pipedal;
using namespace std::chrono_literals;

namespace
{
    // A gate that build functions block on, so tests control when a build completes.
    class Gate
    {
    public:
        void Open()
        {
            std::lock_guard<std::mutex> lock(m);
            open = true;
            cv.notify_all();
        }
        void Wait()
        {
            std::unique_lock<std::mutex> lock(m);
            cv.wait(lock, [this]()
                    { return open; });
        }

    private:
        std::mutex m;
        std::condition_variable cv;
        bool open = false;
    };

    struct DestroyRecord
    {
        std::mutex m;
        std::vector<std::thread::id> threads;
    };

    // RESULT type that records the thread it was destroyed on.
    struct FakeResult
    {
        FakeResult(int value, std::shared_ptr<DestroyRecord> record) : value(value), record(record) {}
        FakeResult(FakeResult &&other) : value(other.value), record(std::move(other.record)) {}
        FakeResult(const FakeResult &) = delete;
        ~FakeResult()
        {
            if (record)
            {
                std::lock_guard<std::mutex> lock(record->m);
                record->threads.push_back(std::this_thread::get_id());
            }
        }
        int value;
        std::shared_ptr<DestroyRecord> record;
    };

    // Fake installer: the "model" side, with its own lock, that re-checks the generation under its lock.
    struct FakeModel
    {
        std::mutex mutex;
        std::vector<int> built;
        std::vector<int> installed;
        std::vector<std::string> errors;
    };

    template <typename T>
    std::vector<T> Locked(std::mutex &m, const std::vector<T> &v)
    {
        std::lock_guard<std::mutex> lock(m);
        return v;
    }

    bool WaitUntil(std::function<bool()> predicate, std::chrono::milliseconds timeout = 5000ms)
    {
        auto end = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < end)
        {
            if (predicate())
                return true;
            std::this_thread::sleep_for(1ms);
        }
        return predicate();
    }
}

TEST_CASE("PedalboardBuilder builds and installs a single request", "[pedalboard_builder]")
{
    FakeModel model;
    auto record = std::make_shared<DestroyRecord>();
    std::thread::id buildThread;

    using Builder = LatestOnlyBuilder<int, FakeResult>;
    Builder *pBuilder = nullptr;
    Builder builder(
        [&](int &request)
        {
            buildThread = std::this_thread::get_id();
            std::lock_guard<std::mutex> lock(model.mutex);
            model.built.push_back(request);
            return FakeResult(request * 10, record);
        },
        [&](uint64_t generation, int &request, FakeResult &result)
        {
            std::lock_guard<std::mutex> lock(model.mutex);
            if (!pBuilder->IsCurrent(generation))
                return;
            model.installed.push_back(result.value);
        });
    pBuilder = &builder;

    REQUIRE(builder.IsIdle());
    uint64_t generation = builder.Submit(7);
    REQUIRE(generation == 1);
    REQUIRE(builder.WaitForIdle(5s));
    REQUIRE(Locked(model.mutex, model.installed) == std::vector<int>{70});
    REQUIRE(buildThread != std::this_thread::get_id());
    {
        std::lock_guard<std::mutex> lock(record->m);
        REQUIRE(record->threads.size() >= 1);
        // the last destruction of the result happens on the builder thread, not the caller.
        REQUIRE(record->threads.back() == buildThread);
    }
    builder.Close();
}

TEST_CASE("PedalboardBuilder coalesces rapid requests and discards stale builds", "[pedalboard_builder]")
{
    FakeModel model;
    auto record = std::make_shared<DestroyRecord>();
    Gate firstBuildGate;
    std::atomic<bool> firstBuildStarted{false};
    std::thread::id buildThread;

    using Builder = LatestOnlyBuilder<int, FakeResult>;
    Builder *pBuilder = nullptr;
    Builder builder(
        [&](int &request)
        {
            buildThread = std::this_thread::get_id();
            {
                std::lock_guard<std::mutex> lock(model.mutex);
                model.built.push_back(request);
            }
            if (request == 1)
            {
                firstBuildStarted = true;
                firstBuildGate.Wait();
            }
            return FakeResult(request, record);
        },
        [&](uint64_t generation, int &request, FakeResult &result)
        {
            std::lock_guard<std::mutex> lock(model.mutex);
            if (!pBuilder->IsCurrent(generation))
                return;
            model.installed.push_back(result.value);
        });
    pBuilder = &builder;

    uint64_t g1 = builder.Submit(1);
    REQUIRE(WaitUntil([&]()
                      { return firstBuildStarted.load(); }));
    REQUIRE_FALSE(builder.IsIdle());

    // while request 1 is building, a burst of changes arrives (e.g. dragging through a preset list).
    uint64_t g2 = builder.Submit(2);
    uint64_t g3 = builder.Submit(3);
    uint64_t g4 = builder.Submit(4);
    REQUIRE(g1 < g2);
    REQUIRE(g2 < g3);
    REQUIRE(g3 < g4);
    REQUIRE_FALSE(builder.IsCurrent(g1));
    REQUIRE(builder.IsCurrent(g4));

    firstBuildGate.Open();
    REQUIRE(builder.WaitForIdle(5s));

    // 1 was built but is stale; 2 and 3 were superseded before they started; only 4 is installed.
    REQUIRE(Locked(model.mutex, model.built) == std::vector<int>{1, 4});
    REQUIRE(Locked(model.mutex, model.installed) == std::vector<int>{4});

    {
        // every built result (including the stale one) was released on the builder thread.
        std::lock_guard<std::mutex> lock(record->m);
        REQUIRE(!record->threads.empty());
        for (auto id : record->threads)
        {
            REQUIRE(id == buildThread);
        }
    }
    builder.Close();
}

TEST_CASE("PedalboardBuilder installer re-check discards a build superseded during install", "[pedalboard_builder]")
{
    // A newer request arrives after the build finished but before the installer got the model lock.
    FakeModel model;
    using Builder = LatestOnlyBuilder<int, int>;
    Builder *pBuilder = nullptr;
    Gate installGate;
    std::atomic<bool> installEntered{false};

    Builder builder(
        [&](int &request)
        { return request; },
        [&](uint64_t generation, int &request, int &result)
        {
            if (request == 1)
            {
                installEntered = true;
                installGate.Wait(); // simulate waiting for the model mutex.
            }
            std::lock_guard<std::mutex> lock(model.mutex);
            if (!pBuilder->IsCurrent(generation))
                return;
            model.installed.push_back(result);
        });
    pBuilder = &builder;

    builder.Submit(1);
    REQUIRE(WaitUntil([&]()
                      { return installEntered.load(); }));
    builder.Submit(2);
    installGate.Open();
    REQUIRE(builder.WaitForIdle(5s));
    REQUIRE(Locked(model.mutex, model.installed) == std::vector<int>{2});
}

TEST_CASE("PedalboardBuilder Invalidate discards pending and in-flight builds", "[pedalboard_builder]")
{
    FakeModel model;
    using Builder = LatestOnlyBuilder<int, int>;
    Builder *pBuilder = nullptr;
    Gate gate;
    std::atomic<bool> started{false};
    Builder builder(
        [&](int &request)
        {
            {
                std::lock_guard<std::mutex> lock(model.mutex);
                model.built.push_back(request);
            }
            if (request == 1)
            {
                started = true;
                gate.Wait();
            }
            return request;
        },
        [&](uint64_t generation, int &request, int &result)
        {
            std::lock_guard<std::mutex> lock(model.mutex);
            if (!pBuilder->IsCurrent(generation))
                return;
            model.installed.push_back(result);
        });
    pBuilder = &builder;

    builder.Submit(1);
    REQUIRE(WaitUntil([&]()
                      { return started.load(); }));
    builder.Submit(2);
    builder.Invalidate(); // e.g. audio restart: neither 1 nor 2 may be installed.
    gate.Open();
    REQUIRE(builder.WaitForIdle(5s));
    REQUIRE(Locked(model.mutex, model.built) == std::vector<int>{1});
    REQUIRE(Locked(model.mutex, model.installed).empty());

    builder.Submit(3);
    REQUIRE(builder.WaitForIdle(5s));
    REQUIRE(Locked(model.mutex, model.installed) == std::vector<int>{3});
}

TEST_CASE("PedalboardBuilder Close while a build is in flight", "[pedalboard_builder]")
{
    FakeModel model;
    auto record = std::make_shared<DestroyRecord>();
    using Builder = LatestOnlyBuilder<int, FakeResult>;
    Gate gate;
    std::atomic<bool> started{false};
    std::atomic<int> installCalls{0};

    Builder *pBuilder = nullptr;
    std::atomic<int> currentInstalls{0};
    auto builder = std::make_unique<Builder>(
        [&](int &request)
        {
            started = true;
            gate.Wait();
            return FakeResult(request, record);
        },
        [&](uint64_t generation, int &request, FakeResult &result)
        {
            ++installCalls;
            if (pBuilder->IsCurrent(generation))
            {
                ++currentInstalls;
            }
        });
    pBuilder = builder.get();

    builder->Submit(1);
    REQUIRE(WaitUntil([&]()
                      { return started.load(); }));
    builder->Submit(2); // pending; must be discarded by Close.

    std::atomic<bool> closed{false};
    std::thread closer([&]()
                       {
        builder->Close();
        closed = true; });

    std::this_thread::sleep_for(50ms);
    REQUIRE_FALSE(closed.load()); // Close waits for the in-flight build.
    gate.Open();
    closer.join();
    REQUIRE(closed.load());
    // the in-flight build is offered to the installer (once), but is never current after Close() started;
    // the superseded pending request (2) is never built.
    REQUIRE(installCalls.load() == 1);
    REQUIRE(currentInstalls.load() == 0);

    {
        std::lock_guard<std::mutex> lock(record->m);
        REQUIRE(record->threads.size() >= 1); // the in-flight result was released.
    }

    // Submit after Close is ignored.
    REQUIRE(builder->Submit(3) == 0);
    REQUIRE(builder->IsIdle());
    builder.reset(); // destructor after Close is safe.
    REQUIRE(installCalls.load() == 1);
}

TEST_CASE("PedalboardBuilder survives a throwing build", "[pedalboard_builder]")
{
    using Builder = LatestOnlyBuilder<int, int>;
    std::mutex m;
    std::vector<int> installed;
    std::vector<std::string> errors;
    Builder builder(
        [&](int &request)
        {
            if (request == 1)
            {
                throw std::runtime_error("plugin failed");
            }
            return request;
        },
        [&](uint64_t generation, int &request, int &result)
        {
            std::lock_guard<std::mutex> lock(m);
            installed.push_back(result);
        },
        [&](const std::exception &e)
        {
            std::lock_guard<std::mutex> lock(m);
            errors.push_back(e.what());
        });

    builder.Submit(1);
    REQUIRE(builder.WaitForIdle(5s));
    builder.Submit(2);
    REQUIRE(builder.WaitForIdle(5s));
    REQUIRE(Locked(m, installed) == std::vector<int>{2});
    REQUIRE(Locked(m, errors) == std::vector<std::string>{"plugin failed"});
}

TEST_CASE("PedalboardBuilder destructor without Submit does not start a thread", "[pedalboard_builder]")
{
    using Builder = LatestOnlyBuilder<int, int>;
    Builder builder(
        [](int &request)
        { return request; },
        [](uint64_t, int &, int &) {});
    REQUIRE(builder.IsIdle());
    REQUIRE(builder.CurrentGeneration() == 0);
}

TEST_CASE("PedalboardBuilder offers stale builds to the installer", "[pedalboard_builder]")
{
    // Stale results are passed to the installer (which may need to dispose of them specially),
    // flagged by IsCurrent(generation) == false.
    using Builder = LatestOnlyBuilder<int, int>;
    Builder *pBuilder = nullptr;
    Gate gate;
    std::atomic<bool> started{false};
    std::mutex m;
    std::vector<std::pair<int, bool>> calls;
    Builder builder(
        [&](int &request)
        {
            if (request == 1)
            {
                started = true;
                gate.Wait();
            }
            return request;
        },
        [&](uint64_t generation, int &request, int &result)
        {
            std::lock_guard<std::mutex> lock(m);
            calls.push_back({result, pBuilder->IsCurrent(generation)});
        });
    pBuilder = &builder;
    builder.Submit(1);
    REQUIRE(WaitUntil([&]()
                      { return started.load(); }));
    builder.Submit(2);
    gate.Open();
    REQUIRE(builder.WaitForIdle(5s));
    std::lock_guard<std::mutex> lock(m);
    REQUIRE(calls == std::vector<std::pair<int, bool>>{{1, false}, {2, true}});
}

namespace
{
    // A model-like harness that mirrors PiPedalModel's RequestPedalboardBuild / InstallBuiltPedalboard.
    struct ModeRequest
    {
        int id;
        bool reuse;
    };
    struct ModeResult
    {
        int id;
        bool reuse;
    };
    class FakeModeModel
    {
    public:
        using Builder = LatestOnlyBuilder<ModeRequest, ModeResult>;

        FakeModeModel()
            : builder(
                  [this](ModeRequest &request)
                  {
                      if (request.id == blockId)
                      {
                          blockStarted = true;
                          gate.Wait();
                      }
                      return ModeResult{request.id, request.reuse};
                  },
                  [this](uint64_t generation, ModeRequest &request, ModeResult &result)
                  {
                      std::lock_guard<std::mutex> lock(mutex);
                      if (builder.IsCurrent(generation))
                      {
                          installed.push_back({result.id, result.reuse});
                          tracker.OnInstalled(result.reuse);
                      }
                      builder.InstallCompleted();
                      idleAfterInstallCompleted.push_back(builder.IsIdle());
                  })
        {
        }
        void Request(int id, bool reuse)
        {
            std::lock_guard<std::mutex> lock(mutex);
            builder.Submit(ModeRequest{id, tracker.ResolveReuse(reuse)});
        }

        std::mutex mutex;
        PedalboardBuildModeTracker tracker;
        std::vector<std::pair<int, bool>> installed;
        std::vector<bool> idleAfterInstallCompleted;
        std::atomic<int> blockId{-1};
        std::atomic<bool> blockStarted{false};
        Gate gate;
        Builder builder; // last: destroyed (joined) first.
    };
}

TEST_CASE("PedalboardBuilder reuse request never supersedes an unfinished full build", "[pedalboard_builder]")
{
    SECTION("nothing installed yet")
    {
        FakeModeModel model;
        model.Request(1, true);
        REQUIRE(model.builder.WaitForIdle(5s));
        REQUIRE(Locked(model.mutex, model.installed) == std::vector<std::pair<int, bool>>{{1, false}});
        // now the running pedalboard is of the current lineage: reuse is allowed.
        model.Request(2, true);
        REQUIRE(model.builder.WaitForIdle(5s));
        REQUIRE(Locked(model.mutex, model.installed) == std::vector<std::pair<int, bool>>{{1, false}, {2, true}});
    }
    SECTION("full build in flight")
    {
        FakeModeModel model;
        model.Request(1, false);
        REQUIRE(model.builder.WaitForIdle(5s));

        model.blockId = 2;
        model.Request(2, false); // e.g. preset load
        REQUIRE(WaitUntil([&]()
                          { return model.blockStarted.load(); }));
        model.Request(3, true); // structure edit while the preset is still loading.
        model.gate.Open();
        REQUIRE(model.builder.WaitForIdle(5s));
        // 2 is stale; 3 must have been built as a full build.
        REQUIRE(Locked(model.mutex, model.installed) == std::vector<std::pair<int, bool>>{{1, false}, {3, false}});
    }
    SECTION("full build pending")
    {
        FakeModeModel model;
        model.Request(1, false);
        REQUIRE(model.builder.WaitForIdle(5s));

        model.blockId = 2;
        model.Request(2, true); // a reuse build occupies the builder thread.
        REQUIRE(WaitUntil([&]()
                          { return model.blockStarted.load(); }));
        model.Request(3, false); // full build, pending.
        model.Request(4, true);  // replaces the pending full build: must become full.
        model.gate.Open();
        REQUIRE(model.builder.WaitForIdle(5s));
        auto installed = Locked(model.mutex, model.installed);
        REQUIRE(installed.back() == std::pair<int, bool>{4, false});
        for (auto &i : installed)
        {
            REQUIRE(i.first != 3); // coalesced away
        }
    }
    SECTION("running pedalboard discarded (audio restart)")
    {
        FakeModeModel model;
        model.Request(1, false);
        REQUIRE(model.builder.WaitForIdle(5s));
        {
            std::lock_guard<std::mutex> lock(model.mutex);
            model.tracker.OnRunningPedalboardDiscarded();
            model.builder.Invalidate();
        }
        model.Request(2, true);
        REQUIRE(model.builder.WaitForIdle(5s));
        REQUIRE(Locked(model.mutex, model.installed).back() == std::pair<int, bool>{2, false});
    }
}

TEST_CASE("PedalboardBuilder is idle as soon as the installer reports completion", "[pedalboard_builder]")
{
    FakeModeModel model;
    model.Request(1, false);
    REQUIRE(model.builder.WaitForIdle(5s));
    model.Request(2, false);
    REQUIRE(model.builder.WaitForIdle(5s));
    REQUIRE(Locked(model.mutex, model.idleAfterInstallCompleted) == std::vector<bool>{true, true});
}

TEST_CASE("PedalboardBuilder install decision", "[pedalboard_builder]")
{
    PedalboardInstallState ok;
    ok.isCurrent = true;
    ok.buildAudioEpoch = ok.currentAudioEpoch = 3;
    ok.buildConfigurationVersion = ok.currentConfigurationVersion = 7;

    SECTION("full build")
    {
        REQUIRE(ShouldInstallBuiltPedalboard(ok));

        auto s = ok;
        s.isCurrent = false;
        REQUIRE_FALSE(ShouldInstallBuiltPedalboard(s)); // stale

        s = ok;
        s.audioRestarting = true;
        REQUIRE_FALSE(ShouldInstallBuiltPedalboard(s)); // RestartAudio in progress

        s = ok;
        s.currentAudioEpoch = 4;
        REQUIRE_FALSE(ShouldInstallBuiltPedalboard(s)); // requested before the audio was restarted

        s = ok;
        s.currentConfigurationVersion = 8;
        REQUIRE_FALSE(ShouldInstallBuiltPedalboard(s)); // built for the old configuration
    }
    SECTION("restart sequence: build requested after Close, built before the new configuration")
    {
        // RestartAudio: Invalidate (restarting) -> Close -> epoch bump -> Open -> config bump -> clear restarting.
        auto s = ok;
        s.currentAudioEpoch = s.buildAudioEpoch = 4; // requested after the epoch bump
        s.audioRestarting = true;                     // installs before RestartAudio requests its build
        REQUIRE_FALSE(ShouldInstallBuiltPedalboard(s));
        s.audioRestarting = false; // installs after RestartAudio finished
        s.currentConfigurationVersion = 8;
        REQUIRE_FALSE(ShouldInstallBuiltPedalboard(s)); // still the old configuration
    }
    SECTION("borrowing build: same rules (borrowing only stages its changes, so discarding it is safe)")
    {
        auto s = ok;
        s.reuseBuild = true;
        REQUIRE(ShouldInstallBuiltPedalboard(s));

        s = ok;
        s.reuseBuild = true;
        s.isCurrent = false;
        REQUIRE_FALSE(ShouldInstallBuiltPedalboard(s)); // superseded: the newer build borrows again.

        s = ok;
        s.reuseBuild = true;
        s.audioRestarting = true;
        REQUIRE_FALSE(ShouldInstallBuiltPedalboard(s));

        s = ok;
        s.reuseBuild = true;
        s.currentAudioEpoch = 4;
        REQUIRE_FALSE(ShouldInstallBuiltPedalboard(s)); // audio stopped since the effects were borrowed.

        s = ok;
        s.reuseBuild = true;
        s.currentConfigurationVersion = 8;
        REQUIRE_FALSE(ShouldInstallBuiltPedalboard(s)); // built for the old audio configuration.
    }
}
