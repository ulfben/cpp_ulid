#pragma once
#include <algorithm>
#include <bit> // for std::bit_cast
#include <cassert>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <iterator>
#include <ranges>
#include <type_traits>
#ifdef _MSC_VER
#include <intrin.h>    // for _umul128, 64x64 multiplication
#endif

// This is an RNG interface that wraps around any engine that meets the RandomBitEngine concept.
// It provides useful functions for generating values, including integers, floating-point numbers, and colors
// as well as methods for Gaussian distribution, coin flips (with odds), picking from collections (index or element), etc.
// Source: https://github.com/ulfben/cpp_prngs/
// Demo is available on Compiler Explorer: https://compiler-explorer.com/z/nzK9joeYE
// Benchmarks:
   // Quick Bench for generating raw random values: https://quick-bench.com/q/vWdKKNz7kEyf6kQSNnUEFOX_4DI
   // Quick Bench for generating normalized floats: https://quick-bench.com/q/GARc3WSfZu4sdVeCAMSWWPMQwSE
   // Quick Bench for generating bounded values: https://quick-bench.com/q/WHEcW9iSV7I8qB_4eb1KWOvNZU0
namespace rnd {

    template<typename E>
    class Random final{
        E _e{}; //the underlying engine providing random bits. This class will turn those into useful values.

       //private helper for 128-bit multiplications
       // computes the high 64 bits of a 64×64-bit multiplication.
       // Used to implement Daniel Lemire’s "fastrange" trick: https://lemire.me/blog/2016/06/27/a-fast-alternative-to-the-modulo-reduction/   
       // which maps uniformly distributed `x` in [0, 2^BITS) to [0, bound) with negligible bias.     
        template<unsigned BITS>
        constexpr std::uint64_t mul_shift_high64(std::uint64_t x, std::uint64_t bound) noexcept{
#if defined(_MSC_VER)
         // MSVC doesn't support __uint128_t; use intrinsic instead
            std::uint64_t high{};
            (void) _umul128(x, bound, &high); // low 64 bits discarded
            return high; //return the high 64 bits
#elif defined(__SIZEOF_INT128__)
            return std::uint64_t((__uint128_t(x) * __uint128_t(bound)) >> BITS);
#else
            static_assert(false, "mul_shift_high64 requires either __uint128_t or MSVC _umul128");
#endif
        }

    public:
        using engine_type = E;
        using result_type = typename E::result_type;
        static constexpr unsigned BITS = std::numeric_limits<result_type>::digits;

        constexpr Random() noexcept = default; //the engine will default initialize
        explicit constexpr Random(engine_type engine) noexcept : _e(engine){}
        explicit constexpr Random(result_type seed_val) noexcept : _e(seed_val){};
        constexpr bool operator==(const Random& rhs) const noexcept = default;

        //access to the underlying engine for manual serialization, etc.
        constexpr const E& engine() const noexcept{ return _e; }
        constexpr E& engine() noexcept{ return _e; }

        //advance the random engine n steps. 
        //some engines (like PCG32) can do this faster than linear time
        constexpr void discard(unsigned long long n) noexcept{
            _e.discard(n);
        }

        constexpr void seed() noexcept{
            _e = E{};
        }
        constexpr void seed(result_type v) noexcept{
            _e.seed(v);
        }

        // returns a decorrelated, forked engine; advances this engine's state
        // use for parallel or independent streams use (think: task/thread-local randomness)
        constexpr Random<E> split() noexcept{
            return Random<E>(_e.split());
        }

        static constexpr auto min() noexcept{
            return E::min();
        }
        static constexpr auto max() noexcept{
            return E::max();
        }

        // Produces a random value in the range [min(), max())
        constexpr result_type next() noexcept{ return _e(); }
        constexpr result_type operator()() noexcept{ return next(); }

        // produces a random value in [0, bound) using Lemire's fastrange.
        // achieves very small bias without using rejection, and is much faster than naive modulo.
        constexpr result_type next(result_type bound) noexcept{
            assert(bound > 0 && "bound must be positive");
            result_type raw_value = next(); // raw_value is in the range [0, 2^BITS)

            if constexpr(BITS <= 32){ // for small engines, multiply into a 64-bit product             
                auto product = std::uint64_t(raw_value) * std::uint64_t(bound); // product is now in [0, bound * 2^BITS)
                auto result = result_type(product >> BITS); // equivalent to floor(product / 2^BITS)
                return result; // result is now in range [0, bound)
            } else if constexpr(BITS <= 64){
                // same logic, but use helper for 128-bit math, since __uint128_t isn't universally available
                return mul_shift_high64<BITS>(raw_value, bound);
            } else{ // fallback for hypothetical >64-bit engines. Naive modulo (slower, more bias)                     
                return bound > 0 ? raw_value % bound : bound; // avoid division by zero in release builds          
            }
        }
        constexpr result_type operator()(result_type bound) noexcept{ return next(bound); }

        // integer in [lo, hi)
        template<std::integral I>
        constexpr I between(I lo, I hi) noexcept{
            if(!(lo < hi)){
                assert(false && "between(lo, hi): inverted or empty range");
                return lo;
            }
            using U = std::make_unsigned_t<I>;
            U bound = U(hi) - U(lo);
            assert(bound <= E::max() && "between(lo, hi): range too large for this engine. Consider a 64-bit engine (xoshiro256ss, SmallFast64) or ensure hi–lo <= max()");
            auto safe_bound = static_cast<result_type>(bound);
            return lo + static_cast<I>(next(safe_bound));
        }

        // real in [lo, hi)
        template<std::floating_point F>
        constexpr F between(F lo, F hi) noexcept{
            return lo + (hi - lo) * normalized<F>();
        }

        // real in [0.0,1.0) using the "IQ float hack"
        //   see Iñigo Quilez, "sfrand": https://iquilezles.org/articles/sfrand/
        // Fast, branchless and, now, portable.
        template <std::floating_point F>
        constexpr F normalized() noexcept{
            static_assert(std::numeric_limits<F>::is_iec559, "normalized() requires IEEE 754 (IEC 559) floating point types.");
            using UInt = std::conditional_t<sizeof(F) == 4, uint32_t, uint64_t>;// Pick wide enough unsigned int type for F
            constexpr int mantissa_bits = std::numeric_limits<F>::digits - 1;   // Number of mantissa bits for F (e.g., 23 for float)
            constexpr UInt base = std::bit_cast<UInt>(F(1.0));                  // Bit pattern for F(1.0), i.e., exponent set, mantissa 0
            UInt mantissa = static_cast<UInt>(bits(mantissa_bits));             // Get random bits to fill the mantissa field
            UInt as_int = base | mantissa;                                      // Combine base (1.0) with random mantissa bits
            return std::bit_cast<F>(as_int) - F(1.0);                           // Convert bits to float/double, then subtract 1.0 to get [0,1)
        }

        // real in [-1.0,1.0) using the IQ float hack.      
        template <std::floating_point F>
        constexpr F signed_norm() noexcept{
            return F(2) * normalized<F>() - F(1); // scale to [0.0, 2.0), then shift to [-1.0, 1.0)
        }

        // boolean
        constexpr bool coin_flip() noexcept{ return bool(next() & 1); }

        // boolean with probability
        template<std::floating_point F>
        constexpr bool coin_flip(F probability) noexcept{
            return normalized<F>() < probability;
        }

        // 24-bit RGB packed as 0xRRGGBB
        constexpr std::uint32_t rgb8() noexcept{
            return static_cast<std::uint32_t>(bits<24>()); //same as next() & 0x00'FF'FF'FFu       
        }

        // 32-bit RGBA packed as 0xRRGGBBAA
        constexpr std::uint32_t rgba8() noexcept{
            return static_cast<std::uint32_t>(bits<32>()); //next() & 0xFF'FF'FF'FFu         
        }

        // pick an index in [0, size)
        template<std::ranges::sized_range R>
        constexpr auto index(const R& collection) noexcept{
            assert(!std::ranges::empty(collection) && "Random::index(): empty collection.");
            using idx_t = std::ranges::range_size_t<R>;
            return static_cast<idx_t>(
                between<idx_t>(0,
                    static_cast<idx_t>(std::ranges::size(collection))));
        }

        // get an iterator to a random element. Accepts const and non-const ranges
        template<std::ranges::forward_range R>
            requires std::ranges::sized_range<R>
        constexpr auto iterator(R&& collection) noexcept{
            assert(!std::ranges::empty(collection) && "Random::iterator(): empty collection");
            auto idx = index(collection); // index accepts const&
            auto it = std::ranges::begin(collection); // picks begin or cbegin for us
            std::advance(it, idx);
            return it;
        }

        //return a reference to a random element in a collection
        //accepts both const and non-const ranges
        template<std::ranges::forward_range R>
            requires std::ranges::sized_range<R>
        constexpr auto element(R&& collection) noexcept{
            return *iterator(std::forward<R>(collection));
        }

        template<std::floating_point F>
        constexpr F gaussian(F mean, F stddev) noexcept{
              // Based on the Central Limit Theorem; https://en.wikipedia.org/wiki/Central_limit_theorem
              // the Irwin–Hall distribution (sum of 12 U(0,1) has mean = 6, variance = 1).            
              // Subtract 6 and multiply by stddev to get an approximate N(mean, stddev) sample.
            F sum{};
            for(auto i = 0; i < 12; ++i){
                sum += normalized<F>();
            }
            return mean + (sum - F(6)) * stddev;
        }

        // Returns N random bits from the engine, with selectable return type.
        // Example: rng.bits<8, std::uint8_t>() or rng.bits<16, std::uint16_t>()
        template<unsigned N, class T = std::uint64_t>
        constexpr T bits() noexcept{
            static_assert(N > 0 && N <= BITS, "Can only extract [1 – std::numeric_limits<result_type>::digits] bits");
            static_assert(std::is_unsigned_v<T>, "bits<N, T> requires an unsigned T");
            static_assert(N <= std::numeric_limits<T>::digits, "Not enough bits in return type T to hold N bits");            
            const result_type x = next();
            if constexpr(N == BITS){
                return static_cast<T>(x);
            }
            const result_type mask = (result_type(1) << N) - 1;
            return static_cast<T>((x >> (BITS - N)) & mask); // take top N bits            
        }

        //convenience overload: fill a T with random bits
        template<class T>
        constexpr T bits_as() noexcept{
            static_assert(std::is_unsigned_v<T>, "bits<T>() requires an unsigned T");
            constexpr unsigned N = std::numeric_limits<T>::digits;
            return bits<N, T>();
        }

        // Returns N random bits from the engine, for use when n is not known at compile time
        constexpr std::uint64_t bits(unsigned n) noexcept{
            assert(n > 0 && n <= 64);
            switch(n){
            case 8:  return bits<8>();
            case 16: return bits<16>();
            case 24: return bits<24>();
            case 32: return bits<32>();
            case 64: return bits<64>();
            default:
                std::uint64_t value = 0;
                unsigned filled = 0;
                while(filled < n){
                    std::uint64_t r = next();
                    unsigned avail = std::min(BITS, n - filled);
                    value |= ((r & ((uint64_t(1) << avail) - 1)) << filled);
                    filled += avail;
                }
                return value;
            }
        }
    };
} //namespace rnd