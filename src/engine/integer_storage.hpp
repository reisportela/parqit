/* Exact integer-storage proof for an explicitly requested native compress.
 * This does not demote double to float: native Stata compress does not either. */
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace parqit {
struct IntegerStorage {
    // Intersection of the byte, int and long ranges which fit every real cell.
    uint32_t mask = 7;
    bool definitely_double = false;
    bool invalid_date = false;

    void add(double value) {
        if (definitely_double) return;
        uint64_t bits;
        std::memcpy(&bits, &value, sizeof bits);
        const unsigned exponent = (bits >> 52) & 2047;
        // These values become missing in the existing Float64 fill. Inspect
        // bits so CPU denormal modes cannot turn a fractional input into zero.
        if (exponent >= 2046) return;
        if (exponent == 0 && (bits & 0xfffffffffffffULL)) {
            mask = 0;
            return;
        }
        if (value != std::trunc(value) || value < -2147483647. || value > 2147483620.) {
            mask = 0;
            definitely_double = true;
            return;
        }
        if (value < -127. || value > 100.) mask &= ~1U;
        if (value < -32767. || value > 32740.) mask &= ~2U;
    }

    void combine(const IntegerStorage &other) {
        mask &= other.mask;
        definitely_double |= other.definitely_double;
        invalid_date |= other.invalid_date;
    }
    // Bit 8 proves that native compress cannot demote a DOUBLE. A subnormal
    // alone remains undecided (0), so Stata handles its own storage semantics.
    uint32_t result() const { return invalid_date ? 0 : definitely_double ? 8 : mask; }
};
} // namespace parqit
