/* NAME-CASE-1: Stata variable names are case-sensitive (`nuemp` and `NUEMP`
 * are two variables), DuckDB identifiers are not. These pin the two engine
 * pieces that keep names exact at the Stata/Parquet boundaries: the
 * case-insensitive engine-alias generator (what a clashing column is called
 * inside the lazy view) and the Parquet footer rename that restores the exact
 * names after DuckDB's COPY dedups them in the written file. */
#include "doctest.h"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "engine/parquet_footer.hpp"
#include "engine/sanitize.hpp"
#include "engine/session.hpp"
#include "test_tmp.hpp"

using parqit::ascii_lower;
using parqit::engine_unique_ci;
using parqit::parquet_leaf_names;
using parqit::parquet_rename_leaf_columns;
using parqit::quote_literal;
using parqit::Session;

TEST_CASE("NAME-CASE-1: engine aliases are case-insensitively unique, deterministic, minimal") {
    using V = std::vector<std::string>;
    CHECK(engine_unique_ci({"a", "b", "c"}) == V{"a", "b", "c"});
    CHECK(engine_unique_ci({"nuemp", "NUEMP"}) == V{"nuemp", "NUEMP_1"});
    CHECK(engine_unique_ci({"NUEMP", "nuemp"}) == V{"NUEMP", "nuemp_1"});
    /* a literal `<name>_1` already present pushes the alias to _2 */
    CHECK(engine_unique_ci({"NUEMP", "nuemp", "NUEMP_1"}) == V{"NUEMP", "nuemp_2", "NUEMP_1"});
    CHECK(engine_unique_ci({"a", "A", "a_1", "A_1"}) == V{"a", "A_2", "a_1", "A_1_1"});
    /* names the caller already holds (a master view) are dodged too */
    CHECK(engine_unique_ci({"X", "y"}, {"x"}) == V{"X_1", "y"});
    CHECK(engine_unique_ci({"caem1L_EMP_QP", "caem1l_EMP_QP"}) ==
          V{"caem1L_EMP_QP", "caem1l_EMP_QP_1"});
    /* property: pairwise case-insensitively distinct; non-clashing names untouched */
    const V in = {"nuemp", "NUEMP", "x", "X", "x_1", "ok", "Ok", "OK", "plain"};
    const V out = engine_unique_ci(in);
    REQUIRE(out.size() == in.size());
    std::set<std::string> low;
    for (size_t i = 0; i < out.size(); i++) {
        CHECK(low.insert(ascii_lower(out[i])).second);
        CHECK(ascii_lower(out[i]).rfind(ascii_lower(in[i]), 0) == 0); /* alias keeps the name as prefix */
    }
    CHECK(out[0] == "nuemp");
    CHECK(out[2] == "x");
    CHECK(out[4] == "x_1");
    CHECK(out[5] == "ok");
    CHECK(out[8] == "plain");
    CHECK(ascii_lower("NUEMP_Ção") == "nuemp_Ção"); /* ASCII fold only, like DuckDB */
}

TEST_CASE("NAME-CASE-1: footer rename restores case-distinct leaf names after DuckDB's COPY dedup") {
    Session &s = Session::instance();
    std::string err;
    REQUIRE_MESSAGE(s.ensure_open(), s.last_error());
    const std::string f = parqit_test::tmp_path("name_case_footer.parquet");
    std::remove(f.c_str());
    REQUIRE_MESSAGE(s.exec("COPY (SELECT 1 AS nuemp, 2 AS NUEMP, 'x' AS s, 3.5 AS d) TO " +
                               quote_literal(f) + " (FORMAT PARQUET)",
                           &err),
                    err);
    std::vector<std::string> names;
    REQUIRE(parquet_leaf_names(f, &names) == "");
    /* DuckDB's binder dedups the clashing name in the written file */
    CHECK(names == std::vector<std::string>{"nuemp", "NUEMP_1", "s", "d"});

    /* wrong count and nested schemas are refused without touching the file */
    CHECK(parquet_rename_leaf_columns(f, {"a", "b", "c"}) != "");
    names.clear();
    REQUIRE(parquet_leaf_names(f, &names) == "");
    CHECK(names == std::vector<std::string>{"nuemp", "NUEMP_1", "s", "d"});

    /* the rename itself */
    REQUIRE(parquet_rename_leaf_columns(f, {"nuemp", "NUEMP", "s", "d"}) == "");
    names.clear();
    REQUIRE(parquet_leaf_names(f, &names) == "");
    CHECK(names == std::vector<std::string>{"nuemp", "NUEMP", "s", "d"});
    /* a no-op rename is a no-op */
    REQUIRE(parquet_rename_leaf_columns(f, {"nuemp", "NUEMP", "s", "d"}) == "");

    /* DuckDB still reads the file — exact names in the footer, data intact */
    std::string v;
    REQUIRE_MESSAGE(s.query_scalar("SELECT string_agg(name, ',') FROM (SELECT name FROM "
                                   "parquet_schema(" + quote_literal(f) +
                                   ") WHERE num_children IS NULL OR num_children = 0)",
                                   &v, &err),
                    err);
    CHECK(v == "nuemp,NUEMP,s,d");
    REQUIRE_MESSAGE(s.query_scalar("SELECT nuemp::VARCHAR || ',' || NUEMP_1::VARCHAR || ',' "
                                   "|| s || ',' || d::VARCHAR FROM read_parquet(" +
                                   quote_literal(f) + ")",
                                   &v, &err),
                    err);
    CHECK(v == "1,2,x,3.5");
    std::remove(f.c_str());

    /* multi row group file: every ColumnChunk path is renamed */
    const std::string g = parqit_test::tmp_path("name_case_footer_rg.parquet");
    std::remove(g.c_str());
    REQUIRE_MESSAGE(s.exec("COPY (SELECT range AS nuemp, range * 2 AS NUEMP, "
                           "'s' || range::VARCHAR AS NuEmp FROM range(100000)) TO " +
                               quote_literal(g) + " (FORMAT PARQUET, ROW_GROUP_SIZE 2048)",
                           &err),
                    err);
    names.clear();
    REQUIRE(parquet_leaf_names(g, &names) == "");
    CHECK(names == std::vector<std::string>{"nuemp", "NUEMP_1", "NuEmp_2"});
    REQUIRE(parquet_rename_leaf_columns(g, {"nuemp", "NUEMP", "NuEmp"}) == "");
    names.clear();
    REQUIRE(parquet_leaf_names(g, &names) == "");
    CHECK(names == std::vector<std::string>{"nuemp", "NUEMP", "NuEmp"});
    REQUIRE_MESSAGE(s.query_scalar("SELECT count(*)::VARCHAR || ',' || sum(NUEMP_1)::VARCHAR "
                                   "|| ',' || max(NuEmp_2) FROM read_parquet(" +
                                   quote_literal(g) + ")",
                                   &v, &err),
                    err);
    CHECK(v == "100000,9999900000,s99999");
    std::remove(g.c_str());

    /* nested (group) columns are refused */
    const std::string h = parqit_test::tmp_path("name_case_footer_nested.parquet");
    std::remove(h.c_str());
    REQUIRE_MESSAGE(s.exec("COPY (SELECT {'a': 1} AS st, 2 AS x) TO " + quote_literal(h) +
                               " (FORMAT PARQUET)",
                           &err),
                    err);
    CHECK(parquet_rename_leaf_columns(h, {"a", "x"}) != "");
    CHECK(parquet_rename_leaf_columns(h, {"st", "x"}) != "");
    std::remove(h.c_str());

    /* not a parquet file */
    CHECK(parquet_rename_leaf_columns(parqit_test::tmp_path("does_not_exist.parquet"), {"a"}) != "");
}

/* Round 2 (audit 2026-08-22, A2-2 relaxed / A2-15(1) / V2.6): parqit predicts
 * the scan names DuckDB produces with exact replicas of the engine's own rules
 * (verified line by line against the fetched v1.5.3 source) and maps them back
 * to the true Parquet leaf names. These pin the replicas. */
TEST_CASE("DUCKDB-DEDUP-1: the Parquet reader's case-insensitive in-file dedup, replicated") {
    using V = std::vector<std::string>;
    using parqit::duckdb_reader_dedup;
    CHECK(duckdb_reader_dedup({"a", "b"}) == V{"a", "b"});
    CHECK(duckdb_reader_dedup({"nuemp", "NUEMP", "s"}) == V{"nuemp", "NUEMP_1", "s"});
    /* the candidate that is taken is re-suffixed: A -> A_1 (a_1 taken) -> A_1_1
     * (a_1_1 taken) -> A_1_1_1 — the nested shape of audit finding A2-1 */
    CHECK(duckdb_reader_dedup({"a", "a_1", "a_1_1", "A"}) == V{"a", "a_1", "a_1_1", "A_1_1_1"});
    /* the counter is per case-folded base name: the third `dup` gets _2 */
    CHECK(duckdb_reader_dedup({"dup", "dup", "DUP"}) == V{"dup", "dup_1", "DUP_2"});
    CHECK(duckdb_reader_dedup({"dup", "dup", "DUP", "a b", "a_b", "A B", "A_B", "1x", "_1x"}) ==
          V{"dup", "dup_1", "DUP_2", "a b", "a_b", "A B_1", "A_B_1", "1x", "_1x"});
    /* empty names: the reader leaves them empty (the binder names them
     * C<index>); a second empty name is deduped like any other */
    CHECK(duckdb_reader_dedup({"a", "", "c"}) == V{"a", "", "c"});
    CHECK(duckdb_reader_dedup({"", ""}) == V{"", "_1"});
    CHECK(parqit::duckdb_empty_column_name(1) == "C1");
    CHECK(parqit::duckdb_empty_column_name(0) == "C0");
}

TEST_CASE("DUCKDB-UNION-1: union_by_name column order and case-insensitive matching, replicated") {
    using V = std::vector<std::string>;
    using parqit::duckdb_union_by_name;
    /* first file's columns, then each later file's NEW names in file order */
    auto u = duckdb_union_by_name({{"nuemp", "NUEMP_1", "s"}, {"nuemp", "NUEMP_1", "s", "extra"}});
    CHECK(u.names == V{"nuemp", "NUEMP_1", "s", "extra"});
    REQUIRE(u.owner.size() == 4);
    CHECK(u.owner[1] == std::make_pair<size_t, size_t>(0, 1));
    CHECK(u.owner[3] == std::make_pair<size_t, size_t>(1, 3));
    CHECK(u.member_of[1][1] == 1);
    CHECK(u.member_of[1][3] == 3);
    auto u2 = duckdb_union_by_name({{"nuemp", "s"}, {"nuemp", "NUEMP_1", "s"}});
    CHECK(u2.names == V{"nuemp", "s", "NUEMP_1"});
    CHECK(u2.owner[2] == std::make_pair<size_t, size_t>(1, 1));
    /* the match is case-insensitive: a later file's NUEMP flows into nuemp */
    auto u3 = duckdb_union_by_name({{"nuemp", "s"}, {"NUEMP", "s"}});
    CHECK(u3.names == V{"nuemp", "s"});
    CHECK(u3.member_of[1][0] == 0);
    /* the cross-wiring shape parqit refuses: a: nuemp, NUEMP (reader: NUEMP_1);
     * b: NUEMP -> b's NUEMP lands in union column 0 (nuemp), while a's NUEMP is
     * union column 1 */
    auto u4 = duckdb_union_by_name({{"nuemp", "NUEMP_1"}, {"NUEMP"}});
    CHECK(u4.names == V{"nuemp", "NUEMP_1"});
    CHECK(u4.member_of[1][0] == 0);
    CHECK(duckdb_union_by_name({}).names.empty());
}

TEST_CASE("DUCKDB-HIVE-1: HivePartitioning::Parse key extraction, replicated") {
    using V = std::vector<std::string>;
    using parqit::duckdb_hive_keys;
    CHECK(duckdb_hive_keys("nm/hive/g=1/part-0.parquet") == V{"g"});
    CHECK(duckdb_hive_keys("/data/year=2020/month=3/x.parquet") == V{"year", "month"});
    CHECK(duckdb_hive_keys("/data/city=a=b/x.parquet").empty()); /* two '=' : not a partition */
    CHECK(duckdb_hive_keys("/data/=v/x.parquet").empty());       /* empty key */
    CHECK(duckdb_hive_keys("/data/k=v/k=w/x.parquet") == V{"k"}); /* std::map::insert keeps the first */
    CHECK(duckdb_hive_keys("g=1.parquet").empty());               /* the basename is never a key */
    CHECK(duckdb_hive_keys("a\\b=1\\c.parquet") == V{"b"});       /* backslash separators too */
    CHECK(duckdb_hive_keys("/d/k=v?x/y.parquet").empty());         /* '?' disqualifies */
    CHECK(duckdb_hive_keys("plain/dir/file.parquet").empty());
}
