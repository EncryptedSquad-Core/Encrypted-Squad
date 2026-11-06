#ifndef KVDB_CRC32_HPP
#define KVDB_CRC32_HPP

#include <array>
#include <cstddef>
#include <cstdint>

namespace kvdb {

// Team-written, table-based IEEE 802.3 CRC32 implementation
// Reflected polynomial: 0xEDB88320, initial value: 0xFFFFFFFF, final XOR: 0xFFFFFFFF
class Crc32 {
public:
    static constexpr std::uint32_t kInitialState = 0xFFFFFFFFu;

    static std::uint32_t update(std::uint32_t crc_state, const void* data, std::size_t length) noexcept {
        const auto& table = lookup_table();
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        std::uint32_t c = crc_state;
        for (std::size_t i = 0; i < length; ++i) {
            c = table[(c ^ bytes[i]) & 0xFFu] ^ (c >> 8);
        }
        return c;
    }

    static constexpr std::uint32_t finalize(std::uint32_t crc_state) noexcept {
        return crc_state ^ 0xFFFFFFFFu;
    }

    static std::uint32_t compute(const void* data, std::size_t length) noexcept {
        return finalize(update(kInitialState, data, length));
    }

private:
    static const std::array<std::uint32_t, 256>& lookup_table() noexcept {
        static const std::array<std::uint32_t, 256> table = generate_table();
        return table;
    }

    static constexpr std::array<std::uint32_t, 256> generate_table() noexcept {
        std::array<std::uint32_t, 256> table{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int bit = 0; bit < 8; ++bit) {
                if (c & 1u) {
                    c = 0xEDB88320u ^ (c >> 1);
                } else {
                    c >>= 1;
                }
            }
            table[i] = c;
        }
        return table;
    }
};

} // namespace kvdb

#endif // KVDB_CRC32_HPP