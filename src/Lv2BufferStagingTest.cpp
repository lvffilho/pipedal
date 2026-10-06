// Copyright (c) 2026 Robin Davies
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


// Lv2Effect buffer staging (plugins that need a block length other than the host's buffer size). No installed
// plugin requires it, so the tests force it: they instantiate the plugin with a copy of its Lv2PluginInfo whose
// maxBlockLength is smaller than the host's maximum buffer size, or whose minBlockLength is larger. Staged output
// is compared with the output of an unstaged instance of the same plugin.

#include "pch.h"
#include "catch.hpp"
#include "PluginHost.hpp"
#include "Lv2Effect.hpp"
#include "lv2/atom/atom.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace pipedal;

namespace
{
    // mono, sample-by-sample DSP: its output doesn't depend on the block length.
    const char *BOOSTER_URI = "http://guitarix.sourceforge.net/plugins/gxbooster#booster";
    const char *A_COMP_URI = "urn:ardour:a-comp"; // has a sidechain input.

    struct StagingCase
    {
        size_t hostFrames; // the host's maximum buffer size, and every host cycle.
        size_t blockLength;
        size_t latency;    // expected.
    };

    struct TestEffect
    {
        std::unique_ptr<Lv2Effect> effect;
        size_t frames = 0;
        std::vector<std::vector<float>> inputs;
        std::vector<std::vector<float>> sidechains;
        std::vector<std::vector<float>> outputs;
        std::vector<std::vector<float>> outputHistory; // per output channel, all cycles.
    };

    // blockLength == 0: no staging.
    // asZeroInputPlugin: the plugin's audio input port is described as a sidechain input, so Lv2Effect treats
    // the plugin as a zero-input plugin (output mixed with the chain's input) whose signal comes from the
    // sidechain: a deterministic stand-in for a generator (no zero-input plugin is loadable here).
    std::unique_ptr<TestEffect> MakeEffect(
        PluginHost &host, const std::string &uri, size_t blockLength, size_t numberOfInputs, bool asZeroInputPlugin = false)
    {
        std::shared_ptr<Lv2PluginInfo> info = host.GetPluginInfo(uri);
        bool staged = blockLength != 0;
        size_t frames = host.asIHost()->GetMaxAudioBufferSize();
        if (staged || asZeroInputPlugin)
        {
            info = std::make_shared<Lv2PluginInfo>(*info);
        }
        if (staged)
        {
            if (blockLength < frames)
            {
                info->maxBlockLength() = (float)blockLength;
            }
            else
            {
                info->minBlockLength() = (float)blockLength;
            }
        }
        if (asZeroInputPlugin)
        {
            for (auto &port : info->ports())
            {
                if (port->is_audio_port() && port->is_input())
                {
                    port = std::make_shared<Lv2PortInfo>(*port);
                    port->is_sidechain(true);
                }
            }
        }
        PedalboardItem item;
        item.instanceId(1);
        item.uri(uri);
        item.isEnabled(true);

        auto result = std::make_unique<TestEffect>();
        result->frames = frames;
        result->effect = std::make_unique<Lv2Effect>(host.asIHost(), info, item);
        Lv2Effect *effect = result->effect.get();
        REQUIRE(effect->RequiresBufferStaging() == staged);

        // wired as Lv2Pedalboard::PrepareItems wires a new instance.
        effect->PrepareNoInputEffect((int)numberOfInputs, frames);
        result->inputs.assign(numberOfInputs, std::vector<float>(frames));
        for (int i = 0; i < effect->GetNumberOfInputAudioBuffers(); ++i)
        {
            effect->SetAudioInputBuffer(i, result->inputs[std::min((size_t)i, numberOfInputs - 1)].data());
        }
        result->sidechains.assign(effect->GetNumberOfSidechainAudioBuffers(), std::vector<float>(frames));
        for (int i = 0; i < effect->GetNumberOfSidechainAudioBuffers(); ++i)
        {
            effect->SetAudioSidechainBuffer(i, result->sidechains[i].data());
        }
        result->outputs.assign(effect->GetNumberOfOutputAudioBuffers(), std::vector<float>(frames));
        result->outputHistory.resize(result->outputs.size());
        for (int i = 0; i < effect->GetNumberOfOutputAudioBuffers(); ++i)
        {
            effect->SetAudioOutputBuffer(i, result->outputs[i].data());
        }
        effect->Activate();
        return result;
    }

    // Runs the effect for `cycles` host cycles of a sine input (and another sine on the sidechain).
    void RunCycles(TestEffect &testEffect, size_t cycles, bool silentInput = false)
    {
        bool staged = testEffect.effect->RequiresBufferStaging();
        size_t frame = 0;
        for (size_t cycle = 0; cycle < cycles; ++cycle)
        {
            for (uint32_t i = 0; i < testEffect.frames; ++i, ++frame)
            {
                float value = silentInput ? 0.0f : 0.25f * (float)std::sin(frame * 0.05);
                for (auto &input : testEffect.inputs)
                {
                    input[i] = value;
                }
                for (auto &sidechain : testEffect.sidechains)
                {
                    sidechain[i] = 0.5f * (float)std::sin(frame * 0.013);
                }
                for (auto &output : testEffect.outputs)
                {
                    output[i] = -99.0f; // must be overwritten.
                }
            }
            testEffect.effect->ResetAtomBuffers(); // as the pedalboard does every cycle.
            if (staged)
            {
                testEffect.effect->RunWithBufferStaging((uint32_t)testEffect.frames, nullptr);
            }
            else
            {
                testEffect.effect->Run((uint32_t)testEffect.frames, nullptr, false);
            }
            for (size_t ch = 0; ch < testEffect.outputs.size(); ++ch)
            {
                for (uint32_t i = 0; i < testEffect.frames; ++i)
                {
                    testEffect.outputHistory[ch].push_back(testEffect.outputs[ch][i]);
                }
            }
        }
    }

    size_t CyclesFor(const StagingCase &c)
    {
        return std::max<size_t>(24, 8 * c.blockLength / c.hostFrames);
    }

    // The staged output is the unstaged output delayed by `latency` frames (silence before), with no gaps.
    void RequireDelayedCopy(const TestEffect &staged, const TestEffect &reference, size_t latency)
    {
        REQUIRE(staged.outputHistory.size() == reference.outputHistory.size());
        for (size_t ch = 0; ch < staged.outputHistory.size(); ++ch)
        {
            const auto &stagedOutput = staged.outputHistory[ch];
            const auto &referenceOutput = reference.outputHistory[ch];
            REQUIRE(stagedOutput.size() == referenceOutput.size());
            size_t mismatches = 0;
            double maxError = 0;
            for (size_t i = 0; i < stagedOutput.size(); ++i)
            {
                float expected = i < latency ? 0.0f : referenceOutput[i - latency];
                REQUIRE(std::isfinite(stagedOutput[i]));
                double error = std::abs((double)stagedOutput[i] - (double)expected);
                maxError = std::max(maxError, error);
                if (error > 1e-4)
                {
                    ++mismatches;
                }
            }
            INFO("channel " << ch << ": max error " << maxError);
            REQUIRE(mismatches == 0);
        }
    }

    bool HasNonZero(const std::vector<float> &values)
    {
        for (float value : values)
        {
            if (value != 0)
            {
                return true;
            }
        }
        return false;
    }

    const StagingCase STAGING_CASES[] = {
        {128, 64, 0},   // host cycle a multiple of the block length: no latency.
        {256, 32, 0},
        {32, 128, 96},  // host cycle divides the block length: B - N.
        {64, 128, 64},
        {64, 48, 32},   // neither: B - gcd(N, B).
        {96, 64, 32},
        {100, 64, 60},
    };
}

TEST_CASE("Lv2 buffer staging: latency", "[lv2_buffer_staging]")
{
    for (const auto &c : STAGING_CASES)
    {
        REQUIRE(StagingLatency(c.hostFrames, c.blockLength) == c.latency);
    }
    REQUIRE(StagingLatency(128, 0) == 0);
}

TEST_CASE("Lv2 buffer staging: staged output is the plugin output delayed by the staging latency", "[lv2_buffer_staging]")
{
    for (const auto &c : STAGING_CASES)
    {
        INFO("host cycle " << c.hostFrames << ", block length " << c.blockLength);
        PluginHost host;
        host.asIHost()->SetMaxAudioBufferSize(c.hostFrames);
        host.LoadLilv("/usr/lib/lv2:/usr/local/lib/lv2:/usr/modep/lv2");
        if (!host.GetPluginInfo(BOOSTER_URI))
        {
            WARN("guitarix gxbooster LV2 plugin is not installed. Skipping.");
            return;
        }
        auto reference = MakeEffect(host, BOOSTER_URI, 0, 1);
        RunCycles(*reference, CyclesFor(c));
        auto staged = MakeEffect(host, BOOSTER_URI, c.blockLength, 1);
        REQUIRE(staged->effect->GetStagingLatency() == c.latency);
        RunCycles(*staged, CyclesFor(c));
        REQUIRE(HasNonZero(reference->outputHistory.at(0)));
        RequireDelayedCopy(*staged, *reference, c.latency);
    }
}

TEST_CASE("Lv2 buffer staging: zero-input plugin", "[lv2_buffer_staging]")
{
    for (const auto &c : STAGING_CASES)
    {
        INFO("host cycle " << c.hostFrames << ", block length " << c.blockLength);
        PluginHost host;
        host.asIHost()->SetMaxAudioBufferSize(c.hostFrames);
        host.LoadLilv("/usr/lib/lv2:/usr/local/lib/lv2:/usr/modep/lv2");
        if (!host.GetPluginInfo(BOOSTER_URI))
        {
            WARN("guitarix gxbooster LV2 plugin is not installed. Skipping.");
            return;
        }
        // A zero-input plugin's output is mixed with the chain's (silent) input. Mono chain: one output; stereo
        // chain: the plugin's mono output goes to both outputs.
        for (size_t numberOfInputs : {1, 2})
        {
            INFO("chain inputs: " << numberOfInputs);
            auto reference = MakeEffect(host, BOOSTER_URI, 0, numberOfInputs, true);
            REQUIRE(reference->effect->GetNumberOfOutputAudioBuffers() == (int)numberOfInputs);
            RunCycles(*reference, CyclesFor(c), true);
            auto staged = MakeEffect(host, BOOSTER_URI, c.blockLength, numberOfInputs, true);
            RunCycles(*staged, CyclesFor(c), true);
            for (const auto &history : reference->outputHistory)
            {
                REQUIRE(HasNonZero(history));
            }
            RequireDelayedCopy(*staged, *reference, c.latency);
        }
    }
}

TEST_CASE("Lv2 buffer staging: a plugin with a sidechain input", "[lv2_buffer_staging]")
{
    PluginHost host;
    host.asIHost()->SetMaxAudioBufferSize(64);
    host.LoadLilv("/usr/lib/lv2:/usr/local/lib/lv2:/usr/modep/lv2");
    if (!host.GetPluginInfo(A_COMP_URI))
    {
        WARN("x42/Ardour a-comp LV2 plugin is not installed. Skipping.");
        return;
    }
    // (the sidechain had no staging buffers: the constructor threw.) a-comp's gain computation depends on its
    // block length, so only continuity and sanity are checked here.
    auto staged = MakeEffect(host, A_COMP_URI, 48, 1);
    REQUIRE(staged->effect->GetNumberOfSidechainAudioBuffers() != 0);
    RunCycles(*staged, 24);
    const auto &output = staged->outputHistory.at(0);
    for (size_t i = 0; i < output.size(); ++i)
    {
        REQUIRE(std::isfinite(output[i]));
        REQUIRE(output[i] != -99.0f); // every frame written.
    }
    REQUIRE(HasNonZero(output));
}

TEST_CASE("Lv2 buffer staging: atom ports", "[lv2_buffer_staging]")
{
    PluginHost host;
    host.asIHost()->SetMaxAudioBufferSize(64);
    host.LoadLilv("/usr/lib/lv2:/usr/local/lib/lv2:/usr/modep/lv2");

    // any mono plugin with atom input and output ports.
    std::string uri;
    for (const auto &info : host.GetPlugins())
    {
        size_t audioInputs = 0, audioOutputs = 0, atomInputs = 0, atomOutputs = 0;
        bool sidechain = false;
        for (const auto &port : info->ports())
        {
            if (port->is_audio_port())
            {
                (port->is_input() ? audioInputs : audioOutputs)++;
                sidechain = sidechain || port->is_sidechain();
            }
            if (port->is_atom_port())
            {
                (port->is_input() ? atomInputs : atomOutputs)++;
            }
        }
        if (audioInputs == 1 && audioOutputs == 1 && !sidechain && atomInputs == 1 && atomOutputs == 1)
        {
            uri = info->uri();
            break;
        }
    }
    if (uri.empty())
    {
        WARN("No mono LV2 plugin with atom input and output ports is installed. Skipping.");
        return;
    }
    INFO(uri);
    LV2_URID atomSequence = host.asIHost()->GetLv2Urid(LV2_ATOM__Sequence);

    auto staged = MakeEffect(host, uri, 48, 1);
    for (size_t cycle = 0; cycle < 8; ++cycle)
    {
        RunCycles(*staged, 1);
        // The cycle's output events go to the output atom buffer as a sequence (the output forge used to
        // write into the input atom buffer, leaving the output buffer as an empty Chunk).
        const LV2_Atom *output = (const LV2_Atom *)staged->effect->GetAtomOutputBuffer(0);
        REQUIRE(output->type == atomSequence);
        REQUIRE(output->size >= sizeof(LV2_Atom_Sequence_Body));
        REQUIRE(output->size <= host.asIHost()->GetAtomBufferSize());
        const LV2_Atom *input = (const LV2_Atom *)staged->effect->GetAtomInputBuffer(0);
        REQUIRE(input->type == atomSequence);
    }
}

TEST_CASE("Lv2 buffer staging: atom events are copied whole", "[lv2_buffer_staging]")
{
    // The staged copy of atom events (Lv2Effect::copyAtomBufferEventSequence, used for both the staged input
    // and output sequences) used to cut off the last 8 bytes of every event body.
    PluginHost host;
    LV2_URID_Map *map = host.asIHost()->GetLv2UridMap();
    LV2_URID sequenceType = map->map(map->handle, LV2_ATOM__Sequence);
    LV2_URID chunkType = map->map(map->handle, LV2_ATOM__Chunk);

    // Source sequence: events with 5-byte (unpadded) and 16-byte bodies.
    alignas(8) uint8_t source[256];
    LV2_Atom_Forge sourceForge;
    lv2_atom_forge_init(&sourceForge, map);
    lv2_atom_forge_set_buffer(&sourceForge, source, sizeof(source));
    LV2_Atom_Forge_Frame sourceFrame;
    lv2_atom_forge_sequence_head(&sourceForge, &sourceFrame, 0);
    const uint8_t body1[5] = {1, 2, 3, 4, 5};
    uint8_t body2[16];
    for (size_t i = 0; i < sizeof(body2); ++i)
    {
        body2[i] = (uint8_t)(0xA0 + i);
    }
    lv2_atom_forge_frame_time(&sourceForge, 3);
    lv2_atom_forge_atom(&sourceForge, sizeof(body1), chunkType);
    lv2_atom_forge_write(&sourceForge, body1, sizeof(body1));
    lv2_atom_forge_frame_time(&sourceForge, 7);
    lv2_atom_forge_atom(&sourceForge, sizeof(body2), chunkType);
    lv2_atom_forge_write(&sourceForge, body2, sizeof(body2));
    lv2_atom_forge_pop(&sourceForge, &sourceFrame);

    struct Event
    {
        int64_t time;
        LV2_URID type;
        std::vector<uint8_t> body;
    };
    auto copyInto = [&](uint32_t capacity, int64_t frameTime)
    {
        std::vector<uint64_t> storage(capacity / 8 + 1);
        uint8_t *buffer = (uint8_t *)storage.data();
        LV2_Atom_Forge forge;
        lv2_atom_forge_init(&forge, map);
        lv2_atom_forge_set_buffer(&forge, buffer, capacity);
        LV2_Atom_Forge_Frame frame;
        lv2_atom_forge_sequence_head(&forge, &frame, 0);
        Lv2Effect::copyAtomBufferEventSequence((LV2_Atom_Sequence *)source, forge, frameTime);
        lv2_atom_forge_pop(&forge, &frame);

        const LV2_Atom_Sequence *sequence = (const LV2_Atom_Sequence *)buffer;
        REQUIRE(sequence->atom.type == sequenceType);
        REQUIRE(sizeof(LV2_Atom) + sequence->atom.size <= capacity);
        std::vector<Event> events;
        LV2_ATOM_SEQUENCE_FOREACH(sequence, ev)
        {
            const uint8_t *bytes = (const uint8_t *)LV2_ATOM_BODY_CONST(&ev->body);
            events.push_back({ev->time.frames, ev->body.type, std::vector<uint8_t>(bytes, bytes + ev->body.size)});
        }
        return events;
    };
    SECTION("both events, with their own frame times")
    {
        auto events = copyInto(256, -1);
        REQUIRE(events.size() == 2);
        REQUIRE(events[0].time == 3);
        REQUIRE(events[0].type == chunkType);
        REQUIRE(events[0].body == std::vector<uint8_t>(body1, body1 + sizeof(body1)));
        REQUIRE(events[1].time == 7);
        REQUIRE(events[1].type == chunkType);
        REQUIRE(events[1].body == std::vector<uint8_t>(body2, body2 + sizeof(body2)));
    }
    SECTION("at a given frame time")
    {
        auto events = copyInto(256, 0);
        REQUIRE(events.size() == 2);
        REQUIRE(events[0].time == 0);
        REQUIRE(events[1].time == 0);
        REQUIRE(events[1].body == std::vector<uint8_t>(body2, body2 + sizeof(body2)));
    }
    SECTION("a full output keeps whole events only")
    {
        // sequence header (16) + first event (8 + 8 + 8 padded) = 40 bytes; the second event (32) doesn't fit,
        // and its frame time isn't written either.
        auto events = copyInto(56, -1);
        REQUIRE(events.size() == 1);
        REQUIRE(events[0].body == std::vector<uint8_t>(body1, body1 + sizeof(body1)));
    }
}
