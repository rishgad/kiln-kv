#include "kiln/database.hpp"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <random>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>

namespace fs = std::filesystem;
namespace {
// Small assertion helper: a failed condition aborts the current test group.
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// Accept a callable/lambda and verify it throws the engine error type.
template<class F> void rejects(F operation) {
    bool threw = false;
    try { operation(); } catch (const kiln::Error&) { threw = true; }
    check(threw, "expected kiln::Error");
}

// Each test owns a unique temporary directory. Cleanup runs on scope exit,
// including exceptions; no test uses the real project database.
struct Temp {
    fs::path root;
    Temp() {
        std::string pattern = (fs::temp_directory_path() / "kiln-tests-XXXXXX").string();
        auto* directory = ::mkdtemp(pattern.data());
        if (!directory) throw std::runtime_error("mkdtemp failed");
        root = directory;
    }
    ~Temp() { std::error_code ignored; fs::remove_all(root, ignored); }
    fs::path file(std::string_view name = "store.kv") const { return root / name; }
};

// Change one bit of an existing file byte to test corruption detection.
void flip_byte(const fs::path& path, std::uint64_t offset) {
    std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
    file.seekg(static_cast<std::streamoff>(offset));
    char byte = 0;
    file.read(&byte, 1);
    check(file.good(), "cannot read byte to corrupt");
    byte ^= 1;
    file.seekp(static_cast<std::streamoff>(offset));
    file.write(&byte, 1);
    file.flush();
    check(file.good(), "cannot write corrupted byte");
}

// Exercise the byte-oriented API. Explicit string lengths preserve embedded NULs.
// Separate scopes close and reopen the database to check recovery.
void basic_and_binary() {
    Temp temp;
    const std::string binary_key("a\0b", 3), binary_value("x\0y\xff", 4);
    {
        kiln::Database db(temp.file());
        check(!db.get("missing"), "missing key");
        check(!db.erase("missing"), "delete missing key");
        db.put("a", "first");
        db.put("a", "second");
        db.put(binary_key, binary_value);
        db.put("", "");
        check(db.get("a") == "second", "overwrite");
        check(db.size() == 3, "size after overwrite");
        check(db.erase("a"), "delete existing");
        db.sync();
    }
    {
        kiln::Database db(temp.file());
        check(!db.get("a"), "tombstone survives reopen");
        check(db.get(binary_key) == binary_value, "binary bytes survive reopen");
        check(db.get("") == "", "empty key and value survive reopen");
        db.put("a", "reborn");
        db.sync();
    }
    kiln::Database db(temp.file());
    check(db.get("a") == "reborn", "reinsert survives reopen");
}

// Check immediate visibility, overwrite recovery, and input rejection.
// Reopen alone is not proof of a physical flush; the contract also relies on fsync.
void durable_put_and_recovery() {
    Temp temp;
    {
        kiln::Database db(temp.file());
        db.put("earlier", "pending");
        db.put_durable("key", "first");
        check(db.get("key") == "first", "durable put visible on return");
        db.put_durable("key", "replacement");
        rejects([&] {
            db.put_durable(std::string(kiln::Database::max_key_bytes + 1, 'k'), "bad");
        });
        check(db.get("key") == "replacement", "invalid durable put preserves state");
    }
    kiln::Database reopened(temp.file());
    check(reopened.get("key") == "replacement", "durable overwrite survives reopen");
    check(reopened.get("earlier") == "pending", "earlier mutation included in flush");
    check(reopened.size() == 2, "invalid durable put does not add a key");
}

// Check the public deletion contract and recovery without another explicit sync.
// Keep an unrelated key so an accidental broad deletion is detected too.
void durable_erase_and_recovery() {
    Temp temp;
    {
        kiln::Database db(temp.file());
        db.put_durable("target", "remove me");
        db.put_durable("keep", "preserve me");

        check(db.erase_durable("target"), "durable erase returns true for existing key");
        check(!db.get("target"), "durable erase immediately removes key");
        check(db.size() == 1, "durable erase updates live-key count");
        check(!db.erase_durable("target"), "repeated durable erase returns false");
        check(!db.erase_durable("never-present"), "missing durable erase returns false");
        check(db.get("keep") == "preserve me", "durable erase preserves unrelated key");

        // Invalid input must throw, leave valid data intact, and keep the instance usable.
        rejects([&] {
            db.erase_durable(std::string(kiln::Database::max_key_bytes + 1, 'k'));
        });
        check(db.get("keep") == "preserve me", "invalid durable erase preserves state");
    }

    kiln::Database reopened(temp.file());
    check(!reopened.get("target"), "durable deletion survives reopen");
    check(reopened.get("keep") == "preserve me", "unrelated value survives reopen");
    check(reopened.size() == 1, "recovered live-key count reflects deletion");

    // A tombstone must not prevent the same key from being inserted again.
    reopened.put_durable("target", "new value");
    check(reopened.get("target") == "new value", "reinsert after durable erase works");
}

// The missing-key path can follow an earlier unsynced deletion.
// Reopen checks recovered state, not whether fsync was actually called: kernel
// caches can preserve unsynced data. Sync ordering also needs code review;
// a deterministic sync-failure test would need an I/O fault-injection mechanism.
void durable_erase_after_pending_delete() {
    Temp temp;
    {
        kiln::Database db(temp.file());
        db.put_durable("target", "old value");
        check(db.erase("target"), "ordinary erase creates pending tombstone");
        db.put("earlier", "pending value");

        check(!db.erase_durable("target"), "already-deleted key returns false");
        check(!db.get("target"), "already-deleted key remains absent");
    }

    kiln::Database reopened(temp.file());
    check(!reopened.get("target"), "pending tombstone remains applied after reopen");
    check(reopened.get("earlier") == "pending value", "earlier mutation recovers too");
}

// A simple map is the reference model. A fixed seed makes failures reproducible.
// Periodically discard the engine instance and compare all recovered live keys.
void deterministic_model() {
    Temp temp;
    std::mt19937 random(0xC0FFEE);
    std::unordered_map<std::string, std::string> model;
    auto db = std::make_unique<kiln::Database>(temp.file());
    for (int step = 0; step < 3000; ++step) {
        const auto key = "key-" + std::to_string(random() % 80);
        switch (random() % 3) {
        case 0: {
            std::string value(random() % 96, '\0');
            for (char& byte : value) byte = static_cast<char>(random() & 0xff);
            db->put(key, value);
            model[key] = value;
            break;
        }
        case 1:
            check(db->erase(key) == (model.erase(key) != 0), "model erase mismatch");
            break;
        default: {
            const auto found = model.find(key);
            const auto result = db->get(key);
            check(found == model.end() ? !result : result == found->second, "model get mismatch");
        }
        }
        if (step % 100 == 0) {
            db->sync();
            db.reset();
            db = std::make_unique<kiln::Database>(temp.file());
            check(db->size() == model.size(), "model recovery size mismatch");
            for (const auto& [stored_key, value] : model)
                check(db->get(stored_key) == value, "model recovery value mismatch");
        }
    }
}

// Model an interrupted append by cutting its suffix at every byte boundary.
// Verify the old value remains and the repaired file accepts new writes.
void every_tail_truncation() {
    Temp temp;
    const auto source = temp.file("source.kv");
    std::uint64_t prefix = 0;
    {
        kiln::Database db(source);
        db.put("stable", "before");
        db.sync();
        prefix = fs::file_size(source);
        db.put("stable", "replacement");
        db.sync();
    }
    const auto full_size = fs::file_size(source);
    // Every missing-byte boundary, including the whole final record.
    for (auto size = prefix; size < full_size; ++size) {
        const auto cut = temp.file("cut.kv");
        fs::copy_file(source, cut, fs::copy_options::overwrite_existing);
        fs::resize_file(cut, size);
        {
            kiln::Database db(cut);
            check(db.get("stable") == "before", "partial put must not become visible");
            check(fs::file_size(cut) == prefix, "tail must be repaired");
            db.put("after", "repair");
            db.sync();
        }
        kiln::Database reopened(cut);
        check(reopened.get("after") == "repair", "append after tail repair");
    }
    // Interrupted tombstones must preserve the prior value too.
    const auto deleted = temp.file("deleted.kv");
    fs::copy_file(source, deleted);
    {
        kiln::Database db(deleted);
        db.erase("stable");
        db.sync();
    }
    const auto deleted_size = fs::file_size(deleted);
    for (auto size = full_size; size < deleted_size; ++size) {
        const auto cut = temp.file("cut.kv");
        fs::copy_file(deleted, cut, fs::copy_options::overwrite_existing);
        fs::resize_file(cut, size);
        kiln::Database db(cut);
        check(db.get("stable") == "replacement", "partial tombstone must not delete");
    }
}

// Complete damaged records must fail, rather than be mistaken for missing tails.
void corruption_is_reported() {
    Temp temp;
    const auto source = temp.file("source.kv");
    {
        kiln::Database db(source);
        db.put("key", "payload");
        db.sync();
    }
    // Every byte of the file header, record header, key, and payload.
    for (std::uint64_t offset = 0; offset < fs::file_size(source); ++offset) {
        const auto damaged = temp.file("damaged.kv");
        fs::copy_file(source, damaged, fs::copy_options::overwrite_existing);
        flip_byte(damaged, offset);
        const auto original_size = fs::file_size(damaged);
        rejects([&] { kiln::Database db(damaged); });
        check(fs::file_size(damaged) == original_size, "corruption must not silently truncate");
    }
    const auto damaged = temp.file("live.kv");
    fs::copy_file(source, damaged);
    kiln::Database db(damaged);
    flip_byte(damaged, fs::file_size(damaged) - 1);
    rejects([&] { static_cast<void>(db.get("key")); });
}

// Rejected inputs must not mutate or poison the store; a second owner is rejected.
void limits_and_ownership() {
    Temp temp;
    kiln::Database db(temp.file());
    rejects([&] { kiln::Database second(temp.file()); });
    const auto size_before = fs::file_size(temp.file());
    rejects([&] { db.put(std::string(kiln::Database::max_key_bytes + 1, 'k'), "v"); });
    rejects([&] { db.put("k", std::string(kiln::Database::max_value_bytes + 1, 'v')); });
    check(fs::file_size(temp.file()) == size_before, "invalid input must not append");
    const std::string large_key(kiln::Database::max_key_bytes, 'k');
    db.put(large_key, "valid");
    check(db.get(large_key) == "valid", "maximum key supported");
    db.put("still-healthy", "yes");
    check(db.get("still-healthy") == "yes", "input rejection must not poison");
    rejects([&] { kiln::Database bad(temp.root / "missing" / "store.kv"); });
}

// Wait for a child and collect its exit/signal status, retrying signal interruptions.
int reap(pid_t child) {
    int status = 0;
    while (::waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) continue;
        throw std::runtime_error("waitpid failed");
    }
    return status;
}

// Use a subprocess so a file-size resource limit cannot affect other tests.
// Allow a full record header but only part of its key, causing a real write failure.
void failed_append_requires_recovery() {
    Temp temp;
    std::uint64_t prefix = 0;
    {
        kiln::Database db(temp.file());
        db.put("stable", "yes");
        db.sync();
        prefix = fs::file_size(temp.file());
    }
    const pid_t child = ::fork();
    check(child >= 0, "fork failed");
    if (child == 0) {
        try {
            kiln::Database db(temp.file());
            // Ignoring this signal lets pwrite return an error that the engine can handle.
            ::signal(SIGXFSZ, SIG_IGN);
            struct rlimit limit {};
            if (::getrlimit(RLIMIT_FSIZE, &limit) != 0) ::_exit(2);
            // 24 is the on-disk record header length; two extra bytes force a partial key.
            limit.rlim_cur = static_cast<rlim_t>(prefix + 24 + 2);
            if (::setrlimit(RLIMIT_FSIZE, &limit) != 0) ::_exit(3);
            rejects([&] { db.put_durable("broken", "unfinished"); });
            rejects([&] { db.put("later", "must not append"); });
            rejects([&] { db.sync(); });
            // Exit without C++ destructor cleanup, leaving the failed append for recovery.
            ::_exit(0);
        } catch (...) { ::_exit(4); }
    }
    const int status = reap(child);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "failed append child failed");
    kiln::Database db(temp.file());
    check(fs::file_size(temp.file()) == prefix, "failed append repaired");
    check(db.get("stable") == "yes" && !db.get("broken"), "stable prefix preserved");
    db.put("later", "works after reopen");
    db.sync();
}

// A pipe tells the parent that put_durable returned before it sends SIGKILL.
// Killing the child skips destructor cleanup but does not simulate power loss.
void synced_data_survives_process_kill() {
    Temp temp;
    int ready[2];
    check(::pipe(ready) == 0, "pipe failed");
    const pid_t child = ::fork();
    check(child >= 0, "fork failed");
    if (child == 0) {
        ::close(ready[0]);
        try {
            kiln::Database db(temp.file());
            db.put("old", "deleted");
            db.erase("old");
            db.put_durable("durable", "acknowledged");
            // Send readiness only after durability acknowledgment; then remain alive to be killed.
            const char message = 'R';
            if (::write(ready[1], &message, 1) != 1) ::_exit(2);
            for (;;) ::pause();
        } catch (...) { ::_exit(3); }
    }
    // The parent closes its own write end so child failure can produce EOF on read.
    ::close(ready[1]);
    char message = 0;
    ssize_t received;
    do { received = ::read(ready[0], &message, 1); } while (received < 0 && errno == EINTR);
    ::close(ready[0]);
    ::kill(child, SIGKILL);
    const int status = reap(child);
    check(received == 1 && message == 'R', "child failed before sync acknowledgment");
    check(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL, "child was not killed");
    kiln::Database db(temp.file());
    check(db.get("durable") == "acknowledged", "synced data survives process kill");
    check(!db.get("old"), "synced tombstone survives process kill");
}
} // namespace

// Run named test groups; the first failure prints its context and exits nonzero.
int main() {
    const std::pair<const char*, std::function<void()>> tests[] = {
        {"basic and binary values", basic_and_binary},
        {"durable put visibility, overwrite, and recovery", durable_put_and_recovery},
        {"durable erase results, recovery, and validation", durable_erase_and_recovery},
        {"durable erase after an earlier pending deletion", durable_erase_after_pending_delete},
        {"deterministic reference model (seed 0xC0FFEE, 3000 operations)", deterministic_model},
        {"every final-record truncation boundary", every_tail_truncation},
        {"corruption detection at every byte", corruption_is_reported},
        {"limits and exclusive file ownership", limits_and_ownership},
        {"partial write failure and poisoned writer", failed_append_requires_recovery},
        {"synced data after SIGKILL", synced_data_survives_process_kill},
    };
    for (const auto& [name, test] : tests) {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) {
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
            return 1;
        }
    }
    std::cout << "All 10 test groups passed.\n";
}
