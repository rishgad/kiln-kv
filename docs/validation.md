# Milestone 1 validation

Validated October 13, 2026 on macOS arm64 using Apple clang 15.0.0 and C++20.

- `make all test`: CLI and tests compiled with warnings treated as errors; all ten test groups passed.
- `make sanitize`: all ten groups passed under AddressSanitizer and UndefinedBehaviorSanitizer.
- Separate CLI processes stored and synced `action-demo -> artifact-manifest`, then read back `artifact-manifest`.

`put_durable` is covered by visibility/reopen checks, the partial-write failure check, and a subprocess killed after its successful return.

`erase_durable` is covered by existing/missing/repeated-key return values, reopen after deletion, preservation of unrelated data, invalid inputs, reinsertion, and a retry after an earlier unsynced deletion. Reopen tests alone cannot prove that a flush occurred; deterministic sync-failure injection remains future work.

The tested failures include an actual partial write induced by a subprocess file-size limit and process termination after sync. These checks do not establish power-loss guarantees, sync-failure handling under injected faults, production readiness, or a performance advantage. The public acknowledgment contract is documented in the README.

No benchmark results have been collected and no remote repository has been created.
