// Copyright (c) Robin E. R. Davies
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
#include "BypassSuspend.hpp"
#include "SplitEffect.hpp"
#include "lv2/atom/util.h"

#include <cmath>
#include <vector>

using namespace pipedal;

namespace
{
    bool ShouldSuspend(const BypassFader &fader, bool settingOn = true, bool hasEnabledPort = false, bool pendingInput = false)
    {
        return BypassSuspendPolicy::ShouldSuspendEffect(settingOn, hasEnabledPort, fader, pendingInput);
    }

    // The soft-bypass dezipper as it was implemented inline in Lv2Effect::MixOutput before
    // it was factored out into BypassFader. Used to check that the gain curve is unchanged.
    struct ReferenceDezipper
    {
        uint32_t startingSamples;
        double targetBypass = 0, currentBypass = 0, currentBypassDx = 0;
        uint32_t samplesRemaining = 0;

        void Set(double v)
        {
            targetBypass = currentBypass = v;
            currentBypassDx = 0;
            samplesRemaining = 0;
        }
        void To(double v)
        {
            targetBypass = v;
            double dx = v - currentBypass;
            if (dx != 0)
            {
                samplesRemaining = (int)(startingSamples * std::abs(dx));
                currentBypassDx = dx / samplesRemaining;
            }
        }
        void Process(std::vector<double> &gains, uint32_t samples)
        {
            if (samplesRemaining == 0)
            {
                for (uint32_t i = 0; i < samples; ++i)
                    gains.push_back(currentBypass);
                return;
            }
            double current = currentBypass;
            double dx = currentBypassDx;
            int32_t remaining = (int32_t)samplesRemaining;
            for (uint32_t i = 0; i < samples; ++i)
            {
                gains.push_back(current);
                if (--remaining == 0)
                {
                    dx = 0;
                    current = targetBypass;
                }
                current += dx;
            }
            if (remaining <= 0)
            {
                samplesRemaining = 0;
                currentBypass = targetBypass;
                currentBypassDx = 0;
            }
            else
            {
                currentBypass = current;
                currentBypassDx = dx;
                samplesRemaining = remaining;
            }
        }
    };
}

TEST_CASE("Bypass suspend: fade complete => suspended, re-enable => running with fade-in", "[bypass_suspend]")
{
    BypassFader fader;
    fader.SetFadeSamples(100);
    fader.Set(1); // enabled, running.

    REQUIRE_FALSE(ShouldSuspend(fader));

    // bypass requested: fading out; must keep running for the whole fade.
    fader.FadeTo(0);
    REQUIRE(fader.IsFading());
    REQUIRE_FALSE(ShouldSuspend(fader));
    fader.Advance(99);
    REQUIRE(fader.IsFading());
    REQUIRE_FALSE(ShouldSuspend(fader));
    fader.Advance(1);
    REQUIRE_FALSE(fader.IsFading());
    REQUIRE(fader.Value() == 0);

    // fade complete: suspended.
    REQUIRE(ShouldSuspend(fader));
    // ... but only when the setting is on.
    REQUIRE_FALSE(ShouldSuspend(fader, false));
    // ... never for plugins with an lv2:enabled port (they handle bypass themselves).
    REQUIRE_FALSE(ShouldSuspend(fader, true, true));
    // ... and not on a cycle that has pending atom input (patch:Set etc. must be delivered).
    REQUIRE_FALSE(ShouldSuspend(fader, true, false, true));

    // stays suspended.
    fader.Advance(1000);
    REQUIRE(ShouldSuspend(fader));

    // enable requested: running again immediately, fading in from dry.
    fader.FadeTo(1);
    REQUIRE_FALSE(ShouldSuspend(fader));
    REQUIRE(fader.Tick() == 0.0); // first sample is still fully dry.
    double previous = 0;
    for (int i = 0; i < 99; ++i)
    {
        double g = fader.Tick();
        REQUIRE(g > previous);
        previous = g;
    }
    REQUIRE_FALSE(fader.IsFading());
    REQUIRE(fader.Value() == 1);
    REQUIRE_FALSE(ShouldSuspend(fader));
}

TEST_CASE("Bypass suspend: reversing a fade before it completes never suspends", "[bypass_suspend]")
{
    BypassFader fader;
    fader.SetFadeSamples(100);
    fader.Set(1);
    fader.FadeTo(0);
    fader.Advance(40);
    REQUIRE_FALSE(ShouldSuspend(fader));
    fader.FadeTo(1); // re-enabled mid fade.
    REQUIRE(fader.IsFading());
    fader.Advance(1000);
    REQUIRE(fader.Value() == 1);
    REQUIRE_FALSE(ShouldSuspend(fader));
}

TEST_CASE("Bypass suspend: plugin loaded bypassed is suspended immediately", "[bypass_suspend]")
{
    BypassFader fader;
    fader.SetFadeSamples(100);
    fader.Set(0); // Activate() of a disabled plugin.
    REQUIRE(ShouldSuspend(fader));
    REQUIRE_FALSE(ShouldSuspend(fader, false));
}

TEST_CASE("Bypass suspend: fader gain curve matches the original dezipper", "[bypass_suspend]")
{
    BypassFader fader;
    fader.SetFadeSamples(4800);
    ReferenceDezipper ref{4800};
    fader.Set(1);
    ref.Set(1);

    std::vector<double> expected;
    std::vector<double> actual;
    auto run = [&](uint32_t samples)
    {
        ref.Process(expected, samples);
        for (uint32_t i = 0; i < samples; ++i)
        {
            if (fader.IsFading())
                actual.push_back(fader.Tick());
            else
                actual.push_back(fader.Value());
        }
    };
    fader.FadeTo(0);
    ref.To(0);
    run(1000);
    fader.FadeTo(1); // reverse mid-fade
    ref.To(1);
    run(64);
    run(5000);
    fader.FadeTo(0);
    ref.To(0);
    run(6000);
    REQUIRE(actual.size() == expected.size());
    for (size_t i = 0; i < actual.size(); ++i)
    {
        REQUIRE(actual[i] == expected[i]);
    }
    REQUIRE(fader.IsFullyBypassed());
}

TEST_CASE("Bypass suspend: tiny fade snaps to target", "[bypass_suspend]")
{
    BypassFader fader;
    fader.SetFadeSamples(100);
    fader.Set(0.001);
    fader.FadeTo(0); // 100*0.001 = 0 samples: must snap, not divide by zero.
    REQUIRE_FALSE(fader.IsFading());
    REQUIRE(fader.Value() == 0);
    REQUIRE(std::isfinite(fader.Tick()));

    BypassFader noFade; // fade samples == 0
    noFade.Set(1);
    noFade.FadeTo(0);
    REQUIRE(noFade.IsFullyBypassed());
}

TEST_CASE("Bypass suspend: split branch with blend 0 is suspended after the transition", "[bypass_suspend]")
{
    const double sampleRate = 1000; // 0.1s transition = 100 samples.
    const uint32_t transition = 100;
    std::vector<float> in(256, 0.5f), topIn(256), bottomIn(256), topOut(256, 0.25f), bottomOut(256, 0.75f), out(256);

    std::vector<float *> inputs{in.data()};
    SplitEffect split(1, sampleRate, inputs);
    split.SetChainBuffers({topIn.data()}, {bottomIn.data()}, {topOut.data()}, {bottomOut.data()}, false);
    split.SetAudioOutputBuffer(0, out.data());
    split.Activate(); // A/B, A selected.

    // A selected: bottom (B) branch is silent, top is not.
    REQUIRE(split.IsBranchSilent(false));
    REQUIRE_FALSE(split.IsBranchSilent(true));
    REQUIRE(BypassSuspendPolicy::ShouldSuspendBranch(true, split.IsBranchSilent(false), false, false));
    REQUIRE_FALSE(BypassSuspendPolicy::ShouldSuspendBranch(false, split.IsBranchSilent(false), false, false));
    REQUIRE_FALSE(BypassSuspendPolicy::ShouldSuspendBranch(true, split.IsBranchSilent(false), true, false));
    REQUIRE_FALSE(BypassSuspendPolicy::ShouldSuspendBranch(true, split.IsBranchSilent(false), false, true)); // contains a sidechain source.

    // select B: both branches must run during the crossfade.
    split.SetControl(SplitEffect::SELECT_CTL, 1);
    REQUIRE_FALSE(split.IsBranchSilent(false));
    REQUIRE_FALSE(split.IsBranchSilent(true));

    split.PreMix(transition - 1);
    split.PostMix(transition - 1);
    REQUIRE_FALSE(split.IsBranchSilent(false));
    REQUIRE_FALSE(split.IsBranchSilent(true));

    split.PreMix(1);
    split.PostMix(1);
    // transition complete: now A (top) is silent.
    REQUIRE(split.IsBranchSilent(true));
    REQUIRE_FALSE(split.IsBranchSilent(false));

    // output is pure B.
    split.PostMix(16);
    for (size_t i = 0; i < 16; ++i)
    {
        REQUIRE(out[i] == 0.75f);
    }

    // Mix mode at center: neither branch is silent.
    split.SetControl(SplitEffect::SPLIT_TYPE_CTL, 1);
    split.SetControl(SplitEffect::MIX_CTL, 0.5f);
    split.PostMix(transition * 2);
    REQUIRE_FALSE(split.IsBranchSilent(true));
    REQUIRE_FALSE(split.IsBranchSilent(false));
}

namespace
{
    struct TestGate
    {
        size_t begin;
        size_t end;
        int id;
    };
    std::vector<size_t> RunGated(size_t nActions, const std::vector<TestGate> &gates, const std::vector<int> &skipIds)
    {
        std::vector<size_t> ran;
        RunGatedActions(
            nActions, gates.data(), gates.size(),
            [&](size_t i)
            { ran.push_back(i); },
            [&](const TestGate &gate)
            {
                for (int id : skipIds)
                    if (id == gate.id)
                        return true;
                return false;
            });
        return ran;
    }
}

TEST_CASE("Bypass suspend: gated action ranges", "[bypass_suspend]")
{
    // 0: outer PreMix; [1,5) outer top, containing inner split: 1 PreMix, [2,3) inner top, [3,4) inner bottom, 4 PostMix;
    // [5,7) outer bottom; 7 outer PostMix; 8 trailing effect.
    std::vector<TestGate> gates{
        {1, 5, 1}, // outer top
        {2, 3, 3}, // inner top
        {3, 4, 4}, // inner bottom
        {5, 7, 2}, // outer bottom
    };
    using V = std::vector<size_t>;
    REQUIRE(RunGated(9, gates, {}) == V{0, 1, 2, 3, 4, 5, 6, 7, 8});
    REQUIRE(RunGated(9, gates, {2}) == V{0, 1, 2, 3, 4, 7, 8});
    REQUIRE(RunGated(9, gates, {1}) == V{0, 5, 6, 7, 8});
    // a nested gate marked for skipping is irrelevant when its parent is skipped.
    REQUIRE(RunGated(9, gates, {1, 4}) == V{0, 5, 6, 7, 8});
    REQUIRE(RunGated(9, gates, {4}) == V{0, 1, 2, 4, 5, 6, 7, 8});
    REQUIRE(RunGated(9, gates, {3, 2}) == V{0, 1, 3, 4, 7, 8});

    // empty branch (begin == end) is never "skipped" and doesn't disturb the walk.
    std::vector<TestGate> emptyTop{{1, 1, 1}, {1, 3, 2}};
    REQUIRE(RunGated(4, emptyTop, {1}) == V{0, 1, 2, 3});
    REQUIRE(RunGated(4, emptyTop, {2}) == V{0, 3});
}

TEST_CASE("Bypass suspend: skipped plugin's Chunk-reset atom output becomes an empty sequence", "[bypass_suspend]")
{
    const uint32_t atom__Chunk = 7;
    const uint32_t atom__Sequence = 11;
    const uint32_t capacity = 1024;

    // What ResetAtomBuffers() leaves behind (a full-capacity Chunk) over stale bytes
    // that look like a patch:Set event from an earlier cycle.
    std::vector<uint64_t> storage(capacity / sizeof(uint64_t), 0x0000002000000010ull);
    LV2_Atom *atom = (LV2_Atom *)storage.data();
    atom->size = capacity - 8;
    atom->type = atom__Chunk;

    WriteEmptyAtomSequence(storage.data(), atom__Sequence);

    LV2_Atom_Sequence *sequence = (LV2_Atom_Sequence *)storage.data();
    REQUIRE(sequence->atom.type == atom__Sequence);
    REQUIRE(sequence->atom.size == sizeof(LV2_Atom_Sequence_Body));
    size_t events = 0;
    LV2_ATOM_SEQUENCE_FOREACH(sequence, ev)
    {
        (void)ev;
        ++events;
    }
    REQUIRE(events == 0);
}

namespace
{
    struct PedalboardGate
    {
        size_t begin;
        size_t end;
        size_t effectBegin;
        size_t effectEnd;
        bool silent;
        bool containsSidechainSource = false;
    };
}

TEST_CASE("Bypass suspend: skipped branch empties the atom outputs of every effect in it, nested ones included", "[bypass_suspend]")
{
    // effects: 0 (outer top, before inner split), 1 (inner top), 2 (inner bottom), 3 (outer bottom), 4 (after).
    // actions: one per effect + split pre/post mixes (as Lv2Pedalboard::PrepareItems lays them out).
    // 0 outerPre, 1 e0, 2 innerPre, 3 e1, 4 e2, 5 innerPost, 6 e3, 7 outerPost, 8 e4
    std::vector<PedalboardGate> gates{
        {1, 6, 0, 3, true},   // outer top (silent): contains e0, inner split (e1, e2).
        {3, 4, 1, 2, false},  // inner top
        {4, 5, 2, 3, false},  // inner bottom
        {6, 7, 3, 4, false},  // outer bottom
    };
    const size_t nEffects = 5;
    const int actionEffect[] = {-1, 0, -1, 1, 2, -1, 3, -1, 4};

    // atom output state per effect: true = valid sequence written this cycle.
    std::vector<bool> outputValid(nEffects, false);
    std::vector<bool> ran(nEffects, false);

    RunGatedActions(
        9, gates.data(), gates.size(),
        [&](size_t i)
        {
            int e = actionEffect[i];
            if (e >= 0)
            {
                ran[e] = true;
                outputValid[e] = true; // plugin run() writes a sequence.
            }
        },
        [&](const PedalboardGate &gate)
        {
            // mirrors Lv2Pedalboard::TrySkipBranch.
            bool skip = BypassSuspendPolicy::ShouldSuspendBranch(true, gate.silent, false, gate.containsSidechainSource);
            if (skip)
            {
                for (size_t e = gate.effectBegin; e < gate.effectEnd; ++e)
                {
                    outputValid[e] = true; // WriteEmptyOutputAtomBuffers()
                }
            }
            return skip;
        });
    REQUIRE_FALSE(ran[0]);
    REQUIRE_FALSE(ran[1]);
    REQUIRE_FALSE(ran[2]);
    REQUIRE(ran[3]);
    REQUIRE(ran[4]);
    for (size_t e = 0; e < nEffects; ++e)
    {
        REQUIRE(outputValid[e]); // nobody is left with a Chunk-reset output buffer.
    }
}

TEST_CASE("Bypass suspend: a branch containing a sidechain source is never skipped", "[bypass_suspend]")
{
    // outer top branch has effects [0,3) (inner split effects 1, 2 nested); outer bottom [3,4).
    auto makeGates = []()
    {
        return std::vector<PedalboardGate>{
            {1, 6, 0, 3, true},
            {3, 4, 1, 2, true},
            {4, 5, 2, 3, true},
            {6, 7, 3, 4, true},
        };
    };

    SECTION("gates exist when the sidechain is connected")
    {
        auto gates = makeGates();
        PinGatesContainingEffect(gates, 2); // effect 2 (inner bottom) feeds a sidechain.
        REQUIRE(gates[0].containsSidechainSource);  // outer top contains it.
        REQUIRE_FALSE(gates[1].containsSidechainSource);
        REQUIRE(gates[2].containsSidechainSource);  // inner bottom contains it.
        REQUIRE_FALSE(gates[3].containsSidechainSource);
    }
    SECTION("gate created after the sidechain is connected")
    {
        std::vector<size_t> sources{2};
        PedalboardGate outerTop{1, 6, 0, 3, true};
        PedalboardGate outerBottom{6, 7, 3, 4, true};
        PinGateIfContainsAny(outerTop, sources);
        PinGateIfContainsAny(outerBottom, sources);
        REQUIRE(outerTop.containsSidechainSource);
        REQUIRE_FALSE(outerBottom.containsSidechainSource);
    }
    SECTION("pinned gates run even when silent")
    {
        auto gates = makeGates();
        PinGatesContainingEffect(gates, 2);
        std::vector<size_t> ranActions;
        RunGatedActions(
            9, gates.data(), gates.size(),
            [&](size_t i)
            { ranActions.push_back(i); },
            [&](const PedalboardGate &gate)
            { return BypassSuspendPolicy::ShouldSuspendBranch(true, gate.silent, false, gate.containsSidechainSource); });
        // outer top and inner bottom pinned; inner top (silent, no source) and outer bottom skipped.
        REQUIRE(ranActions == std::vector<size_t>{0, 1, 2, 4, 5, 7, 8});
    }
}
