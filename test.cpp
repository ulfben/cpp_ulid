#pragma once
#include "ulid.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <compare>
#include <gtest/gtest.h>
#include <random>
#include <set>
#include <string>
#include <vector>
#include <sstream>

namespace {
	using ulid::ulid_t;

	//helper
	// Crockford Base32 alphabet from the spec:
		// 0123456789ABCDEFGHJKMNPQRSTVWXYZ
	constexpr static bool is_crockford_char(char c) noexcept{
		switch(c){
		case '0': case '1': case '2': case '3': case '4':
		case '5': case '6': case '7': case '8': case '9':
		case 'A': case 'B': case 'C': case 'D': case 'E':
		case 'F': case 'G': case 'H': case 'J': case 'K':
		case 'M': case 'N': case 'P': case 'Q': case 'R':
		case 'S': case 'T': case 'V': case 'W': case 'X':
		case 'Y': case 'Z':
			return true;
		default:
			return false;
		}
	};

// Helper: construct a ULID byte array from a 48-bit timestamp
// Timestamp is encoded big-endian in bytes[0..5], rest of the bytes are set to 0.
	static std::array<ulid_t::byte, 16> make_bytes_from_timestamp(std::uint64_t ts) noexcept{
		std::array<ulid_t::byte, 16> bytes{};
		// write 48-bit big-endian timestamp into bytes[0..5]
		for(int i = 0; i < 6; ++i){
			bytes[i] = static_cast<ulid_t::byte>(
				(ts >> ((5 - i) * 8)) & 0xFFu
				);
		}
		// bytes[6..15] remain zero
		return bytes;
	}

	TEST(Ulid, AllZeroBytesRoundtrip){
		ulid_t zero{}; // default-initialized, all bytes zero
		auto str = zero.to_string();

		// Should parse back successfully
		auto parsed_opt = ulid_t::from_string(str);
		ASSERT_TRUE(parsed_opt.has_value());

		EXPECT_EQ(*parsed_opt, zero);
	}

	TEST(Ulid, ToStringHasCorrectLengthAndAlphabet){
		auto id = ulid_t::generate();
		auto s = id.to_string();

		ASSERT_EQ(s.size(), 26u);

		for(char c : s){
			ASSERT_TRUE(is_crockford_char(c))
				<< "Unexpected character in ULID string: " << c;
		}
	}

	TEST(Ulid, RoundtripGenerate){
		// Basic property test: generate -> to_string -> from_string is identity.
		for(int i = 0; i < 1000; ++i){
			auto id = ulid_t::generate();
			auto str = id.to_string();

			auto parsed_opt = ulid_t::from_string(str);
			ASSERT_TRUE(parsed_opt.has_value()) << "Failed to parse: " << str;

			auto parsed = *parsed_opt;
			EXPECT_EQ(parsed, id);
		}
	}

	TEST(Ulid, RoundtripGenerateMonotonic){
		for(int i = 0; i < 1000; ++i){
			auto id = ulid_t::generate_monotonic();
			auto str = id.to_string();

			auto parsed_opt = ulid_t::from_string(str);
			ASSERT_TRUE(parsed_opt.has_value()) << "Failed to parse: " << str;

			auto parsed = *parsed_opt;
			EXPECT_EQ(parsed, id);
		}
	}

	TEST(Ulid, MonotonicSequenceIsStrictlyIncreasing){
		// Per-thread, generate_monotonic() should produce strictly increasing IDs.
		constexpr int N = 512;
		std::array<ulid_t, N> ids{};

		for(int i = 0; i < N; ++i){
			ids[i] = ulid_t::generate_monotonic();
		}

		for(int i = 1; i < N; ++i){
			EXPECT_LT(ids[i - 1], ids[i])
				<< "Non-monotonic at index " << i;
		}
	}

	TEST(Ulid, GenerateProducesMostlyUniqueIds){
		constexpr int N = 2000;
		std::set<std::string> s;

		for(int i = 0; i < N; ++i){
			s.insert(ulid_t::generate().to_string());
		}

		// Extremely unlikely to collide at this scale
		EXPECT_EQ(s.size(), N);
	}

	TEST(Ulid, FromStringRejectsInvalidLength){
		auto too_short = ulid_t::from_string("123");
		auto too_long = ulid_t::from_string(std::string(30, 'A'));
		EXPECT_FALSE(too_short.has_value());
		EXPECT_FALSE(too_long.has_value());
	}

	TEST(Ulid, FromStringRejectsInvalidCharacters){
		// Contains '!' which is not in Crockford Base32
		auto invalid = ulid_t::from_string("01ARZ3NDEKTSV4RRFFQ69G5FA!");
		EXPECT_FALSE(invalid.has_value());
	}

	TEST(Ulid, FromStringRejectsNonCanonicalHighBits){
		// First digit '8' => value 8 (0b01000), which sets the top bits non-zero
		std::string non_canonical = "8" + std::string(25, '0');

		auto parsed = ulid_t::from_string(non_canonical);
		EXPECT_FALSE(parsed.has_value());
	}

	TEST(Ulid, FromStringAcceptsLowercaseAndAmbiguousCharacters){
		const std::string canonical = "01ARZ3NDEKTSV4RRFFQ69G5FAV";

		auto base_opt = ulid_t::from_string(canonical);
		ASSERT_TRUE(base_opt.has_value());
		auto base = *base_opt;

		// lower case version
		std::string lower = canonical;
		std::ranges::transform(lower, lower.begin(),
			[](unsigned char c){ return static_cast<char>(std::tolower(c)); });

		auto lower_opt = ulid_t::from_string(lower);
		ASSERT_TRUE(lower_opt.has_value());

		// replace some digits with ambiguous forms: 0 -> O, 1 -> l
		std::string ambiguous = canonical;
		ambiguous[0] = 'O';  // 0 -> 'O'
		ambiguous[1] = 'l';  // 1 -> 'l'

		auto ambiguous_opt = ulid_t::from_string(ambiguous);
		ASSERT_TRUE(ambiguous_opt.has_value());

		EXPECT_EQ(*lower_opt, base);
		EXPECT_EQ(*ambiguous_opt, base);
	}

	TEST(Ulid, ToStringProducesCanonicalUppercase){
	// Mixed case and ambiguous letters
		std::string messy = "o1arz3ndeKtSv4rrffq69g5fav"; // note 'o', 'k', lower case

		auto parsed_opt = ulid_t::from_string(messy);
		ASSERT_TRUE(parsed_opt.has_value());

		auto canonical = parsed_opt->to_string();

		// All uppercase and still valid Crockford
		for(char c : canonical){
			EXPECT_FALSE(std::islower(static_cast<unsigned char>(c)));
		}

		// Roundtrip again from canonical should be stable
		auto parsed2_opt = ulid_t::from_string(canonical);
		ASSERT_TRUE(parsed2_opt.has_value());
		EXPECT_EQ(*parsed_opt, *parsed2_opt);
	}

	TEST(Ulid, KnownSpecExampleRoundtrip){
		// Canonical example from the official ULID spec:        
		const std::string spec_example = "01ARZ3NDEKTSV4RRFFQ69G5FAV";

		auto parsed_opt = ulid_t::from_string(spec_example);
		ASSERT_TRUE(parsed_opt.has_value());

		auto parsed = *parsed_opt;
		auto encoded = parsed.to_string();

		EXPECT_EQ(encoded, spec_example);
	}

	TEST(Ulid, MonotonicSpecOrderingMatchesComparison){
		// Monotonic example from the spec: two values in the same millisecond
		// where the random part is incremented.
		const std::string s1 = "01BX5ZZKBKACTAV9WEVGEMMVRZ";
		const std::string s2 = "01BX5ZZKBKACTAV9WEVGEMMVS0";

		auto u1_opt = ulid_t::from_string(s1);
		auto u2_opt = ulid_t::from_string(s2);

		ASSERT_TRUE(u1_opt.has_value());
		ASSERT_TRUE(u2_opt.has_value());

		auto u1 = *u1_opt;
		auto u2 = *u2_opt;

		// Lexicographic order of strings should match operator< ordering
		EXPECT_LT(s1, s2);
		EXPECT_LT(u1, u2);

		// Encoding should roundtrip to the same canonical strings
		EXPECT_EQ(u1.to_string(), s1);
		EXPECT_EQ(u2.to_string(), s2);
	}

	TEST(Ulid, EqualityAndThreeWayComparison){
		// Same string should parse to equal ulid_t
		const std::string s = "01ARZ3NDEKTSV4RRFFQ69G5FAV";

		auto a_opt = ulid_t::from_string(s);
		auto b_opt = ulid_t::from_string(s);
		ASSERT_TRUE(a_opt.has_value());
		ASSERT_TRUE(b_opt.has_value());

		auto a = *a_opt;
		auto b = *b_opt;

		EXPECT_EQ(a, b);
		EXPECT_FALSE(a < b);
		EXPECT_FALSE(b < a);
		EXPECT_EQ(a <=> b, std::strong_ordering::equal);
	}

	TEST(Ulid, SortingByValueMatchesSortingByString){
		constexpr int N = 128;
		std::array<ulid_t, N> ids{};

		for(int i = 0; i < N; ++i){
			ids[i] = ulid_t::generate_monotonic();
		}

		// Shuffle to avoid relying on generation order
		std::mt19937 rng{12345};
		std::shuffle(ids.begin(), ids.end(), rng);

		// Sort by ULID value
		auto ids_sorted = ids;
		std::sort(ids_sorted.begin(), ids_sorted.end());

		// Sort by string
		std::vector<std::string> strings;
		strings.reserve(N);
		for(const auto& id : ids){
			strings.push_back(id.to_string());
		}
		std::sort(strings.begin(), strings.end());

		// Compare
		for(int i = 0; i < N; ++i){
			EXPECT_EQ(ids_sorted[i].to_string(), strings[i]);
		}
	}

	TEST(Ulid, StreamsCanonicalString){
		const auto id = ulid_t::generate();

		const auto expected = id.to_string();
		std::ostringstream oss;
		oss << id;

		EXPECT_EQ(oss.str(), expected);
	}

	TEST(Ulid, StreamsEmptyIsConsistent){
		// Construct a zero-initialized ULID and ensure streaming matches to_string.
		const ulid_t zero{};
		const auto expected = zero.to_string();

		std::ostringstream oss;
		oss << zero;

		EXPECT_EQ(oss.str(), expected);
	}

	TEST(Ulid, ExtractsTimestampFromBytes){
		// 48-bit timestamp with a simple, recognisable byte pattern:
		// ts = 0x00 01 02 03 04 05
		const std::uint64_t ts = 0x000102030405ULL;
		const auto bytes = make_bytes_from_timestamp(ts);

		const auto id = ulid_t::from_bytes(std::span<const ulid_t::byte, 16>{bytes});
		EXPECT_EQ(id.timestamp_ms(), ts);
	}

	TEST(Ulid, ExtractsMax48BitTimestamp){
		// Max 48-bit value: 0xFFFF FFFF FFFF
		const std::uint64_t ts = (1ULL << 48) - 1ULL; // 0xFFFFFFFFFFFF
		const auto bytes = make_bytes_from_timestamp(ts);

		const auto id = ulid_t::from_bytes(std::span<const ulid_t::byte, 16>{bytes});
		EXPECT_EQ(id.timestamp_ms(), ts);
	}

	TEST(Ulid, ToBytesFromBytesRoundTrip){
		// Fill with a recognisable pattern so we catch byte-order mistakes.
		std::array<ulid_t::byte, 16> original{};
		for(std::size_t i = 0; i < original.size(); ++i){
			original[i] = static_cast<ulid_t::byte>(i * 7); // arbitrary pattern
		}
		const auto id = ulid_t::from_bytes(std::span<const ulid_t::byte, 16>{original});
		const auto roundtrip = id.to_bytes();
		EXPECT_EQ(roundtrip, original);
	}

	TEST(Ulid, FromBytesEquality){
		std::array<ulid_t::byte, 16> bytes1{};
		std::array<ulid_t::byte, 16> bytes2{};

		for(std::size_t i = 0; i < bytes1.size(); ++i){
			bytes1[i] = static_cast<ulid_t::byte>(i);
			bytes2[i] = static_cast<ulid_t::byte>(i);
		}
		const auto id1 = ulid_t::from_bytes(std::span<const ulid_t::byte, 16>{bytes1});
		const auto id2 = ulid_t::from_bytes(std::span<const ulid_t::byte, 16>{bytes2});
		EXPECT_EQ(id1, id2);
	}

	TEST(Ulid, ReadableStringHasCorrectShape){
		auto id = ulid_t::generate();
		auto s = id.to_readable_string();
		//YYYYMMDDThhmmssmmmZrrrrrrrrrrrrrrrr
		ASSERT_EQ(s.size(), 35u);
		EXPECT_EQ(s[8], 'T');
		EXPECT_EQ(s[18], 'Z');
	}

	TEST(Ulid, ReadableStringHasValidContent){
		auto id = ulid_t::generate();
		auto s = id.to_readable_string();
		ASSERT_EQ(s.size(), 35u);
		// YYYYMMDDThhmmssmmmZrrrrrrrrrrrrrrrr
		for(int i = 0; i < 8; ++i){
			EXPECT_TRUE(std::isdigit(static_cast<unsigned char>(s[i])))
				<< "Expected digit in date at pos " << i << ", got: " << s[i];
		}

		EXPECT_EQ(s[8], 'T');

		for(int i = 9; i <= 17; ++i){
			EXPECT_TRUE(std::isdigit(static_cast<unsigned char>(s[i])))
				<< "Expected digit in time at pos " << i << ", got: " << s[i];
		}

		EXPECT_EQ(s[18], 'Z');

		for(int i = 19; i < 35; ++i){
			ASSERT_TRUE(is_crockford_char(s[i]))
				<< "Unexpected character in readable ULID random tail: " << s[i];
		}
	}

	TEST(Ulid, ReadableStringSortingMatchesValueSorting){
		constexpr int N = 64;
		std::array<ulid_t, N> ids{};

		for(int i = 0; i < N; ++i){
			ids[i] = ulid_t::generate_monotonic();
		}

		// Shuffle to avoid relying on generation order
		std::mt19937 rng{1234};
		std::shuffle(ids.begin(), ids.end(), rng);

		auto ids_sorted = ids;
		std::sort(ids_sorted.begin(), ids_sorted.end());

		std::vector<std::string> readable;
		readable.reserve(N);
		for(const auto& id : ids){
			readable.push_back(id.to_readable_string());
		}
		std::sort(readable.begin(), readable.end());

		for(int i = 0; i < N; ++i){
			EXPECT_EQ(ids_sorted[i].to_readable_string(), readable[i]);
		}
	}

	TEST(Ulid, FromReadableStringRejectsInvalidShape){
		// Too short
		auto t1 = ulid_t::from_readable_string("20250101T000000000Z");
		EXPECT_FALSE(t1.has_value());

		// Missing 'T'
		auto t2 = ulid_t::from_readable_string("20250101X000000000ZABCDEFGHIJKLMNOP");
		EXPECT_FALSE(t2.has_value());

		// Missing 'Z'
		auto t3 = ulid_t::from_readable_string("20250101T000000000XABCDEFGHIJKLMNOP");
		EXPECT_FALSE(t3.has_value());
	}

	TEST(Ulid, ReadableRoundtripGenerate){
	// generate -> to_readable_string -> from_readable_string is identity.
		for(int i = 0; i < 500; ++i){
			auto id = ulid_t::generate();
			auto s = id.to_readable_string();

			auto parsed_opt = ulid_t::from_readable_string(s);
			ASSERT_TRUE(parsed_opt.has_value()) << "Failed to parse readable ULID: " << s;

			EXPECT_EQ(*parsed_opt, id);
		}
	}

	TEST(Ulid, ReadableRoundtripKnownSpecExample){
	// Canonical example from the ULID spec
		const std::string spec_example = "01ARZ3NDEKTSV4RRFFQ69G5FAV";

		auto base_opt = ulid_t::from_string(spec_example);
		ASSERT_TRUE(base_opt.has_value());
		auto base = *base_opt;

		auto readable = base.to_readable_string();

		auto parsed_opt = ulid_t::from_readable_string(readable);
		ASSERT_TRUE(parsed_opt.has_value());

		auto parsed = *parsed_opt;
		EXPECT_EQ(parsed.to_string(), spec_example);
		EXPECT_EQ(parsed, base);
	}

	TEST(Ulid, FromReadableStringRejectsOutOfRangeFields){
		// Helper to make a readable string with a specific "YYYYMMDDThhmmssmmmZ"´and a fixed random tail.
		auto make_readable = [](std::string prefix){
			// prefix must be 19 chars: YYYYMMDDThhmmssmmmZ
			EXPECT_EQ(prefix.size(), 19u);
			return prefix + "0123456789ABCDEFG"; // 16 Crockford chars
		};

		// Month 13
		{
			std::string s = make_readable("20251301T000000000Z");
			auto r = ulid_t::from_readable_string(s);
			EXPECT_FALSE(r.has_value());
		}

		// Day 00
		{
			std::string s = make_readable("20250100T000000000Z");
			auto r = ulid_t::from_readable_string(s);
			EXPECT_FALSE(r.has_value());
		}

		// Day 32
		{
			std::string s = make_readable("20250132T000000000Z");
			auto r = ulid_t::from_readable_string(s);
			EXPECT_FALSE(r.has_value());
		}

		// Hour 24
		{
			std::string s = make_readable("20250101T240000000Z");
			auto r = ulid_t::from_readable_string(s);
			EXPECT_FALSE(r.has_value());
		}

		// Minute 60
		{
			std::string s = make_readable("20250101T006000000Z");
			auto r = ulid_t::from_readable_string(s);
			EXPECT_FALSE(r.has_value());
		}

		// Second 60
		{
			std::string s = make_readable("20250101T000060000Z");
			auto r = ulid_t::from_readable_string(s);
			EXPECT_FALSE(r.has_value());
		}

		// Milliseconds 1000
		{
			std::string s = "20250101T0000001000Z0123456789ABCDEFG";
			auto r = ulid_t::from_readable_string(s);
			EXPECT_FALSE(r.has_value());
		}
	}

	TEST(Ulid, FromReadableStringRejectsInvalidRandomTail){
		// Valid prefix: 2025-01-01T00:00:00.000Z
		std::string prefix = "20250101T000000000Z";

		// Tail with '!' which is not a Crockford Base32 char
		std::string invalid_tail = "0123456789ABF!";
		invalid_tail.resize(16, '0'); // ensure length 16; '!' somewhere in there

		std::string s = prefix + invalid_tail;

		auto r = ulid_t::from_readable_string(s);
		EXPECT_FALSE(r.has_value());
	}

	TEST(Ulid, FromReadableStringSortingMatchesStringSorting){
		constexpr int N = 32;
		std::vector<std::string> readable;
		readable.reserve(N);

		for(int i = 0; i < N; ++i){
			auto id = ulid_t::generate_monotonic();
			readable.push_back(id.to_readable_string());
		}

		// Shuffle to break generation order
		std::mt19937 rng{123};
		std::shuffle(readable.begin(), readable.end(), rng);

		// Sort strings lexicographically		
		std::sort(readable.begin(), readable.end());

		// Parse back and ensure ulids are strictly increasing
		std::vector<ulid_t> parsed;
		parsed.reserve(N);
		for(const auto& s : readable){
			auto r = ulid_t::from_readable_string(s);
			ASSERT_TRUE(r.has_value()) << "Failed to parse: " << s;
			parsed.push_back(*r);
		}

		for(int i = 1; i < N; ++i){
			EXPECT_LT(parsed[i - 1], parsed[i])
				<< "Non-increasing at index " << i;
		}
	}


} // namespace

