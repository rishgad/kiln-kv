CXX = clang++
CPPFLAGS = -Iinclude
CXXFLAGS = -std=c++20 -Wall -Wextra -Wpedantic -Werror -O2 -g

.PHONY: all test sanitize clean
all: build/kiln

build:
	mkdir -p build

build/kiln: src/database.cpp src/main.cpp include/kiln/database.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/database.cpp src/main.cpp -o $@

build/tests: src/database.cpp tests/database_test.cpp include/kiln/database.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/database.cpp tests/database_test.cpp -o $@

test: build/tests
	./build/tests

sanitize: | build
	$(CXX) $(CPPFLAGS) -std=c++20 -Wall -Wextra -Wpedantic -Werror -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer src/database.cpp tests/database_test.cpp -o build/tests-sanitize
	./build/tests-sanitize

clean:
	rm -rf build
