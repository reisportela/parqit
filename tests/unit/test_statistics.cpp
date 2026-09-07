#include "doctest.h"
#include "engine/statistics_math.hpp"

#include <random>

using namespace parqit::statistics;

TEST_CASE("exact moments preserve tiny shape and do not infer constancy from rounded sd") {
    Moments<4> tiny, shape, tie;
    for (double value : {0., 0x1p-1074, 0x1p-1074, 0x1p-1074, 0x1p-1074}) tiny.add(value);
    const auto result = tiny.result();
    CHECK_FALSE(result.invalid);
    CHECK(result.sd == 0);
    CHECK(result.variance == 0);
    CHECK(result.skewness == -1.5);
    CHECK(result.kurtosis == 3.25);
    for (double value : {-1e200, 1., 1e200}) shape.add(value);
    CHECK(shape.skewness() == -0x1.dffd96714e203p-665);
    CHECK(shape.kurtosis() == 1.5);
    for (int64_t value : {0LL, 9007199254740993LL, 18014398509481986LL})
        tie.add(Int256::signed64(value));
    CHECK(tie.sd() == 0x1p53);
    CHECK(tie.variance() == 0x1.0000000000001p106);
}

TEST_CASE("exact covariance retains the complement of a correctly rounded rho of one") {
    Correlation ordinary, extreme;
    ordinary.add(0., 0.); ordinary.add(1., 1.); ordinary.add(2., 2.+1e-12);
    extreme.add(-1e200, -1e200); extreme.add(1., 2.); extreme.add(1e200, 1e200);
    CHECK(ordinary.value() == 1);
    CHECK(extreme.value() == 1);
    CHECK(ordinary.result().sine == 0x1.450c56efe0508p-42);
    CHECK(extreme.result().sine == 0x1.c48a19c4d5199p-666);
    CHECK_FALSE(ordinary.result().perfect);
    CHECK_FALSE(extreme.result().perfect);
}

TEST_CASE("exact accumulation retains three exponent bands and weighted subnormals") {
    Total total;
    for (double x : {1e100, 1e50, 1., -1e100, -1e50}) total.add(x);
    CHECK(total.value() == 1);
    CHECK(total.mean() == .2);
    parqit::numeric::BinarySum scaled, repeated;
    for (double x : {8e307, 0x1p-1074, -8e307, -1e50, 1e50}) {
        scaled.add_multiple(x, 1001);
        for (unsigned i = 0; i < 1001; ++i) repeated.add(x);
    }
    CHECK(scaled.word == repeated.word);
    CHECK(scaled.sum() == std::scalbn(1001., -1074));
    CHECK(scaled.mean(1001) == 0x1p-1074);
    Total integer;
    integer.add(Int256::unsigned128(0, 1ULL << 56));
    integer.add(Int256::unsigned128(1, 8));
    CHECK(integer.value() == 0x1.0000000000001p120);
    CHECK(integer.mean() == 0x1.0000000000001p119);
}

TEST_CASE("sample size rounds the exact global proportion once") {
    using parqit::numeric::sample_count;
    CHECK(sample_count(1234567, .00015) == 2);
    CHECK(sample_count(1234567, .0015) == 19);
    CHECK(sample_count(15, 10) == 2);
    CHECK(sample_count(5, 10) == 1);
    CHECK(sample_count(UINT64_MAX, 50) == (1ULL << 63));
    CHECK(sample_count(UINT64_MAX, 0x1p-1074) == 0);
    CHECK(sample_count(UINT64_MAX, 100) == UINT64_MAX);
}

TEST_CASE("DD renormalization preserves exact 96-bit integer cancellation") {
    std::mt19937_64 random(20260906);
    for (unsigned i = 0; i < 2048; ++i) {
        Int256 a = Int256::unsigned128(random(), random() & 0xffffffffU);
        Int256 b = Int256::unsigned128(random(), random() & 0xffffffffU);
        if (i & 1) a = a.negated();
        if (i & 2) b = b.negated();
        Int256 sum = a;
        sum.add(b);
        const DD expected = sum.to_dd(), actual = a.to_dd() + b.to_dd();
        CHECK(actual.hi == expected.hi);
        CHECK(actual.lo == expected.lo);
        const DD cancelled = a.to_dd() + (-a.to_dd());
        CHECK(zero(cancelled));
    }
}

TEST_CASE("accurate integer and floating totals preserve cancellation and range") {
    Total t;
    for (double x : {1e16, 1., -1e16, 1e16, 1., -1e16}) t.add(x);
    CHECK(t.value() == 2);
    CHECK(t.mean() == 1. / 3);
    Total integer;
    integer.add(Int256::signed64(9007199254740993LL));
    integer.add(Int256::signed64(-9007199254740992LL));
    CHECK(integer.value() == 1);
    CHECK(integer.mean() == .5);
    Total decimal;
    decimal.add(Int256::signed64(10000000000000000LL), 2);
    decimal.add(Int256::signed64(1), 2);
    decimal.add(Int256::signed64(-10000000000000000LL), 2);
    CHECK(decimal.value() == .01);
    Total huge;
    const auto maximum = Int256::signed128(UINT64_MAX, INT64_MAX);
    huge.add(maximum);
    huge.add(maximum);
    CHECK(huge.value() == std::scalbn(1., 128));
    CHECK(huge.mean() == std::scalbn(1., 127));
    Total constant;
    for (int i = 0; i < 100; ++i) constant.add(8e307);
    CHECK(constant.mean() == 8e307);
    CHECK(std::isinf(constant.value()));
}

TEST_CASE("centered scaled moments survive offsets and exponent extremes") {
    Moments<4> offset;
    for (int i = 0; i < 4; ++i) offset.add(1e16 + 2. * i);
    CHECK(offset.variance() == doctest::Approx(20. / 3).epsilon(1e-15));
    CHECK(std::fabs(offset.skewness()) < 1e-15);
    CHECK(offset.kurtosis() == doctest::Approx(1.64).epsilon(1e-15));
    Moments<4> fine;
    for (int i = 0; i < 8; ++i) fine.add(1e12 + .001 * i);
    CHECK(fine.variance() == doctest::Approx(5.9814857585089547e-6).epsilon(1e-14));
    CHECK(fine.skewness() == doctest::Approx(-.019093391176168285).epsilon(1e-13));
    for (int exponent : {-200, -108, -80, 0, 80, 100, 200, 300}) {
        CAPTURE(exponent);
        const double scale = std::pow(10., exponent);
        Moments<4> m;
        for (double x : {1., 2., 3., 10.}) m.add(x * scale);
        CHECK(m.sd() / scale == doctest::Approx(std::sqrt(50. / 3)).epsilon(1e-14));
        CHECK(m.skewness() == doctest::Approx(1.0182337649086284).epsilon(1e-14));
        CHECK(m.kurtosis() == doctest::Approx(2.2304).epsilon(1e-14));
    }
}

TEST_CASE("moment merges preserve per-partition pivots and exact integer inputs") {
    Moments<4> serial, merged;
    for (int part = 0; part < 31; ++part) {
        Moments<4> block;
        for (int i = 0; i < 97; ++i) {
            const double x = 1e12 + .001 * ((part * 97 + i) % 8);
            serial.add(x);
            block.add(x);
        }
        merged.combine(block);
    }
    CHECK(merged.variance() == doctest::Approx(serial.variance()).epsilon(1e-14));
    CHECK(merged.skewness() == doctest::Approx(serial.skewness()).epsilon(1e-13));
    CHECK(merged.kurtosis() == doctest::Approx(serial.kurtosis()).epsilon(1e-14));
    Moments<4> wide;
    auto value = Int256::signed128(UINT64_MAX - 3, INT64_MAX);
    for (int i = 0; i < 4; ++i) {
        wide.add(value);
        value.add(Int256::unsigned128(1));
    }
    CHECK(wide.variance() == doctest::Approx(5. / 3).epsilon(1e-15));
    CHECK(std::fabs(wide.skewness()) < 1e-15);
    CHECK(wide.kurtosis() == doctest::Approx(1.64).epsilon(1e-15));
    Moments<4> a, b;
    a.add(1e-300); b.add(2e-300);
    a.combine(b);
    CHECK(a.sd() / 1e-300 == doctest::Approx(std::sqrt(.5)).epsilon(1e-14));
}

TEST_CASE("correlation is symmetric and range-safe on shifted and scaled inputs") {
    Correlation a, b, reverse, negative;
    for (int i = 0; i < 4; ++i) {
        a.add(1e16 + 2. * i, static_cast<double>(i + 1));
        b.add(static_cast<double>(i + 1), 1e16 + 2. * i);
        reverse.add(1e16 + 2. * (3 - i), static_cast<double>(4 - i));
        negative.add(1e16 + 2. * i, -static_cast<double>(i + 1));
    }
    CHECK(a.value() == 1);
    CHECK(b.value() == 1);
    CHECK(reverse.value() == 1);
    CHECK(negative.value() == -1);
    for (double xs : {1e-300, 1., 1e300}) {
        for (double ys : {1e-300, 1., 1e300}) {
            Correlation serial, merged;
            for (int i = 1; i <= 128; ++i) {
                Correlation part;
                serial.add(xs * i, ys * i);
                part.add(xs * i, ys * i);
                merged.combine(part);
            }
            CHECK(serial.value() == doctest::Approx(1.).epsilon(1e-14));
            CHECK(merged.value() == doctest::Approx(1.).epsilon(1e-14));
        }
    }
}

TEST_CASE("integer block merges decimal scaling and count rounding are explicit") {
    Moments<4> integer, reference;
    for (int block = 0; block < 4; ++block) {
        Moments<4> part;
        for (int i = 0; i < 4; ++i) {
            part.add(Int256::signed64((int64_t(1) << 62) + 2 * block + 2 * i));
            reference.add(1e15 + 2 * block + 2 * i);
        }
        integer.combine(part);
    }
    CHECK(integer.variance() == doctest::Approx(32. / 3).epsilon(1e-15));
    CHECK(std::fabs(integer.skewness()) < 1e-15);
    CHECK(integer.kurtosis() == doctest::Approx(2.32).epsilon(1e-15));
    CHECK(integer.variance() == reference.variance());
    Moments<2> decimal;
    decimal.add(Int256::signed64(100), 2);
    decimal.add(Int256::signed64(300), 2);
    CHECK(decimal.total.value() == 4);
    CHECK(decimal.total.mean() == 2);
    CHECK(decimal.sd() == doctest::Approx(std::sqrt(2.)).epsilon(1e-15));
    CHECK(decimal.variance() == 2);
    CHECK(Int256::unsigned128(9007199254740993ULL).to_double() == 9007199254740992.);
    CHECK(Int256::unsigned128(9007199254740995ULL).to_double() == 9007199254740996.);
    CHECK(Int256::unsigned128(UINT64_MAX).to_double() == std::scalbn(1.,64));
    const auto n = count_value(9007199254740993ULL);
    CHECK(n.hi == 9007199254740992.);
    CHECK(n.lo == 1);
}

TEST_CASE("range-aligned sums empty states and valid extreme pivots are safe") {
    Total a, b;
    a.add(1e308); a.add(1e308);
    b.add(-1e308); b.add(3.);
    a.combine(b);
    a.add(-1e308);
    CHECK(a.value() == 3);
    CHECK(a.mean() == .6);
    Moments<4> empty, one, constant;
    CHECK(empty.total.value() == 0);
    CHECK(std::isnan(empty.total.mean()));
    CHECK(std::isnan(empty.sd()));
    one.add(1.);
    CHECK(std::isnan(one.variance()));
    CHECK(std::isnan(one.skewness()));
    for (int i = 0; i < 3; ++i) constant.add(8e307);
    CHECK(constant.sd() == 0);
    CHECK(constant.variance() == 0);
    CHECK(std::isnan(constant.kurtosis()));
    const double limit = std::scalbn(1.,1023) - std::scalbn(1.,975);
    CHECK(std::isfinite(2*limit)); // the proposed review counterexample is within DBL_MAX
    Moments<4> extreme;
    extreme.add(-limit); extreme.add(0.); extreme.add(limit);
    CHECK(extreme.sd() == doctest::Approx(limit).epsilon(1e-15));
    CHECK(std::isinf(extreme.variance()));
    Correlation pair;
    pair.add(-limit,1.); pair.add(limit,2.);
    CHECK(pair.value() == 1);
}

TEST_CASE("correlation block merges cover varying and mixed integer axes") {
    for (double scale : {1e-300, 1., 1e300}) {
        Correlation serial, merged;
        for (int block = 0; block < 4; ++block) {
            Correlation part;
            for (int i = 0; i < 128; ++i) {
                const int k = block * 128 + i;
                const auto x = Int256::signed64((int64_t(1) << 62) + k);
                const double y = scale * (k + 1);
                serial.add(x,y);
                part.add(x,y);
            }
            merged.combine(part);
        }
        CHECK(serial.value() == doctest::Approx(1.).epsilon(1e-14));
        CHECK(merged.value() == doctest::Approx(serial.value()).epsilon(1e-14));
    }
}
