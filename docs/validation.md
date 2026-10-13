# Milestone 1 validation

Validated October 13, 2026 on macOS arm64 using Apple clang 15.0.0 and C++20.

- `make all test`: CLI and tests compiled with warnings treated as errors; all seven test groups passed.
- `make sanitize`: all seven groups passed under AddressSanitizer and UndefinedBehaviorSanitizer.
- Separate CLI processes stored and synced `action-demo -> artifact-manifest`, then read back `artifact-manifest`.

The tested failures include an actual partial write induced by a subprocess file-size limit and process termination after sync. These checks do not establish power-loss guarantees, sync-failure handling under injected faults, production readiness, or a performance advantage. The public acknowledgment contract is documented in the README.

No benchmark results have been collected and no remote repository has been created.
