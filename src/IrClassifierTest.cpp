// Copyright (c) 2026 Robin Davies
// MIT license; see other source files in this project.
#include "pch.h"
#include "catch.hpp"
#include "IrClassifier.hpp"
#include <fstream>
#include <vector>
#include <cstdint>

using namespace pipedal;
namespace fs = std::filesystem;

static void put32(std::vector<char> &v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back((char)(x >> (8 * i))); }
static void put16(std::vector<char> &v, uint16_t x) { for (int i = 0; i < 2; ++i) v.push_back((char)(x >> (8 * i))); }

static void WriteWav(const fs::path &p, uint32_t rate, uint32_t frames)
{
    std::vector<char> v;
    uint32_t dataBytes = frames * 2;
    v.insert(v.end(), {'R', 'I', 'F', 'F'});
    put32(v, 36 + dataBytes);
    v.insert(v.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put32(v, 16); put16(v, 1); put16(v, 1); put32(v, rate); put32(v, rate * 2); put16(v, 2); put16(v, 16);
    v.insert(v.end(), {'d', 'a', 't', 'a'});
    put32(v, dataBytes);
    v.resize(v.size() + dataBytes);
    std::ofstream(p, std::ios::binary).write(v.data(), v.size());
}

TEST_CASE("IR classification by gear and length", "[ir_classify]")
{
    REQUIRE(ClassifyIr("cab", 5.0) == IrKind::Cab);
    REQUIRE(ClassifyIr("space", 0.1) == IrKind::Reverb);
    REQUIRE(ClassifyIr("CAB", std::nullopt) == IrKind::Cab);
    REQUIRE(ClassifyIr("other", 0.99) == IrKind::Cab);
    REQUIRE(ClassifyIr("other", 1.0) == IrKind::Reverb);
    REQUIRE(ClassifyIr("", 1.5) == IrKind::Reverb);
    REQUIRE(ClassifyIr("", 0.2) == IrKind::Cab);
    REQUIRE(ClassifyIr("", std::nullopt) == IrKind::Cab);
}

TEST_CASE("IR WAV length and upload path mapping", "[ir_classify]")
{
    fs::path root = fs::temp_directory_path() / "pipedal_ir_classify_test";
    fs::remove_all(root);
    fs::create_directories(root);
    WriteWav(root / "short.wav", 48000, 24000);
    WriteWav(root / "long.wav", 48000, 48000);
    { std::ofstream(root / "bad.wav") << "not a wav"; }
    REQUIRE(*GetWavLengthSeconds(root / "short.wav") == Catch::Approx(0.5));
    REQUIRE(*GetWavLengthSeconds(root / "long.wav") == Catch::Approx(1.0));
    REQUIRE(!GetWavLengthSeconds(root / "bad.wav").has_value());
    REQUIRE(!GetWavLengthSeconds(root / "missing.wav").has_value());

    // Data chunk size larger than the file (truncated download / lying header): clamp to file.
    {
        WriteWav(root / "truncated.wav", 48000, 24000); // 0.5 s actually present
        std::fstream f(root / "truncated.wav", std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(40); // data chunk size field
        std::vector<char> big;
        put32(big, 48000 * 2 * 3600); // claims an hour
        f.write(big.data(), big.size());
    }
    REQUIRE(*GetWavLengthSeconds(root / "truncated.wav") == Catch::Approx(0.5));
    {
        WriteWav(root / "streamed.wav", 48000, 12000); // 0.25 s
        std::fstream f(root / "streamed.wav", std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(40);
        std::vector<char> ff;
        put32(ff, 0xFFFFFFFFu);
        f.write(ff.data(), ff.size());
    }
    REQUIRE(*GetWavLengthSeconds(root / "streamed.wav") == Catch::Approx(0.25));
    // Truncated file: still a valid classification (cab), not an hour-long reverb.
    REQUIRE(ClassifyIr("", GetWavLengthSeconds(root / "truncated.wav")) == IrKind::Cab);

    fs::path up = "/var/pipedal/audio_uploads";
    fs::path req = up / "CabIR" / "My Tone" / "a.wav";
    REQUIRE(ResolveIrUploadPath(req, up, "space", root / "short.wav") == up / "ReverbImpulseFiles" / "My Tone" / "a.wav");
    REQUIRE(ResolveIrUploadPath(req, up, "cab", root / "long.wav") == req);
    REQUIRE(ResolveIrUploadPath(req, up, "", root / "long.wav") == up / "ReverbImpulseFiles" / "My Tone" / "a.wav");
    REQUIRE(ResolveIrUploadPath(req, up, "", root / "short.wav") == req);
    REQUIRE(ResolveIrUploadPath(req, up, "", root / "bad.wav") == req); // unreadable => cab
    fs::path rev = up / "ReverbImpulseFiles" / "T" / "a.wav";
    REQUIRE(ResolveIrUploadPath(rev, up, "cab", root / "long.wav") == up / "CabIR" / "T" / "a.wav");

    // Not under an IR root: unchanged.
    fs::path nam = up / "NeuralAmpModels" / "T" / "a.wav";
    REQUIRE(ResolveIrUploadPath(nam, up, "space", root / "long.wav") == nam);
    // Traversal never maps.
    REQUIRE(!MapIrUploadPath(up / "CabIR" / ".." / ".." / "etc" / "a.wav", up, IrKind::Reverb).has_value());
    REQUIRE(!MapIrUploadPath(up / "CabIR" / "x" / ".." / "a.wav", up, IrKind::Reverb).has_value());
    REQUIRE(!MapIrUploadPath(up / "CabIR", up, IrKind::Reverb).has_value());
    REQUIRE(!MapIrUploadPath("/etc/CabIR/a.wav", up, IrKind::Reverb).has_value());
    fs::remove_all(root);
}
