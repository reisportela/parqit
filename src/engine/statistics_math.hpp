/* Bounded, mergeable numerical states. No engine or Stata dependency. */
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cfloat>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <type_traits>

#include "engine/numeric_exact.hpp"

#if defined(__FAST_MATH__) || (defined(_M_FP_FAST) && _M_FP_FAST)
#error "parqit statistics require IEEE arithmetic, without fast-math reassociation"
#endif

namespace parqit::statistics {
static_assert(FLT_EVAL_METHOD == 0, "parqit statistics require binary64 evaluation");

struct DD {
    double hi = 0, lo = 0;
    DD() = default;
    DD(double value) : hi(value) {}
    DD(double high, double low) : hi(high), lo(low) {}
    double value() const { return hi + lo; }
};

inline DD two_sum(double a, double b) {
    const double s = a + b;
    return {s, std::fabs(a) >= std::fabs(b) ? (a - s) + b : (b - s) + a};
}
// Renormalization of non-overlapping DD components (Briggs-Kahan addition).
inline DD quick_two_sum(double a, double b) {
    const double s = a + b;
    return {s, b - (s - a)};
}
inline DD operator-(DD a) { return {-a.hi, -a.lo}; }
inline DD operator+(DD a, DD b) {
    if (b.lo == 0) {
        const DD s = two_sum(a.hi, b.hi);
        return quick_two_sum(s.hi, s.lo + a.lo);
    }
    if (a.lo == 0) {
        const DD s = two_sum(a.hi, b.hi);
        return quick_two_sum(s.hi, s.lo + b.lo);
    }
    DD s = two_sum(a.hi, b.hi), t = two_sum(a.lo, b.lo);
    s = quick_two_sum(s.hi, s.lo + t.hi);
    return quick_two_sum(s.hi, s.lo + t.lo);
}
inline DD operator-(DD a, DD b) { return a + (-b); }
inline DD operator*(DD a, DD b) {
    const double p = a.hi * b.hi;
    const double e = std::fma(a.hi, b.hi, -p);
    if (a.lo == 0 && b.lo == 0) return {p, e};
    DD out = two_sum(p, e + (a.hi * b.lo + a.lo * b.hi));
    return out + DD(a.lo * b.lo);
}
inline DD operator/(DD a, DD b) {
    const double q = a.hi / b.hi;
    const DD residual = a - b * DD(q);
    const double q2 = residual.hi / b.hi;
    const DD residual2 = residual - b * DD(q2);
    return DD(q) + (DD(q2) + DD(residual2.value() / b.hi));
}
inline DD scaled(DD value, int exponent) {
    if (exponent == 0) return value;
    if (exponent >= -1074 && exponent <= 1023) {
        const uint64_t bits = exponent >= -1022 ? static_cast<uint64_t>(exponent + 1023) << 52
                                                : uint64_t(1) << (exponent + 1074);
        double factor;
        std::memcpy(&factor, &bits, sizeof factor);
        return {value.hi * factor, value.lo * factor};
    }
    return {std::scalbn(value.hi, exponent), std::scalbn(value.lo, exponent)};
}
inline bool zero(DD value) { return value.hi == 0 && value.lo == 0; }

inline int binary_exponent(double value) {
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    const int exponent = static_cast<int>((bits >> 52) & 2047);
    return exponent ? exponent - 1023 : std::ilogb(std::fabs(value));
}

/* Signed two's-complement arithmetic. 128-bit inputs times a 64-bit count
 * fit in 193 signed bits; four limbs preserve integer/decimal cancellation. */
struct Int256 {
    std::array<uint64_t, 4> word{};

    static Int256 signed128(uint64_t low, int64_t high) {
        return {{low, static_cast<uint64_t>(high), high < 0 ? UINT64_MAX : 0,
                 high < 0 ? UINT64_MAX : 0}};
    }
    static Int256 unsigned128(uint64_t low, uint64_t high = 0) {
        return {{low, high, 0, 0}};
    }
    static Int256 signed64(int64_t value) {
        return signed128(static_cast<uint64_t>(value), value < 0 ? -1 : 0);
    }
    bool negative() const { return (word[3] >> 63) != 0; }
    bool is_zero() const { return !(word[0] | word[1] | word[2] | word[3]); }
    void add(const Int256 &other) {
        uint64_t carry = 0;
        for (size_t i = 0; i < word.size(); ++i) {
            const uint64_t s = word[i] + other.word[i];
            const uint64_t c = s < word[i];
            const uint64_t t = s + carry;
            carry = c | (t < s);
            word[i] = t;
        }
    }
    Int256 negated() const {
        Int256 out = *this;
        for (auto &w : out.word) w = ~w;
        out.add(unsigned128(1));
        return out;
    }
    Int256 operator-(const Int256 &other) const {
        Int256 out = *this;
        out.add(other.negated());
        return out;
    }
    Int256 multiplied(uint64_t count) const {
        Int256 part = *this, out;
        while (count) {
            if (count & 1) out.add(part);
            count >>= 1;
            if (count) part.add(part);
        }
        return out;
    }
    int compare(const Int256 &other) const {
        if (negative() != other.negative()) return negative() ? -1 : 1;
        for (int i = 3; i >= 0; --i)
            if (word[i] != other.word[i]) return word[i] < other.word[i] ? -1 : 1;
        return 0;
    }
    uint64_t shifted_word(unsigned shift) const {
        const unsigned i = shift / 64, offset = shift % 64;
        if (i >= 4) return 0;
        uint64_t out = word[i] >> offset;
        if (offset && i + 1 < 4) out |= word[i + 1] << (64 - offset);
        return out;
    }
    bool any_below(unsigned bits) const {
        for (unsigned i = 0; i < bits / 64; ++i)
            if (word[i]) return true;
        const unsigned tail = bits % 64;
        return tail && (word[bits / 64] & ((uint64_t(1) << tail) - 1));
    }
    double to_double() const {
        const bool neg = negative();
        const Int256 mag = neg ? negated() : *this;
        int i = 3;
        while (i >= 0 && mag.word[static_cast<size_t>(i)] == 0) --i;
        if (i < 0) return 0;
        uint64_t top = mag.word[static_cast<size_t>(i)];
        unsigned bits = static_cast<unsigned>(i) * 64;
        for (unsigned step : {32U, 16U, 8U, 4U, 2U, 1U}) {
            if (top >> step) { bits += step; top >>= step; }
        }
        ++bits;
        const unsigned shift = bits > 53 ? bits - 53 : 0;
        uint64_t significand = mag.shifted_word(shift);
        if (shift && (mag.shifted_word(shift - 1) & 1) &&
            ((significand & 1) || mag.any_below(shift - 1))) ++significand;
        const double value = std::scalbn(static_cast<double>(significand), shift);
        return neg ? -value : value;
    }
    static Int256 from_integral_double(double value) {
        if (!std::isfinite(value) || value == 0) return {};
        uint64_t bits;
        std::memcpy(&bits, &value, sizeof bits);
        const bool neg = (bits >> 63) != 0;
        const int exponent = static_cast<int>((bits >> 52) & 2047) - 1023 - 52;
        uint64_t significand = (bits & ((uint64_t(1) << 52) - 1)) | (uint64_t(1) << 52);
        Int256 out;
        if (exponent < -63 || exponent >= 256) return out;
        if (exponent < 0) out.word[0] = significand >> -exponent;
        else {
            const unsigned index = static_cast<unsigned>(exponent) / 64;
            const unsigned offset = static_cast<unsigned>(exponent) % 64;
            out.word[index] = significand << offset;
            if (offset && index + 1 < 4) out.word[index + 1] = significand >> (64 - offset);
        }
        return neg ? out.negated() : out;
    }
    DD to_dd() const {
        const Int256 mag = negative() ? negated() : *this;
        if (!(mag.word[1] | mag.word[2] | mag.word[3]) && mag.word[0] <= (uint64_t(1) << 53))
            return DD(negative() ? -static_cast<double>(mag.word[0]) : static_cast<double>(mag.word[0]));
        const double high = to_double();
        return {high, (*this - from_integral_double(high)).to_double()};
    }
};

inline DD count_value(uint64_t n) {
    return n <= (uint64_t(1) << 53) ? DD(static_cast<double>(n)) : Int256::unsigned128(n).to_dd();
}
inline DD decimal_factor(unsigned scale) {
    DD out(1);
    for (unsigned i = 0; i < scale; ++i) out = out * DD(10);
    return out;
}

using FloatSum = numeric::BinarySum;

struct Total {
    union Sum {
        FloatSum floating;
        Int256 integer;
        Sum() : integer{} {}
    } sum;
    uint64_t n = 0;
    bool integral = false;
    unsigned decimal_scale = 0;

    void add(double x) {
        if (n++ == 0) { integral = false; new (&sum.floating) FloatSum{}; }
        sum.floating.add(x);
    }
    void add(const Int256 &x, unsigned scale = 0) {
        if (n++ == 0) { integral = true; decimal_scale = scale; new (&sum.integer) Int256{}; }
        sum.integer.add(x);
    }
    void combine(const Total &other) {
        if (!other.n) return;
        if (!n) { *this = other; return; }
        if (integral) sum.integer.add(other.sum.integer);
        else sum.floating.combine(other.sum.floating);
        n += other.n;
    }
    double value() const {
        if (!n) return 0;
        return integral ? numeric::integer_ratio(sum.integer, 1, decimal_scale)
                        : sum.floating.sum();
    }
    double mean() const {
        if (!n) return NAN;
        return integral ? numeric::integer_ratio(sum.integer, n, decimal_scale)
                        : sum.floating.mean(n);
    }
};

struct Pivot {
    union Value {
        double floating;
        struct { uint64_t low, high; } integer;
        Value() : integer{} {}
    } value;
    bool negative = false;

    void set(double x) { value.floating = x; }
    void set(const Int256 &x) {
        value.integer = {x.word[0], x.word[1]};
        negative = x.negative();
    }
    Int256 integer() const {
        return {{value.integer.low, value.integer.high,
                 negative ? UINT64_MAX : 0, negative ? UINT64_MAX : 0}};
    }
    DD difference(double x) const { return two_sum(x, -value.floating); }
    DD difference(const Int256 &x) const { return (x - integer()).to_dd(); }
    DD difference(const Pivot &x, bool integral) const {
        return integral ? (x.integer() - integer()).to_dd()
                        : difference(x.value.floating);
    }
};

template <unsigned Order> struct ApproximateMoments {
    static_assert(Order == 2 || Order == 4, "moments order must be 2 or 4");
    Total total;
    Pivot pivot;
    std::array<DD, Order> raw{};
    int exponent = 0;
    bool varying = false;

    void rescale(int target) {
        if (varying)
            for (unsigned k = 0; k < Order; ++k)
                raw[k] = scaled(raw[k], static_cast<int>(k + 1) * (exponent - target));
        exponent = target;
        varying = true;
    }
    void add_deviation(DD delta) {
        if (zero(delta)) return;
        const int e = binary_exponent(delta.hi);
        if (!varying || e > exponent) rescale(e);
        DD d = scaled(delta, -exponent), power = d;
        for (unsigned k = 0; k < Order; ++k) {
            raw[k] = raw[k] + power;
            if (k + 1 < Order) power = power * d;
        }
    }
    void add(double x) {
        if (!total.n) pivot.set(x);
        total.add(x);
        add_deviation(pivot.difference(x));
    }
    void add(const Int256 &x, unsigned scale = 0) {
        if (!total.n) pivot.set(x);
        total.add(x, scale);
        add_deviation(pivot.difference(x));
    }
    void combine(const ApproximateMoments &other) {
        if (!other.total.n) return;
        if (!total.n) { *this = other; return; }
        DD delta = pivot.difference(other.pivot, total.integral);
        int target = varying ? exponent : (other.varying ? other.exponent : 0);
        if (other.varying) target = std::max(target, other.exponent);
        if (!zero(delta)) {
            const int e = binary_exponent(delta.hi);
            if (!varying && !other.varying) target = e;
            else target = std::max(target, e);
        }
        if (varying || other.varying || !zero(delta)) {
            rescale(target);
            const DD d = scaled(delta, -target), n = count_value(other.total.n);
            const DD s1 = scaled(other.raw[0], other.exponent - target);
            const DD s2 = scaled(other.raw[1], 2 * (other.exponent - target));
            raw[0] = raw[0] + (s1 + n * d);
            raw[1] = raw[1] + (s2 + DD(2) * d * s1 + n * d * d);
            if constexpr (Order == 4) {
                const DD s3 = scaled(other.raw[2], 3 * (other.exponent - target));
                const DD s4 = scaled(other.raw[3], 4 * (other.exponent - target));
                const DD d2 = d * d, d3 = d2 * d;
                raw[2] = raw[2] + (s3 + DD(3) * d * s2 + DD(3) * d2 * s1 + n * d3);
                raw[3] = raw[3] + (s4 + DD(4) * d * s3 + DD(6) * d2 * s2 +
                                   DD(4) * d3 * s1 + n * d2 * d2);
            }
        }
        total.combine(other.total);
    }
    DD centered2() const {
        if (!varying) return DD(0);
        return raw[1] - raw[0] * (raw[0] / count_value(total.n));
    }
    double sd() const {
        if (total.n < 2) return NAN;
        const double m2 = centered2().value();
        if (m2 < 0) return NAN;
        const double sigma = std::scalbn(std::sqrt(m2 / static_cast<double>(total.n - 1)), exponent);
        return total.integral ? (DD(sigma) / decimal_factor(total.decimal_scale)).value() : sigma;
    }
    double variance() const {
        if (total.n < 2) return NAN;
        const double m2 = centered2().value();
        if (m2 < 0) return NAN;
        double var = std::scalbn(m2 / static_cast<double>(total.n - 1), 2 * exponent);
        if (total.integral) {
            const DD factor = decimal_factor(total.decimal_scale);
            var = (DD(var) / (factor * factor)).value();
        }
        return var;
    }
    double skewness() const {
        static_assert(Order == 4, "shape requires four moments");
        if (!total.n) return NAN;
        const DD mu = raw[0] / count_value(total.n);
        const double m2 = centered2().value();
        if (m2 <= 0) return NAN;
        const DD m3 = raw[2] - DD(3) * mu * raw[1] + DD(2) * mu * mu * raw[0];
        return (m3.value() / m2) * std::sqrt(static_cast<double>(total.n) / m2);
    }
    double kurtosis() const {
        static_assert(Order == 4, "shape requires four moments");
        if (!total.n) return NAN;
        const DD mu = raw[0] / count_value(total.n), mu2 = mu * mu;
        const double m2 = centered2().value();
        if (m2 <= 0) return NAN;
        const DD m4 = raw[3] - DD(4) * mu * raw[2] + DD(6) * mu2 * raw[1] -
                      DD(3) * mu2 * mu * raw[0];
        return (m4.value() / m2) * (static_cast<double>(total.n) / m2);
    }
};

struct ApproximateCorrelation {
    Pivot x, y;
    uint64_t n = 0;
    DD sx, sy, sxx, syy, sxy;
    int ex = 0, ey = 0;
    bool vx = false, vy = false, integer_x = false, integer_y = false;

    void rescale(int nx, int ny) {
        sx = scaled(sx, ex - nx);
        sy = scaled(sy, ey - ny);
        sxx = scaled(sxx, 2 * (ex - nx));
        syy = scaled(syy, 2 * (ey - ny));
        sxy = scaled(sxy, ex - nx + ey - ny);
        ex = nx; ey = ny;
    }
    template <class X, class Y> void add(const X &a, const Y &b) {
        if (!n) {
            x.set(a); y.set(b);
            integer_x = std::is_same<X, Int256>::value;
            integer_y = std::is_same<Y, Int256>::value;
        }
        DD dx = x.difference(a), dy = y.difference(b);
        int nx = ex, ny = ey;
        if (!zero(dx)) {
            const int e = binary_exponent(dx.hi);
            nx = vx ? std::max(ex, e) : e;
            vx = true;
        }
        if (!zero(dy)) {
            const int e = binary_exponent(dy.hi);
            ny = vy ? std::max(ey, e) : e;
            vy = true;
        }
        if (nx != ex || ny != ey) rescale(nx, ny);
        dx = scaled(dx, -ex); dy = scaled(dy, -ey);
        sx = sx + dx; sy = sy + dy;
        sxx = sxx + dx * dx; syy = syy + dy * dy;
        sxy = sxy + dx * dy;
        ++n;
    }
    void combine(const ApproximateCorrelation &other) {
        if (!other.n) return;
        if (!n) { *this = other; return; }
        DD dx = x.difference(other.x, integer_x), dy = y.difference(other.y, integer_y);
        int nx = vx ? ex : (other.vx ? other.ex : 0);
        int ny = vy ? ey : (other.vy ? other.ey : 0);
        if (other.vx) nx = std::max(nx, other.ex);
        if (other.vy) ny = std::max(ny, other.ey);
        if (!zero(dx)) nx = (vx || other.vx) ? std::max(nx, binary_exponent(dx.hi)) : binary_exponent(dx.hi);
        if (!zero(dy)) ny = (vy || other.vy) ? std::max(ny, binary_exponent(dy.hi)) : binary_exponent(dy.hi);
        rescale(nx, ny);
        dx = scaled(dx, -nx); dy = scaled(dy, -ny);
        const DD nn = count_value(other.n);
        const DD bx = scaled(other.sx, other.ex - nx), by = scaled(other.sy, other.ey - ny);
        sx = sx + (bx + nn * dx);
        sy = sy + (by + nn * dy);
        sxx = sxx + (scaled(other.sxx, 2 * (other.ex - nx)) + DD(2) * dx * bx + nn * dx * dx);
        syy = syy + (scaled(other.syy, 2 * (other.ey - ny)) + DD(2) * dy * by + nn * dy * dy);
        sxy = sxy + (scaled(other.sxy, other.ex - nx + other.ey - ny) +
                     dx * by + dy * bx + nn * dx * dy);
        vx = vx || other.vx || !zero(dx);
        vy = vy || other.vy || !zero(dy);
        n += other.n;
    }
    double value() const {
        if (n < 2 || !vx || !vy) return NAN;
        const DD nn = count_value(n);
        const DD xx = sxx - sx * (sx / nn), yy = syy - sy * (sy / nn);
        const DD xy = sxy - sx * (sy / nn);
        if (xx.value() <= 0 || yy.value() <= 0) return NAN;
        if (n == 2) return std::copysign(1., xy.value());
        if (xx.hi == yy.hi && xx.lo == yy.lo) {
            if (xy.hi == xx.hi && xy.lo == xx.lo) return 1;
            if (xy.hi == -xx.hi && xy.lo == -xx.lo) return -1;
        }
        const double rho = xy.value() / (std::sqrt(xx.value()) * std::sqrt(yy.value()));
        if (!std::isfinite(rho)) return NAN;
        if (std::fabs(rho) <= 1) return rho;
        if (std::fabs(rho) <= 1 + 32 * std::numeric_limits<double>::epsilon())
            return std::copysign(1., rho);
        return NAN;
    }
};

} // namespace parqit::statistics

#include "engine/covariance_math.hpp"
#include "engine/moments_math.hpp"
namespace parqit::statistics {
using Correlation = exact_covariance::Correlation;
template<unsigned Order> using Moments = exact_covariance::Moments<Order,Total>;
static_assert(std::is_trivially_copyable<Moments<2>>::value && std::is_trivially_copyable<Moments<4>>::value);
static_assert(sizeof(Moments<2>)==832 && sizeof(Moments<4>)==2736);
}
