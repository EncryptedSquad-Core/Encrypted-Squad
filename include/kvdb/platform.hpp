#ifndef KVDB_PLATFORM_HPP
#define KVDB_PLATFORM_HPP

#include "kvdb/status.hpp"
#include <cstddef>
#include <cstdint>
#include <string>

namespace kvdb {

enum class OpenMode {
    READ_ONLY,
    READ_WRITE_CREATE,
    CREATE_TRUNCATE
};

class File {
public:
    File() noexcept;
    ~File();

    File(const File&) = delete;
    File& operator=(const File&) = delete;

    File(File&& other) noexcept;
    File& operator=(File&& other) noexcept;

    Status open(const std::string& path, OpenMode mode);
    bool is_open() const noexcept;

    // Positional write of all bytes; handles partial transfers and EINTR.
    // Maps OS disk-full errors to Status::DiskFull.
    Status write_all_at(std::uint64_t offset, const void* data, std::size_t size);

    // Positional read of up to `size` bytes; sets `bytes_read` to actual count.
    // Succeeds with bytes_read < size if EOF is reached cleanly.
    Status read_up_to_at(std::uint64_t offset, void* data, std::size_t size, std::size_t& bytes_read) const;

    // Positional read requiring exactly `size` bytes; returns IoError/Corrupt if short.
    Status read_exact_at(std::uint64_t offset, void* data, std::size_t size) const;

    // Durability barrier: fsync() on POSIX, FlushFileBuffers() on Windows.
    Status sync();

    // Truncates or extends the file to `new_size`.
    Status truncate(std::uint64_t new_size);

    // Queries the current file size in bytes from the OS.
    Result<std::uint64_t> size() const;

    // Flushes (if writable) and closes the OS handle cleanly.
    Status close();

    const std::string& path() const noexcept { return path_; }

    // Atomically replaces `main_path` with `temp_path` in the same directory.
    // Syncs parent directory on POSIX; uses ReplaceFileW/MoveFileExW with WRITE_THROUGH on Windows.
    static Status atomic_replace(const std::string& temp_path, const std::string& main_path);

    // Removes a file if it exists; succeeds if the file is already absent.
    static Status remove_file(const std::string& path);

    // Checks whether a file path currently exists.
    static bool file_exists(const std::string& path);

private:
#ifdef _WIN32
    void* handle_;
#else
    int fd_;
#endif
    bool writable_;
    std::string path_;
};

} // namespace kvdb

#endif // KVDB_PLATFORM_HPP