#include "pch.h"
#include "catch.hpp"
#include "CpuUse.hpp"
#include <chrono>
#include <cmath>

using namespace pipedal;

namespace
{
    struct FakeClock
    {
        using duration = std::chrono::steady_clock::duration;
        using rep = duration::rep;
        using period = duration::period;
        using time_point = std::chrono::time_point<FakeClock>;
        static constexpr bool is_steady = true;
        static time_point current;
        static time_point now() { return current; }
        static void Advance(std::chrono::microseconds us) { current += std::chrono::duration_cast<duration>(us); }
    };
    FakeClock::time_point FakeClock::current{};
    using Us = std::chrono::microseconds;
}

TEST_CASE("CpuUse: DSP load is processing time over period", "[cpu_use]")
{
    CpuUseT<FakeClock> cpu;
    cpu.SetPeriod(std::chrono::duration_cast<FakeClock::duration>(Us(1000)));
    cpu.SetStartTime(cpu.Now());
    for (int i = 0; i < 40; ++i)
    {
        cpu.UpdateCpuUse();
        FakeClock::Advance(Us(500)); // blocked in read
        cpu.AddSample(ProfileCategory::Read);
        FakeClock::Advance(Us(50)); // copy in
        cpu.AddSample(ProfileCategory::Driver);
        FakeClock::Advance(Us(200)); // DSP
        cpu.AddSample(ProfileCategory::Execute);
        FakeClock::Advance(Us(50)); // copy out
        cpu.AddSample(ProfileCategory::Driver);
        FakeClock::Advance(Us(200)); // blocked in write
        cpu.AddSample(ProfileCategory::Write);
    }
    cpu.UpdateCpuUse();
    // processing = 50+200+50 = 300us of a 1000us period; Driver not double counted.
    CHECK(std::abs(cpu.GetCpuUse() - 30.0f) < 0.01f);
    // overhead = driver copies only: 100us / 1000us.
    CHECK(std::abs(cpu.GetCpuOverhead() - 10.0f) < 0.01f);
}

TEST_CASE("CpuUse: zero before any samples", "[cpu_use]")
{
    CpuUseT<FakeClock> cpu;
    cpu.UpdateCpuUse();
    CHECK(cpu.GetCpuUse() == 0.0f);
    CHECK(cpu.GetCpuOverhead() == 0.0f);
}

namespace
{
    // One cycle: blocked in read, then driver, DSP, then blocked in write.
    void RunCycle(CpuUseT<FakeClock> &cpu, int readUs, int driverUs, int executeUs, int writeUs)
    {
        cpu.UpdateCpuUse();
        FakeClock::Advance(Us(readUs));
        cpu.AddSample(ProfileCategory::Read);
        FakeClock::Advance(Us(driverUs));
        cpu.AddSample(ProfileCategory::Driver);
        FakeClock::Advance(Us(executeUs));
        cpu.AddSample(ProfileCategory::Execute);
        FakeClock::Advance(Us(writeUs));
        cpu.AddSample(ProfileCategory::Write);
    }
    bool Near(float a, float b) { return std::abs(a - b) < 0.01f; }
}

TEST_CASE("CpuUse: period unset falls back to measured cycle time", "[cpu_use]")
{
    CpuUseT<FakeClock> cpu; // no SetPeriod()
    cpu.SetStartTime(cpu.Now());
    for (int i = 0; i < 5; ++i)
    {
        RunCycle(cpu, 300, 100, 300, 100); // 800us cycle
    }
    cpu.UpdateCpuUse();
    // (100+300) / 800, overhead 100 / 800.
    CHECK(Near(cpu.GetCpuUse(), 50.0f));
    CHECK(Near(cpu.GetCpuOverhead(), 12.5f));
}

TEST_CASE("CpuUse: ramp-up averages only the cycles seen so far", "[cpu_use]")
{
    CpuUseT<FakeClock> cpu;
    cpu.SetPeriod(std::chrono::duration_cast<FakeClock::duration>(Us(1000)));
    cpu.SetStartTime(cpu.Now());

    RunCycle(cpu, 400, 100, 400, 100);
    cpu.UpdateCpuUse();
    // A single cycle is a full-scale reading, not 1/20th of one.
    CHECK(Near(cpu.GetCpuUse(), 50.0f));
    CHECK(Near(cpu.GetCpuOverhead(), 10.0f));

    RunCycle(cpu, 600, 100, 100, 200);
    cpu.UpdateCpuUse();
    // (500 + 200) / 2000
    CHECK(Near(cpu.GetCpuUse(), 35.0f));
    CHECK(Near(cpu.GetCpuOverhead(), 10.0f));
}

TEST_CASE("CpuUse: window rolls over after 20 cycles", "[cpu_use]")
{
    CpuUseT<FakeClock> cpu;
    cpu.SetPeriod(std::chrono::duration_cast<FakeClock::duration>(Us(1000)));
    cpu.SetStartTime(cpu.Now());
    for (int i = 0; i < 30; ++i)
    {
        RunCycle(cpu, 700, 50, 200, 50); // 25%
    }
    cpu.UpdateCpuUse();
    CHECK(Near(cpu.GetCpuUse(), 25.0f));

    // Heavier load: halfway through the window the average is a mix of both.
    for (int i = 0; i < 10; ++i)
    {
        RunCycle(cpu, 400, 50, 500, 50); // 55%
    }
    cpu.UpdateCpuUse();
    CHECK(Near(cpu.GetCpuUse(), 40.0f));

    // After a full window, the old cycles are gone.
    for (int i = 0; i < 10; ++i)
    {
        RunCycle(cpu, 400, 50, 500, 50);
    }
    cpu.UpdateCpuUse();
    CHECK(Near(cpu.GetCpuUse(), 55.0f));
    CHECK(Near(cpu.GetCpuOverhead(), 5.0f));
}

TEST_CASE("CpuUse: an xrun gap does not skew the load", "[cpu_use]")
{
    CpuUseT<FakeClock> cpu;
    cpu.SetPeriod(std::chrono::duration_cast<FakeClock::duration>(Us(1000)));
    cpu.SetStartTime(cpu.Now());
    for (int i = 0; i < 25; ++i)
    {
        RunCycle(cpu, 500, 50, 200, 250); // 25%
    }
    cpu.UpdateCpuUse();
    CHECK(Near(cpu.GetCpuUse(), 25.0f));

    // Input xrun: the driver abandons the cycle with nothing recorded and spends 20ms recovering.
    FakeClock::Advance(Us(20000));
    cpu.UpdateCpuUse(); // empty cycle: ignored.
    CHECK(Near(cpu.GetCpuUse(), 25.0f));
    CHECK(Near(cpu.GetCpuOverhead(), 5.0f));

    // The recovery time lands in the next Read sample, which isn't load.
    RunCycle(cpu, 500, 50, 200, 250);
    cpu.UpdateCpuUse();
    CHECK(Near(cpu.GetCpuUse(), 25.0f));
    CHECK(Near(cpu.GetCpuOverhead(), 5.0f));
}

TEST_CASE("CpuUse: snapshot carries both values from one cycle", "[cpu_use]")
{
    CpuUseT<FakeClock> cpu;
    cpu.SetPeriod(std::chrono::duration_cast<FakeClock::duration>(Us(1000)));
    cpu.SetStartTime(cpu.Now());
    RunCycle(cpu, 0, 250, 500, 250);
    cpu.UpdateCpuUse();
    auto snapshot = cpu.GetSnapshot();
    CHECK(Near(snapshot.cpuUse, 75.0f));
    CHECK(Near(snapshot.overhead, 25.0f));

    // Overload beyond 100% is reported, not wrapped.
    for (int i = 0; i < 20; ++i)
    {
        RunCycle(cpu, 0, 1000, 2000, 0);
    }
    cpu.UpdateCpuUse();
    snapshot = cpu.GetSnapshot();
    CHECK(Near(snapshot.cpuUse, 300.0f));
    CHECK(Near(snapshot.overhead, 100.0f));
}
