# cpp_ulid

A small, header-only C++ library for generating, parsing, and manipulating [ULIDs](https://github.com/ulid/spec) (Universally Unique Lexicographically Sortable Identifiers).

Provides fast generation, optional per-thread monotonicity, canonical [Crockford Base32 encoding](https://www.crockford.com/base32.html), and a compact, modern API. This implementation is written in standard C++23 with no external dependencies beyond the standard library (the PRNG backend is pluggable).

It comes with an extensive test suite validating correctness, encoding/decoding, monotonicity, and spec compliance.

### What are ULIDs?

A [ULID](https://github.com/ulid/spec) is a 128-bit value with the following properties:

- 48 bits timestamp in milliseconds since Unix epoch  
- 80 bits randomness  
- Encoded using [Crockford Base32](https://www.crockford.com/base32.html) into a 26-character string  
- Lexicographically sortable in timestamp order  
- Human-friendly, URL-safe, case-insensitive input, uppercase canonical output  

Great for: filenames, database keys, logs, ordering events in time, and distributed ID generation.

## Usage

```cpp
#include "ulid.hpp"

using ulid::ulid_t;

auto id  = ulid_t::generate();
auto mid = ulid_t::generate_monotonic();

std::string s = id.to_string();
auto parsed   = ulid::ulid_t::from_string(s);

if(parsed) {
    std::uint64_t ts = parsed->timestamp_ms();
}
```

## Creation
| Function                                          | Returns    | Description                                                                                                                                  |
| ------------------------------------------------- | ---------- | -------------------------------------------------------------------------------------------------------------------------------------------- |
| `generate() noexcept`                      | `ulid_t`   | Generates a ULID using current timestamp and a per-thread PRNG. Millisecond-sorted but not strictly monotonic.                               |
| `generate_monotonic() noexcept`            | `ulid_t`   | Per-thread monotonic sequence (guarantueed only up to 2^80 ULIDs/ms). Handles clock rollback.                                              |
| `from_string(string_view) noexcept`       | `optional<ulid_t>` | Parses a 26-character Base32 ULID. Accepts lowercase and ambiguous input, returns canonical value. Rejects invalid or non-canonical strings. |
| `from_bytes(span<const byte,16>) noexcept` | `ulid_t`   | Constructs from 16 raw bytes.                                                                                                                |

## Conversion
| Method                                          | Returns    | Description                                          |
| ----------------------------------------------- | ---------- | ---------------------------------------------------- |
| `to_string() const`                      | `string`   | Canonical uppercase Crockford Base32 representation. |
| `explicit operator string() const`              | `string`   | Same as `to_string()`.                               |
| `to_bytes() const noexcept`      | `array<byte,16>`    | Raw bytes in big-endian layout.                      |
| `as_bytes() const noexcept` | `span<const byte,16>`     | Borrow internal bytes.                               |
| `timestamp_ms() const noexcept`        | `uint64_t` | Extract 48-bit timestamp field.                      |

## Operators
| Operator     | Description                                  |
| ------------ | -------------------------------------------- |
| `<=>`        | Strong ordering across entire 128-bit value. |
| `==`         | Structural equality.                         |
| `operator<<` | Streams the canonical ULID string.           |

## Tests

The repository ships with test.cpp, a comprehensive correctness suite based on Google Test, covering:

- Roundtrip generation and parsing
- Monotonic ordering 
- Crockford Base32 alphabet and canonical encoding
- Spec example roundtrips
- Timestamp and byte-level correctness

## Dependencies
Optional PRNG dependency: https://github.com/ulfben/cpp_prngs/
If you want a different generator, customize:

`using PRNG = rnd::Random<RomuDuoJr>;`
