// Copyright (c) 2024 Robin Davies
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

#include "TemporaryFile.hpp"
#include <fstream>
#include <stdexcept>
#include <vector>
#include <stdlib.h>
#include <unistd.h>

using namespace pipedal;


TemporaryFile::TemporaryFile(const std::filesystem::path&directory)
{
    namespace fs = std::filesystem;
    fs::create_directories(directory);

    // mkstemps: unique name, created atomically with mode 0600 (temp files may hold
    // credentials, e.g. TONE3000 tokens in curl request/response bodies).
    std::string pathTemplate = (directory / "temp_XXXXXX.tmp").string();
    std::vector<char> buffer(pathTemplate.begin(), pathTemplate.end());
    buffer.push_back('\0');
    int fd = mkstemps(buffer.data(), 4);
    if (fd < 0) {
        throw std::runtime_error("Failed to create temporary file");
    }
    close(fd);

    this->path = std::filesystem::path(buffer.data());

}
std::filesystem::path TemporaryFile::Detach() {
    std::filesystem::path result = std::move(this->path);
    this->path.clear(); // moved-from state is unspecified; make "detached" explicit.
    return result;
}
TemporaryFile::~TemporaryFile()
{
    if (!path.empty() && deleteFile)
    {
        std::error_code ec;
        std::filesystem::remove(path, ec); // never throw from a destructor.
    }
}

void TemporaryFile::SetNonDeletedPath(const std::filesystem::path&path)
{
    this->path = path;
    this->deleteFile = false; // Do not delete the file on destruction
}   



// Moves explicitly clear the source's path: a moved-from std::filesystem::path is only
// "valid but unspecified", and a non-empty one would make the source delete our file.
TemporaryFile::TemporaryFile(TemporaryFile&&other) {
    this->path = std::move(other.path);
    this->deleteFile = other.deleteFile;
    other.path.clear();
}
TemporaryFile&TemporaryFile::operator=(TemporaryFile&&other) {
    if (this == &other) {
        return *this;
    }
    if (!this->path.empty() && this->deleteFile) {
        std::error_code ec;
        std::filesystem::remove(this->path, ec);
        this->path.clear();
    }
    this->path = std::move(other.path);
    this->deleteFile = other.deleteFile;
    other.path.clear();
    return *this;
}
