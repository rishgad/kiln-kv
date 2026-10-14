# Compiler can be overridden with make CXX=g++. Build uses no external libraries.
CXX = clang++
CPPFLAGS = -Iinclude
# C++20, strict warnings, optimization, and debug information for tracing execution.
CXXFLAGS = -std=c++20 -Wall -Wextra -Wpedantic -Werror -O2 -g

.PHONY: all test sanitize clean
all: build/kiln

build:
	mkdir -p build

# A header/source change rebuilds the binary. The order-only build prerequisite
# creates the directory without its timestamp forcing unnecessary rebuilds.
build/kiln: src/database.cpp src/main.cpp include/kiln/database.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/database.cpp src/main.cpp -o $@

build/tests: src/database.cpp tests/database_test.cpp include/kiln/database.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/database.cpp tests/database_test.cpp -o $@

test: build/tests
	./build/tests

# Instrument memory access and undefined behavior, then run the same test suite.
sanitize: | build
	$(CXX) $(CPPFLAGS) -std=c++20 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer src/database.cpp tests/database_test.cpp -o build/tests-sanitize
	./build/tests-sanitize

clean:
	rm -rf build
