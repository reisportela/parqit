#include "doctest.h"

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

TEST_CASE("decimal becomes double, never dropped or missing (charter 6.11)") {
    duckdb_logical_type lt = duckdb_create_decimal_type(18, 3);
    ColumnPlan p = plan_read_column("money", lt);
    duckdb_destroy_logical_type(&lt);
    CHECK_FALSE(p.dropped);
    CHECK(p.stata_type == StType::Double);
    CHECK(p.cast_sql.find("AS DOUBLE") != std::string::npos);
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
