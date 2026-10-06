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
#include "Lv2Pedalboard.hpp"
#include "Lv2Effect.hpp"
#include "PresetInstanceReuse.hpp"

#include "SplitEffect.hpp"
#include "BypassSuspend.hpp"
#include <algorithm>
#include "RingBufferReader.hpp"
#include "VuUpdate.hpp"
#include "AudioHost.hpp"
#include "Lv2EventBufferWriter.hpp"
#include "Lv2Log.hpp"
#include "CrashGuard.hpp"
#include "restrict.hpp"
#include "AudioDriver.hpp"

using namespace pipedal;

float *Lv2Pedalboard::CreateNewAudioBuffer()
{
    return bufferPool.AllocateBuffer<float>(pHost->GetMaxAudioBufferSize());
}

std::vector<float *> Lv2Pedalboard::AllocateAudioBuffers(int nChannels)
{
    std::vector<float *> result;
    for (int i = 0; i < nChannels; ++i)
    {
        result.push_back(bufferPool.AllocateBuffer<float>(pHost->GetMaxAudioBufferSize()));
    }
    return result;
}

int Lv2Pedalboard::GetControlIndex(uint64_t instanceId, const std::string &symbol)
{
    for (int i = 0; i < realtimeEffects.size(); ++i)
    {
        auto item = realtimeEffects[i];
        if (realtimeEffectInstanceIds[i] == instanceId)
        {
            return item->GetControlIndex(symbol);
        }
    }
    return -1;
}

Lv2Pedalboard::BorrowedEffect *Lv2Pedalboard::FindBorrowedEffect(const IEffect *effect)
{
    for (auto &borrowedEffect : borrowedEffects)
    {
        if ((const IEffect *)borrowedEffect.effect == effect)
        {
            return &borrowedEffect;
        }
    }
    return nullptr;
}

float *Lv2Pedalboard::GetPreparedOutputBuffer(IEffect *effect, int index)
{
    // the output buffer the effect will have in this pedalboard (borrowed effects: staged until the swap).
    BorrowedEffect *borrowedEffect = FindBorrowedEffect(effect);
    if (borrowedEffect)
    {
        return borrowedEffect->outputBuffers.at(index);
    }
    return effect->GetAudioOutputBuffer(index);
}

std::vector<float *> Lv2Pedalboard::PrepareItems(
    std::vector<PedalboardItem> &items,
    std::vector<float *> inputBuffers,
    Lv2PedalboardErrorList &errorList,
    ExistingEffectMap *existingEffects,
    BorrowMode borrowMode)
{
    for (int i = 0; i < items.size(); ++i)
    {
        auto &item = items[i];
        if (!item.isEmpty())
        {
            std::shared_ptr<IEffect> pEffect = nullptr;

            if (item.isSplit())
            {
                auto pSplit = new SplitEffect(item.instanceId(), pHost->GetSampleRate(), inputBuffers);
                pEffect = std::shared_ptr<IEffect>(pSplit);

                int topInputChannels = inputBuffers.size();
                int bottomInputChannels = inputBuffers.size();

                std::vector<float *> topInputs = AllocateAudioBuffers(topInputChannels);
                std::vector<float *> bottomInputs = AllocateAudioBuffers(bottomInputChannels);

                auto preMixAction = [pSplit](uint32_t frames)
                { pSplit->PreMix(frames); };

                this->processActions.push_back(preMixAction);

                size_t topBegin = this->processActions.size();
                size_t topEffectBegin = this->realtimeEffects.size();
                std::vector<float *> topResult = PrepareItems(item.topChain(), topInputs, errorList, existingEffects, borrowMode);
                size_t bottomBegin = this->processActions.size();
                size_t bottomEffectBegin = this->realtimeEffects.size();
                std::vector<float *> bottomResult = PrepareItems(item.bottomChain(), bottomInputs, errorList, existingEffects, borrowMode);
                size_t bottomEnd = this->processActions.size();
                size_t bottomEffectEnd = this->realtimeEffects.size();

                // record the branch ranges so that a silent branch can be skipped (suspendBypassedPlugins).
                SplitBranchGate topGate{topBegin, bottomBegin, topEffectBegin, bottomEffectBegin, pSplit, true};
                SplitBranchGate bottomGate{bottomBegin, bottomEnd, bottomEffectBegin, bottomEffectEnd, pSplit, false};
                // a branch containing an already-connected sidechain source is never skipped.
                PinGateIfContainsAny(topGate, this->sidechainSourceEffectIndices);
                PinGateIfContainsAny(bottomGate, this->sidechainSourceEffectIndices);
                this->splitBranchGates.push_back(topGate);
                this->splitBranchGates.push_back(bottomGate);
                std::stable_sort(
                    this->splitBranchGates.begin(), this->splitBranchGates.end(),
                    [](const SplitBranchGate &left, const SplitBranchGate &right)
                    { return left.begin < right.begin; });

                this->processActions.push_back(
                    [pSplit](uint32_t frames)
                    { pSplit->PostMix(frames); });
                auto controlValue = item.GetControlValue("splitType");
                // if split is L/R, always output stereo.

                bool forceStereo = (controlValue != nullptr && controlValue->value() == 2);
                pSplit->SetChainBuffers(topInputs, bottomInputs, topResult, bottomResult, forceStereo);

                for (int i = 0; i < item.controlValues().size(); ++i)
                {
                    auto &controlValue = item.controlValues()[i];
                    int index = pSplit->GetControlIndex(controlValue.key());
                    if (index != -1)
                    {
                        pSplit->SetControl(index, controlValue.value());
                    }
                }
            }
            else
            {
                std::shared_ptr<IEffect> pLv2Effect;
                // borrowed effects are left untouched (they are still running in the current pedalboard): their
                // wiring is staged in borrowedEffects[borrowedIndex] and applied when this pedalboard is swapped in.
                int borrowedIndex = -1;

                if (existingEffects && existingEffects->contains(item.instanceId()) &&
                    existingEffects->at(item.instanceId())->IsLv2Effect())
                {
                    // Instance ids are only unique within a pedalboard lineage. Only borrow an instance of the
                    // same plugin, instantiated for the current sample rate and buffer size, whose buffer layout
                    // doesn't change at this position (it keeps running on the audio thread until this
                    // pedalboard is swapped in); otherwise create a new one.
                    Lv2Effect *existing = (Lv2Effect *)existingEffects->at(item.instanceId()).get();
                    if (existing->PluginUri() == item.uri() &&
                        existing->InstantiatedSampleRate() == pHost->GetSampleRate() &&
                        existing->InstantiatedMaxBufferSize() == pHost->GetMaxAudioBufferSize() &&
                        existing->BorrowKeepsBufferLayout(inputBuffers.size(), pHost->GetMaxAudioBufferSize()))
                    {
                        pLv2Effect = existingEffects->at(item.instanceId());
                        // each effect can be borrowed only once.
                        existingEffects->erase(item.instanceId());

                        BorrowedEffect settings;
                        settings.effect = existing;
                        // Sized like the effect's vectors, null-filled: the wiring below sets every entry this
                        // position uses, as for a new instance. (Not a copy of the live vectors: their pointers
                        // belong to the running pedalboard, whose buffers are freed after the swap.)
                        settings.inputBuffers.assign((size_t)existing->GetNumberOfInputAudioBuffers(), nullptr);
                        settings.sidechainBuffers.assign((size_t)existing->GetNumberOfSidechainAudioBuffers(), nullptr);
                        settings.outputBuffers.assign((size_t)existing->GetNumberOfOutputAudioBuffers(), nullptr);
                        settings.instanceId = item.instanceId();

                        if (borrowMode == BorrowMode::ReusedForNewItem)
                        {
                            // Reused for an item of another pedalboard (preset switch). Note that the instance keeps
                            // its runtime DSP state (e.g. delay or reverb tails); only persisted state is matched.
                            settings.reusedForNewItem = true;
                            settings.enabled = item.isEnabled();
                            settings.controlValues = ResolveItemControlValues(
                                item,
                                existing->GetMaxInputControl(),
                                [existing](uint64_t index)
                                { return existing->IsInputControl(index); },
                                [existing](uint64_t index)
                                { return existing->GetDefaultInputControlValue(index); },
                                [existing](const std::string &symbol)
                                { return existing->GetControlIndex(symbol); });
                            // the plugin's enable port follows the item's enabled state (SetBypass), as it
                            // does for a new instance (Lv2Effect::Activate).
                            int bypassControlIndex = existing->BypassControlIndex();
                            std::erase_if(
                                settings.controlValues,
                                [bypassControlIndex](const std::pair<int, float> &value)
                                { return value.first == bypassControlIndex; });
                        }
                        borrowedIndex = (int)this->borrowedEffects.size();
                        this->borrowedEffects.push_back(std::move(settings));
                    }
                }
                if (!pLv2Effect)
                {
                    try
                    {
                        pLv2Effect = std::shared_ptr<IEffect>(this->pHost->CreateEffect(item));
                    }
                    catch (const std::exception &e)
                    {
                        Lv2Log::warning(SS(e.what()));
                    }

                    if (pLv2Effect && pLv2Effect->HasErrorMessage())
                    {
                        std::string error = pLv2Effect->TakeErrorMessage();
                        Lv2Log::error(error);
                        errorList.push_back({item.instanceId(), error});
                    }
                }

                if (pLv2Effect)
                {

                    pEffect = pLv2Effect;

                    if (borrowedIndex == -1)
                    {
                        // (a borrowed effect's layout is already right: see BorrowKeepsBufferLayout.)
                        pLv2Effect->PrepareNoInputEffect(inputBuffers.size(), pHost->GetMaxAudioBufferSize());
                    }
                    IEffect *pTarget = pLv2Effect.get();
                    auto setInputBuffer = [this, borrowedIndex, pTarget](int index, float *buffer)
                    {
                        if (borrowedIndex != -1)
                        {
                            this->borrowedEffects[borrowedIndex].inputBuffers.at(index) = buffer;
                        }
                        else
                        {
                            pTarget->SetAudioInputBuffer(index, buffer);
                        }
                    };
                    auto setSidechainBuffer = [this, borrowedIndex, pTarget](int index, float *buffer)
                    {
                        if (borrowedIndex != -1)
                        {
                            this->borrowedEffects[borrowedIndex].sidechainBuffers.at(index) = buffer;
                        }
                        else
                        {
                            pTarget->SetAudioSidechainBuffer(index, buffer);
                        }
                    };

                    if (inputBuffers.size() == 1)
                    {
                        if (pLv2Effect->GetNumberOfInputAudioBuffers() == 1)
                        {
                            setInputBuffer(0, inputBuffers[0]);
                        }
                        else if (pLv2Effect->GetNumberOfInputAudioBuffers() >= 2)
                        {
                            setInputBuffer(0, inputBuffers[0]);
                            setInputBuffer(1, inputBuffers[0]);
                        }
                    }
                    else
                    {
                        if (pLv2Effect->GetNumberOfInputAudioBuffers() == 1)
                        {
                            setInputBuffer(0, inputBuffers[0]);

                            auto inputBuffer = inputBuffers[0];
                        }
                        else if (pLv2Effect->GetNumberOfInputAudioBuffers() >= 2)
                        {
                            setInputBuffer(0, inputBuffers[0]);
                            setInputBuffer(1, inputBuffers[1]);

                            auto bufferL = inputBuffers[0];
                            auto bufferR = inputBuffers[1];
                        }
                    }
                    // Connect sidechain buffers.

                    if (pLv2Effect->GetNumberOfSidechainAudioBuffers() != 0)
                    {
                        if (item.sideChainInputId() == -2) // -2 means "use pedalboard inputs".
                        {
                            for (size_t i = 0; i < pLv2Effect->GetNumberOfSidechainAudioBuffers(); ++i)
                            {
                                if (i < this->pedalboardInputBuffers.size())
                                {
                                    setSidechainBuffer(i, this->pedalboardInputBuffers[i]);
                                }
                                else
                                {
                                    // just use the first output buffer for all sidechain inputs.
                                    setSidechainBuffer(i, this->pedalboardInputBuffers[0]);
                                }
                            }
                        }
                        else if (item.sideChainInputId() != -1)
                        {
                            IEffect *pSideChainInput = GetEffect(item.sideChainInputId());

                            if (pSideChainInput)
                            {
                                // never skip a split branch that contains the sidechain source (suspendBypassedPlugins).
                                int sourceIndex = GetIndexOfInstanceId(item.sideChainInputId());
                                if (sourceIndex >= 0)
                                {
                                    // several effects may share a sidechain source: record it once.
                                    if (std::find(this->sidechainSourceEffectIndices.begin(), this->sidechainSourceEffectIndices.end(), (size_t)sourceIndex) ==
                                        this->sidechainSourceEffectIndices.end())
                                    {
                                        this->sidechainSourceEffectIndices.push_back((size_t)sourceIndex);
                                    }
                                    PinGatesContainingEffect(this->splitBranchGates, (size_t)sourceIndex);
                                }
                                for (size_t i = 0; i < pLv2Effect->GetNumberOfSidechainAudioBuffers(); ++i)
                                {
                                    if (i < pSideChainInput->GetNumberOfOutputAudioBuffers())
                                    {
                                        setSidechainBuffer(i, GetPreparedOutputBuffer(pSideChainInput, (int)i));
                                    }
                                    else
                                    {
                                        // just use the first output buffer for all sidechain inputs.
                                        setSidechainBuffer(i, GetPreparedOutputBuffer(pSideChainInput, 0));
                                    }
                                }
                            }
                            else
                            {
                                throw std::runtime_error("Internal error: Sidechain input IEffect not found.");
                            }
                        }
                        else
                        {
                            // No sidechain input. Feed zero buffer to all sidechain audio ports..
                            for (size_t i = 0; i < pLv2Effect->GetNumberOfSidechainAudioBuffers(); ++i)
                            {

                                // just use one buffer for all plugins
                                if (!this->pedalboardSidechainBuffer)
                                {
                                    this->pedalboardSidechainBuffer = CreateNewAudioBuffer();
                                }
                                setSidechainBuffer(i, this->pedalboardSidechainBuffer);
                            }
                        }
                    }

                    // check to see whether we need buffer staging.
                    bool requiresBufferStaging = false;
                    if (pLv2Effect->IsLv2Effect())
                    {
                        Lv2Effect *lv2Effect = (Lv2Effect *)pLv2Effect.get();

                        if (lv2Effect->RequiresBufferStaging())
                        {
                            requiresBufferStaging = true;
                            this->processActions.push_back(
                                [lv2Effect, this](uint32_t frames)
                                {
                                    lv2Effect->RunWithBufferStaging(frames, this->ringBufferWriter);
                                });
                        }
                    }

                    if (!requiresBufferStaging)
                    {
                        if (pLv2Effect->IsLv2Effect())
                        {
                            // may skip lilv_instance_run() once fully bypassed (suspendBypassedPlugins).
                            Lv2Effect *lv2Effect = (Lv2Effect *)pLv2Effect.get();
                            this->processActions.push_back(
                                [lv2Effect, this](uint32_t frames)
                                {
                                    lv2Effect->Run(frames, this->ringBufferWriter, this->suspendBypassedPlugins);
                                });
                        }
                        else
                        {
                            this->processActions.push_back(
                                [pLv2Effect, this](uint32_t frames)
                                {
                                    pLv2Effect->Run(frames, this->ringBufferWriter);
                                });
                        }
                    }

                    // reset any trigger controls to default state after processing
                    if (pLv2Effect->IsLv2Effect())
                    {
                        Lv2Effect *lv2Effect = (Lv2Effect *)pLv2Effect.get();

                        auto pluginInfo = pHost->GetPluginInfo(item.uri());
                        if (pluginInfo)
                        {
                            for (auto control : pluginInfo->ports())
                            {
                                if (control->trigger_property() && control->is_input() && control->is_control_port())
                                {
                                    int controlIndex = lv2Effect->GetControlIndex(control->symbol());
                                    if (controlIndex >= 0)
                                    {
                                        float defaultValue = control->default_value();
                                        this->processActions.push_back(
                                            [pLv2Effect, controlIndex, defaultValue](int32_t frames)
                                            {
                                                pLv2Effect->SetControl(controlIndex, defaultValue);
                                            });
                                    }
                                }
                            }
                        }
                    }
                }
            }
            if (pEffect)
            {
                this->effects.push_back(pEffect); // for ownership.

                this->realtimeEffects.push_back(pEffect.get()); // because std::shared_ptr is not threadsafe.
                this->realtimeEffectInstanceIds.push_back(item.instanceId());

                std::vector<float *> effectOutput;

                if (pEffect->GetNumberOfOutputAudioBuffers() == 1)
                {
                    effectOutput.push_back(CreateNewAudioBuffer());
                }
                else if (pEffect->GetNumberOfOutputAudioBuffers() >= 2)
                {
                    effectOutput.push_back(CreateNewAudioBuffer());
                    effectOutput.push_back(CreateNewAudioBuffer());
                }
                BorrowedEffect *borrowedEffect = FindBorrowedEffect(pEffect.get());
                for (size_t i = 0; i < effectOutput.size(); ++i)
                {
                    if (borrowedEffect)
                    {
                        borrowedEffect->outputBuffers.at(i) = effectOutput[i];
                    }
                    else
                    {
                        pEffect->SetAudioOutputBuffer(i, effectOutput[i]);
                    }
                }
                inputBuffers = effectOutput;
            }
        }
    }
    return inputBuffers;
}

void Lv2Pedalboard::Prepare(
    IHost *pHost,
    Pedalboard &pedalboard,
    Lv2PedalboardErrorList &errorList,
    ExistingEffectMap *existingEffects,
    BorrowMode borrowMode)
{
    this->pHost = pHost;

    inputVolume.SetSampleRate((float)(this->pHost->GetSampleRate()));
    outputVolume.SetSampleRate((float)(this->pHost->GetSampleRate()));
    inputVolume.SetMinDb(-60);
    outputVolume.SetMinDb(-60);

    inputVolume.SetTarget(pedalboard.input_volume_db());
    outputVolume.SetTarget(pedalboard.output_volume_db());

    size_t nInputs = std::max(GetNumberOfAudioInputChannels(),(size_t)1);

    for (size_t i = 0; i < nInputs; ++i)
    {
        this->pedalboardInputBuffers.push_back(bufferPool.AllocateBuffer<float>(pHost->GetMaxAudioBufferSize()));
    }

    auto outputs = PrepareItems(pedalboard.items(), this->pedalboardInputBuffers, errorList, existingEffects, borrowMode);
    size_t nOutputs = GetNumberOfAudioOutputChannels();
    if (nOutputs == 1)
    {
        this->pedalboardOutputBuffers.push_back(outputs[0]);
    }
    else
    {
        if (outputs.size() == 1)
        {
            this->pedalboardOutputBuffers.push_back(outputs[0]);
            this->pedalboardOutputBuffers.push_back(outputs[0]);
        }
        else
        {
            this->pedalboardOutputBuffers.push_back(outputs[0]);
            this->pedalboardOutputBuffers.push_back(outputs[1]);
        }
    }
    PrepareMidiMap(pedalboard);
}

void Lv2Pedalboard::PrepareMidiMap(const PedalboardItem &pedalboardItem)
{
    if (pedalboardItem.midiBindings().size() != 0)
    {
        Lv2PluginInfo::ptr pluginInfo;
        if (pedalboardItem.uri() == SPLIT_PEDALBOARD_ITEM_URI)
        {
            pluginInfo = GetSplitterPluginInfo();
        }
        else
        {
            pluginInfo = pHost->GetPluginInfo(pedalboardItem.uri());
        }

        int effectIndex = this->GetIndexOfInstanceId(pedalboardItem.instanceId());

        if (pluginInfo && effectIndex != -1)
        {
            for (size_t bindingIndex = 0; bindingIndex < pedalboardItem.midiBindings().size(); ++bindingIndex)
            {
                auto &binding = pedalboardItem.midiBindings()[bindingIndex];
                {
                    const Lv2PortInfo *pPortInfo;
                    int controlIndex;
                    if (binding.symbol() == "__bypass")
                    {
                        pPortInfo = GetBypassPortInfo();
                        controlIndex = -1;
                    }
                    else
                    {
                        try
                        {
                            pPortInfo = &pluginInfo->getPort(binding.symbol());
                            controlIndex = this->GetControlIndex(pedalboardItem.instanceId(), binding.symbol());
                        }
                        catch (const std::exception &ignored)
                        {
                            continue;
                        }
                    }

                    MidiMapping mapping;
                    mapping.pluginInfo = pluginInfo; // for lifetime management. <shrugs> We're holding internal pointers to this. May save us in a disorderly shutdown.
                    mapping.pPortInfo = pPortInfo;
                    mapping.effectIndex = effectIndex;
                    mapping.controlIndex = controlIndex;
                    mapping.midiBinding = binding;
                    mapping.instanceId = pedalboardItem.instanceId();

                    if (pPortInfo->mod_momentaryOffByDefault() || pPortInfo->mod_momentaryOnByDefault())
                    {
                        mapping.mappingType = MidiControlType::MomentarySwitch;
                    }
                    else if (pPortInfo->trigger_property())
                    {
                        mapping.mappingType = MidiControlType::Trigger;
                    }
                    else if (pPortInfo->IsSwitch())
                    {
                        mapping.mappingType = MidiControlType::Toggle;
                    }
                    else if (pPortInfo->enumeration_property())
                    {
                        mapping.mappingType = MidiControlType::Select;
                    }
                    else if (binding.bindingType() == BINDING_TYPE_TAP_TEMPO) {
                        mapping.mappingType = MidiControlType::TapTempo;
                    }
                    else
                    {
                        mapping.mappingType = MidiControlType::Dial;
                    }
                    if (binding.bindingType() == BINDING_TYPE_NOTE || binding.bindingType() == BINDING_TYPE_TAP_TEMPO)
                    {
                        mapping.key = 0x9000 | binding.note(); // i.e. midi note on.
                    }
                    else if (binding.bindingType() == BINDING_TYPE_CONTROL)
                    {
                        mapping.key = 0xB000 | binding.control(); // i.e. midi control
                    }
                    else
                    {
                        mapping.key = -1;
                    }
                    if (mapping.key != -1)
                    {
                        midiMappings.push_back(std::move(mapping));
                    }
                }
            }
        }
    }
    for (size_t i = 0; i < pedalboardItem.topChain().size(); ++i)
    {
        PrepareMidiMap(pedalboardItem.topChain()[i]);
    }
    for (size_t i = 0; i < pedalboardItem.bottomChain().size(); ++i)
    {
        PrepareMidiMap(pedalboardItem.bottomChain()[i]);
    }
}
void Lv2Pedalboard::PrepareMidiMap(const Pedalboard &pedalboard)
{
    for (size_t i = 0; i < pedalboard.items().size(); ++i)
    {
        auto &item = pedalboard.items()[i];
        PrepareMidiMap(item);
    }
    std::sort(this->midiMappings.begin(), this->midiMappings.end(),
              [](const MidiMapping &left, const MidiMapping &right)
              { return left.key < right.key; });
}

void Lv2Pedalboard::UpdateAudioPorts()
{
    // Called on the audio thread when this pedalboard is swapped in: the outgoing pedalboard no longer runs.
    // Hand the borrowed effects over to this pedalboard (no allocation).
    bool applyingBorrowedEffects = !borrowedEffectsApplied;
    if (applyingBorrowedEffects)
    {
        borrowedEffectsApplied = true;
        for (auto &borrowed : borrowedEffects)
        {
            Lv2Effect *effect = borrowed.effect;
            effect->SetBorrowedEffect(true); // connect its ports below (Lv2Effect::UpdateAudioPorts).
            effect->SetBorrowedAudioBuffers(borrowed.inputBuffers, borrowed.sidechainBuffers, borrowed.outputBuffers);
            if (borrowed.reusedForNewItem)
            {
                effect->SetInstanceId(borrowed.instanceId);
                for (const auto &controlValue : borrowed.controlValues)
                {
                    effect->SetControl(controlValue.first, controlValue.second);
                }
                effect->SetBypass(borrowed.enabled);
            }
        }
    }
    for (int i = 0; i < this->effects.size(); ++i)
    {
        IEffect *effect = this->realtimeEffects[i];
        if (effect->IsLv2Effect())
        {
            Lv2Effect *lv2Effect = (Lv2Effect *)effect;
            lv2Effect->UpdateAudioPorts();
        }
    }
    if (applyingBorrowedEffects)
    {
        // Their ports are now connected to this pedalboard's buffers: from here on they are ordinary
        // effects of this pedalboard (a later borrow stages its wiring again).
        for (auto &borrowed : borrowedEffects)
        {
            borrowed.effect->SetBorrowedEffect(false);
        }
    }
}

void Lv2Pedalboard::Activate()
{
    CrashGuardLock crashGuardLock;

    for (int i = 0; i < this->effects.size(); ++i)
    {
        this->realtimeEffects[i]->Activate();
    }
}
void Lv2Pedalboard::Deactivate()
{
    for (int i = 0; i < this->effects.size(); ++i)
    {
        this->realtimeEffects[i]->Deactivate();
    }
}

static void Copy(float *restrict input, float *restrict output, uint32_t samples)
{
    for (uint32_t i = 0; i < samples; ++i)
    {
        output[i] = input[i];
    }
}
bool Lv2Pedalboard::Run(float **inputBuffers, float **outputBuffers, uint32_t samples, RealtimeRingBufferWriter *ringBufferWriter)
{
    this->ringBufferWriter = ringBufferWriter;
    for (size_t i = 0; i < this->pedalboardInputBuffers.size(); ++i)
    {
        if (inputBuffers[i] == nullptr)
        {
            return false;
        }
    }

    const size_t nInputs = this->pedalboardInputBuffers.size();
    if (this->inputVolume.IsIdle())
    {
        // Steady state: one gain for the whole block, so walk each channel
        // contiguously (channel loop outside, sample loop inside).
        const float volume = this->inputVolume.Tick(); // idle: returns the current gain, no state change.
        for (size_t c = 0; c < nInputs; ++c)
        {
            const float *restrict input = inputBuffers[c];
            float *restrict output = this->pedalboardInputBuffers[c];
            for (size_t i = 0; i < samples; ++i)
            {
                output[i] = input[i] * volume;
            }
        }
    }
    else
    {
        // Ramping: the dezipper must tick exactly once per sample, shared by all channels.
        for (size_t i = 0; i < samples; ++i)
        {
            float volume = this->inputVolume.Tick();
            for (size_t c = 0; c < nInputs; ++c)
            {
                this->pedalboardInputBuffers[c][i] = inputBuffers[c][i] * volume;
            }
        }
    }
    if (!this->suspendBypassedPlugins || this->splitBranchGates.empty())
    {
        for (int i = 0; i < this->processActions.size(); ++i)
        {
            processActions[i](samples);
        }
    }
    else
    {
        // skip split branches that are silent (blend exactly 0, transition complete).
        RunGatedActions(
            this->processActions.size(),
            this->splitBranchGates.data(),
            this->splitBranchGates.size(),
            [this, samples](size_t i)
            { this->processActions[i](samples); },
            [this](const SplitBranchGate &gate)
            { return this->TrySkipBranch(gate); });
    }
    for (size_t i = 0; i < this->realtimeEffects.size(); ++i)
    {
        IEffect *effect = realtimeEffects[i];
        if (effect->HasErrorMessage())
        {
            ringBufferWriter->WriteLv2ErrorMessage(effect->GetInstanceId(), effect->TakeErrorMessage());
        }
    }
    for (size_t i = 0; i < samples; ++i)
    {
        float volume = outputVolume.Tick();
        for (size_t c = 0; c < this->pedalboardOutputBuffers.size(); ++c)
        {
            if (outputBuffers[c] == nullptr) {
                break;
            }
            outputBuffers[c][i] = this->pedalboardOutputBuffers[c][i] * volume;
        }
    }
    this->currentFrameOffset += samples;

    return true;
}

bool Lv2Pedalboard::TrySkipBranch(const SplitBranchGate &gate)
{
    // RT thread.
    if (gate.containsSidechainSource)
    {
        return false;
    }
    bool hasPendingAtomInput = false;
    for (size_t i = gate.effectBegin; i < gate.effectEnd; ++i)
    {
        IEffect *effect = this->realtimeEffects[i];
        if (effect->IsLv2Effect() && ((Lv2Effect *)effect)->HasPendingAtomInput())
        {
            // run the branch this cycle so that patch/MIDI messages aren't lost.
            hasPendingAtomInput = true;
            break;
        }
    }
    bool skip = BypassSuspendPolicy::ShouldSuspendBranch(
        this->suspendBypassedPlugins,
        gate.split->IsBranchSilent(gate.topBranch),
        hasPendingAtomInput,
        gate.containsSidechainSource);
    if (skip)
    {
        // The skipped plugins won't write their atom outputs, which ResetAtomBuffers() left as
        // full-capacity Chunks; make them empty sequences so nothing walks stale bytes.
        // (The range includes the effects of any nested splits.)
        for (size_t i = gate.effectBegin; i < gate.effectEnd; ++i)
        {
            IEffect *effect = this->realtimeEffects[i];
            if (effect->IsLv2Effect())
            {
                ((Lv2Effect *)effect)->WriteEmptyOutputAtomBuffers();
            }
        }
    }
    return skip;
}

float Lv2Pedalboard::GetControlOutputValue(int effectIndex, int portIndex)
{
    auto effect = realtimeEffects[effectIndex];
    return effect->GetOutputControlValue(portIndex);
}

void Lv2Pedalboard::SetControlValue(int effectIndex, int index, float value)
{
    auto effect = realtimeEffects[effectIndex];
    effect->SetControl(index, value);
}
void Lv2Pedalboard::SetBypass(int effectIndex, bool enabled)
{
    auto effect = realtimeEffects[effectIndex];
    effect->SetBypass(enabled);
}


void Lv2Pedalboard::ComputeVus(RealtimeVuBuffers *realtimeVuBuffers, uint32_t samples)
{
    if (realtimeVuBuffers == nullptr) 
    {
        return;
    }
    for (size_t i = 0; i < realtimeVuBuffers->enabledIndexes.size(); ++i)
    {
        auto& rtIndex = realtimeVuBuffers->enabledIndexes[i];
        int index = rtIndex.index;
        if (index == -1) continue;
        if (index == Pedalboard::AUX_START_CONTROL_ID || index == Pedalboard::AUX_END_CONTROL_ID)
        {
            // handled by master VU updates.
            continue;
        }
        VuUpdateX *pUpdate = &realtimeVuBuffers->vuUpdateWorkingData[i];
        if (index == Pedalboard::START_CONTROL_ID)
        {
            if (this->pedalboardInputBuffers.size() == 2)
            {
                // input is handled by master VU updates.
                pUpdate->AccumulateOutputs(&(this->pedalboardInputBuffers[0][0]), &(this->pedalboardInputBuffers[1][0]), samples); // after outputVolume applied.
            }
            else if (this->pedalboardInputBuffers.size() == 1)
            {
                pUpdate->AccumulateOutputs(&(this->pedalboardInputBuffers[0][0]), samples); // before input volume applied.
                // output is handled by master VU updates.
            }
        }
        else if (index == Pedalboard::END_CONTROL_ID)
        {
            if (this->pedalboardOutputBuffers.size() == 2)
            {
                pUpdate->AccumulateInputs(&(this->pedalboardOutputBuffers[0][0]), &(this->pedalboardOutputBuffers[1][0]), samples);
            }
            else if (this->pedalboardOutputBuffers.size() == 1)
            {
                pUpdate->AccumulateInputs(&(this->pedalboardOutputBuffers[0][0]), samples);
            }
        }
        else
        {
            auto effect = this->realtimeEffects[index];

            if (effect->GetNumberOfInputAudioBuffers() == 1)
            {
                pUpdate->AccumulateInputs(effect->GetAudioInputBuffer(0), samples);
            }
            else if (effect->GetNumberOfInputAudioBuffers() >= 2)
            {
                pUpdate->AccumulateInputs(
                    effect->GetAudioInputBuffer(0),
                    effect->GetAudioInputBuffer(1), samples);
            }
            if (effect->GetNumberOfOutputAudioBuffers() == 1)
            {
                pUpdate->AccumulateOutputs(effect->GetAudioOutputBuffer(0), samples);
            }
            else if (effect->GetNumberOfOutputAudioBuffers() >= 2)
            {
                pUpdate->AccumulateOutputs(
                    effect->GetAudioOutputBuffer(0),
                    effect->GetAudioOutputBuffer(1),
                    samples);
            }
        }
    }
}

void Lv2Pedalboard::ResetAtomBuffers()
{
    // Audio thread: use the raw pointer list. Copying the shared_ptr here cost an
    // atomic refcount increment/decrement per effect per cycle.
    for (size_t i = 0; i < this->realtimeEffects.size(); ++i)
    {
        IEffect *effect = this->realtimeEffects[i];
        effect->ResetAtomBuffers();
    }
}

void Lv2Pedalboard::GatherPathPatchProperties(IPatchWriterCallback *cbPatchWriter)
{
    for (IEffect *pEffect : this->realtimeEffects)
    {
        if (pEffect->IsLv2Effect())
        {
            Lv2Effect *pLv2Effect = (Lv2Effect *)pEffect;
            pLv2Effect->GatherPathPatchProperties(cbPatchWriter);
        }
    }
}

void Lv2Pedalboard::ProcessParameterRequests(RealtimePatchPropertyRequest *pParameterRequests, size_t samplesThisTime)
{
    while (pParameterRequests != nullptr)
    {
        pParameterRequests->sampleTimeout -= samplesThisTime;
        IEffect *pEffect = this->GetEffect(pParameterRequests->instanceId);

        if (pParameterRequests->instanceIdLineage != this->instanceIdLineage)
        {
            // addressed to the instance ids of a pedalboard that was replaced by an unrelated one (e.g. a preset
            // switch) before the audio thread received the request: the id may name a different plugin here.
            pParameterRequests->errorMessage = "The pedalboard has changed.";
        }
        else if (pEffect == nullptr)
        {
            pParameterRequests->errorMessage = "No such effect.";
        }
        else if (pEffect->IsVst3())
        {
            pParameterRequests->errorMessage = "Not supported for VST3 plugins";
        }
        else if (pParameterRequests->sampleTimeout < 0)
        {
            pParameterRequests->sampleTimeout = 0;
            pParameterRequests->errorMessage = "Timed out.";
        }
        else
        {
            if (pEffect->IsLv2Effect())
            {
                Lv2Effect *pLv2Effect = dynamic_cast<Lv2Effect *>(pEffect);

                if (pParameterRequests->requestType == RealtimePatchPropertyRequest::RequestType::PatchGet)
                {
                    pLv2Effect->RequestPatchProperty(pParameterRequests->uridUri);
                }
                else if (pParameterRequests->requestType == RealtimePatchPropertyRequest::RequestType::PatchSet)
                {
                    pLv2Effect->SetPatchProperty(
                        pParameterRequests->uridUri,
                        pParameterRequests->GetSize(),
                        (LV2_Atom *)pParameterRequests->GetBuffer());
                    pLv2Effect->SetRequestStateChangedNotification(true);
                }
            }
        }
        pParameterRequests = pParameterRequests->pNext;
    }
}

void Lv2Pedalboard::GatherPatchProperties(RealtimePatchPropertyRequest *pParameterRequests)
{
    while (pParameterRequests != nullptr)
    {
        if (pParameterRequests->requestType == RealtimePatchPropertyRequest::RequestType::PatchGet &&
            pParameterRequests->instanceIdLineage == this->instanceIdLineage) // (else rejected by ProcessParameterRequests)
        {
            IEffect *effect = this->GetEffect(pParameterRequests->instanceId);
            if (effect == nullptr)
            {
                pParameterRequests->errorMessage = "No such effect.";
            }
            else if (effect->IsVst3())
            {
                pParameterRequests->errorMessage = "Not supported for VST3";
            }
            else
            {
                if (effect->IsLv2Effect())
                {
                    Lv2Effect *pLv2Effect = dynamic_cast<Lv2Effect *>(effect);
                    pLv2Effect->GatherPatchProperties(pParameterRequests);
                }
            }
        }

        pParameterRequests = pParameterRequests->pNext;
    }
}

void Lv2Pedalboard::OnMidiMessage(
    const MidiEvent&event,
    void *callbackHandle,
    MidiCallbackFn *pfnCallback)

{
    if (midiMappings.size() == 0)
        return;

    size_t size = event.size;
    const uint8_t *message = event.buffer;
    if (size < 2)
        return;
    uint8_t cmd = message[0];
    uint8_t channel = cmd & 0x0F;
    cmd &= 0xF0;

    uint8_t value;
    uint8_t index;
    if (cmd == 0x80) // note off.
    {
        index = message[1];
        cmd = 0x90;
        index = message[1];
        value = 0;
    }
    else if (cmd == 0x90) // note on.
    {
        if (size < 3)
            return;
        index = message[1];
        value = message[2] == 0 ? 0 : 127; // zero velocity = note off.
    }
    else if (cmd == 0xB0) // midi control.
    {
        if (size < 3)
            return;
        index = message[1];
        value = message[2] & 0x7F;
    }
    int searchKey = (cmd << 8) | index;
    int min = 0;
    int max = midiMappings.size() - 1;
    while (max > min)
    {
        int mid = (min + max) / 2;
        if (midiMappings[mid].key < searchKey)
        {
            min = mid + 1;
        }
        else if (midiMappings[mid].key > searchKey)
        {
            max = mid - 1;
        }
        else
        {
            if (mid == 0)
            {
                min = max = mid;
            }
            else
            {
                if (midiMappings[mid - 1].key == searchKey)
                {
                    max = mid - 1;
                }
                else
                {
                    min = max = mid;
                }
            }
        }
    }
    if (midiMappings[min].key == searchKey)
    {
        float range = value / 127.0;

        for (int i = min; i < midiMappings.size(); ++i)
        {
            MidiMapping &mapping = midiMappings[i];
            if (mapping.key != searchKey)
                break;

            if (mapping.midiBinding.channel() == -1 || mapping.midiBinding.channel() == channel)
            {
                switch (mapping.mappingType)
                {
                case MidiControlType::Trigger:
                {
                    bool triggered = false;
                    if (mapping.midiBinding.switchControlType() == SwitchControlTypeT::TRIGGER_ON_RISING_EDGE || mapping.midiBinding.bindingType() == BINDING_TYPE_NOTE)
                    {
                        if (mapping.lastValue < range)
                        {
                            if (!mapping.lastValueIncreasing)
                            {
                                triggered = true;
                            }
                            mapping.lastValueIncreasing = true;
                            mapping.lastValue = range;
                        }
                        else
                        {
                            mapping.lastValueIncreasing = false;
                            mapping.lastValue = range;
                        }
                    }
                    else
                    {
                        triggered = true;
                    }
                    if (triggered)
                    {
                        IEffect *pEffect = this->realtimeEffects[mapping.effectIndex];
                        float value = mapping.pPortInfo->max_value();
                        if (value == mapping.pPortInfo->default_value())
                        {
                            value = mapping.pPortInfo->min_value();
                        }
                        this->SetControlValue(mapping.effectIndex, mapping.controlIndex, value);
                        // do NOT notify anyone!
                    }
                    break;
                }
                case MidiControlType::Toggle:
                {
                    bool triggered = false;

                    range = std::round(range);

                    if (mapping.midiBinding.switchControlType() == SwitchControlTypeT::TOGGLE_ON_RISING_EDGE)
                    {
                        if (range > mapping.lastValue)
                        {
                            if (!mapping.lastValueIncreasing)
                            {
                                triggered = true;
                            }
                            mapping.lastValueIncreasing = true;
                            mapping.lastValue = range;
                        }
                        else
                        {
                            mapping.lastValueIncreasing = false;
                            mapping.lastValue = range;
                        }
                        if (triggered)
                        {
                            IEffect *pEffect = this->realtimeEffects[mapping.effectIndex];
                            float currentValue = pEffect->GetControlValue(mapping.controlIndex);

                            currentValue = currentValue == 0 ? 1 : 0;
                            pEffect->SetControl(mapping.controlIndex, currentValue);
                            pfnCallback(callbackHandle, mapping.instanceId, mapping.pPortInfo->index(), currentValue);
                        }
                    }
                    else if (mapping.midiBinding.switchControlType() == SwitchControlTypeT::TOGGLE_ON_VALUE)
                    {
                        triggered = true;
                        mapping.lastValue = range;
                        IEffect *pEffect = this->realtimeEffects[mapping.effectIndex];
                        float currentValue = pEffect->GetControlValue(mapping.controlIndex);
                        if (currentValue != range)
                        {
                            pEffect->SetControl(mapping.controlIndex, range);
                            pfnCallback(callbackHandle, mapping.instanceId, mapping.pPortInfo->index(), range);
                        }
                    }
                    else
                    {
                        // any control value toggles.
                        triggered = true;
                        mapping.lastValue = range;
                        IEffect *pEffect = this->realtimeEffects[mapping.effectIndex];
                        float currentValue = pEffect->GetControlValue(mapping.controlIndex);
                        currentValue = currentValue == 0 ? 1 : 0;
                        pEffect->SetControl(mapping.controlIndex, currentValue);
                        pfnCallback(callbackHandle, mapping.instanceId, mapping.pPortInfo->index(), currentValue);
                    }
                    break;
                }
                case MidiControlType::MomentarySwitch:
                {
                    IEffect *pEffect = this->realtimeEffects[mapping.effectIndex];
                    pEffect->SetControl(mapping.controlIndex, range != 0 ? mapping.pPortInfo->max_value() : mapping.pPortInfo->min_value());
                    // do NOT notify anyone!
                }
                break;

                case MidiControlType::Select:
                case MidiControlType::Dial:
                {
                    IEffect *pEffect = this->realtimeEffects[mapping.effectIndex];
                    float range = mapping.midiBinding.calculateRange(value);
                    float currentValue = mapping.pPortInfo->rangeToValue(range);
                    if (pEffect->GetControlValue(mapping.controlIndex) != currentValue)
                    {
                        this->SetControlValue(mapping.effectIndex, mapping.controlIndex, currentValue);
                        pfnCallback(callbackHandle, mapping.instanceId, mapping.pPortInfo->index(), currentValue);
                    }
                    break;
                }
                case MidiControlType::TapTempo:
                {
                    handleTapTempo(value, event.timeStamp, mapping,  callbackHandle, pfnCallback);
                    break;
                }
                case MidiControlType::None:
                default:
                    break;
                }
            }
        }
    }
}

void Lv2Pedalboard::handleTapTempo(uint8_t value, const MidiTimestamp& timestamp, MidiMapping &mapping, void *callbackHandle, MidiCallbackFn *pfnSetControlCallback)
{
    if (value != 0) // only on note on
    {
        if (!mapping.lastTapTimestamp.isEmpty())
        {
            double seconds = timestamp.timeDiff(mapping.lastTapTimestamp); 
            if (seconds > 60.0/450.0) // debounce check. (~= 450bpm)
            {
                auto units = mapping.pPortInfo->units();
                float controlValue = -1;
                switch (units)
                {
                case Units::bpm:
                {
                    controlValue = 60.0f / (float)seconds;
                    break;
                }
                case Units::hz:
                {
                    controlValue = 1.0f / (float)seconds;
                    break;
                }
                case Units::s:
                {
                    controlValue = (float)(seconds);
                    break;
                }
                case Units::ms:
                {
                    controlValue = (float)(seconds * 1000.0);
                    break;
                }
                default:
                {
                    controlValue = -1;
                }
                }
                if (mapping.pPortInfo->min_value() < mapping.pPortInfo->max_value())
                {
                    if (controlValue < mapping.pPortInfo->min_value())
                    {
                        controlValue = -1;
                    }
                    else if (controlValue > mapping.pPortInfo->max_value())
                    {
                        controlValue = -1;
                    }
                }
                else
                {
                    if (controlValue > mapping.pPortInfo->min_value())
                    {
                        controlValue = -1;
                    }
                    else if (controlValue < mapping.pPortInfo->max_value())
                    {
                        controlValue = -1;
                    }
                }
                if (controlValue != -1)
                {

                    IEffect *pEffect = this->realtimeEffects[mapping.effectIndex];
                    if (pEffect->IsLv2Effect())
                    {
                        Lv2Effect *pLv2Effect = dynamic_cast<Lv2Effect *>(pEffect);
                        pEffect->SetControl(mapping.controlIndex, controlValue);
                        pfnSetControlCallback(callbackHandle, mapping.instanceId, mapping.pPortInfo->index(), controlValue);
                    }
                }
            }
        }
        mapping.lastTapTimestamp = timestamp;
    }
}


size_t Lv2Pedalboard::GetNumberOfAudioInputChannels() const {
    return pHost->GetChannelSelection().mainInputChannels().size();
}

size_t Lv2Pedalboard::GetNumberOfAudioOutputChannels() const {
    return pHost->GetChannelSelection().mainOutputChannels().size();
}