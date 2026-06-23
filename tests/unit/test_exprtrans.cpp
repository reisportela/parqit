/* Semantic tests: every translated expression is EXECUTED against DuckDB
 * and its result compared with Stata's documented behaviour. */
#include "doctest.h"

#include <cmath>
#include <cstdlib>

#include "duckdb.h"
#include "engine/exprtrans.hpp"
#include "engine/session.hpp"

using namespace parqit;

static ExprSchema test_schema() {
    ExprSchema s;
    s.kinds = {{"x", 'n'}, {"y", 'n'}, {"s", 's'}, {"d", 'n'}};
    return s;
}

/* fixture rows (id, x, y, s, d):
 *   1: 1,   10,  "a",   21915 (=td(01jan2020))
 *   2: 2,   20,  "bb",  21916
 *   3: NULL, 30, "",    NULL
 *   4: 4,  NULL, NULL,  21918
 *   5: 5,   50,  "héé🦆", 21919   */
static void make_fixture() {
    static bool done = false;
    if (done) return;
    Session &s = Session::instance();
    std::string err;
    REQUIRE_MESSAGE(s.exec("CREATE OR REPLACE TABLE __t AS SELECT * FROM (VALUES "
                           "(1, 1::DOUBLE, 10::DOUBLE, 'a', 21915), "
                           "(2, 2::DOUBLE, 20::DOUBLE, 'bb', 21916), "
                           "(3, NULL::DOUBLE, 30::DOUBLE, '', NULL), "
                           "(4, 4::DOUBLE, NULL::DOUBLE, NULL, 21918), "
                           "(5, 5::DOUBLE, 50::DOUBLE, 'héé🦆', 21919)"
                           ") t(id, x, y, s, d)",
                           &err),
                    err);
    done = true;
}

static long long count_where(const std::string &expr, bool stmiss = false) {
    make_fixture();
    ExprResult r = translate_filter(expr, test_schema(), stmiss);
    REQUIRE_MESSAGE(r.ok, r.error);
    Session &s = Session::instance();
    std::string v, err;
    bool qok = s.query_scalar("SELECT count(*) FROM __t WHERE " + r.sql, &v, &err);
    REQUIRE_MESSAGE(qok, (err + " [sql: " + r.sql + "]"));
    return std::strtoll(v.c_str(), nullptr, 10);
}

static std::string eval_at(const std::string &expr, int id, bool stmiss = false) {
    make_fixture();
    ExprResult r = translate_expression(expr, test_schema(), stmiss);
    REQUIRE_MESSAGE(r.ok, r.error);
    Session &s = Session::instance();
    std::string v, err;
    bool qok = s.query_scalar("SELECT CAST((" + r.sql + ") AS VARCHAR) FROM __t WHERE id = " + std::to_string(id), &v, &err);
    REQUIRE_MESSAGE(qok, (err + " [sql: " + r.sql + "]"));
    return v; /* "" means NULL */
}

TEST_CASE("filters: SQL missing semantics match Stata keep-if outcomes") {
    CHECK(count_where("x > 2") == 2);          /* 4,5 — NULL x drops */
    CHECK(count_where("x >= 1 & x <= 2") == 2);
    CHECK(count_where("x == 2 | x == 5") == 2);
    CHECK(count_where("!(x > 2)") == 2);       /* 1,2 — NULL stays out */
    CHECK(count_where("x != 2") == 3);         /* 1,4,5 under SQL semantics */
    CHECK(count_where("missing(x)") == 1);
    CHECK(count_where("!missing(x)") == 4);
    CHECK(count_where("mi(x, y)") == 2);       /* rows 3 and 4 */
    /* missing-literal comparisons are IS NULL tests in both modes */
    CHECK(count_where("x == .") == 1);
    CHECK(count_where("x != .") == 4);
    CHECK(count_where("x < .") == 4);          /* the classic not-missing idiom */
    CHECK(count_where("x >= .") == 1);
    CHECK(count_where("x == .a") == 1);        /* .a collapses to NULL */
}

TEST_CASE("statamissing mode: missing sorts above every number") {
    CHECK(count_where("x > 2", true) == 3);    /* 4,5 AND the missing row */
    CHECK(count_where("x < 99", true) == 4);   /* missing is NOT < 99 */
    CHECK(count_where("x <= .", true) == 5);   /* everything ≤ missing */
    CHECK(count_where("x != 2", true) == 4);   /* missing ≠ 2 counts */
}

TEST_CASE("string semantics: NULL behaves as empty string") {
    CHECK(count_where("s == \"\"") == 2);      /* row 3 ("") and row 4 (NULL) */
    CHECK(count_where("s != \"\"") == 3);
    CHECK(count_where("missing(s)") == 2);
    CHECK(count_where("s == \"bb\"") == 1);
    CHECK(eval_at("strlen(s)", 5) == "9");     /* bytes: h(1)+é(2)+é(2)+🦆(4) */
    CHECK(eval_at("ustrlen(s)", 5) == "4");    /* characters */
    CHECK(eval_at("upper(s)", 2) == "BB");
    CHECK(eval_at("upper(s)", 5) == "Héé🦆");    /* STR-1: ASCII-only fold, é/🦆 untouched */
    CHECK(eval_at("ustrupper(s)", 5) == "HÉÉ🦆"); /* ustrupper is Unicode-aware */
    CHECK(eval_at("lower(s)", 5) == "héé🦆");     /* ASCII lower leaves é untouched */
    CHECK(eval_at("s + \"!\"", 2) == "bb!");
    CHECK(eval_at("s + \"!\"", 4) == "!");     /* NULL string concats as "" */
    CHECK(eval_at("substr(s, 2, 1)", 2) == "b");
    CHECK(eval_at("substr(s, -1, 1)", 2) == "b"); /* negative from end */
    CHECK(eval_at("strpos(s, \"b\")", 2) == "1");
    CHECK(eval_at("string(x)", 2) == "2");   /* Stata %9.0g: "2", not "2.0" */
    CHECK(eval_at("real(\"3.5\")", 1) == "3.5");
}

TEST_CASE("arithmetic and functions match Stata definitions") {
    CHECK(eval_at("x + y", 1) == "11.0");
    CHECK(eval_at("x + y", 4) == "");          /* missing propagates */
    CHECK(eval_at("x / 2", 5) == "2.5");
    CHECK(eval_at("5 / 2", 1) == "2.5");       /* never integer division */
    CHECK(eval_at("2 ^ 10", 1) == "1024.0");
    CHECK(eval_at("mod(-7, 3)", 1) == "2.0");  /* Stata mod is nonnegative */
    CHECK(eval_at("mod(7, 3)", 1) == "1.0");
    CHECK(eval_at("ln(0)", 1) == "");          /* Stata: missing, not -inf */
    CHECK(eval_at("sqrt(-1)", 1) == "");
    CHECK(eval_at("x / 0", 1) == "");          /* NUM-1: 1/0 → missing, not inf */
    CHECK(eval_at("(-8) ^ 0.5", 1) == "");     /* NUM-1: non-real power → missing */
    /* numeric checks via strtod: the SQL text form varies by result type */
    CHECK(std::strtod(eval_at("int(-2.7)", 1).c_str(), nullptr) == -2.0);
    CHECK(std::strtod(eval_at("round(2.5)", 1).c_str(), nullptr) == 3.0);
    CHECK(std::strtod(eval_at("round(-2.5)", 1).c_str(), nullptr) == -2.0); /* NUM-2: ties → +inf */
    CHECK(std::strtod(eval_at("round(-0.5)", 1).c_str(), nullptr) == 0.0);
    CHECK(std::abs(std::strtod(eval_at("round(123.456, .01)", 1).c_str(), nullptr) -
                   123.46) < 1e-9);
    CHECK(eval_at("min(x, 3)", 5) == "3.0");
    CHECK(eval_at("max(x, y, 100)", 1) == "100.0");
    CHECK(eval_at("cond(x > 1, 7, 8)", 1) == "8");
    CHECK(eval_at("cond(x > 1, 7, 8)", 2) == "7");
    CHECK(eval_at("cond(x, 7, 8, 9)", 3) == "9"); /* numeric missing → 4th branch */
    CHECK(eval_at("cond(x, 7, 8)", 3) == "7");    /* 3-arg: missing → TRUE branch */
    CHECK(eval_at("inrange(x, 2, 4)", 2) == "1");
    CHECK(eval_at("inrange(x, 2, 4)", 3) == "0");     /* missing → 0 */
    CHECK(eval_at("inlist(x, 1, 5)", 5) == "1");
    CHECK(eval_at("inlist(x, 1, 5)", 2) == "0");
    /* comparisons assign as 1/0, missing input → missing result */
    CHECK(eval_at("x > 2", 5) == "1");
    CHECK(eval_at("x > 2", 1) == "0");
    CHECK(eval_at("x > 2", 3) == "");
}

TEST_CASE("date pseudo-literals and date functions on day counts") {
    CHECK(eval_at("td(01jan2020)", 1) == "21915");
    CHECK(eval_at("td(1 jan 1960)", 1) == "0");
    CHECK(eval_at("td(31dec1959)", 1) == "-1");
    CHECK(eval_at("tm(2026m1)", 1) == "792"); /* the audit's canonical value */
    CHECK(eval_at("tq(2026q2)", 1) == "265");
    CHECK(eval_at("ty(2026)", 1) == "2026");
    CHECK(eval_at("tc(01jan1960 00:00:01)", 1) == "1000");
    CHECK(eval_at("d - td(01jan2020)", 2) == "1");
    CHECK(count_where("d >= td(01jan2020) & d < td(05jan2020)") == 3);
    CHECK(eval_at("year(d)", 1) == "2020");
    CHECK(eval_at("month(d)", 1) == "1");
    CHECK(eval_at("day(d)", 2) == "2");
    CHECK(eval_at("dow(td(05jan2020))", 1) == "0"); /* a Sunday */
    CHECK(eval_at("mdy(2, 29, 2020)", 1) == eval_at("td(29feb2020)", 1));
    CHECK(eval_at("mdy(2, 30, 2020)", 1) == "");  /* DATE-1: invalid date → missing, no abort */
    CHECK(eval_at("mdy(13, 1, 2020)", 1) == "");
    CHECK(eval_at("mofd(td(15jun2026))", 1) == "797");
    CHECK(eval_at("dofm(tm(2026m1))", 1) == eval_at("td(01jan2026)", 1));
    CHECK(eval_at("yofd(d)", 5) == "2020");
}

TEST_CASE("errors are loud, anchored, and honest") {
    ExprSchema sch = test_schema();
    ExprResult r = translate_filter("nosuchvar > 1", sch, false);
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("nosuchvar") != std::string::npos);

    r = translate_filter("x > \"a\"", sch, false);
    CHECK_FALSE(r.ok);

    r = translate_expression("frobnicate(x)", sch, false);
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("parqit sql") != std::string::npos);

    r = translate_filter("x = 1", sch, false); /* single = */
    CHECK_FALSE(r.ok);

    r = translate_filter("s", sch, false); /* string as condition */
    CHECK_FALSE(r.ok);

    r = translate_expression("x +", sch, false);
    CHECK_FALSE(r.ok);

    r = translate_expression("td(notadate)", sch, false);
    CHECK_FALSE(r.ok);
}

TEST_CASE("audit fixes: Stata-faithful semantics (verified vs Stata 19.5)") {
    /* ^ is LEFT-associative (XLAT-3): 2^3^2 == (2^3)^2 == 64 */
    CHECK(std::strtod(eval_at("2^3^2", 1).c_str(), nullptr) == 64.0);
    CHECK(std::strtod(eval_at("4^3^2", 1).c_str(), nullptr) == 4096.0);
    CHECK(std::strtod(eval_at("2^-1", 1).c_str(), nullptr) == 0.5); /* signed exponent */

    /* string() is %9.0g, not raw CAST (XLAT-1 / PARITY-2) */
    CHECK(eval_at("string(42)", 1) == "42");
    CHECK(eval_at("string(2020)", 1) == "2020");
    CHECK(eval_at("string(10000000)", 1) == "1.00e+07");
    CHECK(eval_at("string(123456789)", 1) == "1.23e+08");
    CHECK(eval_at("string(1/3)", 1) == ".3333333");
    CHECK(eval_at("string(-0.03)", 1) == "-.03");
    CHECK(eval_at("string(1e100)", 1) == "1.0e+100");
    CHECK(eval_at("string(-1e100)", 1) == "-1.0e+100");
    CHECK(eval_at("string(.00009999999)", 1) == ".0001");
    CHECK(eval_at("string(.000123456)", 1) == ".0001235");
    CHECK(eval_at("string(123456.789)", 1) == "123456.8");
    CHECK(eval_at("string(9999999.9)", 1) == "1.00e+07");
    CHECK(eval_at("string(.0000123456)", 1) == ".0000123");
    CHECK(eval_at("string(.000009999999)", 1) == "1.00e-05");
    CHECK(eval_at("string(.)", 1) == ".");

    /* substr/strpos are BYTE-based (XLAT-2) — row 5 s = "héé🦆"
     *   bytes: h(1) é(2,3) é(4,5) 🦆(6,7,8,9) */
    const std::string repl = "\xEF\xBF\xBD";
    CHECK(eval_at("substr(s, 4, 2)", 5) == "é");    /* bytes 4-5 = the 2nd é */
    CHECK(eval_at("strpos(s, \"🦆\")", 5) == "6");  /* byte offset, not char 4 */
    CHECK(eval_at("substr(s, 2, 2)", 5) == "é");    /* bytes 2-3 = the 1st é */
    CHECK(eval_at("substr(s, 2, 1)", 5) == repl);    /* split UTF-8 start byte */
    CHECK(eval_at("substr(s, 3, 1)", 5) == repl);    /* split continuation byte */
    CHECK(eval_at("substr(s, 6, 4)", 5) == "🦆");   /* full 4-byte code point */
    CHECK(eval_at("substr(s, -4, 4)", 5) == "🦆");  /* negative from byte end */
    CHECK(eval_at("substr(s, 1, .)", 5) == "héé🦆");
    CHECK(eval_at("substr(s, ., 1)", 5) == "");
    CHECK(eval_at("substr(s, 0, 1)", 5) == "");

    /* logical operators: missing is TRUE (XLAT-5) — row 3 x is missing */
    CHECK(eval_at("x & 1", 3) == "1");
    CHECK(eval_at("x | 0", 3) == "1");
    CHECK(eval_at("!x", 3) == "0");
    CHECK(eval_at("x & 0", 3) == "0");
    /* `keep if x`: a missing value is kept (row 3); only x-1==0 (row 1) drops */
    CHECK(count_where("x - 1") == 4);

    /* comparisons-as-values are TOTAL (0/1) under statamissing (XLAT-9) */
    CHECK(eval_at("x == 2", 3, true) == "0");   /* . == 2 -> 0, not missing */
    CHECK(eval_at("x != 2", 3, true) == "1");
    CHECK(eval_at("x > 2", 3, true) == "1");    /* missing is large */

    /* cond() (XLAT-4): 3-arg missing condition -> TRUE branch; 4-arg -> 4th */
    CHECK(eval_at("cond(x, 7, 8)", 3) == "7");
    CHECK(eval_at("cond(x, 7, 8, 9)", 3) == "9");

    /* mod with a nonpositive modulus is missing (XLAT-6) */
    CHECK(eval_at("mod(7, -3)", 1) == "");
    CHECK(eval_at("mod(7, 0)", 1) == "");

    /* inrange with a missing bound (XLAT-7): missing lower = -inf, upper = +inf */
    CHECK(eval_at("inrange(x, ., 4)", 2) == "1");
    CHECK(eval_at("inrange(x, 2, .)", 2) == "1");
    CHECK(eval_at("inrange(x, ., .)", 3) == "0"); /* missing x -> 0 */

    /* real() of non-finite text is missing (PARITY-6) */
    CHECK(eval_at("real(\"inf\")", 1) == "");
    CHECK(eval_at("real(\"nan\")", 1) == "");
    CHECK(eval_at("real(\"2.5\")", 1) == "2.5");

    /* a user string literal may not smuggle the row-context marker (XLAT-8) */
    ExprResult bad =
        translate_expression("\"a__PARQIT_ROW__b\"", test_schema(), false);
    CHECK_FALSE(bad.ok);
}
