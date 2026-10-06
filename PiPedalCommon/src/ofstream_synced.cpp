#include "ofstream_synced.hpp"
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <exception>
#include <atomic>
#include <iostream>
#include <system_error>

using namespace pipedal;
namespace fs = std::filesystem;

void pipedal::FileSystemSync()
{
    ::sync();
}

static void fsyncPath(const fs::path &path, int flags)
{
    int fd = ::open(path.c_str(), flags);
    if (fd >= 0)
    {
        ::fsync(fd);
        ::close(fd);
    }
}

void ofstream_synced::open(const fs::path &filename, ios_base::openmode mode)
{
    if (is_open())
    {
        close();
    }
    atomic = false;
    mode |= ios_base::out;

    bool wantAtomic = (mode & (ios_base::app | ios_base::ate | ios_base::in)) == 0;
    if (wantAtomic)
    {
        std::error_code ec;
        fs::path target = fs::weakly_canonical(filename, ec); // follow symlinks.
        if (ec)
        {
            target = filename;
        }
        auto status = fs::status(target, ec);
        if (!ec && fs::exists(status) && !fs::is_regular_file(status))
        {
            wantAtomic = false; // device, fifo, etc.
        }
        if (wantAtomic)
        {
            targetPath = target;
            tempPath = target;
            tempPath += ".tmp";
            uncaughtAtOpen = std::uncaught_exceptions();
            super::open(tempPath, mode);
            if (!is_open())
            {
                // can't create the temp file (read-only directory, sysfs/procfs attribute...).
                // fall back to an in-place write.
                super::clear();
                static std::atomic<bool> warned{false};
                if (!warned.exchange(true))
                {
                    std::cerr << "ofstream_synced: can't create '" << tempPath.string()
                              << "'; writing files in place." << std::endl;
                }
                tempPath.clear();
                super::open(filename, mode);
                return;
            }
            if (is_open())
            {
                atomic = true;
                // keep the target's permissions and ownership.
                struct stat st;
                if (::stat(targetPath.c_str(), &st) == 0)
                {
                    ::chmod(tempPath.c_str(), st.st_mode & 07777);
                    if (::chown(tempPath.c_str(), st.st_uid, st.st_gid) != 0)
                    {
                        // best effort.
                    }
                }
            }
            return;
        }
    }
    targetPath = filename;
    tempPath.clear();
    super::open(filename, mode);
}

void ofstream_synced::open_in_place(const fs::path &filename, ios_base::openmode mode)
{
    if (is_open())
    {
        close();
    }
    atomic = false;
    targetPath = filename;
    tempPath.clear();
    super::open(filename, mode | ios_base::out);
}

void ofstream_synced::close()
{
    if (!is_open())
    {
        return;
    }
    if (!atomic)
    {
        super::close();
        fsyncPath(targetPath, O_RDONLY);
        return;
    }
    atomic = false;
    super::flush();
    bool ok = !super::fail() && std::uncaught_exceptions() <= uncaughtAtOpen;
    super::close();
    if (ok)
    {
        fsyncPath(tempPath, O_RDONLY);
        std::error_code ec;
        fs::rename(tempPath, targetPath, ec);
        if (ec)
        {
            ok = false;
            setstate(ios_base::failbit);
        }
        else
        {
            fs::path dir = targetPath.parent_path();
            if (dir.empty())
            {
                dir = ".";
            }
            fsyncPath(dir, O_RDONLY | O_DIRECTORY);
        }
    }
    if (!ok)
    {
        std::error_code ec;
        fs::remove(tempPath, ec);
    }
}

ofstream_synced::~ofstream_synced()
{
    close();
}
