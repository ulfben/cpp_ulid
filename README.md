# cpp_ulid

A small, header-only C++ library for generating, parsing, and manipulating [ULIDs](https://github.com/ulid/spec) (Universally Unique Lexicographically Sortable Identifiers).

Provides fast generation, optional per-thread monotonicity, canonical [Crockford Base32 encoding](https://www.crockford.com/base32.html), and a compact, modern API. This implementation is written in standard C++23 with no external dependencies beyond the standard library (the PRNG backend is pluggable).

It comes with an extensive test suite validating correctness, encoding/decoding, monotonicity, and spec compliance. 

[Try it on Compiler Explorer](https://compiler-explorer.com/z/d36E4jf3M)!

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

## Human-readable extension:
This library also supports a non-standard 35-character ULID variant with an embedded ISO8601 timestamp (`YYYYMMDDThhmmssmmmZ`). It preserves millisecond resolution and the same sort order as canonical ULIDs, while making the timestamp easier to inspect in logs, filenames, and debugging tools.

Use `to_readable_string()` and `from_readable_string()` to convert to and from this representation.

## Creation
| Function                                          | Returns            | Description                                                                                                                                  |
| ------------------------------------------------- | ------------------ | -------------------------------------------------------------------------------------------------------------------------------------------- |
| `generate() noexcept`                             | `ulid_t`           | Generates a ULID using current timestamp and a per-thread PRNG. Millisecond-sorted but not strictly monotonic.                              |
| `generate_monotonic() noexcept`                   | `ulid_t`           | Per-thread monotonic sequence (guaranteed only up to 2^80 ULIDs/ms). Handles clock rollback.                                                |
| `from_bytes(span<const byte,16>) noexcept`        | `ulid_t`           | Constructs from 16 raw bytes.                                                                                                                |                                                                                                            |
| `from_uint64s(uint64_t hi, uint64_t lo) noexcept` | `ulid_t` | Constructs a ULID from a 128-bit big-endian value split into high and low 64-bit words. |
| `from_string(string_view) noexcept`               | `optional<ulid_t>` | Parses a 26-character Base32 ULID. Accepts lowercase and ambiguous input, returns canonical ULID or nullopt.|
| `from_readable_string(string_view)`               | `optional<ulid_t>` | Parses the extended 35-character format (`YYYYMMDDThhmmssmmmZxxxxxxxxxxxxxxxx`). Human-readable timestamp + 16-char ULID randomness. Returns canonical ULID or `nullopt` on invalid input. |


## Conversion
| Method                                          | Returns    | Description                                          |
| ----------------------------------------------- | ---------- | ---------------------------------------------------- |
| `to_string() const`                      | `string`   | 26-character canonical uppercase Crockford Base32 representation. |
| `to_readable_string() const`                      | `string`   | 35-character representation with human-readable ISO8601-form timestamp (`YYYYMMDDThhmmssmmmZ`). |
| `explicit operator string() const`              | `string`   | Same as `to_string()`.                               |
| `to_bytes() const noexcept`      | `array<byte,16>`    | Raw bytes in big-endian layout.                      |
| `as_bytes() const noexcept` | `span<const byte,16>`     | Borrow internal bytes.                               |
| `to_uint64s() const noexcept` | `pair<uint64_t,uint64_t>` | Returns the 128-bit value as `{hi, lo}` 64-bit words in big-endian layout. |
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

You can [run all tests on compiler-explorer](https://compiler-explorer.com/z/d36E4jf3M).

## Dependencies
Optional PRNG dependency: https://github.com/ulfben/cpp_prngs/
If you want a different generator, customize:

`using PRNG = rnd::Random<RomuDuoJr>;`
