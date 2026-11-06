#include "kvdb/platform.hpp"

#ifndef _WIN32

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace kvdb {

namespace {

Status map_posix_error(int err, const std::string& context) {
    if (err == ENOSPC || err == EDQUOT) {
        return Status::DiskFull(context + ": " + std::strerror(err));
    }
    return Status::IoError(context + ": " + std::strerror(err));
}

std::string parent_dir_of(const std::string& path) {
    auto pos = path.find_last_of('/');
    if (pos == std::string::npos) {
        return ".";
    }
    if (pos == 0) {
        return "/";
    }
    return path.substr(0, pos);
}

Status sync_parent_directory(const std::string& path) {
    std::string dir = parent_dir_of(path);
    int dir_fd = ::open(dir.c_str(), O_RDONLY);
    if (dir_fd < 0) {
        return Status::Ok(); // Best effort on filesystems that disallow directory open
    }
    int rc = ::fsync(dir_fd);
    int saved_errno = errno;
    ::close(dir_fd);
    if (rc != 0 && saved_errno != EINVAL && saved_errno != EBADF) {
        return map_posix_error(saved_errno, "fsync parent directory failed");
    }
    return Status::Ok();
}

} // namespace

File::File() noexcept
    : fd_(-1), writable_(false), path_() {}

File::~File() {
    close();
}

File::File(File&& other) noexcept
    : fd_(other.fd_), writable_(other.writable_), path_(std::move(other.path_)) {
    other.fd_ = -1;
    other.writable_ = false;
}

File& File::operator=(File&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        writable_ = other.writable_;
        path_ = std::move(other.path_);
        other.fd_ = -1;
        other.writable_ = false;
    }
    return *this;
}

Status File::open(const std::string& path, OpenMode mode) {
    close();

    int flags = 0;
    bool writable = false;

    switch (mode) {
        case OpenMode::READ_ONLY:
            flags = O_RDONLY;
            writable = false;
            break;
        case OpenMode::READ_WRITE_CREATE:
            flags = O_RDWR | O_CREAT;
            writable = true;
            break;
        case OpenMode::CREATE_TRUNCATE:
            flags = O_RDWR | O_CREAT | O_TRUNC;
            writable = true;
            break;
    }

    int fd = -1;
    do {
        fd = ::open(path.c_str(), flags, 0644);
    } while (fd < 0 && errno == EINTR);

    if (fd < 0) {
        return map_posix_error(errno, "open failed for " + path);
    }

    fd_ = fd;
    writable_ = writable;
    path_ = path;
    return Status::Ok();
}

bool File::is_open() const noexcept {
    return fd_ >= 0;
}

Status File::write_all_at(std::uint64_t offset, const void* data, std::size_t size) {
    if (!is_open() || !writable_) {
        return Status::Closed("File not open for writing");
    }
    const auto* ptr = static_cast<const std::uint8_t*>(data);
    std::size_t remaining = size;
    std::uint64_t cur_offset = offset;

    while (remaining > 0) {
        ssize_t written = ::pwrite(fd_, ptr, remaining, static_cast<off_t>(cur_offset));
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return map_posix_error(errno, "pwrite failed");
        }
        if (written == 0) {
            return Status::IoError("pwrite returned 0 bytes");
        }
        ptr += written;
        cur_offset += static_cast<std::uint64_t>(written);
        remaining -= static_cast<std::size_t>(written);
    }
    return Status::Ok();
}

Status File::read_up_to_at(std::uint64_t offset, void* data, std::size_t size, std::size_t& bytes_read) const {
    bytes_read = 0;
    if (!is_open()) {
        return Status::Closed("File not open for reading");
    }
    auto* ptr = static_cast<std::uint8_t*>(data);
    std::size_t remaining = size;
    std::uint64_t cur_offset = offset;

    while (remaining > 0) {
        ssize_t got = ::pread(fd_, ptr, remaining, static_cast<off_t>(cur_offset));
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return map_posix_error(errno, "pread failed");
        }
        if (got == 0) {
            break; // EOF
        }
        ptr += got;
        cur_offset += static_cast<std::uint64_t>(got);
        remaining -= static_cast<std::size_t>(got);
        bytes_read += static_cast<std::size_t>(got);
    }
    return Status::Ok();
}

Status File::read_exact_at(std::uint64_t offset, void* data, std::size_t size) const {
    std::size_t got = 0;
    Status st = read_up_to_at(offset, data, size, got);
    if (!st.ok()) {
        return st;
    }
    if (got != size) {
        return Status::Corrupt("Unexpected EOF during positional read");
    }
    return Status::Ok();
}

Status File::sync() {
    if (!is_open()) {
        return Status::Closed("Cannot sync closed file");
    }
    if (!writable_) {
        return Status::Ok();
    }
    int rc = 0;
    do {
        rc = ::fsync(fd_);
    } while (rc < 0 && errno == EINTR);

    if (rc < 0) {
        return map_posix_error(errno, "fsync failed");
    }
    return Status::Ok();
}

Status File::truncate(std::uint64_t new_size) {
    if (!is_open() || !writable_) {
        return Status::Closed("Cannot truncate closed or read-only file");
    }
    int rc = 0;
    do {
        rc = ::ftruncate(fd_, static_cast<off_t>(new_size));
    } while (rc < 0 && errno == EINTR);

    if (rc < 0) {
        return map_posix_error(errno, "ftruncate failed");
    }
    return Status::Ok();
}

Result<std::uint64_t> File::size() const {
    if (!is_open()) {
        return Status::Closed("Cannot query size of closed file");
    }
    struct stat st{};
    if (::fstat(fd_, &st) != 0) {
        return map_posix_error(errno, "fstat failed");
    }
    return static_cast<std::uint64_t>(st.st_size);
}

Status File::close() {
    if (!is_open()) {
        return Status::Ok();
    }
    Status sync_st = Status::Ok();
    if (writable_) {
        sync_st = sync();
    }
    int rc = ::close(fd_);
    int saved_errno = errno;
    fd_ = -1;
    writable_ = false;
    if (!sync_st.ok()) {
        return sync_st;
    }
    if (rc != 0 && saved_errno != EINTR) {
        return map_posix_error(saved_errno, "close failed");
    }
    return Status::Ok();
}

Status File::atomic_replace(const std::string& temp_path, const std::string& main_path) {
    if (::rename(temp_path.c_str(), main_path.c_str()) != 0) {
        return map_posix_error(errno, "rename failed in atomic_replace");
    }
    return sync_parent_directory(main_path);
}

Status File::remove_file(const std::string& path) {
    if (::unlink(path.c_str()) != 0) {
        if (errno == ENOENT) {
            return Status::Ok();
        }
        return map_posix_error(errno, "unlink failed for " + path);
    }
    return Status::Ok();
}

bool File::file_exists(const std::string& path) {
    struct stat st{};
    return (::stat(path.c_str(), &st) == 0) && S_ISREG(st.st_mode);
}

} // namespace kvdb

#endif // !_WIN32