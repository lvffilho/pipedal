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

#include "JackConfiguration.hpp"
#include <functional>
#include "AlsaSequencer.hpp"
#include "MidiEvent.hpp"
#include "DeviceVus.hpp"



namespace pipedal {


    using ProcessCallback = std::function<void (size_t)>;

    class ChannelSelection;

    class AudioDriverHost {
    public:
        virtual void OnProcess(size_t nFrames) = 0;
        virtual bool OnRealtimeUpdateDeviceVus(size_t nFrames) = 0;

        virtual void OnUnderrun() = 0;
        virtual void OnAlsaDriverStopped() = 0;
        virtual void OnAudioTerminated() = 0;

        // While the driver is stopped after an abnormal termination: process pending
        // host->audio commands only (no DSP), so the host's pedalboard bookkeeping and
        // blocking requests keep moving. Called from the driver's (demoted) audio thread.
        virtual void OnProcessCommandsOnly() {}

        // Audio thread, still realtime, while it waits for a device restart (up to ~30 s):
        // process pending host->audio commands only (no DSP), so that UI commands are not
        // held up. Unlike OnProcessCommandsOnly(), requests that need the pedalboard to run
        // stay queued for the next OnProcess() (or are failed if the restart fails).
        // Must be realtime-safe.
        virtual void OnProcessCommandsWhileRestarting() {}

        // Audio thread: ask a non-realtime thread to call AudioDriver::ServiceRestartRequest().
        // Must not block. Returns false if no such thread is available, in which case the
        // driver restarts the device on its own thread.
        virtual bool RequestDriverRestart() { return false; }
    };
    class AudioDriver {
    public:

        virtual ~AudioDriver() { }

        virtual float CpuUse() = 0;
        virtual float CpuOverhead() = 0;

        virtual uint32_t GetSampleRate() = 0;

        virtual size_t GetMidiInputEventCount() = 0;
        virtual MidiEvent*GetMidiEvents() = 0;

        virtual const ChannelSelection &GetChannelSelection() const = 0;


        virtual std::vector<float*> &DeviceInputBuffers() = 0;
        virtual size_t DeviceInputBufferCount() const = 0;
        virtual float* GetDeviceInputBuffer(size_t channel) const = 0;

        virtual std::vector<float*> &DeviceOutputBuffers() = 0;
        virtual size_t DeviceOutputBufferCount() const = 0;
        virtual float* GetDeviceOutputBuffer(size_t channel) const = 0;

        virtual std::vector<float*> &MainInputBuffers() = 0;
        virtual size_t MainInputBufferCount() const = 0;
        virtual float*GetMainInputBuffer(size_t channel) = 0;

        virtual std::vector<float*> &MainOutputBuffers() = 0;
        virtual size_t MainOutputBufferCount() const = 0;
        virtual float*GetMainOutputBuffer(size_t channel) = 0;

        
        virtual std::vector<float*>&AuxInputBuffers() = 0;
        virtual std::vector<float*>&AuxOutputBuffers() = 0;
        virtual size_t AuxInputBufferCount() const = 0;
        
        virtual float*GetAuxInputBuffer(size_t channel) = 0;
        virtual size_t AuxOutputBufferCount() const = 0;
        virtual float*GetAuxOutputBuffer(size_t channel) = 0;


        virtual float*GetZeroInputBuffer() = 0;
        virtual float*GetDiscardOutputBuffer() = 0;

        virtual void Open(const JackServerSettings & jackServerSettings,const ChannelSelection &channelSelection) = 0;
        virtual void SetAlsaSequencer(AlsaSequencer::ptr alsaSequencer) = 0;
        virtual void Activate() = 0;
        virtual void Deactivate() = 0;
        virtual void Close() = 0;

        virtual std::string GetConfigurationDescription() = 0;
        virtual void DumpBufferTrace(size_t nEntries) {}

        // Non-realtime service thread: perform a restart previously requested through
        // AudioDriverHost::RequestDriverRestart(). No-op if none is pending.
        virtual void ServiceRestartRequest() {}

        // Non-realtime service thread: log counters accumulated by the audio thread
        // (xruns, dropped MIDI events) since the previous call.
        // Returns true if anything was logged.
        virtual bool LogRealtimeStatistics() { return false; }

    };

};