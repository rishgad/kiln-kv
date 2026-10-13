# File format, version 1

All integers use explicit little-endian encoding. Native C++ struct layouts are never written to disk. Offsets are zero-based. CRCs are IEEE CRC-32 (reflected polynomial `0xEDB88320`, initial state `0xFFFFFFFF`, final XOR `0xFFFFFFFF`).

## File header: 16 bytes

| Offset | Length | Field |
|---|---|---|
| 0 | 8 | ASCII magic `KILNKV01` |
| 8 | 4 | Format version: 1 |
| 12 | 4 | CRC of bytes 0 through 11 |

An incomplete or invalid file header is an error. A zero-length file is initialized and synced when opened.

## Record: 24-byte header, then key, then value

| Offset within record | Length | Field |
|---|---|---|
| 0 | 4 | Magic `KVR1` |
| 4 | 1 | Operation: 1 = put, 2 = tombstone |
| 5 | 3 | Reserved; must be zero |
| 8 | 4 | Key byte count |
| 12 | 4 | Value byte count; zero for tombstones |
| 16 | 4 | Record CRC |
| 20 | 4 | Header CRC |
| 24 | key byte count | Key bytes |
| 24 + key byte count | value byte count | Value bytes |

The record CRC covers the complete header with both CRC fields zeroed, followed by the key and value. The header CRC covers bytes 0 through 19 with the record CRC populated. This avoids circular checksum dependencies and lets recovery validate lengths before trusting them.

## Recovery rules

Starting immediately after the file header:

1. Fewer than 24 bytes remaining: discard the incomplete tail.
2. Validate a complete record header, including its independent checksum and size limits. Invalid headers are errors.
3. Valid header but insufficient remaining payload: discard the incomplete tail.
4. Complete payload with a bad record CRC: report corruption; do not discard it silently.
5. Apply the record to the index and advance to the next record.

On tail repair, truncate at the last complete record and fsync the file before subsequent writes. A complete tombstone removes the key from the index. A complete put replaces its index entry.

The supported interrupted-append model assumes a valid prior prefix plus a missing suffix. Arbitrary storage corruption and physical write reordering can violate that model and result in an error rather than successful automatic recovery. Checksums are probabilistic corruption detection, not a proof that bytes are intact.
