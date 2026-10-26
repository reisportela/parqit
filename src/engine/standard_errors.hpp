#pragma once

#include "engine/statistics_math.hpp"

namespace parqit::statistics {

// These domain rules apply to every nonmissing observation, not to its mean.
// Counter products need at most 192 bits. Round only the final square root,
// using the same certified rational kernel as the existing moments.
struct BinomialError {
    uint64_t n = 0, successes = 0;
    bool invalid = false;

    void add(double x) {
        ++n;
        if (x == 1) ++successes;
        else if (x != 0) invalid = true;
    }
    void add(const Int256 &x, unsigned scale = 0) {
        ++n;
        if (x.is_zero()) return;
        if (!x.negative() && numeric::UInt320(x.word).compare(numeric::decimal_power(scale)) == 0)
            ++successes;
        else invalid = true;
    }
    void combine(const BinomialError &other) {
        n += other.n;
        successes += other.successes;
        invalid = invalid || other.invalid;
    }
    double result() const {
        if (!n || invalid) return NAN;
        exact_covariance::UInt<4> numerator(successes), denominator(n);
        numerator.multiply(n - successes);
        denominator.multiply(n);
        denominator.multiply(n);
        return exact_covariance::sqrt_ratio(numerator, denominator);
    }
};

struct PoissonError {
    Total total;
    bool invalid = false;

    void add(double x) {
        invalid = invalid || x < 0;
        total.add(x);
    }
    void add(const Int256 &x, unsigned scale = 0) {
        invalid = invalid || x.negative();
        total.add(x, scale);
    }
    void combine(const PoissonError &other) {
        total.combine(other.total);
        invalid = invalid || other.invalid;
    }
    double result() const {
        if (!total.n || invalid) return NAN;
        // A finite Stata double times a uint64 count needs fewer than 1088
        // integer bits. Decimal totals need fewer than 192; 18 limbs cover
        // both, without converting an exact total to binary64 first.
        using Integer = exact_covariance::UInt<18>;
        Integer count;
        if (!total.integral) {
            const auto &sum = total.sum.floating.word;
            constexpr unsigned fractional_bits = 1074;
            for (unsigned i = 0; i < count.word.size(); ++i) {
                const unsigned bit = fractional_bits + 64 * i;
                const unsigned index = bit / 64, offset = bit % 64;
                if (index >= sum.size()) break;
                count.word[i] = sum[index] >> offset;
                if (offset && index + 1 < sum.size()) count.word[i] |= sum[index + 1] << (64 - offset);
            }
            // Half ties upward, on the exact positive sum. Lower fractional
            // bits cannot change which side of one half the sum occupies.
            if ((sum[(fractional_bits - 1) / 64] >> ((fractional_bits - 1) % 64)) & 1)
                count.add_at(0, 1);
        } else {
            Integer remainder, unit(1);
            for (unsigned i = 0; i < total.sum.integer.word.size(); ++i)
                remainder.word[i] = total.sum.integer.word[i];
            if (!total.decimal_scale) count = remainder;
            else {
                for (unsigned i = 0; i < total.decimal_scale; ++i) unit.multiply(10);
                for (int bit = remainder.highest() - unit.highest(); bit >= 0; --bit) {
                    auto shifted = unit;
                    shifted.shift_left(static_cast<unsigned>(bit));
                    if (remainder.compare(shifted) >= 0) {
                        remainder.subtract(shifted);
                        count.inject(1, static_cast<unsigned>(bit));
                    }
                }
                remainder.multiply(2);
                if (remainder.compare(unit) >= 0) count.add_at(0, 1);
            }
        }
        Integer denominator(total.n);
        denominator.multiply(total.n);
        return exact_covariance::sqrt_ratio(count, denominator);
    }
};

static_assert(std::is_trivially_copyable<BinomialError>::value &&
              std::is_trivially_copyable<PoissonError>::value);

} // namespace parqit::statistics
