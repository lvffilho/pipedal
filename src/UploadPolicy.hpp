// Helpers for upload policy and static-file validators (ETag / If-None-Match).
// Header-only so they can be unit tested without the web server. Not dependency-free:
// IsAllowedUploadExtension uses MimeTypes (MimeTypes.cpp) and the path helpers use
// HtmlHelper (PiPedalCommon), so users must link both (pipedald and pipedaltest do).
#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <stdlib.h>
#include <unistd.h>
#include "MimeTypes.hpp"
#include "HtmlHelper.hpp"

namespace pipedal
{
    // Root of user media uploads. Shared by the upload path validators (IsSafeMediaPath)
    // and IR upload routing so they can't drift apart.
    inline const std::filesystem::path AUDIO_UPLOADS_ROOT{"/var/pipedal/audio_uploads"};
    // Current TONE3000 thumbnail directory, and the pre-audio_uploads legacy location.
    inline const std::filesystem::path TONE3000_THUMBNAILS_ROOT = AUDIO_UPLOADS_ROOT / "tone3000_thumbnails";
    inline const std::filesystem::path LEGACY_TONE3000_THUMBNAILS_ROOT{"/var/pipedal/tone3000_thumbnails"};

    // Single source of truth for "file is inside this upload root": path must be lexically
    // below root (string prefix "root/") AND its parent directory must canonically resolve
    // under root. A final-component FILE symlink is allowed (factory resources are linked into
    // uploads), but symlinked directories and ".." can't escape. Callers add their own
    // file-name / extension checks.
    inline bool IsFileUnderRoot(const std::filesystem::path &root, const std::filesystem::path &path)
    {
        std::string rootString = root.string();
        while (rootString.size() > 1 && rootString.back() == '/')
            rootString.pop_back();
        if (rootString.empty() || !path.string().starts_with(rootString + "/"))
            return false;
        return HtmlHelper::IsParentDirectoryUnderRoot(root, path);
    }

    // True if path is a safe file name strictly inside AUDIO_UPLOADS_ROOT (see IsFileUnderRoot).
    inline bool IsPathInAudioUploads(const std::filesystem::path &path)
    {
        if (!HtmlHelper::IsSafeFileName(path))
            return false;
        return IsFileUnderRoot(AUDIO_UPLOADS_ROOT, path);
    }

    // True if the two directories resolve (weakly_canonical) to the same place. Used at
    // web-server startup to warn if Storage's upload directory and AUDIO_UPLOADS_ROOT diverge
    // (the download intercept uses the former, the upload validators the latter).
    inline bool IsSameDirectory(const std::filesystem::path &a, const std::filesystem::path &b)
    {
        std::error_code ecA, ecB;
        auto ca = std::filesystem::weakly_canonical(a, ecA);
        auto cb = std::filesystem::weakly_canonical(b, ecB);
        if (ecA || ecB)
            return a.lexically_normal() == b.lexically_normal();
        return ca == cb;
    }

    // Slow-loris policy for an HTTP request (headers and body), evaluated with an injected
    // clock so it can be unit tested. The web server checks it each time request bytes arrive
    // (request_with_file_upload::consume) and from a 1 s per-connection watchdog timer, so a
    // client that goes silent is caught too. Requests that send at a normal rate are never
    // rejected: the body rate is the AVERAGE since the body started, and only after a grace period.
    // Residual: consume() alone can't see a connection that sends nothing at all (or stops
    // after the headers); only the watchdog catches those (~10-11 s). If the watchdog can't be
    // started (logged), or once the request has been fully read (handler + response write),
    // the only bound is websocketpp's open-handshake timeout, set to RequestTimeCap().
    class UploadRatePolicy
    {
    public:
        using clock = std::chrono::steady_clock;
        using time_point = clock::time_point;

        static constexpr uint64_t MIN_UPLOAD_RATE = 32 * 1024; // bytes/second, average
        static constexpr std::chrono::seconds BODY_GRACE{10};
        // Headers must be complete within this long of the first byte (or, if nothing has
        // arrived at all, of the connection being accepted).
        static constexpr std::chrono::seconds HEADER_DEADLINE{10};
        static constexpr std::chrono::seconds MIN_REQUEST_CAP{30};
        // Generous on purpose: a lower clamp would undercut the advertised MIN_UPLOAD_RATE
        // floor for large uploads (64 MiB at 32 KiB/s needs ~2058 s). Slow or silent clients
        // are already bounded by the min-rate rule and the watchdog; this is only a backstop.
        static constexpr std::chrono::seconds MAX_REQUEST_CAP{3600};

        // Hard wall-clock cap for reading a whole request (websocketpp's open-handshake
        // timeout): time to receive maxUploadSize at MIN_UPLOAD_RATE plus BODY_GRACE,
        // clamped to [MIN_REQUEST_CAP, MAX_REQUEST_CAP] (uploads up to ~112 MiB are not clamped).
        static std::chrono::milliseconds RequestTimeCap(uint64_t maxUploadSize)
        {
            std::chrono::milliseconds cap =
                std::chrono::seconds(maxUploadSize / MIN_UPLOAD_RATE) + BODY_GRACE;
            return std::clamp<std::chrono::milliseconds>(cap, MIN_REQUEST_CAP, MAX_REQUEST_CAP);
        }

        // Record arrival of request bytes (only the first call matters).
        void OnBytes(time_point now)
        {
            if (!firstByte)
                firstByte = now;
        }
        // Headers complete; body (if any) starts now.
        void OnBodyStart(time_point now)
        {
            OnBytes(now);
            if (!bodyStart)
                bodyStart = now;
        }
        bool BodyStarted() const { return bodyStart.has_value(); }

        // True if the request should be failed. bodyBytes: body bytes received so far.
        // connectedAt: when the connection was accepted (watchdog only; used while no byte
        // has arrived yet). Callers stop asking once the request is complete.
        bool Expired(time_point now, uint64_t bodyBytes,
                     std::optional<time_point> connectedAt = std::nullopt) const
        {
            if (!bodyStart)
            {
                std::optional<time_point> start = firstByte ? firstByte : connectedAt;
                return start && now - *start > HEADER_DEADLINE;
            }
            auto elapsed = now - *bodyStart;
            if (elapsed <= BODY_GRACE)
                return false;
            // Integer math (ms, truncated in the client's favour): bytes/s < MIN_UPLOAD_RATE.
            uint64_t ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
            return bodyBytes * 1000 < MIN_UPLOAD_RATE * ms;
        }

    private:
        std::optional<time_point> firstByte;
        std::optional<time_point> bodyStart;
    };

    // Extensions t3k_uploadAsset will accept for media uploads
    // (TONE3000 .nam models, .wav IRs, proteus .json, archives, readmes, plus every
    // audio extension PiPedal knows about: MimeTypes::AudioExtensions(), e.g. .m4a .aac .opus).
    inline bool IsAllowedUploadExtension(const std::filesystem::path &path)
    {
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c)
                       { return (char)std::tolower(c); });
        static const char *allowed[] = {
            ".wav", ".flac", ".mp3", ".ogg", ".aiff", ".aif", ".nam", ".json", ".zip", ".md"};
        for (const char *a : allowed)
        {
            if (ext == a)
                return true;
        }
        return MimeTypes::instance().AudioExtensions().contains(ext);
    }

    // Creates (atomically, mode 0600, via mkstemps) a uniquely named "*.uploading" temp file
    // in the directory of targetPath, for the copy fallback of an upload. Unique per call, so
    // concurrent uploads to the same target never share a temp file. Hidden (leading '.') so
    // file browsers don't list it; still matched by the stale ".uploading" sweep.
    inline std::filesystem::path MakeUploadTempFile(const std::filesystem::path &targetPath)
    {
        static constexpr std::string_view suffix = ".uploading";
        std::string pathTemplate = (targetPath.parent_path() / ".upload_XXXXXX").string();
        pathTemplate += suffix;
        std::vector<char> buffer(pathTemplate.begin(), pathTemplate.end());
        buffer.push_back('\0');
        int fd = mkstemps(buffer.data(), (int)suffix.size());
        if (fd < 0)
        {
            throw std::runtime_error("Unable to create upload temporary file.");
        }
        close(fd);
        return std::filesystem::path(buffer.data());
    }

    // ETag from size and mtime (strong validator, quoted).
    inline std::string MakeETag(uintmax_t size, std::filesystem::file_time_type mtime)
    {
        auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(mtime.time_since_epoch()).count();
        std::ostringstream s;
        s << "\"" << std::hex << size << "-" << (uint64_t)ticks << "\"";
        return s.str();
    }

    // Does an If-None-Match header value match the given (quoted) etag?
    // Handles "*", comma separated lists, and weak ("W/") prefixes (weak comparison).
    inline bool IfNoneMatchMatches(std::string_view header, std::string_view etag)
    {
        auto strip = [](std::string_view v)
        {
            while (!v.empty() && std::isspace((unsigned char)v.front()))
                v.remove_prefix(1);
            while (!v.empty() && std::isspace((unsigned char)v.back()))
                v.remove_suffix(1);
            if (v.starts_with("W/"))
                v.remove_prefix(2);
            return v;
        };
        std::string_view want = strip(etag);
        if (want.empty())
            return false;
        while (true)
        {
            size_t pos = header.find(',');
            std::string_view item = strip(header.substr(0, pos));
            if (item == "*" || (!item.empty() && item == want))
                return true;
            if (pos == std::string_view::npos)
                return false;
            header.remove_prefix(pos + 1);
        }
    }

    // Counting limiter for concurrent uploads. TryAcquire() returns an RAII slot.
    class UploadLimiter
    {
    public:
        explicit UploadLimiter(int maxSlots) : maxSlots(maxSlots) {}

        class Slot
        {
        public:
            Slot() = default;
            Slot(Slot &&o) noexcept : owner(o.owner) { o.owner = nullptr; }
            Slot &operator=(Slot &&o) noexcept
            {
                if (this != &o)
                {
                    Release();
                    owner = o.owner;
                    o.owner = nullptr;
                }
                return *this;
            }
            Slot(const Slot &) = delete;
            Slot &operator=(const Slot &) = delete;
            ~Slot() { Release(); }
            explicit operator bool() const { return owner != nullptr; }

        private:
            friend class UploadLimiter;
            explicit Slot(UploadLimiter *o) : owner(o) {}
            void Release()
            {
                if (owner)
                {
                    owner->count.fetch_sub(1);
                    owner = nullptr;
                }
            }
            UploadLimiter *owner = nullptr;
        };

        // Returns an empty (false) slot if the limit has been reached.
        Slot TryAcquire()
        {
            int c = count.load();
            while (c < maxSlots)
            {
                if (count.compare_exchange_weak(c, c + 1))
                    return Slot(this);
            }
            return Slot();
        }
        int active() const { return count.load(); }

    private:
        int maxSlots;
        std::atomic<int> count{0};
    };
}
