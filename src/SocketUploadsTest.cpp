#include "pch.h"
#include "catch.hpp"
#include "UploadPolicy.hpp"
#include "MimeTypes.hpp"
#include "TemporaryFile.hpp"
#include <chrono>
#include <set>

using namespace pipedal;
namespace fs = std::filesystem;

TEST_CASE("upload extension allow-list", "[socket_uploads]")
{
    for (const char *ok : {"a.nam", "a.wav", "a.json", "a.flac", "a.mp3", "a.ogg", "a.aiff", "a.zip", "a.NAM", "x/y/README.md"})
        CHECK(IsAllowedUploadExtension(ok));
    for (const char *bad : {"a.sh", "a.so", "a", "a.nam.exe", "a.html", "a.", ".nam/x"})
        CHECK_FALSE(IsAllowedUploadExtension(bad));
}

TEST_CASE("etag generation", "[socket_uploads]")
{
    auto t = fs::file_time_type(std::chrono::seconds(1000));
    auto e1 = MakeETag(10, t);
    CHECK(e1 == MakeETag(10, t));
    CHECK(e1 != MakeETag(11, t));
    CHECK(e1 != MakeETag(10, t + std::chrono::seconds(1)));
    CHECK(e1.front() == '"');
    CHECK(e1.back() == '"');
}

TEST_CASE("if-none-match matching", "[socket_uploads]")
{
    std::string e = "\"abc\"";
    CHECK(IfNoneMatchMatches("\"abc\"", e));
    CHECK(IfNoneMatchMatches("W/\"abc\"", e));
    CHECK(IfNoneMatchMatches("\"x\", \"abc\" ,\"y\"", e));
    CHECK(IfNoneMatchMatches("\"x\",W/\"abc\"", e));
    CHECK(IfNoneMatchMatches("*", e));
    CHECK_FALSE(IfNoneMatchMatches("", e));
    CHECK_FALSE(IfNoneMatchMatches("\"abcd\"", e));
    CHECK_FALSE(IfNoneMatchMatches("\"x\", \"y\"", e));
    CHECK_FALSE(IfNoneMatchMatches("\"abc\"", ""));
}

TEST_CASE("upload slot RAII", "[socket_uploads]")
{
    UploadLimiter lim(2);
    {
        auto a = lim.TryAcquire();
        auto b = lim.TryAcquire();
        CHECK(a);
        CHECK(b);
        CHECK(lim.active() == 2);
        auto c = lim.TryAcquire();
        CHECK_FALSE(c);
        CHECK(lim.active() == 2);
        auto moved = std::move(a);
        CHECK(lim.active() == 2);
    }
    CHECK(lim.active() == 0);
    try
    {
        auto a = lim.TryAcquire();
        throw std::runtime_error("x");
    }
    catch (...)
    {
    }
    CHECK(lim.active() == 0);
    CHECK(lim.TryAcquire());
}

TEST_CASE("upload allow-list includes all known audio extensions", "[socket_uploads]")
{
    for (const char *ok : {"a.m4a", "a.aac", "a.opus", "a.wma", "a.mka", "a.M4A", "a.OPUS"})
        CHECK(IsAllowedUploadExtension(ok));
    for (const std::string &ext : MimeTypes::instance().AudioExtensions())
        CHECK(IsAllowedUploadExtension("a" + ext));
    // Playlists, video and our own temp files are not media uploads.
    for (const char *bad : {"a.m3u", "a.m3u8", "a.pls", "a.mp4", "a.webm", "a.uploading"})
        CHECK_FALSE(IsAllowedUploadExtension(bad));
}

TEST_CASE("upload copy-fallback temp names are unique", "[socket_uploads]")
{
    fs::path dir = fs::temp_directory_path() / "pipedal_upload_temp_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    fs::path target = dir / "model.nam";

    std::set<fs::path> names;
    for (int i = 0; i < 20; ++i)
    {
        fs::path p = MakeUploadTempFile(target);
        CHECK(p.parent_path() == dir);
        CHECK(p.extension() == ".uploading"); // still matched by the stale-temp sweep
        CHECK(p.filename().string().front() == '.');
        CHECK(fs::is_regular_file(p));
        CHECK((fs::status(p).permissions() & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
        names.insert(p);
    }
    CHECK(names.size() == 20);
    CHECK(!fs::exists(target));
    CHECK_THROWS(MakeUploadTempFile(dir / "missing_dir" / "x.nam"));
    fs::remove_all(dir);
}

TEST_CASE("TemporaryFile move clears the source path", "[socket_uploads]")
{
    fs::path dir = fs::temp_directory_path() / "pipedal_tempfile_move_test";
    fs::remove_all(dir);
    {
        TemporaryFile a(dir);
        fs::path kept = a.Path();
        REQUIRE(fs::exists(kept));

        TemporaryFile b(std::move(a));
        CHECK(a.Path().empty());
        CHECK(b.Path() == kept);

        TemporaryFile c;
        c = std::move(b);
        CHECK(b.Path().empty());
        CHECK(c.Path() == kept);

        // Self-move-assignment must not delete the file.
        TemporaryFile &cRef = c;
        c = std::move(cRef);
        CHECK(c.Path() == kept);
        CHECK(fs::exists(kept));

        // Only the final owner deletes the file.
        {
            TemporaryFile sink(std::move(c));
            CHECK(c.Path().empty());
        }
        CHECK(!fs::exists(kept));

        TemporaryFile d(dir);
        fs::path dPath = d.Path();
        fs::path detached = d.Detach();
        CHECK(detached == dPath);
        CHECK(d.Path().empty());
        CHECK(fs::exists(detached));
        fs::remove(detached);
    }
    fs::remove_all(dir);
}

TEST_CASE("upload rate policy: request time cap", "[socket_uploads]")
{
    using namespace std::chrono;
    using P = UploadRatePolicy;
    CHECK(P::RequestTimeCap(0) == seconds(30));                       // clamped up
    CHECK(P::RequestTimeCap(512 * 1024) == seconds(30));              // 16 s + 10 s grace => 30
    CHECK(P::RequestTimeCap(1024 * 1024) == seconds(42));             // 32 s + 10 s
    CHECK(P::RequestTimeCap(10 * 1024 * 1024) == seconds(330));       // 320 s + 10 s
    // 64 MiB: 2048 s + 10 s; not clamped, so the 32 KiB/s floor holds for large uploads.
    CHECK(P::RequestTimeCap(64ull * 1024 * 1024) == seconds(2058));
    CHECK(P::RequestTimeCap(112ull * 1024 * 1024) == seconds(3594));  // just under the cap
    CHECK(P::RequestTimeCap(512ull * 1024 * 1024) == seconds(3600));  // clamped down
}

TEST_CASE("upload rate policy: headers deadline", "[socket_uploads]")
{
    using namespace std::chrono;
    using P = UploadRatePolicy;
    P::time_point t0{seconds(1000)};

    P p;
    // Nothing received and no connection time: never expires (cap is the only bound).
    CHECK_FALSE(p.Expired(t0 + hours(1), 0));
    // Nothing received, watchdog knows when we connected.
    CHECK_FALSE(p.Expired(t0 + seconds(10), 0, t0));
    CHECK(p.Expired(t0 + seconds(11), 0, t0));

    p.OnBytes(t0 + seconds(5));
    p.OnBytes(t0 + seconds(9)); // only the first byte counts
    CHECK_FALSE(p.Expired(t0 + seconds(15), 0, t0)); // first byte wins over connectedAt
    CHECK_FALSE(p.Expired(t0 + seconds(15), 0));
    CHECK(p.Expired(t0 + seconds(15) + milliseconds(1), 0));
}

TEST_CASE("upload rate policy: body grace and minimum rate", "[socket_uploads]")
{
    using namespace std::chrono;
    using P = UploadRatePolicy;
    P::time_point t0{seconds(1000)};

    P p;
    p.OnBytes(t0);
    p.OnBodyStart(t0 + seconds(1));
    CHECK(p.BodyStarted());
    // Grace: nothing received for 10 s is still fine, and the header deadline no longer applies.
    CHECK_FALSE(p.Expired(t0 + seconds(11), 0));
    // After grace, below 32 KiB/s average fails...
    CHECK(p.Expired(t0 + seconds(11) + milliseconds(1), 0));
    CHECK(p.Expired(t0 + seconds(21), 20 * P::MIN_UPLOAD_RATE - 1));
    // ...and at or above it passes.
    CHECK_FALSE(p.Expired(t0 + seconds(21), 20 * P::MIN_UPLOAD_RATE));
    // A burst after a stall is judged on the average, so it recovers.
    CHECK(p.Expired(t0 + seconds(31), 0));
    CHECK_FALSE(p.Expired(t0 + seconds(31), 30 * P::MIN_UPLOAD_RATE));
}

TEST_CASE("upload rate policy: fast uploads are never rejected", "[socket_uploads]")
{
    using namespace std::chrono;
    using P = UploadRatePolicy;
    P::time_point t0{seconds(1000)};

    P p;
    p.OnBytes(t0);
    p.OnBodyStart(t0 + milliseconds(50));
    // 1 MiB/s for 10 minutes, sampled every 100 ms.
    uint64_t bytes = 0;
    bool anyExpired = false;
    for (int i = 1; i <= 6000; ++i)
    {
        bytes += 1024 * 1024 / 10;
        anyExpired |= p.Expired(t0 + milliseconds(50 + 100 * i), bytes);
    }
    CHECK_FALSE(anyExpired);
    // Exactly the minimum rate is accepted throughout.
    P q;
    q.OnBodyStart(t0);
    for (int s = 0; s <= 600; ++s)
    {
        INFO(s);
        CHECK_FALSE(q.Expired(t0 + seconds(s), (uint64_t)s * P::MIN_UPLOAD_RATE));
    }
}
