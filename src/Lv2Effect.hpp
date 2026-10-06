// Copyright (c) 2025 Robin Davies
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

#include "PluginHost.hpp"
#include "Pedalboard.hpp"
#include <lilv/lilv.h>
#include "BufferPool.hpp"
#include "FileBrowserFilesFeature.hpp"
#include "PatchPropertyWriter.hpp"
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <map>
#include <numeric>
#include "MapPathFeature.hpp"
#include "OptionsFeature.hpp"

#include "IEffect.hpp"
#include "BypassSuspend.hpp"
#include "Worker.hpp"
#include "lv2/patch/patch.h"
#include "lv2/log/log.h"
#include "lv2/log/logger.h"
#include "lv2/lv2plug.in/ns/extensions/units/units.h"
#include "lv2/atom/forge.h"
#include "AtomBuffer.hpp"
#include "StateInterface.hpp"
#include "LogFeature.hpp"

namespace pipedal
{
    /**
     * @brief Output latency of Lv2Effect buffer staging: blocks of `blockLength` frames, host cycles of
     * `maxBufferSize` frames. The smallest latency at which the staged output never runs dry
     * (blockLength - gcd(maxBufferSize, blockLength); see Lv2Effect::EnableBufferStaging).
     */
    inline size_t StagingLatency(size_t maxBufferSize, size_t blockLength)
    {
        if (blockLength == 0 || maxBufferSize == 0)
        {
            return 0;
        }
        return blockLength - std::gcd(maxBufferSize, blockLength);
    }


    class RealtimeRingBufferWriter;    
    class IPatchWriterCallback;
    class HostWorkerThread;

    class Lv2Effect : public IEffect, private LogFeature::LogMessageListener
    {
    private:
        virtual void OnLogError(const char*message);
        virtual void OnLogWarning(const char*message);
        virtual void OnLogInfo(const char*message);
        virtual void OnLogDebug(const char*message);

    private:
        size_t GetStagedBufferSize() const;

        std::shared_ptr<HostWorkerThread> workerThread;
        std::unique_ptr<Worker> worker;

        std::unordered_map<std::string,int> controlIndex;

        FileBrowserFilesFeature fileBrowserFilesFeature;
        std::unique_ptr<StateInterface> stateInterface;
        bool RestoreState(PedalboardItem&pedalboardItem);
        LogFeature logFeature;
        std::map<std::string,AtomBuffer> patchPropertyPrototypes;

        IHost *pHost = nullptr;
        int numberOfInputs = 0;
        int numberOfOutputs = 0;
        int numberOfMidiInputs = 0;

        LilvInstance *pInstance;
        std::shared_ptr<Lv2PluginInfo> info;
        std::vector<float> controlValues;
        std::vector<int> inputAudioPortIndices;
        std::vector<int> outputAudioPortIndices;

        std::vector<int> inputSidechainPortIndices;
        
        std::vector<int> inputAtomPortIndices;
        std::vector<int> outputAtomPortIndices;

        std::vector<int> inputMidiPortIndices;
        std::vector<int> outputMidiPortIndices;

        std::vector<int> midiInputIndices;

        std::vector<float *> inputAudioBuffers;
        std::vector<float *> inputSidechainBuffers;
        std::vector<float *> outputAudioBuffers;

        std::vector<char *> inputAtomBuffers;
        std::vector<char *> outputAtomBuffers;
        std::vector<const LV2_Feature *> features;
        LV2_Feature *work_schedule_feature = nullptr;
        MapPathFeature mapPathFeature;

        uint64_t maxInputControlPort = 0;
        std::vector<bool> isInputControlPort;
        std::vector<float> defaultInputControlValues;
        std::vector<bool> isInputTriggerControlPort;;
        int bypassControlIndex = -1;

        virtual std::string GetUri() const { return info->uri(); }

        std::vector<const Lv2PortInfo *> realtimePortInfo;

        void PreparePortIndices();
        void ConnectControlPorts();
        void AssignUnconnectedPorts();

        LV2_Atom_Forge inputForgeRt;
        LV2_Atom_Forge_Frame input_frame;

        LV2_Atom_Forge outputForgeRt;
        LV2_Atom_Forge_Frame output_frame;

        std::vector<LV2_URID> pathProperties;
        std::vector<PatchPropertyWriter> pathPropertyWriters;
        // Path patch properties dropped on the audio thread because they exceeded the
        // reserved PatchPropertyWriter capacity. Process-wide, so that the service thread
        // (rtsvc) can report it without reaching into pedalboards it does not own.
        static std::atomic<uint64_t> droppedPathPatchProperties;

    public:
        // Non-realtime: drops (across all instances) since the previous call.
        static uint64_t TakeDroppedPathPatchProperties()
        {
            return droppedPathPatchProperties.exchange(0, std::memory_order_relaxed);
        }

    private:

        std::unordered_map<std::string,std::string> mainThreadPathProperties; // guarded by mainThreadPathPropertiesMutex.
        mutable std::mutex mainThreadPathPropertiesMutex;
        // Serializes the plugin's state save()/restore() (GetLv2State/SetLv2State), which may be called from
        // several non-realtime threads (the model's state-save path, and the pedalboard builder thread).
        std::mutex stateMutex;

        class Urids
        {
        public:
            Urids(IHost *pHost)
            {
                atom__Chunk = pHost->GetLv2Urid(LV2_ATOM__Chunk);
                atom__Path = pHost->GetLv2Urid(LV2_ATOM__Path);
                atom__Float = pHost->GetLv2Urid(LV2_ATOM__Float);
                atom__Double = pHost->GetLv2Urid(LV2_ATOM__Double);
                atom__Int = pHost->GetLv2Urid(LV2_ATOM__Int);
                atom__Long = pHost->GetLv2Urid(LV2_ATOM__Long);
                atom__Bool = pHost->GetLv2Urid(LV2_ATOM__Bool);
                atom__String = pHost->GetLv2Urid(LV2_ATOM__String);
                atom__Vector = pHost->GetLv2Urid(LV2_ATOM__Vector);
                atom__Object = pHost->GetLv2Urid(LV2_ATOM__Object);


                atom__Sequence = pHost->GetLv2Urid(LV2_ATOM__Sequence);
                atom__URID = pHost->GetLv2Urid(LV2_ATOM__URID);
                patch__Get = pHost->GetLv2Urid(LV2_PATCH__Get);
                patch__Set = pHost->GetLv2Urid(LV2_PATCH__Set);
                patch__Put = pHost->GetLv2Urid(LV2_PATCH__Put);
                patch__body = pHost->GetLv2Urid(LV2_PATCH__body);
                patch__subject = pHost->GetLv2Urid(LV2_PATCH__subject);
                patch__property = pHost->GetLv2Urid(LV2_PATCH__property);
                patch__value = pHost->GetLv2Urid(LV2_PATCH__value);
                units__frame = pHost->GetLv2Urid(LV2_UNITS__frame);
                state__StateChanged = pHost->GetLv2Urid(LV2_STATE__StateChanged);

            }
            LV2_URID atom__Chunk;
            LV2_URID units__frame;
            LV2_URID pluginUri;
            LV2_URID atom__Bool;
            LV2_URID atom__Float;
            LV2_URID atom__Double;
            LV2_URID atom__Int;
            LV2_URID atom__Long;
            LV2_URID atom__String;
            LV2_URID atom__Object;
            LV2_URID atom__Vector;
            LV2_URID atom__Path;
            LV2_URID atom__Sequence;
            LV2_URID atom__URID;
            LV2_URID midi__Event;
            LV2_URID patch__Get;
            LV2_URID patch__Set;
            LV2_URID patch__Put;
            LV2_URID patch__body;
            LV2_URID patch__subject;
            LV2_URID patch__property;
            LV2_URID patch__value;
            LV2_URID state__StateChanged;
        };

        Urids urids;

        // Atomic: a reused instance is re-keyed (SetInstanceId) by the pedalboard builder thread while the
        // audio thread may still be running it in the outgoing pedalboard.
        std::atomic<uint64_t> instanceId;
        BufferPool bufferPool;

        static LV2_Worker_Status worker_schedule_fn(LV2_Worker_Schedule_Handle handle,
                                                    uint32_t size,
                                                    const void *data);


        int GetBypassControlPort() const { return bypassControlIndex; }

        void ResetInputAtomBuffer(char*data);
        void ResetOutputAtomBuffer(char*data);

        bool bypass = true;
        BypassFader bypassFader;

        bool requestStateChangedNotification = false;

        float zeroInputMix = 0.5f;
        int actualAudioInputs = 0;
        int actualAudioOutputs = 0;
        std::vector<std::vector<float>> outputMixBuffers;
        void BypassDezipperTo(float value);
        void BypassDezipperSet(float value);
        bool suspended = false;

        bool borrowedEffect = false;
        bool activated = false;
        double instantiatedSampleRate = 0;
        size_t instantiatedMaxBufferSize = 0;
        void EnableBufferStaging(size_t bufferSize);
        void CheckStagingBufferSentries();

    public:
        bool RequiresBufferStaging() const;
        // Appends the events of `sequence` to the sequence `outputForge` is writing (whole events only; stops
        // when the output is full). frameTime >= 0: every event at that frame; otherwise the events' own times.
        static void copyAtomBufferEventSequence(LV2_Atom_Sequence *sequence, LV2_Atom_Forge &outputForge, int64_t frameTime = -1);
        // Output latency of buffer staging, in frames (0 without staging).
        size_t GetStagingLatency() const { return stagingLatency; }
        bool IsBorrowedEffect() const { return borrowedEffect; }
        void SetBorrowedEffect(bool value) { borrowedEffect = value; }

        // Used to verify that an existing instance can be borrowed by a rebuilt pedalboard.
        const std::string &PluginUri() const { return info->uri(); }
        double InstantiatedSampleRate() const { return instantiatedSampleRate; }
        size_t InstantiatedMaxBufferSize() const { return instantiatedMaxBufferSize; }
        // Whether a borrowed instance can be wired to `numberOfInputs` input channels without resizing its
        // buffer vectors (see BorrowKeepsBufferLayout in PresetInstanceReuse.hpp).
        bool BorrowKeepsBufferLayout(size_t numberOfInputs, size_t maxBufferSize) const;
        // The plugin's lv2:enabled port, or -1. Only written through SetBypass().
        int BypassControlIndex() const { return bypassControlIndex; }
        // Re-key an instance reused for a pedalboard item with a different instance id (preset switches).
        // Called on the audio thread when the pedalboard that reuses it is swapped in (Lv2Pedalboard::UpdateAudioPorts).
        // Messages the instance sends from the audio thread carry the new id from then on.
        void SetInstanceId(uint64_t instanceId) { this->instanceId.store(instanceId); }

        // Audio thread: install the staged buffer pointers of a borrowed effect (element-wise; no allocation).
        // The plugin's ports are then connected by UpdateAudioPorts().
        void SetBorrowedAudioBuffers(
            const std::vector<float *> &inputs,
            const std::vector<float *> &sidechains,
            const std::vector<float *> &outputs);
        // The instance's current path patch properties (property URI -> json atom).
        // Non-RT. Thread-safe (the pedalboard builder thread reads it without the model mutex).
        std::map<std::string, std::string> GetPathPatchProperties() const
        {
            std::lock_guard<std::mutex> lock(mainThreadPathPropertiesMutex);
            return std::map<std::string, std::string>(mainThreadPathProperties.begin(), mainThreadPathProperties.end());
        }
        void UpdateAudioPorts();
        
        // non RT-thread use only.
        std::string GetPathPatchProperty(const std::string&propertyUri);
        // non RT-thread use only.
        void SetPathPatchProperty(const std::string &propertyUri, const std::string&jsonAtom);

        virtual bool IsLv2Effect() const { return true; }
        virtual bool GetLv2State(Lv2PluginState*state) override;
        virtual void SetLv2State(Lv2PluginState&state) override;

        virtual void RequestPatchProperty(LV2_URID uridUri) ;
        virtual void SetPatchProperty(LV2_URID uridUri,size_t size, LV2_Atom*value) override;
        virtual void RequestAllPathPatchProperties();

        virtual bool GetRequestStateChangedNotification() const override;
        virtual void SetRequestStateChangedNotification(bool value)  override;

        virtual void GatherPatchProperties(RealtimePatchPropertyRequest*pRequest);
        void GatherPathPatchProperties(IPatchWriterCallback *cbPatchWriter);        
        virtual bool IsVst3() const { return false; }
        virtual void RelayPatchSetMessages(uint64_t instanceId,RealtimeRingBufferWriter *realtimeRingBufferWriter) ;

        virtual uint8_t*GetAtomInputBuffer() {
            if (this->inputAtomBuffers.size() == 0) return nullptr;
            return (uint8_t*)this->inputAtomBuffers[0];
        }
        virtual uint8_t*GetAtomOutputBuffer()
        {
            if (this->outputAtomBuffers.size() == 0) return nullptr;
            return (uint8_t*)this->outputAtomBuffers[0];

        }
        OptionsFeature optionsFeature;

        bool hasErrorMessage = false;
        char errorMessage[1024];

        bool deleted = false;
        size_t stagingBufferSize = 0;
        size_t stagingInputIx = 0; // index of the next frame in the input staging buffers.
        size_t stagingLatency = 0;  // output delay, in frames (see EnableBufferStaging).
        // Output FIFO (per output port): the plugin's output blocks, read stagingLatency frames behind.
        std::vector<std::vector<float>> stagingOutputFifo;
        size_t stagingFifoCapacity = 0;
        size_t stagingFifoReadIx = 0;
        size_t stagingFifoCount = 0;
        std::vector<std::vector<float>> inputStagingBuffers;
        std::vector<std::vector<float>> sidechainStagingBuffers;
        std::vector<std::vector<float>> outputStagingBuffers;
        std::vector<float*> inputStagingBufferPointers;
        std::vector<float*> sidechainStagingBufferPointers;
        std::vector<float*> outputStagingBufferPointers;

        std::vector<uint8_t> stagedInputAtomBuffer;
        void *stagedInputAtomBufferPointer = nullptr;
        std::vector<uint8_t> stagedOutputAtomBuffer;
        void *stagedOutputAtomBufferPointer = nullptr;

        size_t stageFrames(size_t sampleOffset, size_t samples);
        void PushStagedOutput();
        void PopStagedOutput(size_t samples);
        float *StagedOutputDestination(size_t channel);


        LV2_Atom_Forge stagedInputForgeRt;
        LV2_Atom_Forge_Frame staged_input_frame;

        void MixOutput(uint32_t samples, RealtimeRingBufferWriter *realtimeRingBufferWriter);

        void resetStagedInputAtomBuffer();

    public:
        Lv2Effect(
            IHost *pHost,
            const std::shared_ptr<Lv2PluginInfo> &info,
            PedalboardItem &pedalboardItem);
        ~Lv2Effect();

        bool HasErrorMessage() const { return this->hasErrorMessage; }
        const char*TakeErrorMessage() { this->hasErrorMessage = false; return this->errorMessage; }

        virtual void PrepareNoInputEffect(int numberOfInputs,size_t maxBufferSize) override;


        virtual void ResetAtomBuffers();
        virtual uint64_t GetInstanceId() const { return instanceId; }
        virtual int GetNumberOfInputAudioPorts() const override { return inputAudioPortIndices.size(); }
        virtual int GetNumberOfOutputAudioPorts() const override { return outputAudioPortIndices.size(); }

        virtual int GetNumberOfInputAtomPorts() const  { return inputAtomPortIndices.size(); }

        virtual int GetNumberOfOutputAtomPorts() const  { return outputAtomPortIndices.size(); }
        virtual int GetNumberOfMidiInputPorts() const  { return inputMidiPortIndices.size(); }
        virtual int GetNumberOfMidiOutputPorts() const  { return outputMidiPortIndices.size(); }



        virtual int GetNumberOfInputAudioBuffers() const override { return this->inputAudioBuffers.size(); }
        virtual int GetNumberOfSidechainAudioBuffers() const override { return this->inputSidechainBuffers.size(); }
        virtual int GetNumberOfOutputAudioBuffers() const override {return this->outputAudioBuffers.size(); }

        virtual void SetAudioInputBuffer(int index, float *buffer) override;
        virtual float *GetAudioInputBuffer(int index) const override;

        virtual void SetAudioSidechainBuffer(int index, float *buffer) override;
        virtual float *GetAudioSidechainBuffer(int index) const override;


        virtual void SetAudioInputBuffer(float *buffer);
        virtual void SetAudioInputBuffers(float *left, float *right);

        virtual void SetAudioOutputBuffer(int index, float *buffer);
        virtual float *GetAudioOutputBuffer(int index) const;

        virtual void SetAtomInputBuffer(int index, void *buffer) { this->inputAtomBuffers[index] = (char*)buffer;}
        virtual void *GetAtomInputBuffer(int index) const { return this->inputAtomBuffers[index]; }

        virtual void SetAtomOutputBuffer(int index, void *buffer) { this->outputAtomBuffers[index] = (char*)buffer; }
        virtual void *GetAtomOutputBuffer(int index) const { return this->outputAtomBuffers[index]; }

        virtual int GetControlIndex(const std::string &symbol) const;

        virtual void SetControl(int index, float value) override
        {
            if (index == -1)
            {
                SetBypass(value != 0);
            } else {
                controlValues[index] = value;
            }
        }

        virtual uint64_t GetMaxInputControl() const override;
        virtual bool IsInputControl(uint64_t index) const override;
        virtual float GetDefaultInputControlValue(uint64_t index) const override;


        virtual float GetControlValue(int index) const {
            if (index == -1) 
            {
                return this->bypass? 1: 0;
            }
            return controlValues[index];
        }


        virtual float GetOutputControlValue(int portIndex) const
        {
            if (portIndex >= 0 && portIndex < controlValues.size()) {
                return controlValues[portIndex];
            }
            return 0;
        }

        virtual void SetBypass(bool bypass)
        {
            if (bypass != this->bypass)
            {
                this->bypass = bypass;
                if (bypassControlIndex == -1) {
                    BypassDezipperTo(bypass? 1.0f: 0.0f);
                } else {
                    controlValues[bypassControlIndex] = bypass? 1.0f: 0.0f;
                }
            }

        }

        virtual void Activate();
        virtual void Run(uint32_t samples, RealtimeRingBufferWriter *realtimeRingBufferWriter);
        // Run, but skip lilv_instance_run() if suspendBypassedPlugins is set and the
        // plugin's bypass crossfade has completed. (see BypassSuspend.hpp)
        void Run(uint32_t samples, RealtimeRingBufferWriter *realtimeRingBufferWriter, bool suspendBypassedPlugins);
        // True if the plugin has atom input (patch messages, MIDI) queued for this cycle. RT-safe.
        bool HasPendingAtomInput() const;
        bool IsSuspended() const { return suspended; }
        // Replace the (Chunk-reset) atom output buffers with empty sequences, for a cycle in which run() was skipped. RT-safe.
        void WriteEmptyOutputAtomBuffers();
        virtual void RunWithBufferStaging(uint32_t samples, RealtimeRingBufferWriter *realtimeRingBufferWriter);
        virtual void Deactivate();
    };

} // namespace