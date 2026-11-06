#include "kvdb/engine.hpp"
#include "kvdb/compaction.hpp"
#include "kvdb/crc32.hpp"
#include "kvdb/record.hpp"
#include "kvdb/recovery.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>

namespace kvdb {

namespace {

std::atomic<int> g_crash_hook{static_cast<int>(CrashHookPoint::NONE)};

} // namespace

void set_crash_hook(CrashHookPoint point) {
    g_crash_hook.store(static_cast<int>(point), std::memory_order_Seq_cst);
}

void trigger_crash_hook_if_matched(CrashHookPoint point) {
    if (g_crash_hook.load(std::memory_order_seq_cst) == static_cast<int>(point)) {
        std::_Exit(137); // Immediate ungraceful process termination (simulates kill -9)
    }
}

Database::Database()
    : max_bytes_(std::numeric_limits<std::uint64_t>::max()),
      append_offset_(kFileHeaderSize),
      next_txn_id_(1),
      commit_seq_(0),
      dead_records_(0),
      is_open_(false),
      poisoned_(false),
      last_recovery_ms_(0.0) {}

Database::~Database() {
    close();
}

Status Database::open(const std::string& path, OpenOptions options) {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    auto t0 = std::chrono::steady_clock::now();

    if (is_open_) {
        file_.close();
        is_open_ = false;
    }

    if (options.max_bytes < kFileHeaderSize) {
        return Status::InvalidArgument("max_bytes must be at least 12 bytes");
    }

    Status st = file_.open(path, OpenMode::READ_WRITE_CREATE);
    if (!st.ok()) {
        return st;
    }

    RecoveryResult rec{};
    st = recover_database(file_, path, index_, rec);
    if (!st.ok()) {
        file_.close();
        return st;
    }

    path_          = path;
    max_bytes_     = options.max_bytes;
    append_offset_ = rec.append_offset;
    next_txn_id_   = rec.next_txn_id;
    commit_seq_    = 0;
    dead_records_  = (rec.committed_records > index_.size())
                         ? (rec.committed_records - index_.size())
                         : 0;
    poisoned_      = false;
    is_open_       = true;

    auto t1 = std::chrono::steady_clock::now();
    last_recovery_ms_ = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return Status::Ok();
}

Status Database::close() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (!is_open_) {
        return Status::Ok();
    }
    Status st = file_.close();
    index_.clear();
    is_open_  = false;
    poisoned_ = false;
    return st;
}

Status Database::ensure_open_and_healthy_unlocked() const {
    if (!is_open_) {
        return Status::Closed("Database is not open");
    }
    if (poisoned_) {
        return Status::Closed("Database instance is poisoned after failed rollback; reopen to recover");
    }
    return Status::Ok();
}

Status Database::rollback_or_poison_unlocked(std::uint64_t safe_offset) {
    Status tst = file_.truncate(safe_offset);
    if (!tst.ok()) {
        poisoned_ = true;
        return tst;
    }
    Status sst = file_.sync();
    if (!sst.ok()) {
        poisoned_ = true;
        return sst;
    }
    append_offset_ = safe_offset;
    return Status::Ok();
}

Status Database::compact_unlocked() {
    Status st = compact_database(file_, path_, max_bytes_, index_, append_offset_, next_txn_id_);
    if (st.ok()) {
        dead_records_ = 0;
    }
    return st;
}

Status Database::put(const std::string& key, const std::string& value, std::uint64_t* out_commit_seq) {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    Status st = ensure_open_and_healthy_unlocked();
    if (!st.ok()) {
        return st;
    }
    st = validate_key_size(key.size());
    if (!st.ok()) {
        return st;
    }
    st = validate_value_size(value.size());
    if (!st.ok()) {
        return st;
    }

    auto rec_size_res = calculate_record_size(key.size(), value.size());
    if (!rec_size_res.ok()) {
        return rec_size_res.status;
    }
    std::uint64_t rec_size = rec_size_res.value;

    // If cap would be exceeded and log has reclaimable dead records, auto-compact first (Scenario E5)
    if (append_offset_ > max_bytes_ || rec_size > max_bytes_ - append_offset_) {
        if (dead_records_ > 0) {
            compact_unlocked();
        }
        if (append_offset_ > max_bytes_ || rec_size > max_bytes_ - append_offset_) {
            return Status::DiskFull("PUT exceeds configured max_bytes");
        }
    }

    std::uint64_t start_offset = append_offset_;
    std::vector<std::uint8_t> buf;
    serialize_record(RecordType::PUT, kFlagNone, next_txn_id_, 0, 1, key, value, buf);

    if (g_crash_hook.load(std::memory_order_seq_cst) ==
        static_cast<int>(CrashHookPoint::SINGLE_AFTER_PARTIAL_WRITE)) {
        file_.write_all_at(start_offset, buf.data(), buf.size() / 2);
        trigger_crash_hook_if_matched(CrashHookPoint::SINGLE_AFTER_PARTIAL_WRITE);
    }

    st = file_.write_all_at(start_offset, buf.data(), buf.size());
    if (!st.ok()) {
        rollback_or_poison_unlocked(start_offset);
        return st;
    }

    trigger_crash_hook_if_matched(CrashHookPoint::SINGLE_BEFORE_SYNC);

    st = file_.sync();
    if (!st.ok()) {
        rollback_or_poison_unlocked(start_offset);
        return st;
    }

    trigger_crash_hook_if_matched(CrashHookPoint::SINGLE_AFTER_SYNC_BEFORE_ACK);

    if (index_.contains(key)) {
        ++dead_records_;
    }
    std::uint64_t val_offset = start_offset + kRecordHeaderSize + key.size();
    index_.put(key, val_offset, static_cast<std::uint32_t>(value.size()));

    append_offset_ += rec_size;
    ++next_txn_id_;
    ++commit_seq_;
    if (out_commit_seq != nullptr) {
        *out_commit_seq = commit_seq_;
    }
    return Status::Ok();
}

Status Database::del(const std::string& key, std::uint64_t* out_commit_seq) {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    Status st = ensure_open_and_healthy_unlocked();
    if (!st.ok()) {
        return st;
    }
    st = validate_key_size(key.size());
    if (!st.ok()) {
        return st;
    }

    // Deleting a missing key is not an error and changes no state
    if (!index_.contains(key)) {
        ++commit_seq_;
        if (out_commit_seq != nullptr) {
            *out_commit_seq = commit_seq_;
        }
        return Status::Ok();
    }

    auto rec_size_res = calculate_record_size(key.size(), 0);
    if (!rec_size_res.ok()) {
        return rec_size_res.status;
    }
    std::uint64_t rec_size = rec_size_res.value;

    if (append_offset_ > max_bytes_ || rec_size > max_bytes_ - append_offset_) {
        if (dead_records_ > 0) {
            compact_unlocked();
        }
        if (append_offset_ > max_bytes_ || rec_size > max_bytes_ - append_offset_) {
            return Status::DiskFull("DELETE exceeds configured max_bytes");
        }
    }

    std::uint64_t start_offset = append_offset_;
    std::vector<std::uint8_t> buf;
    serialize_record(RecordType::DEL, kFlagNone, next_txn_id_, 0, 1, key, std::string(), buf);

    st = file_.write_all_at(start_offset, buf.data(), buf.size());
    if (!st.ok()) {
        rollback_or_poison_unlocked(start_offset);
        return st;
    }

    trigger_crash_hook_if_matched(CrashHookPoint::SINGLE_BEFORE_SYNC);

    st = file_.sync();
    if (!st.ok()) {
        rollback_or_poison_unlocked(start_offset);
        return st;
    }

    trigger_crash_hook_if_matched(CrashHookPoint::SINGLE_AFTER_SYNC_BEFORE_ACK);

    index_.erase(key);
    dead_records_ += 2; // Both the old PUT and this DEL tombstone are now reclaimable

    append_offset_ += rec_size;
    ++next_txn_id_;
    ++commit_seq_;
    if (out_commit_seq != nullptr) {
        *out_commit_seq = commit_seq_;
    }
    return Status::Ok();
}

Status Database::get(const std::string& key, std::string& out_value) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);

    Status st = ensure_open_and_healthy_unlocked();
    if (!st.ok()) {
        return st;
    }
    st = validate_key_size(key.size());
    if (!st.ok()) {
        return st;
    }

    IndexEntry entry{};
    if (!index_.get(key, entry)) {
        return Status::NotFound();
    }

    out_value.resize(entry.value_len);
    if (entry.value_len > 0) {
        st = file_.read_exact_at(entry.value_offset, &out_value[0], entry.value_len);
        if (!st.ok()) {
            return st;
        }
    }
    return Status::Ok();
}

Status Database::batch(const std::vector<BatchOp>& ops, std::uint64_t* out_commit_seq) {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    Status st = ensure_open_and_healthy_unlocked();
    if (!st.ok()) {
        return st;
    }
    if (ops.empty()) {
        return Status::InvalidArgument("Batch must contain at least 1 operation");
    }
    if (ops.size() > kMaxBatchOps) {
        return Status::TooLarge("Batch exceeds maximum of 10,000 operations");
    }

    // 1. Preflight validation and exact total byte calculation
    std::uint64_t total_batch_bytes = kRecordHeaderSize + 4; // Commit marker is 36 bytes
    for (const auto& op : ops) {
        st = validate_key_size(op.key.size());
        if (!st.ok()) {
            return st;
        }
        if (op.type == BatchOpType::PUT) {
            st = validate_value_size(op.value.size());
            if (!st.ok()) {
                return st;
            }
            total_batch_bytes += kRecordHeaderSize + op.key.size() + op.value.size();
        } else {
            total_batch_bytes += kRecordHeaderSize + op.key.size();
        }
    }

    if (append_offset_ > max_bytes_ || total_batch_bytes > max_bytes_ - append_offset_) {
        if (dead_records_ > 0) {
            compact_unlocked();
        }
        if (append_offset_ > max_bytes_ || total_batch_bytes > max_bytes_ - append_offset_) {
            return Status::DiskFull("Batch exceeds configured max_bytes");
        }
    }

    const std::uint64_t batch_start = append_offset_;
    const std::uint64_t txn_id      = next_txn_id_;
    const std::uint32_t op_count    = static_cast<std::uint32_t>(ops.size());

    std::uint64_t cur_offset      = batch_start;
    std::uint32_t batch_crc_state = Crc32::kInitialState;

    std::vector<std::uint64_t> val_offsets(ops.size(), 0);
    std::vector<std::uint8_t>  member_buf;

    // 2. Stream all batch member records
    for (std::uint32_t i = 0; i < op_count; ++i) {
        const auto& op = ops[i];
        RecordType rtype = (op.type == BatchOpType::PUT) ? RecordType::PUT : RecordType::DEL;
        const std::string& val_ref = (op.type == BatchOpType::PUT) ? op.value : std::string();

        serialize_record(rtype, kFlagBatchMember, txn_id, i, op_count, op.key, val_ref, member_buf);

        st = file_.write_all_at(cur_offset, member_buf.data(), member_buf.size());
        if (!st.ok()) {
            rollback_or_poison_unlocked(batch_start);
            return st;
        }

        batch_crc_state = Crc32::update(batch_crc_state, member_buf.data(), member_buf.size());
        val_offsets[i]  = cur_offset + kRecordHeaderSize + op.key.size();
        cur_offset     += member_buf.size();

        if (i == 2) {
            trigger_crash_hook_if_matched(CrashHookPoint::BATCH_AFTER_THREE_MEMBERS);
        }
    }

    // 3. First fsync: ensure all batch members are on disk before writing COMMIT marker
    st = file_.sync();
    if (!st.ok()) {
        rollback_or_poison_unlocked(batch_start);
        return st;
    }

    trigger_crash_hook_if_matched(CrashHookPoint::BATCH_BEFORE_COMMIT_MARKER);

    // 4. Write 36-byte COMMIT marker
    std::uint32_t batch_crc = Crc32::finalize(batch_crc_state);
    std::uint8_t commit_buf[kRecordHeaderSize + 4];
    serialize_commit_marker(txn_id, op_count, batch_crc, commit_buf);

    st = file_.write_all_at(cur_offset, commit_buf, sizeof(commit_buf));
    if (!st.ok()) {
        rollback_or_poison_unlocked(batch_start);
        return st;
    }
    cur_offset += sizeof(commit_buf);

    trigger_crash_hook_if_matched(CrashHookPoint::BATCH_AFTER_MARKER_BEFORE_SYNC);

    // 5. Second fsync: make the COMMIT marker durable
    st = file_.sync();
    if (!st.ok()) {
        rollback_or_poison_unlocked(batch_start);
        return st;
    }

    // 6. Apply all operations to the in-memory index in exact batch order (B5)
    for (std::uint32_t i = 0; i < op_count; ++i) {
        const auto& op = ops[i];
        if (op.type == BatchOpType::PUT) {
            if (index_.contains(op.key)) {
                ++dead_records_;
            }
            index_.put(op.key, val_offsets[i], static_cast<std::uint32_t>(op.value.size()));
        } else {
            if (index_.erase(op.key)) {
                dead_records_ += 2;
            } else {
                ++dead_records_;
            }
        }
    }

    append_offset_ = cur_offset;
    ++next_txn_id_;
    ++commit_seq_;
    if (out_commit_seq != nullptr) {
        *out_commit_seq = commit_seq_;
    }
    return Status::Ok();
}

Status Database::compact() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    Status st = ensure_open_and_healthy_unlocked();
    if (!st.ok()) {
        return st;
    }
    return compact_unlocked();
}

Status Database::dump(std::vector<std::pair<std::string, std::string>>& out_items) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    Status st = ensure_open_and_healthy_unlocked();
    if (!st.ok()) {
        return st;
    }

    out_items.clear();
    out_items.reserve(index_.size());

    for (const auto& kv : index_.entries()) {
        std::string val(kv.second.value_len, '\0');
        if (kv.second.value_len > 0) {
            st = file_.read_exact_at(kv.second.value_offset, &val[0], kv.second.value_len);
            if (!st.ok()) {
                return st;
            }
        }
        out_items.emplace_back(kv.first, std::move(val));
    }

    std::sort(out_items.begin(), out_items.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return Status::Ok();
}

Status Database::check(const std::string& path) {
    return verify_database_file(path);
}

bool Database::is_open() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return is_open_;
}

std::size_t Database::size() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return index_.size();
}

std::uint64_t Database::file_size() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return append_offset_;
}

std::uint64_t Database::max_bytes() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return max_bytes_;
}

} // namespace kvdb