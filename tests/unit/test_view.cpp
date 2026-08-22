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
    CHECK(std::strtod(run_scalar("SELECT rank FROM (" + v.compile(false) +
                                 ") WHERE wage = 33")
                          .c_str(),
                      nullptr) == 1.0);
    CHECK(std::strtod(run_scalar("SELECT max(total) FROM (" + v.compile(false) +
                                 ")")
                          .c_str(),
                      nullptr) == 6.0);

    /* keep if _n <= 2 over the order */
    CHECK(v.filter("_n <= 2", false, false).empty());
    CHECK(run_count(v) == 2);
}

TEST_CASE("untyped numeric gen is a physical double") {
    View v = make_view();
    REQUIRE(v.gen("z", "", "42", "", false).empty());
    CHECK(v.cols().back().meta_type == "double");
    CHECK(run_scalar("SELECT typeof(z) FROM (" + v.compile(false) + ") LIMIT 1") ==
          "DOUBLE");
}

TEST_CASE("explicit float gen maps out-of-range values to missing") {
    View v = make_view();
    REQUIRE(v.gen("f_big", "float", "1e300", "", false).empty());
    REQUIRE(v.gen("f_exp", "float", "exp(700)", "", false).empty());
    REQUIRE(v.gen("f_neg", "float", "-1e39", "", false).empty());
    REQUIRE(v.gen("f_ok", "float", "1e38", "", false).empty());
    const std::string sql = v.compile(false);
    CHECK(run_scalar("SELECT f_big IS NULL AND f_exp IS NULL AND f_neg IS NULL "
                     "FROM (" + sql + ") LIMIT 1") == "true");
    CHECK(run_scalar("SELECT typeof(f_ok) FROM (" + sql + ") LIMIT 1") == "FLOAT");
    CHECK(std::strtod(run_scalar("SELECT f_ok FROM (" + sql + ") LIMIT 1").c_str(),
                      nullptr) > 9.9e37);
}

TEST_CASE("replace storage is committed before later lazy expressions") {
    View d = make_view();
    REQUIRE(d.replace("wage", "4000000000", "", false).empty());
    REQUIRE(d.coerce_numeric_column("wage", "double").empty());
    REQUIRE(d.gen("square", "double", "wage*wage", "", false).empty());
    CHECK(run_scalar("SELECT typeof(wage) FROM (" + d.compile(false) +
                     ") LIMIT 1") == "DOUBLE");
    CHECK(run_scalar("SELECT square FROM (" + d.compile(false) +
                     ") LIMIT 1") == "1.6e+19");

    View f = make_view();
    REQUIRE(f.replace("wage", "16777217", "", false).empty());
    REQUIRE(f.coerce_numeric_column("wage", "float").empty());
    REQUIRE(f.gen("after", "double", "wage", "", false).empty());
    CHECK(run_scalar("SELECT typeof(wage) FROM (" + f.compile(false) +
                     ") LIMIT 1") == "FLOAT");
    CHECK(run_scalar("SELECT after FROM (" + f.compile(false) +
                     ") LIMIT 1") == "16777216.0");
}

TEST_CASE("replace of a pending sort key bakes old order and truncates marker") {
    View v = make_view();
    REQUIRE(v.sort({"wage"}, {false}).empty());
    REQUIRE(v.replace("wage", "-wage", "", false).empty());
    CHECK(v.sort_keys().empty());
    REQUIRE(v.gen("seq", "", "_n", "", false).empty());
    CHECK(std::strtod(run_scalar("SELECT seq FROM (" + v.compile(false) +
                                 ") WHERE wage = -33")
                          .c_str(),
                      nullptr) == 5.0);
}

TEST_CASE("group rename validates then commits and supports cycles") {
    View bad = make_view();
    CHECK_FALSE(bad.rename_many({"id", "year"}, {"x", "x"}).empty());
    CHECK(bad.cols()[0].name == "id");
    CHECK(bad.cols()[1].name == "year");

    View cyc = make_view();
    REQUIRE(cyc.rename_many({"id", "year"}, {"year", "id"}).empty());
    CHECK(cyc.cols()[0].name == "year");
    CHECK(cyc.cols()[1].name == "id");
    CHECK(run_scalar("SELECT year FROM (" + cyc.compile(false) +
                     ") WHERE id = 2019 LIMIT 1") == "1");
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

TEST_CASE("join key validation classifies missing names and type mismatches") {
    View v = make_view();
    auto numeric = make_using("SELECT 1 AS id", {{"id", 'n'}});
    auto string = make_using("SELECT '1' AS id", {{"id", 's'}});

    bool type_mismatch = true;
    std::string e =
        v.join_keys_error("merge", {"nosuch"}, numeric, &type_mismatch);
    CHECK(e == "merge: key nosuch not found in the master view");
    CHECK_FALSE(type_mismatch);

    type_mismatch = true;
    e = v.join_keys_error("merge", {"year"}, numeric, &type_mismatch);
    CHECK(e == "merge: key year not found in the using data");
    CHECK_FALSE(type_mismatch);

    type_mismatch = false;
    e = v.join_keys_error("joinby", {"id"}, string, &type_mismatch);
    CHECK(e == "joinby: key id is numeric in master but string in using");
    CHECK(type_mismatch);

    type_mismatch = true;
    CHECK(v.join_keys_error("merge", {"id"}, numeric, &type_mismatch).empty());
    CHECK_FALSE(type_mismatch);
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

TEST_CASE("legacy engine m:m helper follows the clamped sequential rule") {
    /* The public lazy command is refused because a plan cannot retain native
     * physical within-key row order.  Keep the lower-level helper pinned while
     * older serialized plans/tests may still exercise its deterministic rule. */
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

TEST_CASE("append bakes the master's pending order and clears sortedby") {
    View v;
    ViewCol x;
    x.name = "x";
    v.open("SELECT * FROM (VALUES (2), (1)) t(x)", {x},
           nlohmann::json::object(), nlohmann::json::object(), "", "append order");
    REQUIRE(v.sort({"x"}, {false}).empty());
    std::vector<View::UsingSide> srcs;
    srcs.push_back(make_using("SELECT * FROM (VALUES (0), (3)) t(x)", {{"x", 'n'}}));
    std::vector<std::string> warns;
    REQUIRE(v.append_with(std::move(srcs), "", &warns).empty());
    CHECK(v.sort_keys().empty());
    CHECK(run_scalar("SELECT string_agg(CAST(x AS VARCHAR), ',' ORDER BY rowid) "
                     "FROM (SELECT *, row_number() OVER () AS rowid FROM (" +
                     v.compile(false) + "))") == "1,2,0,3");
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

TEST_CASE("dropping a sort-key column bakes the order and truncates the sort") {
    /* native Stata (verified): `sort y` then `drop y` keeps the rows in their
     * y-sorted PHYSICAL order and clears sortedby — no error, no reshuffle.
     * The old plan kept the dropped column in the final ORDER BY and every
     * later materialisation died with a raw DuckDB Binder Error. */
    View v = make_view();
    CHECK(v.sort({"wage"}, {true}).empty()); /* wage DESC */
    CHECK(v.drop_vars({"wage"}).empty());
    CHECK(v.sort_keys().empty()); /* first (only) key dropped -> cleared */
    std::string sql = v.compile(true);
    /* the ordering is baked into the projection stage's source subquery */
    CHECK(sql.find("ORDER BY \"wage\" DESC NULLS LAST") != std::string::npos);
    Session &s = Session::instance();
    duckdb_result res;
    std::string err;
    REQUIRE_MESSAGE(s.query(sql, &res, &err), err);
    REQUIRE(duckdb_row_count(&res) == 6);
    /* wage order 33,30,20,12,10,NULL → first row (id 3, 2020), the NULL-wage
     * row (id 2, 2020) last — preserve_insertion_order carries the baked
     * subquery order through the projection */
    CHECK(duckdb_value_int64(&res, 0, 0) == 3);
    CHECK(duckdb_value_int64(&res, 1, 0) == 2020);
    CHECK(duckdb_value_int64(&res, 0, 5) == 2);
    CHECK(duckdb_value_int64(&res, 1, 5) == 2020);
    duckdb_destroy_result(&res);

    /* prefix truncation: only the keys from the dropped one on are forgotten */
    View v2 = make_view();
    CHECK(v2.sort({"id", "wage", "year"}, {false, false, false}).empty());
    CHECK(v2.keep_vars({"id", "year", "firm"}).empty());
    REQUIRE(v2.sort_keys().size() == 1);
    CHECK(v2.sort_keys()[0] == "\"id\"");
    /* _n windows and keep-in still see a consistent sort state */
    CHECK(v2.gen("r", "", "_n", "", false).empty());
    CHECK(v2.keep_in(1, 2).empty());
    CHECK(run_count(v2) == 2);

    /* keeping every sort key bakes nothing and keeps the sort intact */
    View v3 = make_view();
    CHECK(v3.sort({"id"}, {false}).empty());
    CHECK(v3.keep_vars({"id", "wage"}).empty());
    REQUIRE(v3.sort_keys().size() == 1);
    CHECK(v3.compile(false).find("ORDER BY") == std::string::npos);
}

TEST_CASE("no-varlist duplicates drop folds '' with NULL and NaN with NULL") {
    /* a ('', NULL) string pair — or NaN beside NULL — is reachable
     * mid-pipeline (after merge/append) even though the boundary normalizes
     * sources; each pair must count as ONE duplicate row, like native Stata
     * and like the varlist branch (GROUPKEY-1). */
    View v;
    std::vector<ViewCol> cols;
    ViewCol cs, cx;
    cs.name = "s";
    cs.kind = 's';
    cx.name = "x";
    cx.kind = 'n';
    cols.push_back(cs);
    cols.push_back(cx);
    v.open("SELECT * FROM (VALUES ('', 1.0::DOUBLE), (NULL, 1.0::DOUBLE), "
           "('a', 'NaN'::DOUBLE), ('a', NULL::DOUBLE)) t(s, x)",
           cols, nlohmann::json::object(), nlohmann::json::object(), "",
           "dup fixture");
    CHECK(v.duplicates_drop({}, false).empty());
    std::string sql = v.compile(false);
    /* the normalized-key dedupe replaced the raw SELECT DISTINCT */
    CHECK(sql.find("SELECT DISTINCT") == std::string::npos);
    CHECK(sql.find("nullif(\"s\", '')") != std::string::npos);
    CHECK(run_count(v) == 2);
}

TEST_CASE("two-table verbs clear the normalized flag on string columns") {
    /* merge/append/joinby introduce SQL NULLs into carried string columns
     * (unmatched/absent rows); the MISS-1 flag must not survive, or a lazy
     * save would skip the coalesce('') guard and write NULL strings. Numeric
     * columns keep the flag: a join/union cannot create NaN/Inf. */
    auto normalized_view = []() {
        View v;
        std::vector<ViewCol> cols;
        for (const char *n : {"id", "wage", "firm"}) {
            ViewCol c;
            c.name = n;
            c.kind = (std::string(n) == "firm") ? 's' : 'n';
            c.normalized = true; /* as the lazy boundary marks them */
            cols.push_back(c);
        }
        v.open("SELECT * FROM (VALUES (1, 10.0, 'a'), (2, 20.0, 'b')) "
               "t(id, wage, firm)",
               cols, nlohmann::json::object(), nlohmann::json::object(), "",
               "normalized fixture");
        return v;
    };
    auto flag_of = [](const View &v, const char *n) {
        for (const auto &c : v.cols())
            if (c.name == n) return c.normalized;
        return false;
    };
    std::vector<std::string> warns;

    View m = normalized_view();
    auto u = make_using("SELECT * FROM (VALUES (1, 'x')) t(id, tag)",
                        {{"id", 'n'}, {"tag", 's'}});
    u.cols[0].normalized = u.cols[1].normalized = true;
    REQUIRE(m.merge_with("m:1", {"id"}, u, {}, 0, "", true, &warns).empty());
    CHECK_FALSE(flag_of(m, "firm")); /* master string: cleared */
    CHECK_FALSE(flag_of(m, "tag"));  /* using string: cleared */
    CHECK(flag_of(m, "wage"));       /* numerics keep the flag */
    CHECK(flag_of(m, "id"));

    View a = normalized_view();
    auto s1 = make_using("SELECT * FROM (VALUES (7, 'n1')) t(id, extra)",
                         {{"id", 'n'}, {"extra", 's'}});
    s1.cols[0].normalized = s1.cols[1].normalized = true;
    std::vector<View::UsingSide> srcs;
    srcs.push_back(s1);
    REQUIRE(a.append_with(std::move(srcs), "", &warns).empty());
    CHECK_FALSE(flag_of(a, "firm"));
    CHECK_FALSE(flag_of(a, "extra"));
    CHECK(flag_of(a, "wage"));

    View j = normalized_view();
    auto uj = make_using("SELECT * FROM (VALUES (1, 'p')) t(id, pat)",
                         {{"id", 'n'}, {"pat", 's'}});
    uj.cols[1].normalized = true;
    REQUIRE(j.joinby_with({"id"}, uj, &warns).empty());
    CHECK_FALSE(flag_of(j, "firm"));
    CHECK_FALSE(flag_of(j, "pat"));
    CHECK(flag_of(j, "wage"));
}

TEST_CASE("a failed append leaves the view untouched (validate-then-mutate)") {
    /* a kind conflict in source 2 must not leave source 1's value labels
     * already merged into the abandoned view */
    View v = make_view();
    std::vector<std::string> warns;
    std::vector<View::UsingSide> srcs;
    auto ok = make_using("SELECT * FROM (VALUES (7)) t(id)", {{"id", 'n'}});
    ok.vallabs["lab1"] = {{"entries", nlohmann::json::array(
                              {nlohmann::json::array({1, "one"})})}};
    srcs.push_back(ok);
    srcs.push_back(make_using("SELECT * FROM (VALUES ('oops')) t(wage)",
                              {{"wage", 's'}})); /* kind conflict */
    CHECK_FALSE(v.append_with(std::move(srcs), "", &warns).empty());
    CHECK(v.vallabs().empty()); /* source 1's labels were NOT merged */
    CHECK(v.n_stages() == 0);
    CHECK(v.cols().size() == 4);
    CHECK(warns.empty());
}

TEST_CASE("helper names dodge using-side columns in two-table verbs") {
    /* a using column literally named like a generated helper must not make
     * the compiled join reference ambiguous (charter §6.12) */
    View v = make_view();
    std::vector<std::string> warns;
    auto u = make_using("SELECT * FROM (VALUES (1, 5.0)) t(id, __parqit_um_2)",
                        {{"id", 'n'}, {"__parqit_um_2", 'n'}});
    std::string e = v.merge_with("m:1", {"id"}, u, {}, 0, "", false, &warns);
    REQUIRE_MESSAGE(e.empty(), e);
    std::string sql = v.compile(false);
    CHECK(run_scalar("SELECT count(*) FROM (" + sql + ")") == "6");
    CHECK(run_scalar("SELECT \"__parqit_um_2\" FROM (" + sql +
                     ") WHERE id = 1 LIMIT 1") == "5.0");
    CHECK(run_scalar("SELECT count(*) FROM (" + sql + ") WHERE _merge = 3") ==
          "2");
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

TEST_CASE("pivot decomposition: collapse + reshape_wide, and the snapshot restore") {
    /* parqit pivot compiles to exactly these two stages (cmd_view_pivot);
     * pin the composition here so the decomposition cannot drift */
    View v = make_view();
    REQUIRE(v.collapse({{"mean", "mw", "wage"}, {"count", "n", "wage"}},
                       {"firm", "year"})
                .empty());
    REQUIRE(v.reshape_wide({"mw", "n"}, {"firm"}, "year", {"2019", "2020"}, false)
                .empty());
    /* manifest: i, then stubs interleaved j-major */
    REQUIRE(v.cols().size() == 5);
    CHECK(v.cols()[0].name == "firm");
    CHECK(v.cols()[1].name == "mw2019");
    CHECK(v.cols()[2].name == "n2019");
    CHECK(v.cols()[3].name == "mw2020");
    CHECK(v.cols()[4].name == "n2020");
    std::string sql = v.compile(false);
    /* firm a: 2019 mean(10,30)=20 n 2; 2020 mean(12,33)=22.5 n 2 */
    CHECK(run_scalar("SELECT mw2019 FROM (" + sql + ") WHERE firm='a'") == "20.0");
    CHECK(run_scalar("SELECT mw2020 FROM (" + sql + ") WHERE firm='a'") == "22.5");
    CHECK(run_scalar("SELECT n2019 FROM (" + sql + ") WHERE firm='a'") == "2");
    /* firm b 2020: only the NULL wage → mean NULL, count 0 (not a lost row) */
    CHECK(run_scalar("SELECT mw2020 IS NULL FROM (" + sql + ") WHERE firm='b'") ==
          "true");
    CHECK(run_scalar("SELECT n2020 FROM (" + sql + ") WHERE firm='b'") == "0");
    CHECK(run_count(v) == 2);

    /* the view_pivot atomicity mechanism: snapshot before collapse, restore
     * after a refused spread — the restored plan is the original, executably */
    View w = make_view();
    View snap = w; /* what cmd_view_pivot saves */
    REQUIRE(w.collapse({{"mean", "mw", "wage"}}, {"firm", "year"}).empty());
    /* a decimal j value forms an illegal Stata name → reshape refuses loudly */
    CHECK_FALSE(w.reshape_wide({"mw"}, {"firm"}, "year", {"2.5"}, false).empty());
    w = snap;
    CHECK(w.compile(false) == make_view().compile(false));
    CHECK(w.cols().size() == 4);
    CHECK(run_count(w) == 6);
}

/* ---- audit 2026-08-22: NAME-CASE-1 alias invariants, merge common columns --- */

/* a view with a case-aliased column: engine nuemp / NUEMP_1 (Stata NUEMP) / x */
static View make_alias_view() {
    View v;
    std::vector<ViewCol> cols;
    ViewCol a; a.name = "nuemp"; a.kind = 'n';
    ViewCol b; b.name = "NUEMP_1"; b.stata = "NUEMP"; b.kind = 'n'; b.varlab = "Upper";
    ViewCol c; c.name = "x"; c.kind = 'n';
    cols = {a, b, c};
    nlohmann::json chars = nlohmann::json::object();
    chars["NUEMP"]["note1"] = "upper note";
    chars["x"]["note1"] = "x note";
    v.open("SELECT * FROM (VALUES (1, 10, 0.5), (2, 20, 1.5)) t(nuemp, NUEMP_1, x)",
           cols, nlohmann::json::object(), chars, "", "alias fixture");
    return v;
}

TEST_CASE("R1/A2-4: renaming an aliased column clears the alias and moves its chars") {
    View v = make_alias_view();
    REQUIRE(v.rename("NUEMP_1", "foo").empty());
    CHECK(v.cols()[1].name == "foo");
    CHECK(v.cols()[1].stata.empty());
    CHECK(v.cols()[1].exposed() == "foo");
    CHECK(v.cols()[1].varlab == "Upper");
    CHECK(v.chars().contains("foo"));
    CHECK(v.chars()["foo"]["note1"] == "upper note");
    CHECK_FALSE(v.chars().contains("NUEMP"));
    CHECK_FALSE(v.chars().contains("NUEMP_1"));
    CHECK(v.chars()["x"]["note1"] == "x note"); /* untouched */
    CHECK(run_scalar("SELECT sum(foo) FROM (" + v.compile(false) + ")") == "30");
    /* sortedby follows too, by exposed name */
    View w = make_alias_view();
    REQUIRE(w.sort({"NUEMP_1"}, {false}).empty());
    CHECK(w.sortedby_names() == std::vector<std::string>{"NUEMP"}); /* A2-9 */
    REQUIRE(w.rename("NUEMP_1", "bar").empty());
    CHECK(w.sortedby_names() == std::vector<std::string>{"bar"});
    /* group form, with a cycle through an aliased column */
    View g = make_alias_view();
    REQUIRE(g.rename_many({"nuemp", "NUEMP_1"}, {"lo", "hi"}).empty());
    CHECK(g.cols()[0].name == "lo");
    CHECK(g.cols()[1].name == "hi");
    CHECK(g.cols()[1].stata.empty());
    CHECK(g.chars()["hi"]["note1"] == "upper note");
    CHECK_FALSE(g.chars().contains("NUEMP"));
    View cyc = make_alias_view();
    REQUIRE(cyc.rename_many({"nuemp", "x"}, {"x", "nuemp"}).empty());
    CHECK(cyc.chars()["nuemp"]["note1"] == "x note");
    CHECK_FALSE(cyc.chars().contains("x"));
    /* a rename onto an alias's exposed name is still the documented refusal */
    View r = make_alias_view();
    CHECK_FALSE(r.rename("x", "NUEMP").empty());
}

TEST_CASE("R3/A2-12: selection varlists accept the exact Stata name or the alias") {
    View v = make_alias_view();
    std::vector<std::string> out;
    REQUIRE(v.expand_patterns({"NUEMP"}, &out).empty());
    CHECK(out == std::vector<std::string>{"NUEMP_1"}); /* engine name returned */
    out.clear();
    REQUIRE(v.expand_patterns({"NUE*"}, &out).empty());
    CHECK(out == std::vector<std::string>{"NUEMP_1"});
    out.clear();
    REQUIRE(v.expand_patterns({"n*"}, &out).empty());
    CHECK(out == std::vector<std::string>{"nuemp"});
    REQUIRE(v.keep_vars({"NUEMP", "x"}).empty());
    CHECK(v.cols().size() == 2);
    CHECK(v.cols()[0].name == "NUEMP_1");
    CHECK(v.cols()[0].stata == "NUEMP");
    CHECK(run_scalar("SELECT sum(NUEMP_1) FROM (" + v.compile(false) + ")") == "30");
    View d = make_alias_view();
    REQUIRE(d.drop_vars({"NUEMP"}).empty());
    CHECK(d.cols().size() == 2);
    View o = make_alias_view();
    REQUIRE(o.reorder({"x", "NUEMP"}).empty());
    CHECK(o.cols()[1].name == "NUEMP_1");
}

TEST_CASE("A2-5/A3-8: collapse default targets keep the source identity; labels and formats follow native") {
    View v = make_alias_view();
    REQUIRE(v.collapse({{"mean", "", "NUEMP_1"}, {"sum", "s", "NUEMP_1"},
                        {"count", "n", "x"}},
                       {"nuemp"})
                .empty());
    REQUIRE(v.cols().size() == 4);
    CHECK(v.cols()[1].name == "NUEMP_1");
    CHECK(v.cols()[1].stata == "NUEMP");       /* default target: alias kept */
    CHECK(v.cols()[1].varlab == "(mean) NUEMP");
    CHECK(v.cols()[2].name == "s");
    CHECK(v.cols()[2].stata.empty());          /* explicit target: new name */
    CHECK(v.cols()[2].varlab == "(sum) NUEMP");
    CHECK(v.cols()[3].varlab == "(count) x");
    CHECK(v.cols()[3].meta_type == "long");
    View f = make_view();
    REQUIRE(f.collapse({{"mean", "", "wage"}}, {"firm"}).empty());
    CHECK(f.cols()[1].varlab == "(mean) wage");
}

TEST_CASE("A2-3: reshape wide / pivot over an aliased stub generate names from the exposed name") {
    View v;
    std::vector<ViewCol> cols;
    ViewCol id; id.name = "id"; id.kind = 'n';
    ViewCol j; j.name = "j"; j.kind = 'n';
    ViewCol x; x.name = "x"; x.kind = 'n';
    ViewCol X; X.name = "X_1"; X.stata = "X"; X.kind = 'n'; X.fmt = "%9.2f";
    cols = {id, j, x, X};
    v.open("SELECT * FROM (VALUES (1, 1, 5.0, 10.0), (1, 2, 5.0, 20.0)) t(id, j, x, X_1)",
           cols, nlohmann::json::object(), nlohmann::json::object(), "", "wide fixture");
    /* stubs x and X_1 (Stata X) would generate x1/X1 — names DuckDB cannot
     * hold apart in one relation: refused loudly, view untouched */
    View clash = v;
    CHECK_FALSE(clash.reshape_wide({"x", "X_1"}, {"id"}, "j", {"1", "2"}, false).empty());
    CHECK(clash.compile(false) == v.compile(false));
    REQUIRE(v.reshape_wide({"X_1"}, {"id", "x"}, "j", {"1", "2"}, false).empty());
    std::vector<std::string> names;
    for (const auto &c : v.cols()) names.push_back(c.exposed());
    CHECK(names == std::vector<std::string>{"id", "x", "X1", "X2"});
    for (const auto &c : v.cols()) CHECK(c.stata.empty());
    CHECK(v.cols()[2].name == "X1");
    CHECK(v.cols()[2].varlab == "1 X");  /* native: "<j> <stub name>" */
    CHECK(v.cols()[2].fmt == "%9.2f");   /* format kept */
    CHECK(run_scalar("SELECT X2 FROM (" + v.compile(false) + ")") == "20.0");
    /* a generated name clashing only by case with an i() variable is refused */
    View w;
    ViewCol i2; i2.name = "x1"; i2.kind = 'n';
    w.open("SELECT * FROM (VALUES (1, 1, 1.0, 10.0)) t(x1, j, x, X_1)", {i2, j, x, X},
           nlohmann::json::object(), nlohmann::json::object(), "", "clash");
    CHECK_FALSE(w.reshape_wide({"X_1"}, {"x1"}, "j", {"1"}, false).empty());
    /* the pivot decomposition over an aliased source: collapse default target
     * keeps the alias, the spread derives X1/X2 (no leaked alias, no duplicate) */
    View p = make_alias_view();
    REQUIRE(p.gen("j", "", "nuemp", "", false).empty());
    REQUIRE(p.collapse({{"mean", "", "NUEMP_1"}}, {"x", "j"}).empty());
    REQUIRE(p.reshape_wide({"NUEMP_1"}, {"x"}, "j", {"1", "2"}, false).empty());
    std::vector<std::string> pn;
    for (const auto &c : p.cols()) pn.push_back(c.exposed());
    CHECK(pn == std::vector<std::string>{"x", "NUEMP1", "NUEMP2"});
}

TEST_CASE("MERGE-COMMON-1/A3-1: common non-key columns take the using value on using-only rows") {
    /* master (k c sc a), using (k c sc b): k=4 exists only in using */
    View m;
    std::vector<ViewCol> mc;
    for (const char *n : {"k", "c", "sc", "a"}) {
        ViewCol col; col.name = n; col.kind = (std::string(n) == "sc") ? 's' : 'n';
        mc.push_back(col);
    }
    m.open("SELECT * FROM (VALUES (1, 11, 'm1', 1.0), (2, NULL, '', 2.0), (3, 33, 'm3', 3.0)) "
           "t(k, c, sc, a)", mc, nlohmann::json::object(), nlohmann::json::object(), "", "m");
    View::UsingSide u;
    for (const char *n : {"k", "c", "sc", "b"}) {
        ViewCol col; col.name = n; col.kind = (std::string(n) == "sc") ? 's' : 'n';
        col.normalized = true;
        u.cols.push_back(col);
    }
    u.select_sql = "SELECT * FROM (VALUES (2, 222, 'u2', 20.0), (3, 333, 'u3', 30.0), "
                   "(4, 444, 'u4', 40.0)) t(k, c, sc, b)";
    u.vallabs = nlohmann::json::object();
    std::vector<std::string> warns;
    View v = m;
    REQUIRE(v.merge_with("1:1", {"k"}, u, {}, 0, "", false, &warns).empty());
    const std::string sql = v.compile(false);
    /* matched rows keep the MASTER value, even a missing one (k=2) */
    CHECK(run_scalar("SELECT c IS NULL FROM (" + sql + ") WHERE k = 2") == "true");
    CHECK(run_scalar("SELECT sc FROM (" + sql + ") WHERE k = 2") == "");
    CHECK(run_scalar("SELECT c FROM (" + sql + ") WHERE k = 3") == "33");
    CHECK(run_scalar("SELECT sc FROM (" + sql + ") WHERE k = 3") == "m3");
    /* master-only row: master values */
    CHECK(run_scalar("SELECT c FROM (" + sql + ") WHERE k = 1") == "11");
    /* using-only row (_merge == 2): USING values, like native Stata */
    CHECK(run_scalar("SELECT c FROM (" + sql + ") WHERE k = 4") == "444");
    CHECK(run_scalar("SELECT sc FROM (" + sql + ") WHERE k = 4") == "u4");
    CHECK(run_scalar("SELECT b FROM (" + sql + ") WHERE k = 4") == "40.0");
    CHECK(run_scalar("SELECT a IS NULL FROM (" + sql + ") WHERE k = 4") == "true");
    CHECK(run_scalar("SELECT _merge FROM (" + sql + ") WHERE k = 4") == "2");
    CHECK(v.cols().back().fmt == "%23.0g"); /* A3-8: native _merge format */
    /* keep(using) / keepusing(c) variants */
    View v2 = m;
    REQUIRE(v2.merge_with("m:1", {"k"}, u, {"c"}, 2, "", false, &warns).empty());
    const std::string sql2 = v2.compile(false);
    CHECK(run_count(v2) == 1);
    CHECK(run_scalar("SELECT c FROM (" + sql2 + ")") == "444");
    CHECK(run_scalar("SELECT sc FROM (" + sql2 + ")") == ""); /* sc not kept from using */
    /* keepusing(b): the common c is not brought -> master (missing) on k=4 */
    View v3 = m;
    REQUIRE(v3.merge_with("1:1", {"k"}, u, {"b"}, 0, "", false, &warns).empty());
    CHECK(run_scalar("SELECT c IS NULL FROM (" + v3.compile(false) + ") WHERE k = 4") == "true");
    /* m:m (sequential) branch fills too */
    View v4 = m;
    REQUIRE(v4.merge_with("m:m", {"k"}, u, {}, 0, "", false, &warns).empty());
    CHECK(run_scalar("SELECT c FROM (" + v4.compile(false) + ") WHERE k = 4") == "444");
    /* a common column string on one side and numeric on the other is refused (native r(106)) */
    View::UsingSide bad = u;
    bad.cols[1].kind = 's';
    View v5 = m;
    CHECK_FALSE(v5.merge_with("1:1", {"k"}, bad, {}, 0, "", false, &warns).empty());
}

TEST_CASE("A2-6: append gen() clashing only by case with a using column is refused before mutation") {
    View v = make_view();
    View::UsingSide u;
    ViewCol id; id.name = "id"; id.kind = 'n';
    ViewCol val; val.name = "val"; val.kind = 'n';
    u.cols = {id, val};
    u.select_sql = "SELECT * FROM (VALUES (9, 1.0)) t(id, val)";
    u.vallabs = nlohmann::json::object();
    std::vector<std::string> warns;
    const std::string before = v.compile(false);
    CHECK_FALSE(v.append_with({u}, "Val", &warns).empty());
    CHECK(v.compile(false) == before);
    CHECK(v.append_with({u}, "src", &warns).empty());
}

TEST_CASE("CONTRACT-FREQ-1/A2-15.4: contract refuses an existing frequency variable like native r(110)") {
    View v;
    std::vector<ViewCol> cols;
    ViewCol g; g.name = "g"; g.kind = 'n';
    ViewCol f; f.name = "_freq"; f.kind = 'n';
    v.open("SELECT * FROM (VALUES (1, 5), (1, 6)) t(g, _freq)", {g, f},
           nlohmann::json::object(), nlohmann::json::object(), "", "contract fixture");
    CHECK_FALSE(v.contract({"g"}, "").empty());
    CHECK(v.contract({"g"}, "n").empty());
    CHECK(v.cols().size() == 2);
    CHECK(v.cols()[1].name == "n");
    /* contracting ON the frequency variable itself is fine */
    View w;
    w.open("SELECT * FROM (VALUES (1, 5), (1, 6)) t(g, _freq)", {g, f},
           nlohmann::json::object(), nlohmann::json::object(), "", "contract fixture");
    CHECK_FALSE(w.contract({"_freq"}, "").empty()); /* _freq by-var AND frequency name */
    CHECK(w.contract({"_freq"}, "cnt").empty());
}
