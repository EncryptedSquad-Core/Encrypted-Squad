#ifndef KVDB_INDEX_HPP
#define KVDB_INDEX_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace kvdb {

// Value-offset index entry: stores where the value lives on disk so RAM
// stays small and recovery doesn't allocate 1 MB strings on the heap.
struct IndexEntry {
    std::uint64_t value_offset = 0; // Absolute byte offset of value bytes in db.data
    std::uint32_t value_len    = 0; // 0 is a valid empty value
};

class Index {
public:
    using MapType = std::unordered_map<std::string, IndexEntry>;

    Index() {
        map_.max_load_factor(0.75f);
    }

    void reserve(std::size_t expected_keys) {
        map_.reserve(expected_keys);
    }

    void put(const std::string& key, std::uint64_t value_offset, std::uint32_t value_len) {
        map_[key] = IndexEntry{value_offset, value_len};
    }

    void put(std::string&& key, std::uint64_t value_offset, std::uint32_t value_len) {
        map_[std::move(key)] = IndexEntry{value_offset, value_len};
    }

    bool erase(const std::string& key) {
        return map_.erase(key) > 0;
    }

    bool get(const std::string& key, IndexEntry& out_entry) const {
        auto it = map_.find(key);
        if (it == map_.end()) {
            return false;
        }
        out_entry = it->second;
        return true;
    }

    bool contains(const std::string& key) const {
        return map_.find(key) != map_.end();
    }

    std::size_t size() const noexcept {
        return map_.size();
    }

    void clear() noexcept {
        map_.clear();
    }

    void swap(Index& other) noexcept {
        map_.swap(other.map_);
    }

    const MapType& entries() const noexcept {
        return map_;
    }

private:
    MapType map_;
};

} // namespace kvdb

#endif // KVDB_INDEX_HPP