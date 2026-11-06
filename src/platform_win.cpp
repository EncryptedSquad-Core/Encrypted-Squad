#include "kvdb/platform.hpp"

#ifdef _WIN32

#include <windows.h>
#include <algorithm>
#include <limits>
#include <vector>

namespace kvdb {

namespace {

std::wstring utf8_to_wide(const std::string& str) {
    if (str.empty()) {
        return std::wstring();
    }
    int count = MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), nullptr, 0);
    if (count <= 0) {
        return std::wstring(str.begin(), str.end());
    }
    std::wstring wide(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), &wide[0], count);
    return wide;
}

Status map_win32_error(DWORD err, const std::string& context) {
    if (err == ERROR_DISK_FULL || err == ERROR_HANDLE_DISK_FULL) {
        return Status::DiskFull(context + " (Win32 error " + std::to_string(err) + ")");
    }
    return Status::IoError(context + " (Win32 error " + std::to_string(err) + ")");
}

} // namespace

File::File() noexcept
    : handle_(INVALID_HANDLE_VALUE), writable_(false), path_() {}

File::~File() {
    close();
}

File::File(File&& other) noexcept
    : handle_(other.handle_), writable_(other.writable_), path_(std::move(other.path_)) {
    other.handle_ = INVALID_HANDLE_VALUE;
    other.writable_ = false;
}

File& File::operator=(File&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        writable_ = other.writable_;
        path_ = std::move(other.path_);
        other.handle_ = INVALID_HANDLE_VALUE;
        other.writable_ = false;
    }
    return *this;
}

Status File::open(const std::string& path, OpenMode mode) {
    close();

    DWORD desired_access = 0;
    DWORD creation_disposition = 0;
    bool writable = false;

    switch (mode) {
        case OpenMode::READ_ONLY:
            desired_access = GENERIC_READ;
            creation_disposition = OPEN_EXISTING;
            writable = false;
            break;
        case OpenMode::READ_WRITE_CREATE:
            desired_access = GENERIC_READ | GENERIC_WRITE;
            creation_disposition = OPEN_ALWAYS;
            writable = true;
            break;
        case OpenMode::CREATE_TRUNCATE:
            desired_access = GENERIC_READ | GENERIC_WRITE;
            creation_disposition = CREATE_ALWAYS;
            writable = true;
            break;
    }

    DWORD share_mode = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    std::wstring wpath = utf8_to_wide(path);

    HANDLE h = CreateFileW(
        wpath.c_str(),
        desired_access,
        share_mode,
        nullptr,
        creation_disposition,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (h == INVALID_HANDLE_VALUE) {
        return map_win32_error(GetLastError(), "CreateFileW failed for " + path);
    }

    handle_ = h;
    writable_ = writable;
    path_ = path;
    return Status::Ok();
}

bool File::is_open() const noexcept {
    return handle_ != INVALID_HANDLE_VALUE;
}

Status File::write_all_at(std::uint64_t offset, const void* data, std::size_t size) {
    if (!is_open() || !writable_) {
        return Status::Closed("File not open for writing");
    }
    const auto* ptr = static_cast<const std::uint8_t*>(data);
    std::size_t remaining = size;
    std::uint64_t cur_offset = offset;

    while (remaining > 0) {
        DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(remaining, static_cast<std::size_t>(64u * 1024u * 1024u)));

        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(cur_offset & 0xFFFFFFFFull);
        ov.OffsetHigh = static_cast<DWORD>((cur_offset >> 32) & 0xFFFFFFFFull);

        DWORD written = 0;
        BOOL ok = WriteFile(static_cast<HANDLE>(handle_), ptr, chunk, &written, &ov);
        if (!ok) {
            return map_win32_error(GetLastError(), "WriteFile failed");
        }
        if (written == 0) {
            return Status::IoError("WriteFile wrote 0 bytes");
        }

        ptr += written;
        cur_offset += written;
        remaining -= written;
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
        DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(remaining, static_cast<std::size_t>(64u * 1024u * 1024u)));

        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(cur_offset & 0xFFFFFFFFull);
        ov.OffsetHigh = static_cast<DWORD>((cur_offset >> 32) & 0xFFFFFFFFull);

        DWORD got = 0;
        BOOL ok = ReadFile(static_cast<HANDLE>(handle_), ptr, chunk, &got, &ov);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_HANDLE_EOF) {
                break;
            }
            return map_win32_error(err, "ReadFile failed");
        }
        if (got == 0) {
            break; // Reached EOF
        }

        ptr += got;
        cur_offset += got;
        remaining -= got;
        bytes_read += got;
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
    if (!FlushFileBuffers(static_cast<HANDLE>(handle_))) {
        return map_win32_error(GetLastError(), "FlushFileBuffers failed");
    }
    return Status::Ok();
}

Status File::truncate(std::uint64_t new_size) {
    if (!is_open() || !writable_) {
        return Status::Closed("Cannot truncate closed or read-only file");
    }
    LARGE_INTEGER pos{};
    pos.QuadPart = static_cast<LONGLONG>(new_size);
    if (!SetFilePointerEx(static_cast<HANDLE>(handle_), pos, nullptr, FILE_BEGIN)) {
        return map_win32_error(GetLastError(), "SetFilePointerEx failed during truncate");
    }
    if (!SetEndOfFile(static_cast<HANDLE>(handle_))) {
        return map_win32_error(GetLastError(), "SetEndOfFile failed during truncate");
    }
    return Status::Ok();
}

Result<std::uint64_t> File::size() const {
    if (!is_open()) {
        return Status::Closed("Cannot query size of closed file");
    }
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(static_cast<HANDLE>(handle_), &sz)) {
        return map_win32_error(GetLastError(), "GetFileSizeEx failed");
    }
    return static_cast<std::uint64_t>(sz.QuadPart);
}

Status File::close() {
    if (!is_open()) {
        return Status::Ok();
    }
    Status sync_st = Status::Ok();
    if (writable_) {
        sync_st = sync();
    }
    BOOL closed = CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = INVALID_HANDLE_VALUE;
    writable_ = false;
    if (!sync_st.ok()) {
        return sync_st;
    }
    if (!closed) {
        return map_win32_error(GetLastError(), "CloseHandle failed");
    }
    return Status::Ok();
}

Status File::atomic_replace(const std::string& temp_path, const std::string& main_path) {
    std::wstring wtemp = utf8_to_wide(temp_path);
    std::wstring wmain = utf8_to_wide(main_path);

    if (!MoveFileExW(wtemp.c_str(), wmain.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return map_win32_error(GetLastError(), "MoveFileExW atomic replacement failed");
    }
    return Status::Ok();
}

Status File::remove_file(const std::string& path) {
    std::wstring wpath = utf8_to_wide(path);
    if (!DeleteFileW(wpath.c_str())) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            return Status::Ok();
        }
        return map_win32_error(err, "DeleteFileW failed for " + path);
    }
    return Status::Ok();
}

bool File::file_exists(const std::string& path) {
    std::wstring wpath = utf8_to_wide(path);
    DWORD attrs = GetFileAttributesW(wpath.c_str());
    return (attrs != INVALID_FILE_ATTRIBUTES) && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

} // namespace kvdb

#endif // _WIN32