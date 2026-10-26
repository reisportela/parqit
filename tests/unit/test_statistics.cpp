#include "doctest.h"
#include "engine/sample_key.hpp"
#include "engine/session.hpp"
#include "engine/statistics_math.hpp"
#include "engine/standard_errors.hpp"
#include "engine/integer_storage.hpp"
#include "engine/sort_storage.hpp"
#include "engine/sum_storage.hpp"

#include <cstring>
#include <random>
#include <tuple>
#include <cfenv>

using namespace parqit::statistics;

static uint64_t double_bits(double value) {
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}

TEST_CASE("floating sort keys are reversible and monotone on their checked domain") {
    std::mt19937_64 random(177);
    std::vector<std::pair<double,uint64_t>> values{{0.,parqit::SortStorage::ordered_bits(0)}};
    for (unsigned i = 0; i < 10000; ++i) {
        const uint64_t bits = (random() & (uint64_t(1) << 63)) |
            ((1 + random()%2045) << 52) | (random() & 0xfffffffffffffULL);
        REQUIRE(parqit::SortStorage::bits_safe(bits));
        const auto key = parqit::SortStorage::ordered_bits(bits);
        CHECK(parqit::SortStorage::original_bits(key) == bits);
        double value;
        std::memcpy(&value,&bits,sizeof value);
        values.emplace_back(value,key);
    }
    std::sort(values.begin(),values.end());
    for (size_t i = 1; i < values.size(); ++i) CHECK(values[i-1].second <= values[i].second);
    parqit::SortStorage proof;
    proof.add(.25); CHECK(proof.mask == 0); CHECK(proof.reversible);
    proof.add(-0.); CHECK_FALSE(proof.reversible);
    parqit::SortStorage fraction, subnormal;
    fraction.add(.5); subnormal.add(0x1p-1074); fraction.combine(subnormal);
    CHECK_FALSE(fraction.reversible);
}

TEST_CASE("floating sort conversion round-trips bits and refuses unsupported later values") {
    auto &s = parqit::Session::instance();
    duckdb_result result{};
    std::string error;
    REQUIRE_MESSAGE(s.query("SELECT x,__parqit_sort_restore(__parqit_sort_bits(x)) FROM "
        "(VALUES (-1e200::DOUBLE),(-1.25),(0.),(.5),(1e200),(NULL)) t(x)", &result, &error), error);
    for (idx_t row = 0; row < duckdb_row_count(&result); ++row) {
        CHECK(duckdb_value_is_null(&result,0,row)==duckdb_value_is_null(&result,1,row));
        if (!duckdb_value_is_null(&result,0,row))
            CHECK(double_bits(duckdb_value_double(&result,0,row))==double_bits(duckdb_value_double(&result,1,row)));
    }
    duckdb_destroy_result(&result);
    for (const char *bad : {"'-0.0'","'nan'","'inf'","'-inf'","'4.9406564584124654e-324'"}) {
        CHECK_FALSE(s.query("SELECT __parqit_sort_bits("+std::string(bad)+"::DOUBLE)",&result,&error));
        CHECK(error.find("lossless floating sort-key proof")!=std::string::npos);
    }
}

TEST_CASE("binary sum proof counts actual values and refuses unsafe exponent spans") {
    parqit::SumStorage empty;
    CHECK(empty.safe()); CHECK(empty.width() == 0); CHECK(empty.exponent() == 0);
    empty.add(-0.); empty.add(0.);
    CHECK(empty.safe()); CHECK(empty.n == 2); CHECK(empty.integer_mask == 0);
    parqit::SumStorage a, b;
    a.add(.5); b.add(1.25); a.combine(b);
    CHECK(a.safe()); CHECK(a.n == 2); CHECK(a.exponent() == -2); CHECK(a.width() == 3);
    a.add(0x1p126);
    CHECK_FALSE(a.safe());
    parqit::SumStorage wide;
    wide.add(0x1p-100); wide.add(0x1p100); CHECK_FALSE(wide.safe());
    parqit::SumStorage edge;
    edge.add(1); edge.add(0x1p126); CHECK_FALSE(edge.safe());
    for (double bad : std::array<double,4>{INFINITY, -INFINITY, NAN, 0x1p1023}) {
        parqit::SumStorage proof;
        proof.add(bad); CHECK_FALSE(proof.safe());
    }
    parqit::SumStorage overflow, one;
    overflow.n = UINT64_MAX; one.add(1); overflow.combine(one);
    CHECK_FALSE(overflow.safe()); CHECK(overflow.n == UINT64_MAX);
}

TEST_CASE("checked binary scaling never rounds, wraps or misreads subnormal inputs") {
    parqit::ScaledInteger value;
    REQUIRE(value.assign(-1., 0));
    CHECK(value.low == UINT64_MAX); CHECK(value.high == UINT64_MAX);
    REQUIRE(value.assign(-0x1p64, 0));
    CHECK(value.low == 0); CHECK(value.high == UINT64_MAX);
    REQUIRE(value.assign(0x1p126, 0));
    CHECK(value.low == 0); CHECK(value.high == uint64_t(1) << 62);
    CHECK_FALSE(value.assign(0x1p127, 0));
    CHECK_FALSE(value.assign(-0x1p127, 0));
    REQUIRE(value.assign(-0x1p127, 127));
    CHECK(value.integer().to_double() == -1.);
    REQUIRE(value.assign(0x1p-1074, -1074)); CHECK(value.low == 1); CHECK(value.high == 0);
    REQUIRE(value.assign(1.25, -2)); CHECK(value.low == 5);
    CHECK_FALSE(value.assign(1.25, 0));
    CHECK_FALSE(value.assign(0x1p-1074, 0));
    REQUIRE(value.assign(-0., -1074)); CHECK(value.low == 0); CHECK(value.high == 0);
    for (double bad : std::array<double,4>{INFINITY, -INFINITY, NAN, 0x1p1023}) CHECK_FALSE(value.assign(bad, 0));
    CHECK_FALSE(value.assign(1., -1075)); CHECK_FALSE(value.assign(1., 1023));
}

TEST_CASE("scaled native totals reproduce the original accumulator including double-rounding traps") {
    struct RoundingGuard { int mode = std::fegetround(); ~RoundingGuard() { std::fesetround(mode); } } guard;
    REQUIRE(std::fesetround(FE_TONEAREST) == 0);
    std::mt19937_64 random(176);
    for (int exponent : {-1074,-1050,-1000,-60,0,500,970}) {
        for (unsigned group = 0; group < 16; ++group) {
            std::vector<double> values;
            parqit::SumStorage proof;
            parqit::numeric::BinarySum reference;
            for (unsigned i = 0; i < 1025; ++i) {
                const int64_t coefficient = static_cast<int64_t>(random() % (uint64_t(1) << 40)) - (int64_t(1) << 39);
                const double x = std::scalbn(static_cast<double>(coefficient), exponent);
                values.push_back(x); proof.add(x); reference.add(x);
            }
            REQUIRE(proof.safe());
            Int256 sum;
            for (double x : values) {
                parqit::ScaledInteger integer;
                REQUIRE(integer.assign(x, proof.exponent()));
                sum.add(integer.integer());
            }
            CHECK(double_bits(parqit::scaled_sum(sum, proof.exponent())) == double_bits(reference.sum()));
            CHECK(double_bits(parqit::scaled_mean(sum, proof.n, proof.exponent())) == double_bits(reference.mean(proof.n)));
        }
    }
    const auto numerator = Int256::signed64(6755399441055746LL); //3*2^51+2
    const double correct = parqit::scaled_mean(numerator, 3, -1074);
    CHECK(double_bits(correct) == 0x0008000000000001ULL);
    const double rounded_twice = std::scalbn(parqit::numeric::integer_ratio(numerator, 3, 0), -1074);
    CHECK(double_bits(rounded_twice) == 0x0008000000000000ULL);
    CHECK(double_bits(parqit::scaled_mean(numerator.negated(), 3, -1074)) == 0x8008000000000001ULL);
    CHECK(double_bits(parqit::scaled_mean(Int256::signed64(-1), 2, -1074)) == 0x8000000000000000ULL);
    CHECK(std::isnan(parqit::scaled_mean(Int256{}, 0, 0)));
    CHECK(std::isnan(parqit::scaled_mean(Int256::signed64(1), 1, -1075)));
}

TEST_CASE("scaled sum C API proof, conversion, exact finalisation and native overflow failures") {
    auto &session = parqit::Session::instance();
    std::string error;
    duckdb_result result{};
    REQUIRE_MESSAGE(session.query("SELECT p.n,p.exponent,p.width,p.safe,p.integer32 FROM "
        "(SELECT __parqit_sum_storage(x) p FROM (VALUES (0.5::DOUBLE),(1.25),(NULL)) t(x))",
        &result, &error), error);
    CHECK(duckdb_value_uint64(&result, 0, 0) == 2);
    CHECK(duckdb_value_int32(&result, 1, 0) == -2);
    CHECK(duckdb_value_int32(&result, 2, 0) == 3);
    CHECK(duckdb_value_boolean(&result, 3, 0)); CHECK_FALSE(duckdb_value_boolean(&result, 4, 0));
    duckdb_destroy_result(&result);
    REQUIRE_MESSAGE(session.query("SELECT __parqit_scaled_sum(sum(q),-2),__parqit_scaled_mean(sum(q),count(q),-2) "
        "FROM (SELECT __parqit_scaled_integer(x,-2) q FROM (VALUES (0.5::DOUBLE),(1.25),(NULL)) t(x))",
        &result, &error), error);
    CHECK(duckdb_value_double(&result, 0, 0) == 1.75); CHECK(duckdb_value_double(&result, 1, 0) == .875);
    duckdb_destroy_result(&result);
    REQUIRE_MESSAGE(session.query("SELECT __parqit_scaled_mean(6755399441055746::HUGEINT,3,-1074), "
        "__parqit_scaled_mean(-1,2,-1074),coalesce(__parqit_scaled_sum(NULL,0),0), "
        "__parqit_scaled_mean(NULL,0,0)", &result, &error), error);
    CHECK(double_bits(duckdb_value_double(&result, 0, 0)) == 0x0008000000000001ULL);
    CHECK(double_bits(duckdb_value_double(&result, 1, 0)) == 0x8000000000000000ULL);
    CHECK(duckdb_value_double(&result, 2, 0) == 0); CHECK(duckdb_value_is_null(&result, 3, 0));
    duckdb_destroy_result(&result);
    REQUIRE_MESSAGE(session.query("SELECT __parqit_scaled_sum('27670116110564329473'::HUGEINT,0), "
        "__parqit_double('27670116110564329473'::HUGEINT), __parqit_scaled_sum(5,1022), "
        "(__parqit_total(x)).sum FROM (SELECT '4.49423283715579e307'::DOUBLE x FROM range(5))",
        &result, &error), error);
    CHECK(duckdb_value_double(&result, 0, 0) == 0x1.8000000000001p64);
    CHECK(duckdb_value_double(&result, 1, 0) == 0x1.8000000000001p64);
    CHECK(duckdb_value_is_null(&result, 2, 0)); CHECK(duckdb_value_is_null(&result, 3, 0));
    duckdb_destroy_result(&result);
    for (const char *bad : {"0.5", "'nan'", "'inf'", "'4.9406564584124654e-324'"}) {
        CHECK_FALSE(session.query("SELECT __parqit_scaled_integer(" + std::string(bad) + "::DOUBLE,0)", &result, &error));
        CHECK(error.find("exact binary scaling proof") != std::string::npos);
    }
    CHECK_FALSE(session.query("SELECT sum(x) FROM (SELECT '1329227995784915872903807060280344576'::HUGEINT x "
                              "FROM range(1024))", &result, &error));
    CHECK((error.find("overflow") != std::string::npos || error.find("Overflow") != std::string::npos));
}

TEST_CASE("native integer sums finalise their means once without narrowing the exact numerator") {
    auto &session = parqit::Session::instance();
    std::string error;
    duckdb_result result{};
    const std::vector<std::tuple<std::string, uint64_t, double>> cases = {
        {"1",3,1./3.}, {"-1",3,-1./3.}, {"0",17,0.},
        {"9007199254740995",3,3002399751580331.5},
        {"-18446744073709551616",2,-0x1p63},
        {"170141183460469231731687303715884105727",1,0x1p127}
    };
    for (const auto &test : cases) {
        REQUIRE_MESSAGE(session.query("SELECT __parqit_integer_mean('" + std::get<0>(test) +
            "'::HUGEINT, " + std::to_string(std::get<1>(test)) + ")", &result, &error), error);
        CHECK(duckdb_value_double(&result, 0, 0) == std::get<2>(test));
        duckdb_destroy_result(&result);
    }
    REQUIRE_MESSAGE(session.query("SELECT __parqit_integer_mean(1::DECIMAL(18,2),3), "
        "__parqit_integer_mean(NULL,0), __parqit_integer_mean(1,0), "
        "__parqit_integer_mean(1,NULL)", &result, &error), error);
    CHECK(duckdb_value_double(&result, 0, 0) == 1./3.);
    for (idx_t col = 1; col < 4; ++col) CHECK(duckdb_value_is_null(&result, col, 0));
    duckdb_destroy_result(&result);
    CHECK_FALSE(session.query("SELECT __parqit_integer_mean(1::DOUBLE,3)", &result, &error));
    CHECK(error.find("integral or DECIMAL numerator") != std::string::npos);
    CHECK_FALSE(session.query("SELECT __parqit_integer_mean(1,-1)", &result, &error));
    CHECK(error.find("count is outside uint64 range") != std::string::npos);
    CHECK_FALSE(session.query("SELECT __parqit_integer_mean(1,3.5)", &result, &error));
    CHECK(error.find("integer count") != std::string::npos);
    // All groups exercise cancellation beyond2^53 before the division.
    REQUIRE_MESSAGE(session.query("SELECT count(*) FROM (SELECT g, "
        "__parqit_integer_mean(sum(x),count(x)) m, (__parqit_total(x)).mean reference "
        "FROM (SELECT i%31 g, CASE i%3 WHEN 0 THEN 9007199254740993::BIGINT "
        "WHEN 1 THEN 1::BIGINT ELSE -9007199254740993::BIGINT END x "
        "FROM range(300000) t(i)) d GROUP BY g) WHERE m IS DISTINCT FROM reference",
        &result, &error), error);
    CHECK(duckdb_value_int64(&result, 0, 0) == 0);
    duckdb_destroy_result(&result);
}

TEST_CASE("lossless sort widths preserve zero signs, subnormals and integer endpoints") {
    const std::pair<double, uint32_t> cases[] = {
        {-128.,7}, {127.,7}, {-129.,6}, {128.,6}, {-32768.,6}, {32767.,6},
        {-32769.,4}, {32768.,4}, {-2147483648.,4}, {2147483647.,4},
        {-2147483649.,0}, {2147483648.,0}, {0.,7}, {-0.,0}, {.5,0},
        {0x1p-1074,0}, {-0x1p-1074,0}, {0x1p-1022,0},
        {std::numeric_limits<double>::infinity(),0},
        {-std::numeric_limits<double>::infinity(),0},
        {std::numeric_limits<double>::quiet_NaN(),0}, {0x1p1023,0}, {0x1p53,0}
    };
    for (const auto &test : cases) CHECK(parqit::SortStorage::fits(test.first) == test.second);
    parqit::SortStorage a, b;
    a.add(1); b.add(32768); a.combine(b); CHECK(a.result() == 4);
    b.add(-0.); a.combine(b); CHECK(a.result() == 0);
}

TEST_CASE("sort storage proof and checked casts never round a rejected value") {
    auto &session = parqit::Session::instance();
    std::string error;
    duckdb_result result{};
    REQUIRE_MESSAGE(session.query(
        "SELECT (__parqit_sort_storage(x)).mask FROM "
        "(SELECT CASE WHEN i=65536 THEN '-0.0'::DOUBLE ELSE 1::DOUBLE END x "
        "FROM range(65537) t(i))", &result, &error), error);
    CHECK(duckdb_value_uint32(&result, 0, 0) == 0);
    duckdb_destroy_result(&result);
    for (const char *bad : {"0.5", "'nan'", "'inf'", "'-inf'", "'-0.0'",
                            "'4.9406564584124654e-324'", "2147483648"}) {
        CHECK_FALSE(session.query("SELECT __parqit_sort_i32(CAST(" +
                                  std::string(bad) + " AS DOUBLE))", &result, &error));
        CHECK(error.find("lossless integer proof") != std::string::npos);
    }
    REQUIRE_MESSAGE(session.query(
        "SELECT __parqit_sort_i8(-128.), __parqit_sort_i16(32767.), "
        "__parqit_sort_i32(-2147483648.), __parqit_sort_i8(NULL)",
        &result, &error), error);
    CHECK(duckdb_value_int8(&result, 0, 0) == -128);
    CHECK(duckdb_value_int16(&result, 1, 0) == 32767);
    CHECK(duckdb_value_int32(&result, 2, 0) == -2147483648LL);
    CHECK(duckdb_value_is_null(&result, 3, 0));
    duckdb_destroy_result(&result);
}

TEST_CASE("integer compression proof rejects fractions and every denormal without rounding") {
    const std::pair<double, uint32_t> cases[] = {
        {-127.,7}, {100.,7}, {-128.,6}, {101.,6}, {-32767.,6}, {32740.,6},
        {-32768.,4}, {32741.,4}, {-2147483647.,4}, {2147483620.,4},
        {-2147483648.,0}, {2147483621.,0}, {1.25,0}, {-0.,7},
        {0x1p-1074,0}, {-0x1p-1074,0}, {0x1p-1022,0},
        {std::numeric_limits<double>::infinity(),7},
        {-std::numeric_limits<double>::infinity(),7},
        {std::numeric_limits<double>::quiet_NaN(),7}, {0x1p1023,7}, {-0x1p1023,7}
    };
    for (const auto &test : cases) {
        parqit::IntegerStorage state;
        state.add(test.first);
        CHECK(state.mask == test.second);
    }
    parqit::IntegerStorage small, large, fractional;
    small.add(1); large.add(100000000); fractional.add(.5);
    small.combine(large); CHECK(small.mask == 4);
    small.combine(fractional); CHECK(small.mask == 0);
    CHECK(small.result() == 8);
    parqit::IntegerStorage subnormal, proof;
    subnormal.add(0x1p-1074); CHECK(subnormal.result() == 0);
    proof.add(1.25); proof.combine(subnormal); CHECK(proof.result() == 8);
    subnormal.combine(proof); CHECK(subnormal.result() == 8);
    parqit::IntegerStorage invalid_date;
    invalid_date.invalid_date = true;
    proof.combine(invalid_date); CHECK(proof.result() == 0);
}

TEST_CASE("integer storage C API proves actual grouped cells including late fractions and NULLs") {
    auto &session = parqit::Session::instance();
    std::string error;
    REQUIRE(session.ensure_open());
    duckdb_result result{};
    REQUIRE_MESSAGE(session.query(
        "SELECT g, (__parqit_integer_storage(x)).mask FROM "
        "(SELECT g, CASE WHEN g=0 THEN NULL::DOUBLE WHEN g=1 THEN 100::DOUBLE "
        "WHEN g=2 THEN 10000::DOUBLE WHEN g=3 THEN 1000000::DOUBLE "
        "WHEN i=65536 THEN 0.5 ELSE 1.0 END::DOUBLE AS x "
        "FROM range(5) a(g), range(65537) b(i)) t GROUP BY g ORDER BY g",
        &result, &error), error);
    REQUIRE(duckdb_row_count(&result) == 5);
    const uint32_t expected[] = {7,7,6,4,8};
    for (idx_t i = 0; i < 5; ++i) {
        CHECK_FALSE(duckdb_value_is_null(&result, 1, i));
        CHECK(duckdb_value_uint32(&result, 1, i) == expected[i]);
    }
    duckdb_destroy_result(&result);
    REQUIRE_MESSAGE(session.query(
        "SELECT (__parqit_integer_storage(DATE '1960-01-01')).mask, "
        "(__parqit_integer_storage(DATE '1970-01-01')).mask, "
        "(__parqit_integer_storage(DATE '2059-08-22')).mask, "
        "(__parqit_integer_storage(DATE 'infinity')).mask, "
        "(__parqit_integer_storage(DATE '-infinity')).mask",
        &result, &error), error);
    const uint32_t dates[] = {7,6,4,0,0};
    for (idx_t col = 0; col < 5; ++col) CHECK(duckdb_value_uint32(&result, col, 0) == dates[col]);
    duckdb_destroy_result(&result);
}

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

/* SAMPLE-DESIGN-1: fixed vectors, computed independently in Python from the
 * documented rule, so a change to the mixing constants cannot pass silently */
TEST_CASE("cluster sampling keys are the documented splitmix64 rule") {
    using namespace parqit::sample_key;
    CHECK(of_double(0, 1.0) == 258181728628715636ULL);
    CHECK(of_double(1, 1.0) == 13482269005145962642ULL);
    CHECK(of_double(0, -0.0) == 12035550249420947055ULL);
    CHECK(of_double(0, 0.0) == of_double(0, -0.0));
    CHECK(of_double(7, 2.5) == 3475726179698553490ULL);
    CHECK(of_bytes(0, "", 0) == 6214086819679966137ULL);
    CHECK(of_bytes(0, "1", 1) == 7076828570760510544ULL);
    const char *evora = "\xC3\x89vora";
    CHECK(of_bytes(1, evora, std::strlen(evora)) == 8350063622925145315ULL);
    /* the number 1 and the text "1" are different clusters with different keys */
    CHECK(of_double(0, 1.0) != of_bytes(0, "1", 1));
    /* 2^53 + 1 has no binary64 of its own: it is ranked as 2^53 (documented) */
    CHECK(of_double(0, 0x1p53 + 1.0) == of_double(0, 0x1p53));
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

TEST_CASE("binomial errors validate observations and round the exact probability once") {
    BinomialError empty, valid, bad, decimal;
    CHECK(std::isnan(empty.result()));
    for (double x : {0., 0., 1.}) valid.add(x);
    CHECK(valid.result() == 0x1.16b28f55d72d4p-2); // Decimal-180 sqrt(2/27)
    for (double x : {-1., 0., 1.}) bad.add(x);
    CHECK(std::isnan(bad.result()));
    valid.combine(bad);
    CHECK(std::isnan(valid.result()));
    decimal.add(Int256::signed64(0), 2);
    decimal.add(Int256::signed64(100), 2);
    CHECK(decimal.result() == 0x1.6a09e667f3bcdp-2);
    decimal.add(Int256::signed64(50), 2);
    CHECK(std::isnan(decimal.result()));
    BinomialError large;
    large.n = UINT64_MAX;
    large.successes = UINT64_MAX / 2;
    CHECK(large.result() == 0x1p-33);
}

TEST_CASE("Poisson errors round exact totals before binary64 conversion") {
    PoissonError empty, fractional, below, tie, tiny, decimal_below, decimal_tie;
    CHECK(std::isnan(empty.result()));
    for (double x : {.5, .5, .5}) fractional.add(x);
    CHECK(fractional.result() == 0x1.e2b7dddfefa66p-2); // sqrt(2)/3
    for (double x : {0.5 - 0x1p-54, 0x1p-56, 0x1p-56}) below.add(x);
    REQUIRE(below.total.value() == .5); // binary64 sum would introduce a false tie
    CHECK(below.result() == 0);
    for (double x : {0., .5, 0.}) tie.add(x);
    CHECK(tie.result() == 0x1.5555555555555p-2);
    for (double x : {-0., 0x1p-1074, 0.}) tiny.add(x);
    CHECK(tiny.result() == 0);
    CHECK_FALSE(std::signbit(tiny.result()));
    decimal_below.add(Int256::unsigned128(499999999999999999ULL), 18);
    decimal_tie.add(Int256::unsigned128(500000000000000000ULL), 18);
    REQUIRE(decimal_below.total.value() == .5);
    CHECK(decimal_below.result() == 0);
    CHECK(decimal_tie.result() == 1);
}

TEST_CASE("standard errors combine exact states and retain finite overflow-scale results") {
    PoissonError whole, merged, invalid;
    BinomialError whole_b, merged_b;
    for (unsigned block = 0; block < 4; ++block) {
        PoissonError part;
        BinomialError part_b;
        for (unsigned i = 0; i < 129; ++i) {
            const double x = (block * 129 + i) / 8.;
            part.add(x); whole.add(x);
            part_b.add(double(i % 2)); whole_b.add(double(i % 2));
        }
        merged.combine(part);
        merged_b.combine(part_b);
    }
    CHECK(merged.result() == whole.result());
    CHECK(merged_b.result() == whole_b.result());
    invalid.add(-1.); invalid.add(2.); invalid.add(5.);
    CHECK(std::isnan(invalid.result()));
    merged.combine(invalid);
    CHECK(std::isnan(merged.result()));
    PoissonError extreme;
    extreme.add(8e307); extreme.add(8e307);
    CHECK(extreme.result() == 0x1.e307213a7e69dp+510);
    extreme.add(8e307);
    CHECK(std::isinf(extreme.total.value()));
    CHECK(extreme.result() == 0x1.8a63fd9e2e28bp+510);
}

TEST_CASE("Poisson count rounding carries across limbs and square-root ties round to even") {
    PoissonError carry, carry_two, straddle, root_tie, decimal_carry, decimal_zero, decimal_scale38;
    carry.add(0x1p64 - 2048); carry.add(2047.5);
    CHECK(carry.result() == 0x1p31);
    carry_two.add(0x1p128 - 0x1p75);
    carry_two.add(0x1p75 - 0x1p22);
    carry_two.add(4194303.5);
    CHECK(carry_two.result() == 0x1.5555555555555p62);
    straddle.add(16384.5);
    CHECK(straddle.result() == std::sqrt(16385.));
    for (double x : {9 * 0x1p106, 9 * 0x1p54, 9.}) root_tie.add(x);
    CHECK(root_tie.result() == 0x1p53);
    decimal_carry.add(Int256::unsigned128(UINT64_MAX - 4, 9), 1);
    CHECK(decimal_carry.result() == 0x1p32);
    decimal_zero.add(Int256{}, 38);
    CHECK(decimal_zero.result() == 0);
    auto coefficient = Int256::signed64(1);
    for (unsigned i = 0; i < 38; ++i) coefficient = coefficient.multiplied(10);
    decimal_scale38.add(coefficient - Int256::signed64(1), 38);
    CHECK(decimal_scale38.result() == 1);
    PoissonError empty, combined;
    combined.combine(empty); combined.combine(carry); combined.combine(empty);
    CHECK(combined.result() == carry.result());
    BinomialError empty_b, valid_b, combined_b;
    valid_b.add(0.); valid_b.add(1.);
    combined_b.combine(empty_b); combined_b.combine(valid_b); combined_b.combine(empty_b);
    CHECK(combined_b.result() == valid_b.result());
}

TEST_CASE("standard-error C API retains integer and decimal types and skips missing inputs") {
    auto &session = parqit::Session::instance();
    std::string error;
    REQUIRE(session.ensure_open());
    const std::pair<const char *, double> cases[] = {
        {"SELECT (__parqit_sepoisson(x)).value FROM (VALUES (9007199254741001::BIGINT)) t(x)",
         0x1.6a09e667f3bd0p26},
        {"SELECT (__parqit_sepoisson(x)).value FROM (VALUES (CAST('0.499999999999999999' AS DECIMAL(18,18)))) t(x)", 0},
        {"SELECT (__parqit_sepoisson(x)).value FROM (VALUES (CAST('0.5' AS DECIMAL(38,38)))) t(x)", 1},
        {"SELECT (__parqit_sebinomial(x)).value FROM (VALUES (0::DOUBLE), (1::DOUBLE), "
         "('NaN'::DOUBLE), ('Infinity'::DOUBLE), ('-Infinity'::DOUBLE), (NULL::DOUBLE)) t(x)",
         0x1.6a09e667f3bcdp-2}
    };
    for (const auto &test : cases) {
        duckdb_result result{};
        REQUIRE_MESSAGE(session.query(test.first, &result, &error), error);
        CHECK_FALSE(duckdb_value_is_null(&result, 0, 0));
        CHECK(duckdb_value_double(&result, 0, 0) == test.second);
        duckdb_destroy_result(&result);
    }
    duckdb_result empty{};
    REQUIRE(session.query("SELECT (__parqit_sepoisson(x)).value FROM (SELECT 0::DOUBLE x WHERE false)", &empty, &error));
    CHECK(duckdb_value_is_null(&empty, 0, 0));
    duckdb_destroy_result(&empty);
    duckdb_result groups{};
    REQUIRE(session.query(
        "SELECT g, (__parqit_sebinomial(x)).value, (__parqit_sepoisson(x)).value FROM "
        "(SELECT g, CASE WHEN g%5=0 THEN -1::DOUBLE WHEN g%5=1 THEN NULL::DOUBLE "
        "ELSE v::DOUBLE END x FROM range(10001) t(g), (VALUES (0),(1)) p(v)) q GROUP BY g",
        &groups, &error));
    REQUIRE(duckdb_row_count(&groups) == 10001);
    for (idx_t row = 0; row < 10001; ++row) {
        const auto g = duckdb_value_int64(&groups, 0, row);
        const bool missing = g % 5 <= 1;
        REQUIRE(duckdb_value_is_null(&groups, 1, row) == missing);
        REQUIRE(duckdb_value_is_null(&groups, 2, row) == missing);
        if (!missing) {
            REQUIRE(duckdb_value_double(&groups, 1, row) == 0x1.6a09e667f3bcdp-2);
            REQUIRE(duckdb_value_double(&groups, 2, row) == .5);
        }
    }
    duckdb_destroy_result(&groups);
}
