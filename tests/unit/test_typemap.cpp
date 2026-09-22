#include "doctest.h"

#include <limits>

#include "duckdb.h"
#include "engine/typemap.hpp"

using namespace parqit;

static duckdb_logical_type LT(duckdb_type t) { return duckdb_create_logical_type(t); }

static ColumnPlan plan_for(duckdb_type t, const char *name = "c") {
    duckdb_logical_type lt = LT(t);
    ColumnPlan p = plan_read_column(name, lt);
    duckdb_destroy_logical_type(&lt);
    return p;
}

TEST_CASE("format classification is by prefix, display tokens never matter") {
    /* charter §6.5: %tcHH:MM:SS is a datetime, not a 'time' */
    CHECK(classify_format("%tcHH:MM:SS") == FmtClass::Tc);
    CHECK(classify_format("%tc") == FmtClass::Tc);
    CHECK(classify_format("%-tcCCYY") == FmtClass::Tc);
    CHECK(classify_format("%tC") == FmtClass::TC);
    CHECK(classify_format("%td") == FmtClass::Td);
    CHECK(classify_format("%tdDD/NN/CCYY") == FmtClass::Td);
    CHECK(classify_format("%tm") == FmtClass::Tm);
    CHECK(classify_format("%tq") == FmtClass::Tq);
    CHECK(classify_format("%th") == FmtClass::Th);
    CHECK(classify_format("%tw") == FmtClass::Tw);
    CHECK(classify_format("%ty") == FmtClass::Ty);
    CHECK(classify_format("%tb") == FmtClass::Tb);
    CHECK(classify_format("%9.2f") == FmtClass::None);
    CHECK(classify_format("%12s") == FmtClass::None);
    CHECK(classify_format("") == FmtClass::None);
    /* period counts (charter §6.3) */
    CHECK(fmt_is_period_count(FmtClass::Tm));
    CHECK(fmt_is_period_count(FmtClass::TC));
    CHECK_FALSE(fmt_is_period_count(FmtClass::Td));
    CHECK_FALSE(fmt_is_period_count(FmtClass::Tc));
}

TEST_CASE("integer sizing uses Stata's exact limits") {
    CHECK(integer_type_for_range(-127, 100) == StType::Byte);
    CHECK(integer_type_for_range(-128, 0) == StType::Int);   /* below byte min */
    CHECK(integer_type_for_range(0, 101) == StType::Int);    /* above byte max */
    CHECK(integer_type_for_range(-32767, 32740) == StType::Int);
    CHECK(integer_type_for_range(0, 32741) == StType::Long); /* above int max */
    CHECK(integer_type_for_range(-2147483647.0, 2147483620.0) == StType::Long);
    /* int32 boundary values that would collide with Stata missing codes */
    CHECK(integer_type_for_range(0, 2147483621.0) == StType::Double);
    CHECK(integer_type_for_range(-2147483648.0, 0) == StType::Double);
}

TEST_CASE("uint32 plans can carry values beyond 2^31 (charter 6.6)") {
    ColumnPlan p = plan_for(DUCKDB_TYPE_UINTEGER);
    CHECK_FALSE(p.dropped);
    CHECK(p.transfer == Transfer::Int64); /* via BIGINT cast — no overflow-null */
    CHECK(p.needs_minmax);
    ColumnStats s;
    s.has_minmax = true;
    s.min = 0;
    s.max = 4294967295.0;
    refine_plan(p, s);
    CHECK(p.stata_type == StType::Double); /* beyond Stata long max */
    s.max = 2147483620.0;
    p = plan_for(DUCKDB_TYPE_UINTEGER);
    refine_plan(p, s);
    CHECK(p.stata_type == StType::Long);
}

TEST_CASE("display formats never widen storage; period formats stay >= int (TYPE-1)") {
    /* byte range + a plain display format: parqit-written files always carry
     * the fmt, so this is every round-tripped byte variable */
    ColumnPlan p = plan_for(DUCKDB_TYPE_TINYINT);
    REQUIRE(p.needs_minmax);
    p.stata_format = "%8.0g";
    ColumnStats s;
    s.has_minmax = true;
    s.min = -1;
    s.max = 3;
    refine_plan(p, s);
    CHECK(p.stata_type == StType::Byte);

    p = plan_for(DUCKDB_TYPE_TINYINT);
    p.stata_format = "%9.2f";
    refine_plan(p, s);
    CHECK(p.stata_type == StType::Byte);

    /* a genuine period format keeps integer storage wide enough */
    p = plan_for(DUCKDB_TYPE_TINYINT);
    p.stata_format = "%tq";
    refine_plan(p, s);
    CHECK(p.stata_type == StType::Int);

    /* no format at all: pure range sizing */
    p = plan_for(DUCKDB_TYPE_TINYINT);
    refine_plan(p, s);
    CHECK(p.stata_type == StType::Byte);
}

TEST_CASE("decimal becomes double, never dropped or missing (charter 6.11)") {
    duckdb_logical_type lt = duckdb_create_decimal_type(18, 3);
    ColumnPlan p = plan_read_column("money", lt);
    duckdb_destroy_logical_type(&lt);
    CHECK_FALSE(p.dropped);
    CHECK(p.stata_type == StType::Double);
    CHECK(p.transfer == Transfer::Float64);
}

TEST_CASE("time-of-day maps to ms-since-midnight, never an all-null column (charter 6.5)") {
    ColumnPlan p = plan_for(DUCKDB_TYPE_TIME);
    CHECK_FALSE(p.dropped);
    CHECK(p.transfer == Transfer::TimeUs);
    CHECK(p.stata_format == "%tcHH:MM:SS");
}

TEST_CASE("unrepresentable types are dropped with a reason, not silent (charter 6.11)") {
    for (duckdb_type t : {DUCKDB_TYPE_BLOB, DUCKDB_TYPE_INTERVAL, DUCKDB_TYPE_BIT,
                          DUCKDB_TYPE_SQLNULL}) {
        ColumnPlan p = plan_for(t);
        CHECK(p.dropped);
        CHECK_FALSE(p.drop_reason.empty());
    }
    /* a typeless NULL column is dropped exactly like LIST/STRUCT, never loaded
     * as an all-missing byte variable indistinguishable from a real one
     * (brief §4 lists LIST/STRUCT/NULL together; charter §6.11) */
    ColumnPlan pn = plan_for(DUCKDB_TYPE_SQLNULL);
    CHECK(pn.dropped);
    CHECK(pn.drop_reason.find("NULL") != std::string::npos);
}

TEST_CASE("string sizing: bytes, 2045 boundary, strL beyond") {
    ColumnPlan p = plan_for(DUCKDB_TYPE_VARCHAR);
    REQUIRE(p.needs_strlen);
    ColumnStats s;
    s.max_strlen = 2045;
    refine_plan(p, s);
    CHECK(p.stata_type == StType::Str);
    CHECK(p.str_bytes == 2045);
    p = plan_for(DUCKDB_TYPE_VARCHAR);
    s.max_strlen = 2046;
    refine_plan(p, s);
    CHECK(p.stata_type == StType::StrL);
    p = plan_for(DUCKDB_TYPE_VARCHAR);
    s.max_strlen = 0; /* all null/empty */
    refine_plan(p, s);
    CHECK(p.stata_type == StType::Str);
    CHECK(p.str_bytes == 1);
}

TEST_CASE("write side: period formats stay INTEGER on disk (charter 6.3)") {
    CHECK(duck_type_for(StType::Int, FmtClass::Tm) == "INTEGER");
    CHECK(duck_type_for(StType::Long, FmtClass::Tq) == "INTEGER");
    /* %tC counts are milliseconds — far beyond int32 */
    CHECK(duck_type_for(StType::Double, FmtClass::TC) == "BIGINT");
    CHECK(duck_type_for(StType::Long, FmtClass::Td) == "DATE");
    CHECK(duck_type_for(StType::Double, FmtClass::Tc) == "TIMESTAMP");
    CHECK(duck_type_for(StType::Byte, FmtClass::None) == "TINYINT");
    CHECK(duck_type_for(StType::Int, FmtClass::None) == "SMALLINT");
    CHECK(duck_type_for(StType::Long, FmtClass::None) == "INTEGER");
    CHECK(duck_type_for(StType::Float, FmtClass::None) == "FLOAT");
    CHECK(duck_type_for(StType::Str, FmtClass::None) == "VARCHAR");
}

TEST_CASE("epoch arithmetic: floor division is negative-safe") {
    CHECK(floordiv(7, 2) == 3);
    CHECK(floordiv(-7, 2) == -4);
    CHECK(floordiv(-1000, 1000) == -1);
    CHECK(floordiv(-1001, 1000) == -2);
    CHECK(floordiv(999, 1000) == 0);
    CHECK(kEpochShiftMs == 315619200000LL);
}

#include "engine/session.hpp"

TEST_CASE("TC-US-1: %tc ms -> epoch us is exact beyond 2^56 us (A1-1/A1-11)") {
    auto check_exact = [](long long stata_ms) {
        long long us = 0;
        REQUIRE(stata_tc_ms_to_epoch_us(static_cast<double>(stata_ms), &us));
        CHECK(us == (stata_ms - kEpochShiftMs) * 1000LL);
        CHECK(us % 1000 == 0);
        CHECK(floordiv(us, 1000) + kEpochShiftMs == stata_ms);
    };
    /* 01jan5000 12:00:00.001 (day 1110338) and 31dec9999 23:59:59.999 (day 2936549) */
    check_exact(1110338LL * 86400000LL + 43200001LL);
    check_exact(2936549LL * 86400000LL + 86399999LL);
    /* a sweep of odd ms instants across 4253..9999 (where ms*1000.0 is inexact) */
    for (long long day = 837502; day <= 2936549; day += 97651)
        for (long long ms = 1; ms < 86400000; ms += 7919333)
            check_exact(day * 86400000LL + ms);
    /* the DT-001 ceiling: INT64_MAX/1000 ms past the epoch is the last instant */
    long long us = 0;
    /* (the Stata-side count is past 2^53, so only EVEN ms counts are
     * representable doubles; 9223372036854774 + shift is one) */
    CHECK(stata_tc_ms_to_epoch_us(9223372036854774.0 + kEpochShiftMs, &us));
    CHECK(us == 9223372036854774000LL);
    CHECK_FALSE(stata_tc_ms_to_epoch_us(9223372036854776.0 + kEpochShiftMs, &us));
    CHECK_FALSE(stata_tc_ms_to_epoch_us(-9223372036854776.0 + kEpochShiftMs, &us));
    CHECK_FALSE(stata_tc_ms_to_epoch_us(1.5, &us));          /* not a whole ms */
    /* MSVC rejects a constant 1.0/0.0 (C2124); build the infinity portably */
    CHECK_FALSE(stata_tc_ms_to_epoch_us(std::numeric_limits<double>::infinity(), &us)); /* not finite */
    CHECK(stata_tc_ms_to_epoch_us(0.0, &us));
    CHECK(us == -kEpochShiftMs * 1000LL);
}

TEST_CASE("TEMPORAL-ROUND-1: integer-valued counts pass through, ties round up (A1-7)") {
    CHECK(stata_round_temporal(2.5) == 3.0);
    CHECK(stata_round_temporal(-2.5) == -2.0);
    CHECK(stata_round_temporal(-0.4) == 0.0);
    CHECK(stata_round_temporal(2.4999) == 2.0);
    CHECK(stata_round_temporal(100.5) == 101.0);
    const double odd52 = 4503599627370497.0; /* 2^52 + 1: x + 0.5 is not representable */
    CHECK(stata_round_temporal(odd52) == odd52);
    CHECK(stata_round_temporal(9007199254740991.0) == 9007199254740991.0);
    CHECK(stata_round_temporal(-4503599627370497.0) == -4503599627370497.0);
    /* the SQL twin agrees with the C++ rule */
    parqit::Session &s = parqit::Session::instance();
    std::string v, err;
    REQUIRE(s.ensure_open());
    auto sql_round = [&](const char *lit) {
        std::string out;
        REQUIRE_MESSAGE(s.query_scalar("SELECT CAST(" + stata_round_temporal_sql(lit) +
                                           " AS VARCHAR)",
                                       &out, &err),
                        err);
        return out;
    };
    CHECK(sql_round("2.5::DOUBLE") == "3.0");
    CHECK(sql_round("-2.5::DOUBLE") == "-2.0");
    CHECK(sql_round("4503599627370497::DOUBLE") == "4503599627370497.0");
    CHECK(sql_round("NULL::DOUBLE") == "");
}

TEST_CASE("TS-NS-FLOOR-1: nanosecond instants floor to microseconds toward -infinity (A1-9)") {
    parqit::Session &s = parqit::Session::instance();
    std::string v, err;
    REQUIRE(s.ensure_open());
    REQUIRE_MESSAGE(s.query_scalar("SELECT CAST(epoch_us(" +
                                       timestamp_ns_floor_us_sql("TIMESTAMP_NS '1969-12-31 23:59:59.999999999'") +
                                       ") AS VARCHAR)",
                                   &v, &err),
                    err);
    CHECK(v == "-1"); /* CAST(... AS TIMESTAMP) gave 0 (truncation toward zero) */
    REQUIRE_MESSAGE(s.query_scalar("SELECT CAST(epoch_us(" +
                                       timestamp_ns_floor_us_sql("TIMESTAMP_NS '1970-01-01 00:00:00.000001999'") +
                                       ") AS VARCHAR)",
                                   &v, &err),
                    err);
    CHECK(v == "1");
    ColumnPlan p = plan_for(DUCKDB_TYPE_TIMESTAMP_NS, "t");
    CHECK(p.cast_sql.find("epoch_ns") != std::string::npos);
    CHECK(p.transfer == Transfer::TimestampUs);
}

TEST_CASE("TS-NS-MIN-1: nanosecond floor covers every finite int64 boundary") {
    Session &s = Session::instance();
    REQUIRE(s.ensure_open());
    const std::string sql = "SELECT epoch_us(" + timestamp_ns_floor_us_sql("ns") +
                            ") FROM (SELECT $1::TIMESTAMP_NS AS ns)";
    duckdb_prepared_statement stmt = nullptr;
    REQUIRE(duckdb_prepare(s.con(), sql.c_str(), &stmt) == DuckDBSuccess);
    struct Sample { int64_t ns; int64_t us; };
    const Sample samples[] = {
        {-9223372036854775807LL, -9223372036854776LL},
        {-9223372036854775500LL, -9223372036854776LL},
        {-9223372036854775001LL, -9223372036854776LL},
        {-9223372036854775000LL, -9223372036854775LL},
        {-1001, -2}, {-1000, -1}, {-999, -1}, {-1, -1}, {0, 0},
        {1, 0}, {999, 0}, {1000, 1}, {1001, 1},
        {9223372036854775806LL, 9223372036854775LL}
    };
    for (const auto &sample : samples) {
        INFO("nanoseconds = " << sample.ns);
        /* Bind the raw typed value: DuckDB's text constructor itself rejects
         * the earliest finite nanoseconds, although Parquet stores them. */
        duckdb_value value = duckdb_create_timestamp_ns({sample.ns});
        const duckdb_state bound = duckdb_bind_value(stmt, 1, value);
        duckdb_destroy_value(&value);
        CHECK(bound == DuckDBSuccess);
        duckdb_result result;
        const duckdb_state rc = duckdb_execute_prepared(stmt, &result);
        CHECK_MESSAGE(rc == DuckDBSuccess, duckdb_result_error(&result));
        if (rc == DuckDBSuccess) {
            CHECK_FALSE(duckdb_value_is_null(&result, 0, 0));
            CHECK(duckdb_value_int64(&result, 0, 0) == sample.us);
        }
        duckdb_destroy_result(&result);
    }
    CHECK(duckdb_bind_null(stmt, 1) == DuckDBSuccess);
    duckdb_result result;
    const duckdb_state rc = duckdb_execute_prepared(stmt, &result);
    CHECK(rc == DuckDBSuccess);
    if (rc == DuckDBSuccess) CHECK(duckdb_value_is_null(&result, 0, 0));
    duckdb_destroy_result(&result);
    duckdb_destroy_prepare(&stmt);
}

TEST_CASE("FLOAT-EXACT-1: a manifest float held in a non-FLOAT column is float only when proven exact (V2.3)") {
    /* a %tc written from a float variable is a TIMESTAMP on disk; its ms range
     * (≈1.8e12) exceeds long, so range sizing says double — the recorded float
     * is restored only when the scan proved every value float32-exact */
    ColumnPlan p;
    p.meta_type = "float";
    p.transfer = Transfer::TimestampUs;
    p.stata_format = "%tc";
    p.needs_minmax = true;
    p.needs_float_exact = true;
    ColumnStats st;
    st.has_minmax = true;
    st.min = 1.8e12;
    st.max = 1.8e12;
    /* no scan verdict (describe-style planning): the range forced double */
    {
        ColumnPlan q = p;
        refine_plan(q, st);
        CHECK(q.stata_type == StType::Double);
        apply_meta_type(q);
        CHECK(q.stata_type == StType::Double);
    }
    /* scanned and exact -> float */
    {
        ColumnStats s2 = st;
        s2.float_exact_checked = true;
        s2.float_exact = true;
        ColumnPlan q = p;
        refine_plan(q, s2);
        CHECK(q.float_exact_checked);
        apply_meta_type(q);
        CHECK(q.stata_type == StType::Float);
    }
    /* scanned and NOT exact (a foreign edit) -> double, never a rounded float */
    {
        ColumnStats s2 = st;
        s2.float_exact_checked = true;
        s2.float_exact = false;
        ColumnPlan q = p;
        refine_plan(q, s2);
        apply_meta_type(q);
        CHECK(q.stata_type == StType::Double);
    }
    /* a small range (int ladder): unchecked keeps the old rule (float); checked
     * follows the verdict */
    {
        ColumnStats s3;
        s3.has_minmax = true;
        s3.min = 123;
        s3.max = 123;
        ColumnPlan q = p;
        refine_plan(q, s3);
        CHECK(q.stata_type == StType::Int); /* byte -> int under a date format */
        apply_meta_type(q);
        CHECK(q.stata_type == StType::Float);
        s3.float_exact_checked = true;
        s3.float_exact = false;
        ColumnPlan r = p;
        refine_plan(r, s3);
        apply_meta_type(r);
        CHECK(r.stata_type == StType::Double);
    }
    /* a genuine FLOAT column is untouched by the rule */
    {
        ColumnPlan f;
        f.meta_type = "float";
        f.transfer = Transfer::Float32;
        f.stata_type = StType::Float;
        apply_meta_type(f);
        CHECK(f.stata_type == StType::Float);
    }
}

TEST_CASE("DFMT-1: the old-style %d daily date format is a date (audit 2026-09-01, F6)") {
    CHECK(classify_format("%d") == FmtClass::Td);
    CHECK(classify_format("%-d") == FmtClass::Td);
    CHECK(classify_format("%dCCYY-NN-DD") == FmtClass::Td);
    CHECK(classify_format("%dM_d,_CY") == FmtClass::Td);
    CHECK(classify_format("%td") == FmtClass::Td);
    /* numeric formats never start with %d followed by a letter */
    CHECK(classify_format("%9.2f") == FmtClass::None);
    CHECK(classify_format("%-12.0g") == FmtClass::None);
    CHECK(classify_format("%9,2f") == FmtClass::None);
    CHECK(classify_format("%8.0gc") == FmtClass::None);
    CHECK(classify_format("%d1") == FmtClass::None);
    CHECK(duck_type_for(StType::Long, classify_format("%d")) == "DATE");
}

TEST_CASE("INT64-PROTECT-1/BINARY-DECODE-1: the read-time type option spellings") {
    Int64Mode i = Int64Mode::Round;
    CHECK(int64_mode_parse("refuse", &i));
    CHECK(i == Int64Mode::Refuse);
    CHECK(int64_mode_parse("round", &i));
    CHECK(i == Int64Mode::Round);
    CHECK(int64_mode_parse("string", &i));
    CHECK(i == Int64Mode::String);
    /* an unknown value leaves the caller's mode alone: the plugin refuses it
     * rather than silently downgrading the protection */
    CHECK_FALSE(int64_mode_parse("Round", &i));
    CHECK_FALSE(int64_mode_parse("", &i));
    CHECK_FALSE(int64_mode_parse("text", &i));
    CHECK(i == Int64Mode::String);

    BinaryMode b = BinaryMode::Drop;
    CHECK(binary_mode_parse("drop", &b));
    CHECK(b == BinaryMode::Drop);
    CHECK(binary_mode_parse("text", &b));
    CHECK(b == BinaryMode::Text);
    CHECK(binary_mode_parse("hex", &b));
    CHECK(b == BinaryMode::Hex);
    CHECK_FALSE(binary_mode_parse("HEX", &b));
    CHECK_FALSE(binary_mode_parse("string", &b));
    CHECK(b == BinaryMode::Hex);
}

TEST_CASE("BINARY-DECODE-1: a BLOB drops by default, naming both options") {
    ColumnPlan d = plan_for(DUCKDB_TYPE_BLOB, "b");
    CHECK(d.dropped);
    CHECK(d.drop_reason.find("BLOB has no Stata representation") != std::string::npos);
    CHECK(d.drop_reason.find("binary(text)") != std::string::npos);
    CHECK(d.drop_reason.find("binary(hex)") != std::string::npos);

    duckdb_logical_type lt = LT(DUCKDB_TYPE_BLOB);
    ColumnPlan t = plan_read_column("b", lt, BinaryMode::Text);
    ColumnPlan h = plan_read_column("b", lt, BinaryMode::Hex);
    duckdb_destroy_logical_type(&lt);

    CHECK_FALSE(t.dropped);
    CHECK(t.cast_sql == "decode(\"b\")");
    CHECK(t.transfer == Transfer::Utf8);
    CHECK(t.stata_type == StType::Str);
    CHECK(t.needs_strlen); /* sized like any other string, strL beyond 2045 */
    CHECK(t.note.find("UTF-8") != std::string::npos);

    CHECK_FALSE(h.dropped);
    CHECK(h.cast_sql == "hex(\"b\")");
    CHECK(h.transfer == Transfer::Utf8);
    CHECK(h.stata_type == StType::Str);
    CHECK(h.needs_strlen);
    CHECK(h.note.find("hex") != std::string::npos);
}

TEST_CASE("INT64-PROTECT-1: int64(string) converts a >2^53 column to exact text") {
    ColumnPlan p = plan_for(DUCKDB_TYPE_BIGINT, "bk");
    REQUIRE(p.needs_big53);
    p.stata_format = "%12.0g"; /* a numeric format cannot survive on a string */
    p.vallab = "lbl";
    p.meta_type = "double"; /* the manifest must not pull it back to numeric */

    plan_big53_as_text(p);
    CHECK(p.cast_sql == "CAST(\"bk\" AS VARCHAR)");
    CHECK(p.transfer == Transfer::Utf8);
    CHECK(p.stata_type == StType::Str);
    CHECK(p.needs_strlen);
    CHECK_FALSE(p.needs_big53); /* nothing rounds, so nothing may say it does */
    CHECK_FALSE(p.needs_minmax);
    CHECK(p.stata_format.empty());
    CHECK(p.vallab.empty());
    CHECK(p.note.find("exceed 2^53") != std::string::npos);

    /* the ordinary strlen sizing then gives the exact width, and the rounding
     * note of refine_plan is gone */
    ColumnStats s;
    s.any_beyond_2p53 = true;
    s.max_strlen = 19;
    refine_plan(p, s);
    CHECK(p.stata_type == StType::Str);
    CHECK(p.str_bytes == 19);
    CHECK(p.note.find("rounded to nearest double") == std::string::npos);
    apply_meta_type(p);
    CHECK(p.stata_type == StType::Str); /* meta_type "double" cannot undo it */
    CHECK(p.str_bytes == 19);

    /* a wide DECIMAL arrives carrying "decimal converted to double": that note
     * must be REPLACED, not appended to, or the column claims both at once */
    duckdb_logical_type dt = duckdb_create_decimal_type(20, 2);
    ColumnPlan d = plan_read_column("d", dt);
    duckdb_destroy_logical_type(&dt);
    REQUIRE(d.needs_big53);
    REQUIRE(d.note.find("converted to double") != std::string::npos);
    plan_big53_as_text(d);
    CHECK(d.note == "loaded as text because values exceed 2^53 (exact)");
    CHECK(d.cast_sql == "CAST(\"d\" AS VARCHAR)");

    /* the str#/strL boundary is the ordinary one */
    ColumnPlan w = plan_for(DUCKDB_TYPE_HUGEINT, "h");
    REQUIRE(w.needs_big53);
    plan_big53_as_text(w);
    ColumnStats ws;
    ws.max_strlen = kStataStrMax + 1;
    refine_plan(w, ws);
    CHECK(w.stata_type == StType::StrL);
}

TEST_CASE("INT64-PROTECT-1: int64(round) is untouched — the note still fires") {
    ColumnPlan p = plan_for(DUCKDB_TYPE_BIGINT, "bk");
    ColumnStats s;
    s.has_minmax = true;
    s.min = 0;
    s.max = 9007199254740993.0;
    s.any_beyond_2p53 = true;
    refine_plan(p, s);
    CHECK(p.stata_type == StType::Double);
    CHECK(p.note.find("values beyond 2^53 rounded to nearest double") !=
          std::string::npos);

    /* a column of the same family whose values all fit says nothing */
    ColumnPlan q = plan_for(DUCKDB_TYPE_BIGINT, "ck");
    ColumnStats t;
    t.has_minmax = true;
    t.min = 0;
    t.max = 1000;
    refine_plan(q, t);
    CHECK(q.note.empty());
    CHECK(q.stata_type == StType::Int);
}

#include <cstring>

TEST_CASE("XMISS-1: extended missing codes round-trip Stata's bit patterns") {
    /* Stata (%21x): . = +1.0000000000000X+3ff, .a = +1.0010000000000X+3ff,
     * .z = +1.01a0000000000X+3ff — i.e. 0x7fe0000000000000 + (k << 40). */
    for (int k = 0; k <= kStataExtMissMax; k++) {
        const double d = stata_missing_value(k);
        uint64_t bits = 0;
        std::memcpy(&bits, &d, sizeof bits);
        CHECK(bits == 0x7fe0000000000000ULL + (static_cast<uint64_t>(k) << 40));
        CHECK(stata_missing_code(d) == k);
        CHECK(d >= kStataMissThreshold);
    }
    CHECK(stata_missing_code(std::numeric_limits<double>::infinity()) == -1);
    CHECK(stata_missing_code(-std::numeric_limits<double>::infinity()) == -1);
    CHECK(stata_missing_code(std::numeric_limits<double>::quiet_NaN()) == -1);
    CHECK(stata_missing_code(8.9884656743115785e307) == -1); /* maxdouble() */
    CHECK(stata_missing_code(1e308) == -1);                   /* between codes */
    CHECK(stata_missing_code(-stata_missing_value(1)) == -1);
    CHECK(stata_missing_code(1.0) == -1);
    /* one bit above .a is not a code either */
    {
        uint64_t bits = 0x7fe0100000000001ULL;
        double d = 0;
        std::memcpy(&d, &bits, sizeof d);
        CHECK(stata_missing_code(d) == -1);
    }
    CHECK(std::isnan(stata_missing_value(27)));
    CHECK(std::isnan(stata_missing_value(-1)));
    CHECK(xmissing_companion_name("x") == "_parqit_xm_x");
    CHECK(std::string(kXmissingMetaKey) == "parqit.xmissing");
}
