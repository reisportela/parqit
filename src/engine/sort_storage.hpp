/* Lossless integer widths or reversible bit encodings for sort keys. Unlike
 * native compress, the proof must retain the sign of zero and every original
 * bit pattern, leaving unsupported values on the original sort path. */
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace parqit {
struct SortStorage {
    uint32_t mask = 7; // signed TINYINT, SMALLINT, INTEGER
    bool reversible = true;

    static bool bits_safe(uint64_t bits) {
        const unsigned exponent = (bits >> 52) & 2047;
        return exponent < 2046 && bits != (uint64_t(1) << 63) &&
               !(exponent == 0 && (bits & 0xfffffffffffffULL));
    }
    static uint64_t ordered_bits(uint64_t bits) {
        return bits >> 63 ? ~bits : bits ^ (uint64_t(1) << 63);
    }
    static uint64_t original_bits(uint64_t key) {
        return key >> 63 ? key ^ (uint64_t(1) << 63) : ~key;
    }

    static uint32_t fits(double value) {
        uint64_t bits;
        std::memcpy(&bits, &value, sizeof bits);
        const unsigned exponent = (bits >> 52) & 2047;
        if (bits == (uint64_t(1) << 63) || exponent == 2047 ||
            (exponent == 0 && (bits & 0xfffffffffffffULL))) return 0;
        if (value < -2147483648. || value > 2147483647. ||
            value != std::trunc(value)) return 0;
        uint32_t result = 4;
        if (value >= -32768. && value <= 32767.) result |= 2;
        if (value >= -128. && value <= 127.) result |= 1;
        return result;
    }
    void add(double value) {
        if (!mask && !reversible) return;
        uint64_t bits;
        std::memcpy(&bits, &value, sizeof bits);
        reversible &= bits_safe(bits);
        if (mask) mask &= fits(value);
    }
    void combine(const SortStorage &other) { mask &= other.mask; reversible &= other.reversible; }
    uint32_t result() const { return mask; }
};
} // namespace parqit
