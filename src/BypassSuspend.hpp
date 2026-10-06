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
#include <cstdlib>
#include <vector>
#include "lv2/atom/atom.h"

/*
    "Suspend bypassed plugins" (opt-in, default OFF).

    When enabled, a plugin whose soft-bypass crossfade has fully completed is no
    longer run (lilv_instance_run is skipped); its output is the dry input, which
    is exactly what the bypass path produces anyway. Likewise, a split branch whose
    blend gain is exactly 0 (after the blend transition has completed) is not run at all.

    Resuming: when the plugin is re-enabled, it simply starts running again and
    is crossfaded in by the normal bypass fade. The plugin is NOT deactivated/
    re-activated (those calls are not realtime-safe), so its internal state
    (delay lines, reverb tails, NAM/convolution history) is whatever it was when it
    was suspended; a stale tail may be heard briefly as the plugin fades back in.
    That is why the option is off by default.

    Exclusions:
    - Plugins that declare an lv2:enabled (host-controlled bypass) port implement
      their own bypass (and may need to keep running to fade out or ring out), so
      PiPedal never applies its own soft bypass to them; they are never suspended.
    - A plugin (or a split branch containing one) that has pending atom input this
      cycle (patch:Set/patch:Get, MIDI) is run for that cycle, so that messages such
      as a model-file change are not lost while it is suspended.
    - A split branch containing the sidechain source of some plugin is never skipped
      (the consumer may be audible outside the branch, and would otherwise be keyed
      from a repeated stale block).

    Skipped plugins don't write their atom output buffers (which the host resets to
    an empty Chunk every cycle); the host writes an empty Sequence into them instead,
    so that nothing downstream (RelayPatchSetMessages, GatherPathPatchProperties)
    walks stale bytes.

    Known, accepted side effects while suspended:
    - Output control ports (meters, tuner readouts) and VU/monitor subscriptions
      freeze at their last values.
    - Trigger controls are still reset to their defaults each cycle (by the
      pedalboard's trigger-reset actions for a lone suspended plugin), so a trigger
      fired while suspended is consumed without the plugin seeing it (a skipped
      branch skips those resets too).
    - Worker responses are still delivered (work_response) without run(): a plugin
      that swaps state at the start of run() (e.g. NAM model loaders) defers the
      swap until it resumes.

    Everything here is realtime-safe: plain arithmetic on members, no allocation.
*/

namespace pipedal
{
    // Soft-bypass gain dezipper: 1 = plugin output, 0 = dry input.
    class BypassFader
    {
    public:
        void SetFadeSamples(uint32_t samples) { fadeSamples = samples; }
        uint32_t GetFadeSamples() const { return fadeSamples; }

        // Jump to the value immediately (no fade).
        void Set(double value)
        {
            target = value;
            current = value;
            dx = 0;
            samplesRemaining = 0;
        }
        // Fade linearly to value over fadeSamples * |delta| samples.
        void FadeTo(double value)
        {
            target = value;
            double delta = value - current;
            if (delta == 0)
            {
                return;
            }
            samplesRemaining = (uint32_t)(fadeSamples * std::abs(delta));
            if (samplesRemaining == 0)
            {
                current = target;
                dx = 0;
            }
            else
            {
                dx = delta / samplesRemaining;
            }
        }

        // Returns the gain for the current sample, and advances one sample.
        inline double Tick()
        {
            double result = current;
            if (samplesRemaining != 0)
            {
                if (--samplesRemaining == 0)
                {
                    dx = 0;
                    current = target;
                }
                else
                {
                    current += dx;
                }
            }
            return result;
        }
        // Advance n samples without producing output (used by tests).
        void Advance(uint32_t samples)
        {
            while (samples-- != 0 && samplesRemaining != 0)
            {
                Tick();
            }
        }

        bool IsFading() const { return samplesRemaining != 0; }
        double Value() const { return current; }
        double Target() const { return target; }
        uint32_t SamplesRemaining() const { return samplesRemaining; }

        // The bypass crossfade has fully completed and the output is 100% dry input.
        bool IsFullyBypassed() const { return samplesRemaining == 0 && current == 0; }

    private:
        uint32_t fadeSamples = 0;
        double target = 0;
        double current = 0;
        double dx = 0;
        uint32_t samplesRemaining = 0;
    };

    // Policy decisions, isolated so they can be unit tested.
    struct BypassSuspendPolicy
    {
        // Should an Lv2 effect skip lilv_instance_run() this cycle?
        static bool ShouldSuspendEffect(
            bool suspendBypassedPlugins,
            bool pluginHasEnabledPort, // lv2:enabled designation: the plugin handles bypass itself.
            const BypassFader &fader,
            bool hasPendingAtomInput)
        {
            if (!suspendBypassedPlugins)
                return false;
            if (pluginHasEnabledPort)
                return false;
            if (hasPendingAtomInput)
                return false;
            return fader.IsFullyBypassed();
        }

        // Should the action range of a split branch be skipped this cycle?
        static bool ShouldSuspendBranch(
            bool suspendBypassedPlugins,
            bool branchIsSilent, // blend gain(s) for the branch are exactly 0, and no blend transition in progress.
            bool hasPendingAtomInput,
            bool branchContainsSidechainSource)
        {
            return suspendBypassedPlugins && branchIsSilent && !hasPendingAtomInput && !branchContainsSidechainSource;
        }
    };

    // Write an empty atom sequence into an atom port buffer (for a plugin whose run() was skipped). RT-safe.
    inline void WriteEmptyAtomSequence(void *buffer, uint32_t atom__Sequence)
    {
        LV2_Atom_Sequence *sequence = (LV2_Atom_Sequence *)buffer;
        sequence->atom.size = sizeof(LV2_Atom_Sequence_Body);
        sequence->atom.type = atom__Sequence;
        sequence->body.unit = 0;
        sequence->body.pad = 0;
    }

    /*
        Sidechain pinning (non-RT, at prepare time).

        GATE must have size_t effectBegin, effectEnd, and bool containsSidechainSource.
        Sidechain sources are always prepared before their consumers, but a source's
        enclosing split gate may be created before or after the consumer is connected,
        so both orders are handled:
        - when a sidechain is connected: PinGatesContainingEffect(gates, sourceIndex)
        - when a gate is created: PinGateIfContainsAny(gate, sidechainSourceIndices)
    */
    template <typename GATE>
    inline void PinGatesContainingEffect(std::vector<GATE> &gates, size_t effectIndex)
    {
        for (auto &gate : gates)
        {
            if (effectIndex >= gate.effectBegin && effectIndex < gate.effectEnd)
            {
                gate.containsSidechainSource = true;
            }
        }
    }
    template <typename GATE>
    inline void PinGateIfContainsAny(GATE &gate, const std::vector<size_t> &effectIndices)
    {
        for (size_t effectIndex : effectIndices)
        {
            if (effectIndex >= gate.effectBegin && effectIndex < gate.effectEnd)
            {
                gate.containsSidechainSource = true;
                return;
            }
        }
    }

    /*
        Run a flat list of process actions, skipping gated ranges.

        gates: half-open action ranges [begin,end) (split branches), sorted by begin.
        Gates may nest (a split inside a branch of another split); when a gate is
        skipped, every gate nested inside it is skipped with it.

        runAction(i) runs action i; shouldSkip(gate) decides whether a gate is skipped.
        RT-safe, provided the callbacks are.
    */
    template <typename GATE, typename RUN_ACTION, typename SHOULD_SKIP>
    inline void RunGatedActions(
        size_t nActions,
        const GATE *gates,
        size_t nGates,
        RUN_ACTION &&runAction,
        SHOULD_SKIP &&shouldSkip)
    {
        size_t g = 0;
        size_t i = 0;
        while (i < nActions)
        {
            bool skipped = false;
            while (g < nGates && gates[g].begin <= i)
            {
                const GATE &gate = gates[g];
                ++g;
                if (gate.begin == i && gate.end > i && shouldSkip(gate))
                {
                    i = gate.end;
                    // drop gates nested inside the skipped range.
                    while (g < nGates && gates[g].begin < i)
                    {
                        ++g;
                    }
                    skipped = true;
                    break;
                }
            }
            if (skipped)
            {
                continue;
            }
            runAction(i);
            ++i;
        }
    }
}
