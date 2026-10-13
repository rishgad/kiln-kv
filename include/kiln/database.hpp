#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace kiln {

class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A single-owner, append-only embedded store. Calls are not thread-safe.
// put/erase acknowledge visibility; sync acknowledges persistence.
class Database {
public:
    static constexpr std::uint32_t max_key_bytes = 64 * 1024;
    static constexpr std::uint32_t max_value_bytes = 16 * 1024 * 1024;

    explicit Database(const std::filesystem::path& path);
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&&) = delete;
    Database& operator=(Database&&) = delete;

    void put(std::string_view key, std::string_view value);
    bool erase(std::string_view key);
    std::optional<std::string> get(std::string_view key) const;
    void sync();
    std::size_t size() const;

private:
    struct Entry {
        std::uint64_t offset;
        std::uint32_t key_size;
        std::uint32_t value_size;
    };
    int fd_ = -1;
    int directory_fd_ = -1;
    bool poisoned_ = false;
    std::uint64_t end_ = 0;
    std::unordered_map<std::string, Entry> index_;

    void recover();
    void append(std::uint8_t operation, std::string_view key, std::string_view value);
    void require_healthy() const;
};

} // namespace kiln
