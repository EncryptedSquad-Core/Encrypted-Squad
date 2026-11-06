#include "kvdb/recovery.hpp"
#include "kvdb/crc32.hpp"
#include "kvdb/engine.hpp"
#include <algorithm>
#include <cstring>
#include <vector>

namespace kvdb {

namespace {

// 4 MiB sequential read buffer for <2s recovery on 1,000,000 keys
class SequentialLogReader {
public:
    static constexpr std::size_t kBufferSize = 4u * 1024u * 1024u;

    SequentialLogReader(const File& file, std::uint64_t file_size)
        : file_(file),
          file_size_(file_size),
          buf_(std::min<std::uint64_t>(kBufferSize, std::max<std::uint64_t>(file_size, 4096))),
          buf_file_offset_(0),
          buf_valid_len_(0) {}

    Status read_exact(std::uint64_t offset, void* dst, std::size_t len) {
        if (offset + len > file_size_) {
            return Status::Corrupt("Read past EOF");
        }
        auto* out = static_cast<std::uint8_t*>(dst);
        std::size_t remaining = len;
        std::uint64_t cur = offset;

        while (remaining > 0) {
            if (cur < buf_file_offset_ || cur >= buf_file_offset_ + buf_valid_len_) {
                Status st = refill(cur);
                if (!st.ok()) {
                    return st;
                }
            }
            std::size_t avail = static_cast<std::size_t>((buf_file_offset_ + buf_valid_len_) - cur);
            std::size_t take = std::min(remaining, avail);
            std::memcpy(out, buf_.data() + (cur - buf_file_offset_), take);
            out += take;
            cur += take;
            remaining -= take;
        }
        return Status::Ok();
    }

    // Updates CRC32 over `len` bytes starting at `offset` without allocating a heap buffer
    Status update_crc_range(std::uint64_t offset, std::size_t len, std::uint32_t& crc_state) {
        if (offset + len > file_size_) {
            return Status::Corrupt("CRC scan past EOF");
        }
        std::size_t remaining = len;
        std::uint64_t cur = offset;

        while (remaining > 0) {
            if (cur < buf_file_offset_ || cur >= buf_file_offset_ + buf_valid_len_) {
                Status st = refill(cur);
                if (!st.ok()) {
                    return st;
                }
            }
            std::size_t avail = static_cast<std::size_t>((buf_file_offset_ + buf_valid_len_) - cur);
            std::size_t take = std::min(remaining, avail);
            crc_state = Crc32::update(crc_state, buf_.data() + (cur - buf_file_offset_), take);
            cur += take;
            remaining -= take;
        }
        return Status::Ok();
    }

private:
    Status refill(std::uint64_t target_offset) {
        std::size_t got = 0;
        Status st = file_.read_up_to_at(target_offset, buf_.data(), buf_.size(), got);
        if (!st.ok()) {
            return st;
        }
        if (got == 0) {
            return Status::Corrupt("Unexpected EOF while refilling sequential buffer");
        }
        buf_file_offset_ = target_offset;
        buf_valid_len_ = got;
        return Status::Ok();
    }

    const File&               file_;
    std::uint64_t             file_size_;
    std::vector<std::uint8_t> buf_;
    std::uint64_t             buf_file_offset_;
    std::size_t               buf_valid_len_;
};

Status repair_tail_at(
    File& file,
    std::uint64_t safe_offset,
    std::uint64_t original_size,
    RecoveryResult& out_result) {

    trigger_crash_hook_if_matched(CrashHookPoint::RECOVERY_BEFORE_TRUNCATE);

    Status st = file.truncate(safe_offset);
    if (!st.ok()) {
        return st;
    }
    trigger_crash_hook_if_matched(CrashHookPoint::RECOVERY_AFTER_TRUNCATE);

    st = file.sync();
    if (!st.ok()) {
        return st;
    }
    out_result.append_offset   = safe_offset;
    out_result.repaired_tail   = true;
    out_result.truncated_bytes = (original_size > safe_offset) ? (original_size - safe_offset) : 0;
    return Status::Ok();
}

} // namespace

Status recover_database(
    File& file,
    const std::string& main_path,
    Index& out_index,
    RecoveryResult& out_result) {

    out_index.clear();
    out_result = RecoveryResult{};

    auto sz_res = file.size();
    if (!sz_res.ok()) {
        return sz_res.status;
    }
    std::uint64_t file_size = sz_res.value;

    // 1. Handle empty or interrupted (<12 bytes) first-time header creation
    if (file_size < kFileHeaderSize) {
        if (file_size > 0) {
            Status tst = file.truncate(0);
            if (!tst.ok()) {
                return tst;
            }
        }
        std::uint8_t hdr_bytes[kFileHeaderSize];
        encode_file_header(hdr_bytes);
        Status wst = file.write_all_at(0, hdr_bytes, kFileHeaderSize);
        if (!wst.ok()) {
            return wst;
        }
        Status sst = file.sync();
        if (!sst.ok()) {
            return sst;
        }
        file_size = kFileHeaderSize;
        out_result.initialized_header = true;
    } else {
        std::uint8_t hdr_bytes[kFileHeaderSize];
        Status rst = file.read_exact_at(0, hdr_bytes, kFileHeaderSize);
        if (!rst.ok()) {
            return rst;
        }
        FileHeader hdr{};
        Status vst = decode_and_validate_file_header(hdr_bytes, hdr);
        if (!vst.ok()) {
            return vst;
        }
    }

    // Reserve hash table buckets upfront based on file size (~64 bytes per typical small record)
    if (file_size > kFileHeaderSize) {
        std::size_t est_records = static_cast<std::size_t>((file_size - kFileHeaderSize) / 48u);
        std::size_t reserve_cap = std::min<std::size_t>(est_records + 16u, 1200000u);
        if (reserve_cap > 1024u) {
            out_index.reserve(reserve_cap);
        }
    }

    SequentialLogReader reader(file, file_size);

    std::uint64_t offset            = kFileHeaderSize;
    std::uint64_t expected_txn_id   = 1;
    bool          in_batch          = false;
    std::uint64_t batch_start_off   = kFileHeaderSize;
    std::uint64_t batch_txn_id      = 0;
    std::uint32_t batch_op_count    = 0;
    std::uint32_t batch_next_idx    = 0;
    std::uint32_t batch_crc_state   = Crc32::kInitialState;

    std::vector<PendingBatchOp> pending_ops;
    std::string key_scratch;

    while (offset < file_size) {
        std::uint64_t record_start = offset;
        std::uint64_t rollback_target = in_batch ? batch_start_off : record_start;

        // Check if fewer than 32 header bytes remain at EOF (torn header)
        if (file_size - offset < kRecordHeaderSize) {
            Status rst = repair_tail_at(file, rollback_target, file_size, out_result);
            if (!rst.ok()) {
                return rst;
            }
            in_batch = false;
            pending_ops.clear();
            break;
        }

        std::uint8_t raw_hdr[kRecordHeaderSize];
        Status st = reader.read_exact(offset, raw_hdr, kRecordHeaderSize);
        if (!st.ok()) {
            return st;
        }

        RecordHeader hdr{};
        Status dst = decode_record_header(raw_hdr, hdr);
        if (!dst.ok()) {
            // Torn/damaged tail record -> truncate to transaction boundary
            Status rst = repair_tail_at(file, rollback_target, file_size, out_result);
            if (!rst.ok()) {
                return rst;
            }
            in_batch = false;
            pending_ops.clear();
            break;
        }

        std::uint64_t payload_len = static_cast<std::uint64_t>(hdr.key_len) +
                                    static_cast<std::uint64_t>(hdr.value_len);
        std::uint64_t total_rec_len = static_cast<std::uint64_t>(kRecordHeaderSize) + payload_len;

        // Check if the declared payload extends beyond EOF (torn payload)
        if (file_size - offset < total_rec_len) {
            Status rst = repair_tail_at(file, rollback_target, file_size, out_result);
            if (!rst.ok()) {
                return rst;
            }
            in_batch = false;
            pending_ops.clear();
            break;
        }

        // Verify record_crc32 over header[0..27] + key + value
        std::uint32_t rec_crc_state = Crc32::update(Crc32::kInitialState, raw_hdr, 28);
        if (hdr.key_len > 0) {
            key_scratch.resize(hdr.key_len);
            st = reader.read_exact(offset + kRecordHeaderSize, &key_scratch[0], hdr.key_len);
            if (!st.ok()) {
                return st;
            }
            rec_crc_state = Crc32::update(rec_crc_state, key_scratch.data(), hdr.key_len);
        } else {
            key_scratch.clear();
        }

        std::uint64_t value_offset = offset + kRecordHeaderSize + hdr.key_len;
        std::uint8_t commit_payload[4]{};

        if (hdr.type == RecordType::COMMIT) {
            st = reader.read_exact(value_offset, commit_payload, 4);
            if (!st.ok()) {
                return st;
            }
            rec_crc_state = Crc32::update(rec_crc_state, commit_payload, 4);
        } else if (hdr.value_len > 0) {
            st = reader.update_crc_range(value_offset, hdr.value_len, rec_crc_state);
            if (!st.ok()) {
                return st;
            }
        }

        std::uint32_t computed_rec_crc = Crc32::finalize(rec_crc_state);
        if (computed_rec_crc != hdr.record_crc32) {
            // CRC mismatch at the tail -> roll back incomplete transaction
            Status rst = repair_tail_at(file, rollback_target, file_size, out_result);
            if (!rst.ok()) {
                return rst;
            }
            in_batch = false;
            pending_ops.clear();
            break;
        }

        const bool is_batch_flag = (hdr.flags & kFlagBatchMember) != 0;

        if (!is_batch_flag) {
            // Standalone PUT or DELETE
            if (in_batch || hdr.txn_id != expected_txn_id) {
                Status rst = repair_tail_at(file, rollback_target, file_size, out_result);
                if (!rst.ok()) {
                    return rst;
                }
                in_batch = false;
                pending_ops.clear();
                break;
            }

            if (hdr.type == RecordType::PUT) {
                out_index.put(std::move(key_scratch), value_offset, hdr.value_len);
            } else if (hdr.type == RecordType::DEL) {
                out_index.erase(key_scratch);
            }

            ++expected_txn_id;
            ++out_result.committed_records;
            offset += total_rec_len;
        } else {
            // Batch Member or Batch COMMIT Marker
            if (hdr.type != RecordType::COMMIT) {
                if (!in_batch) {
                    if (hdr.txn_id != expected_txn_id || hdr.op_index != 0) {
                        Status rst = repair_tail_at(file, record_start, file_size, out_result);
                        if (!rst.ok()) {
                            return rst;
                        }
                        break;
                    }
                    in_batch        = true;
                    batch_start_off = record_start;
                    batch_txn_id    = hdr.txn_id;
                    batch_op_count  = hdr.op_count;
                    batch_next_idx  = 0;
                    batch_crc_state = Crc32::kInitialState;
                    pending_ops.clear();
                    pending_ops.reserve(hdr.op_count);
                }

                if (hdr.txn_id != batch_txn_id ||
                    hdr.op_count != batch_op_count ||
                    hdr.op_index != batch_next_idx) {
                    Status rst = repair_tail_at(file, batch_start_off, file_size, out_result);
                    if (!rst.ok()) {
                        return rst;
                    }
                    in_batch = false;
                    pending_ops.clear();
                    break;
                }

                // Update aggregate batch_crc32 over the entire serialized member record
                batch_crc_state = Crc32::update(batch_crc_state, raw_hdr, kRecordHeaderSize);
                if (hdr.key_len > 0) {
                    batch_crc_state = Crc32::update(batch_crc_state, key_scratch.data(), hdr.key_len);
                }
                if (hdr.value_len > 0) {
                    st = reader.update_crc_range(value_offset, hdr.value_len, batch_crc_state);
                    if (!st.ok()) {
                        return st;
                    }
                }

                PendingBatchOp pop;
                pop.type         = hdr.type;
                pop.key          = std::move(key_scratch);
                pop.value_offset = value_offset;
                pop.value_len    = hdr.value_len;
                pending_ops.push_back(std::move(pop));

                ++batch_next_idx;
                offset += total_rec_len;
            } else {
                // RecordType::COMMIT
                std::uint32_t expected_batch_crc = Crc32::finalize(batch_crc_state);
                std::uint32_t stored_batch_crc   = read_le32(commit_payload);

                if (!in_batch ||
                    hdr.txn_id != batch_txn_id ||
                    hdr.op_count != batch_op_count ||
                    batch_next_idx != batch_op_count ||
                    stored_batch_crc != expected_batch_crc) {
                    Status rst = repair_tail_at(file, rollback_target, file_size, out_result);
                    if (!rst.ok()) {
                        return rst;
                    }
                    in_batch = false;
                    pending_ops.clear();
                    break;
                }

                // Batch is durably committed! Apply all pending operations in order
                for (auto& op : pending_ops) {
                    if (op.type == RecordType::PUT) {
                        out_index.put(std::move(op.key), op.value_offset, op.value_len);
                    } else if (op.type == RecordType::DEL) {
                        out_index.erase(op.key);
                    }
                }

                in_batch = false;
                pending_ops.clear();
                ++expected_txn_id;
                out_result.committed_records += batch_op_count;
                offset += total_rec_len;
            }
        }
    }

    // If EOF was reached while a batch was still waiting for its COMMIT marker, roll back to batch_start_off
    if (in_batch) {
        Status rst = repair_tail_at(file, batch_start_off, file_size, out_result);
        if (!rst.ok()) {
            return rst;
        }
        pending_ops.clear();
    } else if (!out_result.repaired_tail) {
        out_result.append_offset = offset;
    }

    out_result.next_txn_id = expected_txn_id;

    // Clean up any leftover compaction shadow file `<main_path>.tmp`
    if (!main_path.empty()) {
        File::remove_file(main_path + ".tmp");
    }

    return Status::Ok();
}

Status verify_database_file(const std::string& path) {
    File file;
    Status st = file.open(path, OpenMode::READ_ONLY);
    if (!st.ok()) {
        return st;
    }
    auto sz_res = file.size();
    if (!sz_res.ok()) {
        return sz_res.status;
    }
    std::uint64_t file_size = sz_res.value;
    if (file_size < kFileHeaderSize) {
        return Status::Corrupt("File shorter than 12-byte header");
    }

    std::uint8_t hdr_bytes[kFileHeaderSize];
    st = file.read_exact_at(0, hdr_bytes, kFileHeaderSize);
    if (!st.ok()) {
        return st;
    }
    FileHeader fhdr{};
    st = decode_and_validate_file_header(hdr_bytes, fhdr);
    if (!st.ok()) {
        return st;
    }

    SequentialLogReader reader(file, file_size);
    std::uint64_t offset          = kFileHeaderSize;
    std::uint64_t expected_txn_id = 1;
    bool          in_batch        = false;
    std::uint64_t batch_txn_id    = 0;
    std::uint32_t batch_op_count  = 0;
    std::uint32_t batch_next_idx  = 0;
    std::uint32_t batch_crc_state = Crc32::kInitialState;
    std::string   key_scratch;

    while (offset < file_size) {
        if (file_size - offset < kRecordHeaderSize) {
            return Status::Corrupt("Torn record header at offset " + std::to_string(offset));
        }
        std::uint8_t raw_hdr[kRecordHeaderSize];
        st = reader.read_exact(offset, raw_hdr, kRecordHeaderSize);
        if (!st.ok()) {
            return st;
        }
        RecordHeader hdr{};
        st = decode_record_header(raw_hdr, hdr);
        if (!st.ok()) {
            return st;
        }

        std::uint64_t payload_len = static_cast<std::uint64_t>(hdr.key_len) +
                                    static_cast<std::uint64_t>(hdr.value_len);
        std::uint64_t total_rec_len = static_cast<std::uint64_t>(kRecordHeaderSize) + payload_len;
        if (file_size - offset < total_rec_len) {
            return Status::Corrupt("Torn record payload at offset " + std::to_string(offset));
        }

        std::uint32_t rec_crc_state = Crc32::update(Crc32::kInitialState, raw_hdr, 28);
        if (hdr.key_len > 0) {
            key_scratch.resize(hdr.key_len);
            st = reader.read_exact(offset + kRecordHeaderSize, &key_scratch[0], hdr.key_len);
            if (!st.ok()) {
                return st;
            }
            rec_crc_state = Crc32::update(rec_crc_state, key_scratch.data(), hdr.key_len);
        }
        std::uint64_t value_offset = offset + kRecordHeaderSize + hdr.key_len;
        std::uint8_t commit_payload[4]{};
        if (hdr.type == RecordType::COMMIT) {
            st = reader.read_exact(value_offset, commit_payload, 4);
            if (!st.ok()) {
                return st;
            }
            rec_crc_state = Crc32::update(rec_crc_state, commit_payload, 4);
        } else if (hdr.value_len > 0) {
            st = reader.update_crc_range(value_offset, hdr.value_len, rec_crc_state);
            if (!st.ok()) {
                return st;
            }
        }

        if (Crc32::finalize(rec_crc_state) != hdr.record_crc32) {
            return Status::Corrupt("Record CRC32 mismatch at offset " + std::to_string(offset));
        }

        const bool is_batch_flag = (hdr.flags & kFlagBatchMember) != 0;
        if (!is_batch_flag) {
            if (in_batch || hdr.txn_id != expected_txn_id) {
                return Status::Corrupt("Unexpected standalone txn_id at offset " + std::to_string(offset));
            }
            ++expected_txn_id;
            offset += total_rec_len;
        } else {
            if (hdr.type != RecordType::COMMIT) {
                if (!in_batch) {
                    if (hdr.txn_id != expected_txn_id || hdr.op_index != 0) {
                        return Status::Corrupt("Invalid batch start at offset " + std::to_string(offset));
                    }
                    in_batch        = true;
                    batch_txn_id    = hdr.txn_id;
                    batch_op_count  = hdr.op_count;
                    batch_next_idx  = 0;
                    batch_crc_state = Crc32::kInitialState;
                }
                if (hdr.txn_id != batch_txn_id ||
                    hdr.op_count != batch_op_count ||
                    hdr.op_index != batch_next_idx) {
                    return Status::Corrupt("Batch sequence mismatch at offset " + std::to_string(offset));
                }
                batch_crc_state = Crc32::update(batch_crc_state, raw_hdr, kRecordHeaderSize);
                if (hdr.key_len > 0) {
                    batch_crc_state = Crc32::update(batch_crc_state, key_scratch.data(), hdr.key_len);
                }
                if (hdr.value_len > 0) {
                    st = reader.update_crc_range(value_offset, hdr.value_len, batch_crc_state);
                    if (!st.ok()) {
                        return st;
                    }
                }
                ++batch_next_idx;
                offset += total_rec_len;
            } else {
                std::uint32_t expected_batch_crc = Crc32::finalize(batch_crc_state);
                std::uint32_t stored_batch_crc   = read_le32(commit_payload);
                if (!in_batch ||
                    hdr.txn_id != batch_txn_id ||
                    hdr.op_count != batch_op_count ||
                    batch_next_idx != batch_op_count ||
                    stored_batch_crc != expected_batch_crc) {
                    return Status::Corrupt("Invalid batch COMMIT marker at offset " + std::to_string(offset));
                }
                in_batch = false;
                ++expected_txn_id;
                offset += total_rec_len;
            }
        }
    }

    if (in_batch) {
        return Status::Corrupt("Uncommitted batch at EOF");
    }
    return Status::Ok();
}

} // namespace kvdb