/*
 * MIT License
 *
 * Copyright (c) Robin E.R. Davies
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace pipedal
{

    enum ProfileCategory
    {
        Init,
        Read,    // time from the previous sample until PCM read returns (includes blocking wait).
        Driver,  // driver-side processing (format conversion, mixing, VU). Counted once.
        Execute, // DSP (AudioDriverHost::OnProcess).
        Write,   // PCM write (where blocking occurs). Not counted as load.

        MaxCategory
    };
    constexpr size_t NUM_PROFILE_CATEGORIES = (size_t)ProfileCategory::MaxCategory;

    // Audio-thread-only writer; any thread may call GetCpuUse()/GetCpuOverhead()/GetSnapshot()
    // (lock-free). Both values are published together in one atomic word, so a reader never
    // sees the CPU use from one cycle paired with the overhead from another.
    //
    // DSP load = (Driver + Execute) time, i.e. PCM-read-return to write-start, as a percentage
    // of the period duration (nframes / sampleRate). Overhead = Driver time as a percentage of
    // the period. If no period has been set, the measured wall time of the cycles is used.
    //
    // A cycle with no samples at all (e.g. an input xrun that skipped the cycle) is ignored:
    // it neither enters the average window nor changes the published values. The time spent
    // recovering lands in the next Read sample, which is not counted as load.
    template <typename ClockT = std::chrono::steady_clock>
    class CpuUseT
    {
    public:
        using TimeT = typename ClockT::time_point;
        using DurationT = typename ClockT::duration;
        using SampleT = typename DurationT::rep;

    private:
        static constexpr size_t NUM_SAMPLES = 20; // average over N cycles.

        class Averager
        {
            SampleT total = 0;
            SampleT samples[NUM_SAMPLES] = {};
            size_t index = 0;

        public:
            SampleT GetTotal() const { return total; }
            void AddSample(SampleT sample)
            {
                total += sample - samples[index];
                samples[index] = sample;
                if (++index >= NUM_SAMPLES)
                    index = 0;
            }
        };

        TimeT lastSample{};
        DurationT period{}; // zero: unknown
        Averager cycleTimes[NUM_PROFILE_CATEGORIES];
        SampleT pending[NUM_PROFILE_CATEGORIES] = {}; // accumulated for the current cycle.
        size_t cyclesCounted = 0;

        // Two 16.16 fixed-point percentages: CPU use in the high 32 bits, overhead in the low 32.
        static constexpr float FIXED_SCALE = 65536.0f;
        static constexpr float MAX_PERCENT = 65535.0f;
        std::atomic<uint64_t> currentSnapshot{0};
        static_assert(std::atomic<uint64_t>::is_always_lock_free, "CpuUse snapshot must be lock-free on the audio thread");

        static uint32_t ToFixed(float percent)
        {
            if (!(percent > 0.0f)) // also catches NaN.
                return 0;
            if (percent >= MAX_PERCENT)
                percent = MAX_PERCENT;
            return (uint32_t)(percent * FIXED_SCALE + 0.5f);
        }
        static float FromFixed(uint32_t value) { return (float)value / FIXED_SCALE; }

    public:
        struct Snapshot
        {
            float cpuUse = 0;
            float overhead = 0;
        };

        static TimeT Now() { return ClockT::now(); }

        // Duration of one audio period (nframes / sampleRate).
        void SetPeriod(DurationT periodDuration) { period = periodDuration; }

        void SetStartTime(TimeT time) { lastSample = time; }

        void AddSample(ProfileCategory category, TimeT time)
        {
            pending[(size_t)category] += (SampleT)(time - lastSample).count();
            lastSample = time;
        }
        void AddSample(ProfileCategory category) { AddSample(category, Now()); }
        void AddSample(ProfileCategory category, TimeT startTime, TimeT endTime)
        {
            pending[(size_t)category] += (SampleT)(endTime - startTime).count();
        }

        // Call once per audio cycle (e.g. at the top of the loop).
        void UpdateCpuUse()
        {
            bool any = false;
            for (size_t i = 0; i < NUM_PROFILE_CATEGORIES; ++i)
            {
                if (pending[i] != 0)
                {
                    any = true;
                    break;
                }
            }
            if (!any)
                return; // skipped cycle (xrun, or nothing measured yet).
            for (size_t i = 0; i < NUM_PROFILE_CATEGORIES; ++i)
            {
                cycleTimes[i].AddSample(pending[i]);
                pending[i] = 0;
            }
            if (cyclesCounted < NUM_SAMPLES)
                ++cyclesCounted;

            SampleT driver = cycleTimes[ProfileCategory::Driver].GetTotal();
            SampleT processing = cycleTimes[ProfileCategory::Execute].GetTotal() + driver;

            SampleT available;
            if (period.count() > 0)
            {
                available = (SampleT)period.count() * (SampleT)cyclesCounted;
            }
            else
            {
                available = 0;
                for (size_t i = 0; i < NUM_PROFILE_CATEGORIES; ++i)
                    available += cycleTimes[i].GetTotal();
            }
            float result = 0.0f, overhead = 0.0f;
            if (available > 0)
            {
                result = 100.0f * (float)processing / (float)available;
                overhead = 100.0f * (float)driver / (float)available;
            }
            currentSnapshot.store(((uint64_t)ToFixed(result) << 32) | (uint64_t)ToFixed(overhead),
                                  std::memory_order_relaxed);
        }
        // CPU use and overhead from the same cycle.
        Snapshot GetSnapshot() const
        {
            uint64_t value = currentSnapshot.load(std::memory_order_relaxed);
            return Snapshot{FromFixed((uint32_t)(value >> 32)), FromFixed((uint32_t)value)};
        }
        float GetCpuUse() const { return GetSnapshot().cpuUse; }
        float GetCpuOverhead() const { return GetSnapshot().overhead; }
    };

    using CpuUse = CpuUseT<std::chrono::steady_clock>;

}
