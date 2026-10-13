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
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class F> void rejects(F operation) {
    bool threw = false;
    try { operation(); } catch (const kiln::Error&) { threw = true; }
    check(threw, "expected kiln::Error");
}

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

int reap(pid_t child) {
    int status = 0;
    while (::waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) continue;
        throw std::runtime_error("waitpid failed");
    }
    return status;
}

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
            ::signal(SIGXFSZ, SIG_IGN);
            struct rlimit limit {};
            if (::getrlimit(RLIMIT_FSIZE, &limit) != 0) ::_exit(2);
            limit.rlim_cur = static_cast<rlim_t>(prefix + 24 + 2);
            if (::setrlimit(RLIMIT_FSIZE, &limit) != 0) ::_exit(3);
            rejects([&] { db.put("broken", "unfinished"); });
            rejects([&] { db.put("later", "must not append"); });
            rejects([&] { db.sync(); });
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
            db.put("durable", "acknowledged");
            db.erase("old");
            db.sync();
            const char message = 'R';
            if (::write(ready[1], &message, 1) != 1) ::_exit(2);
            for (;;) ::pause();
        } catch (...) { ::_exit(3); }
    }
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

int main() {
    const std::pair<const char*, std::function<void()>> tests[] = {
        {"basic and binary values", basic_and_binary},
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
    std::cout << "All 7 test groups passed.\n";
}
