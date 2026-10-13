#include "kiln/database.hpp"
#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: kiln FILE put KEY VALUE | get KEY | erase KEY | count\n";
        return 2;
    }
    try {
        const std::string_view command = argv[2];
        const bool valid = (command == "put" && argc == 5) ||
            ((command == "get" || command == "erase") && argc == 4) ||
            (command == "count" && argc == 3);
        if (!valid) {
            std::cerr << "invalid command or argument count\n";
            return 2;
        }
        kiln::Database db(argv[1]);
        if (command == "put") {
            db.put(argv[3], argv[4]);
            db.sync();
            std::cout << "stored and synced\n";
        } else if (command == "get") {
            const auto value = db.get(argv[3]);
            if (!value) { std::cout << "not found\n"; return 1; }
            std::cout.write(value->data(), static_cast<std::streamsize>(value->size()));
            std::cout << '\n';
        } else if (command == "erase") {
            const bool removed = db.erase(argv[3]);
            db.sync();
            std::cout << (removed ? "erased and synced\n" : "not found\n");
            return removed ? 0 : 1;
        } else {
            std::cout << db.size() << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << "kiln: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
