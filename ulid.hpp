#pragma once
#include "random.hpp" //grab from: https://github.com/ulfben/cpp_prngs/
#include "romuduojr.hpp" //grab from: https://github.com/ulfben/cpp_prngs/
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <format>
#include <charconv>

// ULID (Universally Unique Lexicographically Sortable Identifier) is fundamentally:
// - a 128-bit unsigned integer
// - serialized to 16 numeric bytes
// - encoded using Crockford base32
//
// The 128-bit are laid out thusly:
//   - 48 bits: millisecond timestamp since Unix epoch
//   - 80 bits: randomness
//
// Encoded in Crockford Base32 it becomes a 26 character string that is
// lexicographically sortable in the same order as its timestamp. This makes
// ULIDs useful as human-friendly, time-orderable identifiers for logs,
// database keys, filenames, etc.
// 
// This header provides:
//
//   - ulid_t::generate()
//       Generates a ULID using the current time and a per-thread PRNG. 
//		 The result is time-ordered at millisecond precision, but multiple 
//		 IDs within the same millisecond are not guaranteed to be
//       strictly monotonic.
//
//   - ulid_t::generate_monotonic()
//       Generates a per-thread monotonic ULID sequence. Within each thread,
//       IDs are strictly increasing in lexicographic order, even when many
//       IDs are created in the same millisecond or when the system clock
//       moves backwards.
//		 
//		 Monotonicity is per thread only: there is no cross-thread coordination,
//		 no locking, and no global ordering between threads.
//
//   - ulid_t::from_string()
//       Parses a 26 character Crockford Base32 ULID string into a ulid_t.
//       Returns std::nullopt if the string is invalid or non-canonical.
//
//   - ulid_t::to_string()
//       Encodes a ulid_t into its canonical 26 character Crockford Base32
//       representation.
//
// Many thanks to Marius Bancila for the inspiration! 
//   https://mariusbancila.ro/blog/2025/11/27/universally-unique-lexicographically-sortable-identifiers-ulids/

namespace ulid{

	class ulid_t final{
	public:
		using byte = std::uint8_t;
		using PRNG = rnd::Random<RomuDuoJr>;
		// Feel free to replace RomuDuoJr with any PRNG you like (e.g. std::mt19937).
		// RomuDuoJr is the default here because it is tiny, extremely fast, and produces
		// good statistical quality for non-cryptographic identifiers.
		// see: https://github.com/ulfben/cpp_prngs/ for benchmarks and more information

		[[nodiscard]] static ulid_t generate() noexcept{
			const auto ts = now_ms();
			static thread_local auto rng = PRNG{salted_seed(ts)};
			ulid_t ulid{};
			write_big_endian<6>(ts, ulid.timestamp_bytes()); //fill timestamp bytes			
			for(auto& b : ulid.random_bytes()){
				b = rng.bits_as<byte>(); // get a uniformly-distributed byte from the PRNG.
					// Similar in use to std::uniform_int_distribution(0,255), but drastically faster.
			}
			return ulid;
		}

		[[nodiscard]] static ulid_t generate_monotonic() noexcept{
			const auto ts = now_ms();
			static thread_local auto rng = PRNG{salted_seed(ts)};
			static thread_local ulid_t last{}; // previously generated ULID
			static thread_local std::uint64_t last_ts = 0;
			static thread_local bool have_last = false;

			if(!have_last || ts > last_ts){ // new millisecond: fresh timestamp + fresh randomness			
				last_ts = ts;
				write_big_endian<6>(ts, last.timestamp_bytes());
				for(auto& b : last.random_bytes()){
					b = rng.bits_as<byte>();
				}
				have_last = true;
			} else{ // same millisecond OR clock went backwards.			
				// we re-use the same timestamp and just bump the random field.			
				increment_big_endian(last.random_bytes());
			}
			return last;
		}

		[[nodiscard]] constexpr static std::optional<ulid_t> from_string(std::string_view s) noexcept{
			if(s.size() != 26){ return std::nullopt; }
			std::array<std::uint64_t, 3> acc{}; // 192-bit accumulator: acc[0] = least significant 64 bits        
			for(char ch : s){
				auto v_opt = decode_crockford(ch);
				if(!v_opt){ return std::nullopt; }
				std::uint64_t carry = *v_opt; //carry = 0..31      
				for(auto& a : acc){
					const std::uint64_t new_carry = a >> (64 - 5);
					a = (a << 5) | carry;
					carry = new_carry;
				}
			}
			// Optional canonicality check: top 2 bits of 130-bit value should be zero.
			// Those are bits 128 and 129, which correspond to the lowest 2 bits of acc[2].        
			if((acc[2] & 0x3u) != 0){
				return std::nullopt;
			}
			ulid_t ulid{};
			write_big_endian<8>(acc[1], ulid.high_bytes()); // high bits are in acc[1] (bits 64..127)
			write_big_endian<8>(acc[0], ulid.low_bytes()); // low bits are in acc[0] (bits 0..63)
			return ulid;
		}

		// Note: from_readable_string() is an extension and not part of the ULID standard.
		// Expects "YYYYMMDDThhmmssmmmZrrrrrrrrrrrrrrrr" (35 chars).
		// Timestamp ends at Z, after which follows 16-char Crockford Base32 randomness (same as canonical ULID)
		[[nodiscard]] static std::optional<ulid_t> from_readable_string(std::string_view s){			
			if(s.size() != 35 || s[8] != 'T' || s[18] != 'Z'){
				return std::nullopt;
			}			

			auto parse_u32 = [](std::string_view s, std::size_t pos, std::size_t len) noexcept -> std::optional<unsigned>{
				unsigned v{}; 
				const char* first = s.data() + pos; 
				const char* last = first + len;
				auto rc = std::from_chars(first, last, v);
				if(rc.ec != std::errc{} || rc.ptr != last){
					return std::nullopt;
				}
				return v;
			};

			auto year_num = parse_u32(s, 0, 4); //0..3   year
			auto month_num = parse_u32(s, 4, 2); // 4..5   month
			auto day_num = parse_u32(s, 6, 2); //6..7   day
			auto hour_num = parse_u32(s, 9, 2); //8 = 'T', 9..10  hour
			auto minute_num = parse_u32(s, 11, 2); //11..12 minute
			auto second_num = parse_u32(s, 13, 2); //13..14 second
			auto millis_num = parse_u32(s, 15, 3); //15..17 millisecond

			if(!year_num || !month_num || !day_num || !hour_num || !minute_num || !second_num || !millis_num){
				return std::nullopt;
			}
			if(*month_num == 0 || *month_num > 12 || *day_num == 0 || *day_num > 31 || *hour_num > 23 || *minute_num > 59 || *second_num > 59 || *millis_num > 999){
				return std::nullopt; //failed basic range checks 
			}
			
			using namespace std::chrono;

			year_month_day ymd{
				year{static_cast<int>(*year_num)},
				month{*month_num},
				day{*day_num}
			};
			if(!ymd.ok()){ //calendar validation (feb 30 etc)
				return std::nullopt;
			}
						
			const sys_days day_tp{ymd}; // Start from midnight UTC on that date

			// Add the time-of-day components
			// The resulting time_point will have millisecond precision.
			const auto tp =
				day_tp
				+ hours{*hour_num}
				+ minutes{*minute_num}
				+ seconds{*second_num}
			+ milliseconds{*millis_num};
			
			const auto dur = tp.time_since_epoch();
			if(dur < milliseconds::zero()){
				return std::nullopt; // if someone passes a date before 1970-01-01, tp.time_since_epoch() is negative, which would wrap our uint64
			}
			
			const auto timestamp_ms = static_cast<std::uint64_t>(dur.count()); // milliseconds since Unix epoch
			ulid_t tmp{}; // empty ULID we will populate with the parsed timestamp
			write_big_endian<6>(timestamp_ms, tmp.timestamp_bytes()); // write the 48-bit timestamp into the first 6 bytes
			std::string canonical = tmp.to_string(); // encode this partial ULID into canonical 26-char Base32 form
			canonical.replace(10, 16, s.substr(19, 16)); // splice in the 16-char randomness from the readable input
			return ulid_t::from_string(canonical); // parse the completed canonical ULID
		}

		[[nodiscard]] constexpr static ulid_t from_bytes(std::span<const byte, 16> bytes) noexcept{
			ulid_t ulid{}; // manual copy to avoid pulling in <algorithm>. sorry for the crime scene!
			ulid.data = {bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
				bytes[8], bytes[9], bytes[10],bytes[11],bytes[12],bytes[13],bytes[14],bytes[15]
			};
			return ulid;
		}

		[[nodiscard]] constexpr std::string to_string() const{
			return encode_base32(data);
		}

		[[nodiscard]] constexpr explicit operator std::string() const{
			return to_string();
		}

		[[nodiscard]] constexpr std::array<byte, 16> to_bytes() const noexcept{
			return data;
		}

		[[nodiscard]] constexpr std::span<const byte, 16> as_bytes() const noexcept{
			return std::span<const byte, 16>(data);
		}

		// Note: to_readable_string() is an extension and not part of the ULID standard.
		// It rewrites the ULID timestamp: the first 10 Base32 chars are replaced with
		// a 19-character UTC datetime in compact ISO8601 form (YYYYMMDDThhmmssmmmZ).
		// The random 16-character suffix is preserved unchanged.
		// The result is a 35 character string with human readable timestamp. It retains 
		// millisecond precision and is lexicographically sortable in the same way as a normal ULID.	
		[[nodiscard]] std::string to_readable_string() const{
			using namespace std::chrono;
			using sys_ms = std::chrono::sys_time<std::chrono::milliseconds>;
			const auto tp = sys_ms{milliseconds{timestamp_ms()}};
			const auto ms = duration_cast<milliseconds>(tp.time_since_epoch());
			std::string out = std::format("{:%Y%m%dT%H%M%S}{}Z", //YYYYMMDDThhmmssnnnZ, 19 chars for date-time
				floor<std::chrono::seconds>(tp),
				std::format("{:03}", ms.count() % 1000) //handle milliseconds manually
			);
			const auto full = to_string();				// 26 chars: 10 ts + 16 random
			out.append(full.begin() + 10, full.end());  // append last 16 chars			
			return out; // Final form: "YYYYMMDDThhmmssmmmZrrrrrrrrrrrrrrrr" (35 chars)
		}

		[[nodiscard]] constexpr std::uint64_t timestamp_ms() const noexcept{
			std::uint64_t ts = 0;
			for(const byte b : timestamp_bytes()){
				ts = (ts << 8) | static_cast<std::uint64_t>(b);
			}
			return ts;
		}

		constexpr auto operator<=>(const ulid_t&) const = default;

	private:
		static constexpr char ENCODING[32] = {
			'0','1','2','3','4','5','6','7','8','9',
			'A','B','C','D','E','F','G','H','J','K',
			'M','N','P','Q','R','S','T','V','W','X',
			'Y','Z'
		};

		std::array<byte, 16> data{};

		constexpr std::span<byte, 6> timestamp_bytes() noexcept{
			return std::span<byte, 6>{data.begin(), 6};
		}
		constexpr std::span<const byte, 6> timestamp_bytes() const noexcept{
			return std::span<const byte, 6>{data.cbegin(), 6};
		}
		constexpr std::span<byte, 10> random_bytes() noexcept{
			return std::span<byte, 10>{data.begin() + 6, 10};
		}
		constexpr std::span<byte, 8> high_bytes() noexcept{
			return std::span<byte, 8>{data.begin(), 8};
		}
		constexpr std::span<byte, 8> low_bytes() noexcept{
			return std::span<byte, 8>{data.begin() + 8, 8};
		}

		static std::uint64_t now_ms() noexcept{
			using namespace std::chrono;
			return static_cast<std::uint64_t>(
				duration_cast<milliseconds>(
					system_clock::now().time_since_epoch()
				).count()
				);
		}

		constexpr static std::string encode_base32(std::span<const byte, 16> bytes){
			// interpret the 16 bytes as a single 128-bit big-endian integer: N = (hi << 64) | lo
			std::uint64_t hi = 0;
			for(int i = 0; i < 8; ++i){
				hi = (hi << 8) | bytes[i];
			}
			std::uint64_t lo = 0;
			for(int i = 8; i < 16; ++i){
				lo = (lo << 8) | bytes[i];
			}
			// we want 26 digits, each is 5 bits, covering bits 125..0 of the 128-bit value.
			std::string out(26, '0');
			for(int i = 0; i < 26; ++i){
				const auto digit = extract_digit(hi, lo, i);
				out[i] = ENCODING[digit];
			}
			return out;
		}

		constexpr static std::uint32_t extract_digit(std::uint64_t hi, std::uint64_t lo, int index) noexcept{
			const int shift = 125 - (5 * index); // bit index of MSB of this 5-bit digit
			if(shift == 0){ // Lowest 5 bits of N
				return static_cast<std::uint32_t>(lo & 0x1Fu);
			} else if(shift < 64){ // digit spans (possibly) across hi/lo; use the low 64 bits of (N >> shift)
				const std::uint64_t part = (hi << (64 - shift)) | (lo >> shift);
				return static_cast<std::uint32_t>(part & 0x1Fu);
			}
			return static_cast<std::uint32_t>((hi >> (shift - 64)) & 0x1Fu); // shift in [64, 125], only hits hi		 	
		}

		constexpr static void increment_big_endian(std::span<byte, 10> rand) noexcept{
			for(auto it = rand.rbegin(); it != rand.rend(); ++it){ // ULID is big-endian, so increment from least significant byte (back)
				if(*it != 0xFF){
					++(*it);
					return;
				}
				*it = 0;
			}
			// If we get here, we overflowed 80 bits (all 0xFF -> all 0x00).
			// Monotonicity within that millisecond is technically broken,
			// but if you're greedy enough to take 2^80 IDs/ms ... you deserve it. :P
		}

		//helper for extracting bytes in big-endian order
		template<std::size_t N>
		constexpr static void write_big_endian(std::uint64_t value, std::span<byte, N> out) noexcept{
			static_assert(N <= 8);
			for(std::size_t i = 0; i < N; ++i){
				out[i] = static_cast<byte>((value >> ((N - 1 - i) * 8)) & 0xFF);
			}
		}

		constexpr static std::optional<std::uint8_t> decode_crockford(char c) noexcept{
			using u = std::uint8_t;
			switch(c){
			case '0': case 'O': case 'o': return u{0};
			case '1': case 'I': case 'i': case 'L': case 'l': return u{1};
			case '2': return u{2};
			case '3': return u{3};
			case '4': return u{4};
			case '5': return u{5};
			case '6': return u{6};
			case '7': return u{7};
			case '8': return u{8};
			case '9': return u{9};
			case 'A': case 'a': return u{10};
			case 'B': case 'b': return u{11};
			case 'C': case 'c': return u{12};
			case 'D': case 'd': return u{13};
			case 'E': case 'e': return u{14};
			case 'F': case 'f': return u{15};
			case 'G': case 'g': return u{16};
			case 'H': case 'h': return u{17};
			case 'J': case 'j': return u{18};
			case 'K': case 'k': return u{19};
			case 'M': case 'm': return u{20};
			case 'N': case 'n': return u{21};
			case 'P': case 'p': return u{22};
			case 'Q': case 'q': return u{23};
			case 'R': case 'r': return u{24};
			case 'S': case 's': return u{25};
			case 'T': case 't': return u{26};
			case 'V': case 'v': return u{27};
			case 'W': case 'w': return u{28};
			case 'X': case 'x': return u{29};
			case 'Y': case 'y': return u{30};
			case 'Z': case 'z': return u{31};
			default:
				return std::nullopt;
			}
		}

		//helper for mixing in per-thread entropy, ensuring each thread has its own random stream
		static std::uint64_t salted_seed(std::uint64_t timestamp) noexcept{
			static thread_local std::uint64_t dummy{};
			auto salt = reinterpret_cast<std::uintptr_t>(&dummy);
			return timestamp ^ static_cast<std::uint64_t>(salt);
		}

	};



	inline std::ostream& operator<<(std::ostream& os, const ulid_t& id){
		return os << id.to_string();
	}
} //namespace ulid