#include "pch.h"
#include "catch.hpp"
#include "ofstream_synced.hpp"
#include "Storage.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

using namespace pipedal;
namespace fs = std::filesystem;

namespace
{
    struct TempDir
    {
        fs::path path;
        TempDir()
        {
            path = fs::temp_directory_path() / ("pipedal_persist_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
            fs::create_directories(path);
        }
        ~TempDir()
        {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
        static inline int counter = 0;
    };

    std::string readAll(const fs::path &p)
    {
        std::ifstream f(p);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }
    void writeRaw(const fs::path &p, const std::string &s)
    {
        std::ofstream f(p);
        f << s;
    }
    ino_t inodeOf(const fs::path &p)
    {
        struct stat st;
        REQUIRE(::stat(p.c_str(), &st) == 0);
        return st.st_ino;
    }
    bool parsesAsOk(const fs::path &p)
    {
        return readAll(p).rfind("OK", 0) == 0;
    }
    // RecoverFileFromBackup's mkstemp temp files ("<file>.recover.XXXXXX") left in dir.
    int countRecoveryTemps(const fs::path &dir)
    {
        int count = 0;
        for (const auto &entry : fs::directory_iterator(dir))
        {
            if (entry.path().filename().string().find(".recover.") != std::string::npos)
                ++count;
        }
        return count;
    }
}

TEST_CASE("ofstream_synced writes via temp file and rename", "[persistence]")
{
    TempDir dir;
    auto target = dir.path / "data.json";
    writeRaw(target, "OLD");
    auto oldInode = inodeOf(target);
    {
        ofstream_synced f(target);
        f << "NEW-CONTENT";
        f.flush();
        // while open, the target is untouched and a temp file exists.
        CHECK(readAll(target) == "OLD");
        CHECK(fs::exists(fs::path(target.string() + ".tmp")));
    }
    CHECK(readAll(target) == "NEW-CONTENT");
    CHECK(inodeOf(target) != oldInode); // replaced, not truncated in place.
    CHECK(!fs::exists(fs::path(target.string() + ".tmp")));
}

TEST_CASE("ofstream_synced leaves old content when writer throws", "[persistence]")
{
    TempDir dir;
    auto target = dir.path / "data.json";
    writeRaw(target, "OLD");
    try
    {
        ofstream_synced f(target);
        f << "PARTIAL";
        throw std::runtime_error("boom");
    }
    catch (const std::runtime_error &)
    {
    }
    CHECK(readAll(target) == "OLD");
    CHECK(!fs::exists(fs::path(target.string() + ".tmp")));
}

TEST_CASE("ofstream_synced creates new files and honours append", "[persistence]")
{
    TempDir dir;
    auto target = dir.path / "new.txt";
    {
        ofstream_synced f(target.string());
        f << "A";
    }
    CHECK(readAll(target) == "A");
    {
        ofstream_synced f(target, std::ios_base::app);
        f << "B";
    }
    CHECK(readAll(target) == "AB");
}

TEST_CASE("RecoverFileFromBackup restores from backup or tmp", "[persistence]")
{
    TempDir dir;
    auto main = dir.path / "x.bank";
    auto backup = fs::path(main.string() + ".$$$");
    auto tmp = fs::path(main.string() + ".tmp");

    SECTION("valid main is untouched")
    {
        writeRaw(main, "OK-main");
        writeRaw(backup, "OK-backup");
        CHECK(Storage::RecoverFileFromBackup(main, parsesAsOk));
        CHECK(readAll(main) == "OK-main");
    }
    SECTION("corrupted main restored from backup")
    {
        writeRaw(main, "garbage");
        writeRaw(backup, "OK-backup");
        CHECK(Storage::RecoverFileFromBackup(main, parsesAsOk));
        CHECK(readAll(main) == "OK-backup");
    }
    SECTION("empty main restored; missing main restored")
    {
        writeRaw(main, "");
        writeRaw(backup, "OK-backup");
        CHECK(Storage::RecoverFileFromBackup(main, parsesAsOk));
        CHECK(readAll(main) == "OK-backup");
        fs::remove(main);
        writeRaw(backup, "OK-backup2");
        CHECK(Storage::RecoverFileFromBackup(main, parsesAsOk));
        CHECK(readAll(main) == "OK-backup2");
    }
    SECTION("newer complete tmp preferred over backup; invalid tmp skipped")
    {
        writeRaw(backup, "OK-backup");
        writeRaw(tmp, "OK-tmp");
        CHECK(Storage::RecoverFileFromBackup(main, parsesAsOk));
        CHECK(readAll(main) == "OK-tmp");
        fs::remove(main);
        writeRaw(tmp, "trunc");
        writeRaw(backup, "OK-backup");
        CHECK(Storage::RecoverFileFromBackup(main, parsesAsOk));
        CHECK(readAll(main) == "OK-backup");
    }
    SECTION("nothing valid")
    {
        writeRaw(main, "garbage");
        writeRaw(backup, "garbage");
        CHECK(!Storage::RecoverFileFromBackup(main, parsesAsOk));
        CHECK(readAll(main) == "garbage");
    }
    SECTION("successful restore from tmp removes the candidate and leaves no recovery temp")
    {
        writeRaw(main, "garbage");
        writeRaw(tmp, "OK-tmp");
        CHECK(Storage::RecoverFileFromBackup(main, parsesAsOk));
        CHECK(readAll(main) == "OK-tmp");
        CHECK(!fs::exists(tmp));
        CHECK(countRecoveryTemps(dir.path) == 0);
    }
    SECTION("an existing <file>.recover is neither used nor clobbered")
    {
        writeRaw(main, "garbage");
        writeRaw(tmp, "OK-tmp");
        fs::create_directory(fs::path(main.string() + ".recover"));
        CHECK(Storage::RecoverFileFromBackup(main, parsesAsOk));
        CHECK(readAll(main) == "OK-tmp");
        CHECK(fs::is_directory(fs::path(main.string() + ".recover")));
    }
    SECTION("restore keeps the candidate's permissions")
    {
        writeRaw(tmp, "OK-tmp");
        REQUIRE(::chmod(tmp.c_str(), 0640) == 0);
        CHECK(Storage::RecoverFileFromBackup(main, parsesAsOk));
        struct stat st;
        REQUIRE(::stat(main.c_str(), &st) == 0);
        CHECK((st.st_mode & 0777) == 0640);
    }
    SECTION("failed restore keeps the tmp candidate and cleans up the recovery temp")
    {
        // a non-empty directory squatting on the target makes the final rename fail, regardless of uid.
        fs::create_directory(main);
        writeRaw(main / "occupant", "x");
        writeRaw(tmp, "OK-tmp");
        CHECK(!Storage::RecoverFileFromBackup(main, parsesAsOk));
        CHECK(readAll(tmp) == "OK-tmp");
        CHECK(fs::is_directory(main));
        CHECK(countRecoveryTemps(dir.path) == 0);
    }
}

TEST_CASE("ofstream_synced open_in_place streams visibly", "[persistence]")
{
    TempDir dir;
    auto target = dir.path / "log.txt";
    ofstream_synced f;
    f.open_in_place(target);
    f << "line1" << std::endl;
    CHECK(readAll(target) == "line1\n");
    CHECK(!fs::exists(fs::path(target.string() + ".tmp")));
}

TEST_CASE("ofstream_synced falls back to in-place write when temp can't be created", "[persistence]")
{
    TempDir dir;
    auto target = dir.path / "data.txt";
    writeRaw(target, "OLD");
    // a directory squatting on the temp name makes the temp open fail, regardless of uid.
    fs::create_directory(fs::path(target.string() + ".tmp"));
    {
        ofstream_synced f(target);
        REQUIRE(f.is_open());
        f << "NEW";
    }
    CHECK(readAll(target) == "NEW");
}

TEST_CASE("Storage::DiscardCurrentPreset removes stale autosave", "[persistence]")
{
    TempDir dir;
    Storage storage;
    storage.SetDataRoot(dir.path);
    CurrentPreset cp;
    cp.modified_ = true;
    storage.SaveCurrentPreset(cp);
    CHECK(fs::exists(dir.path / "currentPreset.json"));
    storage.DiscardCurrentPreset();
    CHECK(!fs::exists(dir.path / "currentPreset.json"));
    CurrentPreset out;
    CHECK(!storage.RestoreCurrentPreset(&out));
}
