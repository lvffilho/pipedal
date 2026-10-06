// Copyright (c) Robin E.R. Davies
// MIT license; see the license text at the top of other source files.

#pragma once

namespace pipedal
{
    // Holds /dev/cpu_dma_latency open with a requested latency of 0 while audio runs,
    // to keep the CPU out of deep C-states. Failure to open is logged once and ignored.
    class CpuDmaLatency
    {
    public:
        CpuDmaLatency() = default;
        CpuDmaLatency(const CpuDmaLatency &) = delete;
        CpuDmaLatency &operator=(const CpuDmaLatency &) = delete;
        ~CpuDmaLatency() { Release(); }

        // Not for use on the audio thread (opens a file).
        void Hold(const char *path = "/dev/cpu_dma_latency");
        void Release();
        bool IsHeld() const { return fd != -1; }

    private:
        int fd = -1;
    };
}
