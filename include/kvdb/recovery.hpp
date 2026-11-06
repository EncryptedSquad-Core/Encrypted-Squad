#ifndef KVDB_RECOVERY_HPP
#define KVDB_RECOVERY_HPP

#include "kvdb/index.hpp"
#include "kvdb/platform.hpp"
#include "kvdb/record.hpp"
#include "kvdb/status.hpp"
#include <cstdint>
#include <string>

namespace kvdb {

struct RecoveryResult {
    std::uint64_t append_offset       = kFileHeaderSize;
    std::uint64_t next_txn_id         = 1;
    std::uint64_t committed_records   = 0;
    std::uint64_t truncated_bytes     = 0;
    bool          repaired_tail       = false;
    bool          initialized_header  = false;
};

struct PendingBatchOp {
    RecordType    type         = RecordType::PUT;
    std::string   key;
    std::uint64_t value_offset = 0;
    std::uint32_t value_len    = 0;
};

// Runs full crash recovery on `file` during Database::open():
// 1. Initializes or validates the 12-byte file header.
// 2. Uses a 4 MiB sequential read buffer to scan records and verify CRCs.
// 3. Buffers batch members until a valid COMMIT marker with matching batch_crc32 is found.
// 4. Truncates any torn tail or uncommitted batch back to the start of that transaction and syncs.
// 5. Cleans up any stale `<main_path>.tmp` left behind by an interrupted compaction.
Status recover_database(
    File& file,
    const std::string& main_path,
    Index& out_index,
    RecoveryResult& out_result);

// Read-only integrity verification used by `./kvdb check <path>`.
// Returns Status::Ok() if the file is completely valid with no torn tail or mid-file corruption,
// or Status::Corrupt with an exact diagnostic reason.
Status verify_database_file(const std::string& path);

} // namespace kvdb

#endif // KVDB_RECOVERY_HPP