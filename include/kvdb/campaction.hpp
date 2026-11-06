#ifndef KVDB_COMPACTION_HPP
#define KVDB_COMPACTION_HPP

#include "kvdb/index.hpp"
#include "kvdb/platform.hpp"
#include "kvdb/status.hpp"
#include <cstdint>
#include <string>

namespace kvdb {

// Crash-safe compaction (called while Database holds unique_lock):
// 1. Writes a fresh 12-byte header and all live keys from `index` into `<main_path>.tmp`.
// 2. Enforces `max_bytes` on the shadow file before replacement.
// 3. Calls sync() and close() on `<main_path>.tmp`.
// 4. Closes `main_file`, performs `File::atomic_replace(temp_path, main_path)`, and reopens `main_file`.
// 5. Installs the newly built index, `out_append_offset`, and `out_next_txn_id`.
// If anything fails before replacement, removes `<main_path>.tmp` and leaves the old database intact.
Status compact_database(
    File& main_file,
    const std::string& main_path,
    std::uint64_t max_bytes,
    Index& index,
    std::uint64_t& out_append_offset,
    std::uint64_t& out_next_txn_id);

} // namespace kvdb

#endif // KVDB_COMPACTION_HPP