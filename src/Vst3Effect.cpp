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

#include "ss.hpp"
#include <assert.h>
#include "PluginHost.hpp"

#include "vst3/Vst3Host.hpp"
#include "Lv2Log.hpp"
#include <unordered_map>
#include <set>
#include <unordered_set>
#include <filesystem>

#include "RingBufferReader.hpp"

#include "base/source/fdebug.h" // defines the NEW allocation macro.
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/common/memorystream.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstunits.h"
#include "public.sdk/source/vst/utility/stringconvert.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/processdata.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"

#include "public.sdk/samples/vst-hosting/audiohost/source/media/iparameterclient.h"
#include "public.sdk/samples/vst-hosting/audiohost/source/media/imediaserver.h"

#include <algorithm>
#include <array>

#include <sstream>
#include "LiteralVersion.hpp"
#include <cerrno>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "Vst3MidiToEvent.hpp"

#include "vst3/Vst3EffectImpl.hpp"
#include "PluginHost.hpp"

using namespace pipedal;

std::unique_ptr<Vst3Effect> Vst3Effect::CreateInstance(uint64_t instanceId, const Vst3PluginInfo &info, IHost *pHost)
{
	std::unique_ptr<Vst3EffectImpl> result = std::make_unique<Vst3EffectImpl>();

	result->Load(instanceId, info, pHost);


	return std::move(result);
}

static double NaNGuard(double value)
{
	if (std::isnan(value)) return 0;
	if (std::isinf(value)) return 0;
	return value;
}

static uint32_t GetRefCount(FUnknown *p)
{
	p->addRef();
	return p->release();
}

//------------------------------------------------------------------------
// From Vst2Wrapper
static MidiCCMapping initMidiCtrlerAssignment(IComponent *component, IMidiMapping *midiMapping)
{
	MidiCCMapping midiCCMapping{};

	if (!midiMapping || !component)
		return midiCCMapping;

	int32 busses = std::min<int32>(component->getBusCount(kEvent, kInput), kMaxMidiMappingBusses);

	if (midiCCMapping[0][0].empty())
	{
		for (int32 b = 0; b < busses; b++)
			for (int32 i = 0; i < kMaxMidiChannels; i++)
				midiCCMapping[b][i].resize(Vst::kCountCtrlNumber);
	}

	ParamID paramID;
	for (int32 b = 0; b < busses; b++)
	{
		for (int16 ch = 0; ch < kMaxMidiChannels; ch++)
		{
			for (int32 i = 0; i < Vst::kCountCtrlNumber; i++)
			{
				paramID = kNoParamId;
				if (midiMapping->getMidiControllerAssignment(b, ch, (CtrlNumber)i, paramID) ==
					kResultTrue)
				{
					// TODO check if tag is associated to a parameter
					midiCCMapping[b][ch][i] = paramID;
				}
				else
					midiCCMapping[b][ch][i] = kNoParamId;
			}
		}
	}
	return midiCCMapping;
}

//------------------------------------------------------------------------
static void assignBusBuffers(const IAudioClient::Buffers &buffers, HostProcessData &processData,
							 bool unassign = false)
{
	// Set outputs
	auto bufferIndex = 0;
	for (auto busIndex = 0; busIndex < processData.numOutputs; busIndex++)
	{
		auto channelCount = processData.outputs[busIndex].numChannels;
		for (auto chanIndex = 0; chanIndex < channelCount; chanIndex++)
		{
			if (bufferIndex < buffers.numOutputs)
			{
				processData.setChannelBuffer(BusDirections::kOutput, busIndex, chanIndex,
											 unassign ? nullptr : buffers.outputs[bufferIndex]);
				bufferIndex++;
			}
		}
	}

	// Set inputs
	bufferIndex = 0;
	for (auto busIndex = 0; busIndex < processData.numInputs; busIndex++)
	{
		auto channelCount = processData.inputs[busIndex].numChannels;
		for (auto chanIndex = 0; chanIndex < channelCount; chanIndex++)
		{
			if (bufferIndex < buffers.numInputs)
			{
				processData.setChannelBuffer(BusDirections::kInput, busIndex, chanIndex,
											 unassign ? nullptr : buffers.inputs[bufferIndex]);

				bufferIndex++;
			}
		}
	}
}

//------------------------------------------------------------------------
static void unassignBusBuffers(const IAudioClient::Buffers &buffers, HostProcessData &processData)
{
	assignBusBuffers(buffers, processData, true);
}

//------------------------------------------------------------------------
//  Vst3Processor
//------------------------------------------------------------------------
Vst3EffectImpl::Vst3EffectImpl()
{
	buffers.inputs = nullptr;
	buffers.outputs = nullptr;
	componentHandler.setEffect(this);
}

//------------------------------------------------------------------------
Vst3EffectImpl::~Vst3EffectImpl()
{
	StopControlWorker(); // before the controller goes away.
	delete[] buffers.inputs;
	delete[] buffers.outputs;
	terminate();
}

//------------------------------------------------------------------------

// True while this thread is inside IAudioProcessor::process(), i.e. on the
// audio thread. Lets host callbacks that a non-conforming plugin makes from
// process() (restartComponent()) avoid controller work there.
static thread_local bool tlsInVst3Process = false;

namespace
{
	// Scoped tlsInVst3Process, so that a process() that throws cannot leave
	// the flag set on this thread. Restores the previous value (nesting).
	class InVst3ProcessScope
	{
	public:
		InVst3ProcessScope() : previous(tlsInVst3Process) { tlsInVst3Process = true; }
		~InVst3ProcessScope() { tlsInVst3Process = previous; }
		InVst3ProcessScope(const InVst3ProcessScope &) = delete;
		InVst3ProcessScope &operator=(const InVst3ProcessScope &) = delete;

	private:
		bool previous;
	};
}

void Vst3EffectImpl::SetControl(int index, float value)
{
	// Called on the audio thread (ring buffer SetValue, MIDI bindings), so no
	// IEditController calls and no locks here: IEditController is not
	// realtime-safe per the VST3 spec (plainParamToNormalized is plugin code
	// that may allocate or lock). Record the plain value and let the control
	// worker normalize it; the normalized value comes back to the audio thread
	// through pendingNormalizedValues, picked up in preprocess().
	if (index < 0 || (size_t)index >= parameterValues.size())
	{
		return;
	}
	this->parameterValues[index] = value;
	pendingPlainValues.Set((size_t)index, value);
	controlWorkerWake.Post();
}

void Vst3EffectImpl::FlushControlChanges()
{
	std::lock_guard lock{controllerMutex};
	auto deliver = [this](size_t index, float plainValue)
	{
		ParamID paramId = lv2ToVstParam[index];
		// The plugin may call restartComponent() from inside either call
		// (re-entering on this thread). Mark the slot so that the nested
		// refresh doesn't overwrite parameterValues[index] -- the value we are
		// delivering -- with the controller's not-yet-updated value. A count,
		// not a flag: a nested pass may deliver a newer value for the same
		// index, and must not clear the outer pass's mark when it finishes.
		++controlFlushInFlight[index];
		ParamValue normalizedValue = controller->plainParamToNormalized(paramId, plainValue);
		controller->setParamNormalized(paramId, normalizedValue);
		--controlFlushInFlight[index];
		pendingNormalizedValues.Set(index, normalizedValue);
	};
	++controlFlushDepth;
	try
	{
		if (controlFlushDepth == 1)
		{
			pendingPlainValues.ForEachPending(deliver);
		}
		else
		{
			// Nested inside an outer pass, which has already claimed the
			// summary flag: scan every slot so that the values it hasn't
			// reached yet are delivered now (the outer pass then skips them).
			pendingPlainValues.ForEachPendingScanAll(deliver);
		}
	}
	catch (...)
	{
		if (--controlFlushDepth == 0)
		{
			std::fill(controlFlushInFlight.begin(), controlFlushInFlight.end(), 0);
		}
		throw;
	}
	--controlFlushDepth;
}

// The control worker is non-realtime, but parameter changes sit in
// pendingPlainValues until it runs, so its wake-up latency is the latency of
// every VST3 control change. Run it SCHED_OTHER (it must not inherit a
// realtime policy from whichever thread loaded the plugin), nudged to nice -5
// when permitted so that ordinary load does not starve it. EPERM/EACCES (no
// CAP_SYS_NICE, RLIMIT_NICE too low) just leaves it where it is. An inherited
// nice value that is already higher priority is kept.
static constexpr int VST3_CONTROL_WORKER_NICE = -5;

static void SetControlWorkerScheduling()
{
#ifdef __linux__
	sched_param param{};
	param.sched_priority = 0;
	if (sched_setscheduler(0, SCHED_OTHER, &param) != 0)
	{
		Lv2Log::debug(SS("VST3 control worker: failed to set SCHED_OTHER. (" << strerror(errno) << ")"));
	}

	pid_t tid = (pid_t)syscall(SYS_gettid);
	errno = 0;
	int currentNice = getpriority(PRIO_PROCESS, (id_t)tid);
	if (errno == 0 && currentNice > VST3_CONTROL_WORKER_NICE)
	{
		if (setpriority(PRIO_PROCESS, (id_t)tid, VST3_CONTROL_WORKER_NICE) != 0)
		{
			Lv2Log::debug(SS("VST3 control worker left at nice " << currentNice << ". (" << strerror(errno) << ")"));
		}
	}
#endif
}

void Vst3EffectImpl::ControlWorkerProc()
{
	SetControlWorkerScheduling();
	while (true)
	{
		controlWorkerWake.Wait();
		if (controlWorkerStopping.load())
		{
			break;
		}
		try
		{
			FlushControlChanges();
			// At most one restart per wake-up: requests that land while it
			// runs coalesce into the one flag (and one outstanding Post()),
			// so they cost exactly one more pass. A plugin that re-requests a
			// restart from every process() call therefore keeps the worker
			// doing one restart per wake-up, but no faster than the requests
			// arrive and without unbounded queueing; there is no correct
			// point at which to stop honouring the plugin's requests.
			if (deferredParamValuesRestart.exchange(false))
			{
				RestartParamValues();
			}
		}
		catch (const std::exception &e)
		{
			Lv2Log::error(SS(info.pluginInfo_.name() << ": " << e.what()));
		}
	}
}

void Vst3EffectImpl::StartControlWorker()
{
	controlWorkerStopping = false;
	controlWorkerThread = std::thread([this]() { ControlWorkerProc(); });
}

void Vst3EffectImpl::StopControlWorker()
{
	if (controlWorkerThread.joinable())
	{
		controlWorkerStopping = true;
		controlWorkerWake.Post();
		controlWorkerThread.join();
	}
}

void Vst3EffectImpl::Load(uint64_t instanceId, const Vst3PluginInfo &info, IHost *pHost)
{
	processContext = {};
	processContext.tempo = 120;


	this->pHost = pHost;
	this->instanceId = instanceId;
	this->info = info;
	// Built here, not on the audio thread: SetErrorMessage() must not allocate.
	this->processFailedMessage = info.pluginInfo_.name() + ": IAudioProcessor::process() failed.";

	size_t nControls = info.pluginInfo_.controls().size();

	inputParameterChanges.setMaxParameters(nControls);
	outputParameterChanges.setMaxParameters(nControls);

	lv2ToVstParam.resize(nControls);
	parameterValues.resize(nControls);
	pendingPlainValues.Resize(nControls);
	pendingNormalizedValues.Resize(nControls);
	controlFlushInFlight.assign(nControls, 0);
	for (size_t i = 0; i < nControls; ++i)
	{
		const auto &control = info.pluginInfo_.controls()[i];
		ParamID paramId;
		std::stringstream ss(control.symbol());
		ss >> paramId;

		assert(i == control.index());

		lv2ToVstParam[control.index()] = paramId;

		parameterValues[control.index()] = control.default_value();
		if (control.is_bypass())
		{
			bypassControl = i;
		}
	}

	std::string error;
	this->module = Module::create(info.filePath_, error);

	if (!module)
	{
		throw Vst3Exception(SS("Vst3 load failed: " << error << "(" << info.filePath_ << ")"));
	}
	const PluginFactory &factory = module->getFactory();

	ClassInfo myClassInfo;
	bool foundClassInfo = false;

	VST3::Optional<VST3::UID> myUid = VST3::UID::fromString(info.uid_);

	if (!myUid)
	{
		throw Vst3Exception(SS("Invalid UID"));
	}
	for (const ClassInfo &classInfo : factory.classInfos())
	{
		if (classInfo.category() == kVstAudioEffectClass &&
			classInfo.ID() == *myUid)
		{
			myClassInfo = classInfo;
			foundClassInfo = true;
		}
	}
	if (!foundClassInfo)
	{
		throw Vst3Exception(SS("Vst3 load failed: effect class not found in " << info.filePath_));
	}

	plugProvider = owned(NEW PlugProvider(factory, myClassInfo, true));

	// component and controller are borrowed pointers, kept alive by
	// plugProvider. getComponent()/getController() addRef() for the caller,
	// so adopt and drop those references here. (They used to be kept, and
	// never released, so the plugin objects outlived the module. JUCE plugins
	// keep their message and timer threads running while their objects are
	// alive, and those threads crashed in unmapped code once the module was
	// dlclose()d.)
	{
		OPtr<IComponent> ownedComponent = plugProvider->getComponent(); // also sets the plugin up
		OPtr<IEditController> ownedController = plugProvider->getController();
		this->component = ownedComponent.get();
		this->controller = ownedController.get();
	}
	if (!component || !controller)
	{
		throw Vst3Exception(SS(info.pluginInfo_.name() << ": failed to create the component or edit controller."));
	}
	this->processor = component;

	controller->queryInterface(IMidiMapping::iid, (void **)&midiMapping);

	if (midiMapping)
		midiCCMapping = initMidiCtrlerAssignment(component, midiMapping);

	setBlockSize((Steinberg::int32)pHost->GetMaxAudioBufferSize());
	setSamplerate((SampleRate)pHost->GetSampleRate());

	ProcessSetup setup{kRealtime, kSample32, blockSize, sampleRate};

	initProcessData();

	if (processor->setupProcessing(setup) != kResultOk)
	{
		throw Vst3Exception(SS(info.pluginInfo_.name() << ": setupProcessing() failed."));
	}

	if (component->setActive(true) != kResultOk)
		throw Vst3Exception(SS(info.pluginInfo_.name() << ": setActive() failed."));

	OPtr<IBStream> state = new MemoryStream();

	assert(GetRefCount(state.get()) == 1);
	this->supportsState = false;
	if (component->getState(state) == kResultOk)
	{
		this->supportsState = true;
		state->seek(0, IBStream::kIBSeekSet);
		controller->setComponentState(state);
		refreshControlValues();
	}
	IComponentHandler *handler;

	controller->setComponentHandler(&componentHandler);

	if (midiMapping)
		midiCCMapping = initMidiCtrlerAssignment(component, midiMapping);

	// (the component was already activated above; activating twice trips an
	// assert in stricter plugins, e.g. DPF-based ones.)

	for (size_t i = 0; i < this->lv2ToVstParam.size(); ++i)
	{
		fireControlChanged(i, (float)(controller->getParamNormalized(lv2ToVstParam[i])));
	}
	// Last: SetControl() may be called as soon as Load() returns.
	StartControlWorker();
}
//------------------------------------------------------------------------
//------------------------------------------------------------------------
// void Vst3PluginImpl::createLocalMediaServer (const Name& name)
// {
// 	mediaServer = createMediaServer (name);
// 	mediaServer->registerAudioClient (this);
// 	mediaServer->registerMidiClient (this);
// }

//------------------------------------------------------------------------

//------------------------------------------------------------------------
void Vst3EffectImpl::terminate()
{
	// mediaServer = nullptr;

	if (!processor)
		return;

	processor->setProcessing(false);
	component->setActive(false);
}

//------------------------------------------------------------------------
void Vst3EffectImpl::initProcessData()
{
	// processData.prepare will be done in setBlockSize

	buffers.numInputs = info.pluginInfo_.audio_inputs();
	buffers.numOutputs = info.pluginInfo_.audio_outputs();
	buffers.numSamples = 0;
	buffers.inputs = new float *[buffers.numInputs + 1];
	buffers.outputs = new float *[buffers.numOutputs + 1];

	for (size_t i = 0; i < buffers.numInputs + 1; ++i)
	{
		buffers.inputs[i] = nullptr;
	}
	for (size_t i = 0; i < buffers.numOutputs + 1; ++i)
	{
		buffers.outputs[i] = nullptr;
	}

	processData.inputEvents = &eventList;
	processData.inputParameterChanges = &inputParameterChanges;
	processData.outputParameterChanges = &outputParameterChanges;
	processData.processContext = &processContext;

	setSamplerate(pHost->GetSampleRate());
	setBlockSize((Steinberg::int32)pHost->GetMaxAudioBufferSize());
}

//------------------------------------------------------------------------
IMidiClient::IOSetup Vst3EffectImpl::getMidiIOSetup() const
{
	IMidiClient::IOSetup iosetup;
	auto count = component->getBusCount(MediaTypes::kEvent, BusDirections::kInput);
	for (int32_t i = 0; i < count; i++)
	{
		BusInfo info;
		if (component->getBusInfo(MediaTypes::kEvent, BusDirections::kInput, i, info) != kResultOk)
			continue;

		auto busName = VST3::StringConvert::convert(info.name, 128);
		iosetup.inputs.push_back(busName);
	}

	count = component->getBusCount(MediaTypes::kEvent, BusDirections::kOutput);
	for (int32_t i = 0; i < count; i++)
	{
		BusInfo info;
		if (component->getBusInfo(MediaTypes::kEvent, BusDirections::kOutput, i, info) !=
			kResultOk)
			continue;

		auto busName = VST3::StringConvert::convert(info.name, 128);
		iosetup.outputs.push_back(busName);
	}

	return iosetup;
}

//------------------------------------------------------------------------
IAudioClient::IOSetup Vst3EffectImpl::getIOSetup() const
{
	IAudioClient::IOSetup iosetup;
	auto count = component->getBusCount(MediaTypes::kAudio, BusDirections::kOutput);
	for (int32_t i = 0; i < count; i++)
	{
		BusInfo info;
		if (component->getBusInfo(MediaTypes::kAudio, BusDirections::kOutput, i, info) !=
			kResultOk)
			continue;

		for (int32_t j = 0; j < info.channelCount; j++)
		{
			auto channelName = VST3::StringConvert::convert(info.name, 128);
			iosetup.outputs.push_back(channelName + " " + std::to_string(j));
		}
	}

	count = component->getBusCount(MediaTypes::kAudio, BusDirections::kInput);
	for (int32_t i = 0; i < count; i++)
	{
		BusInfo info;
		if (component->getBusInfo(MediaTypes::kAudio, BusDirections::kInput, i, info) != kResultOk)
			continue;

		for (int32_t j = 0; j < info.channelCount; j++)
		{
			auto channelName = VST3::StringConvert::convert(info.name, 128);
			iosetup.inputs.push_back(channelName + " " + std::to_string(j));
		}
	}

	return iosetup;
}

//------------------------------------------------------------------------
void Vst3EffectImpl::preprocess(Buffers &buffers, int64_t continousFrames)
{
	processData.numSamples = buffers.numSamples;
	processContext.continousTimeSamples = continousFrames;
	assignBusBuffers(buffers, processData);

	// Lock-free: values were normalized off the audio thread by the control
	// worker. inputParameterChanges was sized to the parameter count in
	// Load(), so this does not allocate.
	pendingNormalizedValues.ForEachPending(
		[this](size_t index, double normalizedValue)
		{
			int32 queueIndex = 0;
			IParamValueQueue *queue = inputParameterChanges.addParameterData(lv2ToVstParam[index], queueIndex);
			if (queue)
			{
				int32 pointIndex = 0;
				queue->addPoint(0, normalizedValue, pointIndex);
			}
		});
	outputParameterChanges.clearQueue();
}

//------------------------------------------------------------------------
bool Vst3EffectImpl::process(Buffers &buffers, int64_t continousFrames)
{
	if (!processor || !isProcessing)
		return false;
	buffers.numSamples = continousFrames;
	preprocess(buffers, continousFrames);

	tresult processResult;
	{
		InVst3ProcessScope inProcess;
		processResult = processor->process(processData);
	}
	if (processResult != kResultOk)
	{
		// Previously this failure was swallowed: the plugin stopped producing
		// audio and nothing was reported. Surface it through the same
		// realtime error channel that LV2 effects use. Latched so a
		// persistently failing plugin reports once per episode instead of on
		// every audio block.
		if (!processFailed)
		{
			processFailed = true;
			SetErrorMessage(processFailedMessage.c_str());
		}
		return false;
	}
	processFailed = false;

	postprocess(buffers);

	return true;
}
//------------------------------------------------------------------------
void Vst3EffectImpl::postprocess(Buffers &buffers)
{
	eventList.clear();
	inputParameterChanges.clearQueue();
	unassignBusBuffers(buffers, processData);
}

void Vst3EffectImpl::SendControlChanges(RealtimeRingBufferWriter *realtimeRingBufferWriter)
{
	for (auto i = 0; i < outputParameterChanges.getParameterCount(); ++i)
	{
		IParamValueQueue *queue = outputParameterChanges.getParameterData(i);
		auto points = queue->getPointCount();
		if (points != 0)
		{
			Steinberg::Vst::ParamValue value;
			Steinberg::int32 sampleOffset;
			queue->getPoint(points - 1, sampleOffset, value);

			auto paramId = queue->getParameterId();

			int thisControlId = -1;
			for (size_t lv2Id = 0; lv2Id < this->lv2ToVstParam.size(); ++lv2Id)
			{
				if (lv2ToVstParam[lv2Id] == paramId)
				{
					thisControlId = (int)lv2Id;
					break;
				}
			}
			if (thisControlId != -1)
			{
				// realtimeRingBufferWriter->NotifyVst3ControlValue(this->instanceId,thisControlId,(float)value);
			}
		}
	}
}

//------------------------------------------------------------------------
bool Vst3EffectImpl::setSamplerate(SampleRate value)
{
	if (sampleRate == value)
		return true;

	sampleRate = value;
	processContext.sampleRate = sampleRate;
	if (blockSize == 0)
		return true;
	return true;
}

//------------------------------------------------------------------------
bool Vst3EffectImpl::setBlockSize(int32 value)
{
	blockSize = value;
	if (sampleRate == 0)
		return true;

	processData.prepare(*component, blockSize, kSample32);
	return true;
}

//------------------------------------------------------------------------

//------------------------------------------------------------------------
bool Vst3EffectImpl::isPortInRange(int32 port, int32 channel) const
{
	return port < kMaxMidiMappingBusses && !midiCCMapping[port][channel].empty();
}

//------------------------------------------------------------------------
bool Vst3EffectImpl::processVstEvent(const IMidiClient::Event &event, int32 port)
{
	auto vstEvent = midiToEvent(event.type, event.channel, event.data0, event.data1);
	if (vstEvent)
	{
		vstEvent->busIndex = port;
		if (eventList.addEvent(*vstEvent) != kResultOk)
		{
			assert(false && "Event was not added to EventList!");
		}

		return true;
	}

	return false;
}

//------------------------------------------------------------------------
bool Vst3EffectImpl::processParamChange(const IMidiClient::Event &event, int32 port)
{
	auto paramMapping = [port, this](int32 channel, MidiData data1) -> ParamID
	{
		if (!isPortInRange(port, channel))
			return kNoParamId;

		return midiCCMapping[port][channel][data1];
	};

	auto paramChange =
		midiToParameter(event.type, event.channel, event.data0, event.data1, paramMapping);
	if (paramChange)
	{
		int32 index = 0;
		IParamValueQueue *queue =
			inputParameterChanges.addParameterData((*paramChange).first, index);
		if (queue)
		{
			if (queue->addPoint(event.timestamp, (*paramChange).second, index) != kResultOk)
			{
				assert(false && "Parameter point was not added to ParamValueQueue!");
			}
		}

		return true;
	}

	return false;
}

//------------------------------------------------------------------------
bool Vst3EffectImpl::onEvent(const IMidiClient::Event &event, int32_t port)
{
	// Try to create Event first.
	if (processVstEvent(event, port))
		return true;

	// In case this is no event it must be a parameter.
	if (processParamChange(event, port))
		return true;

	// TODO: Something else???

	return true;
}

//------------------------------------------------------------------------

void Vst3EffectImpl::Activate()
{
	processor->setProcessing(true); // != kResultOk
	this->isProcessing  = true;
}

void Vst3EffectImpl::Deactivate()
{
	if (isProcessing)
	{
		FlushControlChanges();
		Run(0, nullptr); // make sure all pending events have been processed.
		isProcessing = false;
	}
}

void Vst3EffectImpl::Prepare(int32_t sampleRate, size_t maxBufferSize, int inputChannels, int outputChannels)
{
}
void Vst3EffectImpl::Unprepare()
{
	if (isProcessing)
	{
		Deactivate();
	}
}

ssize_t Vst3EffectImpl::ParamIdToLv2Id(ParamID id) const
{
	for (size_t i = 0; i < this->lv2ToVstParam.size(); ++i)
	{
		if (lv2ToVstParam[i] == id)
		{
			return (ssize_t)i;
		}
	}
	return -1;
}

tresult Vst3EffectImpl::beginEdit(ParamID id)
{
	return kResultOk;
}
tresult Vst3EffectImpl::performEdit(ParamID id, ParamValue valueNormalized)
{
	// Called by the plugin, from whatever thread it likes: re-entrantly from
	// inside one of our own controller calls (already holding controllerMutex
	// on this thread; the mutex is recursive, so try_lock succeeds), or from a
	// plugin-owned thread. fireControlChanged() calls back into the
	// controller, so it must be serialized with every other controller call.
	//
	// try_lock, never a blocking lock: a non-conforming plugin may call
	// performEdit() from process(), and the audio thread must never wait
	// behind a long setState()/setComponentState() holding controllerMutex.
	// Not blocking also removes the lock-ordering hazard of a plugin thread
	// that holds a plugin-internal lock while calling performEdit(). If the
	// lock is busy the edit is dropped: performEdit() is advisory (it tells
	// the host about an edit the plugin made itself, normally from its GUI),
	// and PiPedal is headless, so plugin-GUI edits do not occur.
	//
	// From inside process() (the audio thread) the edit is dropped outright:
	// even an uncontended try_lock would then run controller code
	// (normalizedParamToPlain()) and the control-changed handler there.
	if (tlsInVst3Process)
	{
		return kResultFalse;
	}
	std::unique_lock lock{controllerMutex, std::try_to_lock};
	if (!lock.owns_lock())
	{
		return kResultFalse;
	}
	int ix = ParamIdToLv2Id(id);
	if (ix != -1)
	{
		fireControlChanged(ix, (float)valueNormalized);
	}
	return kResultOk;
}

tresult Vst3EffectImpl::endEdit(ParamID id)
{
	return kResultOk;
}

void Vst3EffectImpl::transferControllerStateToComponent()
{
	OPtr<IRtStream> bStream = this->streamPool.AllocateBStream();
	assert(GetRefCount(bStream.get()) == 1);

	// Non-RT. No lock is shared with the audio thread any more; this only
	// serializes against the control worker's IEditController calls.
	std::lock_guard guard{controllerMutex};

	// pendingPlainValues is deliberately not cleared here: anything still
	// queued is at least as new as the values read below, so the control
	// worker must still deliver it to the controller. (SetState() discards
	// values queued *before* the state load itself.)

	// assume that parameters are straightforward and uncomplicated if they
	// didn't provide BStream-based state management
	// Vst2 used to do this. It's not clear that this is still legal in Vst3.

	for (int index = 0; index < this->lv2ToVstParam.size(); ++index)
	{
		ParamID paramId = lv2ToVstParam[index];
		pendingNormalizedValues.Set(
			(size_t)index,
			controller->plainParamToNormalized(paramId, parameterValues[index]));
	}
	if (!this->isProcessing)
	{
		Run(0,nullptr);
	}
}


tresult Vst3EffectImpl::restartComponent(int32 flags)
{
	if (flags & (RestartFlags::kParamValuesChanged))
	{
		// Called by the plugin from whatever thread it likes, possibly the
		// audio thread (a non-conforming plugin calling it from process()),
		// so never block here, and never run the restart on the audio thread:
		// it calls into the edit controller and fires control-changed
		// notifications. Re-entrant calls from inside one of our own
		// controller calls already hold controllerMutex on this thread (it is
		// recursive), so try_lock succeeds and the restart runs inline. If
		// the lock is busy, hand the restart to the control worker: an
		// atomic store and an RT-safe sem_post().
		if (!tlsInVst3Process)
		{
			std::unique_lock lock{controllerMutex, std::try_to_lock};
			if (lock.owns_lock())
			{
				RestartParamValues();
				return kResultOk;
			}
		}
		deferredParamValuesRestart.store(true);
		controlWorkerWake.Post();
	}

	return kResultOk;
}

void Vst3EffectImpl::RestartParamValues()
{
	std::lock_guard lock{controllerMutex};
	// Deliver values queued by SetControl() to the controller first, so
	// they are part of the state we read back, rather than discarding
	// them. If this is a re-entrant call from inside a controller call made
	// by FlushControlChanges() itself, the nested flush scans every slot
	// (the outer pass has already claimed the summary flag), so values the
	// outer pass hasn't reached yet are delivered before the refresh, and
	// the value the outer pass is delivering right now is excluded from the
	// refresh (see refreshControlValues()). Nothing is delivered twice or
	// lost.
	FlushControlChanges();
	refreshControlValues();
	transferControllerStateToComponent();
}

static std::vector<uint8_t> StreamToVec(IBStream *stream)
{
	Steinberg::int64 length;
	stream->seek(0, IBStream::kIBSeekEnd, &length);
	stream->seek(0, IBStream::kIBSeekSet);
	std::vector<uint8_t> result;
	result.resize(length);
	Steinberg::int32 numRead = 0;
	stream->read(&result[0], length, &numRead);
	if (numRead != length)
	{
		throw Vst3Exception("Failed to read.");
	}
	return result;
}

void Vst3EffectImpl::CheckSync()
{
	// Test-only, and never with the audio thread running this effect:
	// deliver anything SetControl() queued to both controller and processor
	// before comparing them.
	FlushControlChanges();
	Run(0, nullptr);

	std::lock_guard lock{controllerMutex};
	OPtr<IBStream> stream{new MemoryStream()};

	if (this->controller->getState(stream) == kResultOk)
	{
		std::vector<uint8_t> initialState = StreamToVec(stream);
		OPtr<IBStream> stream2{new MemoryStream()};
		this->component->getState(stream2);
		stream2->seek(0, IBStream::kIBSeekSet);
		this->controller->setState(stream2);

		OPtr<IBStream> stream3{new MemoryStream()};
		this->controller->getState(stream3);
		std::vector<uint8_t> newState = StreamToVec(stream3);

		if (initialState.size() != newState.size())
		{
			throw Vst3Exception("State changed.");
		}
		for (size_t i = 0; i < newState.size(); ++i)
		{
			if (initialState[i] != newState[i])
			{
				throw Vst3Exception("State changed.");
			}
		}
	}
}

bool Vst3EffectImpl::GetState(std::vector<uint8_t>*state)
{
	OPtr<IBStream> stream{new MemoryStream()};

	if (component->getState(stream) != kResultOk)
	{
		return false;
	}
	Steinberg::int64 length = 0;
	if (stream->seek(0, IBStream::kIBSeekEnd, &length) != kResultOk)
	{
		return false;
	}
	stream->seek(0, IBStream::kIBSeekSet);
	std::vector<uint8_t> result;

	result.resize(length);
	int64 ix = 0;
	while (length != 0)
	{
		int32 thisTime;
		if (length > 0x10000000)
		{
			thisTime = 0x10000000;
		}
		else
		{
			thisTime = (int32)length;
		}
		if (stream->read((void *)(&result[ix]), thisTime, &thisTime) != kResultOk)
		{
			throw Vst3Exception(this->info.pluginInfo_.name() + ": Failed to read state.");
		}
		if (thisTime == 0)
		{
			throw Vst3Exception(this->info.pluginInfo_.name() + ": Unexpected end of file while reading state.");
		}
		ix += thisTime;
		length -= thisTime;
	}
	*state = std::move(result);
	return true;
}
void Vst3EffectImpl::SetState(const std::vector<uint8_t> state)
{
	if (state.empty())
	{
		// &state[0] on an empty vector is undefined behaviour. An empty blob
		// carries nothing to restore, so there is nothing to do.
		return;
	}
	std::lock_guard lock{controllerMutex};

	// Values queued by SetControl() before the state load are superseded by
	// it; don't let them land on top of the restored state afterwards.
	pendingPlainValues.Clear();
	pendingNormalizedValues.Clear();

	OPtr<IBStream> stream{new MemoryStream((void *)(&state[0]), state.size())};

	if (controller->setComponentState(stream) != kResultOk)
	{
		throw Vst3Exception(this->info.pluginInfo_.name() + " Failed to restore state.");
	}
	refreshControlValues();
	stream->seek(0,IBStream::kIBSeekSet);
	if (component->setState(stream) != kResultOk)
	{
		transferControllerStateToComponent();
	}
}

void Vst3EffectImpl::refreshControlValues()
{
	std::lock_guard lock{controllerMutex};
	for (size_t i = 0; i < lv2ToVstParam.size(); ++i)
	{
		// A value FlushControlChanges() is handing to the controller further
		// up this thread's stack: the controller may not have stored it yet,
		// and parameterValues[i] already holds it.
		if (i < controlFlushInFlight.size() && controlFlushInFlight[i])
		{
			continue;
		}
		fireControlChanged(i, controller->getParamNormalized(lv2ToVstParam[i]));
	}
}
std::vector<Vst3ProgramList> Vst3EffectImpl::GetProgramList(int32_t programListId)
{
	std::vector<Vst3ProgramList> result;

	std::lock_guard lock{controllerMutex};
	FUnknownPtr<IUnitInfo> iUnitInfo(controller);
	if (!iUnitInfo)
		throw Vst3Exception("Invalid unit info");

	int count = iUnitInfo->getProgramListCount();
	for (int i = 0; i < count; ++i)
	{
		ProgramListInfo programListInfo;
		if (iUnitInfo->getProgramListInfo(i, programListInfo) != kResultOk)
		{
			break;
		}
		if (programListInfo.id == programListId)
		{
			Vst3ProgramList list;

			list.name = VST3::StringConvert::convert(programListInfo.name);

			for (int prog = 0; prog < programListInfo.programCount; ++prog)
			{
				Vst::String128 progName;
				if (iUnitInfo->getProgramName(programListInfo.id, prog, progName) == kResultOk)
				{
					Vst3ProgramListEntry entry;
					entry.id = prog;
					entry.name = VST3::StringConvert::convert(progName);
					list.programs.push_back(std::move(entry));
				}
			}
			result.push_back(std::move(list));
		}
	}
	return result;
}

void Vst3EffectImpl::fireControlChanged(int control, float normalizedValue)
{
	// Non-RT. Usually already held by the caller (recursive); taken here so
	// that no path reaches normalizedParamToPlain() unserialized.
	std::lock_guard lock{controllerMutex};
	normalizedValue = NaNGuard(normalizedValue);
	float plainValue = NaNGuard(this->controller->normalizedParamToPlain(this->lv2ToVstParam[control], normalizedValue));
	if (parameterValues[control] != plainValue)
	{
		parameterValues[control] = plainValue;
		controlChangedHandler(control,plainValue);
	}
}


const Lv2PluginUiInfo& Vst3EffectImpl::GetCurrentPluginInfo()
{
	std::lock_guard lock{controllerMutex};
	Vst3Host::Private::UpdateControlInfo(this->controller,this->info.pluginInfo_);
	return this->info.pluginInfo_;
}

JSON_MAP_BEGIN(Vst3PluginInfo)
JSON_MAP_REFERENCE(Vst3PluginInfo, filePath)
JSON_MAP_REFERENCE(Vst3PluginInfo, version)
JSON_MAP_REFERENCE(Vst3PluginInfo, uid)
JSON_MAP_REFERENCE(Vst3PluginInfo, pluginInfo)

JSON_MAP_END()
