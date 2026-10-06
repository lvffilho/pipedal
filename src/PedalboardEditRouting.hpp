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

// Helpers that route model edits and plugin notifications to the right pedalboard while pedalboard builds are
// outstanding (see the threading notes above PiPedalModel::LoadCurrentPedalboard()).

#include "Pedalboard.hpp"
#include "PresetInstanceReuse.hpp"
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <vector>

namespace pipedal
{
    // Copies the VST3 state of `source`'s items into the items of `target` that have the same instance id and
    // plugin URI (VST3 items only; an empty source state is not copied).
    //
    // Used while a full build is outstanding: the running pedalboard's instance ids may then name unrelated
    // plugins, so VST3 state can't be read from the running instances. The outstanding build creates its VST3
    // instances from the model's pedalboard, which therefore holds their current state until it is installed.
    inline void MergeVst3State(Pedalboard &target, const Pedalboard &source)
    {
        for (PedalboardItem *item : target.GetAllPlugins())
        {
            if (!IsVst3PluginUri(item->uri()))
            {
                continue;
            }
            const PedalboardItem *sourceItem = source.GetItem(item->instanceId());
            if (sourceItem && sourceItem->uri() == item->uri() && !sourceItem->vstState().empty())
            {
                item->vstState(sourceItem->vstState());
            }
        }
    }

    // Patch property requests (get/set) that arrive while a full build is outstanding, when they can't be sent to
    // the running pedalboard (its instance ids may name unrelated plugins). PiPedalModel sends them to the new
    // pedalboard once it is installed, or fails them if the build never is. Not thread-safe (owner's lock).
    //
    // Bounded: a set of the same instance and property replaces the queued one (only the latest value matters;
    // the replaced request is handed back to the caller to complete), and requests beyond `capacity` are refused.
    template <typename REQUEST>
    class DeferredPatchRequests
    {
    public:
        struct Entry
        {
            int64_t clientId; // the client whose callbacks `request` holds.
            int64_t instanceId;
            std::string propertyUri;
            bool isSet;
            REQUEST request;
        };
        enum class AddResult
        {
            Queued,
            Replaced, // a queued set of the same instance and property was replaced; see `replaced`.
            Full,     // refused: the request is left in `request`.
        };

        explicit DeferredPatchRequests(size_t capacity = DEFAULT_CAPACITY) : capacity(capacity) {}
        static constexpr size_t DEFAULT_CAPACITY = 64;

        AddResult Add(int64_t clientId, int64_t instanceId, const std::string &propertyUri, bool isSet, REQUEST &request, std::optional<REQUEST> *replaced)
        {
            if (isSet)
            {
                for (Entry &entry : entries)
                {
                    if (entry.isSet && entry.instanceId == instanceId && entry.propertyUri == propertyUri)
                    {
                        if (replaced)
                        {
                            replaced->emplace(std::move(entry.request));
                        }
                        entry.request = std::move(request);
                        entry.clientId = clientId;
                        return AddResult::Replaced;
                    }
                }
            }
            if (entries.size() >= capacity)
            {
                return AddResult::Full;
            }
            entries.push_back(Entry{clientId, instanceId, propertyUri, isSet, std::move(request)});
            return AddResult::Queued;
        }
        // Discards a client's requests without completing them (its callbacks may refer to a closed connection).
        // Returns the number removed.
        size_t RemoveClient(int64_t clientId)
        {
            return std::erase_if(entries, [clientId](const Entry &entry)
                                 { return entry.clientId == clientId; });
        }
        // Removes and returns all queued requests, oldest first.
        std::vector<Entry> TakeAll()
        {
            std::vector<Entry> result = std::move(entries);
            entries.clear();
            return result;
        }
        // Removes all queued requests and calls send(entry) for each, oldest first. If send throws, fail(entry, e)
        // is called for that entry, and the remaining ones are still sent.
        template <typename SEND, typename FAIL>
        void SendAll(SEND &&send, FAIL &&fail)
        {
            for (Entry &entry : TakeAll())
            {
                try
                {
                    send(entry);
                }
                catch (const std::exception &e)
                {
                    fail(entry, e);
                }
            }
        }
        size_t size() const { return entries.size(); }
        bool empty() const { return entries.empty(); }

    private:
        size_t capacity;
        std::vector<Entry> entries;
    };

    // What PiPedalModel::InstallBuiltPedalboard() does with the deferred patch requests when a build ends.
    enum class DeferredPatchRequestsAction
    {
        Send, // a pedalboard whose instance ids are those of the model was installed.
        Fail, // no pedalboard that could answer them is coming.
        Keep, // a newer build is outstanding.
    };
    inline DeferredPatchRequestsAction OnBuildEndedDeferredPatchRequests(bool installed, bool runningInstanceIdsMatch, bool builderIdle)
    {
        if (installed && runningInstanceIdsMatch)
        {
            return DeferredPatchRequestsAction::Send;
        }
        return builderIdle ? DeferredPatchRequestsAction::Fail : DeferredPatchRequestsAction::Keep;
    }

    // Index of `element` in `elements`, or -1. Used to identify the instance that sent a notification by its
    // address rather than by its instance id (which is ambiguous between two pedalboards of different lineages).
    template <typename T>
    int IndexOfPointer(const std::vector<T *> &elements, const void *element)
    {
        if (element == nullptr)
        {
            return -1;
        }
        for (size_t i = 0; i < elements.size(); ++i)
        {
            if ((const void *)elements[i] == element)
            {
                return (int)i;
            }
        }
        return -1;
    }
}
