/*
 *   Copyright (c) Robin E.R. Davies
 *   All rights reserved.

 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#pragma once 

#include <string>
#include <vector>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>

namespace pipedal {

    // Lets another thread abort requests: Cancel() kills the curl processes running for
    // requests made with this object, and later requests made with it throw at once.
    // Thread-safe.
    class CurlCancellation
    {
    public:
        void Cancel();
        bool IsCancelled();

    private:
        friend class CurlProcess;
        std::mutex mutex;
        bool cancelled = false;
        std::vector<int> pids; // curl processes running (pid_t).
    };

    // Where request/response temp files go (default /var/pipedal/web_temp). Tests only.
    void SetCurlTempDirectory(const std::filesystem::path &directory);
    std::filesystem::path GetCurlTempDirectory();

    // inputHeadersOpt are passed to curl via a 0600 file (-H @file), never on the command line.
    // maxTimeSeconds > 0 limits the whole transfer (curl --max-time).
    // A cancelled `cancellation` makes the call throw std::runtime_error.
    extern int CurlGet(
        const std::string &url,
        const std::filesystem::path&outputFile,
        std::vector<std::string> *outputHeadersOpt = nullptr,
        const std::vector<std::string> *inputHeadersOpt = nullptr,
        int maxTimeSeconds = 0,
        CurlCancellation *cancellation = nullptr
    );
    // A request without a body: method is one of GET, PUT, DELETE (anything else throws).
    // Headers, timeout and cancellation as for CurlGet.
    extern int CurlRequest(
        const std::string &method,
        const std::string &url,
        const std::filesystem::path&outputFile,
        std::vector<std::string> *outputHeadersOpt = nullptr,
        const std::vector<std::string> *inputHeadersOpt = nullptr,
        int maxTimeSeconds = 0,
        CurlCancellation *cancellation = nullptr
    );
    extern int CurlGet(
        const std::string &url,
        std::vector<uint8_t> &output,
        std::vector<std::string> *outputHeadersOpt = nullptr,
        const std::vector<std::string> *inputHeadersOpt = nullptr

    );
    extern int CurlGet(
        const std::string&url,
        std::vector<std::string> &output,
        std::vector<std::string> *outputHeadersOpt = nullptr,
        const std::vector<std::string> *inputHeadersOpt = nullptr
    );

    struct CurlDownloadRequest{
        std::string url;
        std::filesystem::path outputFile;
    };
    extern int CurlGet(
        const std::vector<CurlDownloadRequest> &request, 
        const std::function<void(size_t completed, size_t total)> &progressCallback,
        std::vector<std::string>*outputHeadersOpt
    );


    extern int CurlPostFile(
        const std::string &url,
        const std::filesystem::path&inputFile,
        const std::filesystem::path&outputFile,
        std::vector<std::string> *outputHeadersOpt = nullptr,
        std::vector<std::string> *inputHeadersOpt = nullptr,
        int maxTimeSeconds = 0,
        CurlCancellation *cancellation = nullptr
    );
    extern int CurlPostStrings(
        const std::string &url,
        const std::string&body,
        std::string&responseBody,
        std::vector<std::string> *outputHeadersOpt = nullptr,
        std::vector<std::string> *inputHeadersOpt = nullptr,
        int maxTimeSeconds = 0,
        CurlCancellation *cancellation = nullptr
    );


}