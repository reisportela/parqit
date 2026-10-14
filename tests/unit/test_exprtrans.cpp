/* Semantic tests: every translated expression is EXECUTED against DuckDB
 * and its result compared with Stata's documented behaviour. */
#include "doctest.h"

#include <cmath>
#include <cstdlib>

#include "duckdb.h"
#include "engine/exprtrans.hpp"
#include "engine/datefn.hpp"
#include "engine/session.hpp"

using namespace parqit;

static ExprSchema test_schema() {
    ExprSchema s;
    s.kinds = {{"x", 'n'}, {"y", 'n'}, {"s", 's'}, {"d", 'n'}, {"f", 'n'}};
    s.float_columns.insert("f");
    return s;
}

/* fixture rows (id, x, y, s, d, f):
 *   1: 1,   10,  "a",   21915 (=td(01jan2020)), 0.1f
 *   2: 2,   20,  "bb",  21916,                  0.3f
 *   3: NULL, 30, "",    NULL,                   NULL
 *   4: 4,  NULL, NULL,  21918,                  1.1f
 *   5: 5,   50,  "héé🦆", 21919,                2.5f
 * f is a FLOAT column (FLOAT-LIT-1): 0.1f/0.3f/1.1f are NOT the doubles 0.1,
 * 0.3, 1.1 (they are slightly above them); 2.5f is exact. */
static void make_fixture() {
    static bool done = false;
    if (done) return;
    Session &s = Session::instance();
    std::string err;
    REQUIRE_MESSAGE(s.exec("CREATE OR REPLACE TABLE __t AS SELECT * FROM (VALUES "
                           "(1, 1::DOUBLE, 10::DOUBLE, 'a', 21915, 0.1::FLOAT), "
                           "(2, 2::DOUBLE, 20::DOUBLE, 'bb', 21916, 0.3::FLOAT), "
                           "(3, NULL::DOUBLE, 30::DOUBLE, '', NULL, NULL::FLOAT), "
                           "(4, 4::DOUBLE, NULL::DOUBLE, NULL, 21918, 1.1::FLOAT), "
                           "(5, 5::DOUBLE, 50::DOUBLE, 'héé🦆', 21919, 2.5::FLOAT)"
                           ") t(id, x, y, s, d, f)",
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
    CHECK(eval_at("strpos(s, \"\")", 2) == "1");  /* nonempty haystack */
    CHECK(eval_at("strpos(s, \"\")", 3) == "0");  /* empty haystack */
    CHECK(eval_at("strpos(s, \"\")", 4) == "0");  /* NULL folds to empty */
    CHECK(eval_at("regexm(\"xyz\", s)", 4) == "1"); /* NULL pattern is Stata "" */
    CHECK(eval_at("string(x)", 2) == "2");   /* Stata %9.0g: "2", not "2.0" */
    CHECK(eval_at("real(\"3.5\")", 1) == "3.5");
}

TEST_CASE("EXPR-4: relational operators chain left-associatively") {
    /* `1 < x < 3000` parses as `(1 < x) < 3000` (a 0/1 result), never an error */
    CHECK(eval_at("1 < x < 3000", 2) == eval_at("(1 < x) < 3000", 2));
    CHECK(eval_at("(1 < x) < 3000", 2) == "1");
    /* left-assoc: (5<10)=1, then 1<3 -> 1; right-assoc would be 5<(10<3)=0 */
    CHECK(eval_at("5 < 10 < 3", 1) == "1");
    /* a column reference named like the internal row-context sentinel is a loud
     * error, not malformed SQL (INJID-1) */
    {
        ExprSchema sc;
        sc.kinds = {{"__PARQIT_ROW__", 'n'}};
        ExprResult r = translate_expression("__PARQIT_ROW__ + 1", sc, false);
        CHECK_FALSE(r.ok);
    }
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
    CHECK(std::strtod(eval_at("cond(x > 1, 7, 8)", 1).c_str(), nullptr) == 8.0);
    CHECK(std::strtod(eval_at("cond(x > 1, 7, 8)", 2).c_str(), nullptr) == 7.0);
    CHECK(std::strtod(eval_at("cond(x, 7, 8, 9)", 3).c_str(), nullptr) == 9.0); /* missing → 4th */
    CHECK(std::strtod(eval_at("cond(x, 7, 8)", 3).c_str(), nullptr) == 7.0);    /* missing → TRUE */
    CHECK(eval_at("inrange(x, 2, 4)", 2) == "1");
    CHECK(eval_at("inrange(x, 2, 4)", 3) == "0");     /* missing → 0 */
    CHECK(eval_at("inlist(x, 1, 5)", 5) == "1");
    CHECK(eval_at("inlist(x, 1, 5)", 2) == "0");
    /* comparisons assign as 1/0, missing input → missing result */
    CHECK(eval_at("x > 2", 5) == "1");
    CHECK(eval_at("x > 2", 1) == "0");
    CHECK(eval_at("x > 2", 3) == "");
}

TEST_CASE("INF-1: generated infinities are missing everywhere, like Stata") {
    /* native adjudication 2026-07-02: exp(710)=., 1e300*1e300=., 8e307+8e307=.,
     * (exp(710) < .)==0, (exp(710) == .)==1, di 1e309 is missing (rc 0) */
    CHECK(eval_at("exp(800)", 1) == "");
    CHECK(eval_at("1e300 * 1e300", 1) == "");
    CHECK(eval_at("8e307 + 8e307", 1) == "");
    CHECK(eval_at("-8e307 - 8e307", 1) == "");
    CHECK(eval_at("1e309", 1) == "");            /* out-of-range literal */
    CHECK(count_where("exp(800) < .") == 0);     /* Inf must not pass < . */
    CHECK(count_where("exp(800) == .") == 5);    /* and IS missing */
    /* finite results pass through the guard unchanged */
    CHECK(std::abs(std::strtod(eval_at("exp(1)", 1).c_str(), nullptr) -
                   2.718281828459045) < 1e-12);
}

TEST_CASE("loud rejections match native Stata r(198)/r(109)") {
    ExprSchema sch = test_schema();
    CHECK_FALSE(translate_expression("x == .A", sch, false).ok);  /* uppercase */
    ExprResult ext = translate_expression("x == .a", sch, false);
    CHECK_FALSE(ext.ok);
    CHECK(ext.error.find("extended-missing literals (.a-.z)") != std::string::npos);
    CHECK(translate_expression("x == .", sch, false).ok);
    CHECK_FALSE(translate_expression("x < 1.2.3", sch, false).ok);
    CHECK_FALSE(translate_expression("x == 1 || x == 2", sch, false).ok);
    CHECK_FALSE(translate_expression("x == 1 && x == 2", sch, false).ok);
    CHECK_FALSE(translate_expression("dow(s)", sch, false).ok);   /* r(109) */
    CHECK_FALSE(translate_expression("year(s)", sch, false).ok);
    CHECK_FALSE(translate_expression("mod(s, 2)", sch, false).ok);
    CHECK_FALSE(translate_expression("round(s)", sch, false).ok);
    CHECK_FALSE(translate_expression("ln(s)", sch, false).ok);
    CHECK_FALSE(translate_expression("sqrt(s)", sch, false).ok);
    CHECK_FALSE(translate_expression("exp(s)", sch, false).ok);
    CHECK_FALSE(translate_expression("trim(x)", sch, false).ok);
    CHECK_FALSE(translate_expression("ltrim(x)", sch, false).ok);
    CHECK_FALSE(translate_expression("rtrim(x)", sch, false).ok);
    CHECK_FALSE(translate_expression("subinstr(x, \"a\", \"b\", .)", sch,
                                     false)
                    .ok);
    CHECK_FALSE(translate_expression("cond(x, \"a\", \"b\", 9)", sch, false).ok);
    CHECK_FALSE(translate_expression("cond(x, 7, 8, \"z\")", sch, false).ok);
}

TEST_CASE("SUBINSTR-NULL-1 / DATE-2: null-safe strings, floored day counts") {
    /* subinstr with an empty/NULL needle returns the string unchanged
     * (native: subinstr("abc","","X",.) == "abc") */
    CHECK(eval_at("subinstr(s, \"\", \"X\", .)", 1) == "a");
    /* fractional day counts floor: day(-0.5) = day(-1) = 31dec1959 → 31,
     * day(21915.9) = day(21915) → 1 (native-adjudicated 2026-07-02) */
    CHECK(eval_at("day(21915.9)", 1) == "1");
    CHECK(eval_at("day(-0.5)", 1) == "31");
    CHECK(eval_at("dow(1.7)", 1) == "6");
    /* an out-of-range day count is row-local missing, never a query abort */
    CHECK(eval_at("year(3e9)", 1) == "");
    CHECK(eval_at("day(3e9)", 1) == "");
    CHECK(eval_at("mofd(3e9)", 1) == "");
}

TEST_CASE("date pseudo-literals and date functions on day counts") {
    CHECK(eval_at("td(01jan2020)", 1) == "21915");
    CHECK(eval_at("td(1 jan 1960)", 1) == "0");
    CHECK(eval_at("td(31dec1959)", 1) == "-1");
    CHECK(eval_at("tm(2026m1)", 1) == "792"); /* the audit's canonical value */
    CHECK(eval_at("tq(2026q2)", 1) == "265");
    CHECK(eval_at("ty(2026)", 1) == "2026");
    CHECK(eval_at("tc(01jan1960 00:00:01)", 1) == "1000");
    /* INF-1: arithmetic computes in double like every Stata expression, so a
     * day-count difference evaluates as 1.0, not integer 1 */
    CHECK(eval_at("d - td(01jan2020)", 2) == "1.0");
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

TEST_CASE("DATE-LIT-1: impossible calendar dates / 60s times are loud, not rolled") {
    ExprSchema sch = test_schema();
    /* an impossible day-of-month is a loud error (native r(198)), never silently
     * advanced by stata_days() arithmetic */
    CHECK_FALSE(translate_expression("td(31feb2020)", sch, false).ok);
    CHECK_FALSE(translate_expression("td(29feb2019)", sch, false).ok); /* not leap */
    CHECK_FALSE(translate_expression("td(00jan2020)", sch, false).ok); /* day 0 */
    CHECK_FALSE(translate_expression("td(32jan2020)", sch, false).ok);
    CHECK_FALSE(translate_expression("td(31apr2020)", sch, false).ok); /* apr=30 */
    CHECK_FALSE(translate_expression("td(31jun2020)", sch, false).ok); /* jun=30 */
    /* valid dates (including a true leap day) still parse */
    CHECK(translate_expression("td(29feb2020)", sch, false).ok); /* 2020 leap */
    CHECK(translate_expression("td(28feb2019)", sch, false).ok);
    CHECK(translate_expression("td(30apr2020)", sch, false).ok);
    CHECK(translate_expression("td(31dec2025)", sch, false).ok);
    /* the 60th second is not a valid tc()/tC() clock second (native r(198)),
     * and impossible hour/minute are rejected; fractional seconds remain valid */
    CHECK_FALSE(translate_expression("tc(01jan2020 00:00:60)", sch, false).ok);
    CHECK_FALSE(translate_expression("tC(01jan2020 00:00:60)", sch, false).ok);
    CHECK_FALSE(translate_expression("tc(01jan2020 24:00:00)", sch, false).ok);
    CHECK_FALSE(translate_expression("tc(01jan2020 00:60:00)", sch, false).ok);
    CHECK(translate_expression("tc(01jan2020 00:00:59)", sch, false).ok);
    CHECK(translate_expression("tc(01jan2020 00:00:59.5)", sch, false).ok);
    /* an impossible date inside a tc() literal is rejected via the same path */
    CHECK_FALSE(translate_expression("tc(31feb2020 00:00:00)", sch, false).ok);
    CHECK_FALSE(translate_expression("td(01jan0099)", sch, false).ok);
    CHECK(translate_expression("td(01jan0100)", sch, false).ok);
    CHECK_FALSE(
        translate_expression("tc(01jan2020 00:00:59.9999)", sch, false).ok);
    CHECK(translate_expression("tc(01jan2020 00:00:59.999)", sch, false).ok);
}

TEST_CASE("DATA-003: numeric literals are canonical Stata binary64") {
    const double got =
        std::strtod(eval_at("9007199254740993", 1).c_str(), nullptr);
    CHECK(got == 9007199254740992.0);
    ExprResult r = translate_expression("42", test_schema(), false);
    REQUIRE(r.ok);
    CHECK(r.sql == "42"); /* preserve native replace's range-based promotion */
}

TEST_CASE("MISS-1: missing() reports generated IEEE specials as Stata missing") {
    /* finite, non-missing values are not missing */
    CHECK(eval_at("missing(1)", 1) == "0");
    CHECK(eval_at("missing(x)", 1) == "0");
    /* a real SQL NULL column value is missing (id 3 has x = NULL) */
    CHECK(eval_at("missing(x)", 3) == "1");
    /* an expression that generates +Inf/NaN is missing — native Stata overflows
     * such results to '.', so missing() must not call them "not missing" */
    CHECK(eval_at("missing(exp(10000))", 1) == "1");
    CHECK(eval_at("missing(1/0)", 1) == "1");        /* div guard nulls it too */
    CHECK(eval_at("missing((-8)^0.5)", 1) == "1");   /* non-real power -> missing */
}

TEST_CASE("MISS-1 fast path: normalized columns use cheap IS NULL, others full") {
    ExprSchema sc;
    sc.kinds = {{"fx", 'n'}, {"gz", 'n'}};
    sc.normalized = {"fx"}; /* fx came through the boundary; gz did not */
    /* a bare ref to a normalized column needs only IS NULL (no per-row scan) */
    ExprResult a = translate_filter("missing(fx)", sc, false);
    REQUIRE(a.ok);
    CHECK(a.sql.find("IS NULL") != std::string::npos);
    CHECK(a.sql.find("isfinite") == std::string::npos);
    /* a non-normalized column (e.g. a gen result) keeps the full finite check */
    ExprResult b = translate_filter("missing(gz)", sc, false);
    REQUIRE(b.ok);
    CHECK(b.sql.find("isfinite") != std::string::npos);
    /* a compound expression over a normalized column still gets the full check —
     * the sql==quote_ident(col) guard defeats any stale col on the transformed
     * Val, so a generated special inside the expression is never missed */
    ExprResult c = translate_filter("missing(fx + 0)", sc, false);
    REQUIRE(c.ok);
    CHECK(c.sql.find("isfinite") != std::string::npos);
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
    CHECK(std::strtod(eval_at("cond(x, 7, 8)", 3).c_str(), nullptr) == 7.0);
    CHECK(std::strtod(eval_at("cond(x, 7, 8, 9)", 3).c_str(), nullptr) == 9.0);

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

TEST_CASE("ROWCTX-1: _n/_N mark the result so non-compiling callers can refuse") {
    /* The translator emits __PARQIT_ROW__/__PARQIT_NROWS__ placeholders that
     * only the view compiler resolves. Callers that apply a filter to an
     * already-compiled SELECT (count if, the list/head preview) must be able
     * to detect that and refuse with their own message — before the token
     * reaches DuckDB and comes back as a Binder Error naming an internal
     * name (charter §5/§6.12). */
    ExprResult f_row = translate_filter("_n <= 5", test_schema(), false);
    REQUIRE(f_row.ok);
    CHECK(f_row.uses_rowctx);

    ExprResult f_bign = translate_filter("_N > 1", test_schema(), false);
    REQUIRE(f_bign.ok);
    CHECK(f_bign.uses_rowctx);

    /* nested inside a function call and behind an operator, not just bare */
    ExprResult f_deep = translate_filter("mod(_n, 2) == 0 & x > 1", test_schema(), false);
    REQUIRE(f_deep.ok);
    CHECK(f_deep.uses_rowctx);

    /* an ordinary filter must NOT be flagged: the refusal has to be precise */
    ExprResult f_plain = translate_filter("x <= 5", test_schema(), false);
    REQUIRE(f_plain.ok);
    CHECK_FALSE(f_plain.uses_rowctx);

    /* the flag survives statamissing mode and the assignment entry point too */
    ExprResult f_sm = translate_filter("_n < 3", test_schema(), true);
    REQUIRE(f_sm.ok);
    CHECK(f_sm.uses_rowctx);

    ExprResult e_row = translate_expression("_n", test_schema(), false);
    REQUIRE(e_row.ok);
    CHECK(e_row.uses_rowctx);

    ExprResult e_plain = translate_expression("x + 1", test_schema(), false);
    REQUIRE(e_plain.ok);
    CHECK_FALSE(e_plain.uses_rowctx);

    /* a string literal that merely spells the marker is still rejected (XLAT-8),
     * so the flag can never be spoofed from user text */
    ExprResult spoof =
        translate_filter("s == \"__PARQIT_ROW__\"", test_schema(), false);
    CHECK_FALSE(spoof.ok);
}

/* ---- audit 2026-08-22 ---------------------------------------------------- */

/* real(<text>) evaluated against DuckDB; "" means NULL (Stata missing) */
static std::string real_of(const std::string &txt) {
    ExprResult r = translate_expression("real(s)", test_schema(), false);
    REQUIRE_MESSAGE(r.ok, r.error);
    Session &s = Session::instance();
    std::string v, err;
    bool ok = s.query_scalar("SELECT CAST((" + r.sql + ") AS VARCHAR) FROM (SELECT " +
                                 quote_literal(txt) + " AS s)",
                             &v, &err);
    REQUIRE_MESSAGE(ok, (err + " [sql: " + r.sql + "]"));
    return v;
}
static double real_num(const std::string &txt) {
    const std::string v = real_of(txt);
    REQUIRE_MESSAGE(!v.empty(), ("real(" + txt + ") unexpectedly missing"));
    return std::strtod(v.c_str(), nullptr);
}

TEST_CASE("REAL-GRAMMAR-1: real() follows Stata's literal grammar (A3-2)") {
    /* verified against StataNow 19.5 (local/audit_2026-08-22/impl/probe) */
    CHECK(real_of("2019_01") == "");     /* DuckDB digit groups: missing natively */
    CHECK(real_of("12_345_678") == "");
    CHECK(real_of("1_000.5") == "");
    CHECK(real_of("1e1_0") == "");
    CHECK(real_num("1d3") == 1000.0);    /* Fortran-style exponent: 1000 natively */
    CHECK(real_num("1D3") == 1000.0);
    CHECK(real_num("1d-3") == 0.001);
    CHECK(real_num("1.5d2") == 150.0);
    CHECK(real_num(" 2 ") == 2.0);
    CHECK(real_num("+3") == 3.0);
    CHECK(real_num(".5") == 0.5);
    CHECK(real_num("5.") == 5.0);
    CHECK(real_num("12.e3") == 12000.0);
    CHECK(real_num("1.e3") == 1000.0);
    CHECK(real_num("-.5") == -0.5);
    CHECK(real_num("1e+3") == 1000.0);
    CHECK(real_num("1.5e-3") == 0.0015);
    CHECK(real_num("0.0") == 0.0);
    CHECK(real_num("1E5") == 100000.0);
    CHECK(real_of("1e") == "");
    CHECK(real_of("0x1A") == "");
    CHECK(real_of("1,5") == "");
    CHECK(real_of("inf") == "");
    CHECK(real_of("nan") == "");
    CHECK(real_of("1e400") == "");
    CHECK(real_of("1,000") == "");
    CHECK(real_of("$5") == "");
    CHECK(real_of("--1") == "");
    CHECK(real_of("1.5.5") == "");
    CHECK(real_of(".e3") == "");
    CHECK(real_of("e3") == "");
    CHECK(real_of("+") == "");
    CHECK(real_of(".") == "");
    CHECK(real_of("") == "");
    CHECK(real_of("  ") == "");
    CHECK(real_of("1 2") == "");
    CHECK(real_of("\xef\xbc\x91\xef\xbc\x92") == ""); /* full-width digits */
}

/* a date function over a single day count d; "" means NULL */
static std::string date_of(const std::string &expr, const std::string &dval) {
    ExprResult r = translate_expression(expr, test_schema(), false);
    REQUIRE_MESSAGE(r.ok, r.error);
    Session &s = Session::instance();
    std::string v, err;
    bool ok = s.query_scalar("SELECT CAST((" + r.sql + ") AS VARCHAR) FROM (SELECT " +
                                 dval + "::DOUBLE AS d)",
                             &v, &err);
    REQUIRE_MESSAGE(ok, (err + " [sql: " + r.sql + "]"));
    return v;
}

TEST_CASE("DATE-DOMAIN-1: date functions are missing outside 01jan0100..31dec9999 and never abort (A3-3/A3-4)") {
    /* natively: year(2936549)=9999, year(2936550)=., year(-679350)=100,
     * year(-679351)=., year(2147483647)=., year(3000000)=. */
    CHECK(date_of("year(d)", "2936549") == "9999");
    CHECK(date_of("year(d)", "2936550") == "");
    CHECK(date_of("year(d)", "-679350") == "100");
    CHECK(date_of("year(d)", "-679351") == "");
    CHECK(date_of("year(d)", "2147483647") == ""); /* used to abort the query */
    CHECK(date_of("year(d)", "-2147483648") == "");
    CHECK(date_of("year(d)", "3000000") == "");
    CHECK(date_of("year(d)", "1e10") == "");
    CHECK(date_of("month(d)", "2936550") == "");
    CHECK(date_of("day(d)", "-679351") == "");
    CHECK(date_of("dow(d)", "2936549") == "5");
    CHECK(date_of("doy(d)", "-679350") == "1");
    CHECK(date_of("mofd(d)", "2936549") == "96479");
    CHECK(date_of("mofd(d)", "2936550") == "");
    CHECK(date_of("yofd(d)", "2936550") == "");
    CHECK(date_of("quarter(d)", "2936550") == "");
    /* mdy: year 100..9999 only; impossible dates missing (natively) */
    CHECK(date_of("mdy(1,1,99)", "0") == "");
    CHECK(date_of("mdy(1,1,100)", "0") == "-679350");
    CHECK(date_of("mdy(12,31,9999)", "0") == "2936549");
    CHECK(date_of("mdy(1,1,10000)", "0") == "");
    CHECK(date_of("mdy(2,30,2020)", "0") == "");
    CHECK(date_of("mdy(0,1,2020)", "0") == "");
    CHECK(date_of("mdy(1,0,2020)", "0") == "");
    CHECK(date_of("mdy(6,15,-1)", "0") == "");
    /* dofm: month counts jan0100..dec9999 = -22320..96479 */
    CHECK(date_of("dofm(d)", "-22320") == "-679350");
    CHECK(date_of("dofm(d)", "-22321") == "");
    CHECK(date_of("dofm(d)", "96479") == "2936519");
    CHECK(date_of("dofm(d)", "96480") == "");
    CHECK(date_of("dofm(d)", "123456.79") == "");
    CHECK(date_of("dofm(d)", "-1234567") == "");
    /* in-domain values unchanged */
    CHECK(date_of("year(d)", "21915") == "2020");
    CHECK(date_of("dofm(d)", "720") == "21915");
}

TEST_CASE("FLOAT-LIT-1: float columns compare with decimal literals in double (audit 2026-09-01, F2)") {
    /* f: 0.1f, 0.3f, NULL, 1.1f, 2.5f — native Stata is an all-double
     * evaluator: float(0.1) != 0.1, float(0.1) > 0.1, float(0.3) > 0.3 … */
    CHECK(count_where("f == 0.1") == 0);
    CHECK(count_where("f > 0.1") == 4);   /* 0.1f, 0.3f, 1.1f, 2.5f */
    CHECK(count_where("f >= 0.1") == 4);
    CHECK(count_where("f < 0.3") == 1);   /* 0.1f only: 0.3f > 0.3 */
    CHECK(count_where("f <= 0.3") == 1);
    CHECK(count_where("f != 1.1") == 4);
    CHECK(count_where("f == 2.5") == 1);  /* float-exact literal */
    CHECK(count_where("f > 1.1") == 2);   /* 1.1f (> 1.1) and 2.5f */
    CHECK(count_where("f < 1.1") == 2);
    CHECK(count_where("inrange(f, 0.1, 0.3)") == 1);
    CHECK(count_where("inlist(f, 0.1, 1.1)") == 0);
    CHECK(count_where("f == float(0.1)") == 1);
    CHECK(count_where("f == -0.1") == 0);
    CHECK(count_where("f > -0.1") == 4);
    CHECK(count_where("(f > 0.1) + (f > 1.1) == 2") == 2);
    CHECK(std::strtod(eval_at("cond(f > 0.1, 1, 0)", 1).c_str(), nullptr) == 1);
    /* the double column and integral literals are untouched */
    CHECK(count_where("x == 1") == 1);
    CHECK(count_where("x > 0.5") == 4);
    /* statamissing mode uses the same operands */
    CHECK(count_where("f > 0.1", true) == 5); /* NULL sorts high */
    CHECK(count_where("f == 0.1", true) == 0);
    /* generated SQL: only a non-integral literal float32 cannot hold is typed
     * DOUBLE, so integer key filters keep their exact untyped literal */
    ExprResult r1 = translate_filter("f == 0.1", test_schema(), false);
    REQUIRE(r1.ok);
    CHECK(r1.sql.find("AS DOUBLE") != std::string::npos);
    ExprResult r2 = translate_filter("f == 2.5", test_schema(), false);
    REQUIRE(r2.ok);
    CHECK(r2.sql.find("CAST(\"f\"") == std::string::npos);
    ExprResult r3 = translate_filter("x == 123456789", test_schema(), false);
    REQUIRE(r3.ok);
    CHECK(r3.sql.find("CAST(") == std::string::npos);
    ExprResult r4 = translate_filter("inrange(f, 0.1, 0.3)", test_schema(), false);
    REQUIRE(r4.ok);
    CHECK(count_where("inrange(f, 0.1, 0.3)", true) == 1);
    ExprResult r5 = translate_filter("f == -0.1", test_schema(), false);
    REQUIRE(r5.ok);
    CHECK(r5.sql.find("AS DOUBLE") != std::string::npos);
}

TEST_CASE("FLOAT-FN-1: float() rounds to float precision like native (audit 2026-09-01, F11)") {
    CHECK(eval_at("float(0.1)", 1) == "0.10000000149011612");
    CHECK(eval_at("float(x)", 1) == "1.0");
    CHECK(eval_at("float(x)", 3) == "");      /* missing stays missing */
    CHECK(eval_at("float(1e39)", 1) == "");    /* beyond Stata's float range */
    CHECK(eval_at("float(-1e39)", 1) == "");
    CHECK(eval_at("float(1.7e38)", 1) != "");
    CHECK(eval_at("float(f)", 2) == "0.30000001192092896");
    /* round() computes in double for a float column too (native: 0.1, 0.3) */
    CHECK(eval_at("round(f, 0.1)", 1) == "0.1");
    CHECK(eval_at("round(f, 0.1)", 2) == "0.30000000000000004"); /* 3 * 0.1, as native */
    CHECK(std::strtod(eval_at("round(f)", 5).c_str(), nullptr) == 3.0);
    ExprResult bad = translate_expression("float(s)", test_schema(), false);
    CHECK_FALSE(bad.ok);
    ExprResult bad2 = translate_expression("float(x, 1)", test_schema(), false);
    CHECK_FALSE(bad2.ok);
}

TEST_CASE("mod uses the correctly rounded remainder of its binary64 inputs") {
    /* Exact Fraction oracles; native quotient subtraction loses low bits. */
    CHECK(std::strtod(eval_at("mod(7, 0.00001)", 1).c_str(), nullptr) == 0x1.4f8b588de4091p-17);
    CHECK(std::strtod(eval_at("mod(0.3, 0.1)", 1).c_str(), nullptr) == 0.09999999999999998);
    CHECK(std::strtod(eval_at("mod(1, 0.1)", 1).c_str(), nullptr) == 0x1.9999999999996p-4);
    CHECK(std::strtod(eval_at("mod(-5.5, 2)", 1).c_str(), nullptr) == 0.5);
    CHECK(std::strtod(eval_at("mod(5.5, 2)", 1).c_str(), nullptr) == 1.5);
    CHECK(std::strtod(eval_at("mod(-7, 3)", 1).c_str(), nullptr) == 2.0);
    CHECK(std::strtod(eval_at("mod(7, 3)", 1).c_str(), nullptr) == 1.0);
    CHECK(eval_at("mod(7, 0)", 1) == "");   /* nonpositive modulus: missing */
    CHECK(eval_at("mod(7, -3)", 1) == "");
    CHECK(eval_at("mod(x, 2)", 3) == "");   /* missing x */
    CHECK(std::strtod(eval_at("mod(f, 0.1)", 2).c_str(), nullptr) ==
          std::strtod(eval_at("mod(0.30000001192092896, 0.1)", 2).c_str(), nullptr)); /* float operand in double */
}

/* STATAMISS-WARN-1: the static analysis behind the missing-value warning */
static std::vector<std::string> diffs(const std::string &e, bool filter = true,
                                      const std::string &guard = "") {
    return missing_rule_differences(e, test_schema(), filter, guard);
}
using Texts = std::vector<std::string>;

TEST_CASE("STATAMISS-WARN-1: comparisons whose missing outcome differs from Stata") {
    /* a condition differs only where Stata makes the comparison true */
    CHECK(diffs("x > 5") == Texts{"x > 5"});
    CHECK(diffs("x >= 5") == Texts{"x >= 5"});
    CHECK(diffs("x != 3") == Texts{"x != 3"});
    CHECK(diffs("x < 5").empty());
    CHECK(diffs("x <= 5").empty());
    CHECK(diffs("x == 5").empty());
    CHECK(diffs("5 < x") == Texts{"5 < x"});
    CHECK(diffs("5 > x").empty());
    CHECK(diffs("x == y") == Texts{"x == y"}); /* . == . is true in Stata */
    /* under an odd number of ! it differs where Stata makes it false */
    CHECK(diffs("!(x < 5)") == Texts{"x < 5"});
    CHECK(diffs("!(x > 5)").empty());
    /* literal-missing tests and strings are identical in both modes */
    CHECK(diffs("x == .").empty());
    CHECK(diffs("x < .").empty());
    CHECK(diffs("s == \"a\"").empty());
    CHECK(diffs("s > \"a\"").empty());
    /* idioms that settle the missing rows in both modes */
    CHECK(diffs("x > 5 & x < .").empty());
    CHECK(diffs("x > 5 & !missing(x)").empty());
    CHECK(diffs("(x > 5) & (x != .)").empty());
    CHECK(diffs("missing(x) | x > 5").empty());
    CHECK(diffs("x == . | x > 5").empty());
    CHECK(diffs("d > td(01jan2020) & d < .").empty());
    CHECK(diffs("x > 5 & y < .") == Texts{"x > 5"});
    CHECK(diffs("(x > 5 | y > 3) & x < .") == Texts{"y > 3"});
    CHECK(diffs("x < . | x > 5") == Texts{"x > 5"});      /* | does not guard */
    CHECK(diffs("missing(x) & x > 5") == Texts{"x > 5"}); /* & does not settle */
    /* an assigned value differs wherever an operand can be missing */
    CHECK(diffs("x < 5", false) == Texts{"x < 5"});
    CHECK(diffs("x > 5 & x < .", false).empty());
    CHECK(diffs("x > 5", false, "x < .").empty()); /* gen v = x > 5 if x < . */
    CHECK(diffs("x > 5", false, "!missing(x) & y > 0").empty());
    CHECK(diffs("x > 5", false, "x < . | y > 0") == Texts{"x > 5"});
    CHECK(diffs("cond(x > 5, 1, 0)", false) == Texts{"x > 5"});
    /* inside a function argument a comparison is a value, even in a condition */
    CHECK(diffs("cond(x < 5, 1, 0)") == Texts{"x < 5"});
    /* in order, without duplicates; a parse error reports nothing */
    CHECK(diffs("x > 5 | x > 5 | y >= 1") == Texts{"x > 5", "y >= 1"});
    CHECK(diffs("x > (").empty());
}

TEST_CASE("STATAMISS-WARN-1: the verdict agrees with both modes on real rows") {
    /* the fixture has a missing x (row 3) and a missing y (row 4) */
    for (const char *e : {"x < 5", "x <= 2", "x == 2", "!(x > 2)", "x > 1 & x < .",
                          "missing(x) | x > 2", "x > 1 & !missing(x)", "x == . | x >= 4"}) {
        CAPTURE(e);
        REQUIRE(diffs(e).empty());
        CHECK(count_where(e) == count_where(e, true));
    }
    for (const char *e : {"x > 2", "x != 2", "!(x < 3)", "y >= 20"}) {
        CAPTURE(e);
        REQUIRE(!diffs(e).empty());
        CHECK(count_where(e) != count_where(e, true));
    }
}


/* ---- BPLIM third round, 2026-10-13 ------------------------------------- */

TEST_CASE("ROWCTX-2: subscripts and _n/_N become window terms the compiler resolves") {
    ExprResult r = translate_expression("x[_n-1] + x[_n+2] - x[1] + x[_N] + x[_N-1] + x[_n]",
                                        test_schema(), false);
    REQUIRE_MESSAGE(r.ok, r.error);
    CHECK(r.uses_rowctx);
    /* six references, five windows: x[_n] is the column itself */
    CHECK(r.windows.size() == 5);
    CHECK(r.windows[0].sql == "lag(\"x\", 1) __PARQIT_OVER_ORD__");
    CHECK(r.windows[1].sql == "lead(\"x\", 2) __PARQIT_OVER_ORD__");
    CHECK(r.windows[2].sql == "nth_value(\"x\", 1) __PARQIT_OVER_FULL__");
    CHECK(r.windows[3].sql == "last_value(\"x\") __PARQIT_OVER_FULL__");
    CHECK(r.windows[4].sql == "nth_value(\"x\", 2) __PARQIT_OVER_FULL_DESC__");
    for (const auto &w : r.windows) CHECK(r.sql.find(w.placeholder) != std::string::npos);
    /* out of range is missing; the same term is shared; _n and _N keep their tokens */
    ExprResult z = translate_expression("x[0] + x[_N+1] + x[-2]", test_schema(), false);
    REQUIRE(z.ok);
    CHECK(z.windows.empty());
    ExprResult two = translate_expression("x[_n-1] * x[_n-1] + _n + _N", test_schema(), false);
    REQUIRE(two.ok);
    CHECK(two.windows.size() == 3);
    CHECK(two.sql.find("__PARQIT_ROW__") != std::string::npos);
    CHECK(two.sql.find("__PARQIT_NROWS__") != std::string::npos);
    /* refused forms: a non-literal offset, a string subscript, no bracket */
    CHECK_FALSE(translate_expression("x[_n-y]", test_schema(), false).ok);
    CHECK_FALSE(translate_expression("x[s]", test_schema(), false).ok);
    CHECK_FALSE(translate_expression("x[_n-1", test_schema(), false).ok);
    CHECK_FALSE(translate_expression("x[1.5]", test_schema(), false).ok);
}

TEST_CASE("AUDIT-157: oversized time operators and date masks are refused before overflow") {
    ExprSchema ts = test_schema();
    ts.kinds["t"] = 'n';
    ts.ts_time = "t";
    for (const std::string &op : {"L", "F", "D", "S"}) {
        CHECK_FALSE(translate_expression(op + "18446744073709551616.x", ts, false).ok);
        CHECK_FALSE(translate_expression(op + std::string(200, '9') + ".x", ts, false).ok);
    }
    CHECK(translate_expression("L00000000000000000000000001.x", ts, false).ok);
    CHECK(translate_expression("L10000.x", ts, false).ok);
    CHECK_FALSE(translate_expression("L10001.x", ts, false).ok);
    CHECK_FALSE(datefn::check_mask(std::string(200, '9') + "YMD", 'd').empty());
    CHECK_FALSE(datefn::check_mask("000YMD", 'd').empty());
    CHECK(datefn::check_mask("20YMD", 'd').empty());
}

TEST_CASE("AUDIT-158: date domains and clock components are row-local native values") {
    for (const std::string &fn : {"dofq", "dofh", "dofw"}) {
        CHECK(eval_at(fn + "(1e10)", 1).empty());
        CHECK(eval_at(fn + "(-1e100)", 1).empty());
    }
    CHECK(eval_at("hms(24,0,0)", 1).empty());
    CHECK(eval_at("hms(0,60,0)", 1).empty());
    CHECK(eval_at("hms(0,0,60)", 1).empty());
    CHECK(eval_at("hms(1.5,0,0)", 1) == "3600000.0");
    CHECK(eval_at("hms(0,0,59.9999)", 1) == "59999.0");
    CHECK(eval_at("dhms(2936550,0,0,0)", 1).empty());
    CHECK(eval_at("dhms(-679351,0,0,0)", 1).empty());
    CHECK(eval_at("dhms(.5,1.5,2.5,3.3333)", 1) == "3723333.0");
    CHECK(eval_at("mdyhms(1,1,2020,24,0,0)", 1).empty());
    CHECK(eval_at("hms(x,0,0)", 3).empty());
    CHECK(eval_at("hms(0,x,0)", 3).empty());
    CHECK(eval_at("dhms(x,0,0,0)", 3).empty());
    CHECK(eval_at("mdyhms(1,x,2020,0,0,0)", 3).empty());
    CHECK(eval_at("mdyhms(2,29,2019,0,0,0)", 1).empty());
    CHECK(eval_at("clock(\"15jan2020 10:30:00\", \"DMY hms\")", 1) == "1894703400000");
    CHECK_FALSE(translate_expression("td(4294967297jan2020)", test_schema(), false).ok);
    CHECK_FALSE(translate_expression("tm(2020m4294967297)", test_schema(), false).ok);
    CHECK_FALSE(translate_expression("tc(01jan2020 4294967296:00:00)", test_schema(), false).ok);
    ExprSchema ts = test_schema(); ts.ts_time = "d";
    CHECK_FALSE(translate_expression("tin(1e100,1e100)", ts, false).ok);
}

TEST_CASE("TSSET-1: L. F. D. S. are RANGE windows by time, refused without a declaration") {
    ExprSchema plain = test_schema();
    CHECK_FALSE(translate_expression("L.x", plain, false).ok);
    CHECK(translate_expression("L.x", plain, false).error.find("xtset") != std::string::npos);
    ExprSchema ts = test_schema();
    ts.kinds["id"] = 'n';
    ts.kinds["t"] = 'n';
    ts.ts_panel = "id";
    ts.ts_time = "t";
    ts.ts_delta = 1;
    ExprResult l1 = translate_expression("L.x", ts, false);
    REQUIRE_MESSAGE(l1.ok, l1.error);
    REQUIRE(l1.windows.size() == 1);
    CHECK(l1.windows[0].sql ==
          "max(\"x\") OVER (PARTITION BY \"id\" ORDER BY \"t\" RANGE BETWEEN 1 PRECEDING AND 1 PRECEDING)");
    /* the term itself, masked on a missing time (native: no lag there) */
    CHECK(l1.sql == "(CASE WHEN \"t\" IS NULL THEN NULL ELSE " + l1.windows[0].placeholder + " END)");
    ExprResult f2 = translate_expression("f2.x", ts, false);
    REQUIRE(f2.ok);
    CHECK(f2.windows[0].sql.find("RANGE BETWEEN 2 FOLLOWING AND 2 FOLLOWING") != std::string::npos);
    /* D2.x = x - 2 L.x + L2.x: two lag terms, signed, finite-guarded */
    ExprResult d2 = translate_expression("D2.x", ts, false);
    REQUIRE(d2.ok);
    CHECK(d2.windows.size() == 2);
    CHECK(d2.sql.find("parqit_finite(") == 0);
    CHECK(d2.sql.find("(-2 * __parqit_double(") != std::string::npos);
    /* LD.x = L.x - L2.x; S3.x = x - L3.x; delta scales the offsets */
    ExprResult ld = translate_expression("LD.x", ts, false);
    REQUIRE(ld.ok);
    CHECK(ld.windows.size() == 2);
    ts.ts_delta = 4;
    ExprResult s3 = translate_expression("S3.x", ts, false);
    REQUIRE(s3.ok);
    CHECK(s3.windows[0].sql.find("12 PRECEDING AND 12 PRECEDING") != std::string::npos);
    /* refused: the time and panel variables, a string, a missing count */
    CHECK_FALSE(translate_expression("L.t", ts, false).ok);
    CHECK_FALSE(translate_expression("L.id", ts, false).ok);
    CHECK_FALSE(translate_expression("L.s", ts, false).ok);
    /* a variable that merely looks like an operator is still a variable */
    ExprSchema odd = test_schema();
    odd.kinds["L2"] = 'n';
    ExprResult plainvar = translate_expression("L2 + 1", odd, false);
    REQUIRE_MESSAGE(plainvar.ok, plainvar.error);
    CHECK(plainvar.windows.empty());
}

TEST_CASE("TSSET-1: tin() and twithin() read the time variable's format") {
    ExprSchema ts = test_schema();
    ts.kinds["t"] = 'n';
    ts.ts_time = "t";
    ts.ts_delta = 1;
    ExprResult a = translate_filter("tin(2001, 2003)", ts, false);
    REQUIRE_MESSAGE(a.ok, a.error);
    CHECK(a.sql == "coalesce(\"t\" >= 2001 AND \"t\" <= 2003, FALSE)");
    ExprResult b = translate_filter("twithin(2001, 2003)", ts, false);
    REQUIRE(b.ok);
    CHECK(b.sql == "coalesce(\"t\" > 2001 AND \"t\" < 2003, FALSE)");
    ts.ts_time_fmt = "%td";
    ExprResult c = translate_filter("tin(01jan2020, 31dec2020)", ts, false);
    REQUIRE_MESSAGE(c.ok, c.error);
    CHECK(c.sql == "coalesce(\"t\" >= 21915 AND \"t\" <= 22280, FALSE)");
    ts.ts_time_fmt = "%tm";
    ExprResult d = translate_filter("tin(2020m1, 2020m12)", ts, false);
    REQUIRE_MESSAGE(d.ok, d.error);
    CHECK(d.sql == "coalesce(\"t\" >= 720 AND \"t\" <= 731, FALSE)");
    CHECK_FALSE(translate_filter("tin(2020m1, 2020m12)", test_schema(), false).ok);
    ts.ts_time_fmt = "%tq";
    CHECK_FALSE(translate_filter("tin(2020m1, 2020m12)", ts, false).ok);
}

TEST_CASE("DATEFN-1: the date functions translate and evaluate as native Stata") {
    make_fixture();
    /* d: 21915 = 01jan2020 (row 1), 21916 (row 2), NULL (row 3) */
    CHECK(eval_at("week(d)", 1) == "1");
    CHECK(eval_at("week(d)", 3) == "");
    CHECK(eval_at("halfyear(d)", 3) == "");
    CHECK(eval_at("qofd(d)", 1) == "240");
    CHECK(eval_at("hofd(d)", 1) == "120");
    CHECK(eval_at("wofd(d)", 1) == "3120");
    CHECK(eval_at("dofw(3120)", 1) == "21915");
    CHECK(eval_at("dofq(242)", 1) == "22097");
    CHECK(eval_at("dofh(121)", 1) == "22097");
    CHECK(eval_at("dofy(2020)", 1) == "21915");
    CHECK(eval_at("ym(2020, 7)", 1) == "726.0");
    CHECK(eval_at("ym(2020, 7.9)", 1) == "726.9");
    CHECK(eval_at("ym(2020, 0)", 1) == "");
    CHECK(eval_at("yw(2020, 53)", 1) == "");
    CHECK(eval_at("hh(1894703445678)", 1) == "10.0");
    CHECK(eval_at("mm(1894703445678)", 1) == "30.0");
    CHECK(eval_at("ss(1894703445678)", 1) == "45.678");
    CHECK(eval_at("dofc(1894703445678)", 1) == "21929.0");
    CHECK(eval_at("cofd(d)", 1) == "1893456000000.0");
    CHECK(eval_at("hms(10, 30, 45)", 1) == "37845000.0");
    CHECK(eval_at("datediff(td(31jan2020), td(29feb2020), \"month\")", 1) == "0.0");
    CHECK(eval_at("datediff(td(31jan2020), td(01mar2020), \"month\")", 1) == "1.0");
    CHECK(eval_at("datediff(td(22dec2020), td(26mar2020), \"month\")", 1) == "-8.0");
    CHECK(eval_at("datediff(td(01jan1960), td(01jan1961), \"year\")", 1) == "1.0");
    CHECK(eval_at("age(td(26mar2020), td(22dec2020))", 1) == "0.0");
    CHECK(eval_at("age(td(22dec2020), td(26mar2020))", 1) == "");
    CHECK_FALSE(translate_expression("datediff(d, d, \"week\")", test_schema(), false).ok);
    /* the text readers: native's rules on the oracle strings (nat_dates2.log) */
    CHECK(eval_at("date(\"15jan2020\", \"DMY\")", 1) == "21929");
    CHECK(eval_at("date(\"15jan2020 10:30\", \"DMY\")", 1) == "");
    CHECK(eval_at("date(\"20200115\", \"YMD\")", 1) == "21929");
    CHECK(eval_at("date(\"2006125\", \"YMD\")", 1) == "17140");
    CHECK(eval_at("date(\"2006125\", \"DMY\")", 1) == "-670049");
    CHECK(eval_at("date(\"1/15/08\", \"MDY\")", 1) == "");
    CHECK(eval_at("date(\"1/15/08\", \"MD20Y\")", 1) == "17546");
    CHECK(eval_at("date(\"1/15/08\", \"MDY\", 2050)", 1) == "17546");
    CHECK(eval_at("date(\"15jan99\", \"DMY\", 2020)", 1) == "14259");
    CHECK(eval_at("date(\"jan 15, 2020\", \"MDY\")", 1) == "21929");
    CHECK(eval_at("date(\"15 January 2020\", \"DMY\")", 1) == "21929");
    CHECK(eval_at("date(\"29feb2019\", \"DMY\")", 1) == "");
    CHECK(eval_at("date(\"15jan0099\", \"DMY\")", 1) == "");
    CHECK(eval_at("clock(\"15jan2020 10:30:45.678\", \"DMYhms\")", 1) == "1894703445678");
    CHECK(eval_at("clock(\"15jan2020 10:30:45.6789\", \"DMYhms\")", 1) == "");
    CHECK(eval_at("clock(\"15jan2020 10:30:45 pm\", \"DMYhms\")", 1) == "1894746645000");
    CHECK(eval_at("clock(\"15jan2020 24:00:00\", \"DMYhms\")", 1) == "");
    CHECK(eval_at("clock(\"20200115103045\", \"YMDhms\")", 1) == "1894703445000");
    CHECK(eval_at("clock(\"15jan2020\", \"DMY\")", 1) == "1894665600000");
    CHECK(eval_at("monthly(\"2020q3\", \"YM\")", 1) == "722");
    CHECK(eval_at("monthly(\"July 2020\", \"MY\")", 1) == "726");
    CHECK(eval_at("weekly(\"2020w53\", \"YW\")", 1) == "");
    CHECK(eval_at("quarterly(\"2020q3\", \"YQ\")", 1) == "242");
    CHECK(eval_at("yearly(\"2020\", \"Y\")", 1) == "2020");
    CHECK(eval_at("date(s, \"DMY\")", 3) == ""); /* an empty string */
    /* the third oracle (nat extra3): # takes one field anywhere, the rest as
     * the last letter; a stray word elsewhere is missing; the first am/pm
     * counts, a second is a word; dots separate a date; the fraction of a
     * second is the last field only when the others come out even */
    CHECK(eval_at("date(\"15jan2020 extra\", \"DMY#\")", 1) == "21929");
    CHECK(eval_at("date(\"15jan2020 extra\", \"DMY\")", 1) == "");
    CHECK(eval_at("date(\"extra 15jan2020\", \"#DMY\")", 1) == "21929");
    CHECK(eval_at("date(\"extra 15jan2020\", \"DMY#\")", 1) == "");
    CHECK(eval_at("date(\"15jan2020\", \"DMY#\")", 1) == "21929");
    CHECK(eval_at("date(\"15jan2020\", \"#DMY\")", 1) == "");
    CHECK(eval_at("date(\"15 jan 2020 foo bar\", \"DMY#\")", 1) == "21929");
    CHECK(eval_at("date(\"15jan2020 10:30 am pm\", \"DMY#\")", 1) == "21929");
    CHECK(eval_at("clock(\"15jan2020 10:30 am pm\", \"DMYhm\")", 1) == "");
    CHECK(eval_at("clock(\"15jan2020 10:30 am pm\", \"DMYhm#\")", 1) == "1894703400000");
    CHECK(eval_at("clock(\"15jan2020 extra 10:30\", \"DMY#hm\")", 1) == "1894703400000");
    CHECK(eval_at("clock(\"15jan2020 10:30 extra\", \"DMY#hm\")", 1) == "");
    CHECK(eval_at("clock(\"15.01.2020 10:30:45\", \"DMYhms\")", 1) == "1894703445000");
    CHECK(eval_at("clock(\"15.01.2020 10:30:45\", \"DMYhm#\")", 1) == "1894703400000");
    CHECK(eval_at("clock(\"2020.01.15\", \"YMD\")", 1) == "1894665600000");
    CHECK(eval_at("clock(\"2020.01.15\", \"YMDhm\")", 1) == "");
    CHECK(eval_at("clock(\"20200115103045.678\", \"YMDhms\")", 1) == "1894703445678");
    CHECK(eval_at("clock(\"15jan2020 10:30.5\", \"DMYhms\")", 1) == "1894703405000");
    CHECK(eval_at("clock(\"15jan2020 10:30.5\", \"DMYhm\")", 1) == "");
    CHECK(eval_at("clock(\"15jan2020 10.30\", \"DMYhm\")", 1) == "1894703400000");
    CHECK(eval_at("clock(\"15jan2020 103045.5\", \"DMYhms\")", 1) == "1894703445500");
    CHECK(eval_at("clock(\"15jan2020 1030\", \"DMYhm\")", 1) == "1894703400000");
    CHECK(eval_at("monthly(\"x2020m7\", \"YM\")", 1) == "");
    CHECK(eval_at("monthly(\"m7 2020\", \"MY\")", 1) == "");
    CHECK(eval_at("monthly(\"2020 m7\", \"YM\")", 1) == "726");
    CHECK(eval_at("monthly(\"2020j7\", \"YM\")", 1) == "726");
    CHECK(eval_at("monthly(\"2020m7 am\", \"YM\")", 1) == "");
    CHECK(eval_at("monthly(\"2020m7 extra\", \"YM\")", 1) == "");
    /* a mask that is not a literal, or malformed, is refused when translated */
    CHECK_FALSE(translate_expression("date(s, s)", test_schema(), false).ok);
    CHECK_FALSE(translate_expression("date(s, \"DMX\")", test_schema(), false).ok);
    CHECK_FALSE(translate_expression("clock(s, \"DMYms\")", test_schema(), false).ok);
    CHECK_FALSE(translate_expression("monthly(s, \"YQ\")", test_schema(), false).ok);
    CHECK_FALSE(translate_expression("date(x, \"DMY\")", test_schema(), false).ok);
}
