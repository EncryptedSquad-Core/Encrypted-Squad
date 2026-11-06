#ifndef KVDB_ENGINE_HPP
#define KVDB_ENGINE_HPP

#include "kvdb/index.hpp"
#include "kvdb/platform.hpp"
#include "kvdb/status.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

namespace kvdb {

enum class BatchOpType : std::uint8_t {
    PUT = 1,
    DEL = 2
};

struct BatchOp {
    BatchOpType type  = BatchOpType::PUT;
    std::string key;
    std::string value;

    static BatchOp Put(std::string k, std::string v) {
        return BatchOp{BatchOpType::PUT, std::move(k), std::move(v)};
    }

    static BatchOp Del(std::string k) {
        return BatchOp{BatchOpType::DEL, std::move(k), std::string()};
    }
};

struct OpenOptions {
    std::uint64_t max_bytes = std::numeric_limits<std::uint64_t>::max();
};

// Optional deterministic fault-injection points for unit tests (disabled in normal/judge runs)
enum class CrashHookPoint {
    NONE = 0,
    SINGLE_AFTER_PARTIAL_WRITE,
    SINGLE_BEFORE_SYNC,
    SINGLE_AFTER_SYNC_BEFORE_ACK,
    BATCH_AFTER_THREE_MEMBERS,
    BATCH_BEFORE_COMMIT_MARKER,
    BATCH_AFTER_MARKER_BEFORE_SYNC,
    COMPACT_BEFORE_REPLACE,
    COMPACT_AFTER_REPLACE_BEFORE_CLEANUP,
    RECOVERY_BEFORE_TRUNCATE,
    RECOVERY_AFTER_TRUNCATE
};

class Database {
public:
    Database();
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // 1. open(path, options) — opens or creates database and runs crash recovery
    Status open(const std::string& path, OpenOptions options = OpenOptions{});

    // 2. close() — flushes and closes cleanly
    Status close();

    // 3. put(key, value) — durable single key-value write
    Status put(const std::string& key, const std::string& value, std::uint64_t* out_commit_seq = nullptr);

    // 4. get(key, out_value) — concurrent read of current value
    Status get(const std::string& key, std::string& out_value) const;

    // 5. del(key) — durable key removal (missing key is a no-op returning OK)
    Status del(const std::string& key, std::uint64_t* out_commit_seq = nullptr);

    // 6. batch(ops) — atomic, all-or-nothing durable batch of PUT and DEL operations
    Status batch(const std::vector<BatchOp>& ops, std::uint64_t* out_commit_seq = nullptr);

    // 7. compact() — crash-safe shadow-file compaction
    Status compact();

    // 8. dump(out_items) — returns all live key-value pairs sorted lexicographically by key
    Status dump(std::vector<std::pair<std::string, std::string>>& out_items) const;

    // 9. check(path) — static integrity check for `./kvdb check <path>`
    static Status check(const std::string& path);

    // Metadata accessors
    bool is_open() const;
    std::size_t size() const;
    std::uint64_t file_size() const;
    std::uint64_t max_bytes() const;
    double last_recovery_ms() const noexcept { return last_recovery_ms_; }

private:
    Status ensure_open_and_healthy_unlocked() const;
    Status rollback_or_poison_unlocked(std::uint64_t safe_offset);
    Status compact_unlocked();

    mutable std::shared_mutex mutex_;
    File                      file_;
    std::string               path_;
    Index                     index_;
    std::uint64_t             max_bytes_;
    std::uint64_t             append_offset_;
    std::uint64_t             next_txn_id_;
    std::uint64_t             commit_seq_;
    std::uint64_t             dead_records_;
    bool                      is_open_;
    bool                      poisoned_;
    double                    last_recovery_ms_;
};

// Test-only crash hook configuration (no-op unless explicitly armed by unit tests)
void set_crash_hook(CrashHookPoint point);
void trigger_crash_hook_if_matched(CrashHookPoint point);

} // namespace kvdb

#endif // KVDB_ENGINE_HPP