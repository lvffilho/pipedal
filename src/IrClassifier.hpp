// Copyright (c) 2026 Robin Davies
// MIT license; see other source files in this project.
#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace pipedal
{
    enum class IrKind
    {
        Cab,    // speaker cabinet IR -> audio_uploads/CabIR
        Reverb, // room/space IR      -> audio_uploads/ReverbImpulseFiles
    };

    // IRs shorter than this (strictly) are treated as cabinet IRs when the catalog gear is unknown.
    constexpr double IR_CAB_MAX_SECONDS = 1.0;

    // Pure classifier. Catalog gear "cab" => Cab, "space" => Reverb. Any other/empty gear
    // falls back to length: < 1.0 s => Cab, otherwise Reverb. Unknown length => Cab.
    // (TONE3000 files impulse responses under cab and space, but also under pedal, outboard
    // and experimental, which say nothing about the kind; checked live, October 2026.)
    IrKind ClassifyIr(const std::string &gear, std::optional<double> lengthSeconds);

    // Minimal RIFF/WAVE header read (frames / sampleRate). nullopt if not a readable WAV.
    std::optional<double> GetWavLengthSeconds(const std::filesystem::path &path);

    // Name of the directory (relative to audio_uploads) for the given kind.
    const char *IrDirectoryName(IrKind kind);

    // Maps a requested upload path of the form <uploadsRoot>/(CabIR|ReverbImpulseFiles)/<rest>
    // onto the directory for `kind`, keeping <rest> (tone subfolder and file name).
    // Returns nullopt if the path is not under one of those two IR roots or contains "..".
    std::optional<std::filesystem::path> MapIrUploadPath(
        const std::filesystem::path &requested,
        const std::filesystem::path &uploadsRoot,
        IrKind kind);

    // Classifies an uploaded IR (gear first; WAV length of `file` only when gear doesn't decide;
    // unreadable => logged, treated as cab) and returns the final upload path, or the requested
    // path unchanged if it is not under an IR root.
    std::filesystem::path ResolveIrUploadPath(
        const std::filesystem::path &requested,
        const std::filesystem::path &uploadsRoot,
        const std::string &gear,
        const std::filesystem::path &file);
}
