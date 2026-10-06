// Copyright (c) Robin E.R. Davies
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

#include <mutex>
#include "CpuGovernor.hpp"
#include "ss.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include "PiPedalException.hpp"
#include "Lv2Log.hpp"
#include <unistd.h>

using namespace pipedal;


static const int SYSFS_RETRIES = 3;

bool pipedal::HasCpuGovernor()
{
#ifdef __WIN32__
    return false;
#else 
    std::filesystem::path sysFsPath = SS("/sys/devices/system/cpu/cpu" << 0 << "/cpufreq/scaling_governor");
    return std::filesystem::exists(sysFsPath);

#endif
}


std::string pipedal::GetCpuGovernor()
{
    std::string result;
    try {
        std::ifstream f("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
        if (!f.is_open())
        {
            return "";
        }
        f >> result;
    } catch (const std::exception &)
    {
        return result;
    }
    return result;

}

static bool writeAndVerify(std::filesystem::path&sysfsPath, const std::string &value)
{
    // return if the value has already been set.
    {
        std::ifstream f;
        f.open(sysfsPath);
        if (f.is_open())
        {
            std::string line;
            std::getline(f,line);
            if (!f.fail() && value == line)
            {
                return true;
            }
        }
    }
    bool warned = false; // warn once per retry loop, not on every iteration.
    for (int i = 0; i < SYSFS_RETRIES; ++i)
    {
        {
            // write the value.
            std::ofstream f;
            f.open(sysfsPath);
            if (f.is_open())
            {
                f << value;
            } else {
                if (!warned)
                {
                    warned = true;
                    Lv2Log::warning(SS("Can't open " << sysfsPath << " for writing."));
                }
                sleep(1);
                continue;
            }
        }
        // verify that we wrote it
        {
            std::ifstream f;
            f.open(sysfsPath);
            if (f.is_open())
            {
                std::string line;
                std::getline(f,line);
                if (value == line)
                {
                    return true;
                }
                if (!warned)
                {
                    warned = true;
                    Lv2Log::warning(SS("Failed to update " << sysfsPath << ". Read value: " << line << " Expected value: " << value));
                }
            }
            else if (!warned)
            {
                warned = true;
                Lv2Log::warning(SS("Failed to update " << sysfsPath));
            }
        }
        sleep(1);
    }    
    return false;
} 

void pipedal::SetCpuGovernor(const std::string &governor) { 
    // using sysfs 
    int nCpu = 0;

    while (true)
    {
        std::filesystem::path base = SS("/sys/devices/system/cpu/cpu" << nCpu);
        if (!std::filesystem::is_directory(base)) 
            break;

        std::filesystem::path sysFsPath = SS("/sys/devices/system/cpu/cpu" << nCpu << "/cpufreq/scaling_governor");
        if (!std::filesystem::exists(sysFsPath))
        {
            return;
        }

        if (!writeAndVerify(sysFsPath,governor))
        {
            throw PiPedalException(SS("Write to " << sysFsPath << " failed."));
            break;
        }            
        ++nCpu;
    }
}
static const size_t MAX_GOVERNOR_NAME_LENGTH = 32;

std::vector<std::string> pipedal::ParseGovernorList(const std::string &text)
{
    std::vector<std::string> result;
    std::istringstream ss(text);
    std::string token;
    while (ss >> token)
    {
        if (token.length() > MAX_GOVERNOR_NAME_LENGTH)
            continue;
        bool valid = true;
        for (char c : token)
        {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            {
                valid = false;
                break;
            }
        }
        if (valid)
            result.push_back(token);
    }
    return result;
}

std::vector<std::string> pipedal::GetAvailableGovernors() {
    return GetAvailableGovernors(nullptr);
}

std::vector<std::string> pipedal::GetAvailableGovernors(bool *fromSysfs) {
    if (fromSysfs)
    {
        *fromSysfs = false;
    }
    if (!HasCpuGovernor())
    {
        return std::vector<std::string>();
    }
    try {
        std::ifstream f("/sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors");
        if (f.is_open())
        {
            std::string line;
            std::getline(f, line);
            auto result = ParseGovernorList(line);
            if (!result.empty())
            {
                if (fromSysfs)
                {
                    *fromSysfs = true;
                }
                return result;
            }
        }
    } catch (const std::exception &)
    {
    }
    // fall back to the traditional list.
    return std::vector<std::string> {
        "performance",
        "ondemand",
        "powersave"
    };
}

std::string pipedal::ResolvePersistedGovernor(
    const std::string &persisted,
    const std::vector<std::string> &available,
    const std::string &current,
    bool availableFromSysfs)
{
    // The hard-coded fallback list is a guess; don't discard a user's setting on its say-so.
    if (!availableFromSysfs || available.empty() || IsValidGovernor(persisted, available))
    {
        return persisted;
    }
    if (!current.empty() && IsValidGovernor(current, available))
    {
        return current;
    }
    return persisted;
}

bool pipedal::IsValidGovernor(const std::string &governor, const std::vector<std::string> &available)
{
    return std::find(available.begin(), available.end(), governor) != available.end();
}

// The governor is re-applied periodically (AdminMain), so a persisted invalid governor would otherwise
// log the same warning every few seconds. Warn once per distinct governor value.
static bool ShouldWarnAboutGovernor(const std::string &governor, bool failed)
{
    static std::mutex warnMutex;
    static std::string lastWarnedGovernor;
    static bool hasWarned = false;

    std::lock_guard<std::mutex> lock(warnMutex);
    if (!failed)
    {
        hasWarned = false; // a later failure of this governor is news again.
        return false;
    }
    if (hasWarned && lastWarnedGovernor == governor)
    {
        return false;
    }
    hasWarned = true;
    lastWarnedGovernor = governor;
    return true;
}

bool pipedal::TrySetCpuGovernor(const std::string &governor)
{
    try
    {
        if (!IsValidGovernor(governor, GetAvailableGovernors()))
        {
            if (ShouldWarnAboutGovernor(governor, true))
            {
                Lv2Log::warning(SS("CPU governor '" << governor << "' is not available. Ignored."));
            }
            return false;
        }
        SetCpuGovernor(governor);
        ShouldWarnAboutGovernor(governor, false);
        return true;
    }
    catch (const std::exception &e)
    {
        if (ShouldWarnAboutGovernor(governor, true))
        {
            Lv2Log::warning(SS("Failed to set CPU governor '" << governor << "': " << e.what()));
        }
        return false;
    }
}
