/* Exact binary scaling for native integer aggregation of DOUBLE values.
 * The original BinarySum remains the fallback for wide exponent ranges. */
#pragma once

#include "engine/sort_storage.hpp"
#include "engine/statistics_math.hpp"

namespace parqit {

inline unsigned highest_bit(uint64_t value) {
#if defined(__GNUC__) || defined(__clang__)
    return 63U - static_cast<unsigned>(__builtin_clzll(value));
#else
    unsigned bit = 0;
    for (unsigned step : {32U,16U,8U,4U,2U,1U})
        if (value >> step) { value >>= step; bit += step; }
    return bit;
#endif
}

inline unsigned lowest_bit(uint64_t value) {
#if defined(__GNUC__) || defined(__clang__)
    return static_cast<unsigned>(__builtin_ctzll(value));
#else
    unsigned bit = 0;
    for (unsigned step : {32U,16U,8U,4U,2U,1U})
        if (!(value & ((uint64_t(1) << step) - 1))) { value >>= step; bit += step; }
    return bit;
#endif
}

inline unsigned ceil_log2(uint64_t value) {
    return value <= 1 ? 0 : highest_bit(value - 1) + 1;
}

struct Binary64Parts {
    uint64_t mantissa = 0;
    int exponent = 0;
    bool negative = false;
    bool decode(double value) {
        uint64_t bits;
        std::memcpy(&bits, &value, sizeof bits);
        const unsigned field = (bits >> 52) & 2047;
        // These values are excluded by the existing Stata numeric boundary.
        if (field >= 2046) return false;
        negative = (bits >> 63) != 0;
        mantissa = (bits & 0xfffffffffffffULL) | (field ? uint64_t(1) << 52 : 0);
        exponent = field ? static_cast<int>(field) - 1075 : -1074;
        return true;
    }
};

struct SumStorage {
    uint64_t n = 0;
    int minimum_lsb = 1023, maximum_top = -1075;
    uint32_t integer_mask = 7;
    bool invalid = false;

    void add(double value) {
        if (n == UINT64_MAX) { invalid = true; return; }
        ++n;
        Binary64Parts parts;
        if (!parts.decode(value)) { invalid = true; return; }
        if (integer_mask) integer_mask &= SortStorage::fits(value);
        if (!parts.mantissa) return;
        minimum_lsb = std::min(minimum_lsb, parts.exponent + static_cast<int>(lowest_bit(parts.mantissa)));
        maximum_top = std::max(maximum_top, parts.exponent + static_cast<int>(highest_bit(parts.mantissa)));
    }
    void combine(const SumStorage &other) {
        if (other.n > UINT64_MAX - n) { invalid = true; n = UINT64_MAX; }
        else n += other.n;
        invalid |= other.invalid;
        minimum_lsb = std::min(minimum_lsb, other.minimum_lsb);
        maximum_top = std::max(maximum_top, other.maximum_top);
        integer_mask &= other.integer_mask;
    }
    bool nonzero() const { return maximum_top >= minimum_lsb; }
    int exponent() const { return nonzero() ? minimum_lsb : 0; }
    unsigned width() const { return nonzero() ? static_cast<unsigned>(maximum_top - minimum_lsb + 1) : 0; }
    bool safe() const { return !invalid && width() + ceil_log2(n) <= 127; }
};

// Two unsigned words keep this portable to compilers without __int128.
struct ScaledInteger {
    uint64_t low = 0, high = 0;

    bool assign(double value, int unit_exponent) {
        if (unit_exponent < -1074 || unit_exponent > 1022) return false;
        Binary64Parts parts;
        if (!parts.decode(value)) return false;
        if (!parts.mantissa) { low = high = 0; return true; }
        uint64_t mantissa = parts.mantissa;
        int shift = parts.exponent - unit_exponent;
        if (shift < 0) {
            const unsigned right = static_cast<unsigned>(-shift);
            if (right >= 64 || (mantissa & ((uint64_t(1) << right) - 1))) return false;
            mantissa >>= right;
            shift = 0;
        }
        // The proof is deliberately strict: every magnitude is <2^127.
        if (shift + static_cast<int>(highest_bit(mantissa)) > 126) return false;
        low = shift < 64 ? mantissa << shift : 0;
        high = shift == 0 ? 0 : shift < 64 ? mantissa >> (64 - shift) : mantissa << (shift - 64);
        if (parts.negative) {
            low = ~low + 1;
            high = ~high + (low == 0);
        }
        return true;
    }
    statistics::Int256 integer() const {
        const uint64_t sign = high >> 63 ? UINT64_MAX : 0;
        return {{low, high, sign, sign}};
    }
};

inline double scaled_sum(const statistics::Int256 &sum, int exponent) {
    if (exponent < -1074 || exponent > 1022) return NAN;
    // For >53-bit integers, exponent>=-1074 guarantees a normal result.
    // Smaller integers convert exactly, including the subnormal result range.
    return std::scalbn(sum.to_double(), exponent);
}

inline double scaled_mean(const statistics::Int256 &sum, uint64_t n, int exponent) {
    if (!n || exponent < -1074 || exponent > 1022) return NAN;
    if (sum.is_zero()) return 0;
    const bool negative = sum.negative();
    const auto magnitude = negative ? sum.negated() : sum;
    if (magnitude.word[2] || magnitude.word[3]) return NAN;
    const unsigned top = magnitude.word[1] ? 64 + highest_bit(magnitude.word[1]) : highest_bit(magnitude.word[0]);
    if (static_cast<int>(top) + exponent - static_cast<int>(ceil_log2(n)) >= -1021)
        return std::scalbn(numeric::integer_ratio(sum, n, 0), exponent);

    // Near subnormal output, scaling an already rounded ratio can round twice.
    // Reconstruct exactly the old accumulator's signed units of2^-1074 and
    // reuse its remainder-aware mean. This branch has exponent<=-958, so its
    // shifted128-bit magnitude fits comfortably in34words.
    numeric::BinarySum exact;
    const unsigned shift = static_cast<unsigned>(exponent + 1074);
    const unsigned word = shift / 64, bits = shift % 64;
    for (unsigned i = 0; i < 2; ++i) {
        exact.word[word + i] |= magnitude.word[i] << bits;
        if (bits) exact.word[word + i + 1] |= magnitude.word[i] >> (64 - bits);
    }
    if (negative) {
        for (auto &value : exact.word) value = ~value;
        exact.add_word(0, 1, false);
    }
    return exact.mean(n);
}

} // namespace parqit
