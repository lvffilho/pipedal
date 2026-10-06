// Copyright (c) Robin E.R. Davies
// MIT license; see the license text at the top of other source files.

#include "CpuDmaLatency.hpp"
#include "Lv2Log.hpp"
#include "ss.hpp"
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

using namespace pipedal;

void CpuDmaLatency::Hold(const char *path)
{
    if (fd != -1)
        return;
    static std::atomic<bool> warned{false};

    int f = open(path, O_WRONLY | O_CLOEXEC);
    if (f == -1)
    {
        int e = errno;
        if (!warned.exchange(true))
        {
            Lv2Log::warning(SS("Unable to open " << path << " (" << strerror(e) << "). CPU idle latency will not be limited."));
        }
        return;
    }
    int32_t value = 0;
    if (write(f, &value, sizeof(value)) != (ssize_t)sizeof(value))
    {
        int e = errno;
        if (!warned.exchange(true))
        {
            Lv2Log::warning(SS("Unable to write to " << path << " (" << strerror(e) << ")."));
        }
        close(f);
        return;
    }
    fd = f; // the request lasts for as long as the fd stays open.
}

void CpuDmaLatency::Release()
{
    if (fd != -1)
    {
        close(fd);
        fd = -1;
    }
}
