#ifndef KVDB_STATUS_HPP
#define KVDB_STATUS_HPP

#include <string>
#include <utility>

namespace kvdb {

enum class StatusCode {
    OK = 0,
    NOT_FOUND,
    INVALID_ARGUMENT,
    TOO_LARGE,
    DISK_FULL,
    IO_ERROR,
    CORRUPT,
    CLOSED
};

class Status {
public:
    Status() : code_(StatusCode::OK), message_() {}
    Status(StatusCode code, std::string message = "")
        : code_(code), message_(std::move(message)) {}

    static Status Ok() { return Status(); }
    static Status NotFound(std::string msg = "Key not found") {
        return Status(StatusCode::NOT_FOUND, std::move(msg));
    }
    static Status InvalidArgument(std::string msg = "Invalid argument") {
        return Status(StatusCode::INVALID_ARGUMENT, std::move(msg));
    }
    static Status TooLarge(std::string msg = "Key or value too large") {
        return Status(StatusCode::TOO_LARGE, std::move(msg));
    }
    static Status DiskFull(std::string msg = "Disk or file size limit reached") {
        return Status(StatusCode::DISK_FULL, std::move(msg));
    }
    static Status IoError(std::string msg = "I/O error") {
        return Status(StatusCode::IO_ERROR, std::move(msg));
    }
    static Status Corrupt(std::string msg = "Database file corrupt") {
        return Status(StatusCode::CORRUPT, std::move(msg));
    }
    static Status Closed(std::string msg = "Database is closed or poisoned") {
        return Status(StatusCode::CLOSED, std::move(msg));
    }

    bool ok() const { return code_ == StatusCode::OK; }
    bool is_not_found() const { return code_ == StatusCode::NOT_FOUND; }
    bool is_invalid_argument() const { return code_ == StatusCode::INVALID_ARGUMENT; }
    bool is_too_large() const { return code_ == StatusCode::TOO_LARGE; }
    bool is_disk_full() const { return code_ == StatusCode::DISK_FULL; }
    bool is_io_error() const { return code_ == StatusCode::IO_ERROR; }
    bool is_corrupt() const { return code_ == StatusCode::CORRUPT; }
    bool is_closed() const { return code_ == StatusCode::CLOSED; }

    StatusCode code() const { return code_; }
    const std::string& message() const { return message_; }

    const char* code_string() const {
        switch (code_) {
            case StatusCode::OK:               return "OK";
            case StatusCode::NOT_FOUND:        return "NOT_FOUND";
            case StatusCode::INVALID_ARGUMENT: return "INVALID_ARGUMENT";
            case StatusCode::TOO_LARGE:        return "TOO_LARGE";
            case StatusCode::DISK_FULL:        return "DISK_FULL";
            case StatusCode::IO_ERROR:         return "IO_ERROR";
            case StatusCode::CORRUPT:          return "CORRUPT";
            case StatusCode::CLOSED:           return "CLOSED";
        }
        return "UNKNOWN";
    }

    std::string to_string() const {
        if (message_.empty()) {
            return code_string();
        }
        return std::string(code_string()) + ": " + message_;
    }

private:
    StatusCode code_;
    std::string message_;
};

template <typename T>
struct Result {
    Status status;
    T value{};

    Result(Status s) : status(std::move(s)), value{} {}
    Result(T v) : status(Status::Ok()), value(std::move(v)) {}
    Result(Status s, T v) : status(std::move(s)), value(std::move(v)) {}

    bool ok() const { return status.ok(); }
};

} // namespace kvdb

#endif // KVDB_STATUS_HPP