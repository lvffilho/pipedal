/*
 * MIT License
 *
 * Copyright (c) Robin E.R. Davies
 * Copyright (c) Roberto Figliè
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

#include "pch.h"
#include "PiPedalCommon.hpp"
#include "util.hpp"
#include <cmath>
#include "Finally.hpp"
#include <bit>
#include <memory>
#include "ss.hpp"
#include "AlsaDriver.hpp"
#include "JackServerSettings.hpp"
#include <thread>
#include "RtInversionGuard.hpp"
#include "PiPedalException.hpp"
#include "DummyAudioDriver.hpp"
#include "SchedulerPriority.hpp"
#include "CrashGuard.hpp"
#include <iostream>
#include <iomanip>
#include "ChannelRouterSettings.hpp"

#include "CpuUse.hpp"
#include "CpuDmaLatency.hpp"
#include "AudioSampleClamp.hpp"
#include "AlsaDriverRealtime.hpp"

#include <alsa/asoundlib.h>
#include <sched.h>

#include "Lv2Log.hpp"
#include <limits>
#include "ss.hpp"

#undef ALSADRIVER_CONFIG_DBG

#ifdef ALSADRIVER_CONFIG_DBG
#include <stdio.h>
#endif

using namespace pipedal;

namespace pipedal
{

#define TRACE_BUFFER_POSITIONS 0
    static bool ShouldForceStereoChannels(snd_pcm_t *pcmHandle, snd_pcm_hw_params_t *hwParams, unsigned int channelsMin, unsigned int channelsMax)
    {
        // The problem: old IC2 drivers seem to return 1-8 channels, but 8 channels is non-functinal. The assumption is that legacy drivers
        // (I2C drivers, particularl, but also the Rpi headphones device, as an interesting example) that don't support channel maps do this.
        // Hypothetically, devices could allow slection of hardware-downmixed surround channels. So deal with this case defensively.
        // The approach: check the channel map and do our best to interpret what we find.
        // No channel map, or any part of the channel map is unknown? Probably the legalcy case we're interested in. Return TRUE
        // If the channel map is a surround format, return true in that case as well.
        // If the channel map is pairwise, return false! (legitimately multi-channel devices should not be forced to stereo).
        // If the channel map is all FL/FR/MONO return false (a hypothetical configuration for a multi-channel device)
        // If the channel map is not all FL/FR/MONO, assume that it's an upmixed/downmixed surround format, and return TRUE.
        // This is high-risk code, because it attempts to anticipate hypothetical device configurations with no actual testing.

        if (channelsMax <= 2)
            return false;
        if (channelsMin == channelsMax)
            return false;
        if (channelsMin > 2)
            return false; // can't imagine what sort of device this is.

        snd_pcm_hw_params_t *test_params;

        snd_pcm_hw_params_alloca(&test_params);
        snd_pcm_hw_params_copy(test_params, hwParams);

        // can we select 2 channels?
        if (snd_pcm_hw_params_set_channels(pcmHandle, test_params, (unsigned int)2) >= 0)
        {
            snd_pcm_chmap_query_t **chmaps = snd_pcm_query_chmaps(pcmHandle);

            if (chmaps == nullptr)
            {
                return true; // probably an old driver. Do it.
            }
            Finally ff([chmaps]()
                       { snd_pcm_free_chmaps(chmaps); });
            for (size_t i = 0; chmaps[i] != nullptr; ++i)
            {
                snd_pcm_chmap_query_t *chmap = chmaps[i];
                if (chmap->map.channels == channelsMax)
                {
                    switch (chmap->type)
                    {
                    case SND_CHMAP_TYPE_NONE:
                    default:
                        return true; // weird legacy case?  Do it.
                    case SND_CHMAP_TYPE_PAIRED:
                        return false; // A legitimate multi-channel device. definitely don't do it.

                    case SND_CHMAP_TYPE_VAR:
                    case SND_CHMAP_TYPE_FIXED:
                    {
                        // we should do it for surround formats. guard against other hypothetical mappings for legitimately multi-channel devices.

                        snd_pcm_chmap_position pos0 = (snd_pcm_chmap_position)(chmap->map.pos[0]);
                        if (pos0 == snd_pcm_chmap_position::SND_CHMAP_MONO) // hypothetical channel map of all mono channesl.
                        {
                            return false; // don't do it.
                        }
                        if (pos0 != snd_pcm_chmap_position::SND_CHMAP_FL && pos0 != snd_pcm_chmap_position::SND_CHMAP_FL) // surround formats always start with FL. Hypothetical quad formats could start with FC.
                        {
                            return false; // don't do it.
                        }
                        // accept a hypothetical channel map of mixed FL's and FR's, FC's and MONOs. (Multi-channel with mixed mono and stereo pairs).
                        // But otherwise assume it's a surround map, and use a stereo channel configuration instead.
                        for (size_t i = 0; i < chmap->map.channels; ++i)
                        {
                            snd_pcm_chmap_position pos = (snd_pcm_chmap_position)(chmap->map.pos[i]);
                            switch (pos)
                            {
                            case snd_pcm_chmap_position::SND_CHMAP_MONO:
                            case snd_pcm_chmap_position::SND_CHMAP_FL:
                            case snd_pcm_chmap_position::SND_CHMAP_FR:
                            case snd_pcm_chmap_position::SND_CHMAP_FC:
                                break; // keep going.
                            default:
                                return true; // probably a surround sound map.
                            }
                        }
                        return false;
                    };
                    }
                }
            }
            return true; // no matching channel map(!??). nonsensical case. may as well use the stereo config, which might be more sensible.
        }
        return false;
    }

    struct AudioFormat
    {
        char name[40];
        snd_pcm_format_t pcm_format;
    };

    bool SetPreferredAlsaFormat(
        const char *streamType,
        snd_pcm_t *handle,
        snd_pcm_hw_params_t *hwParams,
        AudioFormat *formats,
        size_t nItems)
    {
        snd_pcm_hw_params_t *test_params;
        snd_pcm_hw_params_alloca(&test_params);

        for (size_t i = 0; i < nItems; ++i)
        {
            snd_pcm_hw_params_copy(test_params, hwParams);

            int err = snd_pcm_hw_params_set_format(handle, test_params, formats[i].pcm_format);
            if (err == 0)
            {
                int err = snd_pcm_hw_params_set_format(handle, hwParams, formats[i].pcm_format);
                if (err == 0)
                {
                    return true;
                }
            }
        }
        return false;
    }

    static AudioFormat leFormats[]{
        {"32-bit float little-endian", SND_PCM_FORMAT_FLOAT_LE},
        {"32-bit integer little-endian", SND_PCM_FORMAT_S32_LE},
        {"24-bit little-endian", SND_PCM_FORMAT_S24_LE},
        {"24-bit little-endian in 3bytes format", SND_PCM_FORMAT_S24_3LE},
        {"16-bit little-endian", SND_PCM_FORMAT_S16_LE},

    };
    static AudioFormat beFormats[]{
        {"32-bit float big-endian", SND_PCM_FORMAT_FLOAT_BE},
        {"32-bit integer big-endian", SND_PCM_FORMAT_S32_BE},
        {"24-bit big-endian", SND_PCM_FORMAT_S24_BE},
        {"24-bit big-endian in 3bytes format", SND_PCM_FORMAT_S24_3BE},
        {"16-bit big-endian", SND_PCM_FORMAT_S16_BE},
    };
    [[noreturn]] static void AlsaError(const std::string &message)
    {
        throw PiPedalStateException(message);
    }

    std::string GetAlsaFormatDescription(snd_pcm_format_t format)
    {
        for (size_t i = 0; i < sizeof(beFormats) / sizeof(beFormats[0]); ++i)
        {
            if (beFormats[i].pcm_format == format)
            {
                return beFormats[i].name;
            }
        }
        for (size_t i = 0; i < sizeof(leFormats) / sizeof(leFormats[0]); ++i)
        {
            if (leFormats[i].pcm_format == format)
            {
                return leFormats[i].name;
            }
        }
        return "Unknown format.";
    }

    void SetPreferredAlsaFormat(
        const std::string &alsa_device_name,
        const char *streamType,
        snd_pcm_t *handle,
        snd_pcm_hw_params_t *hwParams)
    {
        int err;

        if (std::endian::native == std::endian::big)
        {
            if (SetPreferredAlsaFormat(streamType, handle, hwParams, beFormats, sizeof(beFormats) / sizeof(beFormats[0])))
                return;
            if (SetPreferredAlsaFormat(streamType, handle, hwParams, leFormats, sizeof(leFormats) / sizeof(leFormats[0])))
                return;
        }
        else
        {
            if (SetPreferredAlsaFormat(streamType, handle, hwParams, leFormats, sizeof(leFormats) / sizeof(leFormats[0])))
                return;
            if (SetPreferredAlsaFormat(streamType, handle, hwParams, beFormats, sizeof(beFormats) / sizeof(beFormats[0])))
                return;
        }
        AlsaError(SS("No supported audio formats (" << alsa_device_name << "/" << streamType << ")"));
    }

    class AlsaDriverImpl : public AudioDriver
    {
    private:
        struct BufferTrace
        {
            uint64_t time;
            snd_pcm_sframes_t inAvail;
            snd_pcm_sframes_t outAvail;
            snd_pcm_sframes_t buffered;
            snd_pcm_sframes_t total;
            char code;
        };

        std::vector<BufferTrace> bufferTraces{1000};
        size_t bufferTraceIndex = 0;

        virtual void DumpBufferTrace(size_t nEntries) override;

        inline void TraceBufferPositions(size_t framesInBuffer, char code = ' ')
        {
#if TRACE_BUFFER_POSITIONS
            uint64_t time = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            auto inAvail = snd_pcm_avail_update(this->captureHandle);
            auto outAvail = snd_pcm_avail_update(this->playbackHandle);

            auto total = (inAvail >= 0 ? inAvail : 0) + (outAvail >= 0 ? outAvail : 0) + framesInBuffer;
            bufferTraces[bufferTraceIndex++] = {
                time,
                inAvail,
                outAvail,
                (snd_pcm_sframes_t)framesInBuffer,
                (snd_pcm_sframes_t)total,
                code};
            if (bufferTraceIndex == bufferTraces.size())
            {
                bufferTraceIndex = 0;
            }
#endif
        }

        std::recursive_mutex restartMutex;

        pipedal::CpuUse cpuUse;

#ifdef ALSADRIVER_CONFIG_DBG
        snd_output_t *snd_output = nullptr;
        snd_pcm_status_t *snd_status = nullptr;

#endif
        uint32_t sampleRate = 0;

        uint32_t bufferSize = 0;
        uint32_t numberOfBuffers = 0;

        unsigned int capturePeriods = 0;
        unsigned int playbackPeriods = 0;

        uint32_t captureHardwarePeriodSize = 0;
        uint32_t playbackHardwarePeriodSize = 0;
        ;

        int playbackChannels = 0;
        int captureChannels = 0;

        uint32_t user_threshold = 0;
        bool soft_mode = false;

        snd_pcm_format_t captureFormat = snd_pcm_format_t::SND_PCM_FORMAT_UNKNOWN;

        uint32_t playbackSampleSize = 0;
        uint32_t captureSampleSize = 0;
        uint32_t playbackFrameSize = 0;
        uint32_t captureFrameSize = 0;

        using CopyFunction = void (AlsaDriverImpl::*)(size_t frames);

        CopyFunction copyInputFn;
        CopyFunction copyOutputFn;

        bool inputSwapped = false;
        bool outputSwapped = false;

        std::vector<std::vector<float>> allocatedBuffers;

        std::vector<float *> deviceCaptureBuffers;
        std::vector<float *> devicePlaybackBuffers;
        float *zeroInputBuffer = nullptr;
        float *discardOutputBuffer = nullptr;
        std::vector<float *> mainCaptureBuffers;
        std::vector<float *> mainPlaybackBuffers;
        std::vector<float *> mainVuPlaybackBuffers;

        std::vector<float *> auxCaptureBuffers;
        std::vector<float *> auxPlaybackBuffers;
        std::vector<float *> auxVuPlaybackBuffers;


        std::vector<uint8_t> rawCaptureBuffer;
        std::vector<uint8_t> rawPlaybackBuffer;

        AudioDriverHost *driverHost = nullptr;

        void validate_capture_handle()
        { // leftover debugging for a buffer overrun :-/
#ifdef DEBUG
            auto pcmType = snd_pcm_type(captureHandle);

            if (pcmType != SND_PCM_TYPE_HW && pcmType != SND_PCM_TYPE_NULL)
            {
                throw std::runtime_error("Capture handle has been overwritten");
            }
#endif
        }

    public:
        AlsaDriverImpl(AudioDriverHost *driverHost)
            : driverHost(driverHost)
        {
        }
        virtual ~AlsaDriverImpl()
        {
            Close();
#ifdef ALSADRIVER_CONFIG_DBG
            if (snd_output)
            {
                snd_output_close(snd_output);
                snd_output = nullptr;
            }
            if (snd_status)
            {
                snd_pcm_status_free(snd_status);
                snd_status = nullptr;
            }
#endif
        }

    private:
        void OnShutdown()
        {
            Lv2Log::info("ALSA Audio Server has shut down.");
        }

        static void
        jack_shutdown_fn(void *arg)
        {
            ((AlsaDriverImpl *)arg)->OnShutdown();
        }

        static int xrun_callback_fn(void *arg)
        {
            ((AudioDriverHost *)arg)->OnUnderrun();
            return 0;
        }

        virtual uint32_t GetSampleRate()
        {
            return this->sampleRate;
        }

        JackServerSettings jackServerSettings;

        std::string alsa_device_name;

        snd_pcm_t *playbackHandle = nullptr;
        snd_pcm_t *captureHandle = nullptr;

        snd_pcm_hw_params_t *captureHwParams = nullptr;
        snd_pcm_sw_params_t *captureSwParams = nullptr;
        snd_pcm_hw_params_t *playbackHwParams = nullptr;
        snd_pcm_sw_params_t *playbackSwParams = nullptr;

        bool capture_and_playback_not_synced = false;

        std::mutex terminateSync;

        std::atomic<bool> terminateAudio_ = false;

        void terminateAudio(bool terminate)
        {
            this->terminateAudio_ = terminate;
        }

        bool terminateAudio()
        {
            return this->terminateAudio_;
        }

    private:
        void AlsaCloseAudio()
        {
            std::lock_guard lock{restartMutex};

            if (captureHandle)
            {
                Lv2Log::debug("ALSA capture handle closed.");
                snd_pcm_drain(captureHandle);
                snd_pcm_close(captureHandle);
                captureHandle = nullptr;
            }
            if (playbackHandle)
            {
                Lv2Log::debug("ALSA playback handle closed.");
                snd_pcm_drain(playbackHandle);
                snd_pcm_close(playbackHandle);
                playbackHandle = nullptr;
            }
            if (captureHwParams)
            {
                snd_pcm_hw_params_free(captureHwParams);
                captureHwParams = nullptr;
            }
            if (captureSwParams)
            {
                snd_pcm_sw_params_free(captureSwParams);
                captureSwParams = nullptr;
            }
            if (playbackHwParams)
            {
                snd_pcm_hw_params_free(playbackHwParams);
                playbackHwParams = nullptr;
            }
            if (playbackSwParams)
            {
                snd_pcm_sw_params_free(playbackSwParams);
                playbackSwParams = nullptr;
            }
        }
        void AlsaCleanup()
        {
            AlsaCloseAudio();
        }

        std::string discover_alsa_using_apps()
        {
            return ""; // xxx fix me.
        }

        void AlsaConfigureStream(
            const std::string &alsa_device_name,
            const char *streamType,
            snd_pcm_t *handle,
            snd_pcm_hw_params_t *hwParams,
            snd_pcm_sw_params_t *swParams,
            int *channels,
            unsigned int *periods,
            unsigned int *hwPeriodSize)
        {
            int err;
            snd_pcm_uframes_t stop_th;

            bool isCaptureStream = strcmp(streamType, "capture") == 0;

            if ((err = snd_pcm_hw_params_any(handle, hwParams)) < 0)
            {
                AlsaError(SS("No playback configurations available (" << snd_strerror(err) << ")"));
            }

            err = snd_pcm_hw_params_set_access(handle, hwParams, SND_PCM_ACCESS_RW_INTERLEAVED);
            if (err < 0)
            {
                AlsaError("snd_pcm_hw_params_set_access failed.");
            }

            SetPreferredAlsaFormat(alsa_device_name, streamType, handle, hwParams);

            unsigned int sampleRate = (unsigned int)this->sampleRate;
            err = snd_pcm_hw_params_set_rate_near(handle, hwParams,
                                                  &sampleRate, NULL);
            this->sampleRate = sampleRate;
            if (err < 0)
            {
                AlsaError(SS("Can't set sample rate to " << this->sampleRate << " (" << alsa_device_name << "/" << streamType << ")"));
            }
            if (!*channels)
            {
                /*if not user-specified, try to find the maximum
                 * number of channels */
                unsigned int channels_max = 0;
                unsigned int channels_min = 0;
                err = snd_pcm_hw_params_get_channels_max(hwParams,
                                                         &channels_max);
                if (err < 0)
                {
                    AlsaError(SS("Can't get channels_max."));
                }

                err = snd_pcm_hw_params_get_channels_min(hwParams,
                                                         &channels_min);
                if (err < 0)
                {
                    AlsaError(SS("Can't get channels_min."));
                }

                *channels = channels_max;

                if (ShouldForceStereoChannels(handle, hwParams, channels_min, channels_max))
                {
                    *channels = 2;
                }

                if (*channels >= 1024)
                {
                    // The default PCM device has unlimited channels.
                    // report 2 channels
                    *channels = 2;
                }
            }

            if ((err = snd_pcm_hw_params_set_channels(handle, hwParams,
                                                      *channels)) < 0)
            {
                AlsaError(SS("Can't set channel count to " << *channels << " (" << alsa_device_name << "/" << streamType << ")"));
            }

            snd_pcm_uframes_t effectivePeriodSize = this->bufferSize;

            int dir = 0;
            if ((err = snd_pcm_hw_params_set_period_size_near(handle, hwParams,
                                                              &effectivePeriodSize,
                                                              &dir)) < 0)
            {
                AlsaError(SS("Can't set period size to " << this->bufferSize << " (" << alsa_device_name << "/" << streamType << ")"));
            }
            *hwPeriodSize = effectivePeriodSize;

            *periods = this->numberOfBuffers;
            dir = 0;
            snd_pcm_hw_params_set_periods_min(handle, hwParams, periods, &dir);
            if (*periods < this->numberOfBuffers)
                *periods = this->numberOfBuffers;
            if (snd_pcm_hw_params_set_periods_near(handle, hwParams,
                                                   periods, NULL) < 0)
            {
                AlsaError(SS("Can't set number of periods to " << (*periods) << " (" << alsa_device_name << "/" << streamType << ")"));
            }

            if (*periods < this->numberOfBuffers)
            {
                AlsaError(SS("Got smaller periods " << *periods << " than " << this->numberOfBuffers));
            }

            snd_pcm_uframes_t bSize;

            // if ((err = snd_pcm_hw_params_set_buffer_size(handle, hwParams,
            //                                              *periods *
            //                                                  this->bufferSize)) < 0)
            // {
            //     AlsaError(SS("Can't set buffer length to " << (*periods * this->bufferSize)));
            // }

            if ((err = snd_pcm_hw_params(handle, hwParams)) < 0)
            {
                AlsaError(SS("Cannot set hardware parameters for " << alsa_device_name));
            }

            snd_pcm_sw_params_current(handle, swParams);

            if (isCaptureStream)
            {
                if ((err = snd_pcm_sw_params_set_start_threshold(handle, swParams,
                                                                 0)) < 0)
                {
                    AlsaError(SS("Cannot set start mode for " << alsa_device_name));
                }
            }
            else
            {
                if ((err = snd_pcm_sw_params_set_start_threshold(handle, swParams,
                                                                 0x7fffffff)) < 0)
                {
                    AlsaError(SS("Cannot set start mode for " << alsa_device_name));
                }
            }

            stop_th = *periods * *hwPeriodSize;
            if (this->soft_mode)
            {
                stop_th = (snd_pcm_uframes_t)-1;
            }

            if ((err = snd_pcm_sw_params_set_stop_threshold(
                     handle, swParams, stop_th)) < 0)
            {
                AlsaError(SS("ALSA: cannot set stop mode for " << alsa_device_name));
            }

            if ((err = snd_pcm_sw_params_set_silence_threshold(
                     handle, swParams, 0)) < 0)
            {
                AlsaError(SS("Cannot set silence threshold for " << alsa_device_name));
            }

            if (!isCaptureStream)
            {
                // For playback, set avail_min to one buffer size to minimize latency
                // while ensuring we have enough buffered data to prevent underruns
                snd_pcm_uframes_t playback_avail_min = this->bufferSize;
                err = snd_pcm_sw_params_set_avail_min(
                    handle, swParams, playback_avail_min);
            }
            else
            {
                err = snd_pcm_sw_params_set_avail_min(
                    handle, swParams, this->bufferSize);
            }

            if (err < 0)
            {
                AlsaError(SS("Cannot set avail min for " << alsa_device_name));
            }

            // err = snd_pcm_sw_params_set_tstamp_mode(handle, swParams, SND_PCM_TSTAMP_ENABLE);
            // if (err < 0)
            // {
            //     Lv2Log::info(SS(
            //         "Could not enable ALSA time stamp mode for " << alsa_device_name << " (err " << err << ")"));
            // }

#if SND_LIB_MAJOR >= 1 && SND_LIB_MINOR >= 1
            err = snd_pcm_sw_params_set_tstamp_type(handle, swParams, SND_PCM_TSTAMP_TYPE_MONOTONIC);
            if (err < 0)
            {
                Lv2Log::info(SS(
                    "Could not use monotonic ALSA time stamps for " << alsa_device_name << "(err " << err << ")"));
            }
#endif

            if ((err = snd_pcm_sw_params(handle, swParams)) < 0)
            {
                AlsaError(SS("Cannot set software parameters for " << alsa_device_name));
            }
            err = snd_pcm_prepare(handle);
            if (err < 0)
            {
                AlsaError(SS("ALSA prepare failed. " << snd_strerror(err)));
            }
        }
        void SetAlsaParameters(uint32_t bufferSize, uint32_t numberOfBuffers, uint32_t sampleRate)
        {
            this->bufferSize = bufferSize;
            this->numberOfBuffers = numberOfBuffers;
            this->sampleRate = sampleRate;

            if (this->captureHandle)
            {
                this->alsa_device_name = this->jackServerSettings.GetAlsaInputDevice();
                AlsaConfigureStream(
                    this->alsa_device_name,
                    "capture",
                    captureHandle,
                    captureHwParams,
                    captureSwParams,
                    &captureChannels,
                    &this->capturePeriods,
                    &this->captureHardwarePeriodSize);
            }
            if (this->playbackHandle)
            {
                this->alsa_device_name = this->jackServerSettings.GetAlsaOutputDevice();
                AlsaConfigureStream(
                    this->alsa_device_name,
                    "playback",
                    playbackHandle,
                    playbackHwParams,
                    playbackSwParams,
                    &playbackChannels,
                    &this->playbackPeriods,
                    &this->playbackHardwarePeriodSize);
            }

#ifdef ALSADRIVER_CONFIG_DBG
            snd_pcm_dump(captureHandle, snd_output);
            snd_pcm_dump(playbackHandle, snd_output);
#endif
        }

        int32_t EndianSwap(int32_t v)
        {
            int32_t b0 = v & 0xFF;
            int32_t b1 = (v >> 8) & 0xFF;
            int32_t b2 = (v >> 16) & 0xFF;
            int32_t b3 = (v >> 24) & 0xFF;

            return (b0 << 24) | (b1 << 16) | (b2 << 8) | (b3);
        }
        int16_t EndianSwap(int16_t v)
        {
            int16_t b0 = v & 0xFF;
            int16_t b1 = (v >> 8) & 0xFF;

            return (b0 << 8) | (b1);
        }
        void EndianSwap(float *p, float v_)
        {
            int32_t v = EndianSwap(*(int32_t *)&v_);
            *(int32_t *)p = v;
        }
        template <typename T>
        static T *getCaptureBuffer(std::vector<uint8_t> &buffer) { return (T *)(buffer.data()); }

        void CopyCaptureFloatBe(size_t frames)
        {
            int32_t *p = getCaptureBuffer<int32_t>(rawCaptureBuffer);

            std::vector<float *> &buffers = this->deviceCaptureBuffers;
            int channels = this->captureChannels;
            for (size_t frame = 0; frame < frames; ++frame)
            {
                for (int channel = 0; channel < channels; ++channel)
                {
                    int32_t v = EndianSwap(*p);
                    ++p;

                    *(int32_t *)(buffers[channel] + frame) = v;
                }
            }
        }

        void CopyCaptureFloatLe(size_t frames)
        {
            float *p = getCaptureBuffer<float>(rawCaptureBuffer);

            std::vector<float *> &buffers = this->deviceCaptureBuffers;
            int channels = this->captureChannels;
            for (size_t frame = 0; frame < frames; ++frame)
            {
                for (int channel = 0; channel < channels; ++channel)
                {
                    float v = *p++;
                    buffers[channel][frame] = v;
                }
            }
        }

        void CopyCaptureS16Le(size_t frames)
        {
            int16_t *p = getCaptureBuffer<int16_t>(rawCaptureBuffer);

            std::vector<float *> &buffers = this->deviceCaptureBuffers;
            int channels = this->captureChannels;
            constexpr double scale = 1.0f / (std::numeric_limits<int16_t>::max() + 1L);
            for (size_t frame = 0; frame < frames; ++frame)
            {
                for (int channel = 0; channel < channels; ++channel)
                {
                    int16_t v = *p++;
                    buffers[channel][frame] = scale * v;
                }
            }
        }
        void CopyCaptureS16Be(size_t frames)
        {
            int16_t *p = getCaptureBuffer<int16_t>(rawCaptureBuffer);

            std::vector<float *> &buffers = this->deviceCaptureBuffers;
            int channels = this->captureChannels;
            constexpr float scale = 1.0f / (std::numeric_limits<int16_t>::max() + 1L);
            for (size_t frame = 0; frame < frames; ++frame)
            {
                for (int channel = 0; channel < channels; ++channel)
                {
                    int16_t v = EndianSwap(*p++);
                    buffers[channel][frame] = scale * v;
                }
            }
        }

        void CopyCaptureS32Le(size_t frames)
        {
            int32_t *p = getCaptureBuffer<int32_t>(rawCaptureBuffer);

            std::vector<float *> &buffers = this->deviceCaptureBuffers;
            int channels = this->captureChannels;
            constexpr float scale = 1.0f / (std::numeric_limits<int32_t>::max() + 1L);
            for (size_t frame = 0; frame < frames; ++frame)
            {
                for (int channel = 0; channel < channels; ++channel)
                {
                    int32_t v = *p++;
                    buffers[channel][frame] = scale * v;
                }
            }
        }
        void CopyCaptureS24_3Le(size_t frames)
        {
            uint8_t *p = getCaptureBuffer<uint8_t>(rawCaptureBuffer);

            std::vector<float *> &buffers = this->deviceCaptureBuffers;
            int channels = this->captureChannels;
            constexpr float scale = 1.0f / (std::numeric_limits<int32_t>::max() + 1LL);
            for (size_t frame = 0; frame < frames; ++frame)
            {
                for (int channel = 0; channel < channels; ++channel)
                {
                    int32_t v = (p[0] << 8) + (p[1] << 16) | (p[2] << 24);
                    p += 3;
                    buffers[channel][frame] = scale * v;
                }
            }
        }
        void CopyCaptureS24_3Be(size_t frames)
        {
            uint8_t *p = (uint8_t *)rawCaptureBuffer.data();

            std::vector<float *> &buffers = this->deviceCaptureBuffers;
            int channels = this->captureChannels;
            constexpr float scale = 1.0f / (std::numeric_limits<int32_t>::max() + 1LL);
            for (size_t frame = 0; frame < frames; ++frame)
            {
                for (int channel = 0; channel < channels; ++channel)
                {
                    int32_t v = (p[2] << 8) + (p[1] << 16) | (p[0] << 24);
                    p += 3;
                    buffers[channel][frame] = scale * v;
                }
            }
        }
        void CopyCaptureS24Le(size_t frames)
        {
            int32_t *p = (int32_t *)rawCaptureBuffer.data();

            std::vector<float *> &buffers = this->deviceCaptureBuffers;
            int channels = this->captureChannels;
            // Signed 24-bit full-scale is 0x7FFFFF (2^23-1), not 0x00FFFFFF.
            // Using 2^24 here decoded full-scale input to only 0.5, i.e. 6 dB
            // low, matching S16/S32/S24_3 (which all map full-scale to 1.0).
            constexpr float scale = 1.0f / (0x7FFFFFL + 1L);
            for (size_t frame = 0; frame < frames; ++frame)
            {
                for (int channel = 0; channel < channels; ++channel)
                {
                    int32_t v = *p++;
                    buffers[channel][frame] = scale * v;
                }
            }
        }
        void CopyCaptureS24Be(size_t frames)
        {
            int32_t *p = (int32_t *)rawCaptureBuffer.data();

            std::vector<float *> &buffers = this->deviceCaptureBuffers;
            int channels = this->captureChannels;
            // Signed 24-bit full-scale is 0x7FFFFF (2^23-1), not 0x00FFFFFF.
            // Using 2^24 here decoded full-scale input to only 0.5, i.e. 6 dB
            // low, matching S16/S32/S24_3 (which all map full-scale to 1.0).
            constexpr float scale = 1.0f / (0x7FFFFFL + 1L);
            for (size_t frame = 0; frame < frames; ++frame)
            {
                for (int channel = 0; channel < channels; ++channel)
                {
                    int32_t v = EndianSwap(*p++);
                    buffers[channel][frame] = scale * v;
                }
            }
        }
        void CopyCaptureS32Be(size_t frames)
        {
            int32_t *p = (int32_t *)rawCaptureBuffer.data();

            std::vector<float *> &buffers = this->deviceCaptureBuffers;
            int channels = this->captureChannels;
            constexpr float scale = 1.0f / (std::numeric_limits<int32_t>::max() + 1L);
            for (size_t frame = 0; frame < frames; ++frame)
            {
                for (int channel = 0; channel < channels; ++channel)
                {
                    int32_t v = EndianSwap(*p++);
                    buffers[channel][frame] = scale * v;
                }
            }
        }
        void CopyPlaybackS16Le(size_t frames)
        {
            int16_t *p = (int16_t *)rawPlaybackBuffer.data();

            std::vector<float *> &buffers = this->devicePlaybackBuffers;
            int channels = this->playbackChannels;
            constexpr float scale = std::numeric_limits<int16_t>::max();
            for (size_t frame = 0; frame < frames; ++frame)
            {
                for (int channel = 0; channel < channels; ++channel)
                {
                    float v = buffers[channel][frame];
                    v = ClampOutputSample(v);
                    *p++ = (int16_t)(scale * v);
                }
            }
        }
        void CopyPlaybackS16Be(size_t frames)
        {
            int16_t *p = (int16_t *)rawPlaybackBuffer.data();

            std::vector<float *> &buffers = this->devicePlaybackBuffers;
            int channels = this->playbackChannels;
            constexpr float scale = std::numeric_limits<int16_t>::max();
            for (size_t frame = 0; frame < frames; ++frame)
            {
                for (int channel = 0; channel < channels; ++channel)
                {
                    float v = buffers[channel][frame];
                    v = ClampOutputSample(v);
                    *p++ = EndianSwap((int16_t)(scale * v));
                }
            }
        }
        void CopyPlaybackS32Le(size_t frames)
        {
            int32_t *p = (int32_t *)rawPlaybackBuffer.data();

            std::vector<float *> &buffers = this->devicePlaybackBuffers;
            int channels = this->playbackChannels;
            constexpr double scale = std::numeric_limits<int32_t>::max();
            for (size_t frame = 0; frame < frames; ++frame)
            {
                for (int channel = 0; channel < channels; ++channel)
                {
                    float v = buffers[channel][frame];
                    v = ClampOutputSample(v);
                    *p++ = (int32_t)(scale * v);
                }
            }
        }
        void CopyPlaybackS24Le(size_t frames)
        {
            // 24 bits in low bits of an int32_t.

            int32_t *p = (int32_t *)rawPlaybackBuffer.data();

            std::vector<float *> &buffers = this->devicePlaybackBuffers;
            int channels = this->playbackChannels;
            // Signed 24-bit full-scale is 0x7FFFFF (2^23-1). The old value of
            // 0x00FFFFFF mapped any float above 0.5 (-6 dBFS) to 0x800000.., i.e.
            // past the signed-24-bit range, so the low 24 bits wrapped to the
            // opposite polarity -> harsh high-frequency distortion proportional
            // to signal level (no xruns, normal CPU). The clamp above now limits
            // correctly to full-scale.
            constexpr double scale = 0x7FFFFF;
            for (size_t frame = 0; frame < frames; ++frame)
            {
                for (int channel = 0; channel < channels; ++channel)
                {
                    float v = buffers[channel][frame];
                    v = ClampOutputSample(v);
                    *p++ = (int32_t)(scale * v);
                }
            }
        }
        void CopyPlaybackS24Be(size_t frames)
        {
            // 24 bits in low bits of an int32_t.

            int32_t *p = (int32_t *)rawPlaybackBuffer.data();

            std::vector<float *> &buffers = this->devicePlaybackBuffers;
            int channels = this->playbackChannels;
            // Signed 24-bit full-scale is 0x7FFFFF (2^23-1). The old value of
            // 0x00FFFFFF mapped any float above 0.5 (-6 dBFS) to 0x800000.., i.e.
            // past the signed-24-bit range, so the low 24 bits wrapped to the
            // opposite polarity -> harsh high-frequency distortion proportional
            // to signal level (no xruns, normal CPU). The clamp above now limits
            // correctly to full-scale.
            constexpr double scale = 0x7FFFFF;
            for (size_t frame = 0; frame < frames; ++frame)
            {
                for (int channel = 0; channel < channels; ++channel)
                {
                    float v = buffers[channel][frame];
                    v = ClampOutputSample(v);
                    *p++ = EndianSwap((int32_t)(scale * v));
                }
            }
        }
        void CopyPlaybackS32Be(size_t frames)
        {
            int32_t *p = (int32_t *)rawPlaybackBuffer.data();

            std::vector<float *> &buffers = this->devicePlaybackBuffers;
            int channels = this->playbackChannels;
            constexpr double scale = std::numeric_limits<int32_t>::max();
            for (size_t frame = 0; frame < frames; ++frame)
            {
                for (int channel = 0; channel < channels; ++channel)
                {
                    float v = buffers[channel][frame];
                    v = ClampOutputSample(v);
                    *p++ = EndianSwap((int32_t)(scale * v));
                }
            }
        }
        void CopyPlaybackS24_3Be(size_t frames)
        {
            uint8_t *p = (uint8_t *)rawPlaybackBuffer.data();

            std::vector<float *> &buffers = this->devicePlaybackBuffers;
            int channels = this->playbackChannels;
            constexpr double scale = std::numeric_limits<int32_t>::max();
            for (size_t frame = 0; frame < frames; ++frame)
            {
                for (int channel = 0; channel < channels; ++channel)
                {
                    float v = buffers[channel][frame];
                    v = ClampOutputSample(v);
                    int32_t iValue = (int32_t)(scale * v);
                    p[0] = (uint8_t)(iValue >> 24);
                    p[1] = (uint8_t)(iValue >> 16);
                    p[2] = (uint8_t)(iValue >> 8);

                    p += 3;
                }
            }
        }
        void CopyPlaybackS24_3Le(size_t frames)
        {
            uint8_t *p = (uint8_t *)rawPlaybackBuffer.data();

            std::vector<float *> &buffers = this->devicePlaybackBuffers;
            int channels = this->playbackChannels;
            constexpr double scale = std::numeric_limits<int32_t>::max();
            for (size_t frame = 0; frame < frames; ++frame)
            {
                for (int channel = 0; channel < channels; ++channel)
                {
                    float v = buffers[channel][frame];
                    v = ClampOutputSample(v);
                    int32_t iValue = (int32_t)(scale * v);
                    p[0] = (uint8_t)(iValue >> 8);
                    p[1] = (uint8_t)(iValue >> 16);
                    p[2] = (uint8_t)(iValue >> 24);

                    p += 3;
                }
            }
        }

        void CopyPlaybackFloatLe(size_t frames)
        {
            float *p = (float *)rawPlaybackBuffer.data();

            std::vector<float *> &buffers = this->devicePlaybackBuffers;
            int channels = this->playbackChannels;
            for (size_t frame = 0; frame < frames; ++frame)
            {
                for (int channel = 0; channel < channels; ++channel)
                {
                    float v = SanitizeFloatOutputSample(buffers[channel][frame]);
                    *p++ = v;
                }
            }
        }
        void CopyPlaybackFloatBe(size_t frames)
        {
            float *p = (float *)rawPlaybackBuffer.data();

            std::vector<float *> &buffers = this->devicePlaybackBuffers;
            int channels = this->playbackChannels;
            for (size_t frame = 0; frame < frames; ++frame)
            {
                for (int channel = 0; channel < channels; ++channel)
                {
                    float v = SanitizeFloatOutputSample(buffers[channel][frame]);
                    EndianSwap(p, v);
                    p++;
                }
            }
        }

        virtual void Open(const JackServerSettings &jackServerSettings, const ChannelSelection &channelSelection) override
        {
            this->isDummyDriver = jackServerSettings.IsDummyAudioDevice();
            terminateAudio_ = false;
            if (open)
            {
                throw PiPedalStateException("Already open.");
            }
            this->jackServerSettings = jackServerSettings;
            this->channelSelection = channelSelection;

            open = true;
            try
            {
                OpenAudio(jackServerSettings, channelSelection);
                std::atomic_thread_fence(std::memory_order::release);
            }
            catch (const std::exception &e)
            {
                std::atomic_thread_fence(std::memory_order::release);

                Close();
                throw;
            }
        }

    public:
        void TestFormatEncodeDecode(snd_pcm_format_t captureFormat);

    private:
        void AllocateBuffers(std::vector<float *> &buffers, size_t n)
        {
            buffers.resize(n);
            for (size_t i = 0; i < n; ++i)
            {
                buffers[i] = new float[this->bufferSize];
                for (size_t j = 0; j < this->bufferSize; ++j)
                {
                    buffers[i][j] = 0;
                }
            }
        }

        ChannelSelection channelSelection;

        bool open = false;

        void RestartAlsa()
        {
            std::lock_guard lock{restartMutex};
            Lv2Log::debug("Restarting ALSA devices.");

            // Failures are not logged here: the exception carries the details, and the
            // caller logs it once (ServiceRestartRequest retries, so it logs per attempt).
            try
            {
                AlsaCloseAudio();
            }
            catch (const std::exception &e)
            {
                throw std::runtime_error(SS("Unable to restart the audio stream. Error cleaning up ALSA: " << e.what()));
            }
            try
            {
                OpenAudio(this->jackServerSettings, this->channelSelection);
                validate_capture_handle();
                FillOutputBuffer();
            }
            catch (const std::exception &e)
            {
                throw std::runtime_error(SS("Unable to restart the audio stream. Error opening ALSA: " << e.what()));
            }
            int err;

            if ((err = snd_pcm_start(captureHandle)) < 0)
            {
                throw PiPedalStateException(SS("Unable to restart ALSA capture: " << snd_strerror(err)));
            }
            TraceBufferPositions(0, '+');
            audioRunning = true;
        }

        void PrepareCaptureFunctions(snd_pcm_format_t captureFormat)
        {
            this->captureFormat = captureFormat;

            switch (captureFormat)
            {
            case SND_PCM_FORMAT_FLOAT_LE:
                captureSampleSize = 4;
                copyInputFn = &AlsaDriverImpl::CopyCaptureFloatLe;
                break;
            case SND_PCM_FORMAT_S24_3LE:
                copyInputFn = &AlsaDriverImpl::CopyCaptureS24_3Le;
                captureSampleSize = 3;
                break;
            case SND_PCM_FORMAT_S32_LE:
                captureSampleSize = 4;
                copyInputFn = &AlsaDriverImpl::CopyCaptureS32Le;
                break;
            case SND_PCM_FORMAT_S24_LE:
                captureSampleSize = 4;
                copyInputFn = &AlsaDriverImpl::CopyCaptureS24Le;
                break;
            case SND_PCM_FORMAT_S16_LE:
                captureSampleSize = 2;
                copyInputFn = &AlsaDriverImpl::CopyCaptureS16Le;
                break;
            case SND_PCM_FORMAT_FLOAT_BE:
                captureSampleSize = 4;
                copyInputFn = &AlsaDriverImpl::CopyCaptureFloatBe;
                captureSampleSize = 4;
                break;
            case SND_PCM_FORMAT_S24_3BE:
                captureSampleSize = 3;
                copyInputFn = &AlsaDriverImpl::CopyCaptureS24_3Be;
                break;
            case SND_PCM_FORMAT_S32_BE:
                copyInputFn = &AlsaDriverImpl::CopyCaptureS32Be;
                captureSampleSize = 4;
                break;
            case SND_PCM_FORMAT_S24_BE:
                copyInputFn = &AlsaDriverImpl::CopyCaptureS24Be;
                captureSampleSize = 4;
                break;
            case SND_PCM_FORMAT_S16_BE:
                copyInputFn = &AlsaDriverImpl::CopyCaptureS16Be;
                captureSampleSize = 2;
                break;
            default:
                break;
            }
            if (copyInputFn == nullptr)
            {
                throw PiPedalStateException(SS("Audio input format not supported. (" << captureFormat << ")"));
            }

            captureFrameSize = captureSampleSize * captureChannels;
            rawCaptureBuffer.resize(captureFrameSize * bufferSize * 2);
            memset(rawCaptureBuffer.data(), 0, rawCaptureBuffer.size());

            AllocateBuffers(deviceCaptureBuffers, captureChannels);
        }

        virtual std::string GetConfigurationDescription()
        {
            std::string result = SS(
                "ALSA, "
                << this->alsa_device_name
                << ", " << GetAlsaFormatDescription(this->captureFormat)
                << ", " << this->sampleRate
                << ", " << this->bufferSize << "x" << this->numberOfBuffers
                << ", " << "device: " << this->DeviceInputBufferCount() << "/" << this->DeviceOutputBufferCount()
                << ", main: " << this->MainInputBufferCount() << "/" << this->MainOutputBufferCount()
                << ", aux: " << this->AuxInputBufferCount() << "/" << this->AuxOutputBufferCount()
            );
            return result;
        }
        void PreparePlaybackFunctions(snd_pcm_format_t playbackFormat)
        {
            copyOutputFn = nullptr;
            switch (playbackFormat)
            {
            case SND_PCM_FORMAT_FLOAT_LE:
                playbackSampleSize = 4;
                copyOutputFn = &AlsaDriverImpl::CopyPlaybackFloatLe;
                break;
            case SND_PCM_FORMAT_S24_3LE:
                copyOutputFn = &AlsaDriverImpl::CopyPlaybackS24_3Le;
                playbackSampleSize = 3;
                break;
            case SND_PCM_FORMAT_S32_LE:
                copyOutputFn = &AlsaDriverImpl::CopyPlaybackS32Le;
                playbackSampleSize = 4;
                break;
            case SND_PCM_FORMAT_S24_LE:
                copyOutputFn = &AlsaDriverImpl::CopyPlaybackS24Le;
                playbackSampleSize = 4;
                break;
            case SND_PCM_FORMAT_S16_LE:
                copyOutputFn = &AlsaDriverImpl::CopyPlaybackS16Le;
                playbackSampleSize = 2;
                break;
            case SND_PCM_FORMAT_FLOAT_BE:
                copyOutputFn = &AlsaDriverImpl::CopyPlaybackFloatBe;
                playbackSampleSize = 4;
                break;
            case SND_PCM_FORMAT_S24_3BE:
                copyOutputFn = &AlsaDriverImpl::CopyPlaybackS24_3Be;
                playbackSampleSize = 3;
                break;
            case SND_PCM_FORMAT_S32_BE:
                copyOutputFn = &AlsaDriverImpl::CopyPlaybackS32Be;
                playbackSampleSize = 4;
                break;
            case SND_PCM_FORMAT_S24_BE:
                copyOutputFn = &AlsaDriverImpl::CopyPlaybackS24Be;
                playbackSampleSize = 4;
                break;
            case SND_PCM_FORMAT_S16_BE:
                copyOutputFn = &AlsaDriverImpl::CopyPlaybackS16Be;
                playbackSampleSize = 2;
                break;
            default:
                break;
            }
            if (copyOutputFn == nullptr)
            {
                throw PiPedalStateException(SS("Unsupported audio output format. (" << playbackFormat << ")"));
            }

            playbackFrameSize = playbackSampleSize * playbackChannels;
            rawPlaybackBuffer.resize(playbackFrameSize * bufferSize);
            memset(rawPlaybackBuffer.data(), 0, playbackFrameSize * bufferSize);

            AllocateBuffers(devicePlaybackBuffers, playbackChannels);
        }

        void OpenAudio(const JackServerSettings &jackServerSettings, const ChannelSelection &channelSelection)
        {
            std::lock_guard lock{restartMutex};

            int err;

            std::string inputName = jackServerSettings.GetAlsaInputDevice();
            std::string outputName = jackServerSettings.GetAlsaOutputDevice();

            this->numberOfBuffers = jackServerSettings.GetNumberOfBuffers();
            this->bufferSize = jackServerSettings.GetBufferSize();
            this->user_threshold = jackServerSettings.GetBufferSize();

            try
            {

                this->alsa_device_name = outputName;
                err = snd_pcm_open(&playbackHandle, outputName.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
                if (err < 0)
                {
                    switch (errno)
                    {
                    case EBUSY:
                    {
                        std::string apps = discover_alsa_using_apps();
                        std::string message;
                        if (apps.size() != 0)
                        {
                            message =
                                SS("Device " << alsa_device_name << " in use. The following applications are using your soundcard: " << apps
                                             << ". Stop them as neccesary before trying to  start pipedald.");
                        }
                        else
                        {
                            message =
                                SS("Device " << alsa_device_name << " in use. Stop the application using it before trying to restart pipedald. ");
                        }
                        Lv2Log::error(message);
                        throw PiPedalStateException(std::move(message));
                    }
                    break;
                    case EPERM:
                        throw PiPedalStateException(SS("Permission denied opening device '" << alsa_device_name << "'"));
                    default:
                        throw PiPedalStateException(SS("Unexepected error (" << errno << ") opening device '" << alsa_device_name << "'"));
                    }
                }
                if (this->playbackHandle)
                {
                    snd_pcm_nonblock(playbackHandle, 0);
                }

                this->alsa_device_name = inputName;
                err = snd_pcm_open(&captureHandle, inputName.c_str(), SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);

                if (err < 0)
                {
                    switch (errno)
                    {
                    case EBUSY:
                    {
                        std::string apps = discover_alsa_using_apps();
                        std::string message;
                        if (apps.size() != 0)
                        {
                            message =
                                SS("Device " << alsa_device_name << " in use. The following applications are using your soundcard: " << apps
                                             << ". Stop them as neccesary before trying to restart pipedald.");
                        }
                        else
                        {
                            message =
                                SS("Device " << alsa_device_name << " in use. Stop the application using it before trying to restart pipedald. ");
                        }
                        Lv2Log::error(message);
                        throw PiPedalStateException(std::move(message));
                    }
                    break;
                    case EPERM:
                        throw PiPedalStateException(SS("Permission denied opening device '" << alsa_device_name << "'"));
                    default:
                        throw PiPedalStateException(SS("Unexepected error (" << errno << ") opening device '" << alsa_device_name << "'"));
                    }
                }
                if (this->captureHandle)
                {
                    snd_pcm_nonblock(captureHandle, 0);
                }

                if ((err = snd_pcm_hw_params_malloc(&captureHwParams)) < 0)
                {
                    throw PiPedalStateException("Failed to allocate captureHwParams");
                }
                if ((err = snd_pcm_sw_params_malloc(&captureSwParams)) < 0)
                {
                    throw PiPedalStateException("Failed to allocate captureSwParams");
                }
                if ((err = snd_pcm_hw_params_malloc(&playbackHwParams)) < 0)
                {
                    throw PiPedalStateException("Failed to allocate playbackHwParams");
                }
                if ((err = snd_pcm_sw_params_malloc(&playbackSwParams)) < 0)
                {
                    throw PiPedalStateException("Failed to allocate playbackSwParams");
                }

                SetAlsaParameters(jackServerSettings.GetBufferSize(), jackServerSettings.GetNumberOfBuffers(), jackServerSettings.GetSampleRate());
                PublishPeriodDuration();
                capture_and_playback_not_synced = false;

                if (captureHandle && playbackHandle)
                {
                    if (snd_pcm_link(playbackHandle,
                                     captureHandle) != 0)
                    {
                        capture_and_playback_not_synced = true;
                    }
                }

                snd_pcm_format_t captureFormat;
                snd_pcm_hw_params_get_format(captureHwParams, &captureFormat);
                copyInputFn = nullptr;

                PrepareCaptureFunctions(captureFormat);

                snd_pcm_format_t playbackFormat;
                snd_pcm_hw_params_get_format(playbackHwParams, &playbackFormat);

                PreparePlaybackFunctions(playbackFormat);
            }
            catch (const std::exception &e)
            {
                AlsaCleanup();
                throw;
            }
        }

        // Shared body of FillOutputBuffer()/FillOutputBufferRt(). Never throws, never
        // allocates. Returns true on success; on failure, *what is a static description
        // and *errorCode the ALSA error (0 if none). Sleeps between retries only if
        // allowSleep (not on the audio thread).
        bool FillOutputBuffer_(bool allowSleep, const char **what, int *errorCode) noexcept
        {
            memset(rawPlaybackBuffer.data(), 0, rawPlaybackBuffer.size());
            int retry = 0;
            if (this->isDummyDriver)
            {
                return true; // dummy driver is insatiable.
            }
            while (true)
            {
                auto avail = snd_pcm_avail(this->playbackHandle);
                if (avail < 0)
                {
                    if (avail == -EAGAIN)
                    {
                        return true;
                    }
                    if (++retry >= 5) // kinda sus code. let's make sure we don't spin forever.
                    {
                        *what = "Timed out trying to fill the audio output buffer.";
                        *errorCode = (int)avail;
                        return false;
                    }

                    int err = snd_pcm_prepare(playbackHandle);
                    if (err < 0)
                    {
                        *what = "Audio playback failed.";
                        *errorCode = err;
                        return false;
                    }
                    if (allowSleep)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    continue;
                }
                if (avail == 0)
                    break;

                // Write in chunks of the preallocated buffer rather than growing it:
                // this also runs on the audio thread (xrun resync).
                snd_pcm_sframes_t maxFrames = (snd_pcm_sframes_t)(this->rawPlaybackBuffer.size() / playbackFrameSize);
                if (maxFrames == 0)
                    break;
                if (avail > maxFrames)
                    avail = maxFrames;

                ssize_t err = WriteBuffer(playbackHandle, rawPlaybackBuffer.data(), avail);
                if (err < 0)
                {
                    *what = "Audio playback failed.";
                    *errorCode = (int)err;
                    return false;
                }
            }
            return true;
        }

        // Non-realtime callers (start-up, device restart): throws on failure.
        void FillOutputBuffer()
        {
            validate_capture_handle();
            const char *what = nullptr;
            int err = 0;
            if (!FillOutputBuffer_(true, &what, &err))
            {
                if (err != 0)
                {
                    throw PiPedalStateException(SS(what << " " << snd_strerror(err)));
                }
                throw PiPedalStateException(what);
            }
            validate_capture_handle();
        }

        // Audio thread (xrun resync): no exception, no allocation, no sleep. On failure
        // the reason goes to recoveryError for the service thread to log.
        bool FillOutputBufferRt(const char *direction) noexcept
        {
            const char *what = nullptr;
            int err = 0;
            if (!FillOutputBuffer_(false, &what, &err))
            {
                recoveryError.Set(err, "Cannot refill playback stream (", direction, " xrun): ", what);
                return false;
            }
            return true;
        }
        // Bring both streams back from an xrun and start them together again.
        //
        // Both recovery paths need exactly this sequence. It used to be spelled
        // out only in the input path, and the output path had its own shorter
        // version that prepared playback, drained capture and started neither —
        // which left capture stopped for good. That failure is invisible from
        // inside the driver: no error is returned and no xrun is reported, the
        // capture stream simply stops delivering while playback carries on, so
        // the symptom is silence at the interface with the plugin chain still
        // apparently running. Sharing one implementation is what stops the two
        // from drifting apart again.
        //
        // Returns false on failure, with the reason in recoveryError: no logging, no
        // allocation here, since this runs on the audio thread. Callers count the xrun
        // in xrunCounter, and the host's service thread logs the counts
        // (LogRealtimeStatistics). A recovery that fails is logged, with the reason, by
        // the service thread when it restarts the device (ServiceRestartRequest).
        bool resync_streams(snd_pcm_t *capture_handle, snd_pcm_t *playback_handle,
                            const char *direction)
        {
            int err;

            // Unlink first. While the streams are linked, prepare and start
            // propagate across the whole group, so recovering them one at a
            // time requires them apart.
            const bool relink = !capture_and_playback_not_synced;
            if (relink)
            {
                snd_pcm_unlink(capture_handle);
            }

            if ((err = snd_pcm_prepare(playback_handle)) < 0)
            {
                recoveryError.Set(err, "Cannot prepare playback stream (", direction, " xrun)");
                return false;
            }
            if ((err = snd_pcm_prepare(capture_handle)) < 0)
            {
                recoveryError.Set(err, "Cannot prepare capture stream (", direction, " xrun)");
                return false;
            }

            if (relink)
            {
                if ((err = snd_pcm_link(capture_handle, playback_handle)) < 0)
                {
                    recoveryError.Set(err, "Cannot relink streams (", direction, " xrun)");
                    return false;
                }
            }

            // Same order the audio thread uses for the very first start: fill
            // the playback buffer, then start capture explicitly. Playback
            // starts itself once its start threshold is reached, but capture
            // never does — without this call it sits in PREPARED forever.
            if (!FillOutputBufferRt(direction))
            {
                return false;
            }
            if ((err = snd_pcm_start(capture_handle)) < 0)
            {
                recoveryError.Set(err, "Cannot restart capture stream (", direction, " xrun)");
                return false;
            }
            return true;
        }

        // Returns false if the device could not be recovered or restarted: the audio thread
        // has to stop (the reason is in audioStopReason).
        bool recover_from_output_underrun(snd_pcm_t *capture_handle, snd_pcm_t *playback_handle, int err, size_t framesRead)
        {
            validate_capture_handle();

            TraceBufferPositions(framesRead, 'w');
            xrunCounter.OnOutputXrun();
            bool recovered = false;
            if (err == -EPIPE)
            {
                recovered = resync_streams(capture_handle, playback_handle, "output");
                TraceBufferPositions(framesRead, 'x');
            }
            else if (err == -ESTRPIPE)
            {
                // Suspended (system suspend/resume). Not recovered in place: snd_pcm_resume()
                // has to be retried with sleeps, which has no place on the audio thread.
                // Reopening the device on the service thread recovers it.
                TraceBufferPositions(framesRead, 'z');
                recoveryError.Set(err, "ALSA playback stream suspended");
            }
            else
            {
                TraceBufferPositions(framesRead, 'z');
                recoveryError.Set(err, "Can't recover from ALSA output error");
            }
            if (!recovered && !RestartAfterFailedRecovery())
            {
                return false;
            }
            if (!terminateAudio()) // see AudioThread(): handles may be in flux while shutting down.
            {
                validate_capture_handle();
            }
            return true;
        }
        // Returns false if the device could not be recovered or restarted (see recover_from_output_underrun).
        bool recover_from_input_underrun(snd_pcm_t *capture_handle, snd_pcm_t *playback_handle, int err, size_t bufferedFrames)
        {
            validate_capture_handle();

            TraceBufferPositions(bufferedFrames, 'r');
            xrunCounter.OnInputXrun();
            bool recovered = false;
            if (err == -EPIPE)
            {
                recovered = resync_streams(capture_handle, playback_handle, "input");
                validate_capture_handle();
            }
            else if (err == -ESTRPIPE)
            {
                // Suspended. See recover_from_output_underrun(): handed to the service thread.
                recoveryError.Set(err, "ALSA capture stream suspended");
            }
            else
            {
                recoveryError.Set(err, "Can't recover from ALSA input error");
            }
            if (!recovered && !RestartAfterFailedRecovery())
            {
                return false;
            }
            return true;
        }

        // Audio thread. In-place xrun recovery failed, so the device has to be closed
        // and reopened. That means logging, allocation and blocking ALSA calls, none of
        // which belong on the audio thread, so the restart is handed to the host's
        // non-realtime service thread (rtsvc, via AudioDriverHost::RequestDriverRestart)
        // and this thread waits for the outcome, for a bounded time (RESTART_WAIT_LIMIT_MS).
        // There is no audio to process meanwhile, but host commands keep flowing.
        //
        // Returns false if the device is gone for good: the caller leaves the audio loop,
        // and the reason (a static string: nothing is allocated here) is in audioStopReason.
        // Also returns true when shutting down; the caller sees terminateAudio() next.
        //
        // If the host has no service thread to take the request, fall back to the
        // previous behaviour: restart on this thread (which is not realtime-safe, and
        // throws on failure).
        bool RestartAfterFailedRecovery()
        {
            xrunCounter.OnFailedRecovery();
            deferredRestart.Request();
            if (!driverHost->RequestDriverRestart())
            {
                if (deferredRestart.Withdraw())
                {
                    // Counted, and reported by the service thread when it runs (LogRealtimeStatistics), or
                    // when the driver is closed.
                    xrunCounter.OnAudioThreadRestart();
                    RestartAlsa();
                    audioRunning = true;
                    return true;
                }
                // Claimed anyway (cannot normally happen without the message). Wait for it.
            }
            return WaitForDeferredRestart();
        }

        bool WaitForDeferredRestart()
        {
            DeferredRestartResult result = pipedal::WaitForDeferredRestart(
                deferredRestart,
                std::chrono::milliseconds(RESTART_WAIT_LIMIT_MS),
                PeriodDuration(),
                [this]()
                { return terminateAudio(); },
                [this]()
                {
                    // Parked, but still at realtime priority: the host's command
                    // processing is realtime-safe.
                    this->driverHost->OnProcessCommandsWhileRestarting();
                });
            switch (result)
            {
            case DeferredRestartResult::Restarted:
                audioRunning = true;
                return true;
            case DeferredRestartResult::Terminated:
                // Shutting down. If the restart was still running, Close() serialises with
                // it on restartMutex, and the service thread drops the abandoned request.
                return true;
            case DeferredRestartResult::Failed:
                audioStopReason = "Unable to restart the audio stream.";
                return false;
            case DeferredRestartResult::TimedOut:
            default:
                audioStopReason = "Timed out waiting for the audio stream to restart.";
                return false;
            }
        }
        // Why the audio thread stopped abnormally without an exception. Static strings only.
        const char *audioStopReason = nullptr;

        // Safe from any thread: reads the snapshot published by OpenAudio(), not
        // bufferSize/sampleRate, which a restart on the service thread rewrites while the
        // audio thread waits for it (WaitForDeferredRestart).
        std::chrono::microseconds PeriodDuration() const
        {
            return std::chrono::microseconds(periodDurationUs.load(std::memory_order_relaxed));
        }
        void PublishPeriodDuration()
        {
            periodDurationUs.store(RealtimePeriodDuration(bufferSize, sampleRate).count(), std::memory_order_relaxed);
        }
        std::atomic<int64_t> periodDurationUs{RealtimePeriodDuration(0, 0).count()};

    public:
        // Non-realtime service thread (rtsvc). See RestartAfterFailedRecovery().
        //
        // The reopen is retried for a while: after a suspend/resume (laptop lid), a USB
        // interface typically re-enumerates a few seconds after the process thaws, and a
        // single attempt would stop audio for good. restartMutex is held per attempt only,
        // not across the waits, so Close() is never held up for long.
        virtual void ServiceRestartRequest() override
        {
            uint64_t generation;
            {
                std::lock_guard lock{restartMutex};
                if (!deferredRestart.TryBegin())
                {
                    return;
                }
                // Read only once the request is ours: a value read earlier could predate a
                // Close()/Open() whose new session made this request, and the loop below
                // would then return without completing it.
                generation = closeGeneration.load(std::memory_order_acquire);
                if (terminateAudio())
                {
                    deferredRestart.Complete(false);
                    return;
                }
            }
            std::string reason = TakeRecoveryError();
            if (!reason.empty())
            {
                Lv2Log::warning(SS("ALSA xrun recovery failed: " << reason << ". Restarting the audio device."));
            }
            else
            {
                Lv2Log::warning("ALSA xrun recovery failed. Restarting the audio device.");
            }

            const auto retryInterval = std::chrono::milliseconds(RESTART_RETRY_INTERVAL_MS);
            for (int attempt = 1; attempt <= RESTART_ATTEMPTS; ++attempt)
            {
                {
                    std::lock_guard lock{restartMutex};
                    if (closeGeneration.load(std::memory_order_acquire) != generation)
                    {
                        // Closed (and perhaps reopened) while we waited. The audio thread that
                        // asked has gone; don't touch the state a new session may be using.
                        return;
                    }
                    if (deferredRestart.ReleaseAbandoned())
                    {
                        Lv2Log::error("ALSA device restart abandoned: the audio thread stopped waiting for it.");
                        return;
                    }
                    if (deferredRestart.Get() != DeferredRestart::State::Running)
                    {
                        // Reset by Deactivate()/Close() after joining the audio thread (or a new
                        // request from a later session): this one is stale.
                        return;
                    }
                    if (terminateAudio())
                    {
                        deferredRestart.Complete(false);
                        return;
                    }
                    try
                    {
                        RestartAlsa();
                        if (!deferredRestart.Complete(true))
                        {
                            // The audio thread gave up while this attempt ran. The device stays
                            // open but unused until the driver is closed.
                            Lv2Log::error("ALSA device restarted, but the audio thread had already stopped waiting for it.");
                            return;
                        }
                        if (attempt > 1)
                        {
                            Lv2Log::info(SS("ALSA device restarted (attempt " << attempt << ")."));
                        }
                        return;
                    }
                    catch (const std::exception &e)
                    {
                        Lv2Log::error(SS("ALSA restart attempt " << attempt << "/" << RESTART_ATTEMPTS << " failed. " << e.what()));
                    }
                }
                if (attempt == RESTART_ATTEMPTS)
                {
                    break;
                }
                // Wait without the lock, in short steps so a shutdown is noticed promptly.
                auto resumeAt = std::chrono::steady_clock::now() + retryInterval;
                while (std::chrono::steady_clock::now() < resumeAt)
                {
                    if (terminateAudio() || closeGeneration.load(std::memory_order_acquire) != generation ||
                        deferredRestart.IsAbandoned())
                    {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
            }
            std::lock_guard lock{restartMutex};
            if (closeGeneration.load(std::memory_order_acquire) == generation)
            {
                if (deferredRestart.Complete(false))
                {
                    Lv2Log::error("Giving up on restarting the ALSA device.");
                }
                else
                {
                    Lv2Log::error("ALSA device restart abandoned: the audio thread stopped waiting for it.");
                }
            }
        }

        // Service thread: the reason the audio thread last gave up on in-place recovery
        // (empty if none is pending).
        std::string TakeRecoveryError()
        {
            char text[RealtimeErrorMessage::CAPACITY];
            int err = 0;
            if (!recoveryError.Take(text, &err))
            {
                return std::string();
            }
            std::string result = text;
            if (err != 0)
            {
                result = SS(result << " (" << snd_strerror(err) << ")");
            }
            return result;
        }

        // Non-realtime service thread (rtsvc): log what the audio thread counted.
        virtual bool LogRealtimeStatistics() override
        {
            bool logged = false;
            // Normally taken (and logged) by ServiceRestartRequest(). Still pending here if
            // the audio thread had to restart the device itself. Leave it alone while a
            // deferred restart is requested or running, so the reason is logged together
            // with its restart. (Residual: the audio thread sets the reason just before it
            // requests the restart; if we run in that instant, the reason is logged here.)
            std::string recoveryReason;
            if (deferredRestart.Get() == DeferredRestart::State::Idle)
            {
                recoveryReason = TakeRecoveryError();
            }
            if (!recoveryReason.empty())
            {
                Lv2Log::warning(SS("ALSA xrun recovery failed: " << recoveryReason << "."));
                logged = true;
            }
            XrunCounts xruns = xrunCounter.Take();
            if (xruns.Any())
            {
                Lv2Log::info(SS(
                    "ALSA xrun recoveries: " << xruns.input << " input, " << xruns.output << " output"
                                             << ", " << xruns.failedRecoveries << " requiring a device restart."));
                if (xruns.audioThreadRestarts != 0)
                {
                    Lv2Log::warning(SS("ALSA: " << xruns.audioThreadRestarts
                                                << " device restart(s) performed on the audio thread (no service thread was running)."));
                }
                logged = true;
            }
            uint64_t droppedMidiEvents = midiInput.TakeDroppedEvents();
            if (droppedMidiEvents != 0)
            {
                Lv2Log::warning(SS("MIDI input overflow: " << droppedMidiEvents << " event(s) dropped."));
                logged = true;
            }
            if (alsaSequencer)
            {
                uint64_t malformedMidiEvents = alsaSequencer->TakeMalformedEventCount();
                if (malformedMidiEvents != 0)
                {
                    Lv2Log::warning(SS("MIDI input: " << malformedMidiEvents << " malformed sequencer event(s) dropped."));
                    logged = true;
                }
            }
            return logged;
        }

    private:
        RealtimeXrunCounter xrunCounter;
        DeferredRestart deferredRestart;
        // Why in-place recovery last failed. Set on the audio thread, logged by the service thread.
        RealtimeErrorMessage recoveryError;

        // ServiceRestartRequest(): up to RESTART_ATTEMPTS reopens, RESTART_RETRY_INTERVAL_MS
        // apart (about 10 s in all) before the audio thread is told the device is gone.
        static constexpr int RESTART_ATTEMPTS = 20;
        static constexpr int RESTART_RETRY_INTERVAL_MS = 500;
        // How long the audio thread waits for that outcome (WaitForDeferredRestart): the retry
        // budget, plus generous slack for the reopen attempts themselves (each may take a
        // while on a re-enumerating USB device) and for rtsvc picking up the request. Past
        // it, the request is abandoned and the audio thread stops.
        static constexpr int RESTART_WAIT_SLACK_MS = 20000;
        static constexpr int RESTART_WAIT_LIMIT_MS = RESTART_ATTEMPTS * RESTART_RETRY_INTERVAL_MS + RESTART_WAIT_SLACK_MS;
        // Bumped by Close(), so a restart still retrying from a previous session gives up.
        std::atomic<uint64_t> closeGeneration{0};

        void DumpStatus(snd_pcm_t *handle)
        {
#ifdef ALSADRIVER_CONFIG_DBG
            snd_pcm_status(handle, snd_status);
            snd_pcm_status_dump(snd_status, snd_output);
#endif
        }

        std::unique_ptr<std::jthread> audioThread;
        bool audioRunning;

        bool block = false;

        snd_pcm_sframes_t ReadBuffer(snd_pcm_t *handle, uint8_t *buffer, snd_pcm_uframes_t frames)
        {
            // transcode to jack format.
            // expand running status if neccessary.
            // deal with regular and sysex messages split across
            // buffer boundaries (but discard them)
            snd_pcm_sframes_t framesRead = 0;

            auto state = snd_pcm_state(handle);
            auto frame_bytes = this->captureFrameSize;
            do
            {
                TraceBufferPositions(framesRead, '1');

                framesRead = snd_pcm_readi(handle, buffer, frames);
                if (framesRead < 0)
                {
                    return framesRead;
                }
                if (framesRead > 0)
                {
                    buffer += framesRead * frame_bytes;
                    frames -= framesRead;
                }
                if (framesRead == 0)
                {
                    snd_pcm_wait(handle, frames);
                }
            } while (frames > 0);

            TraceBufferPositions(framesRead, '2');

            return framesRead;
        }

    protected:
        // Audio thread, once per cycle (after midiInput.Clear(), before the PCM read).
        // Appends to the cycle's events; never resets them, never grows the buffer.
        void ReadMidiData(uint32_t audioFrame)
        {
            // Raw pointer: no refcount traffic on the audio thread. The sequencer is set
            // before Activate() and released only in Close(), after the audio thread is joined.
            AlsaSequencer *sequencer = this->alsaSequencer.get();
            if (!sequencer)
            {
                return;
            }
            AlsaMidiMessage message;
            DrainMidiInput(*sequencer, message, midiInput, audioFrame);
        }

    private:
        bool isDummyDriver = false;
        long WriteBuffer(snd_pcm_t *handle, uint8_t *buf, size_t frames)
        {
            long framesRead;
            auto frame_bytes = this->playbackFrameSize;

            while (frames > 0)
            {
                framesRead = snd_pcm_writei(handle, buf, frames);
                if (framesRead == -EAGAIN)
                    continue;
                if (framesRead < 0)
                    return framesRead;
                buf += framesRead * frame_bytes;
                frames -= framesRead;
            }
            return 0;
        }
        PIPEDAL_NON_INLINE void AudioThread()
        {
            SetThreadName("alsaDriver");

            // Set when a failed xrun recovery could not restart the device. Reported (with
            // audioStopReason) only after leaving realtime operation: nothing is allocated or
            // logged on the way out of the loop.
            bool deviceLost = false;

            try
            {
                SetThreadPriority(SchedulerPriority::RealtimeAudio);
                EnableFlushToZero();

                bool ok = true;

                auto playbackState = snd_pcm_state(playbackHandle);

                FillOutputBuffer();

                int err;
                if ((err = snd_pcm_start(captureHandle)) < 0)
                {
                    throw PiPedalStateException(SS("Unable to start ALSA capture. " << snd_strerror(err)));
                }

                CrashGuardLock crashGuardLock;

                cpuUse.SetPeriod(std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>((double)bufferSize / (double)sampleRate)));
                cpuUse.SetStartTime(cpuUse.Now());
                while (true)
                {
                    // Terminating first: after a wait for a restart that ended in shutdown, an
                    // abandoned restart may still be replacing the handles on the service thread.
                    if (terminateAudio())
                    {
                        break;
                    }
                    validate_capture_handle();
                    cpuUse.UpdateCpuUse();

                    // MIDI: one read per cycle, before the PCM read. (It used to run on every
                    // partial-read iteration, each one discarding the events gathered so far.)
                    this->midiInput.Clear();
                    ReadMidiData(0);

                    // snd_pcm_wait(captureHandle, 1);
                    ssize_t framesToRead = bufferSize;
                    ssize_t framesRead = 0;
                    bool xrun = false;
                    validate_capture_handle();

                    while (framesToRead != 0)
                    {
                        ssize_t thisTime = framesToRead;
                        ssize_t nFrames;
                        if ((nFrames = ReadBuffer(
                                 captureHandle,
                                 this->rawCaptureBuffer.data() + this->captureFrameSize * framesRead,
                                 framesToRead)) < 0)
                        {
                            this->driverHost->OnUnderrun();
                            if (!recover_from_input_underrun(captureHandle, playbackHandle, nFrames, framesRead))
                            {
                                deviceLost = true;
                            }
                            xrun = true;
                            break;
                        }
                        framesRead += nFrames;
                        framesToRead -= nFrames;
                    }
                    if (deviceLost)
                    {
                        // Before touching the handles: an abandoned restart may still be
                        // replacing them on the service thread.
                        break;
                    }
                    if (terminateAudio())
                    {
                        break; // Likewise (the restart wait ended in shutdown).
                    }
                    validate_capture_handle();

                    if (xrun)
                    {
                        continue;
                    }
                    cpuUse.AddSample(ProfileCategory::Read);
                    if (framesRead == 0)
                        continue;
                    if (framesRead != bufferSize)
                    {
                        throw PiPedalStateException("Invalid read.");
                    }

                    (this->*copyInputFn)(framesRead);
                    cpuUse.AddSample(ProfileCategory::Driver);

                    this->driverHost->OnProcess(framesRead);

                    cpuUse.AddSample(ProfileCategory::Execute);

                    // Perform any neccessary mixing of outputs.
                    for (auto&mixOp: this->mixOps)
                    {
                        mixOp(framesRead);
                    }

                    // final format conversion.
                    (this->*copyOutputFn)(framesRead);

                    if (this->driverHost)
                    {
                        driverHost->OnRealtimeUpdateDeviceVus(framesRead);
                    }

                    cpuUse.AddSample(ProfileCategory::Driver);
                    // process.

                    ssize_t err = WriteBuffer(playbackHandle, rawPlaybackBuffer.data(), framesRead);

                    if (err < 0)
                    {
                        this->driverHost->OnUnderrun();

                        if (!recover_from_output_underrun(captureHandle, playbackHandle, err, framesRead))
                        {
                            deviceLost = true;
                            break;
                        }
                        framesRead = 0;
                    }
                    if (isDummyDriver)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1000 / 30));
                    }
                    cpuUse.AddSample(ProfileCategory::Write);
                }
            }
            catch (const std::exception &e)
            {
                // Leaving realtime operation for good: demote before logging.
                DemoteToNonRealtime();
                // Thrown while parked for a restart (e.g. by the host's command processing):
                // don't leave the request pending for a thread that will never collect it.
                // A running restart is abandoned; the service thread drops it.
                DeferredRestart::State state = deferredRestart.Abandon();
                if (state == DeferredRestart::State::Succeeded || state == DeferredRestart::State::Failed)
                {
                    deferredRestart.Reset();
                }
                Lv2Log::error(e.what());
                Lv2Log::error("ALSA audio thread terminated abnormally.");
            }
            if (deviceLost)
            {
                DemoteToNonRealtime();
                Lv2Log::error(audioStopReason ? audioStopReason : "Unable to restart the audio stream.");
                Lv2Log::error("ALSA audio thread terminated abnormally.");
            }

            // if we terminated abnormally, pump messages until we have been terminated.
            if (!terminateAudio())
            {
                DemoteToNonRealtime();
                this->driverHost->OnAlsaDriverStopped();
                {
                    // An abandoned restart may still be running on the service thread, rewriting
                    // the buffers and bufferSize: serialise with it (held per attempt only).
                    std::lock_guard lock{restartMutex};
                    // zero out input buffers.
                    for (size_t i = 0; i < this->deviceCaptureBuffers.size(); ++i)
                    {
                        float *pBuffer = deviceCaptureBuffers[i];
                        for (size_t j = 0; j < this->bufferSize; ++j)
                        {
                            pBuffer[j] = 0;
                        }
                    }
                }
                try
                {
                    // The device is dead: no DSP, just keep host commands flowing until
                    // Deactivate(). Once per period, at SCHED_OTHER.
                    const auto period = PeriodDuration();
                    while (!terminateAudio())
                    {
                        std::this_thread::sleep_for(period);
                        this->driverHost->OnProcessCommandsOnly();
                    }
                }
                catch (const std::exception &e)
                {
                }
            }
            this->driverHost->OnAudioTerminated();
        }

        // The audio thread after an abnormal stop: nothing realtime left to do.
        static void DemoteToNonRealtime()
        {
            struct sched_param param;
            memset(&param, 0, sizeof(param));
            param.sched_priority = 0;
            sched_setscheduler(0, SCHED_OTHER, &param);
        }

        bool alsaActive = false;

        PIPEDAL_NON_INLINE void AllocateInputChannels(
            const std::vector<int64_t> &channelSelection,
            std::vector<float *> &channelBuffers)
        {
            size_t nChannels = channelSelection.size();
            if (nChannels == 0)
            {
                channelBuffers.resize(0);
                return;
            }

            channelBuffers.resize(nChannels);
            for (size_t i = 0; i < nChannels; ++i)
            {
                int64_t deviceChannel = channelSelection[i];
                if (deviceChannel == -1 || deviceChannel >= captureChannels)
                {
                    channelBuffers[i] = zeroInputBuffer;
                }
                else
                {
                    channelBuffers[i] = deviceCaptureBuffers[deviceChannel];
                }
            }
        }
        PIPEDAL_NON_INLINE void AllocateOutputChannels(
            const std::vector<int64_t> &channelSelection,
            std::vector<float *> &channelBuffers)
        {
            size_t nChannels = channelSelection.size();

            if (nChannels == 0)
            {
                channelBuffers.resize(0);
                return;
            }
            channelBuffers.resize(nChannels);
            for (size_t i = 0; i < nChannels; ++i)
            {
                int64_t deviceChannel = channelSelection[i];
                if (deviceChannel == -1)
                {
                    channelBuffers[i] = this->GetDiscardOutputBuffer();
                    ;
                }
                else
                {
                    float *mixBuffer = AllocateAudioBuffer();
                    channelBuffers[i] = mixBuffer;
                }
            }
        }


        PIPEDAL_NON_INLINE  void AllocateAuxChannels()
        {
            for (auto ix : channelSelection.auxInputChannels())
            {
                auxCaptureBuffers.push_back(this->deviceCaptureBuffers[ix]);
            }
            for (auto ix : channelSelection.auxOutputChannels())
            {
                auxPlaybackBuffers.push_back(this->devicePlaybackBuffers[ix]);
            }
        }

        using MixOp = std::function<void(size_t nFrames)>;
        std::vector<MixOp> mixOps;

        PIPEDAL_NON_INLINE 
        void AddMixCopyOp(float*inputBuffer, float*outputBuffer)
        {
            mixOps.push_back([inputBuffer,outputBuffer](size_t nFrames) {
                float*PIPEDAL_RESTRICT pIn = inputBuffer;
                float*PIPEDAL_RESTRICT pOut = outputBuffer;
                for (size_t i = 0; i < nFrames; ++i)
                {
                    pOut[i] = pIn[i];
                }
            });
        }
        PIPEDAL_NON_INLINE 
        void AddMixAddOp(float*inputBuffer, float*outputBuffer)
        {
            mixOps.push_back([inputBuffer,outputBuffer](size_t nFrames) {
                float*PIPEDAL_RESTRICT pIn = inputBuffer;
                float*PIPEDAL_RESTRICT pOut = outputBuffer;
                for (size_t i = 0; i < nFrames; ++i)
                {
                    pOut[i] += pIn[i];
                }
            });
        }
        PIPEDAL_NON_INLINE 
        void AddMixCopyOp(float scale, float*inputBuffer, float*outputBuffer)
        {
            mixOps.push_back([scale,inputBuffer,outputBuffer](size_t nFrames) {
                float*PIPEDAL_RESTRICT pIn = inputBuffer;
                float*PIPEDAL_RESTRICT pOut = outputBuffer;
                for (size_t i = 0; i < nFrames; ++i)
                {
                    pOut[i] = scale*pIn[i];
                }
            });
        }
        PIPEDAL_NON_INLINE 
        void AddMixAddOp(float scale, float*inputBuffer, float*outputBuffer)
        {
            mixOps.push_back([scale,inputBuffer,outputBuffer](size_t nFrames) {
                float*PIPEDAL_RESTRICT pIn = inputBuffer;
                float*PIPEDAL_RESTRICT pOut = outputBuffer;
                for (size_t i = 0; i < nFrames; ++i)
                {
                    pOut[i] += scale*pIn[i];
                }
            });
        }
        PIPEDAL_NON_INLINE void AddMixOps() 
        {
            std::set<size_t> usedOutputChannels;

            for (size_t i = 0; i < this->channelSelection.mainOutputChannels().size(); ++i) {
                size_t outputChannel = this->channelSelection.mainOutputChannels()[i];
                AddMixCopyOp(this->mainPlaybackBuffers[i],this->devicePlaybackBuffers[outputChannel]);
                usedOutputChannels.insert(outputChannel);
            }
            if (channelSelection.auxInputChannels().size() <= channelSelection.auxOutputChannels().size())
            {
                for (size_t i = 0; i < this->channelSelection.auxOutputChannels().size(); ++i)
                {
                    size_t outputChannel = this->channelSelection.auxOutputChannels()[i];
                    size_t inputChannel;
                    if (this->channelSelection.auxInputChannels().size() == 0) break;
                    if (i >= this->channelSelection.auxInputChannels().size()) {
                        inputChannel = 0;
                    } else {
                        inputChannel = this->channelSelection.auxInputChannels()[i];
                    }
                    if (outputChannel >= this->devicePlaybackBuffers.size())
                    {
                        continue;
                    }
                    if (usedOutputChannels.contains(outputChannel))
                    {
                        AddMixAddOp(this->deviceCaptureBuffers[inputChannel],this->devicePlaybackBuffers[outputChannel]);
                    } else {
                        AddMixCopyOp(this->deviceCaptureBuffers[inputChannel],this->devicePlaybackBuffers[outputChannel]);
                    }
                    usedOutputChannels.insert(outputChannel);
                }
            } else if (channelSelection.auxInputChannels().size() >= 2 && channelSelection.auxOutputChannels().size() == 1) {
                float scale = 1.0/channelSelection.auxInputChannels().size();
                for (size_t i = 0; i < this->channelSelection.auxOutputChannels().size(); ++i)
                {
                    size_t outputChannel = this->channelSelection.auxOutputChannels()[i];
                    if (usedOutputChannels.contains(outputChannel))
                    {
                        AddMixAddOp(scale,this->auxCaptureBuffers[0],this->devicePlaybackBuffers[outputChannel]);
                    } else {
                        AddMixCopyOp(scale,this->auxCaptureBuffers[0],this->devicePlaybackBuffers[outputChannel]);
                    }
                    usedOutputChannels.insert(outputChannel);
                }
            } 
        }

        bool activated = false;
        PIPEDAL_NON_INLINE virtual void Activate()
        {
            if (activated)
            {
                throw PiPedalStateException("Already activated.");
            }

            activated = true;

            // Reset previously allocated buffers.
            allocatedBuffers.resize(0);

            // Allocate device capture buffers.
            zeroInputBuffer = AllocateAudioBuffer();
            deviceCaptureBuffers.resize(captureChannels);
            for (size_t i = 0; i < captureChannels; ++i)
            {
                deviceCaptureBuffers[i] = AllocateAudioBuffer();
            }
            devicePlaybackBuffers.resize(playbackChannels);
            for (size_t i = 0; i < playbackChannels; ++i)
            {
                devicePlaybackBuffers[i] = AllocateAudioBuffer();
            }


            AllocateInputChannels(
                channelSelection.mainInputChannels(),
                this->mainCaptureBuffers);
            AllocateOutputChannels(
                channelSelection.mainOutputChannels(),
                this->mainPlaybackBuffers);

            AllocateAuxChannels();                
            AddMixOps();

            cpuDmaLatency.Hold();
            audioThread = std::make_unique<std::jthread>([this]()
                                                         { AudioThread(); });
        }

        virtual void Deactivate()
        {
            if (!activated)
            {
                return;
            }
            activated = false;
            // Stale restart retries (ServiceRestartRequest) stop at their next check, even if
            // the driver is re-activated without a Close().
            closeGeneration.fetch_add(1, std::memory_order_acq_rel);
            terminateAudio(true);
            if (audioThread)
            {
                this->audioThread = 0; // jthread joins.
            }
            {
                // The audio thread is joined: a restart it abandoned, or a result it never
                // collected, must not reach the next one. Under restartMutex, so a service
                // thread mid-attempt finishes first and then sees the new generation.
                std::lock_guard lock{restartMutex};
                deferredRestart.Reset();
            }
            Lv2Log::debug("Audio thread joined.");
            cpuDmaLatency.Release();
        }
        CpuDmaLatency cpuDmaLatency;

        static constexpr size_t MIDI_MEMORY_BUFFER_SIZE = 32 * 1024;
        static constexpr size_t MAX_MIDI_EVENT = 4 * 1024;

        // Fixed capacity; overflow is dropped and counted, never grown on the audio thread.
        RealtimeMidiEventBuffer midiInput{MAX_MIDI_EVENT, MIDI_MEMORY_BUFFER_SIZE};
        AlsaSequencer::ptr alsaSequencer;

    public:
        virtual const ChannelSelection &GetChannelSelection() const override
        {
            return channelSelection;
        }

        virtual void SetAlsaSequencer(AlsaSequencer::ptr alsaSequencer) override
        {
            this->alsaSequencer = alsaSequencer;
        }

        virtual size_t DeviceInputBufferCount() const override
        {
            return deviceCaptureBuffers.size();
        }
        virtual size_t DeviceOutputBufferCount() const override
        {
            return devicePlaybackBuffers.size();
        }

        virtual float *GetDeviceInputBuffer(size_t channel) const override
        {
            if (channel >= deviceCaptureBuffers.size())
                return nullptr;
            return deviceCaptureBuffers[channel];
        }
        virtual float *GetDeviceOutputBuffer(size_t channel) const override
        {
            if (channel >= devicePlaybackBuffers.size())
                return nullptr;
            return devicePlaybackBuffers[channel];
        }

        virtual float *GetZeroInputBuffer()
        {
            if (zeroInputBuffer == nullptr)
            {
                zeroInputBuffer = AllocateAudioBuffer();
            }
            return zeroInputBuffer;
        }
        virtual float *GetDiscardOutputBuffer()
        {
            if (discardOutputBuffer == nullptr)
            {
                discardOutputBuffer = AllocateAudioBuffer();
            }
            return discardOutputBuffer;
        }

        virtual std::vector<float *> &DeviceInputBuffers() override { return this->deviceCaptureBuffers; }
        virtual std::vector<float *> &DeviceOutputBuffers() override { return this->devicePlaybackBuffers; }

        virtual std::vector<float *> &MainInputBuffers() override { return this->mainCaptureBuffers; }
        virtual std::vector<float *> &MainOutputBuffers() override { return this->mainPlaybackBuffers; }

        virtual std::vector<float *> &AuxInputBuffers() override { return this->auxCaptureBuffers; }
        virtual std::vector<float *> &AuxOutputBuffers() override { return this->auxPlaybackBuffers; }

        virtual size_t MainInputBufferCount() const { return mainCaptureBuffers.size(); }
        virtual float *GetMainInputBuffer(size_t channel) override
        {
            if (channel >= (int64_t)mainCaptureBuffers.size())
            {
                throw std::runtime_error("Argument out of range.");
            }
            return mainCaptureBuffers[channel];
        }
        virtual size_t AuxInputBufferCount() const { return auxCaptureBuffers.size(); }
        virtual float *GetAuxInputBuffer(size_t channel) override
        {
            return auxCaptureBuffers[channel];
        }
        virtual size_t AuxOutputBufferCount() const
        {
            return auxPlaybackBuffers.size();
        }
        virtual float *GetAuxOutputBuffer(size_t channel) override
        {
            return auxPlaybackBuffers[channel];
        }

        virtual size_t GetMidiInputEventCount() override
        {
            return midiInput.Count();
        }
        virtual MidiEvent *GetMidiEvents() override
        {
            return this->midiInput.Events();
        }

        virtual size_t MainOutputBufferCount() const { return mainPlaybackBuffers.size(); }
        virtual float *GetMainOutputBuffer(size_t channel) override
        {
            return mainPlaybackBuffers[channel];
        }
        float *AllocateAudioBuffer()
        {
            std::vector<float> buffer;
            buffer.resize(this->bufferSize);
            float *pBuffer = buffer.data();
            allocatedBuffers.push_back(std::move(buffer));
            return pBuffer;
        }
        void DeleteBuffers()
        {
            mainCaptureBuffers.clear();
            mainPlaybackBuffers.clear();
            auxCaptureBuffers.clear();
            auxPlaybackBuffers.clear();
            zeroInputBuffer = nullptr;
            discardOutputBuffer = nullptr;
            allocatedBuffers.clear();
        }
        virtual void Close()
        {
            std::atomic_thread_fence(std::memory_order::acquire);

            if (!open)
            {
                return;
            }
            open = false;
            closeGeneration.fetch_add(1, std::memory_order_acq_rel);
            Deactivate();
            {
                // The audio thread is joined. A restart it abandoned (or a result it never
                // collected) must not outlive the session: under restartMutex, so a service
                // thread mid-attempt finishes first, then sees the new generation and leaves
                // the state alone. Idle also lets LogRealtimeStatistics() report the reason.
                std::lock_guard lock{restartMutex};
                deferredRestart.Reset();
            }
            // The audio thread has stopped: report what it counted since the service thread last did
            // (e.g. device restarts it had to perform itself because no service thread was running).
            LogRealtimeStatistics();
            AlsaCleanup();
            DeleteBuffers();
            this->alsaSequencer = nullptr;

            std::atomic_thread_fence(std::memory_order::release);
        }

        virtual float CpuUse()
        {
            return cpuUse.GetCpuUse();
        }

        virtual float CpuOverhead()
        {
            return cpuUse.GetCpuOverhead();
        }
    };

    AudioDriver *CreateAlsaDriver(AudioDriverHost *driverHost)
    {
        return new AlsaDriverImpl(driverHost);
    }

    bool GetAlsaChannels(const JackServerSettings &jackServerSettings,
                         std::vector<std::string> &inputAudioPorts,
                         std::vector<std::string> &outputAudioPorts)
    {
        // if (jackServerSettings.IsDummyAudioDevice())
        // {
        //     auto nChannels = GetDummyAudioChannels(jackServerSettings.GetAlsaInputDevice());

        //     inputAudioPorts.clear();
        //     outputAudioPorts.clear();
        //     for (uint32_t i = 0; i < nChannels; ++i)
        //     {
        //         inputAudioPorts.push_back(std::string(SS("system::capture_" << i)));
        //         outputAudioPorts.push_back(std::string(SS("system::playback_" << i)));
        //     }
        //     return true;
        // }

        snd_pcm_t *playbackHandle = nullptr;
        snd_pcm_t *captureHandle = nullptr;
        snd_pcm_hw_params_t *playbackHwParams = nullptr;
        snd_pcm_hw_params_t *captureHwParams = nullptr;

        Finally ff_playbackHandle{
            [&playbackHandle]()
            {
                if (playbackHandle)
                {
                    int rc = snd_pcm_close(playbackHandle);
                    if (rc < 0)
                    {
                        throw std::runtime_error("snd_pcm_close failed.");
                    }
                    playbackHandle = nullptr;
                }
            }};
        Finally ff_captureHandle{
            [&captureHandle]()
            {
                if (captureHandle)
                {
                    int rc = snd_pcm_close(captureHandle);
                    if (rc < 0)
                    {
                        throw std::runtime_error("snd_pcm_close failed.");
                    }

                    captureHandle = nullptr;
                }
            }};
        Finally ff_playbackHwParams{
            [&playbackHwParams]()
            {
                if (playbackHwParams)
                {
                    snd_pcm_hw_params_free(playbackHwParams);
                }
            }};
        Finally ff_captureHwParams{
            [&captureHwParams]()
            {
                if (captureHwParams)
                {
                    snd_pcm_hw_params_free(captureHwParams);
                }
            }};

        std::string alsaDeviceName = jackServerSettings.GetAlsaInputDevice();
        bool result = false;

        try
        {
            int err;
            for (int retry = 0; retry < 4; ++retry)
            {
                err = snd_pcm_open(&playbackHandle, alsaDeviceName.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
                if (err < 0) // field report of a device that is present, but won't immediately open.
                {
                    sleep(1);
                    continue;
                }
                break;
            }
            if (err < 0)
            {
                throw PiPedalStateException(SS(alsaDeviceName << " playback device not found. "
                                                              << "(" << snd_strerror(err) << ")"));
            }

            for (int retry = 0; retry < 15; ++retry)
            {
                err = snd_pcm_open(&captureHandle, alsaDeviceName.c_str(), SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
                if (err == -EBUSY)
                {
                    sleep(1);
                    continue;
                }
                break;
            }
            if (err < 0)
                throw PiPedalStateException(SS(alsaDeviceName << " capture device not found."));

            if (snd_pcm_hw_params_malloc(&playbackHwParams) < 0)
            {
                throw PiPedalLogicException("Out of memory.");
            }
            if (snd_pcm_hw_params_malloc(&captureHwParams) < 0)
            {
                throw PiPedalLogicException("Out of memory.");
            }

            snd_pcm_hw_params_any(playbackHandle, playbackHwParams);
            snd_pcm_hw_params_any(captureHandle, captureHwParams);

            SetPreferredAlsaFormat(alsaDeviceName, "capture", captureHandle, captureHwParams);
            SetPreferredAlsaFormat(alsaDeviceName, "output", playbackHandle, playbackHwParams);

            unsigned int sampleRate = jackServerSettings.GetSampleRate();
            err = snd_pcm_hw_params_set_rate_near(playbackHandle, playbackHwParams, &sampleRate, 0);
            if (err < 0)
            {
                throw PiPedalLogicException("Sample rate not supported.");
            }
            sampleRate = jackServerSettings.GetSampleRate();
            err = snd_pcm_hw_params_set_rate_near(captureHandle, captureHwParams, &sampleRate, 0);
            if (err < 0)
            {
                throw PiPedalLogicException("Sample rate not supported.");
            }

            unsigned int playbackChannels, captureChannels;

            err = snd_pcm_hw_params_get_channels_max(playbackHwParams, &playbackChannels);
            if (err < 0)
            {
                throw PiPedalLogicException("No outut channels.");
            }
            unsigned int channelsMin;
            err = snd_pcm_hw_params_get_channels_min(playbackHwParams, &channelsMin);
            if (err < 0)
            {
                throw PiPedalLogicException("No outut channels.");
            }
            if (ShouldForceStereoChannels(playbackHandle, playbackHwParams, channelsMin, playbackChannels))
            {
                playbackChannels = 2;
            }

            err = snd_pcm_hw_params_get_channels_max(captureHwParams, &captureChannels);
            if (err < 0)
            {
                throw PiPedalLogicException("No input channels.");
            }
            err = snd_pcm_hw_params_get_channels_min(captureHwParams, &channelsMin);
            if (err >= 0)
            {
                if (ShouldForceStereoChannels(captureHandle, captureHwParams, channelsMin, captureChannels))
                {
                    captureChannels = 2;
                }
            }

            inputAudioPorts.clear();
            for (unsigned int i = 0; i < captureChannels; ++i)
            {
                inputAudioPorts.push_back(SS("system::capture_" << i));
            }

            outputAudioPorts.clear();
            for (unsigned int i = 0; i < playbackChannels; ++i)
            {
                outputAudioPorts.push_back(SS("system::playback_" << i));
            }

            result = true;
        }
        catch (const std::exception &e)
        {
            result = false;
            throw;
        }
        return result;
    }

    static void AlsaAssert(bool value)
    {
        if (!value)
            throw PiPedalStateException("Assert failed.");
    }

#ifdef JUNK
    static void ExpectEvent(AlsaDriverImpl::AlsaMidiDeviceImpl &m, int event, const std::vector<uint8_t> message)
    {
        MidiEvent e;
        m.GetMidiInputEvent(&e, event);
        AlsaAssert(e.size == message.size());
        for (size_t i = 0; i < message.size(); ++i)
        {
            AlsaAssert(message[i] == e.buffer[i]);
        }
    }
#endif

    void AlsaDriverImpl::TestFormatEncodeDecode(snd_pcm_format_t captureFormat)
    {
        this->alsa_device_name = "Test";
        this->numberOfBuffers = 3;
        this->bufferSize = 64;
        this->user_threshold = this->bufferSize;
        this->sampleRate = 44100;
        this->captureChannels = 2;
        this->playbackChannels = 2;

        PrepareCaptureFunctions(captureFormat);
        PreparePlaybackFunctions(captureFormat);

        // make sure encode decode round-trips with reasonable accuracy.

        for (size_t i = 0; i < bufferSize; ++i)
        {
            for (size_t c = 0; c < captureChannels; ++c)
            {
                // provide a rich set of approximately readable bits in the output.
                float value = 1.0f * i / bufferSize + 1.0f * (i) / (128.0 * 256.0);

                // only 16-bits of precision in data for 16-bit formats
                if (captureFormat != snd_pcm_format_t::SND_PCM_FORMAT_S16_BE && captureFormat != snd_pcm_format_t::SND_PCM_FORMAT_S16_LE)
                {
                    value += 1.0f * (c) / (128.0 * 256.0 * 256.0);
                }
                this->devicePlaybackBuffers[c][i] = value;
            }
        }

        (this->*copyOutputFn)(bufferSize);

        assert(captureFrameSize == playbackFrameSize);
        memcpy(this->rawCaptureBuffer.data(), this->rawPlaybackBuffer.data(), captureFrameSize * bufferSize);

        (this->*copyInputFn)(bufferSize);

        for (size_t i = 0; i < bufferSize; ++i)
        {
            for (size_t c = 0; c < captureChannels; ++c)
            {
                float error =
                    this->deviceCaptureBuffers[c][i] - this->devicePlaybackBuffers[c][i];

                assert(std::abs(error) < 4e-5);
            }
        }
    }

    void test::AlsaFormatEncodeDecodeTest(AudioDriverHost *testDriverHost)
    {
        static snd_pcm_format_t formats[] = {
            snd_pcm_format_t::SND_PCM_FORMAT_S16_LE,
            snd_pcm_format_t::SND_PCM_FORMAT_S16_BE,
            snd_pcm_format_t::SND_PCM_FORMAT_S32_LE,
            snd_pcm_format_t::SND_PCM_FORMAT_S32_BE,
            snd_pcm_format_t::SND_PCM_FORMAT_S24_3BE,
            snd_pcm_format_t::SND_PCM_FORMAT_S24_3LE,
            snd_pcm_format_t::SND_PCM_FORMAT_FLOAT_BE,
            snd_pcm_format_t::SND_PCM_FORMAT_FLOAT_LE,
        };

        for (auto format : formats)
        {
            // Check audio encode/decode.
            std::unique_ptr<AlsaDriverImpl> alsaDriver{
                (AlsaDriverImpl *)new AlsaDriverImpl(testDriverHost)};

            alsaDriver->TestFormatEncodeDecode(format);
        }
    }
    void test::MidiDecoderTest()
    {
#ifdef JUNK
        AlsaDriverImpl::AlsaMidiDeviceImpl midiState;

        MidiEvent event;

        // Running status decoding.
        {
            static uint8_t m0[] = {0x80, 0x1, 0x2, 0x3, 0x4, 0x5};
            midiState.NextEventBuffer();
            midiState.ProcessInputBuffer(m0, sizeof(m0));
            AlsaAssert(midiState.GetMidiInputEventCount() == 2);
            AlsaAssert(midiState.GetMidiInputEvent(&event, 0));

            ExpectEvent(midiState, 0, {0x80, 0x1, 0x2});
            ExpectEvent(midiState, 1, {0x80, 0x3, 0x4});

            static uint8_t m1[] = {0x06, 0xC0, 0x1, 0x2};
            midiState.NextEventBuffer();
            midiState.ProcessInputBuffer(m1, sizeof(m1));
            AlsaAssert(midiState.GetMidiInputEventCount() == 3);
            ExpectEvent(midiState, 0, {0x80, 0x05, 0x06});
            ExpectEvent(midiState, 1, {0xC0, 0x1});
            ExpectEvent(midiState, 2, {0xC0, 0x2});
        }

        // SYSEX.
        {
            static uint8_t m0[] = {0xF0, 0x76, 0xF7, 0xA};
            midiState.NextEventBuffer();
            midiState.ProcessInputBuffer(m0, 4);
            AlsaAssert(midiState.GetMidiInputEventCount() == 2);
            AlsaAssert(midiState.GetMidiInputEvent(&event, 0));
            AlsaAssert(event.size == 2);
            AlsaAssert(event.buffer[0] == 0xF0);
            AlsaAssert(event.buffer[1] == 0x76);
        }

        // SPLIT SYSEX
        {
            static uint8_t m0[] = {0xF0, 0x76, 0x3B};
            midiState.NextEventBuffer();
            midiState.ProcessInputBuffer(m0, sizeof(m0));
            AlsaAssert(midiState.GetMidiInputEventCount() == 0);
            static uint8_t m1[] = {0x77, 0xF7};
            midiState.NextEventBuffer();
            midiState.ProcessInputBuffer(m1, sizeof(m1));
            AlsaAssert(midiState.GetMidiInputEventCount() == 2);

            AlsaAssert(midiState.GetMidiInputEvent(&event, 0));
            AlsaAssert(event.size == 0x4);
            AlsaAssert(event.buffer[0] == 0xF0);
            AlsaAssert(event.buffer[1] == 0x76);
            AlsaAssert(event.buffer[2] == 0x3B);
            AlsaAssert(event.buffer[3] == 0x77);
        }
#endif
    }

    void FreeAlsaGlobals()
    {
        snd_config_update_free_global(); // to get a clean Valgrind report.
    }

    void AlsaDriverImpl::DumpBufferTrace(size_t nEntries)
    {
        using namespace std;
        int savedPrecision = cout.precision();
        auto savedFlags = cout.flags();

        size_t ix = bufferTraceIndex;
        if (ix < nEntries)
        {
            ix = ix + bufferTraces.size() - nEntries;
        }
        else
        {
            ix -= nEntries;
        }
        uint64_t t0;
        if (bufferTraceIndex == 0)
        {
            t0 = bufferTraces[bufferTraces.size() - 1].time;
        }
        else
        {
            t0 = bufferTraces[bufferTraceIndex - 1].time;
        }
        while (ix != bufferTraceIndex)
        {
            auto &bufferTrace = bufferTraces[ix];

            if (bufferTrace.time != 0)
            {
                int64_t dt = (int64_t)bufferTrace.time - (int64_t)t0;

                cout << bufferTrace.code << " "
                     << fixed << setprecision(3) << dt * 0.001
                     << " " << "inAvail: " << bufferTrace.inAvail
                     << " " << "outAvail: " << bufferTrace.outAvail
                     << " " << "buffered: " << bufferTrace.buffered
                     << " " << "total: " << bufferTrace.total
                     << endl;
            }

            ++ix;
            if (ix == bufferTraces.size())
            {
                ix = 0;
            }
        }

        cout.precision(savedPrecision);
        cout.flags(savedFlags);
    }

    AlsaDeviceInfo MakeDummyDeviceInfo(uint32_t channels)
    {
        AlsaDeviceInfo result;
        constexpr int DUMMY_DEVICE_ID_OFFSET = 100974;
        result.cardId_ = DUMMY_DEVICE_ID_OFFSET + channels;
        result.id_ = SS("dummy:channels_" << channels);
        result.name_ = SS("Dummy Device (" << channels << " channels)");
        result.longName_ = result.name_;
        result.sampleRates_.push_back(44100);
        result.sampleRates_.push_back(48000);
        result.minBufferSize_ = 16;
        result.maxBufferSize_ = 1024;
        result.supportsCapture_ = true;
        result.supportsPlayback_ = true;
        return result;
    }

    uint32_t GetDummyAudioChannels(const std::string &deviceName)
    {
        uint32_t channels;
        int pos = deviceName.find_last_of('_');
        if (pos == std::string::npos)
        {
            throw std::runtime_error("Invalid dummy device name");
        }
        std::istringstream ss(deviceName.substr(pos + 1));
        ss >> channels;
        return channels;
    }
    AudioDriver *CreateDummyAudioDriver(AudioDriverHost *driverHost, const JackServerSettings &jackServerSettings, const ChannelSelection &channelSelection)
    {
        auto dummyServerSettings = jackServerSettings;
        dummyServerSettings.UseDummyAudioDevice();
        auto dummyChannelSelection = channelSelection;
        AudioDriver *driver = new AlsaDriverImpl(driverHost);
        try
        {
            driver->Open(jackServerSettings, channelSelection);
            return driver;
        }
        catch (const std::exception &e)
        {
            delete driver;
            throw;
        }
    }

} // namespace
