#pragma once
// RT-safe deferred MIDI buffer: fixed storage, no allocation, no locks.
// Layout: repeated [size(uint8_t), bytes...].
#include <cstddef>
#include <cstdint>

namespace pipedal
{
    enum class DeferredMidiReplayResult
    {
        Continue,
        ProgramChangePending, // stop; leave buffer untouched
        SnapshotPending,      // stop; consume what was replayed, keep the tail
    };

    template <size_t BUFFER_SIZE>
    class DeferredMidiBuffer
    {
    public:
        size_t count = 0;
        uint8_t data[BUFFER_SIZE];

        void Clear() { count = 0; }

        bool Push(const uint8_t *bytes, size_t size)
        {
            if (size > 0 && size < 128 && size + 1 + count <= BUFFER_SIZE)
            {
                data[count++] = (uint8_t)size;
                for (size_t i = 0; i < size; ++i)
                    data[count++] = bytes[i];
                return true;
            }
            return false;
        }

        // fn(const uint8_t *bytes, size_t size) -> DeferredMidiReplayResult
        template <typename FN>
        void Replay(FN &&fn)
        {
            for (size_t i = 0; i < count; /**/)
            {
                uint8_t size = data[i++];
                const uint8_t *bytes = data + i;
                DeferredMidiReplayResult result = fn(bytes, (size_t)size);
                if (result == DeferredMidiReplayResult::ProgramChangePending)
                    return;
                i += size;
                if (result == DeferredMidiReplayResult::SnapshotPending)
                {
                    size_t remaining = count - i;
                    for (size_t j = 0; j < remaining; ++j)
                        data[j] = data[i + j];
                    count = remaining;
                    return;
                }
            }
            count = 0;
        }
    };
}
