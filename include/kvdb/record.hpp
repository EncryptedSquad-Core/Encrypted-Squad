#ifndef KVDB_RECORD_HPP
#define KVDB_RECORD_HPP

#include "kvdb/status.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace kvdb {

// Format constants matching ARCHITECTURE.md Section 4
constexpr std::size_t   kFileHeaderSize     = 12;
constexpr std::size_t   kRecordHeaderSize   = 32;
constexpr std::uint16_t kFormatVersion      = 1;
constexpr std::uint16_t kRecordMagic        = 0xAA55u;

// Strict data limits from Problem Statement Section 2
constexpr std::uint32_t kMinKeyBytes        = 1;
constexpr std::uint32_t kMaxKeyBytes        = 1024;
constexpr std::uint32_t kMaxValueBytes      = 1000000; // 1 MB decimal (1,000,000 bytes)
constexpr std::uint32_t kMaxBatchOps        = 10000;

enum class RecordType : std::uint8_t {
    PUT    = 0x01,
    DEL    = 0x02,
    COMMIT = 0x03
};

constexpr std::uint8_t kFlagNone        = 0x00;
constexpr std::uint8_t kFlagBatchMember = 0x01; // Bit 0 = BATCH_MEMBER

// 12-byte File Header (never written via raw struct cast)
struct FileHeader {
    std::uint16_t version = kFormatVersion;
    std::uint16_t flags   = 0;
    std::uint32_t crc32   = 0;
};

// 32-byte Record Header (never written via raw struct cast)
struct RecordHeader {
    std::uint16_t magic        = kRecordMagic;
    RecordType    type         = RecordType::PUT;
    std::uint8_t  flags        = kFlagNone;
    std::uint64_t txn_id       = 0;
    std::uint32_t op_index     = 0;
    std::uint32_t op_count     = 1;
    std::uint32_t key_len      = 0;
    std::uint32_t value_len    = 0;
    std::uint32_t record_crc32 = 0;
};

//Explicit Little-Endian helpers
void write_le16(std::uint8_t* dst, std::uint16_t v) noexcept;
void write_le32(std::uint8_t* dst, std::uint32_t v) noexcept;
void write_le64(std::uint8_t* dst, std::uint64_t v) noexcept;

std::uint16_t read_le16(const std::uint8_t* src) noexcept;
std::uint32_t read_le32(const std::uint8_t* src) noexcept;
std::uint64_t read_le64(const std::uint8_t* src) noexcept;

// File Header encode/decode
void encode_file_header(std::uint8_t out[kFileHeaderSize]) noexcept;
Status decode_and_validate_file_header(const std::uint8_t in[kFileHeaderSize], FileHeader& out_hdr);

// Record Header encode/decode
// Computes record_crc32 over header bytes [0..27] + key + value, writes all 32 header bytes to `out`
std::uint32_t encode_record_header(
    RecordHeader hdr,
    const void* key_data,
    const void* value_data,
    std::uint8_t out[kRecordHeaderSize]) noexcept;

// Decodes 32-byte header and validates magic, type, flags, and length bounds
Status decode_record_header(const std::uint8_t in[kRecordHeaderSize], RecordHeader& out_hdr);

// Validates key and value sizes against hackathon limits
Status validate_key_size(std::size_t key_size);
Status validate_value_size(std::size_t value_size);

// Checked 64-bit record size calculation: 32 + key_len + value_len
Result<std::uint64_t> calculate_record_size(std::size_t key_len, std::size_t value_len);

// Serializes a complete record (32-byte header + key + value) into `out_buf`
void serialize_record(
    RecordType type,
    std::uint8_t flags,
    std::uint64_t txn_id,
    std::uint32_t op_index,
    std::uint32_t op_count,
    const std::string& key,
    const std::string& value,
    std::vector<std::uint8_t>& out_buf);

// Serializes a 36-byte Batch COMMIT marker (32-byte header + 4-byte batch_crc32 payload)
void serialize_commit_marker(
    std::uint64_t txn_id,
    std::uint32_t op_count,
    std::uint32_t batch_crc32,
    std::uint8_t out_buf[kRecordHeaderSize + 4]) noexcept;

} // namespace kvdb

#endif // KVDB_RECORD_HPP