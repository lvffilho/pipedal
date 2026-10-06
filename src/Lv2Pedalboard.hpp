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
#include "Pedalboard.hpp"
#include "MidiEvent.hpp"
#include "PluginHost.hpp"
#include "Lv2Effect.hpp"
#include "BufferPool.hpp"
#include <functional>
#include <lv2/urid/urid.h>
#include <functional>
#include "DbDezipper.hpp"

namespace pipedal
{

    class AudioDriver;
    class Lv2Effect;
    class IPatchWriterCallback;
    class RealtimeVuBuffers;
    class RealtimePatchPropertyRequest;
    class RealtimeRingBufferWriter;
    class SplitEffect;

    // Running Lv2 effects that a new pedalboard may borrow, keyed by the instance id of the pedalboard item that
    // will use them. Each effect appears at most once.
    using ExistingEffectMap = std::map<uint64_t, std::shared_ptr<IEffect>>;

    // How Lv2Pedalboard::Prepare() treats borrowed effects.
    enum class BorrowMode
    {
        // Same instance, same item (structural edit of the current pedalboard): settings are kept as they are.
        SameItem,
        // Instance reused for an item of a different pedalboard (preset switch): the effect is re-keyed to the
        // item's instance id, and the item's control values and enabled state are applied, when the
        // pedalboard is swapped in on the audio thread. It keeps its runtime DSP state (e.g. reverb or delay
        // tails): only persisted state is matched.
        ReusedForNewItem
    };

    struct Lv2PedalboardError
    {
        int64_t intanceId;
        std::string message;
    };

    class Lv2PedalboardErrorList : public std::vector<Lv2PedalboardError> // (forward declaration issues with a using statement)
    {
    };

    class Lv2Pedalboard
    {
    private:
        IHost *pHost = nullptr;
        size_t currentFrameOffset = 0;
        DbDezipper inputVolume;
        DbDezipper outputVolume;

        BufferPool bufferPool;
        std::vector<float *> pedalboardInputBuffers;
        std::vector<float *> pedalboardOutputBuffers;
        float *pedalboardSidechainBuffer = nullptr;

        std::vector<std::shared_ptr<IEffect>> effects;
        std::vector<IEffect *> realtimeEffects;
        // The instance id of each of realtimeEffects in this pedalboard. All lookups by instance id use these:
        // a borrowed effect only takes its new instance id (Lv2Effect::GetInstanceId()) when this pedalboard is
        // swapped in.
        std::vector<uint64_t> realtimeEffectInstanceIds;

        // Effects borrowed from the running pedalboard. They keep running there, untouched, until this
        // pedalboard is swapped in: Prepare() only stages their new buffer pointers (and, for
        // BorrowMode::ReusedForNewItem, their new instance id and settings) here; UpdateAudioPorts() applies
        // them on the audio thread at the swap. So a borrowing build that is discarded or fails leaves the
        // running pedalboard unchanged.
        struct BorrowedEffect
        {
            Lv2Effect *effect = nullptr;
            // staged buffer pointers; same sizes as the effect's (BorrowKeepsBufferLayout), so applying them
            // doesn't allocate. Start null-filled; Prepare() sets the entries this position uses.
            std::vector<float *> inputBuffers;
            std::vector<float *> sidechainBuffers;
            std::vector<float *> outputBuffers;

            bool reusedForNewItem = false;
            uint64_t instanceId = 0;
            std::vector<std::pair<int, float>> controlValues;
            bool enabled = true;
        };
        std::vector<BorrowedEffect> borrowedEffects;
        bool borrowedEffectsApplied = false;
        uint64_t instanceIdLineage = 0;
        BorrowedEffect *FindBorrowedEffect(const IEffect *effect);
        float *GetPreparedOutputBuffer(IEffect *effect, int index);

        using Action = std::function<void()>;
        using ProcessAction = std::function<void(uint32_t frames)>;

        std::vector<Action> activateActions;

        std::vector<ProcessAction> processActions;

        // A split branch: the range of processActions (and realtimeEffects) that
        // implement one chain of a split. Used to skip a silent branch when
        // suspendBypassedPlugins is enabled. Sorted by beginAction.
        struct SplitBranchGate
        {
            size_t begin;       // first processAction of the branch.
            size_t end;         // one past the last processAction of the branch.
            size_t effectBegin; // range of realtimeEffects in the branch.
            size_t effectEnd;
            SplitEffect *split;
            bool topBranch;
            bool containsSidechainSource = false; // never skipped.
        };
        std::vector<SplitBranchGate> splitBranchGates;
        std::vector<size_t> sidechainSourceEffectIndices; // indices into realtimeEffects.

        // "Suspend bypassed plugins" setting. Set before activation, or on the RT thread thereafter.
        bool suspendBypassedPlugins = false;
        // RT: decide whether to skip a branch; if so, empties its effects' atom outputs.
        bool TrySkipBranch(const SplitBranchGate &gate);

        std::vector<Action> deactivateActions;

        float *CreateNewAudioBuffer();

        RealtimeRingBufferWriter *ringBufferWriter;

        enum class MidiControlType
        {
            None,
            Select,
            Dial,
            Toggle,
            Trigger,
            MomentarySwitch,
            TapTempo
        };
        class MidiMapping
        {
        public:
            std::shared_ptr<Lv2PluginInfo> pluginInfo; // lifecycle
            const Lv2PortInfo *pPortInfo = nullptr;    // owned by port.
            int instanceId = -1;
            int effectIndex = -1;
            int controlIndex = -1;
            int key; // key to the note or control. internal use only.
            MidiTimestamp lastTapTimestamp;
            bool hasLastValue = false;
            bool lastValueIncreasing = false;
            float lastValue = 0;
            MidiControlType mappingType;
            MidiBinding midiBinding;
        };

        std::vector<MidiMapping> midiMappings;

        std::vector<float *> PrepareItems(
            std::vector<PedalboardItem> &items,
            std::vector<float *> inputBuffers,
            Lv2PedalboardErrorList &errorList,
            ExistingEffectMap *existingEffects,
            BorrowMode borrowMode);

        void PrepareMidiMap(const Pedalboard &pedalboard);
        void PrepareMidiMap(const PedalboardItem &pedalboardItem);

        std::vector<float *> AllocateAudioBuffers(int nChannels);
        int CalculateChainInputs(const std::vector<float *> &inputBuffers, const std::vector<PedalboardItem> &items);
        void AppendParameterRequest(uint8_t *atomBuffer, LV2_URID uridParameter);

    public:
        Lv2Pedalboard() {}
        ~Lv2Pedalboard() {}

        void Prepare(
            IHost *pHost,
            Pedalboard &pedalboard,
            Lv2PedalboardErrorList &errorList,
            ExistingEffectMap *existingEffects = nullptr,
            BorrowMode borrowMode = BorrowMode::SameItem);

        // Number of effects borrowed from existingEffects by Prepare().
        size_t GetBorrowedEffectCount() const { return borrowedEffects.size(); }

        // Pedalboards whose instance ids name the same items share a lineage (see PiPedalModel::instanceIdLineage).
        // Set before the pedalboard is handed to the audio thread. Realtime parameter requests carry the lineage
        // they were addressed to, and are rejected by a pedalboard of another lineage.
        void SetInstanceIdLineage(uint64_t lineage) { instanceIdLineage = lineage; }
        uint64_t GetInstanceIdLineage() const { return instanceIdLineage; }

        // Instance id of GetEffects()[index] in this pedalboard.
        uint64_t GetInstanceIdAt(size_t index) const { return realtimeEffectInstanceIds.at(index); }

        std::vector<IEffect *> &GetEffects() { return realtimeEffects; }
        std::vector<std::shared_ptr<IEffect>> &GetSharedEffectList() { return effects; }

        size_t GetNumberOfAudioInputChannels() const;
        size_t GetNumberOfAudioOutputChannels() const;

        int GetIndexOfInstanceId(uint64_t instanceId)
        {
            for (int i = 0; i < this->realtimeEffects.size(); ++i)
            {
                if (this->realtimeEffectInstanceIds[i] == instanceId)
                    return i;
            }
            return -1;
        }
        IEffect *GetEffect(uint64_t instanceId)
        {
            for (int i = 0; i < realtimeEffects.size(); ++i)
            {
                if (realtimeEffectInstanceIds[i] == instanceId)
                {
                    return realtimeEffects[i];
                }
            }
            return nullptr;
        }
        void Activate();
        void Deactivate();
        void UpdateAudioPorts();

        bool Run(float **inputBuffers, float **outputBuffers, uint32_t samples, RealtimeRingBufferWriter *realtimeWriter);

        void ResetAtomBuffers();

        void ProcessParameterRequests(RealtimePatchPropertyRequest *pParameterRequests, size_t samplesThisTime);
        void GatherPatchProperties(RealtimePatchPropertyRequest *pParameterRequests);
        void GatherPathPatchProperties(IPatchWriterCallback *cbPatchWriter);

        std::vector<float *> &GetInputBuffers() { return this->pedalboardInputBuffers; }
        std::vector<float *> &GetoutputBuffers() { return this->pedalboardOutputBuffers; }

        int GetControlIndex(uint64_t instanceId, const std::string &symbol);
        void SetControlValue(int effectIndex, int portIndex, float value);
        void SetInputVolume(float value) { this->inputVolume.SetTarget(value); }
        void SetOutputVolume(float value) { this->outputVolume.SetTarget(value); }
        void SetBypass(int effectIndex, bool enabled);
        // Non-RT before the pedalboard is handed to the audio thread; RT thread afterwards.
        void SetSuspendBypassedPlugins(bool value) { this->suspendBypassedPlugins = value; }
        bool GetSuspendBypassedPlugins() const { return this->suspendBypassedPlugins; }

        void ComputeVus(RealtimeVuBuffers *vuConfiguration, uint32_t samples);

        float GetControlOutputValue(int effectIndex, int portIndex);

        typedef void(MidiCallbackFn)(void *data, uint64_t intanceId, int controlIndex, float value);
        void OnMidiMessage(
            const MidiEvent&message,
            void *callbackHandle,
            MidiCallbackFn *pfnCallback);
private:
        void handleTapTempo(
            uint8_t value, 
            const MidiTimestamp& timestamp, 
            MidiMapping &mapping,
            void *callbackHandle,
            MidiCallbackFn *pfnCallback);


    };

} // namespace