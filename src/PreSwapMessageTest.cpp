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

// Messages from the audio thread produced by an outgoing pedalboard (source identity), and
// parameter request completions that must never be lost.

#include "pch.h"
#include "catch.hpp"
#include "RingBuffer.hpp"
#include "RingBufferReader.hpp"

#include <cstring>
#include <memory>
#include <vector>

using namespace pipedal;

namespace
{
    // Stand-in for Lv2Pedalboard's effects and their instance ids.
    struct FakePedalboard
    {
        std::vector<IEffect *> effects;
        std::vector<uint64_t> instanceIds;
        void Add(uint64_t instanceId, IEffect *effect)
        {
            instanceIds.push_back(instanceId);
            effects.push_back(effect);
        }
        std::vector<IEffect *> &GetEffects() { return effects; }
        uint64_t GetInstanceIdAt(size_t index) const { return instanceIds.at(index); }
    };

    // The instance id the installed pedalboard gives the sender, or -1 if the message is dropped.
    int64_t Resolve(FakePedalboard *installed, const IEffect *sender)
    {
        uint64_t instanceId = 0;
        return FindInstalledSender(installed, sender, &instanceId) ? (int64_t)instanceId : -1;
    }

    // Distinct effect identities. Only compared, never dereferenced.
    IEffect *EffectId(intptr_t n) { return (IEffect *)(intptr_t)(0x1000 * n); }

    struct RequestLog
    {
        std::vector<int64_t> completed; // clientIds, in completion order.
    };

    std::unique_ptr<RealtimePatchPropertyRequest> MakeRequest(RequestLog &log, int64_t clientId)
    {
        return std::make_unique<RealtimePatchPropertyRequest>(
            [&log](RealtimePatchPropertyRequest *pRequest)
            { log.completed.push_back(pRequest->clientId); },
            clientId, 1, 0,
            [](const std::string &) {},
            [](const std::string &) {},
            0);
    }

    void CompleteChain(RealtimePatchPropertyRequest *pRequest)
    {
        while (pRequest != nullptr)
        {
            auto pNext = pRequest->pNext;
            pRequest->onPatchRequestComplete(pRequest);
            pRequest = pNext;
        }
    }

    // Fills the ring with Lv2StateChanged messages. Returns the number written.
    int FillRing(RealtimeRingBufferWriter &writer)
    {
        int filler = 0;
        uint64_t unused = 0;
        while (writer.write(RingBufferCommand::Lv2StateChanged, unused))
        {
            ++filler;
        }
        writer.TakeDroppedWrites();
        return filler;
    }
}

TEST_CASE("MidiValueChanged and AtomOutput carry the sending effect", "[audio_messages]")
{
    RingBuffer<false, true> ring(4096, false);
    RealtimeRingBufferWriter writer(&ring);
    HostRingBufferReader reader(&ring);

    writer.MidiValueChanged(7, 3, 0.25f, EffectId(1), (Lv2Pedalboard *)(intptr_t)0x500);
    LV2_Atom_Int atom{{sizeof(int32_t), 99}, 42};
    writer.AtomOutput(8, EffectId(2), &atom.atom);

    RingBufferCommand command;
    reader.read(&command);
    REQUIRE(command == RingBufferCommand::MidiValueChanged);
    MidiValueChangedBody body;
    reader.read(&body);
    REQUIRE(body.instanceId == 7);
    REQUIRE(body.controlIndex == 3);
    REQUIRE(body.value == 0.25f);
    REQUIRE(body.sourceEffect == EffectId(1));
    REQUIRE(body.sourcePedalboard == (Lv2Pedalboard *)(intptr_t)0x500);

    reader.read(&command);
    REQUIRE(command == RingBufferCommand::AtomOutput);
    AtomOutputBody header;
    reader.read(&header);
    REQUIRE(header.instanceId == 8);
    REQUIRE(header.sourceEffect == EffectId(2));
    size_t size;
    reader.read(&size);
    REQUIRE(size == sizeof(atom));
    LV2_Atom_Int received;
    reader.read(size, (uint8_t *)&received);
    REQUIRE(received.body == 42);
    REQUIRE(reader.readSpace() == 0);
}

TEST_CASE("Messages from the outgoing pedalboard don't apply to the installed one", "[audio_messages]")
{
    // Outgoing pedalboard: item 7 is effect A. A full build (preset switch) installs a pedalboard whose
    // item 7 is an unrelated effect C, and the audio thread has not swapped it in yet.
    FakePedalboard outgoing;
    outgoing.Add(7, EffectId(1));
    FakePedalboard installed;
    installed.Add(7, EffectId(3));

    // A message A sent before the swap (naming item 7) is not applied to the new item 7.
    REQUIRE(Resolve(&outgoing, EffectId(1)) == 7);
    REQUIRE(Resolve(&installed, EffectId(1)) == -1);
    // After the swap, C's own messages apply.
    REQUIRE(Resolve(&installed, EffectId(3)) == 7);

    // Instance A reused for the same item: its messages still apply, before and after the swap.
    FakePedalboard reusedSameItem;
    reusedSameItem.Add(7, EffectId(1));
    REQUIRE(Resolve(&reusedSameItem, EffectId(1)) == 7);

    // Instance A reused for a new item 9 (item 7 is now another effect): A still sends id 7 until the
    // swap; its messages are re-keyed to item 9, never applied to the new item 7.
    FakePedalboard reusedNewItem;
    reusedNewItem.Add(7, EffectId(4));
    reusedNewItem.Add(9, EffectId(1));
    REQUIRE(Resolve(&reusedNewItem, EffectId(1)) == 9);
    REQUIRE(Resolve(&reusedNewItem, EffectId(4)) == 7);

    // No identity, or no installed pedalboard: never applied.
    REQUIRE(Resolve(&installed, nullptr) == -1);
    REQUIRE(Resolve(nullptr, EffectId(3)) == -1);
}

TEST_CASE("MIDI value changes from the outgoing pedalboard are always dropped", "[audio_messages]")
{
    // Distinct pedalboard objects; only compared.
    FakePedalboard outgoing;
    outgoing.Add(7, EffectId(1));
    outgoing.Add(8, EffectId(2));

    // Reuse build (same lineage): the installed pedalboard reuses both instances for the same items.
    FakePedalboard reuse;
    reuse.Add(7, EffectId(1));
    reuse.Add(8, EffectId(2));
    // Before the swap, the outgoing pedalboard is still running: its values are dropped, even though
    // the sender resolves to the same item (the swap applies the installed item's value).
    REQUIRE(Resolve(&reuse, EffectId(1)) == 7);
    REQUIRE_FALSE(IsMidiValueFromInstalledPedalboard(&reuse, &outgoing, 7, EffectId(1)));
    // After the swap, values come from the installed pedalboard and apply.
    REQUIRE(IsMidiValueFromInstalledPedalboard(&reuse, &reuse, 7, EffectId(1)));
    REQUIRE(IsMidiValueFromInstalledPedalboard(&reuse, &reuse, 8, EffectId(2)));

    // Full build reusing instance 1 for a new item 9: pre-swap values are dropped (not re-keyed).
    FakePedalboard full;
    full.Add(7, EffectId(3));
    full.Add(9, EffectId(1));
    REQUIRE_FALSE(IsMidiValueFromInstalledPedalboard(&full, &outgoing, 7, EffectId(1)));
    REQUIRE(IsMidiValueFromInstalledPedalboard(&full, &full, 9, EffectId(1)));
    // The installed pedalboard, but an id the sender doesn't have there: dropped.
    REQUIRE_FALSE(IsMidiValueFromInstalledPedalboard(&full, &full, 7, EffectId(1)));

    // Nothing installed, no sender, no source pedalboard: dropped.
    REQUIRE_FALSE(IsMidiValueFromInstalledPedalboard((FakePedalboard *)nullptr, nullptr, 7, EffectId(1)));
    REQUIRE_FALSE(IsMidiValueFromInstalledPedalboard(&full, &full, 7, nullptr));
    REQUIRE_FALSE(IsMidiValueFromInstalledPedalboard(&full, nullptr, 9, EffectId(1)));
}

TEST_CASE("Parameter request completions are deferred, never lost", "[audio_messages][rt_hygiene]")
{
    RingBuffer<false, true> ring(256, false);
    RealtimeRingBufferWriter writer(&ring);
    HostRingBufferReader reader(&ring);
    RequestLog log;
    auto r1 = MakeRequest(log, 1);
    auto r2 = MakeRequest(log, 2);
    auto r3 = MakeRequest(log, 3);
    auto r4 = MakeRequest(log, 4);
    r2->pNext = r3.get(); // the audio thread completes chains.

    int filler = FillRing(writer);

    // Every release entry used, and one more release lost.
    for (size_t i = 0; i < RealtimeRingBufferWriter::MAX_DEFERRED_RELEASES - 1; ++i)
    {
        writer.FreeVuSubscriptions((RealtimeVuBuffers *)(intptr_t)(0x100 + i));
    }
    writer.FreeVuSubscriptions((RealtimeVuBuffers *)(intptr_t)0x999);
    REQUIRE(writer.TakeLostReleases() == 1);

    // Completions still find room: the first takes the kept entry, the next joins its chain.
    writer.ParameterRequestComplete(r1.get());
    writer.ParameterRequestComplete(r2.get());
    writer.ParameterRequestComplete(r4.get()); // joins at the kept tail (r3), not by walking the chain.
    REQUIRE(writer.DeferredReleaseCount() == RealtimeRingBufferWriter::MAX_DEFERRED_RELEASES);
    REQUIRE(writer.TakeLostReleases() == 0);
    REQUIRE(writer.DroppedWrites() == 0);

    // The reader drains; retries deliver everything, the completion as a single chain.
    std::vector<RealtimePatchPropertyRequest *> completions;
    size_t releases = 0;
    for (int iteration = 0; iteration < 100 && (writer.DeferredReleaseCount() != 0 || reader.readSpace() != 0); ++iteration)
    {
        while (reader.readSpace() > sizeof(RingBufferCommand))
        {
            RingBufferCommand command;
            reader.read(&command);
            int64_t value;
            reader.read(&value);
            if (command == RingBufferCommand::ParameterRequestComplete)
            {
                completions.push_back((RealtimePatchPropertyRequest *)(intptr_t)value);
            }
            else if (command == RingBufferCommand::FreeVuSubscriptions)
            {
                ++releases;
            }
        }
        writer.RetryReleases();
    }
    (void)filler;
    REQUIRE(writer.DeferredReleaseCount() == 0);
    REQUIRE(releases == RealtimeRingBufferWriter::MAX_DEFERRED_RELEASES - 1);
    REQUIRE(completions.size() == 1);
    CompleteChain(completions[0]);
    REQUIRE(log.completed == std::vector<int64_t>{1, 2, 3, 4});
}

TEST_CASE("Completions waiting for ring space are handed back at close", "[audio_messages][rt_hygiene]")
{
    RingBuffer<false, true> ring(256, false);
    RealtimeRingBufferWriter writer(&ring);
    RequestLog log;
    auto r1 = MakeRequest(log, 1);
    auto r2 = MakeRequest(log, 2);

    FillRing(writer);
    writer.EffectReplaced((Lv2Pedalboard *)(intptr_t)0x10);
    writer.ParameterRequestComplete(r1.get());
    writer.ParameterRequestComplete(r2.get());
    REQUIRE(writer.DeferredReleaseCount() == 2);

    // Close(): the audio thread has stopped; complete what is still waiting.
    std::vector<RingBufferCommand> commands;
    writer.TakeDeferredReleases(
        [&](RingBufferCommand command, void *pointer)
        {
            commands.push_back(command);
            if (command == RingBufferCommand::ParameterRequestComplete)
            {
                CompleteChain((RealtimePatchPropertyRequest *)pointer);
            }
        });
    REQUIRE(commands == std::vector<RingBufferCommand>{RingBufferCommand::EffectReplaced, RingBufferCommand::ParameterRequestComplete});
    REQUIRE(writer.DeferredReleaseCount() == 0);
    REQUIRE(log.completed == std::vector<int64_t>{1, 2});
}

TEST_CASE("Draining the audio->host ring at close completes requests found there", "[audio_messages]")
{
    RingBuffer<false, true> ring(8192, false);
    RealtimeRingBufferWriter writer(&ring);
    HostRingBufferReader reader(&ring);
    RequestLog log;
    auto r1 = MakeRequest(log, 1);
    auto r2 = MakeRequest(log, 2);
    auto r3 = MakeRequest(log, 3);
    r1->pNext = r2.get();

    // Every kind of audio->host message, completions in between.
    writer.MidiValueChanged(1, 2, 0.5f, EffectId(1), nullptr);
    writer.OnMidiListen(MidiNotifyBody(0xB0, 1, 2));
    writer.ParameterRequestComplete(r1.get());
    LV2_Atom_Int atom{{sizeof(int32_t), 99}, 42};
    writer.AtomOutput(8, EffectId(2), &atom.atom);
    writer.WriteLv2ErrorMessage(4, "error text");
    writer.WriteLv2ErrorMessage(4, "");
    writer.Lv2StateChanged(4);
    writer.MaybeLv2StateChanged(4);
    writer.EffectReplaced((Lv2Pedalboard *)(intptr_t)0x10);
    writer.FreeVuSubscriptions((RealtimeVuBuffers *)(intptr_t)0x20);
    writer.FreeMonitorPortSubscriptions((RealtimeMonitorPortSubscriptions *)(intptr_t)0x30);
    writer.FreeSnapshot((IndexedSnapshot *)(intptr_t)0x40);
    writer.SendVuUpdate((const std::vector<VuUpdateX> *)(intptr_t)0x50);
    writer.SendMonitorPortUpdate(nullptr, 5, 1.0f);
    writer.SendPathPropertyBuffer((PatchPropertyWriter::Buffer *)(intptr_t)0x60);
    writer.OnMidiProgramChange(1, 2, 3);
    writer.OnNextMidiProgram(1, 1);
    writer.OnNextMidiBank(1, 1);
    writer.OnNextMidiSnapshot(1, 1);
    writer.OnRealtimeMidiEvent(RealtimeMidiEventType::StartHotspot);
    writer.OnRealtimeMidiSnapshotRequest(1, 2);
    writer.AlsaRestartRequested();
    writer.AudioTerminatedAbnormally();
    writer.ParameterRequestComplete(r3.get());
    REQUIRE(writer.DroppedWrites() == 0);
    REQUIRE(writer.DeferredReleaseCount() == 0);

    size_t messages = DrainRealtimeOutputRing(reader, [](RealtimePatchPropertyRequest *pRequest)
                                              { CompleteChain(pRequest); });
    REQUIRE(messages == 24);
    REQUIRE(reader.readSpace() == 0);
    REQUIRE(log.completed == std::vector<int64_t>{1, 2, 3});
}
