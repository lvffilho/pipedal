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

// Reuse of plugin instances across preset switches.
//
// Instantiating some plugins is expensive (ToobNAM loads and prewarms a neural amp model, which takes
// hundreds of milliseconds). When a pedalboard with a different structure is loaded (preset or bank
// switch), an instance of the running pedalboard can be reused for an item of the new pedalboard if
// re-instantiating it would produce the same plugin state:
//  - same plugin URI (LV2 only: VST3 instances are never reused),
//  - identical persisted plugin state: the LV2 state blob and the path-type patch properties
//    (model file, IR file, ...),
//  - the new item doesn't load a lilv preset (which would replace the state at instantiation).
// The audio channel configuration is checked when the instance is borrowed (Lv2Pedalboard::PrepareItems):
// the instance must have been instantiated for the current sample rate and buffer size, and its buffer
// layout must not change at its new position (BorrowKeepsBufferLayout).
//
// Control values (and the enabled state) of a reused instance are those of the new item. Instance ids
// differ between pedalboards, so a reused instance is re-keyed to the new item's instance id. All of this is
// staged in the new pedalboard and applied on the audio thread when it is swapped in; the running pedalboard
// isn't modified by a build. A reused instance keeps its runtime DSP state (e.g. reverb or delay tails),
// which is not persisted state.
// See PiPedalModel::BuildPedalboard() for the threading rules.

#include "Pedalboard.hpp"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pipedal
{
    inline bool IsVst3PluginUri(const std::string &uri)
    {
        return uri.starts_with("vst3:");
    }

    // Whether an item can take part in instance reuse at all.
    inline bool IsInstanceReuseCandidate(const PedalboardItem &item)
    {
        return !item.isSplit() && !item.isEmpty() && !item.uri().empty() && !IsVst3PluginUri(item.uri());
    }

    // Whether the running instance described by `running` can be reused for `incoming`.
    // `running` describes the instance's current persisted state (not necessarily what was last saved).
    inline bool CanReuseInstance(const PedalboardItem &running, const PedalboardItem &incoming)
    {
        if (!IsInstanceReuseCandidate(running) || !IsInstanceReuseCandidate(incoming))
        {
            return false;
        }
        if (running.uri() != incoming.uri())
        {
            return false;
        }
        if (!incoming.lilvPresetUri().empty())
        {
            // the new instance would restore a lilv preset when instantiated.
            return false;
        }
        if (running.lv2State() != incoming.lv2State())
        {
            return false;
        }
        if (running.pathProperties() != incoming.pathProperties())
        {
            return false;
        }
        return true;
    }

    // All plugin items of a pedalboard, including the contents of splits, in pedalboard order.
    inline void FlattenPedalboardItems(const std::vector<PedalboardItem> &items, std::vector<const PedalboardItem *> &result)
    {
        for (const auto &item : items)
        {
            if (item.isSplit())
            {
                FlattenPedalboardItems(item.topChain(), result);
                FlattenPedalboardItems(item.bottomChain(), result);
            }
            else if (!item.isEmpty())
            {
                result.push_back(&item);
            }
        }
    }
    inline std::vector<const PedalboardItem *> FlattenPedalboardItems(const Pedalboard &pedalboard)
    {
        std::vector<const PedalboardItem *> result;
        FlattenPedalboardItems(pedalboard.items(), result);
        return result;
    }

    /**
     * @brief Match running instances to the items of a new pedalboard.
     *
     * Pure function. `running` describes the instances of the running pedalboard (instance id, URI and
     * current persisted state); `incoming` the items of the pedalboard being loaded.
     *
     * Each running instance is matched at most once, and each incoming item at most once. An instance whose
     * instance id equals the incoming item's is preferred; otherwise items are matched in pedalboard order.
     *
     * @return map from incoming instance id to the running instance id to reuse for it.
     */
    inline std::map<int64_t, int64_t> MatchReusableInstances(
        const std::vector<const PedalboardItem *> &running,
        const std::vector<const PedalboardItem *> &incoming)
    {
        std::map<int64_t, int64_t> result;
        std::vector<bool> used(running.size(), false);

        // Pass 1: same instance id (e.g. reloading the current preset, or a plugin preset change).
        for (const PedalboardItem *item : incoming)
        {
            if (result.contains(item->instanceId()))
            {
                continue;
            }
            for (size_t i = 0; i < running.size(); ++i)
            {
                if (!used[i] && running[i]->instanceId() == item->instanceId() && CanReuseInstance(*running[i], *item))
                {
                    used[i] = true;
                    result[item->instanceId()] = running[i]->instanceId();
                    break;
                }
            }
        }
        // Pass 2: any matching instance, in pedalboard order.
        for (const PedalboardItem *item : incoming)
        {
            if (result.contains(item->instanceId()))
            {
                continue;
            }
            for (size_t i = 0; i < running.size(); ++i)
            {
                if (!used[i] && CanReuseInstance(*running[i], *item))
                {
                    used[i] = true;
                    result[item->instanceId()] = running[i]->instanceId();
                    break;
                }
            }
        }
        return result;
    }

    /**
     * @brief Converts a path property json atom to its canonical (abstract) form; may throw.
     *
     * Path properties are stored abstract (relative to the upload directory) in pedalboards, but may be
     * absolute in a running effect (when reported by the plugin).
     */
    using PathPropertyNormalizer = std::function<std::string(const std::string &jsonAtom)>;

    // Normalize every value of `properties`; a value the normalizer rejects is kept as is.
    inline std::map<std::string, std::string> NormalizePathProperties(
        const std::map<std::string, std::string> &properties,
        const PathPropertyNormalizer &normalize)
    {
        std::map<std::string, std::string> normalized;
        for (const auto &property : properties)
        {
            try
            {
                normalized[property.first] = normalize(property.second);
            }
            catch (const std::exception &)
            {
                normalized[property.first] = property.second;
            }
        }
        return normalized;
    }

    /**
     * @brief Describe a running instance for MatchReusableInstances() by its *live* persisted state.
     *
     * @param state the state the plugin reports now (nullopt: the plugin has no state interface).
     * @param livePathProperties the instance's current path properties (not the ones it was built with).
     */
    inline PedalboardItem DescribeRunningInstance(
        int64_t instanceId,
        const std::string &uri,
        const std::optional<Lv2PluginState> &state,
        const std::map<std::string, std::string> &livePathProperties,
        const PathPropertyNormalizer &normalize)
    {
        PedalboardItem item;
        item.instanceId(instanceId);
        item.uri(uri);
        if (state)
        {
            item.lv2State(*state);
        }
        item.pathProperties(NormalizePathProperties(livePathProperties, normalize));
        return item;
    }

    // The fields of an incoming item that MatchReusableInstances() compares, path properties normalized.
    inline PedalboardItem DescribeIncomingItem(const PedalboardItem &item, const PathPropertyNormalizer &normalize)
    {
        PedalboardItem copy;
        copy.instanceId(item.instanceId());
        copy.uri(item.uri());
        copy.lv2State(item.lv2State());
        copy.lilvPresetUri(item.lilvPresetUri());
        copy.pathProperties(NormalizePathProperties(item.pathProperties(), normalize));
        return copy;
    }

    /**
     * @brief The buffer layout of an Lv2Effect, as far as Lv2Effect::PrepareNoInputEffect() changes it.
     */
    struct EffectBufferLayout
    {
        size_t inputAudioPorts = 0;
        size_t outputAudioPorts = 0;
        size_t inputAudioBuffers = 0;
        size_t outputAudioBuffers = 0;
        size_t passThroughOutputs = 0;     // Lv2Effect::numberOfOutputs (zero-output plugins)
        size_t outputMixBuffers = 0;       // zero-input plugins
        bool outputMixBuffersSized = true; // every output mix buffer holds maxBufferSize samples.
    };

    /**
     * @brief Whether a running effect can be wired into a new pedalboard position with `numberOfInputs`
     * input channels without changing its buffer layout.
     *
     * Borrowed effects keep running on the audio thread until the new pedalboard is swapped in, so
     * Lv2Pedalboard must not resize their buffer vectors or reconnect their mix buffers. This mirrors
     * Lv2Effect::PrepareNoInputEffect(): it returns true when that call would change nothing.
     */
    inline bool BorrowKeepsBufferLayout(const EffectBufferLayout &layout, size_t numberOfInputs)
    {
        if (layout.outputAudioPorts == 0)
        {
            return layout.inputAudioBuffers == std::max(numberOfInputs, layout.inputAudioPorts) &&
                   layout.outputAudioBuffers == layout.passThroughOutputs;
        }
        if (layout.inputAudioPorts == 0)
        {
            return layout.inputAudioBuffers == numberOfInputs &&
                   layout.outputAudioBuffers == std::max(numberOfInputs, layout.outputAudioPorts) &&
                   layout.outputMixBuffers == layout.outputAudioPorts &&
                   layout.outputMixBuffersSized;
        }
        return true;
    }

    /**
     * @brief What an Lv2Effect connects the plugin's audio output ports to.
     */
    enum class AudioOutputPortConnection
    {
        MixBuffer,     // zero-input plugins: their output mix buffers, which MixOutput() mixes with the input.
        StagingBuffer, // plugins that require buffer staging: the effect's output staging buffers.
        OutputBuffer,  // otherwise: the output buffers the pedalboard assigned (SetAudioOutputBuffer).
    };

    /**
     * @brief The connection of an Lv2Effect's audio output ports.
     *
     * Applies equally when a new instance is wired (PrepareNoInputEffect, SetAudioOutputBuffer) and when a
     * borrowed instance's staged buffers are applied at the swap (Lv2Effect::UpdateAudioPorts). With buffer
     * staging, every plugin writes to its output staging buffers; RunWithBufferStaging copies them to the
     * output buffers, or, for a zero-input plugin, to its output mix buffers.
     */
    inline AudioOutputPortConnection GetAudioOutputPortConnection(size_t inputAudioPorts, bool bufferStaging)
    {
        if (bufferStaging)
        {
            return AudioOutputPortConnection::StagingBuffer;
        }
        return inputAudioPorts == 0 ? AudioOutputPortConnection::MixBuffer : AudioOutputPortConnection::OutputBuffer;
    }

    /**
     * @brief The input control values an instance gets for `item`: every input control port gets the
     * item's value, or the port's default value if the item has none (as when a snapshot is applied).
     *
     * @param maxInputControl one more than the highest input control port index.
     * @return (port index, value) for every input control port.
     */
    inline std::vector<std::pair<int, float>> ResolveItemControlValues(
        const PedalboardItem &item,
        uint64_t maxInputControl,
        const std::function<bool(uint64_t index)> &isInputControl,
        const std::function<float(uint64_t index)> &defaultValue,
        const std::function<int(const std::string &symbol)> &controlIndex)
    {
        std::vector<std::pair<int, float>> result;
        std::map<int, size_t> positions;
        for (uint64_t i = 0; i < maxInputControl; ++i)
        {
            if (isInputControl(i))
            {
                positions[(int)i] = result.size();
                result.push_back({(int)i, defaultValue(i)});
            }
        }
        for (const auto &controlValue : item.controlValues())
        {
            int index = controlIndex(controlValue.key());
            auto f = positions.find(index);
            if (f != positions.end())
            {
                result[f->second].second = controlValue.value();
            }
        }
        return result;
    }
}
