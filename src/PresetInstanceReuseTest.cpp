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
#include "PresetInstanceReuse.hpp"
#include "PedalboardBuilder.hpp"
#include "PluginHost.hpp"
#include "Lv2Pedalboard.hpp"
#include "Lv2Effect.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace pipedal;

namespace
{
    const char *NAM_URI = "http://two-play.com/plugins/toob-nam";
    const char *EQ_URI = "http://two-play.com/plugins/toob-eq";
    const char *MODEL_PROPERTY = "http://two-play.com/plugins/toob-nam#modelFile";

    Lv2PluginState MakeState(const std::string &modelPath)
    {
        Lv2PluginState state;
        state.isValid_ = true;
        Lv2PluginStateEntry entry;
        entry.atomType_ = "http://lv2plug.in/ns/ext/atom#Path";
        entry.value_ = std::vector<uint8_t>(modelPath.begin(), modelPath.end());
        entry.value_.push_back(0);
        state.values_["http://two-play.com/plugins/toob-nam#modelFile"] = entry;
        return state;
    }

    PedalboardItem MakeItem(int64_t instanceId, const std::string &uri, const std::string &model = "")
    {
        PedalboardItem item;
        item.instanceId(instanceId);
        item.uri(uri);
        if (!model.empty())
        {
            item.lv2State(MakeState(model));
            std::map<std::string, std::string> pathProperties;
            pathProperties[MODEL_PROPERTY] = "{\"otype_\":\"Path\",\"value\":\"" + model + "\"}";
            item.pathProperties(pathProperties);
        }
        return item;
    }

    std::vector<const PedalboardItem *> Pointers(const std::vector<PedalboardItem> &items)
    {
        std::vector<const PedalboardItem *> result;
        for (const auto &item : items)
        {
            result.push_back(&item);
        }
        return result;
    }

    std::map<int64_t, int64_t> Match(const std::vector<PedalboardItem> &running, const std::vector<PedalboardItem> &incoming)
    {
        return MatchReusableInstances(Pointers(running), Pointers(incoming));
    }
}

TEST_CASE("Preset instance reuse: matching criteria", "[preset_instance_reuse]")
{
    std::vector<PedalboardItem> running{MakeItem(3, NAM_URI, "amps/Plexi.nam")};

    SECTION("same uri, state and path properties: reused under the new instance id")
    {
        std::vector<PedalboardItem> incoming{MakeItem(1, EQ_URI), MakeItem(7, NAM_URI, "amps/Plexi.nam")};
        auto matches = Match(running, incoming);
        REQUIRE(matches.size() == 1);
        REQUIRE(matches.at(7) == 3);
    }
    SECTION("different uri")
    {
        PedalboardItem item = MakeItem(7, EQ_URI, "amps/Plexi.nam");
        REQUIRE_FALSE(CanReuseInstance(running[0], item));
        REQUIRE(Match(running, {item}).empty());
    }
    SECTION("different state blob")
    {
        PedalboardItem item = MakeItem(7, NAM_URI, "amps/Plexi.nam");
        item.lv2State(MakeState("amps/Bassman.nam"));
        REQUIRE_FALSE(CanReuseInstance(running[0], item));
        REQUIRE(Match(running, {item}).empty());
    }
    SECTION("state present vs absent")
    {
        PedalboardItem item = MakeItem(7, NAM_URI, "amps/Plexi.nam");
        item.lv2State(Lv2PluginState());
        REQUIRE_FALSE(CanReuseInstance(running[0], item));
    }
    SECTION("different path property value")
    {
        PedalboardItem item = MakeItem(7, NAM_URI, "amps/Plexi.nam");
        std::map<std::string, std::string> pathProperties;
        pathProperties[MODEL_PROPERTY] = "{\"otype_\":\"Path\",\"value\":\"amps/Bassman.nam\"}";
        item.pathProperties(pathProperties);
        REQUIRE_FALSE(CanReuseInstance(running[0], item));
        REQUIRE(Match(running, {item}).empty());
    }
    SECTION("missing path property")
    {
        PedalboardItem item = MakeItem(7, NAM_URI, "amps/Plexi.nam");
        item.pathProperties(std::map<std::string, std::string>());
        REQUIRE_FALSE(CanReuseInstance(running[0], item));
    }
    SECTION("the new item loads a lilv preset")
    {
        PedalboardItem item = MakeItem(7, NAM_URI, "amps/Plexi.nam");
        item.lilvPresetUri("http://two-play.com/plugins/toob-nam#preset1");
        REQUIRE_FALSE(CanReuseInstance(running[0], item));
    }
    SECTION("plugins without state or path properties match on uri")
    {
        std::vector<PedalboardItem> runningEq{MakeItem(2, EQ_URI)};
        auto matches = Match(runningEq, {MakeItem(5, EQ_URI)});
        REQUIRE(matches.size() == 1);
        REQUIRE(matches.at(5) == 2);
    }
}

TEST_CASE("Preset instance reuse: running instances are described by their live state", "[preset_instance_reuse]")
{
    // Stands in for Storage::ToAbstractPathFromJson: strips the upload directory; rejects non-json values.
    PathPropertyNormalizer normalize = [](const std::string &jsonAtom)
    {
        if (!jsonAtom.starts_with("{"))
        {
            throw std::runtime_error("not a json atom");
        }
        std::string result = jsonAtom;
        const std::string uploads = "/var/pipedal/audio_uploads/";
        auto pos = result.find(uploads);
        if (pos != std::string::npos)
        {
            result.erase(pos, uploads.size());
        }
        return result;
    };
    auto pathAtom = [](const std::string &path)
    { return "{\"otype_\":\"Path\",\"value\":\"" + path + "\"}"; };

    // the running instance was built from a pedalboard with Plexi.nam; the model file was changed since.
    std::map<std::string, std::string> livePathProperties{{MODEL_PROPERTY, pathAtom("/var/pipedal/audio_uploads/amps/Bassman.nam")}};
    std::optional<Lv2PluginState> liveState = MakeState("amps/Bassman.nam");
    std::vector<PedalboardItem> running{DescribeRunningInstance(3, NAM_URI, liveState, livePathProperties, normalize)};

    REQUIRE(running[0].instanceId() == 3);
    REQUIRE(running[0].uri() == NAM_URI);
    REQUIRE(running[0].pathProperties().at(MODEL_PROPERTY) == pathAtom("amps/Bassman.nam")); // normalized.

    SECTION("an item matching the saved (stale) state isn't matched")
    {
        PedalboardItem incoming = DescribeIncomingItem(MakeItem(7, NAM_URI, "amps/Plexi.nam"), normalize);
        REQUIRE(Match(running, {incoming}).empty());
    }
    SECTION("an item matching the live state is matched, absolute and abstract paths alike")
    {
        PedalboardItem item = MakeItem(7, NAM_URI, "amps/Bassman.nam");
        PedalboardItem incoming = DescribeIncomingItem(item, normalize);
        auto matches = Match(running, {incoming});
        REQUIRE(matches.size() == 1);
        REQUIRE(matches.at(7) == 3);
    }
    SECTION("the incoming description keeps the lilv preset (which prevents reuse)")
    {
        PedalboardItem item = MakeItem(7, NAM_URI, "amps/Bassman.nam");
        item.lilvPresetUri("http://two-play.com/plugins/toob-nam#preset1");
        PedalboardItem incoming = DescribeIncomingItem(item, normalize);
        REQUIRE(incoming.lilvPresetUri() == item.lilvPresetUri());
        REQUIRE(Match(running, {incoming}).empty());
    }
    SECTION("no state interface: described without state")
    {
        std::vector<PedalboardItem> runningEq{DescribeRunningInstance(2, EQ_URI, std::nullopt, {}, normalize)};
        REQUIRE(Match(runningEq, {DescribeIncomingItem(MakeItem(5, EQ_URI), normalize)}).at(5) == 2);
        // an item that carries a state blob doesn't match an instance without one.
        PedalboardItem withState = MakeItem(5, EQ_URI);
        withState.lv2State(MakeState("x"));
        REQUIRE(Match(runningEq, {DescribeIncomingItem(withState, normalize)}).empty());
    }
    SECTION("values the normalizer rejects are compared as is")
    {
        std::map<std::string, std::string> raw{{MODEL_PROPERTY, "not-json"}};
        REQUIRE(NormalizePathProperties(raw, normalize).at(MODEL_PROPERTY) == "not-json");
    }
}

TEST_CASE("Preset instance reuse: VST3 instances are never reused", "[preset_instance_reuse]")
{
    const char *VST3_URI = "vst3:01234567-89ab-cdef-0123-456789abcdef";
    std::vector<PedalboardItem> running{MakeItem(1, VST3_URI)};
    std::vector<PedalboardItem> incoming{MakeItem(1, VST3_URI), MakeItem(2, VST3_URI)};
    REQUIRE_FALSE(IsInstanceReuseCandidate(running[0]));
    REQUIRE_FALSE(CanReuseInstance(running[0], incoming[0]));
    REQUIRE(Match(running, incoming).empty());
}

TEST_CASE("Preset instance reuse: each running instance is used at most once", "[preset_instance_reuse]")
{
    SECTION("two matching items, one running instance")
    {
        std::vector<PedalboardItem> running{MakeItem(3, NAM_URI, "amps/Plexi.nam")};
        std::vector<PedalboardItem> incoming{MakeItem(1, NAM_URI, "amps/Plexi.nam"), MakeItem(2, NAM_URI, "amps/Plexi.nam")};
        auto matches = Match(running, incoming);
        REQUIRE(matches.size() == 1);
        REQUIRE(matches.at(1) == 3); // first in pedalboard order.
    }
    SECTION("two matching items, two running instances")
    {
        std::vector<PedalboardItem> running{MakeItem(3, NAM_URI, "amps/Plexi.nam"), MakeItem(4, NAM_URI, "amps/Plexi.nam")};
        std::vector<PedalboardItem> incoming{MakeItem(1, NAM_URI, "amps/Plexi.nam"), MakeItem(2, NAM_URI, "amps/Plexi.nam")};
        auto matches = Match(running, incoming);
        REQUIRE(matches.size() == 2);
        REQUIRE(matches.at(1) == 3);
        REQUIRE(matches.at(2) == 4);
    }
    SECTION("each instance goes to the item with the matching state")
    {
        std::vector<PedalboardItem> running{MakeItem(3, NAM_URI, "amps/Plexi.nam"), MakeItem(4, NAM_URI, "amps/Bassman.nam")};
        std::vector<PedalboardItem> incoming{MakeItem(1, NAM_URI, "amps/Bassman.nam"), MakeItem(2, NAM_URI, "amps/Plexi.nam")};
        auto matches = Match(running, incoming);
        REQUIRE(matches.size() == 2);
        REQUIRE(matches.at(1) == 4);
        REQUIRE(matches.at(2) == 3);
    }
    SECTION("an instance with the same instance id is preferred")
    {
        std::vector<PedalboardItem> running{MakeItem(3, NAM_URI, "amps/Plexi.nam"), MakeItem(2, NAM_URI, "amps/Plexi.nam")};
        std::vector<PedalboardItem> incoming{MakeItem(1, NAM_URI, "amps/Plexi.nam"), MakeItem(2, NAM_URI, "amps/Plexi.nam")};
        auto matches = Match(running, incoming);
        REQUIRE(matches.size() == 2);
        REQUIRE(matches.at(2) == 2);
        REQUIRE(matches.at(1) == 3);
    }
}

TEST_CASE("Preset instance reuse: split chains are searched, splits are never reused", "[preset_instance_reuse]")
{
    Pedalboard pedalboard;
    PedalboardItem split = pedalboard.MakeSplit();
    split.topChain().push_back(MakeItem(11, NAM_URI, "amps/Plexi.nam"));
    split.bottomChain().push_back(MakeItem(12, EQ_URI));
    split.bottomChain().push_back(pedalboard.MakeEmptyItem());
    pedalboard.items().push_back(MakeItem(10, EQ_URI));
    pedalboard.items().push_back(split);

    auto flattened = FlattenPedalboardItems(pedalboard);
    REQUIRE(flattened.size() == 3);
    REQUIRE(flattened[0]->instanceId() == 10);
    REQUIRE(flattened[1]->instanceId() == 11);
    REQUIRE(flattened[2]->instanceId() == 12);

    REQUIRE_FALSE(IsInstanceReuseCandidate(split));
    REQUIRE_FALSE(IsInstanceReuseCandidate(pedalboard.MakeEmptyItem()));

    std::vector<PedalboardItem> running{MakeItem(1, NAM_URI, "amps/Plexi.nam")};
    auto matches = MatchReusableInstances(Pointers(running), flattened);
    REQUIRE(matches.size() == 1);
    REQUIRE(matches.at(11) == 1);
}

TEST_CASE("Preset instance reuse: channel configuration (buffer layout)", "[preset_instance_reuse]")
{
    SECTION("effects with audio inputs and outputs: any number of input channels")
    {
        EffectBufferLayout layout;
        layout.inputAudioPorts = 1;
        layout.outputAudioPorts = 1;
        layout.inputAudioBuffers = 1;
        layout.outputAudioBuffers = 1;
        REQUIRE(BorrowKeepsBufferLayout(layout, 1));
        REQUIRE(BorrowKeepsBufferLayout(layout, 2));
    }
    SECTION("zero-input effect: same channel count only")
    {
        EffectBufferLayout layout; // e.g. a mono generator prepared for mono input.
        layout.inputAudioPorts = 0;
        layout.outputAudioPorts = 1;
        layout.inputAudioBuffers = 1;
        layout.outputAudioBuffers = 1;
        layout.outputMixBuffers = 1;
        REQUIRE(BorrowKeepsBufferLayout(layout, 1));
        REQUIRE_FALSE(BorrowKeepsBufferLayout(layout, 2));

        layout.outputMixBuffersSized = false; // prepared for a different buffer size.
        REQUIRE_FALSE(BorrowKeepsBufferLayout(layout, 1));
    }
    SECTION("zero-input effect, stereo input")
    {
        EffectBufferLayout layout;
        layout.inputAudioPorts = 0;
        layout.outputAudioPorts = 1;
        layout.inputAudioBuffers = 2;
        layout.outputAudioBuffers = 2;
        layout.outputMixBuffers = 1;
        REQUIRE(BorrowKeepsBufferLayout(layout, 2));
        REQUIRE_FALSE(BorrowKeepsBufferLayout(layout, 1));
    }
    SECTION("zero-output effect")
    {
        EffectBufferLayout layout;
        layout.inputAudioPorts = 1;
        layout.outputAudioPorts = 0;
        layout.inputAudioBuffers = 2;
        layout.outputAudioBuffers = 0;
        REQUIRE(BorrowKeepsBufferLayout(layout, 2));
        REQUIRE_FALSE(BorrowKeepsBufferLayout(layout, 1));
    }
}

TEST_CASE("Preset instance reuse: control values of a reused instance", "[preset_instance_reuse]")
{
    // ports: 0 = audio in, 1 = gain (input control), 2 = level (input control), 3 = meter (output control),
    // 4 = tone (input control).
    std::map<std::string, int> indices{{"gain", 1}, {"level", 2}, {"meter", 3}, {"tone", 4}};
    std::vector<bool> isInput{false, true, true, false, true};
    std::vector<float> defaults{0, 0.5f, 0.25f, 0, 0.75f};

    PedalboardItem item = MakeItem(7, NAM_URI);
    item.controlValues().push_back(ControlValue("gain", 0.9f));
    item.controlValues().push_back(ControlValue("meter", 3.0f));   // output: ignored
    item.controlValues().push_back(ControlValue("unknown", 4.0f)); // not a port: ignored
    item.controlValues().push_back(ControlValue("tone", 0.1f));

    auto values = ResolveItemControlValues(
        item,
        5,
        [&](uint64_t index)
        { return (bool)isInput[index]; },
        [&](uint64_t index)
        { return defaults[index]; },
        [&](const std::string &symbol)
        {
            auto f = indices.find(symbol);
            return f == indices.end() ? -1 : f->second;
        });

    std::vector<std::pair<int, float>> expected{{1, 0.9f}, {2, 0.25f}, {4, 0.1f}};
    REQUIRE(values == expected);
}

TEST_CASE("Preset instance reuse: build mode tracker", "[preset_instance_reuse]")
{
    PedalboardBuildModeTracker tracker;
    REQUIRE_FALSE(tracker.CanReuseRunningInstances()); // nothing running yet.

    REQUIRE_FALSE(tracker.ResolveReuse(false)); // first full build.
    tracker.OnInstalled(false);
    REQUIRE(tracker.CanReuseRunningInstances());
    REQUIRE_FALSE(tracker.FullBuildOutstanding());

    SECTION("a stale full build installed because it reused instances doesn't establish the lineage")
    {
        REQUIRE_FALSE(tracker.ResolveReuse(false)); // preset A (reuses instances)
        REQUIRE_FALSE(tracker.ResolveReuse(false)); // preset B, while A is in flight.
        tracker.OnInstalled(true);                  // A installed while stale.
        REQUIRE(tracker.FullBuildOutstanding());
        REQUIRE(tracker.CanReuseRunningInstances());
        REQUIRE_FALSE(tracker.ResolveReuse(true)); // an edit of B can't borrow by instance id from A.
        tracker.OnInstalled(false);                // B installed.
        REQUIRE_FALSE(tracker.FullBuildOutstanding());
        REQUIRE(tracker.ResolveReuse(true));
    }
    SECTION("no reuse after the running pedalboard was discarded (restart, failed build)")
    {
        tracker.OnRunningPedalboardDiscarded();
        REQUIRE_FALSE(tracker.CanReuseRunningInstances());
        REQUIRE_FALSE(tracker.ResolveReuse(false));
        tracker.OnInstalled(false);
        REQUIRE(tracker.CanReuseRunningInstances());
    }
}

TEST_CASE("Preset instance reuse: a stale build that reused instances is discarded", "[preset_instance_reuse]")
{
    PedalboardInstallState state;
    state.reuseBuild = true; // borrowed running effects
    state.isCurrent = true;
    state.buildAudioEpoch = 4;
    state.currentAudioEpoch = 4;
    state.buildConfigurationVersion = 1;
    state.currentConfigurationVersion = 1;
    REQUIRE(ShouldInstallBuiltPedalboard(state));

    state.isCurrent = false; // superseded: borrowing only staged its changes, so the running effects are intact.
    REQUIRE_FALSE(ShouldInstallBuiltPedalboard(state));

    state.isCurrent = true;
    state.currentAudioEpoch = 5; // the audio thread that ran the borrowed effects was stopped.
    REQUIRE_FALSE(ShouldInstallBuiltPedalboard(state));

    state.currentAudioEpoch = 4;
    state.currentConfigurationVersion = 2; // the audio configuration changed.
    REQUIRE_FALSE(ShouldInstallBuiltPedalboard(state));
}

TEST_CASE("Preset instance reuse: reused Lv2 instance is re-keyed and gets the new settings at the swap", "[preset_instance_reuse]")
{
    const char *A_EQ_URI = "urn:ardour:a-eq";
    const char *A_COMP_URI = "urn:ardour:a-comp";

    PluginHost host;
    host.LoadLilv("/usr/lib/lv2:/usr/local/lib/lv2:/usr/modep/lv2");
    if (!host.GetPluginInfo(A_EQ_URI) || !host.GetPluginInfo(A_COMP_URI))
    {
        WARN("x42/Ardour a-eq and a-comp LV2 plugins are not installed. Skipping.");
        return;
    }

    // Running pedalboard: a-eq with instance id 3.
    Pedalboard pedalboardA;
    PedalboardItem eqA = MakeItem(3, A_EQ_URI);
    eqA.controlValues().push_back(ControlValue("g1", 6.0f));
    eqA.isEnabled(true);
    pedalboardA.items().push_back(eqA);

    Lv2PedalboardErrorList errors;
    std::shared_ptr<Lv2Pedalboard> lv2PedalboardA{host.CreateLv2Pedalboard(pedalboardA, errors)};
    IEffect *effect = lv2PedalboardA->GetEffect(3);
    REQUIRE(effect != nullptr);
    int g1 = effect->GetControlIndex("g1");
    int freq1 = effect->GetControlIndex("freq1");
    REQUIRE(g1 >= 0);
    REQUIRE(freq1 >= 0);
    REQUIRE(effect->GetControlValue(g1) == 6.0f);

    // New pedalboard (another preset): a-comp with instance id 3 (ids collide across presets), and
    // a-eq with instance id 7, which reuses the running a-eq.
    Pedalboard pedalboardB;
    PedalboardItem comp = MakeItem(3, A_COMP_URI);
    PedalboardItem eqB = MakeItem(7, A_EQ_URI);
    eqB.controlValues().push_back(ControlValue("g1", -3.0f));
    eqB.isEnabled(false);
    pedalboardB.items().push_back(comp);
    pedalboardB.items().push_back(eqB);

    ExistingEffectMap reusable;
    reusable[7] = lv2PedalboardA->GetSharedEffectList()[0];
    std::shared_ptr<Lv2Pedalboard> lv2PedalboardB{host.CreateLv2PedalboardReusingInstances(pedalboardB, reusable, errors)};

    REQUIRE(lv2PedalboardB->GetBorrowedEffectCount() == 1);
    REQUIRE(reusable.empty()); // borrowed at most once.

    // In the new pedalboard, routing by the new instance id reaches the reused instance.
    REQUIRE(lv2PedalboardB->GetEffect(7) == effect);
    REQUIRE(lv2PedalboardB->GetIndexOfInstanceId(7) == 1);
    REQUIRE(lv2PedalboardB->GetInstanceIdAt(1) == 7);
    REQUIRE(lv2PedalboardB->GetControlIndex(7, "g1") == g1);
    // instance id 3 is the new a-comp instance, not the reused a-eq.
    REQUIRE(lv2PedalboardB->GetEffect(3) != nullptr);
    REQUIRE(lv2PedalboardB->GetEffect(3) != effect);

    // The outgoing pedalboard keeps running unchanged (old id, old settings) until the new one is swapped in.
    REQUIRE(lv2PedalboardA->GetEffect(3) == effect);
    REQUIRE(lv2PedalboardA->GetIndexOfInstanceId(7) == -1);
    REQUIRE(effect->GetInstanceId() == 3);
    REQUIRE(effect->GetControlValue(g1) == 6.0f);
    REQUIRE(effect->GetControlValue(-1) == 1.0f);

    REQUIRE(((Lv2Effect *)effect)->IsBorrowedEffect() == false); // staged only.
    lv2PedalboardB->UpdateAudioPorts(); // what the audio thread does when it swaps the pedalboard in.
    // handed over: an ordinary effect of the new pedalboard again.
    REQUIRE(((Lv2Effect *)effect)->IsBorrowedEffect() == false);

    REQUIRE(effect->GetInstanceId() == 7);
    REQUIRE(effect->GetControlValue(g1) == -3.0f);
    // controls the new item doesn't set get their default values.
    REQUIRE(effect->GetControlValue(freq1) == effect->GetDefaultInputControlValue(freq1));
    REQUIRE(effect->GetControlValue(-1) == 0.0f); // disabled.

    // Settings are applied once only (later edits go through the normal realtime paths).
    effect->SetControl(g1, 1.0f);
    lv2PedalboardB->UpdateAudioPorts();
    REQUIRE(effect->GetControlValue(g1) == 1.0f);
}

TEST_CASE("Preset instance reuse: a plugin's enable port follows the new item's enabled state", "[preset_instance_reuse]")
{
    const char *A_COMP_URI = "urn:ardour:a-comp"; // has an lv2:enabled port ("enable").

    PluginHost host;
    host.LoadLilv("/usr/lib/lv2:/usr/local/lib/lv2:/usr/modep/lv2");
    if (!host.GetPluginInfo(A_COMP_URI))
    {
        WARN("x42/Ardour a-comp LV2 plugin is not installed. Skipping.");
        return;
    }
    auto runDisabledTo = [&](bool newEnabled)
    {
        Pedalboard pedalboardA;
        PedalboardItem compA = MakeItem(1, A_COMP_URI);
        compA.isEnabled(false);
        pedalboardA.items().push_back(compA);
        Lv2PedalboardErrorList errors;
        std::shared_ptr<Lv2Pedalboard> lv2PedalboardA{host.CreateLv2Pedalboard(pedalboardA, errors)};
        lv2PedalboardA->Activate(); // the enable port is set from the item's enabled state on activation.
        IEffect *effect = lv2PedalboardA->GetEffect(1);
        int enable = effect->GetControlIndex("enable");
        REQUIRE(enable >= 0);
        REQUIRE(effect->GetControlValue(enable) == 0.0f);

        Pedalboard pedalboardB;
        PedalboardItem compB = MakeItem(9, A_COMP_URI);
        compB.isEnabled(newEnabled); // (no value for "enable": its default is 1)
        pedalboardB.items().push_back(compB);
        ExistingEffectMap reusable;
        reusable[9] = lv2PedalboardA->GetSharedEffectList()[0];
        std::shared_ptr<Lv2Pedalboard> lv2PedalboardB{host.CreateLv2PedalboardReusingInstances(pedalboardB, reusable, errors)};
        REQUIRE(lv2PedalboardB->GetEffect(9) == effect);
        lv2PedalboardB->UpdateAudioPorts();
        float result = effect->GetControlValue(enable);
        lv2PedalboardB->Deactivate();
        return result;
    };
    REQUIRE(runDisabledTo(false) == 0.0f);
    REQUIRE(runDisabledTo(true) == 1.0f);
}

TEST_CASE("Preset instance reuse: same-item borrowing keeps instance id and settings", "[preset_instance_reuse]")
{
    const char *A_EQ_URI = "urn:ardour:a-eq";

    PluginHost host;
    host.LoadLilv("/usr/lib/lv2:/usr/local/lib/lv2:/usr/modep/lv2");
    if (!host.GetPluginInfo(A_EQ_URI))
    {
        WARN("x42/Ardour a-eq LV2 plugin is not installed. Skipping.");
        return;
    }
    Pedalboard pedalboard;
    PedalboardItem eq = MakeItem(3, A_EQ_URI);
    eq.controlValues().push_back(ControlValue("g1", 6.0f));
    pedalboard.items().push_back(eq);

    Lv2PedalboardErrorList errors;
    std::shared_ptr<Lv2Pedalboard> original{host.CreateLv2Pedalboard(pedalboard, errors)};
    IEffect *effect = original->GetEffect(3);
    int g1 = effect->GetControlIndex("g1");
    effect->SetControl(g1, 2.0f); // live value (e.g. a realtime control change).

    std::shared_ptr<Lv2Pedalboard> updated{host.UpdateLv2PedalboardStructure(pedalboard, original.get(), errors)};
    REQUIRE(updated->GetBorrowedEffectCount() == 1);
    REQUIRE(updated->GetEffect(3) == effect);
    REQUIRE(effect->GetInstanceId() == 3);
    updated->UpdateAudioPorts();
    REQUIRE(effect->GetControlValue(g1) == 2.0f);
}

namespace
{
    // A mono plugin without an lv2:enabled port: when bypassed, Lv2Effect itself copies its input buffer to its
    // output buffer (MixOutput), through the buffer pointers that borrowing re-points.
    const char *BOOSTER_URI = "http://guitarix.sourceforge.net/plugins/gxbooster#booster";
    constexpr uint32_t FRAMES = 64;

    void Fill(float *buffer, float value)
    {
        for (uint32_t i = 0; i < FRAMES; ++i)
        {
            buffer[i] = value;
        }
    }
    bool AllEqual(const float *buffer, float value)
    {
        for (uint32_t i = 0; i < FRAMES; ++i)
        {
            if (buffer[i] != value)
            {
                return false;
            }
        }
        return true;
    }

    struct BypassedRunningEffect
    {
        std::shared_ptr<Lv2Pedalboard> pedalboard;
        Pedalboard description;
        Lv2Effect *effect = nullptr;
        float *input = nullptr;
        float *output = nullptr;
    };
    BypassedRunningEffect MakeBypassedRunningEffect(PluginHost &host)
    {
        BypassedRunningEffect result;
        PedalboardItem booster = MakeItem(3, BOOSTER_URI);
        booster.isEnabled(false);
        result.description.items().push_back(booster);
        Lv2PedalboardErrorList errors;
        result.pedalboard = std::shared_ptr<Lv2Pedalboard>(host.CreateLv2Pedalboard(result.description, errors));
        result.pedalboard->Activate();
        IEffect *effect = result.pedalboard->GetEffect(3);
        REQUIRE(effect != nullptr);
        REQUIRE(effect->IsLv2Effect());
        result.effect = (Lv2Effect *)effect;
        REQUIRE(result.effect->BypassControlIndex() == -1);
        result.input = result.pedalboard->GetInputBuffers()[0];
        result.output = result.pedalboard->GetoutputBuffers()[0];
        REQUIRE(result.effect->GetAudioInputBuffer(0) == result.input);
        REQUIRE(result.effect->GetAudioOutputBuffer(0) == result.output);
        return result;
    }
}

TEST_CASE("Preset instance reuse: a borrowed effect runs on the old pedalboard's buffers until the swap", "[preset_instance_reuse]")
{
    PluginHost host;
    host.LoadLilv("/usr/lib/lv2:/usr/local/lib/lv2:/usr/modep/lv2");
    if (!host.GetPluginInfo(BOOSTER_URI))
    {
        WARN("guitarix gxbooster LV2 plugin is not installed. Skipping.");
        return;
    }
    auto check = [&](bool reuseForNewItem)
    {
        BypassedRunningEffect running = MakeBypassedRunningEffect(host);
        Lv2Effect *effect = running.effect;

        // Build the next pedalboard while the effect "runs" in the current one.
        Lv2PedalboardErrorList errors;
        std::shared_ptr<Lv2Pedalboard> next;
        Pedalboard nextDescription;
        if (reuseForNewItem)
        {
            PedalboardItem booster = MakeItem(7, BOOSTER_URI);
            booster.isEnabled(false);
            nextDescription.items().push_back(booster);
            ExistingEffectMap reusable;
            reusable[7] = running.pedalboard->GetSharedEffectList()[0];
            next = std::shared_ptr<Lv2Pedalboard>(host.CreateLv2PedalboardReusingInstances(nextDescription, reusable, errors));
        }
        else
        {
            nextDescription = running.description;
            next = std::shared_ptr<Lv2Pedalboard>(host.UpdateLv2PedalboardStructure(nextDescription, running.pedalboard.get(), errors));
        }
        REQUIRE(next->GetBorrowedEffectCount() == 1);
        float *newInput = next->GetInputBuffers()[0];
        float *newOutput = next->GetoutputBuffers()[0];
        REQUIRE(newInput != running.input);
        REQUIRE(newOutput != running.output);

        // Until the swap, the old pedalboard's run of the (bypassed) effect reads and writes the old buffers.
        REQUIRE(effect->GetAudioInputBuffer(0) == running.input);
        REQUIRE(effect->GetAudioOutputBuffer(0) == running.output);
        REQUIRE(effect->GetInstanceId() == 3);
        Fill(running.input, 0.5f);
        Fill(running.output, 0.0f);
        Fill(newInput, 0.0f);
        Fill(newOutput, -1.0f);
        effect->Run(FRAMES, nullptr, false);
        REQUIRE(AllEqual(running.output, 0.5f)); // passed through to the old chain.
        REQUIRE(AllEqual(newOutput, -1.0f));     // the new pedalboard's buffers are untouched.

        next->UpdateAudioPorts(); // the swap, on the audio thread.

        REQUIRE(effect->GetAudioInputBuffer(0) == newInput);
        REQUIRE(effect->GetAudioOutputBuffer(0) == newOutput);
        REQUIRE(effect->GetInstanceId() == (reuseForNewItem ? 7u : 3u));
        Fill(newInput, 0.25f);
        Fill(running.output, -1.0f);
        effect->Run(FRAMES, nullptr, false);
        REQUIRE(AllEqual(newOutput, 0.25f));
        REQUIRE(AllEqual(running.output, -1.0f));
    };
    SECTION("reused for a new item (preset switch)")
    {
        check(true);
    }
    SECTION("same item (structural edit)")
    {
        check(false);
    }
}

TEST_CASE("Preset instance reuse: a discarded borrowing build leaves the running effect unchanged", "[preset_instance_reuse]")
{
    PluginHost host;
    host.LoadLilv("/usr/lib/lv2:/usr/local/lib/lv2:/usr/modep/lv2");
    if (!host.GetPluginInfo(BOOSTER_URI))
    {
        WARN("guitarix gxbooster LV2 plugin is not installed. Skipping.");
        return;
    }
    BypassedRunningEffect running = MakeBypassedRunningEffect(host);
    Lv2Effect *effect = running.effect;
    {
        Pedalboard nextDescription;
        PedalboardItem booster = MakeItem(7, BOOSTER_URI);
        booster.isEnabled(true);
        booster.controlValues().push_back(ControlValue("fslider0_", 3.0f));
        nextDescription.items().push_back(booster);
        ExistingEffectMap reusable;
        reusable[7] = running.pedalboard->GetSharedEffectList()[0];
        Lv2PedalboardErrorList errors;
        std::shared_ptr<Lv2Pedalboard> next{host.CreateLv2PedalboardReusingInstances(nextDescription, reusable, errors)};
        REQUIRE(next->GetBorrowedEffectCount() == 1);
        // discarded (stale, or a later step of the build failed) without being swapped in.
    }
    REQUIRE(running.pedalboard->GetEffect(3) == effect);
    REQUIRE(effect->GetInstanceId() == 3);
    REQUIRE(effect->GetControlValue(-1) == 0.0f); // still bypassed.
    REQUIRE(effect->GetAudioInputBuffer(0) == running.input);
    REQUIRE(effect->GetAudioOutputBuffer(0) == running.output);
    Fill(running.input, 0.5f);
    Fill(running.output, 0.0f);
    effect->Run(FRAMES, nullptr, false);
    REQUIRE(AllEqual(running.output, 0.5f));
}

TEST_CASE("Preset instance reuse: a borrowing build that fails after staging leaves the running effect unchanged", "[preset_instance_reuse]")
{
    const char *A_COMP_URI = "urn:ardour:a-comp"; // has a sidechain input.

    PluginHost host;
    host.LoadLilv("/usr/lib/lv2:/usr/local/lib/lv2:/usr/modep/lv2");
    if (!host.GetPluginInfo(BOOSTER_URI) || !host.GetPluginInfo(A_COMP_URI))
    {
        WARN("guitarix gxbooster or x42/Ardour a-comp LV2 plugin is not installed. Skipping.");
        return;
    }
    auto check = [&](bool reuseForNewItem)
    {
        BypassedRunningEffect running = MakeBypassedRunningEffect(host);
        Lv2Effect *effect = running.effect;

        // The booster is borrowed (its wiring staged) first; then the a-comp, whose sidechain source
        // doesn't exist, makes Prepare() throw.
        Pedalboard nextDescription;
        PedalboardItem booster = MakeItem(reuseForNewItem ? 7 : 3, BOOSTER_URI);
        booster.isEnabled(true);
        booster.controlValues().push_back(ControlValue("fslider0_", 3.0f));
        nextDescription.items().push_back(booster);
        PedalboardItem comp = MakeItem(5, A_COMP_URI);
        comp.sideChainInputId(42);
        nextDescription.items().push_back(comp);

        Lv2PedalboardErrorList errors;
        if (reuseForNewItem)
        {
            ExistingEffectMap reusable;
            reusable[7] = running.pedalboard->GetSharedEffectList()[0];
            REQUIRE_THROWS(host.CreateLv2PedalboardReusingInstances(nextDescription, reusable, errors));
            REQUIRE(reusable.empty()); // the booster was borrowed before the failure.
        }
        else
        {
            REQUIRE_THROWS(host.UpdateLv2PedalboardStructure(nextDescription, running.pedalboard.get(), errors));
        }

        // The failed pedalboard (and its buffers) is gone; the running effect never saw them.
        REQUIRE_FALSE(effect->IsBorrowedEffect());
        REQUIRE(running.pedalboard->GetEffect(3) == effect);
        REQUIRE(effect->GetInstanceId() == 3);
        REQUIRE(effect->GetControlValue(-1) == 0.0f); // still bypassed.
        REQUIRE(effect->GetAudioInputBuffer(0) == running.input);
        REQUIRE(effect->GetAudioOutputBuffer(0) == running.output);
        Fill(running.input, 0.5f);
        Fill(running.output, 0.0f);
        effect->Run(FRAMES, nullptr, false);
        REQUIRE(AllEqual(running.output, 0.5f));

        // ... and it can still be borrowed by a later build.
        Pedalboard retryDescription = running.description;
        std::shared_ptr<Lv2Pedalboard> retry{host.UpdateLv2PedalboardStructure(retryDescription, running.pedalboard.get(), errors)};
        REQUIRE(retry->GetBorrowedEffectCount() == 1);
        retry->UpdateAudioPorts();
        REQUIRE(effect->GetAudioInputBuffer(0) == retry->GetInputBuffers()[0]);
        REQUIRE(effect->GetAudioOutputBuffer(0) == retry->GetoutputBuffers()[0]);
    };
    SECTION("reused for a new item (preset switch)")
    {
        check(true);
    }
    SECTION("same item (structural edit)")
    {
        check(false);
    }
}

TEST_CASE("Preset instance reuse: audio output port connection of new and borrowed effects", "[preset_instance_reuse]")
{
    // With buffer staging, every plugin writes to its output staging buffers (RunWithBufferStaging copies a
    // zero-input plugin's into its mix buffers), so new and borrowed instances (Lv2Effect::UpdateAudioPorts)
    // agree. Without staging, zero-input plugins write to their output mix buffers.
    REQUIRE(GetAudioOutputPortConnection(0, false) == AudioOutputPortConnection::MixBuffer);
    REQUIRE(GetAudioOutputPortConnection(0, true) == AudioOutputPortConnection::StagingBuffer);
    REQUIRE(GetAudioOutputPortConnection(1, true) == AudioOutputPortConnection::StagingBuffer);
    REQUIRE(GetAudioOutputPortConnection(2, true) == AudioOutputPortConnection::StagingBuffer);
    REQUIRE(GetAudioOutputPortConnection(1, false) == AudioOutputPortConnection::OutputBuffer);
    REQUIRE(GetAudioOutputPortConnection(2, false) == AudioOutputPortConnection::OutputBuffer);
}
