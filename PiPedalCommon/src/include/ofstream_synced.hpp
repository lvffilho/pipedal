#pragma once

#include <fstream>
#include <filesystem>
#include <string>

namespace pipedal
{
    void FileSystemSync();

    // An ofstream that makes writes durable and (for truncating writes) atomic.
    //
    // Opening for output without app/ate/in writes to "<file>.tmp". close() flushes,
    // fsync()s the temp file, renames it over the target, and fsync()s the directory,
    // so a crash or power loss leaves either the old or the new content, never a
    // truncated file. If the stream failed, or the stream is destroyed while an exception
    // is propagating, the temp file is discarded and the target is left untouched.
    //
    // Append/in/ate opens and non-regular targets (devices, etc.) are written in place
    // and fsync()ed on close.
    class ofstream_synced : public std::ofstream
    {
    public:
        using super = std::ofstream;
        ofstream_synced() {}

        explicit ofstream_synced(const std::string &filename, ios_base::openmode mode = ios_base::out)
        {
            open(std::filesystem::path(filename), mode);
        }
        explicit ofstream_synced(const char *filename, ios_base::openmode mode = ios_base::out)
        {
            open(std::filesystem::path(filename), mode);
        }
        explicit ofstream_synced(const std::filesystem::path &filename, ios_base::openmode mode = ios_base::out)
        {
            open(filename, mode);
        }
        void open(const std::filesystem::path &filename, ios_base::openmode mode = ios_base::out);
        void open(const std::string &filename, ios_base::openmode mode = ios_base::out)
        {
            open(std::filesystem::path(filename), mode);
        }
        void open(const char *filename, ios_base::openmode mode = ios_base::out)
        {
            open(std::filesystem::path(filename), mode);
        }
        // Explicit non-atomic mode for long-lived streaming writers (logs): opens the file
        // in place; contents are visible immediately. fsync()ed on close.
        void open_in_place(const std::filesystem::path &filename, ios_base::openmode mode = ios_base::out);
        void close();
        ~ofstream_synced();

    private:
        bool atomic = false;
        int uncaughtAtOpen = 0;
        std::filesystem::path targetPath;
        std::filesystem::path tempPath;
    };

}
