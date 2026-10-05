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

#include "PluginHost.hpp"
#include "vst3/Vst3Host.hpp"
#include "StateInterface.hpp"
#include <iostream>
#include <cmath>
#include <vector>

using namespace pipedal;
using namespace std;

void EnumerateVst3Plugins()
{
    cout << "--- Enumeration Test --------" << endl;
    Vst3Host::Ptr vst3Host = Vst3Host::CreateInstance();
    vst3Host->RescanPlugins();


    vst3Host->RefreshPlugins();
}


void CheckSync(IEffect*effect)
{
    Vst3Effect *vst3Effect = (Vst3Effect*)effect;

    vst3Effect->CheckSync();
}

void TestPrograms(const Vst3PluginInfo &info, Vst3Effect *effect)
{
    for (size_t i = 0; i < info.pluginInfo_.port_groups().size(); ++i)
    {
        const auto &port_group = info.pluginInfo_.port_groups()[i];
        if (port_group.program_list_id() != -1)
        {
            auto programList = effect->GetProgramList(port_group.program_list_id());
            for (const auto& programGroup: programList)
            {
                cout <<"        " << port_group.program_list_id() << " " << programGroup.name  << " (" << programGroup.programs.size() <<  " programs)" << std::endl;
                // for (const auto&program: programGroup.programs)
                // {
                //     cout << "             " << program.id << " " << program.name << endl;
                // }
            }
        }
    }    
}


// Verifies that Vst3EffectImpl::GetLv2State()/SetLv2State() actually round-trip.
// These used to be no-op stubs, so VST3 presets silently reloaded with default
// values. Saves state, drives every input control away from its saved value,
// restores, and checks that the controls came back.
void TestStateRoundTrip(const Vst3PluginInfo &info, Vst3Effect *plugin)
{
    if (!plugin->SupportsState())
    {
        cout << "        state: plugin does not support state" << endl;
        return;
    }
    const auto &controls = info.pluginInfo_.controls();
    if (controls.size() == 0)
    {
        return;
    }

    Lv2PluginState saved;
    if (!plugin->GetLv2State(&saved) || !saved.isValid_)
    {
        cout << "        state: FAILED - GetLv2State() produced nothing" << endl;
        return;
    }

    std::vector<float> before;
    before.reserve(controls.size());
    for (size_t i = 0; i < controls.size(); ++i)
    {
        before.push_back(plugin->GetControlValue((int)i));
    }

    size_t perturbed = 0;
    for (size_t i = 0; i < controls.size(); ++i)
    {
        const auto &c = controls[i];
        if (!c.is_input())
            continue;
        float mid = (c.min_value() + c.max_value()) / 2;
        float target = (before[i] <= mid) ? c.max_value() : c.min_value();
        if (target != before[i])
        {
            plugin->SetControl((int)i, target);
            ++perturbed;
        }
    }
    if (perturbed == 0)
    {
        cout << "        state: skipped - no control could be perturbed" << endl;
        return;
    }

    plugin->SetLv2State(saved);

    // Compare input controls only. Output controls are meters that reflect the
    // live signal (mda SpecMeter is 35 of them), so they are not expected to
    // come back to a saved value.
    size_t mismatches = 0, compared = 0;
    for (size_t i = 0; i < controls.size(); ++i)
    {
        if (!controls[i].is_input())
            continue;
        ++compared;
        float now = plugin->GetControlValue((int)i);
        float tolerance = 1e-4f * (1.0f + std::fabs(before[i]));
        if (std::fabs(now - before[i]) > tolerance)
        {
            ++mismatches;
            cout << "            did not restore: [" << controls[i].symbol() << "] \""
                 << controls[i].name() << "\" saved=" << before[i] << " now=" << now
                 << (controls[i].is_bypass() ? " (bypass)" : "") << endl;
        }
    }
    if (mismatches == 0)
    {
        cout << "        state round-trip OK (" << perturbed << " perturbed, " << compared << " input controls restored)" << endl;
    }
    else
    {
        cout << "        state round-trip FAILED: " << mismatches << "/" << compared
             << " input controls did not restore" << endl;
    }
}

void RunVsts()
{
    cout << "--- RunVsts ----------------" << endl;
    Vst3Host::Ptr vst3Host = Vst3Host::CreateInstance();

    cout << "Scanning" << endl;
    const auto & plugins = vst3Host->RescanPlugins();

    PluginHost host;

    IHost *pHost = host.asIHost();
    host.setSampleRate(44100);

    for (const auto & info : plugins)
    {

        cout << "   Running " << info->pluginInfo_.name() << endl;
        auto  plugin = vst3Host->CreatePlugin(0,info->pluginInfo_.uri(),pHost);

        int nInputs = info->pluginInfo_.audio_inputs();
        int nOutputs = info->pluginInfo_.audio_outputs();

        plugin->Prepare(host.GetSampleRate(),pHost->GetMaxAudioBufferSize(),info->pluginInfo_.audio_inputs(), info->pluginInfo_.audio_outputs());

        plugin->Activate();

        std::vector<float*> inputs;
        std::vector<float*> outputs;
        inputs.resize(nInputs);
        for (int i = 0; i < nInputs; ++i)
        {
             inputs[i] = new float[512];
             plugin->SetAudioInputBuffer(i,inputs[i]);

             for (int j = 0; j < 512; ++j)
             {
                inputs[i][j] = j / 512.0;
             }
        }
        outputs.resize(nOutputs);
        for (int i = 0; i < nOutputs; ++i) 
        {
            outputs[i] = new float[512];
            plugin->SetAudioOutputBuffer(i,outputs[i]);
        }

        plugin->Run(512,nullptr);

        for (int i = 0; i < info->pluginInfo_.controls().size(); ++i)
        {
            float v = plugin->GetControlValue(i);
            plugin->SetControl(i,v);

            plugin->Run(0,nullptr);

            plugin->CheckSync();
        }

        TestPrograms(*info,plugin.get());

        TestStateRoundTrip(*info, plugin.get());

        plugin->Deactivate();
    }

    

}

void BugReportTest();

int main(int, char**)
{
    BugReportTest();
    
    RunVsts();

    EnumerateVst3Plugins();


    return 0;   
}