#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace kiln {

// Storage and validation failures use one exception type that callers can catch.
class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A single-owner, append-only embedded store. Calls are not thread-safe.
// put/erase acknowledge visibility; sync acknowledges persistence.
class Database {
public:
    // Bounds keep record sizes representable and limit allocation per record.
    static constexpr std::uint32_t max_key_bytes = 64 * 1024;
    static constexpr std::uint32_t max_value_bytes = 16 * 1024 * 1024;

    // Open/create the file, take its exclusive lock, and reconstruct the index.
    // explicit prevents an accidental conversion from a path into a database.
    explicit Database(const std::filesystem::path& path);
    ~Database();
    // The instance owns OS file descriptors. Copies could close the same handles
    // twice; moving is also disabled until ownership transfer is designed.
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&&) = delete;
    Database& operator=(Database&&) = delete;

    // Input views borrow caller bytes for this call; stored index keys are copied.
    // Ordinary put permits batching: it updates visibility without a sync.
    void put(std::string_view key, std::string_view value);
    // Append and sync before returning. Also flushes earlier pending mutations.
    // Failure does not guarantee rollback; reopen after append/sync failure.
    void put_durable(std::string_view key, std::string_view value);
    // Append a tombstone for an existing key; false means the key was absent.
    bool erase(std::string_view key);

    bool erase_durable(std::string_view key);
    // Return an owned value, or nullopt for absence. An empty value still exists.
    // const means this call does not modify the Database object.
    std::optional<std::string> get(std::string_view key) const;
    // Flush all pending mutations, subject to filesystem/device guarantees.
    void sync();
    // Count live keys; obsolete disk records are not included.
    std::size_t size() const;

private:
    // Index metadata locates a full record; value bytes remain in the file.
    struct Entry {
        std::uint64_t offset;
        std::uint32_t key_size;
        std::uint32_t value_size;
    };
    // File descriptors are OS handles. -1 means no handle has been acquired.
    int fd_ = -1;
    // Keep the parent open so sync can also persist the file directory entry.
    int directory_fd_ = -1;
    // After uncertain append/sync failure, reject operations until reopen.
    bool poisoned_ = false;
    // Byte offset where the next append starts; maintained by the single owner.
    std::uint64_t end_ = 0;
    // Only latest live records are indexed. Hash lookup avoids a file scan,
    // but owning every live key makes memory usage grow with key count.
    std::unordered_map<std::string, Entry> index_;

    void recover();
    void append(std::uint8_t operation, std::string_view key, std::string_view value);
    void require_healthy() const;
};

} // namespace kiln
