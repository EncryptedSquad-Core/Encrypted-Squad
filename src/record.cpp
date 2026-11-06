#include "kvdb/record.hpp"
#include "kvdb/crc32.hpp"
#include <cstring>
#include <limits>

namespace kvdb {

void write_le16(std::uint8_t* dst, std::uint16_t v) noexcept {
    dst[0] = static_cast<std::uint8_t>(v & 0xFFu);
    dst[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
}

void write_le32(std::uint8_t* dst, std::uint32_t v) noexcept {
    dst[0] = static_cast<std::uint8_t>(v & 0xFFu);
    dst[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
    dst[2] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
    dst[3] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
}

void write_le64(std::uint8_t* dst, std::uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) {
        dst[i] = static_cast<std::uint8_t>((v >> (i * 8)) & 0xFFull);
    }
}

std::uint16_t read_le16(const std::uint8_t* src) noexcept {
    return static_cast<std::uint16_t>(src[0]) |
           (static_cast<std::uint16_t>(src[1]) << 8);
}

std::uint32_t read_le32(const std::uint8_t* src) noexcept {
    return static_cast<std::uint32_t>(src[0]) |
           (static_cast<std::uint32_t>(src[1]) << 8) |
           (static_cast<std::uint32_t>(src[2]) << 16) |
           (static_cast<std::uint32_t>(src[3]) << 24);
}

std::uint64_t read_le64(const std::uint8_t* src) noexcept {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= (static_cast<std::uint64_t>(src[i]) << (i * 8));
    }
    return v;
}

void encode_file_header(std::uint8_t out[kFileHeaderSize]) noexcept {
    // Bytes 0..3: literal ASCII 'K', 'V', 'D', 'B' (4B 56 44 42)
    out[0] = 0x4Bu;
    out[1] = 0x56u;
    out[2] = 0x44u;
    out[3] = 0x42u;
    // Bytes 4..5: version = 1 (LE)
    write_le16(out + 4, kFormatVersion);
    // Bytes 6..7: flags = 0 (LE)
    write_le16(out + 6, 0);
    // Bytes 8..11: IEEE CRC32 over bytes 0..7
    std::uint32_t crc = Crc32::compute(out, 8);
    write_le32(out + 8, crc);
}

Status decode_and_validate_file_header(const std::uint8_t in[kFileHeaderSize], FileHeader& out_hdr) {
    if (in[0] != 0x4Bu || in[1] != 0x56u || in[2] != 0x44u || in[3] != 0x42u) {
        return Status::Corrupt("Invalid file header magic (expected KVDB)");
    }
    out_hdr.version = read_le16(in + 4);
    out_hdr.flags   = read_le16(in + 6);
    out_hdr.crc32   = read_le32(in + 8);

    if (out_hdr.version != kFormatVersion) {
        return Status::Corrupt("Unsupported file header version");
    }
    if (out_hdr.flags != 0) {
        return Status::Corrupt("Non-zero file header flags in v1");
    }
    std::uint32_t expected_crc = Crc32::compute(in, 8);
    if (out_hdr.crc32 != expected_crc) {
        return Status::Corrupt("File header CRC32 mismatch");
    }
    return Status::Ok();
}

std::uint32_t encode_record_header(
    RecordHeader hdr,
    const void* key_data,
    const void* value_data,
    std::uint8_t out[kRecordHeaderSize]) noexcept {

    write_le16(out + 0, hdr.magic);
    out[2] = static_cast<std::uint8_t>(hdr.type);
    out[3] = hdr.flags;
    write_le64(out + 4, hdr.txn_id);
    write_le32(out + 12, hdr.op_index);
    write_le32(out + 16, hdr.op_count);
    write_le32(out + 20, hdr.key_len);
    write_le32(out + 24, hdr.value_len);

    std::uint32_t crc_state = Crc32::update(Crc32::kInitialState, out, 28);
    if (hdr.key_len > 0 && key_data != nullptr) {
        crc_state = Crc32::update(crc_state, key_data, hdr.key_len);
    }
    if (hdr.value_len > 0 && value_data != nullptr) {
        crc_state = Crc32::update(crc_state, value_data, hdr.value_len);
    }
    std::uint32_t crc = Crc32::finalize(crc_state);
    write_le32(out + 28, crc);
    return crc;
}

Status decode_record_header(const std::uint8_t in[kRecordHeaderSize], RecordHeader& out_hdr) {
    out_hdr.magic = read_le16(in + 0);
    if (out_hdr.magic != kRecordMagic) {
        return Status::Corrupt("Invalid record magic (expected 0xAA55)");
    }

    std::uint8_t raw_type = in[2];
    if (raw_type != static_cast<std::uint8_t>(RecordType::PUT) &&
        raw_type != static_cast<std::uint8_t>(RecordType::DEL) &&
        raw_type != static_cast<std::uint8_t>(RecordType::COMMIT)) {
        return Status::Corrupt("Unknown record_type byte");
    }
    out_hdr.type = static_cast<RecordType>(raw_type);

    out_hdr.flags = in[3];
    if ((out_hdr.flags & ~kFlagBatchMember) != 0) {
        return Status::Corrupt("Reserved bits set in record flags");
    }

    out_hdr.txn_id       = read_le64(in + 4);
    out_hdr.op_index     = read_le32(in + 12);
    out_hdr.op_count     = read_le32(in + 16);
    out_hdr.key_len      = read_le32(in + 20);
    out_hdr.value_len    = read_le32(in + 24);
    out_hdr.record_crc32 = read_le32(in + 28);

    if (out_hdr.txn_id == 0) {
        return Status::Corrupt("Record txn_id cannot be zero");
    }
    if (out_hdr.op_count < 1 || out_hdr.op_count > kMaxBatchOps) {
        return Status::Corrupt("Record op_count out of bounds [1..10000]");
    }

    const bool is_batch = (out_hdr.flags & kFlagBatchMember) != 0;

    if (out_hdr.type == RecordType::COMMIT) {
        if (!is_batch) {
            return Status::Corrupt("COMMIT record missing BATCH_MEMBER flag");
        }
        if (out_hdr.op_index != out_hdr.op_count) {
            return Status::Corrupt("COMMIT record op_index must equal op_count");
        }
        if (out_hdr.key_len != 0 || out_hdr.value_len != 4) {
            return Status::Corrupt("COMMIT record must have key_len=0 and value_len=4");
        }
    } else {
        if (!is_batch) {
            if (out_hdr.op_count != 1 || out_hdr.op_index != 0) {
                return Status::Corrupt("Standalone record must have op_count=1 and op_index=0");
            }
        } else {
            if (out_hdr.op_index >= out_hdr.op_count) {
                return Status::Corrupt("Batch member op_index out of range");
            }
        }
        if (out_hdr.key_len < kMinKeyBytes || out_hdr.key_len > kMaxKeyBytes) {
            return Status::Corrupt("Record key_len out of bounds [1..1024]");
        }
        if (out_hdr.type == RecordType::PUT) {
            if (out_hdr.value_len > kMaxValueBytes) {
                return Status::Corrupt("PUT value_len exceeds 1,000,000 bytes");
            }
        } else if (out_hdr.type == RecordType::DEL) {
            if (out_hdr.value_len != 0) {
                return Status::Corrupt("DELETE record must have value_len=0");
            }
        }
    }

    return Status::Ok();
}

Status validate_key_size(std::size_t key_size) {
    if (key_size < kMinKeyBytes) {
        return Status::InvalidArgument("Key must be at least 1 byte");
    }
    if (key_size > kMaxKeyBytes) {
        return Status::TooLarge("Key exceeds maximum length of 1,024 bytes");
    }
    return Status::Ok();
}

Status validate_value_size(std::size_t value_size) {
    if (value_size > kMaxValueBytes) {
        return Status::TooLarge("Value exceeds maximum length of 1,000,000 bytes");
    }
    return Status::Ok();
}

Result<std::uint64_t> calculate_record_size(std::size_t key_len, std::size_t value_len) {
    std::uint64_t k = static_cast<std::uint64_t>(key_len);
    std::uint64_t v = static_cast<std::uint64_t>(value_len);
    if (k > kMaxKeyBytes || v > kMaxValueBytes) {
        return Status::TooLarge("Key or value exceeds configured limits");
    }
    return static_cast<std::uint64_t>(kRecordHeaderSize) + k + v;
}

void serialize_record(
    RecordType type,
    std::uint8_t flags,
    std::uint64_t txn_id,
    std::uint32_t op_index,
    std::uint32_t op_count,
    const std::string& key,
    const std::string& value,
    std::vector<std::uint8_t>& out_buf) {

    const std::size_t total_size = kRecordHeaderSize + key.size() + value.size();
    out_buf.resize(total_size);

    RecordHeader hdr{};
    hdr.magic     = kRecordMagic;
    hdr.type      = type;
    hdr.flags     = flags;
    hdr.txn_id    = txn_id;
    hdr.op_index  = op_index;
    hdr.op_count  = op_count;
    hdr.key_len   = static_cast<std::uint32_t>(key.size());
    hdr.value_len = static_cast<std::uint32_t>(value.size());

    encode_record_header(
        hdr,
        key.empty() ? nullptr : key.data(),
        value.empty() ? nullptr : value.data(),
        out_buf.data());

    if (!key.empty()) {
        std::memcpy(out_buf.data() + kRecordHeaderSize, key.data(), key.size());
    }
    if (!value.empty()) {
        std::memcpy(out_buf.data() + kRecordHeaderSize + key.size(), value.data(), value.size());
    }
}

void serialize_commit_marker(
    std::uint64_t txn_id,
    std::uint32_t op_count,
    std::uint32_t batch_crc32,
    std::uint8_t out_buf[kRecordHeaderSize + 4]) noexcept {

    std::uint8_t payload[4];
    write_le32(payload, batch_crc32);

    RecordHeader hdr{};
    hdr.magic     = kRecordMagic;
    hdr.type      = RecordType::COMMIT;
    hdr.flags     = kFlagBatchMember;
    hdr.txn_id    = txn_id;
    hdr.op_index  = op_count;
    hdr.op_count  = op_count;
    hdr.key_len   = 0;
    hdr.value_len = 4;

    encode_record_header(hdr, nullptr, payload, out_buf);
    std::memcpy(out_buf + kRecordHeaderSize, payload, 4);
}

} // namespace kvdb