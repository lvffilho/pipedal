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

// Routing of edits and notifications to the right pedalboard while builds are outstanding
// (PiPedalModel; see the threading notes above PiPedalModel::LoadCurrentPedalboard()).

#include "pch.h"
#include "catch.hpp"
#include "PedalboardEditRouting.hpp"
#include "PedalboardBuilder.hpp"
#include "AudioHost.hpp"
#include "PluginHost.hpp"
#include "Lv2Pedalboard.hpp"
#include "Lv2Effect.hpp"
#include "ss.hpp"
#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace pipedal;

namespace
{
    const char *A_EQ_URI = "urn:ardour:a-eq";
    const char *A_COMP_URI = "urn:ardour:a-comp";

    PedalboardItem MakeItem(int64_t instanceId, const std::string &uri, const std::string &vstState = "")
    {
        PedalboardItem item;
        item.instanceId(instanceId);
        item.uri(uri);
        item.vstState(vstState);
        return item;
    }

    bool LoadTestPlugins(PluginHost &host)
    {
        host.LoadLilv("/usr/lib/lv2:/usr/local/lib/lv2:/usr/modep/lv2");
        if (!host.GetPluginInfo(A_EQ_URI) || !host.GetPluginInfo(A_COMP_URI))
        {
            WARN("x42/Ardour a-eq and a-comp LV2 plugins are not installed. Skipping.");
            return false;
        }
        return true;
    }
}

TEST_CASE("Edit routing: VST3 state comes from the model while a full build is outstanding", "[model_edit_routing]")
{
    Pedalboard model; // what the outstanding build was requested from.
    model.items().push_back(MakeItem(1, "vst3:plugin-a", "AABB"));
    model.items().push_back(MakeItem(2, "vst3:plugin-b", ""));
    model.items().push_back(MakeItem(3, "urn:lv2-plugin", "ignored"));
    model.items().push_back(MakeItem(4, "vst3:plugin-c", "CCDD"));

    Pedalboard incoming; // an edited copy from a client, possibly with stale VST3 state.
    incoming.items().push_back(MakeItem(1, "vst3:plugin-a", "0011"));
    incoming.items().push_back(MakeItem(2, "vst3:plugin-b", "2233"));
    incoming.items().push_back(MakeItem(3, "urn:lv2-plugin", ""));
    incoming.items().push_back(MakeItem(4, "vst3:other-plugin", "4455")); // same id, another plugin.
    incoming.items().push_back(MakeItem(5, "vst3:plugin-new", "6677"));   // not in the model.

    MergeVst3State(incoming, model);

    REQUIRE(incoming.GetItem(1)->vstState() == "AABB"); // the model's state wins.
    REQUIRE(incoming.GetItem(2)->vstState() == "2233"); // empty model state isn't copied.
    REQUIRE(incoming.GetItem(3)->vstState() == "");     // not a VST3 item.
    REQUIRE(incoming.GetItem(4)->vstState() == "4455"); // different plugin: untouched.
    REQUIRE(incoming.GetItem(5)->vstState() == "6677"); // new item: untouched.
}

TEST_CASE("Edit routing: IndexOfPointer", "[model_edit_routing]")
{
    int a = 0, b = 0, c = 0;
    std::vector<int *> v{&a, &b};
    REQUIRE(IndexOfPointer(v, &a) == 0);
    REQUIRE(IndexOfPointer(v, &b) == 1);
    REQUIRE(IndexOfPointer(v, &c) == -1);
    REQUIRE(IndexOfPointer(v, nullptr) == -1);
}

TEST_CASE("Edit routing: realtime parameter requests are rejected by a pedalboard of another lineage", "[model_edit_routing]")
{
    Lv2Pedalboard pedalboard; // empty: no effects.
    pedalboard.SetInstanceIdLineage(2);

    auto makeRequest = [](uint64_t lineage)
    {
        auto request = std::make_unique<RealtimePatchPropertyRequest>(
            [](RealtimePatchPropertyRequest *) {}, 1, 3, (LV2_URID)1,
            [](const std::string &) {}, [](const std::string &) {}, (size_t)48000);
        request->instanceIdLineage = lineage;
        return request;
    };

    SECTION("addressed to the previous lineage")
    {
        auto request = makeRequest(1);
        pedalboard.ProcessParameterRequests(request.get(), 64);
        pedalboard.GatherPatchProperties(request.get());
        REQUIRE(request->errorMessage != nullptr);
        REQUIRE(std::string(request->errorMessage) == "The pedalboard has changed.");
    }
    SECTION("addressed to this lineage: resolved by instance id")
    {
        auto request = makeRequest(2);
        pedalboard.ProcessParameterRequests(request.get(), 64);
        REQUIRE(request->errorMessage != nullptr);
        REQUIRE(std::string(request->errorMessage) == "No such effect.");
    }
}

TEST_CASE("Edit routing: notifications are matched to the installed pedalboard by sender address", "[model_edit_routing]")
{
    PluginHost host;
    if (!LoadTestPlugins(host))
    {
        return;
    }
    Lv2PedalboardErrorList errors;

    // Running pedalboard A: a-eq (id 3) and a-comp (id 5).
    Pedalboard pedalboardA;
    pedalboardA.items().push_back(MakeItem(3, A_EQ_URI));
    pedalboardA.items().push_back(MakeItem(5, A_COMP_URI));
    std::shared_ptr<Lv2Pedalboard> lv2PedalboardA{host.CreateLv2Pedalboard(pedalboardA, errors)};
    IEffect *eqA = lv2PedalboardA->GetEffect(3);
    IEffect *compA = lv2PedalboardA->GetEffect(5);
    REQUIRE(eqA != nullptr);
    REQUIRE(compA != nullptr);

    // Installed (not yet swapped in) pedalboard B, another preset: a new a-comp with the colliding id 3,
    // and A's a-eq reused under id 7.
    Pedalboard pedalboardB;
    pedalboardB.items().push_back(MakeItem(3, A_COMP_URI));
    pedalboardB.items().push_back(MakeItem(7, A_EQ_URI));
    ExistingEffectMap reusable;
    reusable[7] = lv2PedalboardA->GetSharedEffectList()[0];
    std::shared_ptr<Lv2Pedalboard> lv2PedalboardB{host.CreateLv2PedalboardReusingInstances(pedalboardB, reusable, errors)};
    REQUIRE(lv2PedalboardB->GetBorrowedEffectCount() == 1);

    // A notification from A's a-eq still carries id 3 (it is re-keyed at the swap), which names B's new a-comp.
    REQUIRE(eqA->GetInstanceId() == 3);
    REQUIRE(lv2PedalboardB->GetEffect(3) != eqA);
    // By address, it is B's item 7.
    int index = IndexOfPointer(lv2PedalboardB->GetEffects(), static_cast<const IEffect *>(eqA));
    REQUIRE(index != -1);
    REQUIRE(lv2PedalboardB->GetInstanceIdAt(index) == 7);
    // A's a-comp isn't in B: its notifications are dropped.
    REQUIRE(IndexOfPointer(lv2PedalboardB->GetEffects(), static_cast<const IEffect *>(compA)) == -1);
}

TEST_CASE("Edit routing: a discarded borrowing build leaves the running effects intact", "[model_edit_routing]")
{
    PluginHost host;
    if (!LoadTestPlugins(host))
    {
        return;
    }
    Lv2PedalboardErrorList errors;

    Pedalboard pedalboardA;
    PedalboardItem eq = MakeItem(3, A_EQ_URI);
    eq.controlValues().push_back(ControlValue("g1", 6.0f));
    pedalboardA.items().push_back(eq);
    std::shared_ptr<Lv2Pedalboard> lv2PedalboardA{host.CreateLv2Pedalboard(pedalboardA, errors)};
    IEffect *effect = lv2PedalboardA->GetEffect(3);
    REQUIRE(effect != nullptr);
    int g1 = effect->GetControlIndex("g1");
    REQUIRE(g1 >= 0);

    // An edit adds a plugin in front: borrowing build B (superseded, so never installed).
    Pedalboard pedalboardB;
    pedalboardB.items().push_back(MakeItem(4, A_COMP_URI));
    PedalboardItem eqB = eq;
    eqB.controlValues()[0].value(-6.0f);
    pedalboardB.items().push_back(eqB);
    {
        std::shared_ptr<Lv2Pedalboard> lv2PedalboardB{host.UpdateLv2PedalboardStructure(pedalboardB, lv2PedalboardA.get(), errors)};
        REQUIRE(lv2PedalboardB->GetEffect(3) == effect); // borrowed.
    } // discarded.

    REQUIRE(effect->GetInstanceId() == 3);
    REQUIRE(effect->GetControlValue(g1) == 6.0f);
    REQUIRE(lv2PedalboardA->GetEffect(3) == effect);

    // The build that superseded it borrows the instance again.
    std::shared_ptr<Lv2Pedalboard> lv2PedalboardC{host.UpdateLv2PedalboardStructure(pedalboardB, lv2PedalboardA.get(), errors)};
    REQUIRE(lv2PedalboardC->GetEffect(3) == effect);
}

TEST_CASE("Edit routing: plugin state and path properties can be read while the model writes them", "[model_edit_routing]")
{
    // Smoke test for the locks that let FindReusableInstances() run without the model mutex (run under TSAN to
    // check for data races).
    PluginHost host;
    if (!LoadTestPlugins(host))
    {
        return;
    }
    Lv2PedalboardErrorList errors;
    Pedalboard pedalboard;
    pedalboard.items().push_back(MakeItem(3, A_EQ_URI));
    std::shared_ptr<Lv2Pedalboard> lv2Pedalboard{host.CreateLv2Pedalboard(pedalboard, errors)};
    Lv2Effect *effect = dynamic_cast<Lv2Effect *>(lv2Pedalboard->GetEffect(3));
    REQUIRE(effect != nullptr);

    effect->SetPathPatchProperty("urn:test#file", "{\"otype_\":\"Path\",\"value\":\"f.wav\"}");
    std::atomic<bool> stop{false};
    std::thread writer([&]()
                       {
        int i = 0;
        while (!stop)
        {
            effect->SetPathPatchProperty("urn:test#file", SS("{\"otype_\":\"Path\",\"value\":\"f" << (i++ % 7) << ".wav\"}"));
            Lv2PluginState state;
            effect->GetLv2State(&state); // the model's state-save path.
        } });
    for (int i = 0; i < 2000; ++i)
    {
        auto properties = effect->GetPathPatchProperties(); // the builder thread's scan.
        Lv2PluginState state;
        effect->GetLv2State(&state);
        REQUIRE(properties.contains("urn:test#file"));
    }
    stop = true;
    writer.join();
}

TEST_CASE("Edit routing: deferred patch requests are queued, coalesced and bounded", "[model_edit_routing]")
{
    using Queue = DeferredPatchRequests<std::string>;
    Queue queue(3);
    std::optional<std::string> replaced;

    std::string r = "get-a";
    REQUIRE(queue.Add(10, 1, "urn:a", false, r, &replaced) == Queue::AddResult::Queued);
    r = "set-a-1";
    REQUIRE(queue.Add(10, 1, "urn:a", true, r, &replaced) == Queue::AddResult::Queued);
    REQUIRE_FALSE(replaced.has_value());

    SECTION("a newer set of the same instance and property replaces the queued one")
    {
        r = "set-a-2";
        REQUIRE(queue.Add(10, 1, "urn:a", true, r, &replaced) == Queue::AddResult::Replaced);
        REQUIRE(replaced == std::string("set-a-1"));
        REQUIRE(queue.size() == 2);
        // gets aren't coalesced (each one needs its answer).
        r = "get-a-again";
        REQUIRE(queue.Add(10, 1, "urn:a", false, r, &replaced) == Queue::AddResult::Queued);
        auto entries = queue.TakeAll();
        REQUIRE(entries.size() == 3);
        REQUIRE(entries[0].request == "get-a");
        REQUIRE(entries[1].request == "set-a-2"); // keeps its place.
        REQUIRE(entries[1].isSet);
        REQUIRE(entries[2].request == "get-a-again");
        REQUIRE(queue.empty());
    }
    SECTION("sets of other instances or properties are separate; the queue is bounded")
    {
        r = "set-b";
        REQUIRE(queue.Add(10, 2, "urn:a", true, r, &replaced) == Queue::AddResult::Queued);
        r = "set-c";
        REQUIRE(queue.Add(10, 1, "urn:c", true, r, &replaced) == Queue::AddResult::Full);
        REQUIRE(r == "set-c"); // left with the caller, which fails it.
        r = "set-b-2"; // a replacement still fits.
        REQUIRE(queue.Add(10, 2, "urn:a", true, r, &replaced) == Queue::AddResult::Replaced);
        REQUIRE(replaced == std::string("set-b"));
        REQUIRE(queue.size() == 3);
    }
}

TEST_CASE("Edit routing: a disconnected client's deferred requests are discarded", "[model_edit_routing]")
{
    using Queue = DeferredPatchRequests<std::string>;
    Queue queue;
    std::string r = "a-get";
    queue.Add(1, 3, "urn:x", false, r, nullptr);
    r = "b-set";
    queue.Add(2, 3, "urn:y", true, r, nullptr);
    r = "a-set";
    queue.Add(1, 4, "urn:y", true, r, nullptr);
    r = "b-replaces-a"; // coalesced: the entry now holds client 2's callbacks.
    queue.Add(2, 4, "urn:y", true, r, nullptr);

    REQUIRE(queue.RemoveClient(1) == 1);
    auto entries = queue.TakeAll();
    REQUIRE(entries.size() == 2);
    REQUIRE(entries[0].request == "b-set");
    REQUIRE(entries[1].request == "b-replaces-a");
    REQUIRE(entries[1].clientId == 2);
    REQUIRE(queue.RemoveClient(2) == 0);
}

TEST_CASE("Edit routing: a deferred request that fails to send doesn't affect the others", "[model_edit_routing]")
{
    using Queue = DeferredPatchRequests<std::string>;
    Queue queue;
    for (const char *name : {"one", "bad", "three"})
    {
        std::string r = name;
        queue.Add(1, 3, name, false, r, nullptr);
    }
    std::vector<std::string> sent, failed;
    REQUIRE_NOTHROW(queue.SendAll(
        [&](Queue::Entry &entry)
        {
            if (entry.request == "bad")
            {
                throw std::runtime_error("malformed");
            }
            sent.push_back(entry.request);
        },
        [&](Queue::Entry &entry, const std::exception &e)
        { failed.push_back(entry.request + ":" + e.what()); }));
    REQUIRE(sent == std::vector<std::string>{"one", "three"});
    REQUIRE(failed == std::vector<std::string>{"bad:malformed"});
    REQUIRE(queue.empty());
}

TEST_CASE("Edit routing: deferred requests when a build ends", "[model_edit_routing]")
{
    using A = DeferredPatchRequestsAction;
    REQUIRE(OnBuildEndedDeferredPatchRequests(true, true, true) == A::Send);
    REQUIRE(OnBuildEndedDeferredPatchRequests(true, true, false) == A::Send);
    // installed, but the ids still don't match (can't happen today): don't strand them if nothing else is coming.
    REQUIRE(OnBuildEndedDeferredPatchRequests(true, false, true) == A::Fail);
    REQUIRE(OnBuildEndedDeferredPatchRequests(true, false, false) == A::Keep);
    // discarded or failed.
    REQUIRE(OnBuildEndedDeferredPatchRequests(false, true, true) == A::Fail);
    REQUIRE(OnBuildEndedDeferredPatchRequests(false, false, false) == A::Keep); // a newer build is outstanding.
}

TEST_CASE("Edit routing: Lv2Effect::SetLv2State ignores an invalid state", "[model_edit_routing]")
{
    // (The installed test plugins have no state interface, so this only checks that the call is a no-op.)
    PluginHost host;
    if (!LoadTestPlugins(host))
    {
        return;
    }
    Lv2PedalboardErrorList errors;
    Pedalboard pedalboard;
    pedalboard.items().push_back(MakeItem(3, A_EQ_URI));
    std::shared_ptr<Lv2Pedalboard> lv2Pedalboard{host.CreateLv2Pedalboard(pedalboard, errors)};
    IEffect *effect = lv2Pedalboard->GetEffect(3);
    REQUIRE(effect != nullptr);
    Lv2PluginState invalid;
    REQUIRE_FALSE(invalid.isValid_);
    effect->SetLv2State(invalid);
    Lv2PluginState state;
    REQUIRE_FALSE(effect->GetLv2State(&state));
}
