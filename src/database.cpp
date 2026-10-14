#include "kiln/database.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <span>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace kiln {
// An unnamed namespace keeps these helpers private to this translation unit.
namespace {
// Disk layout is explicit bytes, not raw C++ structs (which may have padding).
// A record is a 24-byte header followed by key bytes and then value bytes.
constexpr std::size_t file_header_size = 16;
constexpr std::size_t record_header_size = 24;
constexpr std::uint32_t record_magic = 0x3152564b; // "KVR1", little endian
constexpr std::uint8_t put_operation = 1;
constexpr std::uint8_t erase_operation = 2;
using Header = std::array<char, record_header_size>;

// noreturn tells the compiler this helper always throws. Save errno before
// building the message so it describes the failed OS operation.
[[noreturn]] void io_error(std::string_view operation) {
    const int code = errno;
    throw Error(std::string(operation) + ": " + std::strerror(code));
}

// Encode least-significant byte first (little endian), independent of CPU layout.
void encode32(char* dst, std::uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) dst[i] = static_cast<char>((n >> (8 * i)) & 0xff);
}

// Rebuild the integer from four bytes. Unsigned char avoids sign extension
// when a byte has its high bit set.
std::uint32_t decode32(const char* src) {
    std::uint32_t n = 0;
    for (unsigned i = 0; i < 4; ++i)
        n |= static_cast<std::uint32_t>(static_cast<unsigned char>(src[i])) << (8 * i);
    return n;
}

// Advance IEEE CRC-32 over bytes. Carrying crc between calls lets us cover
// header, key, and value without joining them into another buffer.
// The bitwise implementation favors clarity; CRC is not a security hash.
std::uint32_t crc_update(std::uint32_t crc, std::string_view bytes) {
    for (unsigned char byte : bytes) {
        crc ^= byte;
        for (unsigned bit = 0; bit < 8; ++bit)
            // Unsigned subtraction makes a mask: all ones for a low bit of 1, else 0.
            // That selects whether to XOR the reflected CRC polynomial.
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return crc;
}

// Header is passed by value so zeroing checksum fields changes only a copy.
// Zero both fields to avoid making the checksum depend on itself.
std::uint32_t record_crc(Header header, std::string_view key, std::string_view value) {
    encode32(header.data() + 16, 0);
    encode32(header.data() + 20, 0);
    auto crc = crc_update(0xffffffffu, {header.data(), header.size()});
    crc = crc_update(crc, key);
    return crc_update(crc, value) ^ 0xffffffffu;
}

// span is a non-owning writable buffer view. pread reads at an explicit
// offset without changing the descriptor current file position.
// Short reads are legal; loop until the requested buffer is filled.
void read_exact(int fd, std::span<char> bytes, std::uint64_t offset) {
    while (!bytes.empty()) {
        const auto n = ::pread(fd, bytes.data(), bytes.size(), static_cast<off_t>(offset));
        if (n < 0) {
            if (errno == EINTR) continue;
            // EINTR was retried above: a signal interrupted the system call.
            // Other read failures are reported to the caller.
            io_error("pread");
        }
        if (n == 0) throw Error("unexpected end of file");
        offset += static_cast<std::uint64_t>(n);
        // Advance the buffer view past bytes already read; no buffer copy is needed.
        bytes = bytes.subspan(static_cast<std::size_t>(n));
    }
}

// pwrite uses an explicit offset. Short writes require advancing both the
// file offset and the input view, then retrying the remaining bytes.
// This helper completes writes but does not flush them for durability.
void write_exact(int fd, std::string_view bytes, std::uint64_t offset) {
    while (!bytes.empty()) {
        const auto n = ::pwrite(fd, bytes.data(), bytes.size(), static_cast<off_t>(offset));
        if (n < 0) {
            if (errno == EINTR) continue;
            io_error("pwrite");
        }
        // Avoid an infinite loop if the OS reports success but writes zero bytes.
        if (n == 0) throw Error("pwrite made no progress");
        offset += static_cast<std::uint64_t>(n);
        bytes.remove_prefix(static_cast<std::size_t>(n));
    }
}

// Retry an interrupted fsync; any other failure prevents a durable acknowledgment.
void sync_fd(int fd) {
    while (::fsync(fd) != 0) {
        if (errno == EINTR) continue;
        io_error("fsync");
    }
}

// Reject oversized caller inputs before writing or narrowing lengths to 32 bits.
void validate_lengths(std::string_view key, std::string_view value) {
    if (key.size() > Database::max_key_bytes) throw Error("key exceeds 64 KiB limit");
    if (value.size() > Database::max_value_bytes) throw Error("value exceeds 16 MiB limit");
}

// Decoded metadata for one record; this struct itself is never serialized.
struct Record {
    std::uint8_t operation;
    std::uint32_t key_size;
    std::uint32_t value_size;
};

// Validate magic, operation, reserved bytes, header CRC, and size bounds
// before using lengths to allocate a payload. Tombstones have no value.
Record parse_header(const Header& header) {
    const auto operation = static_cast<std::uint8_t>(header[4]);
    const auto key_size = decode32(header.data() + 8);
    const auto value_size = decode32(header.data() + 12);
    if (decode32(header.data()) != record_magic ||
        (operation != put_operation && operation != erase_operation) ||
        header[5] != 0 || header[6] != 0 || header[7] != 0 ||
        decode32(header.data() + 20) !=
            (crc_update(0xffffffffu, {header.data(), 20}) ^ 0xffffffffu) ||
        key_size > Database::max_key_bytes || value_size > Database::max_value_bytes ||
        (operation == erase_operation && value_size != 0))
        throw Error("invalid record header");
    return {operation, key_size, value_size};
}

// Payload is key followed by value. Check both plus metadata against the
// stored record CRC before trusting the record contents.
void verify_crc(const Header& header, std::string_view payload, const Record& record) {
    if (record_crc(header, payload.substr(0, record.key_size),
                   payload.substr(record.key_size)) != decode32(header.data() + 16))
        throw Error("record checksum mismatch");
}
} // namespace

// Acquiring resources can fail partway through construction. The catch block
// closes acquired descriptors because a failed constructor has no destructor call.
Database::Database(const std::filesystem::path& path) {
    try {
        auto parent = path.parent_path();
        if (parent.empty()) parent = ".";
        // O_DIRECTORY requires a directory. O_CLOEXEC closes this handle on exec.
        directory_fd_ = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory_fd_ < 0) io_error("open parent directory");
        // Open read/write, creating if absent. 0600 gives the owner read/write access
        // on creation; there is no O_TRUNC, so existing data is preserved.
        fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd_ < 0) io_error("open database");
        // Reject a competing file owner immediately. This advisory file lock
        // does not make calls on one instance safe across threads.
        if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) io_error("lock database");
        // Inspect the opened file rather than assuming the path names a regular file.
        struct stat info {};
        if (::fstat(fd_, &info) != 0) io_error("fstat");
        if (!S_ISREG(info.st_mode)) throw Error("database must be a regular file");
        end_ = static_cast<std::uint64_t>(info.st_size);
        // Empty file: write format identity/version/checksum and sync its creation.
        // Existing file: replay its records to rebuild the in-memory index.
        if (end_ == 0) {
            std::array<char, file_header_size> header {};
            std::memcpy(header.data(), "KILNKV01", 8);
            encode32(header.data() + 8, 1);
            encode32(header.data() + 12,
                     crc_update(0xffffffffu, {header.data(), 12}) ^ 0xffffffffu);
            write_exact(fd_, {header.data(), header.size()}, 0);
            end_ = file_header_size;
            sync();
        } else {
            recover();
        }
    } catch (...) {
        if (fd_ >= 0) ::close(fd_);
        if (directory_fd_ >= 0) ::close(directory_fd_);
        throw;
    }
}

Database::~Database() {
    // Destruction does not acknowledge durability; callers must use sync().
    if (fd_ >= 0) ::close(fd_);
    if (directory_fd_ >= 0) ::close(directory_fd_);
}

// A failed mutation can leave disk and memory uncertain. Reopen forces
// recovery before another operation can rely on them.
void Database::require_healthy() const {
    if (poisoned_) throw Error("database requires reopen after a failed mutation or sync");
}

// Validate the file identity, then replay records in append order.
// Later puts replace earlier index entries; tombstones remove them.
void Database::recover() {
    if (end_ < file_header_size) throw Error("incomplete database header");
    std::array<char, file_header_size> file_header {};
    read_exact(fd_, file_header, 0);
    if (std::memcmp(file_header.data(), "KILNKV01", 8) != 0 ||
        decode32(file_header.data() + 8) != 1 ||
        decode32(file_header.data() + 12) !=
            (crc_update(0xffffffffu, {file_header.data(), 12}) ^ 0xffffffffu))
        throw Error("invalid database header or unsupported version");

    // Skip the file header. Fewer than 24 trailing bytes cannot form a record header.
    std::uint64_t offset = file_header_size;
    while (end_ - offset >= record_header_size) {
        Header header {};
        read_exact(fd_, header, offset);
        const auto record = parse_header(header);
        const auto payload_size = static_cast<std::uint64_t>(record.key_size) + record.value_size;
        // A valid header with a missing payload is an interrupted final append.
        // Checksum errors on complete records throw instead of being repaired silently.
        if (payload_size > end_ - offset - record_header_size) break;
        std::string payload(static_cast<std::size_t>(payload_size), '\0');
        read_exact(fd_, {payload.data(), payload.size()}, offset + record_header_size);
        verify_crc(header, payload, record);
        // Own the key in the index; the temporary payload buffer is discarded each iteration.
        std::string key = payload.substr(0, record.key_size);
        if (record.operation == put_operation)
            index_.insert_or_assign(std::move(key), Entry {offset, record.key_size, record.value_size});
        else
            index_.erase(key);
        offset += record_header_size + payload_size;
    }
    // Remove an incomplete suffix so future appends begin at a clean boundary.
    // This assumes a valid prefix plus missing suffix, not arbitrary corruption.
    if (offset != end_) {
        if (::ftruncate(fd_, static_cast<off_t>(offset)) != 0) io_error("truncate incomplete tail");
        // Persist repair before admitting subsequent writes.
        sync_fd(fd_);
        end_ = offset;
    }
}

// One writer must own this entire sequence: offset choice, record write,
// index publication, and end advancement. There is no internal mutex yet.
void Database::append(std::uint8_t operation, std::string_view key, std::string_view value) {
    require_healthy();
    validate_lengths(key, value);
    const auto record_size = record_header_size + key.size() + value.size();
    // Check the file-offset limit before adding record_size, avoiding overflow.
    if (end_ > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) - record_size)
        throw Error("database exceeds supported file size");
    // Zero initialization sets reserved bytes. Encode fields at format offsets;
    // compute the record CRC first, then the independent header CRC.
    Header header {};
    encode32(header.data(), record_magic);
    header[4] = static_cast<char>(operation);
    encode32(header.data() + 8, static_cast<std::uint32_t>(key.size()));
    encode32(header.data() + 12, static_cast<std::uint32_t>(value.size()));
    encode32(header.data() + 16, record_crc(header, key, value));
    encode32(header.data() + 20,
             crc_update(0xffffffffu, {header.data(), 20}) ^ 0xffffffffu);
    try {
        // Write every byte before publishing the location in the index.
        // These writes may still reside in the OS cache until sync completes.
        write_exact(fd_, {header.data(), header.size()}, end_);
        write_exact(fd_, key, end_ + record_header_size);
        write_exact(fd_, value, end_ + record_header_size + key.size());
        // The record is complete. Publish its latest location, or remove the key.
        // Memory allocation can also fail here after bytes reached the log.
        if (operation == put_operation)
            index_.insert_or_assign(std::string(key), Entry {end_,
                static_cast<std::uint32_t>(key.size()), static_cast<std::uint32_t>(value.size())});
        else
            index_.erase(std::string(key));
        end_ += record_size;
    } catch (...) {
        // Do not continue after uncertain state. throw; preserves the original exception.
        poisoned_ = true;
        throw;
    }
}

// Ordinary put intentionally leaves the flush decision to its caller.
void Database::put(std::string_view key, std::string_view value) {
    append(put_operation, key, value);
}

// Convenience durability boundary: successful return requires both calls.
// sync also flushes earlier pending writes; failure does not roll back put.
void Database::put_durable(std::string_view key, std::string_view value) {
    put(key, value);
    sync();
}

// Deletion appends a tombstone; it does not remove old file bytes.
// Recovery needs the tombstone to avoid resurrecting an earlier put.
bool Database::erase(std::string_view key) {
    require_healthy();
    validate_lengths(key, {});
    if (!index_.contains(std::string(key))) return false;
    append(erase_operation, key, {});
    return true;
}

bool Database::erase_durable(std::string_view key) {
    bool erased = erase(key);
    sync();
    return erased;
}

// Locate the latest record in memory, read it from the file, and verify it.
// Return an owned string so the value outlives our temporary read buffer.
std::optional<std::string> Database::get(std::string_view key) const {
    require_healthy();
    validate_lengths(key, {});
    // The current hash map requires a temporary string for lookup.
    // Transparent hashing could later allow direct string_view lookup.
    const auto found = index_.find(std::string(key));
    if (found == index_.end()) return std::nullopt;
    const auto entry = found->second;
    Header header {};
    read_exact(fd_, header, entry.offset);
    const auto record = parse_header(header);
    // Cross-check disk metadata against the index before allocating the payload.
    if (record.operation != put_operation || record.key_size != entry.key_size ||
        record.value_size != entry.value_size) throw Error("record disagrees with index");
    std::string payload(static_cast<std::size_t>(entry.key_size) + entry.value_size, '\0');
    read_exact(fd_, {payload.data(), payload.size()}, entry.offset + record_header_size);
    verify_crc(header, payload, record);
    if (std::string_view(payload).substr(0, entry.key_size) != key)
        throw Error("record key disagrees with index");
    return payload.substr(entry.key_size);
}

// Flush file contents, then the parent directory entry. Successful return
// is our durability boundary within filesystem and device guarantees.
void Database::sync() {
    require_healthy();
    try {
        sync_fd(fd_);
        // A new file also needs its name persisted in the parent directory.
        sync_fd(directory_fd_);
    } catch (...) {
        poisoned_ = true;
        throw;
    }
}

// Live-key count comes from the index, not the number of historical records.
std::size_t Database::size() const {
    require_healthy();
    return index_.size();
}
} // namespace kiln
