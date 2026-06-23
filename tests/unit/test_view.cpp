/* The view compiler under execution: every compiled pipeline runs against
 * DuckDB and the results are compared with Stata's semantics. */
#include "doctest.h"

#include <vector>

#include "duckdb.h"
#include "engine/session.hpp"
#include "engine/view.hpp"

using namespace parqit;

/* worker panel fixture: id, year, wage (NULLs), firm (string) */
static const char *kFixture =
    "SELECT * FROM (VALUES "
    " (1, 2019, 10.0::DOUBLE, 'a'),"
    " (1, 2020, 12.0::DOUBLE, 'a'),"
    " (2, 2019, 20.0::DOUBLE, 'b'),"
    " (2, 2020, NULL::DOUBLE, 'b'),"
    " (3, 2019, 30.0::DOUBLE, 'a'),"
    " (3, 2020, 33.0::DOUBLE, 'a')"
    ") t(id, year, wage, firm)";

static View make_view() {
    View v;
    std::vector<ViewCol> cols;
    for (const char *n : {"id", "year", "wage", "firm"}) {
        ViewCol c;
        c.name = n;
        c.kind = (std::string(n) == "firm") ? 's' : 'n';
        cols.push_back(c);
    }
    v.open(kFixture, cols, nlohmann::json::object(), nlohmann::json::object(), "",
           "test fixture");
    return v;
}

static std::string run_scalar(const std::string &sql) {
    Session &s = Session::instance();
    std::string v, err;
    bool qok = s.query_scalar(sql, &v, &err);
    REQUIRE_MESSAGE(qok, (err + " [sql: " + sql + "]"));
    return v;
}

static long long run_count(const View &v) {
    return std::strtoll(
        run_scalar("SELECT count(*) FROM (" + v.compile(false) + ")").c_str(),
        nullptr, 10);
}

TEST_CASE("keep/drop/filter/gen pipeline executes with Stata semantics") {
    View v = make_view();
    CHECK(v.filter("wage > 15", false, false).empty());
    CHECK(run_count(v) == 3); /* 20, 30, 33 — NULL drops */

    CHECK(v.gen("lwage", "double", "ln(wage)", "", false).empty());
    CHECK(v.keep_vars({"id", "year", "lwage"}).empty());
    CHECK(v.cols().size() == 3);

    /* the wildcard expander */
    View v2 = make_view();
    CHECK(v2.keep_vars({"id", "w*"}).empty());
    CHECK(v2.cols().size() == 2);
    CHECK(v2.cols()[1].name == "wage");

    /* unknown name is loud */
    View v3 = make_view();
    CHECK_FALSE(v3.keep_vars({"nope"}).empty());

    /* drop if keeps missing-condition rows (Stata semantics) */
    View v4 = make_view();
    CHECK(v4.filter("wage > 15", true, false).empty());
    CHECK(run_count(v4) == 3); /* 10, 12 and the NULL row stay */
}

TEST_CASE("gen with if-qualifier, replace, rename, order") {
    View v = make_view();
    CHECK(v.gen("hi", "", "1", "wage >= 20", false).empty());
    std::string nhi = run_scalar("SELECT count(hi) FROM (" + v.compile(false) + ")");
    CHECK(nhi == "3"); /* wages 20, 30, 33 — only qualifier rows nonmissing */

    CHECK(v.replace("wage", "wage * 2", "id == 1", false).empty());
    CHECK(run_scalar("SELECT sum(wage) FROM (" + v.compile(false) +
                     ") WHERE id = 1") == "44.0");
    CHECK(run_scalar("SELECT sum(wage) FROM (" + v.compile(false) +
                     ") WHERE id = 3") == "63.0");

    CHECK(v.rename("wage", "w").empty());
    CHECK_FALSE(v.rename("nope", "x").empty());
    CHECK_FALSE(v.rename("w", "id").empty()); /* collision is loud */

    CHECK(v.reorder({"firm"}).empty());
    CHECK(v.cols()[0].name == "firm");

    /* type mismatch on replace is loud */
    View v2 = make_view();
    CHECK_FALSE(v2.replace("firm", "wage + 1", "", false).empty());
}

TEST_CASE("sort + _n/_N compile to windows over the declared order") {
    View v = make_view();
    CHECK(v.sort({"wage"}, {true}).empty()); /* wage DESC */
    CHECK(v.gen("rank", "", "_n", "", false).empty());
    CHECK(v.gen("total", "", "_N", "", false).empty());
    /* highest wage (30) must get rank 1; NULL sorts last under DESC? DuckDB
     * default NULLS LAST for DESC — fine for the check below */
    CHECK(run_scalar("SELECT rank FROM (" + v.compile(false) +
                     ") WHERE wage = 33") == "1");
    CHECK(run_scalar("SELECT max(total) FROM (" + v.compile(false) + ")") == "6");

    /* keep if _n <= 2 over the order */
    CHECK(v.filter("_n <= 2", false, false).empty());
    CHECK(run_count(v) == 2);
}

TEST_CASE("PERF-1: row-context windows are emitted only when referenced") {
    /* _n alone must NOT drag in the blocking count(*) OVER () window: that would
     * turn a streaming plan into one that buffers the whole input. */
    View vn = make_view();
    CHECK(vn.gen("seq", "", "_n", "", false).empty());
    std::string sn = vn.compile(false);
    CHECK(sn.find("row_number()") != std::string::npos);
    CHECK(sn.find("count(*) OVER") == std::string::npos);

    /* _N alone needs the count window but not row_number(). */
    View vN = make_view();
    CHECK(vN.gen("tot", "", "_N", "", false).empty());
    std::string sN = vN.compile(false);
    CHECK(sN.find("count(*) OVER") != std::string::npos);
    CHECK(sN.find("row_number()") == std::string::npos);

    /* both referenced → both emitted (and the result is still correct). */
    View vb = make_view();
    CHECK(vb.gen("r", "", "_n / _N", "", false).empty());
    std::string sb = vb.compile(false);
    CHECK(sb.find("row_number()") != std::string::npos);
    CHECK(sb.find("count(*) OVER") != std::string::npos);

    /* keep if _n <= K is streaming too (no blocking count window). */
    View vf = make_view();
    CHECK(vf.filter("_n <= 2", false, false).empty());
    CHECK(vf.compile(false).find("count(*) OVER") == std::string::npos);
}

TEST_CASE("collapse: Stata statistics incl. exact percentile rule") {
    View v = make_view();
    std::vector<View::CollapseSpec> specs = {
        {"mean", "mw", "wage"}, {"sum", "sw", "wage"},   {"count", "n", "wage"},
        {"min", "lo", "wage"},  {"median", "med", "wage"}, {"p25", "q1", "wage"},
        {"first", "f", "wage"}, {"lastnm", "l", "wage"},
    };
    CHECK(v.sort({"id", "year"}, {false, false}).empty());
    CHECK(v.collapse(specs, {"firm"}).empty());
    std::string sql = v.compile(true);

    /* firm a: wages 10,12,30,33 → mean 21.25, sum 85, n 4, min 10,
     * median (12+30)/2=21, p25: np=1 → (x1+x2)/2 = 11, first 10, lastnm 33 */
    CHECK(run_scalar("SELECT mw FROM (" + sql + ") WHERE firm='a'") == "21.25");
    CHECK(run_scalar("SELECT sw FROM (" + sql + ") WHERE firm='a'") == "85.0");
    CHECK(run_scalar("SELECT n FROM (" + sql + ") WHERE firm='a'") == "4");
    CHECK(run_scalar("SELECT med FROM (" + sql + ") WHERE firm='a'") == "21.0");
    CHECK(run_scalar("SELECT q1 FROM (" + sql + ") WHERE firm='a'") == "11.0");
    CHECK(run_scalar("SELECT f FROM (" + sql + ") WHERE firm='a'") == "10.0");
    CHECK(run_scalar("SELECT l FROM (" + sql + ") WHERE firm='a'") == "33.0");
    /* firm b: wages 20, NULL → sum 20, n 1, median 20; first 20; lastnm 20 */
    CHECK(run_scalar("SELECT sw FROM (" + sql + ") WHERE firm='b'") == "20.0");
    CHECK(run_scalar("SELECT n FROM (" + sql + ") WHERE firm='b'") == "1");
    CHECK(run_scalar("SELECT med FROM (" + sql + ") WHERE firm='b'") == "20.0");

    /* (first) keeps a missing first value missing: sort so the NULL row of
     * firm b comes first */
    View v2 = make_view();
    CHECK(v2.sort({"year"}, {true}).empty()); /* 2020 first → b's NULL first */
    CHECK(v2.collapse({{"first", "fw", "wage"}}, {"firm"}).empty());
    CHECK(run_scalar("SELECT fw IS NULL FROM (" + v2.compile(false) +
                     ") WHERE firm='b'") == "true");
}

TEST_CASE("contract, duplicates drop, keep in, sample") {
    View v = make_view();
    CHECK(v.contract({"firm"}, "").empty());
    CHECK(run_scalar("SELECT _freq FROM (" + v.compile(false) +
                     ") WHERE firm='a'") == "4");

    View v2 = make_view();
    CHECK(v2.keep_vars({"firm"}).empty());
    CHECK(v2.duplicates_drop({}, false).empty());
    CHECK(run_count(v2) == 2);

    /* by-varlist dedup requires an order (determinism by design) */
    View v3 = make_view();
    CHECK_FALSE(v3.duplicates_drop({"id"}, true).empty());
    CHECK(v3.sort({"year"}, {false}).empty());
    CHECK(v3.duplicates_drop({"id"}, true).empty());
    CHECK(run_count(v3) == 3);
    CHECK(run_scalar("SELECT min(year) || '/' || max(year) FROM (" +
                     v3.compile(false) + ")") == "2019/2019");

    View v4 = make_view();
    CHECK(v4.sort({"id", "year"}, {false, false}).empty());
    CHECK(v4.keep_in(2, 4).empty());
    CHECK(run_count(v4) == 3);
    CHECK_FALSE(v4.keep_in(-1, 5).empty()); /* loud, charter §6.13 */
    CHECK_FALSE(v4.keep_in(5, 2).empty());

    View v5 = make_view();
    CHECK(v5.sample(3, true, 42).empty());
    CHECK(run_count(v5) == 3);
    View v6 = make_view();
    CHECK(v6.sample(50, false, 7).empty());
    CHECK(run_count(v6) == 3); /* 50% of 6 */
}

TEST_CASE("egen group statistics") {
    View v = make_view();
    CHECK(v.egen("tw", "total", "wage", {"firm"}, false).empty());
    CHECK(v.egen("mw", "mean", "wage", {"firm"}, false).empty());
    std::string sql = v.compile(false);
    CHECK(run_scalar("SELECT max(tw) FROM (" + sql + ") WHERE firm='a'") == "85.0");
    CHECK(run_scalar("SELECT max(tw) FROM (" + sql + ") WHERE firm='b'") == "20.0");
    CHECK_FALSE(v.egen("x", "mode", "wage", {}, false).empty()); /* loud */
}

TEST_CASE("show produces a readable CTE pipeline") {
    View v = make_view();
    CHECK(v.filter("wage > 15", false, false).empty());
    CHECK(v.gen("lw", "", "ln(wage)", "", false).empty());
    std::string s = v.show();
    CHECK(s.find("__parqit_s0") != std::string::npos);
    CHECK(s.find("keep if wage > 15") != std::string::npos);
    CHECK(s.find("gen lw = ln(wage)") != std::string::npos);
}

/* ---- two-table verbs ---------------------------------------------------- */

static View::UsingSide make_using(const char *sql,
                                  std::vector<std::pair<const char *, char>> cols) {
    View::UsingSide u;
    u.select_sql = sql;
    for (auto &c : cols) {
        ViewCol vc;
        vc.name = c.first;
        vc.kind = c.second;
        u.cols.push_back(vc);
    }
    u.vallabs = nlohmann::json::object();
    return u;
}

TEST_CASE("merge m:1: _merge semantics, NULL keys match, master wins") {
    View v = make_view(); /* ids 1,2,3 (×2 each) */
    std::vector<std::string> warns;
    /* using: firm chars for ids 1,2,4 + a NULL id row; also a conflicting
     * column named wage that must NOT come across */
    auto u = make_using(
        "SELECT * FROM (VALUES (1, 100.0, 9.9), (2, 200.0, 9.9), (4, 400.0, 9.9), "
        "(NULL, 999.0, 9.9)) t(id, tfp, wage)",
        {{"id", 'n'}, {"tfp", 'n'}, {"wage", 'n'}});
    std::string e = v.merge_with("m:1", {"id"}, u, {}, 0, "", false, &warns);
    REQUIRE_MESSAGE(e.empty(), e);
    REQUIRE(warns.size() == 1); /* wage collision warned */

    std::string sql = v.compile(false);
    CHECK(run_scalar("SELECT count(*) FROM (" + sql + ")") == "8"); /* 6 master + id4 + NULL */
    CHECK(run_scalar("SELECT count(*) FROM (" + sql + ") WHERE _merge = 3") == "4");
    CHECK(run_scalar("SELECT count(*) FROM (" + sql + ") WHERE _merge = 1") == "2"); /* id 3 */
    CHECK(run_scalar("SELECT count(*) FROM (" + sql + ") WHERE _merge = 2") == "2"); /* id 4 + NULL */
    CHECK(run_scalar("SELECT tfp FROM (" + sql + ") WHERE id = 1 LIMIT 1") == "100.0");
    CHECK(run_scalar("SELECT count(wage) FROM (" + sql + ") WHERE id = 1") == "2"); /* master wage kept */
}

TEST_CASE("merge keep(match) + keepusing + gen") {
    View v = make_view();
    std::vector<std::string> warns;
    auto u = make_using(
        "SELECT * FROM (VALUES (1, 100.0, 'x'), (9, 900.0, 'y')) t(id, tfp, extra)",
        {{"id", 'n'}, {"tfp", 'n'}, {"extra", 's'}});
    /* keep matches only (mask 4), keepusing(tfp), custom marker name */
    std::string e = v.merge_with("m:1", {"id"}, u, {"tfp"}, 4, "mk", false, &warns);
    REQUIRE_MESSAGE(e.empty(), e);
    std::string sql = v.compile(false);
    CHECK(run_scalar("SELECT count(*) FROM (" + sql + ")") == "2"); /* id 1 ×2 */
    CHECK(run_scalar("SELECT min(mk) || max(mk) FROM (" + sql + ")") == "33");
    /* extra not brought */
    bool has_extra = false;
    for (const auto &c : v.cols()) has_extra |= (c.name == "extra");
    CHECK_FALSE(has_extra);
}

TEST_CASE("merge m:m follows Stata's sequential pairing") {
    /* master: key a ×3 rows (v=1,2,3); using: key a ×2 rows (w=10,20).
     * Stata m:m → 3 rows: (1,10) (2,20) (3,20 clamped). */
    View v;
    std::vector<ViewCol> cols;
    ViewCol k, val;
    k.name = "k";
    k.kind = 's';
    val.name = "v";
    val.kind = 'n';
    cols.push_back(k);
    cols.push_back(val);
    v.open("SELECT * FROM (VALUES ('a', 1), ('a', 2), ('a', 3)) t(k, v)", cols,
           nlohmann::json::object(), nlohmann::json::object(), "", "mm fixture");
    CHECK(v.sort({"v"}, {false}).empty());
    std::vector<std::string> warns;
    auto u = make_using("SELECT * FROM (VALUES ('a', 10), ('a', 20)) t(k, w)",
                        {{"k", 's'}, {"w", 'n'}});
    std::string e = v.merge_with("m:m", {"k"}, u, {}, 0, "", true, &warns);
    REQUIRE_MESSAGE(e.empty(), e);
    std::string sql = v.compile(false);
    CHECK(run_scalar("SELECT count(*) FROM (" + sql + ")") == "3");
    CHECK(run_scalar("SELECT w FROM (" + sql + ") WHERE v = 1") == "10");
    CHECK(run_scalar("SELECT w FROM (" + sql + ") WHERE v = 2") == "20");
    CHECK(run_scalar("SELECT w FROM (" + sql + ") WHERE v = 3") == "20");
}

TEST_CASE("append: union by name, missing columns null, gen marker") {
    View v = make_view();
    std::vector<std::string> warns;
    std::vector<View::UsingSide> srcs;
    srcs.push_back(make_using(
        "SELECT * FROM (VALUES (7, 70.0, 'new1')) t(id, wage, extra)",
        {{"id", 'n'}, {"wage", 'n'}, {"extra", 's'}}));
    srcs.push_back(make_using("SELECT * FROM (VALUES (8, 2025)) t(id, year)",
                              {{"id", 'n'}, {"year", 'n'}}));
    std::string e = v.append_with(srcs, "src", &warns);
    REQUIRE_MESSAGE(e.empty(), e);
    std::string sql = v.compile(false);
    CHECK(run_scalar("SELECT count(*) FROM (" + sql + ")") == "8");
    CHECK(run_scalar("SELECT count(*) FROM (" + sql + ") WHERE src = 0") == "6");
    CHECK(run_scalar("SELECT extra FROM (" + sql + ") WHERE src = 1") == "new1");
    CHECK(run_scalar("SELECT count(firm) FROM (" + sql + ") WHERE src > 0") == "0");
    CHECK(run_scalar("SELECT year FROM (" + sql + ") WHERE src = 2") == "2025");

    /* type conflicts are loud */
    View v2 = make_view();
    std::vector<View::UsingSide> bad;
    bad.push_back(make_using("SELECT * FROM (VALUES ('oops')) t(wage)",
                             {{"wage", 's'}}));
    CHECK_FALSE(v2.append_with(bad, "", &warns).empty());
}

TEST_CASE("joinby: within-key cartesian product") {
    View v = make_view(); /* 2 rows per id */
    std::vector<std::string> warns;
    auto u = make_using(
        "SELECT * FROM (VALUES (1, 'p1'), (1, 'p2'), (1, 'p3'), (2, 'q1')) t(id, pat)",
        {{"id", 'n'}, {"pat", 's'}});
    std::string e = v.joinby_with({"id"}, u, &warns);
    REQUIRE_MESSAGE(e.empty(), e);
    std::string sql = v.compile(false);
    /* id1: 2×3=6, id2: 2×1=2, id3: no match → 8 */
    CHECK(run_scalar("SELECT count(*) FROM (" + sql + ")") == "8");
    CHECK(run_scalar("SELECT count(*) FROM (" + sql + ") WHERE id = 1") == "6");
}

/* ---- a view as the using side (merge/joinby/append across views) ------- */

static View::UsingSide using_of(const View &v) {
    View::UsingSide u;
    u.select_sql = v.compile(true);
    u.cols = v.cols();
    u.vallabs = v.vallabs();
    return u;
}

TEST_CASE("merge with another VIEW as the using side") {
    /* master: the worker panel, filtered; using: a per-id aggregate VIEW */
    View m = make_view();
    REQUIRE(m.filter("wage > 11", false, false).empty());

    View u = make_view();
    REQUIRE(u.collapse({{"mean", "mw", "wage"}, {"count", "nw", "wage"}}, {"id"})
                .empty());

    std::vector<std::string> warns;
    std::string e = m.merge_with("m:1", {"id"}, using_of(u), {}, 0, "", false,
                                 &warns);
    REQUIRE_MESSAGE(e.empty(), e);
    std::string sql = m.compile(false);
    /* master rows surviving the filter: wages 12,20,30,33 (4 rows);
     * all ids 1..3 exist in the aggregate → _merge==3 for all of them;
     * id 2 has rows in u (mean of 20) but only one master row kept */
    CHECK(run_scalar("SELECT count(*) FROM (" + sql + ") WHERE _merge = 3") == "4");
    CHECK(run_scalar("SELECT mw FROM (" + sql + ") WHERE id = 1 LIMIT 1") == "11.0");
    CHECK(run_scalar("SELECT nw FROM (" + sql + ") WHERE id = 2 LIMIT 1") == "1");

    /* nested WITH: the using view's CTE names shadow the master's inside
     * the subquery — prove a SELF-merge compiles and runs */
    View s1 = make_view();
    REQUIRE(s1.collapse({{"mean", "mw", "wage"}}, {"id"}).empty());
    View s2 = s1; /* same pipeline, same CTE names */
    std::vector<std::string> w2;
    std::string e2 = s1.merge_with("1:1", {"id"}, using_of(s2), {}, 0, "", true, &w2);
    REQUIRE_MESSAGE(e2.empty(), e2);
    CHECK(run_scalar("SELECT count(*) FROM (" + s1.compile(false) + ")") == "3");
}

TEST_CASE("joinby and append with views as sources") {
    View m = make_view();
    View pats = make_view();
    REQUIRE(pats.keep_vars({"id", "firm"}).empty());
    REQUIRE(pats.rename("firm", "tag").empty());
    std::vector<std::string> warns;
    REQUIRE_MESSAGE(m.joinby_with({"id"}, using_of(pats), &warns).empty(), "joinby");
    /* per id: 2 master rows × 2 using rows = 4; ids 1..3 → 12 */
    CHECK(run_scalar("SELECT count(*) FROM (" + m.compile(false) + ")") == "12");

    View a = make_view();
    View b = make_view();
    REQUIRE(b.filter("id == 1", false, false).empty());
    std::vector<View::UsingSide> srcs;
    srcs.push_back(using_of(b));
    REQUIRE_MESSAGE(a.append_with(std::move(srcs), "src", &warns).empty(), "append");
    CHECK(run_scalar("SELECT count(*) FROM (" + a.compile(false) + ")") == "8");
    CHECK(run_scalar("SELECT count(*) FROM (" + a.compile(false) +
                     ") WHERE src = 1") == "2");
}
