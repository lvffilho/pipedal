// Copyright (c) 2026 Robin Davies
// MIT license; see other source files in this project.
#include "IrClassifier.hpp"
#include "Lv2Log.hpp"
#include "ss.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

using namespace pipedal;
namespace fs = std::filesystem;

static std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c)
                   { return std::tolower(c); });
    return s;
}

IrKind pipedal::ClassifyIr(const std::string &gear, std::optional<double> lengthSeconds)
{
    std::string g = toLower(gear);
    if (g == "cab")
        return IrKind::Cab;
    if (g == "space")
        return IrKind::Reverb;
    if (!lengthSeconds.has_value())
        return IrKind::Cab;
    return *lengthSeconds < IR_CAB_MAX_SECONDS ? IrKind::Cab : IrKind::Reverb;
}

static uint32_t rd32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

std::optional<double> pipedal::GetWavLengthSeconds(const fs::path &path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return std::nullopt;
    unsigned char hdr[12];
    if (!f.read((char *)hdr, 12))
        return std::nullopt;
    if (memcmp(hdr, "RIFF", 4) != 0 && memcmp(hdr, "RF64", 4) != 0)
        return std::nullopt;
    if (memcmp(hdr + 8, "WAVE", 4) != 0)
        return std::nullopt;
    uint32_t sampleRate = 0, blockAlign = 0;
    while (true)
    {
        unsigned char ch[8];
        if (!f.read((char *)ch, 8))
            return std::nullopt;
        uint32_t size = rd32(ch + 4);
        if (memcmp(ch, "fmt ", 4) == 0)
        {
            if (size < 16 || size > 4096)
                return std::nullopt;
            std::vector<unsigned char> fmt(size);
            if (!f.read((char *)fmt.data(), size))
                return std::nullopt;
            sampleRate = rd32(fmt.data() + 4);
            blockAlign = rd16(fmt.data() + 12);
        }
        else if (memcmp(ch, "data", 4) == 0)
        {
            if (sampleRate == 0 || blockAlign == 0)
                return std::nullopt;
            // Never trust the header's data size beyond what is actually in the file
            // (truncated downloads, or 0xFFFFFFFF for streamed/RF64 files).
            std::error_code ec;
            auto total = fs::file_size(path, ec);
            auto pos = f.tellg();
            if (ec || pos < 0)
                return std::nullopt;
            uint64_t remaining = total > (uint64_t)pos ? total - (uint64_t)pos : 0;
            uint64_t dataSize = std::min<uint64_t>(size, remaining);
            return (double)(dataSize / blockAlign) / (double)sampleRate;
        }
        else
        {
            f.seekg(size, std::ios::cur);
        }
        if (size & 1)
            f.seekg(1, std::ios::cur);
    }
}

const char *pipedal::IrDirectoryName(IrKind kind)
{
    return kind == IrKind::Cab ? "CabIR" : "ReverbImpulseFiles";
}

std::optional<fs::path> pipedal::MapIrUploadPath(
    const fs::path &requested, const fs::path &uploadsRoot, IrKind kind)
{
    for (const auto &c : requested)
        if (c == "..")
            return std::nullopt;
    fs::path rel = requested.lexically_normal().lexically_relative(uploadsRoot.lexically_normal());
    if (rel.empty())
        return std::nullopt;
    fs::path rest;
    bool first = true;
    for (const auto &c : rel)
    {
        if (c == ".." || c == ".")
            return std::nullopt;
        if (first)
        {
            if (c != "CabIR" && c != "ReverbImpulseFiles")
                return std::nullopt;
            first = false;
        }
        else
        {
            rest /= c;
        }
    }
    if (rest.empty())
        return std::nullopt;
    return uploadsRoot / IrDirectoryName(kind) / rest;
}

fs::path pipedal::ResolveIrUploadPath(
    const fs::path &requested, const fs::path &uploadsRoot, const std::string &gear, const fs::path &file)
{
    std::optional<double> length;
    std::string g = toLower(gear);
    if (g != "cab" && g != "space")
    {
        length = GetWavLengthSeconds(file);
        if (!length)
            Lv2Log::warning(SS("Tone3000: unable to read IR length of " << requested << "; treating as cab IR."));
    }
    auto mapped = MapIrUploadPath(requested, uploadsRoot, ClassifyIr(gear, length));
    return mapped ? *mapped : requested;
}
