#include "kiln/database.hpp"
#include <iostream>
#include <string_view>

// argv contains program name, database path, command, and optional operands.
// Exit codes: 0 success, 1 missing key/storage failure, 2 invalid usage.
int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: kiln FILE put KEY VALUE | get KEY | erase KEY | count\n";
        return 2;
    }
    try {
        const std::string_view command = argv[2];
        // Validate argument counts before accessing operands or opening a database.
        const bool valid = (command == "put" && argc == 5) ||
            ((command == "get" || command == "erase") && argc == 4) ||
            (command == "count" && argc == 3);
        if (!valid) {
            std::cerr << "invalid command or argument count\n";
            return 2;
        }
        // Opening reconstructs the index. Scope exit closes the file handles (RAII).
        kiln::Database db(argv[1]);
        if (command == "put") {
            // Print success only after the write and sync both finish successfully.
            db.put_durable(argv[3], argv[4]);
            std::cout << "stored and synced\n";
        } else if (command == "get") {
            const auto value = db.get(argv[3]);
            if (!value) { std::cout << "not found\n"; return 1; }
            // Write the explicit length so embedded NUL bytes do not truncate output.
            std::cout.write(value->data(), static_cast<std::streamsize>(value->size()));
            std::cout << '\n';
        } else if (command == "erase") {
            const bool removed = db.erase(argv[3]);
            // erase is unsynced in the engine; explicitly flush before CLI acknowledgment.
            db.sync();
            std::cout << (removed ? "erased and synced\n" : "not found\n");
            return removed ? 0 : 1;
        } else {
            std::cout << db.size() << '\n';
        }
    // Engine exceptions skip success output and become a nonzero process exit.
    } catch (const std::exception& error) {
        std::cerr << "kiln: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
