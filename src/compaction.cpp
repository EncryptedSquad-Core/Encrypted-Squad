#include "kvdb/compaction.hpp"
#include "kvdb/engine.hpp"
#include "kvdb/record.hpp"
#include <algorithm>
#include <string>
#include <vector>

namespace kvdb {

Status compact_database(
    File& main_file,
    const std::string& main_path,
    std::uint64_t max_bytes,
    Index& index,
    std::uint64_t& out_append_offset,
    std::uint64_t& out_next_txn_id) {

    const std::string temp_path = main_path + ".tmp";

    // Pre-calculate exact compacted file size and check against max_bytes before writing
    std::uint64_t projected_size = kFileHeaderSize;
    std::vector<std::pair<std::string, IndexEntry>> sorted_entries;
    sorted_entries.reserve(index.size());

    for (const auto& kv : index.entries()) {
        sorted_entries.push_back(kv);
        projected_size += kRecordHeaderSize + kv.first.size() + kv.second.value_len;
    }

    if (projected_size > max_bytes) {
        return Status::DiskFull("Compacted database size would exceed max_bytes");
    }

    std::sort(sorted_entries.begin(), sorted_entries.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    File temp_file;
    Status st = temp_file.open(temp_path, OpenMode::CREATE_TRUNCATE);
    if (!st.ok()) {
        File::remove_file(temp_path);
        return st;
    }

    std::uint8_t file_hdr[kFileHeaderSize];
    encode_file_header(file_hdr);
    st = temp_file.write_all_at(0, file_hdr, kFileHeaderSize);
    if (!st.ok()) {
        temp_file.close();
        File::remove_file(temp_path);
        return st;
    }

    Index new_index;
    if (!sorted_entries.empty()) {
        new_index.reserve(sorted_entries.size());
    }

    std::uint64_t write_offset = kFileHeaderSize;
    std::uint64_t txn_id       = 1;
    std::string   val_buf;

    for (const auto& item : sorted_entries) {
        const std::string& key   = item.first;
        const IndexEntry&  entry = item.second;

        val_buf.resize(entry.value_len);
        if (entry.value_len > 0) {
            st = main_file.read_exact_at(entry.value_offset, &val_buf[0], entry.value_len);
            if (!st.ok()) {
                temp_file.close();
                File::remove_file(temp_path);
                return st;
            }
        }

        RecordHeader rhdr{};
        rhdr.magic     = kRecordMagic;
        rhdr.type      = RecordType::PUT;
        rhdr.flags     = kFlagNone;
        rhdr.txn_id    = txn_id;
        rhdr.op_index  = 0;
        rhdr.op_count  = 1;
        rhdr.key_len   = static_cast<std::uint32_t>(key.size());
        rhdr.value_len = entry.value_len;

        std::uint8_t raw_rhdr[kRecordHeaderSize];
        encode_record_header(
            rhdr,
            key.data(),
            entry.value_len > 0 ? val_buf.data() : nullptr,
            raw_rhdr);

        st = temp_file.write_all_at(write_offset, raw_rhdr, kRecordHeaderSize);
        if (!st.ok()) {
            temp_file.close();
            File::remove_file(temp_path);
            return st;
        }

        st = temp_file.write_all_at(write_offset + kRecordHeaderSize, key.data(), key.size());
        if (!st.ok()) {
            temp_file.close();
            File::remove_file(temp_path);
            return st;
        }

        std::uint64_t new_val_offset = write_offset + kRecordHeaderSize + key.size();
        if (entry.value_len > 0) {
            st = temp_file.write_all_at(new_val_offset, val_buf.data(), entry.value_len);
            if (!st.ok()) {
                temp_file.close();
                File::remove_file(temp_path);
                return st;
            }
        }

        new_index.put(key, new_val_offset, entry.value_len);
        write_offset = new_val_offset + entry.value_len;
        ++txn_id;
    }

    // Sync and close the shadow file before atomic replacement
    st = temp_file.sync();
    if (!st.ok()) {
        temp_file.close();
        File::remove_file(temp_path);
        return st;
    }
    st = temp_file.close();
    if (!st.ok()) {
        File::remove_file(temp_path);
        return st;
    }

    trigger_crash_hook_if_matched(CrashHookPoint::COMPACT_BEFORE_REPLACE);

    // Close old handle while exclusive lock is held so Windows allows atomic replacement
    main_file.close();

    Status rep_st = File::atomic_replace(temp_path, main_path);
    if (!rep_st.ok()) {
        // Replacement failed: reopen old authoritative main file and discard temp
        main_file.open(main_path, OpenMode::READ_WRITE_CREATE);
        File::remove_file(temp_path);
        return rep_st;
    }

    trigger_crash_hook_if_matched(CrashHookPoint::COMPACT_AFTER_REPLACE_BEFORE_CLEANUP);

    // Reopen the newly installed authoritative file
    Status open_st = main_file.open(main_path, OpenMode::READ_WRITE_CREATE);
    if (!open_st.ok()) {
        return open_st;
    }

    index.swap(new_index);
    out_append_offset = write_offset;
    out_next_txn_id   = txn_id;
    return Status::Ok();
}

} // namespace kvdb