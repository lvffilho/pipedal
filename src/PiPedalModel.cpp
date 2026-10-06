// Copyright (c) 2022-2024 Robin Davies
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
#include <future>
#include "ServiceConfiguration.hpp"
#include "AudioConfig.hpp"
#include "ConfigUtil.hpp"
#include <sched.h>
#include "PiPedalModel.hpp"
#include "AudioHost.hpp"
#include "Lv2Log.hpp"
#include <set>
#include "PiPedalConfiguration.hpp"
#include "AdminClient.hpp"
#include "SplitEffect.hpp"
#include "CpuGovernor.hpp"
#include "RegDb.hpp"
#include "RingBufferReader.hpp"
#include "PiPedalUI.hpp"
#include "atom_object.hpp"
#include "Lv2PluginChangeMonitor.hpp"
#include "HotspotManager.hpp"
#include "DBusToLv2Log.hpp"
#include "SysExec.hpp"
#include "Updater.hpp"
#include "util.hpp"
#include "DBusLog.hpp"
#include "AvahiService.hpp"
#include "DummyAudioDriver.hpp"
#include "AudioFiles.hpp"
#include "CrashGuard.hpp"
#include "Tone3000Downloader.hpp"
#include "HtmlHelper.hpp"
#include "UploadPolicy.hpp"
#include "PedalboardBuilder.hpp"
#include "Lv2Pedalboard.hpp"
#include "Lv2Effect.hpp"
#include "PresetInstanceReuse.hpp"
#include "PedalboardEditRouting.hpp"

#ifndef NO_MLOCK
#include <sys/mman.h>
#endif /* NO_MLOCK */

using namespace pipedal;
namespace fs = std::filesystem;

namespace pipedal
{
    // A request to build an Lv2Pedalboard on the pedalboard builder thread.
    // Everything the build needs is copied here under PiPedalModel::mutex, so the build never touches model state.
    struct PedalboardBuildRequest
    {
        Pedalboard pedalboard;
        // When reuseExistingEffects is true, Lv2 effect instances are borrowed from the running pedalboard
        // (PluginHost::UpdateLv2PedalboardStructure). outgoingPedalboard is the pedalboard being replaced;
        // it is captured by the builder thread when a reuse build starts (not at submit time), so that it is
        // always the pedalboard that is actually running. Holding a reference keeps it alive while building.
        // Full builds capture it the same way when they reuse matching instances of the running pedalboard
        // (see BuildPedalboard()); otherwise they leave it empty.
        std::shared_ptr<Lv2Pedalboard> outgoingPedalboard;
        bool reuseExistingEffects = false;
        // PiPedalModel::audioEpoch when the request was made.
        uint64_t audioEpoch = 0;
    };
    struct PedalboardBuildResult
    {
        std::shared_ptr<Lv2Pedalboard> lv2Pedalboard;
        Lv2PedalboardErrorList errorMessages;
        // PiPedalModel::pluginHostConfigurationVersion at build time (sample rate, buffer size, channels).
        uint64_t pluginHostConfigurationVersion = 0;
        // PiPedalModel::audioEpoch when outgoingPedalboard was captured.
        uint64_t audioEpoch = 0;
        // The build borrowed effects of the running pedalboard (reuse builds, and full builds that reused
        // matching instances); see TryInstallBuiltPedalboard().
        bool borrowedRunningEffects = false;
        // Number of running instances reused by a full build.
        size_t reusedInstances = 0;
    };
    class PedalboardBuilder : public LatestOnlyBuilder<PedalboardBuildRequest, PedalboardBuildResult>
    {
    public:
        using base = LatestOnlyBuilder<PedalboardBuildRequest, PedalboardBuildResult>;
        using base::base;

        // Protected by PiPedalModel::mutex.
        PedalboardBuildModeTracker modeTracker;
    };
}

template <typename T>
T &constMutex(const T &mutex)
{
    return const_cast<T &>(mutex);
}

static const char *hexChars = "0123456789ABCDEF";

static std::string BytesToHex(const std::vector<uint8_t> &bytes)
{
    std::stringstream s;

    for (size_t i = 0; i < bytes.size(); ++i)
    {
        uint8_t b = bytes[i];
        s << hexChars[(b >> 4) & 0x0F] << hexChars[b & 0x0F];
    }
    return s.str();
}

PiPedalModel::PiPedalModel()
    : pluginHost(),
      atomConverter(pluginHost.GetMapFeature())
{
    this->updater = Updater::Create();
    this->currentUpdateStatus = updater->GetCurrentStatus();
    this->pedalboard = Pedalboard::MakeDefault();

    this->pedalboardBuilder = std::make_unique<PedalboardBuilder>(
        [this](PedalboardBuildRequest &request)
        {
            return this->BuildPedalboard(request);
        },
        [this](uint64_t generation, PedalboardBuildRequest &request, PedalboardBuildResult &result)
        {
            this->InstallBuiltPedalboard(generation, request, result);
        },
        [this](const std::exception &e)
        {
            this->OnPedalboardBuildFailed(e.what());
        });

    this->jackServerSettings = this->storage.GetJackServerSettings();

    updater->SetUpdateListener(
        [this](const UpdateStatus &updateStatus)
        {
            this->OnUpdateStatusChanged(updateStatus);
        });

    DbusLogToLv2Log();
    SetDBusLogLevel(DBusLogLevel::Info);

    hotspotManager = HotspotManager::Create();
    hotspotManager->SetNetworkChangingListener(
        [this](bool ethernetConnected, bool hotspotEnabling)
        {
            OnNetworkChanging(ethernetConnected, hotspotEnabling);
        });
    hotspotManager->SetHasWifiListener(
        [this](bool hasWifi)
        {
            this->SetHasWifi(hasWifi);
        });
    // don't actuall start the hotspotManager until after LV2 is initialized (in order to avoid logging oddities)
}

void PrepareSnapshostsForSave(Pedalboard &pedalboard)
{
    if (pedalboard.selectedSnapshot() != -1)
    {
        auto &currentSnapshot = pedalboard.snapshots()[pedalboard.selectedSnapshot()];
        if (!currentSnapshot || currentSnapshot->isModified_)
        {
            pedalboard.selectedSnapshot(-1);
        }
    }
    for (auto &snapshot : pedalboard.snapshots())
    {
        if (snapshot)
        {
            snapshot->isModified_ = false;
        }
    }
}

void PiPedalModel::Close()
{
    // Unlocked: the builder thread takes `mutex` to install, so it must be joined without holding it.
    // Done first, while the audio host is still running, so that an in-flight build that borrowed live
    // effects can still be installed. No pedalboard build runs or installs after this point.
    ClosePedalboardBuilder();

    std::unique_ptr<AudioHost> oldAudioHost;
    std::shared_ptr<Tone3000Auth> oldTone3000Auth;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex);

        if (closed)
        {
            return;
        }
        if (tone3000Downloader)
        {
            tone3000Downloader->Close();
        }
        oldTone3000Auth = this->tone3000Auth;
        closed = true;

        // No build is installed after ClosePedalboardBuilder(): release acks still waiting for one.
        SendMidiAcks(pendingMidiAcks.TakeAll());
        FailDeferredPatchRequests("Shutting down.");

        CancelAudioRetry();

        if (avahiService)
        {
            this->avahiService = nullptr; // and close.
        }

        // take a snapshot incase a client unsusbscribes in the notification handler (in which case the mutex won't protect us)
        std::vector<IPiPedalModelSubscriber::ptr> t{subscribers.begin(), subscribers.end()};
        for (auto &subscriber : t)
        {
            subscriber->Close();
        }
        this->subscribers.resize(0);

        oldAudioHost = std::move(this->audioHost);
    } // end lock.

    // lockless to avoid deadlocks while shutting down the audio thread.
    if (oldAudioHost)
    {
        oldAudioHost->Close();
    }
    // lockless: a poll thread may be waiting on `mutex` to deliver a status update.
    if (oldTone3000Auth)
    {
        oldTone3000Auth->Close();
    }
}

std::shared_ptr<Tone3000Auth> PiPedalModel::GetTone3000Auth()
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return tone3000Auth;
}

void PiPedalModel::OnTone3000AuthStatusChanged(const Tone3000AuthStatus &status)
{
    SubscriberList subscribers;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        if (closed)
        {
            return;
        }
        subscribers = this->subscribers;
    }
    for (auto &subscriber : subscribers)
    {
        subscriber->OnTone3000AuthStatusChanged(status);
    }
}

void PiPedalModel::ClosePedalboardBuilder()
{
    if (pedalboardBuilder)
    {
        pedalboardBuilder->Close();
    }
}

PiPedalModel::~PiPedalModel()
{
    ClosePedalboardBuilder(); // before any member the installer uses is destroyed.
    CancelNetworkChangingTimer();
    try
    {
        std::lock_guard<std::recursive_mutex> guard{mutex};
        if (autosaveHandle)
        {
            CancelPost(autosaveHandle);
            autosaveHandle = 0;
        }
    }
    catch (...)
    {
    }
    hotspotManager = nullptr; // turn off the hotspot.
    if (tone3000Auth)
    {
        tone3000Auth->Close(); // drops the listener that points at us.
    }

    pluginChangeMonitor = nullptr; // stop monitorin LV2 directories.
    try
    {
        adminClient.UnmonitorGovernor();
    }
    catch (...) // noexcept here!
    {
    }

    try
    {
        CurrentPreset currentPreset;
        currentPreset.modified_ = this->hasPresetChanged;
        currentPreset.preset_ = this->pedalboard;
        storage.SaveCurrentPreset(currentPreset);
    }
    catch (...)
    {
    }

    try
    {
        if (audioHost)
        {
            audioHost->Close();
        }
    }
    catch (...)
    {
    }
}

#include <fstream>

void PiPedalModel::Init(const PiPedalConfiguration &configuration)
{
    std::lock_guard<std::recursive_mutex> lock(mutex); // prevent callbacks while we're initializing.
    if (updaterEnabled)
    {
        this->updater->Start();
    }

    this->configuration = configuration;
    pluginHost.SetConfiguration(configuration);
    storage.SetConfigRoot(configuration.GetDocRoot());
    storage.SetDataRoot(configuration.GetLocalStoragePath());
    storage.Initialize(this);
    pluginHost.SetPluginStoragePath(storage.GetPluginUploadDirectory());

    // Tokens live in the daemon's private config directory (file mode 0600).
    this->tone3000Auth = Tone3000Auth::Create(storage.GetDataRoot() / "config" / "tone3000_auth.json");
    this->tone3000Auth->SetStatusListener(
        [this](const Tone3000AuthStatus &status)
        {
            OnTone3000AuthStatusChanged(status);
        });

    this->systemMidiBindings = storage.GetSystemMidiBindings();

    this->jackServerSettings = storage.GetJackServerSettings();
    try
    {
        this->jackConfiguration.AlsaInitialize(jackServerSettings);
    }
    catch (const std::exception &)
    {
        //
    }

    this->channelRouterSettings = storage.GetChannelRouterSettings();
    pluginHost.OnConfigurationChanged(
        jackConfiguration,
        storage.GetChannelSelection());
}

int64_t PiPedalModel::DownloadModelsFromTone3000(
    const std::string &uri,
    const Tone3000PkceParams &pckeParams,
    const std::string &downloadPath,
    Tone3000DownloadType downloadType)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);

    // Validate the download path
    std::filesystem::path path{downloadPath};
    if (!IsInUploadsDirectory(path))
    {
        throw PiPedalException("Invalid path: not in uploads directory.");
    }

    // Check for ".." in path components (security check)
    for (const auto &component : path)
    {
        if (component == "..")
        {
            throw PiPedalException("Invalid path: contains '..' component.");
        }
    }

    if (!tone3000Downloader)
    {
        tone3000Downloader = Tone3000Downloader::Create();
        tone3000Downloader->SetListener(this);
    }
    return tone3000Downloader->RequestTone3000Download(
        uri,
        pckeParams,
        downloadPath,
        downloadType);
}

void PiPedalModel::CancelTone3000Download(
    int64_t clientId,
    int64_t downloadHandle)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);

    if (tone3000Downloader)
    {
        tone3000Downloader->CancelDownload(downloadHandle);
    }
}

// Tone3000Downloader::Listener implementation
void PiPedalModel::OnStartTone3000Download(int64_t handle, const std::string &title)
{

    SubscriberList subscribers;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        subscribers = this->subscribers;
    }
    for (auto &subscriber : subscribers)
    {
        subscriber->OnTone3000DownloadStarted(handle, title);
    }
}

void PiPedalModel::OnTone3000Progress(const Tone3000DownloadProgress &progress)
{
    SubscriberList subscribers;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        subscribers = this->subscribers;
    }
    for (auto &subscriber : subscribers)
    {
        subscriber->OnTone3000DownloadProgress(progress);
    }
}

void PiPedalModel::OnTone3000DownloadComplete(int64_t handle, const std::string &resultPath)
{
    SubscriberList subscribers;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        subscribers = this->subscribers;
    }
    for (auto subscriber : subscribers)
    {
        subscriber->OnTone3000DownloadComplete(handle, resultPath);
    }
}

void PiPedalModel::OnTone3000DownloadError(int64_t handle, const std::string &errorMessage)
{
    SubscriberList subscribers;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        subscribers = this->subscribers;
    }
    for (auto subscriber : subscribers)
    {
        subscriber->OnTone3000DownloadError(handle, errorMessage);
    }
}

void PiPedalModel::LoadLv2PluginInfo()
{
    // Not all Lv2 directories have the lv2 base declarations. Load a full set of plugin classes generated on a default /usr/local/lib/lv2 directory.
    std::filesystem::path pluginClassesPath = configuration.GetDocRoot() / "plugin_classes.json";
    try
    {
        if (!std::filesystem::exists(pluginClassesPath))
            throw PiPedalException("File not found.");
        pluginHost.LoadPluginClassesFromJson(pluginClassesPath);
    }
    catch (const std::exception &e)
    {
        std::stringstream s;
        s << "Unable to load " << pluginClassesPath << ". " << e.what();
        throw PiPedalException(s.str().c_str());
    }

    pluginChangeMonitor = std::make_unique<Lv2PluginChangeMonitor>(*this);
    pluginHost.LoadLilv(configuration.GetLv2Path().c_str());

    // Copy all presets out of Lilv data to json files
    // so that we can close lilv while we're actually
    // running.

    uint64_t pluginPresetIndexVersion = storage.GetPluginPresetIndexVersion();
    for (const auto &plugin : pluginHost.GetPlugins())
    {
        if (plugin->has_factory_presets())
        {
            if (!storage.HasPluginPresets(plugin->uri()))
            {
                PluginPresets pluginPresets = pluginHost.GetFactoryPluginPresets(plugin->uri());
                storage.SavePluginPresets(plugin->uri(), pluginPresets);
            }
            else
            {
                if (pluginPresetIndexVersion == 0)
                {
                    if (plugin->uri() == "http://two-play.com/plugins/toob-convolution-reverb" || plugin->uri() == "http://two-play.com/plugins/toob-convolution-reverb-stereo")
                    {
                        // overwrite previous factory presets!
                        PluginPresets pluginPresets = pluginHost.GetFactoryPluginPresets(plugin->uri());
                        for (auto &pluginPreset : pluginPresets.presets_)
                        {
                            storage.SavePluginPreset(
                                plugin->uri(),
                                pluginPreset);
                        }
                    }
                }
            }
        }
    }
    storage.SetPluginPresetIndexVersion(1);
}
void PiPedalModel::Load()
{
    this->webRoot = configuration.GetWebRoot();
    this->webPort = (uint16_t)configuration.GetSocketServerPort();

    {
        // A persisted governor that this kernel doesn't offer (e.g. settings copied from another
        // device) would be retried by the monitor thread forever and shown in the UI as if it
        // were in effect. Replace it with the governor actually running, and persist that.
        std::string persistedGovernor = storage.GetGovernorSettings();
        bool governorsFromSysfs = false;
        std::vector<std::string> availableGovernors = pipedal::GetAvailableGovernors(&governorsFromSysfs);
        std::string governor = pipedal::ResolvePersistedGovernor(
            persistedGovernor, availableGovernors, pipedal::GetCpuGovernor(), governorsFromSysfs);
        if (governor != persistedGovernor)
        {
            Lv2Log::warning(SS("CPU governor '" << persistedGovernor << "' is not available. Using '" << governor << "'."));
            try
            {
                storage.SetGovernorSettings(governor);
            }
            catch (const std::exception &e)
            {
                Lv2Log::warning(SS("Unable to save CPU governor setting. " << e.what()));
            }
        }
        adminClient.MonitorGovernor(governor);
    }

    // pluginHost.Load(configuration.GetLv2Path().c_str());

    this->pedalboard = storage.GetCurrentPreset(); // the current *saved* preset.

    // the current edited preset, saved only across orderly shutdowns.

    CrashGuard::SetCrashGuardFileName(storage.GetDataRoot() / "crash_guard.data");

    if (CrashGuard::HasCrashed())
    {
        // ignore the current preset, and load a blank pedalboard in order to avoid a potential plugin crash.
        this->pedalboard = Pedalboard::MakeDefault();
    }
    else
    {
        CurrentPreset currentPreset;
        try
        {
            if (storage.RestoreCurrentPreset(&currentPreset))
            {
                this->pedalboard = currentPreset.preset_;
                this->UpdateDefaults(&pedalboard);
                this->hasPresetChanged = currentPreset.modified_;
            }
        }
        catch (const std::exception &e)
        {
            Lv2Log::warning(SS("Failed to load current preset. " << e.what()));
        }
    }

    UpdateDefaults(&this->pedalboard);

    std::unique_ptr<AudioHost> p{AudioHost::CreateInstance(pluginHost.asIHost())};
    this->audioHost = std::move(p);

    this->audioHost->SetNotificationCallbacks(this);

    this->systemMidiBindings = storage.GetSystemMidiBindings();

    this->audioHost->SetSystemMidiBindings(this->systemMidiBindings);

    audioHost->SetAlsaSequencerConfiguration(storage.GetAlsaSequencerConfiguration());

    this->audioHost->SetSuspendBypassedPlugins(storage.GetSuspendBypassedPlugins());

    if (configuration.GetMLock())
    {
#ifndef NO_MLOCK
        int result = mlockall(MCL_CURRENT | MCL_FUTURE);
        if (result)
        {
            throw PiPedalStateException("mlockall failed. You can disable the call to mlockall  in 'config.json'.");
        }

#endif
    }

    RestartAudio();
}

IPiPedalModelSubscriber *PiPedalModel::GetNotificationSubscriber(int64_t clientId)
{
    for (size_t i = 0; i < subscribers.size(); ++i)
    {
        if (subscribers[i]->GetClientId() == clientId)
        {
            return subscribers[i].get();
        }
    }
    return nullptr;
}

void PiPedalModel::AddNotificationSubscription(std::shared_ptr<IPiPedalModelSubscriber> pSubscriber)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    this->subscribers.push_back(pSubscriber);
}
void PiPedalModel::RemoveNotificationSubsription(std::shared_ptr<IPiPedalModelSubscriber> pSubscriber)
{
    {
        std::lock_guard<std::recursive_mutex> lock(mutex);

        for (auto it = this->subscribers.begin(); it != this->subscribers.end(); ++it)
        {
            if ((*it).get() == pSubscriber.get())
            {
                this->subscribers.erase(it);
                break;
            }
        }
        int64_t clientId = pSubscriber->GetClientId();

        this->DeleteMidiListeners(clientId);
        this->DeleteAtomOutputListeners(clientId);

        for (int i = 0; i < this->outstandingParameterRequests.size(); ++i)
        {
            if (outstandingParameterRequests[i]->clientId == clientId)
            {
                outstandingParameterRequests.erase(outstandingParameterRequests.begin() + i);
                --i;
            }
        }
        // likewise for requests waiting for a pedalboard build: their callbacks refer to the closed connection.
        deferredPatchRequests.RemoveClient(clientId);
    }
}

void PiPedalModel::PreviewControl(int64_t clientId, int64_t pedalItemId, const std::string &symbol, float value)
{
    // Called without `mutex` from the websocket (PiPedalSocket). The builder thread and RestartAudio replace/reset
    // this->lv2Pedalboard (and the audio host's pedalboard) under `mutex`, so the check, the single read of
    // this->lv2Pedalboard and the dispatch to the audio host (which resolves the instance id against its own
    // copy of the installed pedalboard) all happen under `mutex`: otherwise a pedalboard installed in between
    // could receive a value addressed to an instance id of the previous one.
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (!RunningInstanceIdsMatchPedalboard())
    {
        // A full build (preset/bank switch) is outstanding: the running pedalboard is the outgoing one,
        // whose instance ids may name unrelated plugins. The installer applies the model's current
        // settings when the new pedalboard is installed.
        return;
    }
    const std::shared_ptr<Lv2Pedalboard> lv2Pedalboard = this->lv2Pedalboard;
    if (!lv2Pedalboard || !audioHost)
    {
        return; // still building. The installer applies current settings when the build is installed.
    }
    IEffect *effect = lv2Pedalboard->GetEffect(pedalItemId);
    if (!effect)
    {
        return;
    }
    if (effect->IsVst3())
    {
        int index = lv2Pedalboard->GetControlIndex(pedalItemId, symbol);
        if (index != -1)
        {
            effect->SetControl(index, value);
        }
    }
    else
    {
        audioHost->SetControlValue(pedalItemId, symbol, value);
    }
}

// we were explicitly told that the state was changed by the plugin
void PiPedalModel::OnNotifyLv2StateChanged(uint64_t instanceId)
{
    // a sent PATCH_Set, or an explicit state changed notification.
    OnNotifyMaybeLv2StateChanged(instanceId);
}

// Called with `mutex` held. Whether the instance ids of the running pedalboard are those of this->pedalboard.
// While a full build (preset/bank switch) is outstanding, the running pedalboard is the outgoing one, and its
// instance ids may name unrelated items of this->pedalboard (ids collide across presets), so plugin state and
// path property notifications from it must not be stored into this->pedalboard.
bool PiPedalModel::RunningInstanceIdsMatchPedalboard() const
{
    return pedalboardBuilder && !pedalboardBuilder->modeTracker.FullBuildOutstanding();
}

// The plugin notified us that a  path path property changed. The state *purrobably changed.
bool PiPedalModel::OnNotifyMaybeLv2StateChanged(uint64_t instanceId)
{
    // one or more received PATCH_Sets, which MAY change the state.
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (!RunningInstanceIdsMatchPedalboard())
    {
        // the notification is from an instance of the outgoing pedalboard (e.g. during a preset switch).
        return false;
    }
    PedalboardItem *item = pedalboard.GetItem(instanceId);
    if (item != nullptr)
    {
        if (!audioHost)
        {
            return false;
        }
        bool changed = this->audioHost->UpdatePluginState(*item);
        if (changed)
        {

            item->stateUpdateCount(item->stateUpdateCount() + 1);

            Lv2PluginState newState = item->lv2State();

            FireLv2StateChanged(instanceId, newState);
            this->SetPresetChanged(-1, true, false);
            return true;
        }
    }
    return false;
}

void PiPedalModel::SetInputVolume(float value)
{
    PreviewInputVolume(value);
    {
        SubscriberList subscribers;
        {
            std::lock_guard<std::recursive_mutex> lock(mutex);
            subscribers = this->subscribers;

            this->pedalboard.input_volume_db(value);
        }
        // take a snapshot incase a client unsusbscribes in the notification handler (in which case the mutex won't protect us)
        for (auto &subscriber : subscribers)
        {
            subscriber->OnInputVolumeChanged(value);
        }

        this->SetPresetChanged(-1, true);
    }
}
void PiPedalModel::SetOutputVolume(float value)
{
    PreviewOutputVolume(value);
    {
        SubscriberList subscribers;
        {
            std::lock_guard<std::recursive_mutex> lock(mutex);
            subscribers = this->subscribers;

            this->pedalboard.output_volume_db(value);
        }
        for (auto &subscriber : subscribers)
        {
            subscriber->OnOutputVolumeChanged(value);
        }

        this->SetPresetChanged(-1, true);
    }
}
void PiPedalModel::PreviewInputVolume(float value)
{
    audioHost->SetInputVolume(value);
}
void PiPedalModel::PreviewOutputVolume(float value)
{
    audioHost->SetOutputVolume(value);
}

void PiPedalModel::SetControl(int64_t clientId, int64_t pedalItemId, const std::string &symbol, float value)
{
    SubscriberList subscribers;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        subscribers = this->subscribers;

        if (!this->pedalboard.SetControlValue(pedalItemId, symbol, value))
        {
            return;
        }

        PedalboardItem *item = pedalboard.GetItem(pedalItemId);

        // change of split type requires rebuild of the effect
        // since it can change the number of output channels.
        if (item != nullptr && item->isSplit() && symbol == "splitType")
        {
            this->FirePedalboardChanged(clientId);
            return;
        }
        PreviewControl(clientId, pedalItemId, symbol, value);
    }

    for (auto &subscriber : subscribers)
    {
        subscriber->OnControlChanged(clientId, pedalItemId, symbol, value);
    }

    this->SetPresetChanged(clientId, true);
}

void PiPedalModel::FireJackConfigurationChanged(const JackConfiguration &jackConfiguration)
{

    SubscriberList subscribers;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        subscribers = this->subscribers;
    }

    // noify subscribers.

    for (auto &subscriber : subscribers)
    {
        subscriber->OnJackConfigurationChanged(jackConfiguration);
    }
}

void PiPedalModel::FireBanksChanged(int64_t clientId)
{
    SubscriberList subscribers;
    BankIndex banksSnapshot;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        subscribers = this->subscribers;
        banksSnapshot = this->storage.GetBanks(); // copy under lock; subscribers run unlocked.
    }
    // noify subscribers.
    for (auto &subscriber : subscribers)
    {
        subscriber->OnBankIndexChanged(banksSnapshot);
    }
}

void PiPedalModel::FirePedalboardChanged(int64_t clientId, bool loadAudioThread)
{
    SubscriberList subscribers;
    Pedalboard pedalboardSnapshot;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        subscribers = this->subscribers;

        if (loadAudioThread)
        {
            // notify the audio thread.
            if (audioHost && audioHost->IsOpen())
            {
                LoadCurrentPedalboard();

                UpdateRealtimeVuSubscriptions();
                UpdateRealtimeMonitorPortSubscriptions();
            }
        }
        // one copy under lock; subscribers run unlocked and must not see later mutations.
        pedalboardSnapshot = this->pedalboard;
    }
    // noify subscribers.
    for (auto &subscriber : subscribers)
    {
        subscriber->OnPedalboardChanged(clientId, pedalboardSnapshot);
    }
}
void PiPedalModel::SetPedalboard(int64_t clientId, Pedalboard &pedalboard)
{
    {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        this->pedalboard = pedalboard;
        UpdateDefaults(&this->pedalboard);
    }
    this->FirePedalboardChanged(clientId);
    this->SetPresetChanged(clientId, true);
}

void PiPedalModel::SetSnapshot(int64_t selectedSnapshot)
{
    bool pedalboardChanged = false;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        if (this->pedalboard.ApplySnapshot(selectedSnapshot, pluginHost))
        {
            this->pedalboard.selectedSnapshot(selectedSnapshot);
            for (auto snapshot : this->pedalboard.snapshots())
            {
                if (snapshot)
                {
                    snapshot->isModified_ = false;
                }
            }
            pedalboardChanged = true;
        } else if (selectedSnapshot == -1) 
        {
            this->pedalboard.selectedSnapshot(-1);
            pedalboardChanged = true;
        }

    }
    if (pedalboardChanged)
    {
        FirePedalboardChanged(-1, true);
    }
}

void PiPedalModel::SetSnapshots(std::vector<std::shared_ptr<Snapshot>> &snapshots, int64_t selectedSnapshot)
{
    {
        std::lock_guard<std::recursive_mutex> lock(mutex);

        UpdateVst3Settings(pedalboard);

        this->pedalboard.snapshots(std::move(snapshots));

        if (selectedSnapshot != -1)
        {
            this->pedalboard.selectedSnapshot(selectedSnapshot);
            for (auto &snapshot : pedalboard.snapshots())
            {
                if (snapshot)
                {
                    snapshot->isModified_ = false;
                }
            }
        }
    }

    this->FirePedalboardChanged(-1, false); // notify clients (but don't change the running pedalboard, because it's still the same)
                                            // this means that all clients get an up-to-date copy of the snapshots AND the currently selected snapshot if that applies
                                            // (and a fresh copy of the pedalboard settings as well, which is harmless, since they have not changed)
    this->SetPresetChanged(-1, true, false);
}

void PiPedalModel::UpdateCurrentPedalboard(int64_t clientId, Pedalboard &pedalboard)
{
    {
        std::lock_guard<std::recursive_mutex> lock(mutex);

        // update vst3 presets if neccessary.
        // the pedalboard must be a manipualted instance of the current Lv2Pedalboard.

        UpdateVst3Settings(pedalboard);

        this->pedalboard = pedalboard;

        // Rebuild on the builder thread, borrowing existing Lv2 effect instances from the running pedalboard.
        // The installer updates previousPedalboard and the realtime VU/monitor subscriptions.
        RequestPedalboardBuild(true);
    }
    this->FirePedalboardChanged(clientId, false);
    this->SetPresetChanged(clientId, true);
}

void PiPedalModel::SetPedalboardItemUseModUi(int64_t clientId, int64_t instanceId, bool enabled)
{
    std::lock_guard<std::recursive_mutex> guard{mutex};
    {
        this->pedalboard.SetItemUseModUi(instanceId, enabled);

        // Notify clients.
        std::vector<IPiPedalModelSubscriber::ptr> t{subscribers.begin(), subscribers.end()};
        for (auto &subscriber : t)
        {
            subscriber->OnItemUseModUiChanged(clientId, instanceId, enabled);
        }
    }
    this->SetPresetChanged(clientId, true);
}

void PiPedalModel::SetPedalboardItemEnable(int64_t clientId, int64_t pedalItemId, bool enabled)
{
    SubscriberList subscribers;
    std::lock_guard<std::recursive_mutex> guard{mutex};
    // While a full build is outstanding, the running pedalboard's instance ids may name unrelated plugins;
    // the installer applies the model's enabled state when the new pedalboard is installed.
    bool updateAudioHost = RunningInstanceIdsMatchPedalboard();
    {
        subscribers = this->subscribers;

        this->pedalboard.SetItemEnabled(pedalItemId, enabled);
        PedalboardItem *pPedalboardItem = this->pedalboard.GetItem(pedalItemId);
        if (pPedalboardItem)
        {
            Lv2PluginInfo::ptr pluginInfo = GetPluginInfo(pPedalboardItem->uri());
            if (pluginInfo)
            {
                for (auto &port : pluginInfo->ports())
                {
                    if (port->is_bypass())
                    {
                        pPedalboardItem->SetControlValue(port->symbol(), enabled ? 1.0 : 0.0f);
                    }
                }
            }
        }
        // Notify audo thread.
    }
    if (updateAudioHost)
    {
        this->audioHost->SetBypass(pedalItemId, enabled);
    }

    // Notify clients.
    for (auto &subscriber : subscribers)
    {
        subscriber->OnItemEnabledChanged(clientId, pedalItemId, enabled);
    }
    this->SetPresetChanged(clientId, true);
}

void PiPedalModel::GetPresets(PresetIndex *pResult)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);

    this->storage.GetPresetIndex(pResult);
    pResult->presetChanged(this->hasPresetChanged);
}

Pedalboard PiPedalModel::GetPreset(int64_t instanceId)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return this->storage.GetPreset(instanceId);
}
void PiPedalModel::GetBank(int64_t instanceId, BankFile *pResult)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    this->storage.GetBankFile(instanceId, pResult);
}

void PiPedalModel::SetPresetChanged(int64_t clientId, bool value, bool changeSnapshotSelect)
{
    if (changeSnapshotSelect && value && this->pedalboard.selectedSnapshot() != -1)
    {
        auto &snapshot = this->pedalboard.snapshots()[pedalboard.selectedSnapshot()];
        if (snapshot)
        {
            if (!snapshot->isModified_)
            {
                snapshot->isModified_ = true;
                FireSnapshotModified(pedalboard.selectedSnapshot(), true);
            }
        }

        this->pedalboard.SetCurrentSnapshotModified(true);
    }
    if (value != this->hasPresetChanged)
    {
        hasPresetChanged = value;
        if (!value)
        {
            storage.DiscardCurrentPreset(); // saved: the autosave is no longer meaningful.
        }
        FirePresetChanged(value);
    }
    if (value)
    {
        ScheduleCurrentPresetAutosave();
    }
}

static const std::chrono::seconds AUTOSAVE_DELAY{3};

void PiPedalModel::ScheduleCurrentPresetAutosave()
{
    // Coalescing: record the time of the last change; a single pending timer re-arms itself
    // until 3 seconds have passed with no further change.
    std::lock_guard<std::recursive_mutex> guard{mutex};
    autosaveLastChange = clock::now();
    if (autosaveHandle == 0)
    {
        try
        {
            autosaveHandle = PostDelayed(AUTOSAVE_DELAY, [this]()
                                         { OnAutosaveTimer(); });
        }
        catch (const std::exception &)
        {
            // dispatcher not ready yet. The shutdown save still applies.
        }
    }
}

void PiPedalModel::OnAutosaveTimer()
{
    CurrentPreset currentPreset;
    {
        std::lock_guard<std::recursive_mutex> guard{mutex};
        auto elapsed = clock::now() - autosaveLastChange;
        if (elapsed < AUTOSAVE_DELAY)
        {
            try
            {
                autosaveHandle = PostDelayed(AUTOSAVE_DELAY - elapsed, [this]()
                                             { OnAutosaveTimer(); });
                return;
            }
            catch (const std::exception &)
            {
            }
        }
        autosaveHandle = 0;
        if (!hasPresetChanged)
        {
            storage.DiscardCurrentPreset();
            return; // nothing to save.
        }
        // serialize a snapshot under the lock; write to disk outside it.
        currentPreset.modified_ = true;
        currentPreset.preset_ = this->pedalboard;
    }
    storage.SaveCurrentPreset(currentPreset);
    {
        // the preset may have been saved or replaced while we were writing.
        std::lock_guard<std::recursive_mutex> guard{mutex};
        if (!hasPresetChanged)
        {
            storage.DiscardCurrentPreset();
        }
    }
}

void PiPedalModel::FireSnapshotModified(int64_t snapshotIndex, bool modified)
{
    SubscriberList subscribers;
    std::lock_guard<std::recursive_mutex> guard{mutex};
    {
        subscribers = this->subscribers;
    }
    for (auto &subscriber : subscribers)
    {
        subscriber->OnSnapshotModified(snapshotIndex, modified);
    }
}

void PiPedalModel::FireSelectedSnapshotChanged(int64_t selectedSnapshot)
{
    SubscriberList subscribers;
    std::lock_guard<std::recursive_mutex> guard{mutex};
    {
        subscribers = this->subscribers;
        // take a snapshot incase a client unsusbscribes in the notification handler (in which case the mutex won't protect us)
    }
    for (auto &subscriber : subscribers)
    {
        subscriber->OnSelectedSnapshotChanged(selectedSnapshot);
    }
}

void PiPedalModel::FirePresetChanged(bool changed)
{
    SubscriberList subscribers;
    PresetIndex presets;
    std::lock_guard<std::recursive_mutex> guard{mutex};
    {
        subscribers = this->subscribers;
        // take a snapshot incase a client unsusbscribes in the notification handler (in which case the mutex won't protect us)

        GetPresets(&presets);
    }
    for (auto &subscriber : subscribers)
    {
        subscriber->OnPresetChanged(changed);
    }
}

void PiPedalModel::FirePresetsChanged(int64_t clientId)
{
    SubscriberList subscribers;
    PresetIndex presets;
    std::lock_guard<std::recursive_mutex> guard{mutex};
    {
        subscribers = this->subscribers;
        GetPresets(&presets);
    }
    for (auto &subscriber : subscribers)
    {
        subscriber->OnPresetsChanged(clientId, presets);
    }
}
void PiPedalModel::FirePluginPresetsChanged(const std::string &pluginUri)
{
    SubscriberList subscribers;
    std::lock_guard<std::recursive_mutex> guard{mutex};
    {
        subscribers = this->subscribers;
        // take a snapshot incase a client unsusbscribes in the notification handler (in which case the mutex won't protect us)
    }
    for (auto &subscriber : subscribers)
    {
        subscriber->OnPluginPresetsChanged(pluginUri);
    }
}

void PiPedalModel::UpdateVst3Settings(Pedalboard &pedalboard)
{
    // get the vst3 state bundle from lv2Pedalboard for the current pedalboard.
#if ENABLE_VST3
    if (!lv2Pedalboard || pedalboardBuilder->modeTracker.FullBuildOutstanding())
    {
        // The running pedalboard is not of the current pedalboard's lineage (instance ids don't match). The
        // outstanding build creates its VST3 instances from this->pedalboard, so that is where their current
        // state is until it is installed.
        if (&pedalboard != &this->pedalboard)
        {
            MergeVst3State(pedalboard, this->pedalboard);
        }
        return;
    }
    Pedalboard pb;
    for (IEffect *effect : lv2Pedalboard->GetEffects())
    {
        if (effect->IsVst3())
        {
            PedalboardItem *item = pedalboard.GetItem(effect->GetInstanceId());
            if (item)
            {
                Vst3Effect *vst3Effect = (Vst3Effect *)effect;
                std::vector<uint8_t> state;
                if (vst3Effect->GetState(&state))
                {
                    item->vstState(BytesToHex(state));
                }
            }
        }
    }
#endif
}

void PiPedalModel::FireLv2StateChanged(int64_t instanceId, const Lv2PluginState &lv2State)
{
    SubscriberList subscribers;
    {
        std::lock_guard<std::recursive_mutex> guard{mutex};
        subscribers = this->subscribers;
    }

    for (auto &subscriber : subscribers)
    {
        subscriber->OnLv2StateChanged(instanceId, lv2State);
    }
}

// referesh the plugin state for all plugins.
bool PiPedalModel::SyncLv2State()
{
    std::lock_guard<std::recursive_mutex> guard{mutex};
    bool changed = false;
    if (!audioHost || !lv2Pedalboard || !RunningInstanceIdsMatchPedalboard())
    {
        // A full build is outstanding: the running pedalboard's instance ids may name unrelated plugins, whose
        // state must not be stored into this->pedalboard. Its items' state is the state the outstanding build
        // was created from, so it is already current.
        return false;
    }
    auto pedalboardItems = pedalboard.GetAllPlugins();
    for (PedalboardItem *item : pedalboardItems)
    {
        if (!item->isSplit())
        {
            if (audioHost->UpdatePluginState(*item))
            {
                FireLv2StateChanged(item->instanceId(), item->lv2State());
                changed = true;
            }
        }
    }
    return changed;
}
void PiPedalModel::SaveCurrentPreset(int64_t clientId)
{
    std::lock_guard<std::recursive_mutex> guard{mutex};

    UpdateVst3Settings(this->pedalboard);
    SyncLv2State();
    PrepareSnapshostsForSave(pedalboard);

    storage.SaveCurrentPreset(this->pedalboard);
    this->SetPresetChanged(clientId, false);
}

uint64_t PiPedalModel::CopyPluginPreset(const std::string &pluginUri, uint64_t presetId)
{
    uint64_t result = storage.CopyPluginPreset(pluginUri, presetId);
    FirePluginPresetsChanged(pluginUri);
    return result;
}

void PiPedalModel::UpdatePluginPresets(const PluginUiPresets &pluginPresets)
{
    storage.UpdatePluginPresets(pluginPresets);
    FirePluginPresetsChanged(pluginPresets.pluginUri_);
}
int64_t PiPedalModel::SavePluginPresetAs(int64_t instanceId, const std::string &name)
{
    PedalboardItem *item = this->pedalboard.GetItem(instanceId);
    if (!item)
    {
        throw PiPedalException("Plugin not found.");
    }
    uint64_t presetId = storage.SavePluginPreset(name, *item);
    FirePluginPresetsChanged(item->uri());
    return presetId;
}

int64_t PiPedalModel::SaveCurrentPresetAs(int64_t clientId, int64_t bankInstanceId, const std::string &name, int64_t saveAfterInstanceId)
{
    std::lock_guard<std::recursive_mutex> guard{mutex};

    SyncLv2State();
    auto pedalboard = this->pedalboard.DeepCopy();
    PrepareSnapshostsForSave(pedalboard);

    UpdateVst3Settings(pedalboard);
    pedalboard.name(name);
    int64_t result = storage.SaveCurrentPresetAs(pedalboard, bankInstanceId, name, saveAfterInstanceId);
    FirePresetsChanged(clientId);
    return result;
}

void PiPedalModel::UploadPluginPresets(const PluginPresets &pluginPresets)
{
    if (pluginPresets.pluginUri_.length() == 0)
    {
        throw PiPedalException("Invalid plugin presets.");
    }
    std::lock_guard<std::recursive_mutex> guard{mutex};
    storage.MergePluginPresets(pluginPresets.pluginUri_, pluginPresets);
    FirePluginPresetsChanged(pluginPresets.pluginUri_);
}
int64_t PiPedalModel::UploadPreset(const BankFile &bankFile, int64_t uploadAfter)
{
    std::lock_guard<std::recursive_mutex> guard{mutex};

    int64_t newPreset = this->storage.UploadPreset(bankFile, uploadAfter);
    FirePresetsChanged(-1);
    return newPreset;
}
int64_t PiPedalModel::UploadBank(BankFile &bankFile, int64_t uploadAfter)
{
    std::lock_guard<std::recursive_mutex> guard{mutex};

    int64_t newPreset = this->storage.UploadBank(bankFile, uploadAfter);
    FireBanksChanged(-1);
    return newPreset;
}

void PiPedalModel::NextBank(Direction direction)
{
    std::lock_guard<std::recursive_mutex> guard{mutex};

    auto bankIndex = this->GetBankIndex();
    if (bankIndex.entries().size() == 0)
    {
        return;
    }
    size_t index = 0;
    for (size_t i = 0; i < bankIndex.entries().size(); ++i)
    {
        auto &entry = bankIndex.entries()[i];
        if (entry.instanceId() == bankIndex.selectedBank())
        {
            index = i;
            break;
        }
    }
    if (direction == Direction::Increase)
    {
        ++index;
        if (index >= bankIndex.entries().size())
        {
            index = 0;
        }
    }
    else
    {
        if (index == 0)
        {
            index = bankIndex.entries().size() - 1;
        }
        else
        {
            --index;
        }
    }
    this->OpenBank(-1, bankIndex.entries()[index].instanceId());
}
void PiPedalModel::NextPreset(Direction direction)
{
    PresetIndex index;

    storage.GetPresetIndex(&index);
    auto currentPresetId = storage.GetCurrentPresetId();
    size_t currentPresetIndex = 0;

    for (size_t i = 0; i < index.presets().size(); ++i)
    {
        if (index.presets()[i].instanceId() == currentPresetId)
        {
            currentPresetIndex = i;
            break;
        }
    }
    if (index.presets().size() == 0)
    {
        return;
    }
    if (direction == Direction::Decrease)
    {
        if (currentPresetIndex == 0)
        {
            currentPresetIndex = index.presets().size() - 1;
        }
        else
        {
            --currentPresetIndex;
        }
    }
    else
    {
        ++currentPresetIndex;
        if (currentPresetIndex >= index.presets().size())
        {
            currentPresetIndex = 0;
        }
    }
    LoadPreset(-1, index.presets()[currentPresetIndex].instanceId());
}

void PiPedalModel::NextSnapshot(Direction direction)
{
    std::lock_guard<std::recursive_mutex> guard{mutex};

    auto &snapshots = this->pedalboard.snapshots();
    if (snapshots.size() == 0)
    {
        return;
    }
    int64_t currentSnapshot = this->pedalboard.selectedSnapshot();
    int64_t nextSnapshot = -1;

    if (direction == Direction::Increase)
    {
        for (int64_t i = currentSnapshot + 1; i < (int64_t)snapshots.size(); ++i)
        {
            if (snapshots[i])
            {
                nextSnapshot = i;
                break;
            }
        }
        if (nextSnapshot == -1)
        {
            for (int64_t i = 0; i <= currentSnapshot && i < (int64_t)snapshots.size(); ++i)
            {
                if (snapshots[i])
                {
                    nextSnapshot = i;
                    break;
                }
            }
        }
    }
    else
    {
        for (int64_t i = currentSnapshot - 1; i >= 0; --i)
        {
            if (snapshots[i])
            {
                nextSnapshot = i;
                break;
            }
        }
        if (nextSnapshot == -1)
        {
            for (int64_t i = (int64_t)snapshots.size() - 1; i > currentSnapshot; --i)
            {
                if (snapshots[i])
                {
                    nextSnapshot = i;
                    break;
                }
            }
        }
    }
    if (nextSnapshot != -1 && nextSnapshot != currentSnapshot)
    {
        SetSnapshot(nextSnapshot);
    }
}

void PiPedalModel::OnNotifyNextMidiSnapshot(const RealtimeNextMidiProgramRequest &request)
{
    std::lock_guard<std::recursive_mutex> guard{mutex};
    try
    {
        if (request.direction >= 0)
        {
            NextSnapshot();
        }
        else
        {
            PreviousSnapshot();
        }
    }
    catch (std::exception &e)
    {
        Lv2Log::error(e.what());
    }
    AckMidiRequest(PendingMidiAckTracker::Kind::Program, request.requestId);
}

void PiPedalModel::OnNotifyNextMidiProgram(const RealtimeNextMidiProgramRequest &request)
{
    std::lock_guard<std::recursive_mutex> guard{mutex};
    try
    {

        if (request.direction >= 0)
        {
            NextPreset();
        }
        else
        {
            PreviousPreset();
        }
    }
    catch (std::exception &e)
    {

        Lv2Log::error(e.what());
    }
    AckMidiRequest(PendingMidiAckTracker::Kind::Program, request.requestId);
}

void PiPedalModel::OnNotifyMidiRealtimeSnapshotRequest(int32_t snapshotIndex, int64_t snapshotRequestId)
{
    std::lock_guard<std::recursive_mutex> guard{mutex};
    try
    {
        SetSnapshot((int64_t)snapshotIndex);
    }
    catch (const std::exception &e)
    {
        Lv2Log::error(SS("SetSnapshot failed. " << e.what()));
    }
    AckMidiRequest(PendingMidiAckTracker::Kind::Snapshot, snapshotRequestId);
}

void PiPedalModel::OnNotifyNextMidiBank(const RealtimeNextMidiProgramRequest &request)
{
    std::lock_guard<std::recursive_mutex> guard{mutex};
    try
    {

        if (request.direction >= 0)
        {
            NextBank();
        }
        else
        {
            PreviousBank();
        }
    }
    catch (std::exception &e)
    {

        Lv2Log::error(e.what());
    }
    AckMidiRequest(PendingMidiAckTracker::Kind::Program, request.requestId);
}

void PiPedalModel::OnNotifyMidiProgramChange(RealtimeMidiProgramRequest &midiProgramRequest)
{
    std::lock_guard<std::recursive_mutex> guard{mutex};
    try
    {
        if (midiProgramRequest.bank >= 0)
        {
            int64_t bankId = storage.GetBankByMidiBankNumber(midiProgramRequest.bank);
            if (bankId == -1)
                throw PiPedalException("Bank not found.");
            if (bankId != this->storage.GetBanks().selectedBank())
            {
                storage.LoadBank(bankId);

                FireBanksChanged(-1);
                FirePresetsChanged(-1);
            }
        }
        int64_t presetId = storage.GetPresetByProgramNumber(midiProgramRequest.program);
        if (presetId == -1)
            throw PiPedalException("No valid preset.");
        LoadPreset(-1, presetId);
    }
    catch (std::exception &e)
    {

        Lv2Log::error(e.what());
    }
    AckMidiRequest(PendingMidiAckTracker::Kind::Program, midiProgramRequest.requestId);
}

// Called with `mutex` held, once a realtime MIDI program/snapshot request has been handled.
// If the request (or an earlier change) left a pedalboard build outstanding, the audio thread must keep
// deferring MIDI until that pedalboard is running, so the ack is sent by the installer (after
// audioHost->SetPedalboard()), or by whichever path ends the build. Otherwise, ack now.
void PiPedalModel::AckMidiRequest(PendingMidiAckTracker::Kind kind, int64_t requestId)
{
    bool buildOutstanding = !closed && pedalboardBuilder && !pedalboardBuilder->IsIdle();
    uint64_t generation = pedalboardBuilder ? pedalboardBuilder->CurrentGeneration() : 0;
    if (pendingMidiAcks.Defer(kind, requestId, buildOutstanding, generation))
    {
        SendMidiAcks({PendingMidiAckTracker::Ack{kind, requestId, generation}});
    }
}

// Called with `mutex` held.
void PiPedalModel::SendMidiAcks(const std::vector<PendingMidiAckTracker::Ack> &acks)
{
    if (!this->audioHost)
    {
        return;
    }
    for (const auto &ack : acks)
    {
        if (ack.kind == PendingMidiAckTracker::Kind::Snapshot)
        {
            this->audioHost->AckSnapshotRequest(ack.requestId);
        }
        else
        {
            this->audioHost->AckMidiProgramRequest(ack.requestId);
        }
    }
}

void PiPedalModel::LoadPreset(int64_t clientId, int64_t instanceId)
{
    std::lock_guard<std::recursive_mutex> guard{mutex};

    if (storage.LoadPreset(instanceId))
    {
        this->pedalboard = storage.GetCurrentPreset();
        UpdateDefaults(&this->pedalboard);

        this->hasPresetChanged = false; // no fire.
        storage.DiscardCurrentPreset(); // drop any stale autosave of an abandoned edit.
        this->FirePedalboardChanged(clientId);
        this->FirePresetsChanged(clientId); // fire now.
    }
}

int64_t PiPedalModel::CopyPreset(int64_t clientId, int64_t from, int64_t to)
{
    std::lock_guard<std::recursive_mutex> guard{mutex};

    int64_t result = storage.CopyPreset(from, to);
    if (result != -1)
    {
        this->FirePresetsChanged(clientId); // fire now.
    }
    else
    {
        throw PiPedalStateException("Copy failed.");
    }
    return result;
}
bool PiPedalModel::UpdatePresets(int64_t clientId, const PresetIndex &presets)
{
    std::lock_guard<std::recursive_mutex> guard{mutex};
    storage.SetPresetIndex(presets);
    FirePresetsChanged(clientId);
    return true;
}

void PiPedalModel::MoveBank(int64_t clientId, int from, int to)
{
    std::lock_guard<std::recursive_mutex> guard{mutex};
    storage.MoveBank(from, to);
    FireBanksChanged(clientId);
}
int64_t PiPedalModel::DeleteBank(int64_t clientId, int64_t instanceId)
{
    std::lock_guard<std::recursive_mutex> guard{mutex};
    int64_t selectedBank = this->storage.GetBanks().selectedBank();
    int64_t newSelection = storage.DeleteBank(instanceId);

    int64_t newSelectedBank = this->storage.GetBanks().selectedBank();

    this->FireBanksChanged(clientId); // fire now.

    if (newSelectedBank != selectedBank)
    {
        this->OpenBank(clientId, newSelectedBank);
    }
    return newSelection;
}

int64_t PiPedalModel::DeletePresets(int64_t clientId, const std::vector<int64_t> &presetInstanceIds)
{
    std::lock_guard<std::recursive_mutex> guard{mutex};
    int64_t oldSelection = storage.GetCurrentPresetId();
    int64_t newSelection = storage.DeletePresets(presetInstanceIds);
    this->FirePresetsChanged(clientId); // fire BEFORE we load a new preset.
    if (oldSelection != newSelection)
    {
        this->LoadPreset(
            -1, // can't use cached version.
            newSelection);
    }
    return newSelection;
}
bool PiPedalModel::RenamePreset(int64_t clientId, int64_t instanceId, const std::string &name)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (storage.RenamePreset(instanceId, name))
    {
        this->FirePresetsChanged(clientId);
        if (storage.GetCurrentPresetId() == instanceId)
        {
            this->pedalboard.name(name);
            this->FirePedalboardChanged(-1);
        }
        return true;
    }
    else
    {
        throw PiPedalStateException("Rename failed.");
    }
}

GovernorSettings PiPedalModel::GetGovernorSettings()
{
    {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        GovernorSettings result;
        result.governor_ = storage.GetGovernorSettings();
        result.governors_ = pipedal::GetAvailableGovernors();
        return result;
    }
}
void PiPedalModel::SetGovernorSettings(const std::string &governor)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (!pipedal::IsValidGovernor(governor, pipedal::GetAvailableGovernors()))
    {
        // Thrown back to the client as an error reply, so the UI can drop its optimistic value.
        Lv2Log::warning(SS("Rejecting unavailable CPU governor '" << governor << "'."));
        throw PiPedalException(SS("CPU governor '" << governor << "' is not available on this device."));
    }
    adminClient.SetGovernorSettings(governor);

    this->storage.SetGovernorSettings(governor);

    std::vector<IPiPedalModelSubscriber::ptr> t{subscribers.begin(), subscribers.end()};
    for (auto &subscriber : t)
    {
        subscriber->OnGovernorSettingsChanged(governor);
    }
}

void PiPedalModel::SetWifiConfigSettings(const WifiConfigSettings &wifiConfigSettings)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);

#if NEW_WIFI_CONFIG
    if (this->storage.SetWifiConfigSettings(wifiConfigSettings))
    {
        this->UpdateDnsSd();
        if (this->hotspotManager)
        {
            this->hotspotManager->Reload();
        }
    }
#else
    this->storage.SetWifiConfigSettings(wifiConfigSettings);
    adminClient.SetWifiConfig(wifiConfigSettings);
#endif

    {
        WifiConfigSettings settingsWithNoSecrets = storage.GetWifiConfigSettings(); // (the passwordless version)

        std::vector<IPiPedalModelSubscriber::ptr> t{subscribers.begin(), subscribers.end()};
        for (auto &subscriber : t)
        {
            subscriber->OnWifiConfigSettingsChanged(settingsWithNoSecrets);
        }
    }
}

static std::string GetP2pdName()
{
    std::string name = "/etc/pipedal/config/pipedal_p2pd.conf";

    std::string result;

    if (ConfigUtil::GetConfigLine(name, "p2p_device_name", &result))
    {
        return result;
    }
    return "";
}

void PiPedalModel::UpdateDnsSd()
{
    if (!avahiService)
    {
        throw std::runtime_error("Not ready.");
    }
    ServiceConfiguration deviceIdFile;
    deviceIdFile.Load();
    WifiConfigSettings wifiSettings;
    wifiSettings.Load();
    std::string serviceName = wifiSettings.hotspotName_;
    if (serviceName == "")
    {
        serviceName = deviceIdFile.deviceName;
    }
    if (serviceName == "")
    {
        serviceName = "pipedal";
    }

    std::string hostName = GetHostName();
    if (serviceName != "" && deviceIdFile.uuid != "")
    {
        avahiService->Announce(webPort, serviceName, deviceIdFile.uuid, hostName, true);
    }
    else
    {
        // device_uuid file is written at install time. This warning is harmless if you're debugging.
        // Without it, we can't pulblish the website via dnsDS.
        // Run "pipedalconfig --install" to create the file.
        Lv2Log::warning("Cant read device_uuid file from service.conf file. dnsSD announcement skipped.");
    }
}
void PiPedalModel::SetWifiDirectConfigSettings(const WifiDirectConfigSettings &wifiDirectConfigSettings)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);

    adminClient.SetWifiDirectConfig(wifiDirectConfigSettings);

    this->storage.SetWifiDirectConfigSettings(wifiDirectConfigSettings);

    {
        WifiDirectConfigSettings tWifiDirectConfigSettings = storage.GetWifiDirectConfigSettings(); // (the passwordless version)

        std::vector<IPiPedalModelSubscriber::ptr> t{subscribers.begin(), subscribers.end()};
        for (auto &subscriber : t)
        {
            subscriber->OnWifiDirectConfigSettingsChanged(tWifiDirectConfigSettings);
        }

        // update NSD-SD announement.
        UpdateDnsSd();
    }
}

WifiConfigSettings PiPedalModel::GetWifiConfigSettings()
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return this->storage.GetWifiConfigSettings();
}
WifiDirectConfigSettings PiPedalModel::GetWifiDirectConfigSettings()
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return this->storage.GetWifiDirectConfigSettings();
}

void PiPedalModel::SetShowStatusMonitor(bool show)
{
    {
        std::lock_guard<std::recursive_mutex> lock(mutex); // copy atomically.
        storage.SetShowStatusMonitor(show);

        // Notify clients.
        std::vector<IPiPedalModelSubscriber::ptr> t{subscribers.begin(), subscribers.end()};
        for (auto &subscriber : t)
        {
            subscriber->OnShowStatusMonitorChanged(show);
        }
    }
}
bool PiPedalModel::GetShowStatusMonitor()
{
    std::lock_guard<std::recursive_mutex> lock(mutex); // copy atomically.
    return storage.GetShowStatusMonitor();
}

void PiPedalModel::SetSuspendBypassedPlugins(bool value)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (storage.GetSuspendBypassedPlugins() == value)
    {
        return;
    }
    storage.SetSuspendBypassedPlugins(value);
    if (audioHost)
    {
        // reaches the audio thread through the host->RT ring buffer.
        audioHost->SetSuspendBypassedPlugins(value);
    }

    // Notify clients.
    std::vector<IPiPedalModelSubscriber::ptr> t{subscribers.begin(), subscribers.end()};
    for (auto &subscriber : t)
    {
        subscriber->OnSuspendBypassedPluginsChanged(value);
    }
}
bool PiPedalModel::GetSuspendBypassedPlugins()
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return storage.GetSuspendBypassedPlugins();
}

JackConfiguration PiPedalModel::GetJackConfiguration()
{
    std::lock_guard<std::recursive_mutex> lock(mutex); // copy atomically.
    return this->jackConfiguration;
}

void PiPedalModel::RestartAudio(bool useDummyAudioDriver)
{
    {
        // Any pedalboard that is pending or being built was built for the old audio configuration.
        // Discard it; LoadCurrentPedalboard() below requests a fresh build.
        std::lock_guard<std::recursive_mutex> lock(mutex);
        // Until the fresh build is requested below, no full build may be installed: it could have been built
        // for the old configuration but installed into the re-opened audio host.
        audioRestarting = true;
        if (pedalboardBuilder)
        {
            pedalboardBuilder->Invalidate();
        }
        // The builds that MIDI requests were waiting on will never be installed. (AudioHost::Open() also
        // clears the audio thread's pending request state, in case these acks are lost with the old ring.)
        SendMidiAcks(pendingMidiAcks.TakeAll());
        FailDeferredPatchRequests("The audio is restarting.");
    }
    struct RestartingGuard
    {
        PiPedalModel *this_;
        ~RestartingGuard()
        {
            std::lock_guard<std::recursive_mutex> lock(this_->mutex);
            this_->audioRestarting = false;
        }
    } restartingGuard{this};
    try
    {
        if (useDummyAudioDriver)
        {
            CancelAudioRetry();
        }
        if (this->audioHost->IsOpen())
        {

            this->audioHost->Close();
        }
        // restarting is a bit dodgy. It was impossible with Jack, but
        // now very plausible with the ALSA audio stack.

        // Still bugs wrt/ restarting the circular buffers for the audio thread.

        // do a complete reload.
        {
            // The audio thread has stopped: pedalboards that borrowed its effects may now be discarded,
            // and its effects (instantiated for the old configuration) must never be borrowed again.
            std::lock_guard<std::recursive_mutex> lock(mutex);
            ++audioEpoch;
            this->audioHost->SetPedalboard(nullptr);
            this->lv2Pedalboard = nullptr;
            previousPedalboardLoaded = false;
            pedalboardBuilder->modeTracker.OnRunningPedalboardDiscarded();
        }
        auto jackServerSettings = this->jackServerSettings;
        if (useDummyAudioDriver)
        {
            jackServerSettings.UseDummyAudioDevice();
        }

        auto jackConfiguration = this->jackConfiguration;
        jackConfiguration.AlsaInitialize(jackServerSettings);
        if (!jackConfiguration.isValid())
        {
            jackConfiguration.setErrorStatus("Error");
        }
        if (!useDummyAudioDriver)
        {
            this->jackConfiguration = jackConfiguration;
            FireJackConfigurationChanged(jackConfiguration);
        }

        if (!jackServerSettings.IsValid() || !jackConfiguration.isValid())
        {
            throw std::runtime_error("Audio configuration not valid.");
        }

        auto channelSelection = this->storage.GetChannelSelection();

        this->audioHost->Open(jackServerSettings, channelSelection); 

        {
            std::lock_guard<std::mutex> configLock(pluginHostConfigurationMutex); // wait for any in-flight build.
            ++pluginHostConfigurationVersion;
            this->pluginHost.OnConfigurationChanged(jackConfiguration, channelSelection);
        }

        FireChannelRouterSettingsChanged(-1);
        {
            std::lock_guard<std::recursive_mutex> lock(mutex);
            audioRestarting = false;
            LoadCurrentPedalboard();
        }

        this->UpdateRealtimeVuSubscriptions();
        UpdateRealtimeMonitorPortSubscriptions();
    }
    catch (const std::exception &e)
    {
        this->audioHost->Close();
        if (useDummyAudioDriver)
        {
            Lv2Log::error(SS("Failed to start dummy audio driver. " << e.what()));
        }
        else
        {
            Lv2Log::error(SS("Failed to start audio. " << e.what()));
        }
        if (!useDummyAudioDriver)
        {
            RestartAudio(true); // use the dummy audio driver.
        }
    }
}

void PiPedalModel::OnAlsaSequencerDeviceAdded(int client, const std::string &clientName)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    auto alsaSequencerConfiguration = this->storage.GetAlsaSequencerConfiguration();
    std::string key = "seq:" + clientName;
    bool interested = false;
    for (const auto &port : alsaSequencerConfiguration.connections())
    {
        if (port.id().starts_with(key))
        {
            interested = true;
            break;
        }
    }
    if (interested)
    {
        Post(
            [this]
            {
                // reconfigure connections.
                std::lock_guard<std::recursive_mutex> lock(this->mutex);
                if (this->audioHost)
                {
                    this->audioHost->SetAlsaSequencerConfiguration(this->storage.GetAlsaSequencerConfiguration());
                }
            });
    }
}
void PiPedalModel::OnAlsaSequencerDeviceRemoved(int client)
{
    // no action  required.
}

void PiPedalModel::SetAlsaSequencerConfiguration(const AlsaSequencerConfiguration &alsaSequencerConfiguration)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);

    // reset midi connections even if the configuration hasn't changed.
    this->audioHost->SetAlsaSequencerConfiguration(alsaSequencerConfiguration);

    auto current = storage.GetAlsaSequencerConfiguration();
    if (alsaSequencerConfiguration != current)
    {
        this->storage.SetAlsaSequencerConfiguration(alsaSequencerConfiguration);
        // notify subscribers.
        std::vector<IPiPedalModelSubscriber::ptr> t{subscribers.begin(), subscribers.end()};
        for (auto &subscriber : t)
        {
            subscriber->OnAlsaSequencerConfigurationChanged(alsaSequencerConfiguration);
        }
    }
}
AlsaSequencerConfiguration PiPedalModel::GetAlsaSequencerConfiguration()
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return this->storage.GetAlsaSequencerConfiguration();
}

std::vector<AlsaSequencerPortSelection> PiPedalModel::GetAlsaSequencerPorts()
{
    auto ports = AlsaSequencer::EnumeratePorts();
    std::vector<AlsaSequencerPortSelection> result;
    for (auto &port : ports)
    {
        result.push_back(AlsaSequencerPortSelection{
            port.id,
            port.name,
            port.displaySortOrder});
    }
    return result;
}

void PiPedalModel::FireChannelRouterSettingsChanged(int64_t clientId)
{
    std::lock_guard<std::recursive_mutex> guard{mutex};
    {
        // take a snapshot incase a client unsusbscribes in the notification handler (in which case the mutex won't protect us)
        ChannelRouterSettings::ptr channelRouterSettings = this->channelRouterSettings;

        std::vector<IPiPedalModelSubscriber::ptr> t{subscribers.begin(), subscribers.end()};
        for (auto &subscriber : t)
        {
            subscriber->OnChannelRouterSettingsChanged(clientId, *channelRouterSettings);
        }
    }
}

JackChannelSelection PiPedalModel::GetJackChannelSelection()
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    JackChannelSelection t = this->storage.GetJackChannelSelection(this->jackConfiguration);

    return t;
}

int64_t PiPedalModel::AddVuSubscription(int64_t instanceId)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    int64_t subscriptionId = ++nextSubscriptionId;
    activeVuSubscriptions.push_back(VuSubscription{subscriptionId, instanceId});

    UpdateRealtimeVuSubscriptions();

    return subscriptionId;
}
void PiPedalModel::RemoveVuSubscription(int64_t subscriptionHandle)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    for (auto i = activeVuSubscriptions.begin(); i != activeVuSubscriptions.end(); ++i)
    {
        if ((*i).subscriptionHandle == subscriptionHandle)
        {
            activeVuSubscriptions.erase(i);
            break;
        }
    }
    UpdateRealtimeVuSubscriptions();
}

void PiPedalModel::OnNotifyMidiValueChanged(int64_t instanceId, int portIndex, float value, IEffect *sourceEffect, Lv2Pedalboard *sourcePedalboard)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (!RunningInstanceIdsMatchPedalboard() ||
        !IsMidiValueFromInstalledPedalboard(this->lv2Pedalboard.get(), sourcePedalboard, (uint64_t)instanceId, sourceEffect))
    {
        // From the outgoing pedalboard (before the install, or after it but before the audio thread swapped
        // the new pedalboard in): its instance ids may name unrelated items of this->pedalboard, and the swap
        // applies the installed item's own value even to a reused instance.
        return;
    }
    PedalboardItem *item = this->pedalboard.GetItem(instanceId);
    if (item)
    {
        Lv2PluginInfo::ptr pPluginInfo;
        if (item->uri() == SPLIT_PEDALBOARD_ITEM_URI)
        {
            pPluginInfo = GetSplitterPluginInfo();
        }
        else
        {
            pPluginInfo = pluginHost.GetPluginInfo(item->uri());
        }
        if (pPluginInfo)
        {
            if (portIndex == -1)
            {
                this->pedalboard.SetItemEnabled(instanceId, value != 0);
                // take a snapshot incase a client unsusbscribes in the notification handler (in which case the mutex won't protect us)
                std::vector<IPiPedalModelSubscriber::ptr> t{subscribers.begin(), subscribers.end()};
                for (auto &subscriber : t)
                {
                    subscriber->OnItemEnabledChanged(-1, instanceId, value != 0);
                }

                this->SetPresetChanged(-1, true);
                return;
            }
            else
            {
                for (int i = 0; i < pPluginInfo->ports().size(); ++i)
                {
                    auto &port = pPluginInfo->ports()[i];
                    if (port->index() == portIndex)
                    {
                        std::string symbol = port->symbol();

                        this->pedalboard.SetControlValue(instanceId, symbol, value);
                        {

                            // take a snapshot incase a client unsusbscribes in the notification handler (in which case the mutex won't protect us)
                            std::vector<IPiPedalModelSubscriber::ptr> t{subscribers.begin(), subscribers.end()};
                            for (auto &subscriber : t)
                            {
                                subscriber->OnMidiValueChanged(instanceId, symbol, value);
                            }

                            this->SetPresetChanged(-1, true);
                            return;
                        }
                    }
                }
            }
        }
    }
}

void PiPedalModel::OnNotifyVusSubscription(const std::vector<VuUpdateX> &updates)
{
    std::vector<IPiPedalModelSubscriber::ptr> subscribers;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        subscribers = this->subscribers;
    }
    // subscribers is a snapshot, in case a client unsubscribes in the notification handler.
    for (auto &subscriber : subscribers)
    {
        subscriber->OnVuMeterUpdate(updates);
    }
}

static bool isStartOrEndControl(int64_t controlId)
{
    switch (controlId)
    {
    case Pedalboard::START_CONTROL_ID:
    case Pedalboard::END_CONTROL_ID:
    case Pedalboard::AUX_START_CONTROL_ID:
    case Pedalboard::AUX_END_CONTROL_ID:
        return true;
    default:
        return false;
    }
}

void PiPedalModel::UpdateRealtimeVuSubscriptions()
{
    std::set<int64_t> addedInstances;

    for (int i = 0; i < activeVuSubscriptions.size(); ++i)
    {
        auto instanceId = activeVuSubscriptions[i].instanceid;
        if (pedalboard.HasItem(instanceId) || isStartOrEndControl(instanceId))
        {
            addedInstances.insert(activeVuSubscriptions[i].instanceid);
        }
    }
    if (audioHost)
    {
        std::vector<int64_t> instanceids(addedInstances.begin(), addedInstances.end());
        audioHost->SetVuSubscriptions(instanceids);
    }
}

void PiPedalModel::UpdateRealtimeMonitorPortSubscriptions()
{
    if (!audioHost)
    {
        return;
    }
    audioHost->SetMonitorPortSubscriptions(this->activeMonitorPortSubscriptions);
}

int64_t PiPedalModel::MonitorPort(int64_t instanceId, const std::string &key, float updateInterval, PortMonitorCallback onUpdate)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    int64_t subscriptionId = ++nextSubscriptionId;
    activeMonitorPortSubscriptions.push_back(
        MonitorPortSubscription{
            subscriptionId,
            instanceId,
            key,
            updateInterval,
            onUpdate});

    UpdateRealtimeMonitorPortSubscriptions();

    return subscriptionId;
}
void PiPedalModel::UnmonitorPort(int64_t subscriptionHandle)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);

    for (auto i = activeMonitorPortSubscriptions.begin(); i != activeMonitorPortSubscriptions.end(); ++i)
    {
        if ((*i).subscriptionHandle == subscriptionHandle)
        {
            activeMonitorPortSubscriptions.erase(i);
            UpdateRealtimeMonitorPortSubscriptions();
            break;
        }
    }
}

void PiPedalModel::OnNotifyMonitorPort(const MonitorPortUpdate &update)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);

    for (auto i = activeMonitorPortSubscriptions.begin(); i != activeMonitorPortSubscriptions.end(); ++i)
    {
        if ((*i).subscriptionHandle == update.subscriptionHandle)
        {

            // make the call ONLY if the subscription handle is still valid.
            (*update.callbackPtr)(update.subscriptionHandle, update.value);
            break;
        }
    }
}

void PiPedalModel::SendSetPatchProperty(
    int64_t clientId,
    int64_t instanceId,
    const std::string propertyUri,
    const json_variant &value,
    std::function<void()> onSuccess,
    std::function<void(const std::string &error)> onError)
{

    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (!audioHost)
    {
        onError("Audio not running.");
        return;
    }

    // save the property to the preset (currently used to reconstruct snapshots only)
    bool savedPathProperty = false;
    PedalboardItem *pedalboardItem = this->pedalboard.GetItem(instanceId);
    if (pedalboardItem)
    {
        std::shared_ptr<Lv2PluginInfo> pluginInfo = GetPluginInfo(pedalboardItem->uri_);
        auto pipedalUi = pluginInfo ? pluginInfo->piPedalUI() : nullptr;
        if (pipedalUi)
        {
            auto fileProperty = pipedalUi->GetFileProperty(propertyUri);
            if (fileProperty)
            {

                json_variant abstractPath = pluginHost.AbstractPath(value);
                std::string atomString = abstractPath.to_string();
                pedalboardItem->pathProperties_[propertyUri] = atomString;
                savedPathProperty = true;
            }
            this->SetPresetChanged(clientId, true);
        }
    }
    if (!RunningInstanceIdsMatchPedalboard() || !lv2Pedalboard)
    {
        // A full build (preset/bank switch) is outstanding: the running pedalboard is the outgoing one, whose
        // instance ids may name unrelated plugins.
        if (savedPathProperty)
        {
            // applied to the new pedalboard by the installer (the reconciliation snapshot carries path properties).
            if (onSuccess)
            {
                onSuccess();
            }
            return;
        }
        try
        {
            atomConverter.ToAtom(value); // reject malformed values now, not when the pedalboard is installed.
        }
        catch (const std::exception &e)
        {
            if (onError)
            {
                onError(SS("Invalid value. " << e.what()));
            }
            return;
        }
        DeferredPatchRequest deferred;
        deferred.clientId = clientId;
        deferred.value = value;
        deferred.onSetSuccess = std::move(onSuccess);
        deferred.onError = std::move(onError);
        DeferPatchRequest(instanceId, propertyUri, true, std::move(deferred));
        return;
    }
    SendPatchSetRequest(clientId, instanceId, propertyUri, value, std::move(onSuccess), std::move(onError));
}

// Called with `mutex` held, when the running pedalboard's instance ids are those of this->pedalboard.
void PiPedalModel::SendPatchSetRequest(
    int64_t clientId,
    int64_t instanceId,
    const std::string &propertyUri,
    const json_variant &value,
    std::function<void()> onSuccess,
    std::function<void(const std::string &error)> onError)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    LV2_Atom *atomValue = atomConverter.ToAtom(value);

    std::function<void(RealtimePatchPropertyRequest *)> onRequestComplete{
        [this, onSuccess](RealtimePatchPropertyRequest *pParameter)
        {
            {
                std::lock_guard<std::recursive_mutex> lock(mutex);
                bool cancelled = true;
                for (auto i = this->outstandingParameterRequests.begin();
                     i != this->outstandingParameterRequests.end(); ++i)
                {
                    if ((*i) == pParameter)
                    {
                        cancelled = false;
                        this->outstandingParameterRequests.erase(i);
                        break;
                    }
                }
                if (!cancelled)
                {
                    if (pParameter->errorMessage != nullptr)
                    {
                        if (pParameter->onError)
                        {
                            pParameter->onError(pParameter->errorMessage);
                        }
                    }
                    else
                    {
                        if (onSuccess)
                        {
                            onSuccess();
                        }
                    }
                }
                delete pParameter;
            }
        }};

    LV2_URID urid = this->pluginHost.GetLv2Urid(propertyUri.c_str());
    size_t sampleTimeout = 0.5 * audioHost->GetSampleRate();
    RealtimePatchPropertyRequest *request = new RealtimePatchPropertyRequest(
        onRequestComplete,
        clientId, instanceId, urid, atomValue, nullptr, onError,
        sampleTimeout);
    request->instanceIdLineage = this->instanceIdLineage;

    outstandingParameterRequests.push_back(request);
    if (this->audioHost)
    {
        this->audioHost->sendRealtimeParameterRequest(request);
    }
}

void PiPedalModel::SendGetPatchProperty(
    int64_t clientId,
    int64_t instanceId,
    const std::string uri,
    std::function<void(const std::string &jsonResult)> onSuccess,
    std::function<void(const std::string &error)> onError)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);

    if (!this->audioHost)
    {
        onError("Audio stopped.");
        return;
    }
    if (!RunningInstanceIdsMatchPedalboard() || !lv2Pedalboard)
    {
        // A full build (preset/bank switch) is outstanding: the running pedalboard is the outgoing one, whose
        // instance ids may name unrelated plugins. Ask the new pedalboard's plugin once it is installed.
        DeferredPatchRequest deferred;
        deferred.clientId = clientId;
        deferred.onGetSuccess = std::move(onSuccess);
        deferred.onError = std::move(onError);
        DeferPatchRequest(instanceId, uri, false, std::move(deferred));
        return;
    }
    SendPatchGetRequest(clientId, instanceId, uri, std::move(onSuccess), std::move(onError));
}

// Called with `mutex` held, when the running pedalboard's instance ids are those of this->pedalboard.
void PiPedalModel::SendPatchGetRequest(
    int64_t clientId,
    int64_t instanceId,
    const std::string &uri,
    std::function<void(const std::string &jsonResult)> onSuccess,
    std::function<void(const std::string &error)> onError)
{
    std::function<void(RealtimePatchPropertyRequest *)> onRequestComplete{
        [this](RealtimePatchPropertyRequest *pParameter)
        {
            {
                std::lock_guard<std::recursive_mutex> lock(mutex);
                bool cancelled = true;
                for (auto i = this->outstandingParameterRequests.begin();
                     i != this->outstandingParameterRequests.end(); ++i)
                {
                    if ((*i) == pParameter)
                    {
                        cancelled = false;
                        this->outstandingParameterRequests.erase(i);
                        break;
                    }
                }
                if (!cancelled)
                {
                    if (pParameter->errorMessage != nullptr)
                    {
                        if (pParameter->onError)
                        {
                            pParameter->onError(pParameter->errorMessage);
                        }
                    }
                    else if (pParameter->GetSize() == 0 && pParameter->requestType == RealtimePatchPropertyRequest::RequestType::PatchGet)
                    {
                        if (pParameter->onError)
                        {
                            // For plugins that don't respond (e.g. a buncha MOD plugins), use the value we last set on the plugin!
                            bool foundValue = false;
                            std::lock_guard<std::recursive_mutex> lock(mutex);
                            auto pedalboardItem = this->pedalboard.GetItem(pParameter->instanceId);
                            if (pedalboardItem)
                            {
                                if (pedalboardItem->pathProperties_.contains(pParameter->uri))
                                {
                                    pParameter->jsonResponse = pedalboardItem->pathProperties_[pParameter->uri];
                                    if (pParameter->onSuccess)
                                    {
                                        foundValue = true;
                                        pParameter->onSuccess(pParameter->jsonResponse);
                                    }
                                }
                            }
                            if (!foundValue)
                            {
                                pParameter->onError("No response.");
                            }
                        }
                    }
                    else
                    {
                        if (pParameter->onSuccess)
                        {
                            pParameter->onSuccess(pParameter->jsonResponse);
                        }
                    }
                }
                delete pParameter;
            }
        }};

    std::lock_guard<std::recursive_mutex> lock(mutex);
    LV2_URID urid = this->pluginHost.GetLv2Urid(uri.c_str());
    size_t sampleTimeout = 0.3 * audioHost->GetSampleRate();
    RealtimePatchPropertyRequest *request = new RealtimePatchPropertyRequest(
        onRequestComplete,
        clientId, instanceId, urid, onSuccess, onError, sampleTimeout);
    request->uri = uri;
    request->instanceIdLineage = this->instanceIdLineage;

    outstandingParameterRequests.push_back(request);
    this->audioHost->sendRealtimeParameterRequest(request);
}

// Called with `mutex` held.
void PiPedalModel::DeferPatchRequest(int64_t instanceId, const std::string &propertyUri, bool isSet, DeferredPatchRequest &&request)
{
    if (!pedalboardBuilder || pedalboardBuilder->IsIdle())
    {
        // no build is coming that would answer it (e.g. the last one failed).
        if (request.onError)
        {
            request.onError("The pedalboard is not loaded.");
        }
        return;
    }
    std::optional<DeferredPatchRequest> replaced;
    switch (deferredPatchRequests.Add(request.clientId, instanceId, propertyUri, isSet, request, &replaced))
    {
    case DeferredPatchRequests<DeferredPatchRequest>::AddResult::Queued:
        break;
    case DeferredPatchRequests<DeferredPatchRequest>::AddResult::Replaced:
        // superseded by a newer value of the same property, which will be sent instead.
        if (replaced && replaced->onSetSuccess)
        {
            replaced->onSetSuccess();
        }
        break;
    case DeferredPatchRequests<DeferredPatchRequest>::AddResult::Full:
        if (request.onError)
        {
            request.onError("Too many requests while the pedalboard is loading.");
        }
        break;
    }
}

// Called with `mutex` held, after a pedalboard was installed: sends the requests deferred while a full build was
// outstanding to the installed pedalboard (whose instance ids are now those of this->pedalboard).
void PiPedalModel::FlushDeferredPatchRequests()
{
    if (deferredPatchRequests.empty() || !RunningInstanceIdsMatchPedalboard() || !lv2Pedalboard || !audioHost)
    {
        return;
    }
    // Each entry is isolated: one that fails (e.g. a value the atom converter rejects) is reported to its own
    // client and doesn't affect the others, or the install that triggered the flush.
    deferredPatchRequests.SendAll(
        [this](DeferredPatchRequests<DeferredPatchRequest>::Entry &entry)
        {
            DeferredPatchRequest &request = entry.request;
            if (!this->pedalboard.HasItem(entry.instanceId))
            {
                if (request.onError)
                {
                    request.onError("The plugin is no longer in the pedalboard.");
                }
                return;
            }
            // (callbacks copied: the entry keeps onError in case sending throws.)
            if (entry.isSet)
            {
                SendPatchSetRequest(
                    request.clientId, entry.instanceId, entry.propertyUri, request.value,
                    request.onSetSuccess, request.onError);
            }
            else
            {
                SendPatchGetRequest(
                    request.clientId, entry.instanceId, entry.propertyUri,
                    request.onGetSuccess, request.onError);
            }
        },
        [](DeferredPatchRequests<DeferredPatchRequest>::Entry &entry, const std::exception &e)
        {
            Lv2Log::warning(SS("Deferred patch property request failed. " << e.what()));
            try
            {
                if (entry.request.onError)
                {
                    entry.request.onError(e.what());
                }
            }
            catch (const std::exception &)
            {
            }
        });
}

// Called with `mutex` held: the pedalboard the deferred requests were waiting for will never be installed.
void PiPedalModel::FailDeferredPatchRequests(const std::string &error)
{
    for (auto &entry : deferredPatchRequests.TakeAll())
    {
        try
        {
            if (entry.request.onError)
            {
                entry.request.onError(error);
            }
        }
        catch (const std::exception &e)
        {
            Lv2Log::warning(SS("Failed to report a patch property error. " << e.what()));
        }
    }
}

BankIndex PiPedalModel::GetBankIndex() const
{
    std::lock_guard<std::recursive_mutex> guard(const_cast<std::recursive_mutex &>(mutex));
    return storage.GetBanks();
}

void PiPedalModel::RenameBank(int64_t clientId, int64_t bankId, const std::string &newName)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    storage.RenameBank(bankId, newName);
    FireBanksChanged(clientId);
}

int64_t PiPedalModel::SaveBankAs(int64_t clientId, int64_t bankId, const std::string &newName)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    int64_t newId = storage.SaveBankAs(bankId, newName);
    FireBanksChanged(clientId);
    return newId;
}

void PiPedalModel::OpenBank(int64_t clientId, int64_t bankId)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);

    storage.LoadBank(bankId);
    FireBanksChanged(clientId);

    FirePresetsChanged(clientId);

    this->pedalboard = storage.GetCurrentPreset();

    UpdateDefaults(&this->pedalboard);
    this->hasPresetChanged = false;
    storage.DiscardCurrentPreset(); // drop any stale autosave of an abandoned edit.
    this->FirePedalboardChanged(clientId);
}

JackServerSettings PiPedalModel::GetJackServerSettings()
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return this->jackServerSettings;
}

void PiPedalModel::SetOnboarding(bool value)
{
    std::unique_lock<std::recursive_mutex> guard(mutex);
    this->jackServerSettings.SetIsOnboarding(value);
    SetJackServerSettings(this->jackServerSettings);
}

void PiPedalModel::SetJackServerSettings(const JackServerSettings &jackServerSettings)
{
    std::unique_lock<std::recursive_mutex> guard(mutex);

#if JACK_HOST
    if (!adminClient.CanUseShutdownClient())
    {
        throw PiPedalException("Can't change server settings when running a debug server.");
    }
#endif

    this->jackServerSettings = jackServerSettings;

    // take a snapshot incase a client unsusbscribes in the notification handler (in which case the mutex won't protect us)
    std::vector<IPiPedalModelSubscriber::ptr> t{subscribers.begin(), subscribers.end()};
    for (auto &subscriber : t)
    {
        subscriber->OnJackServerSettingsChanged(jackServerSettings);
    }

#if ALSA_HOST
    storage.SetJackServerSettings(jackServerSettings);

    FireJackConfigurationChanged(this->jackConfiguration);

    CancelAudioRetry();

    guard.unlock();
    RestartAudio();

#endif
#if JACK_HOST
    if (adminClient.CanUseShutdownClient())
    {
        // save the current (edited) preset now in case the service shutdown isn't clean.
        CurrentPreset currentPreset;
        currentPreset.modified_ = this->hasPresetChanged;
        currentPreset.preset_ = this->pedalboard;
        storage.SaveCurrentPreset(currentPreset);

        this->jackConfiguration.SetIsRestarting(true);
        FireJackConfigurationChanged(this->jackConfiguration);
        this->audioHost->UpdateServerConfiguration(
            jackServerSettings,
            [this](bool success, const std::string &errorMessage)
            {
                std::lock_guard<std::recursive_mutex> lock(mutex);
                if (!success)
                {
                    std::stringstream s;
                    s << "UpdateServerconfiguration failed: " << errorMessage;
                    Lv2Log::error(s.str().c_str());
                }
                // Update jack server status.
                if (!success)
                {
                    this->jackConfiguration.SetIsRestarting(false);
                    this->jackConfiguration.SetErrorStatus(errorMessage);
                    FireJackConfigurationChanged(this->jackConfiguration);
                }
                else
                {
                // we now do a complete restart of the services,
                // so just sit tight and wait for the restart.
#ifdef JUNK
                    this->jackConfiguration.SetErrorStatus("");
                    FireJackConfigurationChanged(this->jackConfiguration);

                    // restart the pedalboard on a new instance.
                    std::shared_ptr<Lv2Pedalboard> lv2Pedalboard{this->pluginHost.CreateLv2Pedalboard(this->pedalboard)};
                    this->lv2Pedalboard = lv2Pedalboard;

                    audioHost->SetPedalboard(lv2Pedalboard);
                    UpdateRealtimeVuSubscriptions();
                    UpdateRealtimeMonitorPortSubscriptions();
#endif
                }
            });
    }
#endif
}

std::shared_ptr<Lv2PluginInfo> PiPedalModel::GetPluginInfo(const std::string &uri)
{
    return pluginHost.GetPluginInfo(uri);
}

void PiPedalModel::UpdateDefaults(SnapshotValue &snapshotValue, const PedalboardItem *pedalboardItem_)
{
    std::shared_ptr<Lv2PluginInfo> pPlugin = pluginHost.GetPluginInfo(pedalboardItem_->uri());
    if (!pPlugin)
    {
        if (pedalboardItem_->uri() == SPLIT_PEDALBOARD_ITEM_URI)
        {
            pPlugin = GetSplitterPluginInfo();
        }
    }
    if (pPlugin)
    {
        // Fix incorrect bypass settings presint in Pipedal < 1.5.96
        for (size_t i = 0; i < pPlugin->ports().size(); ++i)
        {
            auto &port = pPlugin->ports()[i];
            if (port->is_bypass() && port->is_control_port() && port->is_input())
            {
                ControlValue *pValue = snapshotValue.GetControlValue(port->symbol());
                float value = snapshotValue.isEnabled_ ? 1.0f : 0.0f;

                if (pValue == nullptr)
                {
                    snapshotValue.controlValues_.push_back(
                        pipedal::ControlValue(port->symbol().c_str(), value));
                }
                else
                {
                    pValue->value(value);
                }
            }
        }
        //////// PLUGIN SPECIFIC UPGRADES //////////////////////
        if (pPlugin->uri() == "http://two-play.com/plugins/toob-nam")
        {
            ControlValue *pVersion = snapshotValue.GetControlValue("version");
            if (pVersion == nullptr)
            {
                ControlValue *pValue = snapshotValue.GetControlValue("inputCalibrationMode");
                if (pValue == nullptr)
                {
                    // calibration is OFF when upgradfing.
                    snapshotValue.SetControlValue("inputCalibrationMode", 0.0f);
                }
                // convert old gate threshold to new gate threshold.
                ControlValue *pGateValue = snapshotValue.GetControlValue("gate");
                if (pGateValue)
                {
                    float value = pGateValue->value();
                    // Is the gate disabled?
                    if (value <= -100.0f)
                    {
                        value = -120.0f; // "disabled in new range."
                    }
                    else
                    {
                        value = value * 0.5; // correct the bug in original implementation.
                    }
                    snapshotValue.SetControlValue("gate", value);
                }
                snapshotValue.SetControlValue("version", 0.0f);
            }
        }
        if (pPlugin->piPedalUI())
        {
            PiPedalUI::ptr piPedalUI = pPlugin->piPedalUI();
            std::set<std::string> validFileProperties;
            for (auto &fileProperty : piPedalUI->fileProperties())
            {
                validFileProperties.insert(fileProperty->patchProperty());
            }
            for (auto i = snapshotValue.pathProperties_.begin(); i != snapshotValue.pathProperties_.end(); /**/)
            {
                if (!validFileProperties.contains(i->first))
                {
                    i = snapshotValue.pathProperties_.erase(i);
                }
                else
                {
                    ++i;
                }
            }
        }
    }
}

void PiPedalModel::UpdateDefaults(PedalboardItem *pedalboardItem, std::unordered_map<int64_t, PedalboardItem *> &itemMap)
{
    itemMap[pedalboardItem->instanceId()] = pedalboardItem;

    std::shared_ptr<Lv2PluginInfo> pPlugin = pluginHost.GetPluginInfo(pedalboardItem->uri());
    if (!pPlugin)
    {
        pedalboardItem->instanceId();
        if (pedalboardItem->uri() == SPLIT_PEDALBOARD_ITEM_URI)
        {
            pPlugin = GetSplitterPluginInfo();
        }
    }
    if (pPlugin)
    {
        if (pPlugin->hasMidiInput())
        {
            if (!pedalboardItem->midiChannelBinding())
            {
                pedalboardItem->midiChannelBinding(MidiChannelBinding::DefaultForMissingValue());
            }
        }
        else
        {
            if (pedalboardItem->midiChannelBinding())
            {
                pedalboardItem->midiChannelBinding(std::optional<MidiChannelBinding>()); // clear it.
            }
        }
        //////// PLUGIN SPECIFIC UPGRADES //////////////////////
        if (pPlugin->uri() == "http://two-play.com/plugins/toob-nam")
        {
            ControlValue *pVersion = pedalboardItem->GetControlValue("version");
            if (pVersion == nullptr)
            {
                ControlValue *pValue = pedalboardItem->GetControlValue("inputCalibrationMode");
                if (pValue == nullptr)
                {
                    // calibration is OFF when upgradfing.
                    pedalboardItem->SetControlValue("inputCalibrationMode", 0.0f);
                }
                // convert old gate threshold to new gate threshold.
                ControlValue *pGateValue = pedalboardItem->GetControlValue("gate");
                if (pGateValue)
                {
                    float value = pGateValue->value();
                    // Is the gate disabled?
                    if (value <= -100.0f)
                    {
                        value = -120.0f; // "disabled in new range."
                    }
                    else
                    {
                        value = value * 0.5; // correct the bug in original implementation.
                    }
                    pedalboardItem->SetControlValue("gate", value);
                }
                pedalboardItem->SetControlValue("version", 0.0f);
            }
        }
        for (size_t i = 0; i < pPlugin->ports().size(); ++i)
        {
            auto port = pPlugin->ports()[i];

            if (port->is_control_port() && port->is_input())
            {
                if (port->is_bypass())
                {
                    // retroactively correct bug in version of PiPedal prior to 1.5.96.
                    ControlValue *pValue = pedalboardItem->GetControlValue(port->symbol());
                    float value = pedalboardItem->isEnabled() ? 1.0f : 0.0f;

                    if (pValue == nullptr)
                    {
                        pedalboardItem->controlValues().push_back(
                            pipedal::ControlValue(port->symbol().c_str(), value));
                    }
                    else
                    {
                        pValue->value(value);
                    }
                }
                else
                {
                    ControlValue *pValue = pedalboardItem->GetControlValue(port->symbol());
                    if (pValue == nullptr)
                    {
                        // Missing? Set it to default value.
                        pedalboardItem->controlValues().push_back(
                            pipedal::ControlValue(port->symbol().c_str(), port->default_value()));
                    }
                }
            }
        }
        if (pPlugin->piPedalUI())
        {
            PiPedalUI::ptr piPedalUI = pPlugin->piPedalUI();
            std::set<std::string> validFileProperties;
            for (auto &fileProperty : piPedalUI->fileProperties())
            {
                validFileProperties.insert(fileProperty->patchProperty());
                if (!pedalboardItem->pathProperties_.contains(fileProperty->patchProperty()))
                {
                    // make sure each pedalboard item has a complete list of path properties, even if it doesn't yet have values.
                    pedalboardItem->pathProperties_[fileProperty->patchProperty()] = "null";
                }
            }
            for (auto i = pedalboardItem->pathProperties_.begin(); i != pedalboardItem->pathProperties_.end(); /**/)
            {
                if (!validFileProperties.contains(i->first))
                {
                    i = pedalboardItem->pathProperties_.erase(i);
                }
                else
                {
                    ++i;
                }
            }
        }
    }
    else
    {
        // an old bug leaks lv2states.  Clean it up here.
        if (pedalboardItem->uri() == EMPTY_PEDALBOARD_ITEM_URI)
        {
            pedalboardItem->lv2State(Lv2PluginState());
        }
    }
    for (size_t i = 0; i < pedalboardItem->topChain().size(); ++i)
    {
        UpdateDefaults(&(pedalboardItem->topChain()[i]), itemMap);
    }
    for (size_t i = 0; i < pedalboardItem->bottomChain().size(); ++i)
    {
        UpdateDefaults(&(pedalboardItem->bottomChain()[i]), itemMap);
    }
}

void PiPedalModel::UpdateDefaults(Snapshot *snapshot, std::unordered_map<int64_t, PedalboardItem *> &itemMap)
{
    if (!snapshot)
        return;
    for (size_t i = 0; i < snapshot->values_.size(); ++i)
    {
        SnapshotValue &value = snapshot->values_[i];
        auto f = itemMap.find(value.instanceId_);
        if (f == itemMap.end())
        {
            // plugin is no longer present. Remove from the snapshot.
            snapshot->values_.erase(snapshot->values_.begin() + i);
            --i;
        }
        else
        {
            UpdateDefaults(value, f->second);
        }
    }
    for (auto &snapshotValue : snapshot->values_)
    {
    }
}

void PiPedalModel::UpdateDefaults(Pedalboard *pedalboard)
{
    // add missing values.
    std::unordered_map<int64_t, PedalboardItem *> itemMap;
    auto allPlugins = pedalboard->GetAllPlugins();
    for (size_t i = 0; i < allPlugins.size(); ++i)
    {
        UpdateDefaults(allPlugins[i], itemMap);
    }
    // set all missing values on snapshots to default values.
    for (auto snapshot : pedalboard->snapshots())
    {
        if (snapshot)
        {
            UpdateDefaults(snapshot.get(), itemMap);
        }
    }
}

PluginPresets PiPedalModel::GetPluginPresets(const std::string &pluginUri)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);

    return storage.GetPluginPresets(pluginUri);
}

PluginUiPresets PiPedalModel::GetPluginUiPresets(const std::string &pluginUri)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);

    return storage.GetPluginUiPresets(pluginUri);
}

void PiPedalModel::LoadPluginPreset(int64_t pluginInstanceId, uint64_t presetInstanceId)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);

    PedalboardItem *pedalboardItem = this->pedalboard.GetItem(pluginInstanceId);
    if (pedalboardItem != nullptr)
    {
        int32_t oldStateUpdateCount = pedalboardItem->stateUpdateCount();

        PluginPresetValues presetValues = storage.GetPluginPresetValues(pedalboardItem->uri(), presetInstanceId);
        // if the plugin has state, we have to rebuild the pedalboard, since setting state is not thread-safe.
        // Same goes if lilvPresetUri is not empty.

        // lilvPresetUri: use lilv to load the preset from the RDF model. Occurs when using a factory preset
        // that has state:state, because lilv doesn't allow us to read this data, but does load it.
        // This is a transient condition.

        for (auto &control : presetValues.controls)
        {
            this->pedalboard.SetControlValue(pluginInstanceId, control.key(), control.value());
        }

        if ((!presetValues.state.isValid_) && presetValues.lilvPresetUri.empty() && presetValues.pathProperties.empty())
        {
            // fast path for control changes only.
            // (While a full build is outstanding, the running pedalboard's instance ids may name unrelated
            // plugins; the installer applies the model's control values when the new pedalboard is installed.)
            if (RunningInstanceIdsMatchPedalboard())
            {
                audioHost->SetPluginPreset(pluginInstanceId, presetValues.controls);
            }

            std::vector<IPiPedalModelSubscriber::ptr> t{subscribers.begin(), subscribers.end()};
            for (auto &subscriber : t)
            {
                subscriber->OnLoadPluginPreset(pluginInstanceId, presetValues.controls);
            }
        }
        else
        {
            pedalboardItem->lv2State(presetValues.state);
            pedalboardItem->lilvPresetUri(presetValues.lilvPresetUri);
            pedalboardItem->stateUpdateCount(oldStateUpdateCount + 1);
            pedalboardItem->pathProperties(presetValues.pathProperties);
            FirePedalboardChanged(-1); // does a complete reload of both client and audio server.
        }
        this->SetPresetChanged(-1, true);
    }
}

void PiPedalModel::DeleteAtomOutputListeners(int64_t clientId)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    for (size_t i = 0; i < atomOutputListeners.size(); ++i)
    {
        if (atomOutputListeners[i].clientId == clientId)
        {
            atomOutputListeners.erase(atomOutputListeners.begin() + i);
            --i;
        }
    }
    if (audioHost)
    {
        audioHost->SetListenForAtomOutput(atomOutputListeners.size() != 0);
    }
}

void PiPedalModel::DeleteMidiListeners(int64_t clientId)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    for (size_t i = 0; i < midiEventListeners.size(); ++i)
    {
        if (midiEventListeners[i].clientId == clientId)
        {
            midiEventListeners.erase(midiEventListeners.begin() + i);
            --i;
        }
    }
    if (audioHost)
    {
        audioHost->SetListenForMidiEvent(midiEventListeners.size() != 0);
    }
}

void PiPedalModel::OnPatchSetReply(uint64_t instanceId, IEffect *sourceEffect, LV2_URID patchSetProperty, const LV2_Atom *atomValue)
{
    std::vector<IPiPedalModelSubscriber::ptr> subscribers;
    std::vector<AtomOutputListener> atomOutputListeners;
    std::string propertyUri;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        uint64_t installedInstanceId = 0;
        if (!RunningInstanceIdsMatchPedalboard() || !FindInstalledSender(this->lv2Pedalboard.get(), sourceEffect, &installedInstanceId))
        {
            // From the outgoing pedalboard (see OnNotifyMidiValueChanged()).
            return;
        }
        instanceId = installedInstanceId;

        subscribers = this->subscribers;
        atomOutputListeners = this->atomOutputListeners;
        propertyUri = pluginHost.GetMapFeature().UridToString(patchSetProperty);

        {
            PedalboardItem *item = pedalboard.GetItem((int64_t)instanceId);
            if (item == nullptr)
                return;
            atom_object atomObject{atomValue};

            PedalboardItem::PropertyMap &properties = item->PatchProperties();
            if (properties.contains(propertyUri) && properties[propertyUri] == atomObject)
            {
                // do noting.
            }
            else
            {
                properties[propertyUri] = std::move(atomObject);
            }
        }
        if (audioHost)
        {
            audioHost->SetListenForAtomOutput(atomOutputListeners.size() != 0);
        }
    }
    OnNotifyMaybeLv2StateChanged(instanceId);

    bool hasAtomJson = false;
    std::string atomJson;

    for (int i = 0; i < atomOutputListeners.size(); ++i)
    {
        auto &listener = atomOutputListeners[i];
        if (listener.WantsProperty(instanceId, patchSetProperty))
        {
            auto subscriber = this->GetNotificationSubscriber(listener.clientId);
            if (subscriber)
            {
                if (!hasAtomJson)
                {
                    atomJson = this->audioHost->AtomToJson(atomValue);
                    hasAtomJson = true;
                }
                subscriber->OnNotifyPatchProperty(listener.clientHandle, instanceId, propertyUri, atomJson);
            }
            else
            {
                atomOutputListeners.erase(atomOutputListeners.begin() + i);
                --i;
            }
        }
    }
}

void PiPedalModel::OnNotifyPathPatchPropertyReceived(
    int64_t instanceId,
    const IEffect *sourceEffect,
    LV2_URID pathPatchProperty,
    LV2_Atom *pathProperty)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);

    // Identify the sender by address in the installed pedalboard. Just after a pedalboard is installed, until the
    // audio thread has swapped it in, notifications still come from the outgoing pedalboard, whose instance ids
    // may name different (non-reused) instances of the installed one; and a reused instance is only re-keyed to
    // its new instance id at the swap.
    uint64_t installedInstanceId = 0;
    if (!FindInstalledSender(this->lv2Pedalboard.get(), sourceEffect, &installedInstanceId))
    {
        return; // from an outgoing pedalboard that is being replaced.
    }
    IEffect *effect = this->lv2Pedalboard->GetEffect(installedInstanceId); // == sourceEffect
    instanceId = (int64_t)installedInstanceId;

    std::string pathPatchPropertyUri = this->pluginHost.Lv2UridToString(pathPatchProperty);
    std::string atomString = atomConverter.ToString(pathProperty);
    auto pedalboardItem = this->pedalboard.GetItem(instanceId);

    if (effect->IsLv2Effect() && !effect->IsVst3())
    {
        // keep the instance's own record of its path properties current (FindReusableInstances, snapshots).
        ((Lv2Effect *)effect)->SetPathPatchProperty(pathPatchPropertyUri, atomString);
    }

    if (pedalboardItem == nullptr || !RunningInstanceIdsMatchPedalboard())
    {
        return;
    }

    auto i = pedalboardItem->pathProperties_.find(pathPatchPropertyUri);
    if (i != pedalboardItem->pathProperties_.end())
    {
        std::string abstractAtomString = storage.ToAbstractPathFromJson(atomString);
        pedalboardItem->pathProperties_[pathPatchPropertyUri] = abstractAtomString;

        std::vector<IPiPedalModelSubscriber::ptr> t{subscribers.begin(), subscribers.end()};
        for (auto &subscriber : t)
        {
            subscriber->OnNotifyPathPatchPropertyChanged(
                instanceId,
                pathPatchPropertyUri,
                abstractAtomString);
        }
    }
}

void PiPedalModel::OnNotifyMidiListen(uint8_t cc0, uint8_t cc1, uint8_t cc2)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    bool isNote = (cc0 & 0xF0) == 0x90; // Note On
    if (isNote && cc2 == 0)
    {
        return; // Note off. Oopsie.
    }
    bool isControl = (cc0 & 0xF0) == 0xB0; // Control Change
    if (!isNote && !isControl)
    {
        return; // Not a note on or control change.
    }

    for (int i = 0; i < midiEventListeners.size(); ++i)
    {
        auto &listener = midiEventListeners[i];
        auto subscriber = this->GetNotificationSubscriber(listener.clientId);
        if (subscriber)
        {
            subscriber->OnNotifyMidiListener(listener.clientHandle, cc0, cc1, cc2);
        }
        else
        {
            midiEventListeners.erase(midiEventListeners.begin() + i);
            --i;
        }
    }
    audioHost->SetListenForMidiEvent(midiEventListeners.size() != 0);
}

void PiPedalModel::ListenForMidiEvent(int64_t clientId, int64_t clientHandle)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    MidiListener listener{clientId, clientHandle};
    midiEventListeners.push_back(listener);
    audioHost->SetListenForMidiEvent(true);
}

void PiPedalModel::CancelListenForMidiEvent(int64_t clientId, int64_t clientHandle)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    for (size_t i = 0; i < midiEventListeners.size(); ++i)
    {
        const auto &listener = midiEventListeners[i];
        if (listener.clientId == clientId && listener.clientHandle == clientHandle)
        {
            midiEventListeners.erase(midiEventListeners.begin() + i);
            break;
        }
    }
    if (midiEventListeners.size() == 0)
    {
        audioHost->SetListenForMidiEvent(false);
    }
}

void PiPedalModel::MonitorPatchProperty(int64_t clientId, int64_t clientHandle, uint64_t instanceId, const std::string &propertyUri)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    LV2_URID propertyUrid = 0;
    if (propertyUri.length() != 0)
    {
        propertyUrid = pluginHost.GetMapFeature().GetUrid(propertyUri.c_str());
    }
    AtomOutputListener listener{clientId, clientHandle, instanceId, propertyUrid};
    atomOutputListeners.push_back(listener);
    audioHost->SetListenForAtomOutput(true);

    PedalboardItem *item = this->pedalboard.GetItem(instanceId);
    if (item)
    {
        auto &map = item->pathProperties();
        if (map.contains(propertyUri))
        {
            const auto &value = map.at(propertyUri);
            if (value != "null")
            {
                try
                {
                    std::string json = storage.FromAbstractPathJson(value);

                    for (auto &subscriber : this->subscribers)
                    {
                        if (subscriber->GetClientId() == clientId)
                        {
                            subscriber->OnNotifyPatchProperty(clientHandle, instanceId, propertyUri, json);
                        }
                    }
                }
                catch (const std::exception &ignored)
                {
                }
            }
        }
    }
}

void PiPedalModel::CancelMonitorPatchProperty(int64_t clientId, int64_t clientHandle)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    for (size_t i = 0; i < atomOutputListeners.size(); ++i)
    {
        const auto &listener = atomOutputListeners[i];
        if (listener.clientId == clientId && listener.clientHandle == clientHandle)
        {
            atomOutputListeners.erase(atomOutputListeners.begin() + i);
            break;
        }
    }
    if (midiEventListeners.size() == 0)
    {
        audioHost->SetListenForMidiEvent(false);
    }
}

std::vector<AlsaDeviceInfo> PiPedalModel::GetAlsaDevices()
{
    std::vector<AlsaDeviceInfo> result = this->alsaDevices.GetAlsaDevices();
#ifdef JUNK
    // Useful for debugging non-stereo device configurations
    result.push_back(MakeDummyDeviceInfo(1));
    result.push_back(MakeDummyDeviceInfo(2));
    result.push_back(MakeDummyDeviceInfo(8));
#endif
    return result;
}

const std::filesystem::path &PiPedalModel::GetWebRoot() const
{
    return webRoot;
}

std::map<std::string, bool> PiPedalModel::GetFavorites() const
{
    std::lock_guard<std::recursive_mutex> guard(const_cast<std::recursive_mutex &>(mutex));

    return storage.GetFavorites();
}
void PiPedalModel::SetFavorites(const std::map<std::string, bool> &favorites)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    storage.SetFavorites(favorites);

    // take a snapshot incase a client unsusbscribes in the notification handler (in which case the mutex won't protect us)
    std::vector<IPiPedalModelSubscriber::ptr> t{subscribers.begin(), subscribers.end()};
    for (auto &subscriber : t)
    {
        subscriber->OnFavoritesChanged(favorites);
    }
}
std::vector<MidiBinding> PiPedalModel::GetSystemMidiBidings()
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return this->systemMidiBindings;
}
void PiPedalModel::SetSystemMidiBindings(std::vector<MidiBinding> &bindings)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    this->systemMidiBindings = bindings;
    storage.SetSystemMidiBindings(bindings);
    if (this->audioHost)
    {
        this->audioHost->SetSystemMidiBindings(bindings);
    }

    std::vector<IPiPedalModelSubscriber::ptr> t{subscribers.begin(), subscribers.end()};
    for (auto &subscriber : t)
    {
        subscriber->OnSystemMidiBindingsChanged(bindings);
    }
}

PedalboardItem *PiPedalModel::GetPedalboardItemForFileProperty(const UiFileProperty &fileProperty)
{
    for (PedalboardItem *pedalboardItem : this->pedalboard.GetAllPlugins())
    {
        if (pedalboardItem->pathProperties_.contains(fileProperty.patchProperty()))
        {
            return pedalboardItem;
        }
    }
    return nullptr;
}
FileRequestResult PiPedalModel::GetFileList2(const std::string &relativePath_, const UiFileProperty &fileProperty)
{
    std::string relativePath = relativePath_;
    try
    {
        if (!storage.IsInUploadsDirectory(relativePath))
        {

            // if relativePath is in a resource directory of the plugin, then we have loaded a factory preset or are using a default property.
            // map the resource path to the corresponding file in the uploads directory.
            // :-(
            PedalboardItem *pedalboardItem = GetPedalboardItemForFileProperty(fileProperty);
            if (pedalboardItem)
            {
                auto pluginInfo = GetPluginInfo(pedalboardItem->uri());
                if (pluginInfo)
                {
                    std::filesystem::path resourcePath = fs::path(pluginInfo->bundle_path()) / fileProperty.resourceDirectory();
                    if (IsSubdirectory(relativePath, resourcePath))
                    {
                        fs::path t = MakeRelativePath(relativePath, resourcePath);
                        t = fileProperty.directory() / t;
                        if (fs::exists(t))
                        {
                            relativePath = t;
                        }
                    }
                }
            }
        }
        return this->storage.GetFileList2(relativePath, fileProperty);
    }
    catch (const std::exception &e)
    {
        Lv2Log::warning("GetFileList() failed:  (%s)", e.what());
        throw;
    }
}

std::string PiPedalModel::RenameFilePropertyFile(
    const std::string &oldRelativePath,
    const std::string &newRelativePath,
    const UiFileProperty &uiFileProperty)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return storage.RenameFilePropertyFile(oldRelativePath, newRelativePath, uiFileProperty);
}

std::string PiPedalModel::CopyFilePropertyFile(
    const std::string &oldRelativePath,
    const std::string &newRelativePath,
    const UiFileProperty &uiFileProperty,
    bool overwrite)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return storage.CopyFilePropertyFile(oldRelativePath, newRelativePath, uiFileProperty, overwrite);
}

void PiPedalModel::DeleteSampleFile(const std::filesystem::path &fileName)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    storage.DeleteSampleFile(fileName);
}

std::string PiPedalModel::CreateNewSampleDirectory(const std::string &relativePath, const UiFileProperty &uiFileProperty)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return storage.CreateNewSampleDirectory(relativePath, uiFileProperty);
}
FilePropertyDirectoryTree::ptr PiPedalModel::GetFilePropertydirectoryTree(const UiFileProperty &uiFileProperty, const std::string &selectedPath)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return storage.GetFilePropertydirectoryTree(uiFileProperty, selectedPath);
}

UiFileProperty::ptr PiPedalModel::FindLoadedPatchProperty(int64_t instanceId, const std::string &patchPropertyUri)
{

    auto pedalboardItems = pedalboard.GetAllPlugins();

    for (const auto &pedalboardItem : pedalboardItems)
    {
        if (pedalboardItem->instanceId() == instanceId)
        {
            Lv2PluginInfo::ptr pluginInfo = GetPluginInfo(pedalboardItem->uri());
            if (pluginInfo && pluginInfo->piPedalUI())
            {
                for (const auto &fileProperty : pluginInfo->piPedalUI()->fileProperties())
                    if (fileProperty->patchProperty() == patchPropertyUri)
                    {
                        return fileProperty;
                    }
            }
        }
    }
    return nullptr;
    throw std::runtime_error("Permission denied. Plugin not currently loaded.");
}

std::string PiPedalModel::UploadUserFile(const std::string &directory, int64_t instanceId, const std::string &patchProperty, const std::string &filename, std::istream &stream, size_t contentLength)
{
    UiFileProperty::ptr fileProperty = FindLoadedPatchProperty(instanceId, patchProperty);
    if (!fileProperty)
    {
        Lv2Log::error(SS("Upload fle: Permission denied. No currently-loaded plugin provides that patch property: " << patchProperty));
        throw std::runtime_error("Permission denied.");
    }
    return storage.UploadUserFile(directory, fileProperty, filename, stream, contentLength);
}

uint64_t PiPedalModel::CreateNewPreset()
{
    std::lock_guard<std::recursive_mutex> lock(mutex);

    return storage.CreateNewPreset();
}

void PiPedalModel::CheckForResourceInitialization(Pedalboard &pedalboard)
{
    for (auto item : pedalboard.GetAllPlugins())
    {
        if (!item->isSplit())
        {
            pluginHost.CheckForResourceInitialization(item->uri(), storage.GetPluginUploadDirectory());
        }
    }
}
Pedalboard &PiPedalModel::GetPedalboard()
{
    return this->pedalboard;
}
std::shared_ptr<Lv2Pedalboard> PiPedalModel::GetLv2Pedalboard()
{
    // test only.
    Lv2PedalboardErrorList errorMessages;
    std::shared_ptr<Lv2Pedalboard> lv2Pedalboard{this->pluginHost.CreateLv2Pedalboard(this->pedalboard, errorMessages)};
    if (errorMessages.size() != 0)
    {
        throw std::runtime_error(errorMessages[0].message);
    }
    return lv2Pedalboard;
}

void PiPedalModel::SetSelectedPedalboardPlugin(uint64_t clientId, uint64_t pedalboardId)
{
    // Thinking on this:
    // 1) do NOT mark the pedalboard as changed. This shouldn't set a change flag.
    // 2) do NOT broadcast the change. Whoever set it last controls what happens when the plugin is reloaded. Meh.
    // 3) Clients must be able to save a non-changed pedalboard.
    pedalboard.selectedPlugin(pedalboardId);
}

// Pedalboard loading and threading.
//
// Instantiating plugins (e.g. loading a NAM model or a convolution IR) can take hundreds of milliseconds,
// so Lv2Pedalboards are built on a dedicated non-realtime thread (pedalboardBuilder) without holding `mutex`.
// Websocket handlers, MIDI program changes, posted tasks and the AudioHost reader thread
// (OnNotifyMonitorPort) therefore never wait for plugin instantiation.
//
//  1. Under `mutex` (callers usually already hold it), LoadCurrentPedalboard() either applies a snapshot
//     (fast path: structure identical to the last installed pedalboard, and no build outstanding), or calls
//     RequestPedalboardBuild(), which copies this->pedalboard into a request and submits it.
//     UpdateCurrentPedalboard() submits a request that reuses the running effect instances, unless a full
//     build is still outstanding (PedalboardBuildModeTracker), in which case it is downgraded to a full build.
//     Submitting bumps the builder's generation, so any older pending/in-flight build becomes stale.
//     A pending request that has not started yet is replaced, so rapid successive changes coalesce.
//  2. The builder thread calls BuildPedalboard() with no model lock held (CrashGuardLock and
//     pluginHostConfigurationMutex held), producing an Lv2Pedalboard and its error list.
//  3. The builder thread calls InstallBuiltPedalboard(), which takes `mutex`, re-checks that the generation
//     is still current, and installs via audioHost->SetPedalboard(). Edits made to this->pedalboard while
//     building are then applied as a snapshot, and the realtime VU/monitor subscriptions are refreshed.
//     Stale builds (including builds that borrowed running effects) are discarded and freed on the builder
//     thread (never on the realtime thread); see TryInstallBuiltPedalboard().
//     Until the audio thread swaps the installed pedalboard in, notifications still come from the outgoing
//     one: path property notifications are matched to their sender by address, and realtime parameter
//     requests carry the instance-id lineage they are addressed to (Lv2Pedalboard::GetInstanceIdLineage()).
//
// Lock order: `mutex` -> pluginHostConfigurationMutex -> builder's internal mutex. The builder thread never
// holds pluginHostConfigurationMutex while taking `mutex`. Only the installer (on the builder thread, so no build
// is in progress) takes pluginHostConfigurationMutex under `mutex`; other code must not, since it is held for
// the whole of a build. Build failures are reported to clients via OnErrorMessage.
//
// Clients are notified (FirePedalboardChanged) as soon as the model changes; the audio thread switches when
// the build is installed. RestartAudio() invalidates outstanding builds (they were built for the old audio
// configuration). Close() and ~PiPedalModel() join the builder thread without holding `mutex`, before the
// audio host is closed; no build is installed after that.
bool PiPedalModel::LoadCurrentPedalboard()
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (previousPedalboardLoaded && pedalboardBuilder->IsIdle() && pedalboard.IsStructureIdentical(previousPedalboard))
    {
        CrashGuardLock crashGuardLock;
        // then we can send a snapshot update instead!
        Snapshot snapshot = pedalboard.MakeSnapshotFromCurrentSettings(previousPedalboard);
        audioHost->LoadSnapshot(snapshot, pluginHost);
        this->previousPedalboard = this->pedalboard;
        return true;
    }
    if (lastRequestedPedalboardValid && pedalboardBuilder &&
        lastRequestedGeneration == pedalboardBuilder->CurrentGeneration() &&
        !pedalboardBuilder->IsIdle() &&
        pedalboard.IsStructureIdentical(lastRequestedPedalboard))
    {
        // e.g. a snapshot selected while a preset build is in flight: the outstanding (current) build has the
        // same structure, and the installer applies this->pedalboard's current settings when it installs it.
        return true;
    }
    RequestPedalboardBuild(false);
    return true;
}

void PiPedalModel::RequestPedalboardBuild(bool reuseExistingEffects)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (closed || !pedalboardBuilder)
    {
        return;
    }
    PedalboardBuildRequest request;
    request.pedalboard = this->pedalboard;
    // A reuse request must never supersede an unfinished full build (the running pedalboard would then be
    // of an unrelated lineage, and borrowing matches effects by instance id).
    request.reuseExistingEffects = pedalboardBuilder->modeTracker.ResolveReuse(reuseExistingEffects);
    request.audioEpoch = this->audioEpoch; // full builds: rejected if the audio is restarted before install.
    lastRequestedPedalboard = this->pedalboard;
    lastRequestedGeneration = pedalboardBuilder->Submit(std::move(request));
    lastRequestedPedalboardValid = lastRequestedGeneration != 0;
}

// Runs on the pedalboard builder thread. Apart from capturing the running pedalboard (under `mutex`), it only
// uses the request and pluginHost (read-only, protected against configuration changes by
// pluginHostConfigurationMutex).
PedalboardBuildResult PiPedalModel::BuildPedalboard(PedalboardBuildRequest &request)
{
    PedalboardBuildResult result;
    // Full builds (preset/bank switches): running effect instances whose persisted state matches an item of the
    // new pedalboard (e.g. a NAM model that takes hundreds of ms to load) are reused instead of instantiated.
    ExistingEffectMap reusableEffects;
    if (request.reuseExistingEffects)
    {
        // Only the builder thread installs pedalboards, so this stays the running pedalboard until
        // this build is installed (or the audio is stopped by RestartAudio(), which bumps audioEpoch).
        std::lock_guard<std::recursive_mutex> lock(mutex);
        request.outgoingPedalboard = this->lv2Pedalboard;
        result.audioEpoch = this->audioEpoch;
    }
    else
    {
        // Same rules as borrowing by instance id: only from the pedalboard that is running now (captured here, at
        // build time), only within the current audio epoch, and a build that actually borrowed instances follows
        // the borrowing-build install rules (see TryInstallBuiltPedalboard). Borrowing doesn't modify the running
        // effects: their new buffers, instance ids and settings are staged in the new pedalboard and applied on
        // the audio thread when it is swapped in (Lv2Pedalboard::UpdateAudioPorts()).
        std::shared_ptr<Lv2Pedalboard> runningPedalboard;
        uint64_t runningAudioEpoch = 0;
        std::string uploadDirectory;
        {
            std::lock_guard<std::recursive_mutex> lock(mutex);
            if (!closed && this->lv2Pedalboard && !this->audioRestarting &&
                request.audioEpoch == this->audioEpoch &&
                pedalboardBuilder->modeTracker.CanReuseRunningInstances())
            {
                runningPedalboard = this->lv2Pedalboard;
                runningAudioEpoch = this->audioEpoch;
                uploadDirectory = storage.GetPluginUploadDirectory().string();
            }
        }
        if (runningPedalboard)
        {
            // Without `mutex`, so that websocket handlers, MIDI and the AudioHost reader thread don't wait for
            // the plugins' state save() calls. See FindReusableInstances() for why that is safe.
            reusableEffects = FindReusableInstances(request.pedalboard, *runningPedalboard, uploadDirectory);
            if (!reusableEffects.empty())
            {
                request.outgoingPedalboard = std::move(runningPedalboard);
                result.audioEpoch = runningAudioEpoch;
            }
        }
    }

    std::lock_guard<std::mutex> configLock(pluginHostConfigurationMutex);
    CrashGuardLock crashGuardLock;

    result.pluginHostConfigurationVersion = this->pluginHostConfigurationVersion;
    if (request.reuseExistingEffects)
    {
        result.lv2Pedalboard = std::shared_ptr<Lv2Pedalboard>(
            this->pluginHost.UpdateLv2PedalboardStructure(request.pedalboard, request.outgoingPedalboard.get(), result.errorMessages));
        result.borrowedRunningEffects = true;
    }
    else if (!reusableEffects.empty())
    {
        size_t candidates = reusableEffects.size();
        result.lv2Pedalboard = std::shared_ptr<Lv2Pedalboard>(
            this->pluginHost.CreateLv2PedalboardReusingInstances(request.pedalboard, reusableEffects, result.errorMessages));
        result.reusedInstances = result.lv2Pedalboard->GetBorrowedEffectCount();
        result.borrowedRunningEffects = result.reusedInstances != 0;
        Lv2Log::debug(SS("Pedalboard load: reused " << result.reusedInstances << " of " << candidates
                                                    << " matching plugin instance(s)."));
    }
    else
    {
        result.lv2Pedalboard = std::shared_ptr<Lv2Pedalboard>(
            this->pluginHost.CreateLv2Pedalboard(request.pedalboard, result.errorMessages));
        Lv2Log::debug("Pedalboard load: reused 0 plugin instances.");
    }
    return result;
}

// Called on the pedalboard builder thread, WITHOUT `mutex`.
// Returns the running effects that can be reused for items of `pedalboard`, keyed by the item's instance id.
//
// The caller holds a shared_ptr to runningPedalboard, which keeps it and its effects alive. What is read here,
// and why it doesn't need `mutex`:
//  - the pedalboard's effect list and instance ids, and the effects' plugin URIs: not modified once built.
//  - plugin state: Lv2Effect::GetLv2State() serializes the plugin's state save() per instance, so it can't run
//    concurrently with the model's state-save path (AudioHost::UpdatePluginState(), called under `mutex`).
//  - path properties: Lv2Effect::GetPathPatchProperties() copies the map under the instance's lock, which
//    SetPathPatchProperty() (path property notifications, snapshot loads; under `mutex`) also takes.
//  - uploadDirectory: copied by the caller under `mutex`.
// The plugins' instantiation-class functions (activate/deactivate/cleanup), which must not run concurrently
// with save(), only run when a pedalboard is installed (on this thread) or when an effect is destroyed
// (prevented by the shared_ptr). runningPedalboard can't be replaced meanwhile, since only this thread installs
// pedalboards. RestartAudio() can stop it, but then bumps audioEpoch, so a build that borrowed from it is
// discarded by the installer.
ExistingEffectMap PiPedalModel::FindReusableInstances(
    const Pedalboard &pedalboard, Lv2Pedalboard &runningPedalboard, const std::string &uploadDirectory)
{
    ExistingEffectMap result;

    std::vector<const PedalboardItem *> incoming = FlattenPedalboardItems(pedalboard);
    std::set<std::string> incomingUris;
    for (const PedalboardItem *item : incoming)
    {
        if (IsInstanceReuseCandidate(*item))
        {
            incomingUris.insert(item->uri());
        }
    }
    if (incomingUris.empty())
    {
        return result;
    }

    // Path properties are stored as json atoms, abstract (relative to the upload directory) in pedalboards,
    // but possibly absolute in the effect (when reported by the plugin). Compare normalized values.
    PathPropertyNormalizer normalizePath = [&uploadDirectory](const std::string &jsonAtom)
    {
        // (as Storage::ToAbstractPathFromJson(), without touching storage.)
        return AtomConverter::AbstractPath(json_variant::parse(jsonAtom), uploadDirectory).to_string();
    };

    // Describe the running instances by their *current* persisted state, read from the plugins: the
    // pedalboard they were built from may be out of date (e.g. a model file was changed since).
    std::vector<PedalboardItem> runningItems;
    std::map<int64_t, std::shared_ptr<IEffect>> runningEffects;
    auto &effects = runningPedalboard.GetSharedEffectList();
    for (size_t effectIndex = 0; effectIndex < effects.size(); ++effectIndex)
    {
        const std::shared_ptr<IEffect> &effect = effects[effectIndex];
        if (!effect->IsLv2Effect() || effect->IsVst3())
        {
            continue; // VST3 instances are never reused.
        }
        Lv2Effect *lv2Effect = (Lv2Effect *)effect.get();
        if (!incomingUris.contains(lv2Effect->PluginUri()))
        {
            continue;
        }
        int64_t instanceId = (int64_t)runningPedalboard.GetInstanceIdAt(effectIndex);
        if (runningEffects.contains(instanceId))
        {
            continue; // (can't happen) ambiguous.
        }
        std::optional<Lv2PluginState> state;
        try
        {
            Lv2PluginState liveState;
            if (lv2Effect->GetLv2State(&liveState))
            {
                state = std::move(liveState);
            }
        }
        catch (const std::exception &e)
        {
            // in doubt about the plugin's state: don't reuse it.
            Lv2Log::debug(SS("Not reusing " << lv2Effect->PluginUri() << ": can't read its state. " << e.what()));
            continue;
        }
        runningItems.push_back(DescribeRunningInstance(
            instanceId, lv2Effect->PluginUri(), state, lv2Effect->GetPathPatchProperties(), normalizePath));
        runningEffects[instanceId] = effect;
    }
    if (runningItems.empty())
    {
        return result;
    }

    // Normalize the incoming items' path properties the same way.
    std::vector<PedalboardItem> normalizedIncoming;
    normalizedIncoming.reserve(incoming.size());
    for (const PedalboardItem *item : incoming)
    {
        normalizedIncoming.push_back(DescribeIncomingItem(*item, normalizePath));
    }

    std::vector<const PedalboardItem *> runningPointers;
    for (const auto &item : runningItems)
    {
        runningPointers.push_back(&item);
    }
    std::vector<const PedalboardItem *> incomingPointers;
    for (const auto &item : normalizedIncoming)
    {
        incomingPointers.push_back(&item);
    }

    for (const auto &match : MatchReusableInstances(runningPointers, incomingPointers))
    {
        result[(uint64_t)match.first] = runningEffects.at(match.second);
    }
    return result;
}

static std::string SnapshotToJson(const Snapshot &snapshot)
{
    std::stringstream s;
    json_writer writer(s, true);
    writer.write(snapshot);
    return s.str();
}

void PiPedalModel::OnPedalboardBuildFailed(const std::string &message)
{
    // Runs on the pedalboard builder thread, no locks held.
    std::string error = SS("Failed to load pedalboard. " << message);
    Lv2Log::error(error);

    SubscriberList subscribers;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        // the running pedalboard may have been partially modified (borrowed effects); don't borrow again.
        pedalboardBuilder->modeTracker.OnRunningPedalboardDiscarded();
        pedalboardBuilder->InstallCompleted();
        // don't leave the audio thread deferring MIDI for a build that will never be installed.
        SendMidiAcks(pendingMidiAcks.OnBuildTerminated(pedalboardBuilder->IsIdle()));
        if (pedalboardBuilder->IsIdle())
        {
            FailDeferredPatchRequests("Failed to load the pedalboard.");
        }
        subscribers = this->subscribers;
    }
    for (auto &subscriber : subscribers)
    {
        subscriber->OnErrorMessage(error);
    }
}

// Runs on the pedalboard builder thread.
void PiPedalModel::InstallBuiltPedalboard(uint64_t generation, PedalboardBuildRequest &request, PedalboardBuildResult &result)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    bool installed = TryInstallBuiltPedalboard(generation, request, result);
    if (installed)
    {
        pedalboardBuilder->modeTracker.OnInstalled(request.reuseExistingEffects);
    }
    // under the lock, so that LoadCurrentPedalboard() sees an idle builder as soon as we're done.
    pedalboardBuilder->InstallCompleted();

    // Release MIDI program/snapshot acks that were waiting for this build. When installed, they follow the
    // ReplaceEffect (audioHost->SetPedalboard()) in the host->realtime ring, so the audio thread replays its
    // deferred MIDI against the new pedalboard.
    bool idle = pedalboardBuilder->IsIdle();
    SendMidiAcks(installed ? pendingMidiAcks.OnInstalled(generation, idle) : pendingMidiAcks.OnBuildTerminated(idle));

    // Patch property requests deferred while a full build was outstanding: send them to the installed pedalboard,
    // after the ReplaceEffect in the host->realtime ring. If this build was discarded and no newer one is coming,
    // they can never be answered.
    switch (OnBuildEndedDeferredPatchRequests(installed, RunningInstanceIdsMatchPedalboard(), idle))
    {
    case DeferredPatchRequestsAction::Send:
        FlushDeferredPatchRequests();
        break;
    case DeferredPatchRequestsAction::Fail:
        FailDeferredPatchRequests("The pedalboard was not loaded.");
        break;
    case DeferredPatchRequestsAction::Keep:
        break;
    }
}

// Called with `mutex` held, on the pedalboard builder thread. Returns true if the pedalboard was installed.
bool PiPedalModel::TryInstallBuiltPedalboard(uint64_t generation, PedalboardBuildRequest &request, PedalboardBuildResult &result)
{
    if (closed || !audioHost)
    {
        return false;
    }
    // A build is installed only if current, not during an audio restart, requested (or, if it borrowed running
    // effects, borrowed) in the current audio epoch, and built with the current plugin host configuration.
    // Otherwise it is discarded (freed by the builder thread, never the realtime thread). This includes builds
    // that borrowed running effects (reuse builds, and full builds that reused matching instances of the running
    // pedalboard; see BuildPedalboard()): borrowing only stages its changes in the new pedalboard until the swap,
    // so the running effects are left intact, and the build that superseded it borrows them again. Since only
    // current builds are installed, the reconciliation below always sees the pedalboard that was last requested.
    PedalboardInstallState state;
    state.reuseBuild = result.borrowedRunningEffects;
    state.isCurrent = pedalboardBuilder->IsCurrent(generation);
    state.audioRestarting = this->audioRestarting;
    state.buildAudioEpoch = result.borrowedRunningEffects ? result.audioEpoch : request.audioEpoch;
    state.currentAudioEpoch = this->audioEpoch;
    state.buildConfigurationVersion = result.pluginHostConfigurationVersion;
    {
        // Doesn't block: we are on the builder thread, so no build holds this mutex.
        std::lock_guard<std::mutex> configLock(pluginHostConfigurationMutex);
        state.currentConfigurationVersion = this->pluginHostConfigurationVersion;
    }
    if (!ShouldInstallBuiltPedalboard(state))
    {
        return false;
    }
    // A full build's instance ids may name unrelated plugins of the running pedalboard (ids collide across
    // presets): realtime parameter requests addressed to the running pedalboard's ids must not reach it if
    // the audio thread processes them after the swap (see Lv2Pedalboard::ProcessParameterRequests()).
    if (!request.reuseExistingEffects)
    {
        ++instanceIdLineage;
    }
    result.lv2Pedalboard->SetInstanceIdLineage(instanceIdLineage); // not yet visible to the audio thread.

    // Reused instances get the new items' control values and enabled state on the audio thread, when the
    // pedalboard is swapped in (Lv2Pedalboard::UpdateAudioPorts()).
    this->lv2Pedalboard = result.lv2Pedalboard;
    // result.errorMessages have already been logged by Lv2Pedalboard::Prepare (errors are also reported
    // through the effects themselves). They are kept in the result for callers that want them.

    CheckForResourceInitialization(request.pedalboard);
    audioHost->SetPedalboard(lv2Pedalboard);
    previousPedalboard = std::move(request.pedalboard);
    previousPedalboardLoaded = true;

    // Apply edits that were made to this->pedalboard while the build was in progress
    // (control changes, bypass, volumes). Structural changes would have submitted a newer build.
    if (this->pedalboard.IsStructureIdentical(previousPedalboard))
    {
        Snapshot current = this->pedalboard.MakeSnapshotFromCurrentSettings(previousPedalboard);
        Snapshot built = previousPedalboard.MakeSnapshotFromCurrentSettings(previousPedalboard);
        if (SnapshotToJson(current) != SnapshotToJson(built))
        {
            CrashGuardLock crashGuardLock;
            audioHost->LoadSnapshot(current, pluginHost);
        }
        if (this->pedalboard.input_volume_db() != previousPedalboard.input_volume_db())
        {
            audioHost->SetInputVolume(this->pedalboard.input_volume_db());
        }
        if (this->pedalboard.output_volume_db() != previousPedalboard.output_volume_db())
        {
            audioHost->SetOutputVolume(this->pedalboard.output_volume_db());
        }
        previousPedalboard = this->pedalboard;
    }

    UpdateRealtimeVuSubscriptions();
    UpdateRealtimeMonitorPortSubscriptions();
    return true;
}

void PiPedalModel::OnNotifyLv2RealtimeError(int64_t instanceId, const std::string &error)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);

    // Notify clients.
    size_t n = subscribers.size();
    std::vector<IPiPedalModelSubscriber::ptr> t{subscribers.begin(), subscribers.end()};
    for (auto &subscriber : t)
    {
        subscriber->OnErrorMessage(error);
    }
}
std::filesystem::path PiPedalModel::GetPluginUploadDirectory() const
{
    return storage.GetPluginUploadDirectory();
}

void PiPedalModel::OnLv2PluginsChanged()
{
    Lv2Log::info("Lv2 plugins have changed. Reloading plugins.");
    std::lock_guard<std::recursive_mutex> lock(mutex);
    {
        // Notify clients.
        std::vector<IPiPedalModelSubscriber::ptr> t{subscribers.begin(), subscribers.end()};
        for (auto &subscriber : t)
        {
            subscriber->OnLv2PluginsChanging();
        }
    }
    std::thread(
        [this]()
        {
            // wait for the message to propagate. It would be better to use some kind of flush()
            // operation, but it's not clear how to do that with asyncio.
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            restartListener();
        })
        .detach();
}
void PiPedalModel::SetRestartListener(std::function<void(void)> &&listener)
{
    this->restartListener = std::move(listener);
}

void PiPedalModel::OnUpdateStatusChanged(const UpdateStatus &updateStatus)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);

    if (this->currentUpdateStatus != updateStatus)
    {
        this->currentUpdateStatus = updateStatus;
        FireUpdateStatusChanged(this->currentUpdateStatus);
    }
}
void PiPedalModel::FireUpdateStatusChanged(const UpdateStatus &updateStatus)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);

    std::vector<IPiPedalModelSubscriber::ptr> t{subscribers.begin(), subscribers.end()};
    for (auto &subscriber : t)
    {
        subscriber->OnUpdateStatusChanged(updateStatus);
    }
}
UpdateStatus PiPedalModel::GetUpdateStatus()
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return updater->GetCurrentStatus();
}

void PiPedalModel::UpdateNow(const std::string &updateUrl)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    std::filesystem::path fileName, signatureName;
    updater->DownloadUpdate(updateUrl, &fileName, &signatureName);

    adminClient.InstallUpdate(fileName);
}
void PiPedalModel::ForceUpdateCheck()
{
    updater->ForceUpdateCheck();
}
void PiPedalModel::SetUpdatePolicy(UpdatePolicyT updatePolicy)
{
    updater->SetUpdatePolicy(updatePolicy);
}

static bool HasAlsaDevice(const std::vector<AlsaDeviceInfo> devices, const std::string &deviceId)
{
    for (auto &device : devices)
    {
        if (device.id_ == deviceId)
            return true;
    }
    return false;
}

void PiPedalModel::StartHotspotMonitoring()
{
    this->avahiService = std::make_unique<AvahiService>();

    SetThreadName("avahi"); // hack to name the avahi service thread.
    UpdateDnsSd();          // now that the server is running, publish a  DNS-SD announcement.
    SetThreadName("main");

    this->hotspotManager->Open();
}

void PiPedalModel::WaitForAudioDeviceToComeOnline()
{
    auto serverSettings = this->GetJackServerSettings();
    // Wait for selected audio device to be initialized.
    // It may take some time for ALSA to publish all available devices when rebooting.

    if (serverSettings.IsValid())
    {
        // wait up to 15 seconds for the midi device to come online.
        auto devices = GetAlsaDevices();
        bool found = false;
        if (HasAlsaDevice(devices, serverSettings.GetAlsaInputDevice()))
        {
            Lv2Log::info(SS("Found ALSA device " << serverSettings.GetAlsaInputDevice() << "."));
        }
        else
        {
            Lv2Log::info(SS("Waiting for ALSA device " << serverSettings.GetAlsaInputDevice() << "."));
            for (int i = 0; i < 5; ++i)
            {
                sleep(2);
                devices = GetAlsaDevices();
                if (HasAlsaDevice(devices, serverSettings.GetAlsaInputDevice()))
                {
                    found = true;
                    break;
                }
            }
            if (found)
            {
                Lv2Log::info(SS("Found ALSA device " << serverSettings.GetAlsaInputDevice() << "."));
            }
            else
            {
                Lv2Log::info(SS("ALSA device " << serverSettings.GetAlsaInputDevice() << " not found."));
            }
        }
    }
    else
    {
        Lv2Log::info("No ALSA device selected.");
    }

    // pre-cache device info before we let audio services run.
    GetAlsaDevices();
}

PiPedalModel::PostHandle PiPedalModel::Post(PostCallback &&fn)
{
    // I know. odd place to forward this to, but it's a very serviceable dispatcher implementation.
    // Why? because it's there, and PiPedalModel has no thread of its own to do dispatching.
    if (!hotspotManager)
    {
        throw std::runtime_error("Too early. It's not ready yet.");
    }
    return hotspotManager->Post(std::move(fn));
}
PiPedalModel::PostHandle PiPedalModel::PostDelayed(const clock::duration &delay, PostCallback &&fn)
{
    if (!hotspotManager)
    {
        throw std::runtime_error("Too early. It's not ready yet.");
    }
    return hotspotManager->PostDelayed(delay, std::move(fn));
}
bool PiPedalModel::CancelPost(PostHandle handle)
{
    if (!hotspotManager)
    {
        throw std::runtime_error("Too early. It's not ready yet.");
    }
    return hotspotManager->CancelPost(handle);
}

void PiPedalModel::CancelNetworkChangingTimer()
{
    if (networkChangingDelayHandle)
    {
        CancelPost(networkChangingDelayHandle);
        networkChangingDelayHandle = 0;
    }
}

std::vector<std::string> PiPedalModel::GetKnownWifiNetworks()
{
    if (!this->hotspotManager)
    {
        return std::vector<std::string>();
    }
    return this->hotspotManager->GetKnownWifiNetworks();
}

void PiPedalModel::OnNetworkChanging(bool ethernetConnected, bool hotspotConnected)
{
    CancelNetworkChangingTimer();
    this->networkChangingDelayHandle =
        PostDelayed(std::chrono::seconds(10), // takes a while for network configuration to be fully applied.
                    [this, ethernetConnected, hotspotConnected]()
                    {
                        this->networkChangingDelayHandle = 0;
                        OnNetworkChanged(ethernetConnected, hotspotConnected);
                    });

    // take a snapshot incase a client unsusbscribes in the notification handler (in which case the mutex won't protect us)
    std::vector<IPiPedalModelSubscriber::ptr> t{subscribers.begin(), subscribers.end()};
    for (auto &subscriber : t)
    {
        subscriber->OnNetworkChanging(hotspotConnected);
    }
}
void PiPedalModel::OnNetworkChanged(bool ethernetConnected, bool hotspotConnected)
{
    FireNetworkChanged();
}

void PiPedalModel::OnNotifyMidiRealtimeEvent(RealtimeMidiEventType eventType)
{
    try
    {
        switch (eventType)
        {
        case RealtimeMidiEventType::Shutdown:
        {
            this->RequestShutdown(false);
        }
        break;
        case RealtimeMidiEventType::Reboot:
        {
            this->RequestShutdown(true);
        }
        break;
        case RealtimeMidiEventType::StartHotspot:
        {
            WifiConfigSettings settings = storage.GetWifiConfigSettings();
            if (!settings.hasSavedPassword_)
            {
                throw std::runtime_error("Can't start Wi-Fi hotspot because no password has been configured.");
            }
            settings.autoStartMode_ = (uint16_t)HotspotAutoStartMode::Always;
            this->SetWifiConfigSettings(settings);
        }
        break;
        case RealtimeMidiEventType::StopHotspot:
        {
            WifiConfigSettings settings = storage.GetWifiConfigSettings();
            settings.autoStartMode_ = (uint16_t)HotspotAutoStartMode::Never;
            this->SetWifiConfigSettings(settings);
        }
        break;

        default:
            break;
        }
    }
    catch (const std::exception &e)
    {
        Lv2Log::error(SS("Failed to process realtime MIDI event. " << e.what()));
    }
}

void PiPedalModel::RequestShutdown(bool restart)
{
    if (GetAdminClient().CanUseAdminClient())
    {
        GetAdminClient().RequestShutdown(restart);
    }
    else
    {
        // ONLY works when interactively logged in.
        std::stringstream s;
        s << "/usr/sbin/shutdown ";
        if (restart)
        {
            s << "-r";
        }
        else
        {
            s << "-P";
        }
        s << " now";

        if (sysExec(s.str().c_str()) != EXIT_SUCCESS)
        {
            Lv2Log::error("shutdown failed.");
            if (restart)
            {
                throw new PiPedalStateException("Restart request failed.");
            }
            else
            {
                throw new PiPedalStateException("Shutdown request failed.");
            }
        }
    }
}

void PiPedalModel::SetHasWifi(bool hasWifi)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (this->hasWifi != hasWifi)
    {
        this->hasWifi = hasWifi;

        std::vector<IPiPedalModelSubscriber::ptr> t{subscribers.begin(), subscribers.end()};

        for (auto &subscriber : t)
        {
            subscriber->OnHasWifiChanged(hasWifi);
        }
    }
}
bool PiPedalModel::GetHasWifi()
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return hasWifi;
}

std::map<std::string, std::string> PiPedalModel::GetWifiRegulatoryDomains()
{
    std::map<std::string, std::string> result;
    try
    {
        auto &regDb = RegDb::GetInstance();
        result = regDb.getRegulatoryDomains(storage.GetConfigRoot() / "iso_codes.json");
    }
    catch (const std::exception &e)
    {
        Lv2Log::warning(SS("Unable to query Wifi Regulatory domains. " << e.what()));
    }
    return result;
}

void PiPedalModel::CancelAudioRetry()
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (audioRetryPostHandle)
    {
        // don't think this can ever happen, but if it did, it would be bad.
        this->CancelPost(audioRetryPostHandle);
        audioRetryPostHandle = 0;
    }
}

void PiPedalModel::OnAlsaDriverTerminatedAbnormally()
{
    // notification from the realtime thread, via the audiohost that the
    // ALSA stream has broken. We want to restart.

    // get off the service thread as promptly as possible
    this->Post([&]()
               {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        if (closed) return;

        auto now = clock::now();
        clock::duration timeSinceLastRetry = now-this->lastRestartTime;
        this->lastRestartTime = now;
        if (timeSinceLastRetry > std::chrono::duration_cast<clock::duration>(std::chrono::milliseconds(1000))) {
            audioRestartRetries = 0;
        }
        CancelAudioRetry();

        if (audioRestartRetries == 0)
        {
            this->audioRetryPostHandle = this->Post(
                // No lock to avoid deadlocks!
                [this]() {
                    Lv2Log::info("Restarting audio.");
                    this->RestartAudio();
                });
            ++audioRestartRetries;
        } else if (audioRestartRetries < 3) 
        {
            this->audioRetryPostHandle = this->PostDelayed(
                std::chrono::milliseconds(100 * audioRestartRetries),
                [this]() {
                    if (closed) {
                        return;
                    }
                    Lv2Log::info(SS("Restarting audio. (retry " << audioRestartRetries << ")"));

                    RestartAudio();
                });
            ++audioRestartRetries;
        } else if (audioRestartRetries == 3)  // one attempt to start the dummy driver.
        {
            {
                this->audioRetryPostHandle = this->Post(
                    // No lock to avoid deadlocks!
                    [this]() {
                        Lv2Log::info(SS("Switching to dummy driver."));
                        RestartAudio(true); // switch to the dummy driver.
                    });
            } 
            ++audioRestartRetries;
        } else {
            Lv2Log::error(SS("Unable to reastart audio."));

        } });
}

bool PiPedalModel::IsInUploadsDirectory(const std::string &path)
{
    return storage.IsInUploadsDirectory(path);
}

void PiPedalModel::MoveAudioFile(
    const std::string &directory,
    int32_t fromPosition,
    int32_t toPosition)
{
    if (directory.empty())
    {
        throw std::runtime_error("Directory is empty.");
    }
    AudioDirectoryInfo::Ptr dir = AudioDirectoryInfo::Create(directory);
    dir->MoveAudioFile(directory, fromPosition, toPosition);
}
void PiPedalModel::SetPedalboardItemTitle(int64_t instanceId, const std::string &title, const std::string &colorKey)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (!this->pedalboard.SetItemTitle(instanceId, title, colorKey))
    {
        return;
    }
    // no need to reload the pedalboard, but we do need to notify subscribers.
    this->SetPresetChanged(-1, true);
    this->FirePedalboardChanged(-1, false);
}

std::vector<PresetIndexEntry> PiPedalModel::RequestBankPresets(int64_t bankInstanceId)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);

    return storage.RequestBankPresets(bankInstanceId);
}

int64_t PiPedalModel::ImportPresetsFromBank(int64_t bankInstanceId, const std::vector<int64_t> &presets)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    uint64_t lastAdded = storage.ImportPresetsFromBank(bankInstanceId, presets);

    FirePresetsChanged(-1);
    return lastAdded;
}
int64_t PiPedalModel::CopyPresetsToBank(int64_t bankInstanceId, const std::vector<int64_t> &presets)
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    uint64_t lastAdded = storage.CopyPresetsToBank(bankInstanceId, presets);
    return lastAdded;
}

ChannelRouterSettings::ptr PiPedalModel::GetChannelRouterSettings()
{
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return this->channelRouterSettings;
}

void PiPedalModel::SetChannelRouterSettings(int64_t clientId, ChannelRouterSettings::ptr &settings)
{
    JackConfiguration jackConfiguration;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        this->channelRouterSettings = settings;
        this->storage.SetChannelRouterSettings(settings);
        jackConfiguration = this->jackConfiguration;
        CancelAudioRetry();
    }
    {
        // Not under `mutex`: this waits for any in-flight pedalboard build, which must not block the model.
        std::lock_guard<std::mutex> configLock(pluginHostConfigurationMutex);
        ++pluginHostConfigurationVersion;
        this->pluginHost.OnConfigurationChanged(jackConfiguration, *settings);
    }
    RestartAudio(); // no lock to avoid mutex deadlock when reader thread is sending notifications..

    this->FireChannelRouterSettingsChanged(clientId);
}

std::string PiPedalModel::Tone3000ThumbnailDirectory()
{
    return TONE3000_THUMBNAILS_ROOT.string();
}
std::string PiPedalModel::OldTone3000ThumbnailDirectory()
{
    return LEGACY_TONE3000_THUMBNAILS_ROOT.string();
}

void PiPedalModel::EnableUpdater(bool enable)
{
    this->updaterEnabled = enable;
}

static bool IsSafeMediaPath(const fs::path path)
{
    return IsPathInAudioUploads(path);
}

void PiPedalModel::WriteTone3000Readme(const std::filesystem::path &filePath, const tone3000::Tone &tone, const std::string &thumbnailUrl)
{
    if (!IsSafeMediaPath(filePath))
    {
        throw std::runtime_error("Invalid media path.");
    }
    tone3000::WriteTone3000Readme(filePath, tone, thumbnailUrl);
}
