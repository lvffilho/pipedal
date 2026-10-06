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


#pragma once 
#include <string>
#include <vector>

namespace pipedal {
    bool HasCpuGovernor(); 
    std::string GetCpuGovernor();

    void SetCpuGovernor(const std::string &governor);

    std::vector<std::string> GetAvailableGovernors();
    // As above; *fromSysfs is set true only if the list was read from scaling_available_governors
    // (false when the hard-coded fallback list was returned, or there is no governor).
    std::vector<std::string> GetAvailableGovernors(bool *fromSysfs);

    // Parses the contents of scaling_available_governors (space-separated).
    std::vector<std::string> ParseGovernorList(const std::string &text);

    // True if governor is one of the available governors.
    bool IsValidGovernor(const std::string &governor, const std::vector<std::string> &available);

    // The governor to use at startup: the persisted one if it is available; otherwise the
    // currently running governor (if that is itself available). Returns persisted unchanged
    // when no replacement can be determined (no governor list, list not read from sysfs, or
    // current governor unknown).
    std::string ResolvePersistedGovernor(
        const std::string &persisted,
        const std::vector<std::string> &available,
        const std::string &current,
        bool availableFromSysfs);

    // Validates, then sets. Never throws; returns false (after logging) on failure.
    bool TrySetCpuGovernor(const std::string &governor);
};