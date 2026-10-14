# Kiln KV

[![Storage tests](https://github.com/rishgad/kiln-kv/actions/workflows/ci.yml/badge.svg)](https://github.com/rishgad/kiln-kv/actions/workflows/ci.yml)

A C++20 persistent key-value engine for a planned shared build-artifact cache.

**Current status:** an embedded storage engine and command-line tool. Artifact caching and a network service are not implemented yet.

## Intended use case

Developers and CI workers often compile the same inputs repeatedly. A shared build cache lets one worker reuse an output produced by another when source files, dependencies, the compiler, flags, and other relevant inputs match.

The planned cache will store two kinds of records:

- **Content hash → artifact bytes:** immutable compiled outputs, identified by their contents.
- **Build-input fingerprint → result manifest:** the artifacts produced by a particular build action.

The build client will construct input fingerprints; the storage engine will persist and retrieve records. Complete fingerprinting and verified artifact publication are necessary for correct cache hits. The current engine stores ordinary byte keys and values and does not enforce these artifact semantics.

## What works today

- Insert, overwrite, retrieve, and delete byte keys and values.
- Append-only disk storage with an in-memory index.
- Independent header and record checksums.
- Explicit synchronization for persistence, plus `put_durable` for immediate durable acknowledgment.
- Restart recovery, including incomplete final-record repair.
- Exclusive file ownership through an advisory lock.
- A CLI and deterministic correctness tests.

## Build and run

Requires Make and a C++20 compiler. Uses POSIX file APIs and has been validated on macOS arm64; the GitHub Actions matrix also validates Linux and macOS builds, correctness tests, and sanitizers. No third-party libraries are required.

```sh
make
make test
make sanitize
```

To use another compiler, run `make CXX=g++` from a clean build directory.

Each command below opens the same database file, demonstrating persistence across processes:

```sh
./build/kiln demo.kv put example-key 'example value'
./build/kiln demo.kv get example-key
./build/kiln demo.kv count
./build/kiln demo.kv erase example-key
```

Use a new file name to start with an empty database. The parent directory must already exist. The CLI accepts text arguments; the C++ API supports arbitrary bytes, including embedded NULs and empty strings.

## C++ API

```cpp
#include "kiln/database.hpp"

kiln::Database db("example.kv");
db.put_durable("key", "value"); // Returns after append and sync succeed

auto value = db.get("key"); // std::optional<std::string>
bool removed = db.erase("key");
db.sync();
```

| Operation | Behavior |
|---|---|
| `put(key, value)` | Insert or replace a value; visible after successful return |
| `put_durable(key, value)` | Insert or replace, then sync before successful return |
| `get(key)` | Return a value, or `std::nullopt` when absent |
| `erase(key)` | Delete an existing key; return whether it existed |
| `erase_durable(key)` | Delete if present, then sync even if absent; return whether it existed |
| `size()` | Return the number of live keys |
| `sync()` | Flush the file and parent directory using `fsync` |

An empty value is distinct from a missing key. Keys are limited to 64 KiB and values to 16 MiB. Operations report storage and validation errors with `kiln::Error`.

## Storage design and tradeoffs

Writes append records to a single file. The in-memory index maps each live key to its latest record's offset and lengths. Reads use that index to locate and checksum the record. On restart, the engine scans the log to reconstruct the index and apply deletion records, called tombstones.

| Choice | Benefit | Cost |
|---|---|---|
| Append-only records | Avoid overwriting existing records in place | Overwrites and deletes leave obsolete bytes on disk |
| In-memory key index | Locate records without scanning the log on each read | Memory usage grows with live keys |
| Full log scan on startup | Recover without maintaining a separate persistent index | Startup work grows with total log size |
| Checksums on headers and records | Detect accidental damage and validate lengths before allocation | Additional CPU work during reads, writes, and recovery |
| Explicit `sync` | Allow callers to choose flush boundaries | Callers must distinguish visibility from durability |

See [the file-format specification](docs/file-format.md) for record layouts and recovery rules.

## Durability and failure behavior

`put` and `erase` make a mutation visible in the current process; they do not acknowledge durability. A successful `sync` is the persistence boundary, subject to operating system, filesystem, and device guarantees. The CLI syncs mutations before reporting success. Closing the database does not substitute for `sync`.

`put_durable` combines `put` and `sync`. Successful return means the value is visible through the index and the sync completed. It also flushes earlier pending mutations in the same file. It adds a flush to each call; use separate `put` calls followed by one `sync` when batching is appropriate. It does not add transaction atomicity or rollback on failure.

Recovery discards an incomplete final record and flushes the repair before admitting new writes. Invalid complete headers and checksum mismatches produce errors instead of silently discarding records. This recovery model assumes a valid prior prefix followed by an interrupted append; it does not guarantee automatic repair of arbitrary corruption.

A failed append or sync makes the instance unusable until reopened. A failed mutation may still have reached the log; recovery determines the resulting state. Invalid input rejected before writing does not poison the instance.

One instance may own a database file at a time. Operations on an instance are not thread-safe, and the file must not be modified or replaced externally while open. Checksums detect accidental corruption, not malicious modifications.

## Verification

The test suite covers:

- Binary data, empty values, overwrites, deletion, and reinsertion across reopen.
- Durable put visibility, overwrite recovery, and invalid-input rejection.
- Durable erase return values, deletion recovery, pending-deletion retry, and invalid-input rejection.
- 3,000 deterministic operations compared against a reference map, with periodic reopen and state checks.
- Every truncation boundary in a final put or tombstone, including writes after repair.
- Single-bit corruption at every byte of a sample file, plus corruption during a live read.
- Input limits and exclusive file ownership.
- An induced partial-write failure, poisoned-instance behavior, and recovery.
- Recovery after a subprocess is killed following successful sync.

The suite also runs under AddressSanitizer and UndefinedBehaviorSanitizer. See [recorded validation](docs/validation.md). Process-kill tests are not power-loss tests; filesystem and sync-failure fault injection remain future work.

## Next steps

1. Immutable artifact storage and build-result manifests.
2. A controlled build client with complete input fingerprinting.
3. Storage budgets, eviction, and compaction.
4. A shared server and multiple build workers.
5. Reproducible engine and end-to-end cache comparisons.

There is currently no compaction, eviction, artifact hashing, networking, replication, or benchmark evidence of an advantage over established databases. This README will change as those capabilities are implemented and verified.
