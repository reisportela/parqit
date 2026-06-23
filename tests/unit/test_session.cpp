/* Engine capability gate: these tests pin down everything parqit assumes about
 * the vendored DuckDB build. If a DuckDB upgrade drops or changes any of it
 * (as 1.5 did when it moved core functions out of the amalgamation), this
 * file fails before any Stata user can be affected. */
#include "doctest.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "abi.h" /* Arrow C Data Interface */
#include "duckdb.h"
#include "engine/session.hpp"

using parqit::Session;

static std::string tmp_parquet_path() {
#ifdef _WIN32
    const char *base = getenv("TEMP");
    std::string dir = base ? base : ".";
    return dir + "\\parqit_test_session.parquet";
#else
    return "/tmp/parqit_test_session.parquet";
#endif
}

TEST_CASE("session opens and core_functions is statically present") {
    Session &s = Session::instance();
    std::string err, v;
    REQUIRE_MESSAGE(s.ensure_open(), s.last_error());
    /* version() lives in the core_functions extension in DuckDB >= 1.5;
     * duckdb_library_version() already carries the leading "v" */
    REQUIRE_MESSAGE(s.query_scalar("SELECT version()", &v, &err), err);
    CHECK(v == duckdb_library_version());
    /* a second core_functions citizen used heavily by the verb layer */
    REQUIRE_MESSAGE(s.query_scalar("SELECT round(ln(exp(1.0)), 6)::VARCHAR", &v, &err), err);
    CHECK(v == "1.0");
}

TEST_CASE("parquet write/read and parqit KV metadata") {
    Session &s = Session::instance();
    std::string err, v;
    const std::string file = tmp_parquet_path();

    REQUIRE_MESSAGE(
        s.exec("COPY (SELECT range AS i, 'r' || range::VARCHAR AS sv, "
               "      CASE WHEN range = 2 THEN NULL ELSE range / 2.0 END AS dv "
               "      FROM range(5)) TO " +
                   parqit::quote_literal(file) +
                   " (FORMAT PARQUET, KV_METADATA {'parqit.schema': '{\"n\":3}'})",
               &err),
        err);

    REQUIRE_MESSAGE(s.query_scalar("SELECT count(*)::VARCHAR || '/' || sum(i)::VARCHAR "
                                   "FROM read_parquet(" +
                                       parqit::quote_literal(file) + ")",
                                   &v, &err),
                    err);
    CHECK(v == "5/10");

    REQUIRE_MESSAGE(s.query_scalar("SELECT decode(value) FROM parquet_kv_metadata(" +
                                       parqit::quote_literal(file) +
                                       ") WHERE decode(key) = 'parqit.schema'",
                                   &v, &err),
                    err);
    CHECK(v == "{\"n\":3}");

    /* row-group / schema introspection used by `parqit describe` */
    REQUIRE_MESSAGE(s.query_scalar("SELECT count(distinct row_group_id)::VARCHAR FROM "
                                   "parquet_metadata(" +
                                       parqit::quote_literal(file) + ")",
                                   &v, &err),
                    err);
    CHECK(v == "1");
    std::remove(file.c_str());
}

TEST_CASE("arrow C data interface conversion of a result chunk") {
    Session &s = Session::instance();
    std::string err;
    duckdb_result res;
    REQUIRE_MESSAGE(
        s.query("SELECT i::INTEGER AS i, "
                "       CASE WHEN i = 1 THEN NULL ELSE 'v' || i::VARCHAR END AS sv, "
                "       (DATE '1960-01-01' + INTERVAL (i) DAY)::DATE AS d "
                "FROM range(3) t(i) ORDER BY i",
                &res, &err),
        err);

    duckdb_arrow_options aopts;
    duckdb_connection_get_arrow_options(s.con(), &aopts);

    idx_t ncol = duckdb_column_count(&res);
    REQUIRE(ncol == 3);
    duckdb_logical_type types[3];
    const char *names[3];
    for (idx_t c = 0; c < ncol; c++) {
        types[c] = duckdb_column_logical_type(&res, c);
        names[c] = duckdb_column_name(&res, c);
    }

    ArrowSchema schema;
    std::memset(&schema, 0, sizeof(schema));
    duckdb_error_data ed = duckdb_to_arrow_schema(aopts, types, names, ncol, &schema);
    REQUIRE_MESSAGE(ed == nullptr, duckdb_error_data_message(ed));
    REQUIRE(schema.n_children == 3);
    CHECK(std::string(schema.children[0]->format) == "i"); /* int32 */
    CHECK(std::string(schema.children[1]->format) == "u"); /* utf8 */
    CHECK(std::string(schema.children[2]->format) == "tdD"); /* date32 */

    duckdb_data_chunk chunk = duckdb_fetch_chunk(res);
    REQUIRE(chunk != nullptr);
    ArrowArray arr;
    std::memset(&arr, 0, sizeof(arr));
    ed = duckdb_data_chunk_to_arrow(aopts, chunk, &arr);
    REQUIRE_MESSAGE(ed == nullptr, duckdb_error_data_message(ed));
    REQUIRE(arr.length == 3);
    REQUIRE(arr.n_children == 3);

    /* int32 payload */
    const ArrowArray *c0 = arr.children[0];
    const int32_t *ivals = static_cast<const int32_t *>(c0->buffers[1]);
    CHECK(ivals[c0->offset + 0] == 0);
    CHECK(ivals[c0->offset + 2] == 2);

    /* utf8 column: validity bitmap marks row 1 NULL; offsets+data check */
    const ArrowArray *c1 = arr.children[1];
    REQUIRE(c1->null_count == 1);
    const uint8_t *validity = static_cast<const uint8_t *>(c1->buffers[0]);
    REQUIRE(validity != nullptr);
    auto is_valid = [&](int64_t row) {
        int64_t pos = c1->offset + row;
        return (validity[pos / 8] >> (pos % 8)) & 1;
    };
    CHECK(is_valid(0));
    CHECK_FALSE(is_valid(1));
    CHECK(is_valid(2));
    const int32_t *offs = static_cast<const int32_t *>(c1->buffers[1]);
    const char *chars = static_cast<const char *>(c1->buffers[2]);
    std::string row0(chars + offs[c1->offset], offs[c1->offset + 1] - offs[c1->offset]);
    CHECK(row0 == "v0");

    /* date32 payload: row 0 is 1960-01-01 = -3653 days from the unix epoch
     * (the Stata epoch offset parqit's transfer layer adds back) */
    const ArrowArray *c2 = arr.children[2];
    const int32_t *dvals = static_cast<const int32_t *>(c2->buffers[1]);
    CHECK(dvals[c2->offset + 0] == -3653);

    if (arr.release) arr.release(&arr);
    if (schema.release) schema.release(&schema);
    duckdb_destroy_data_chunk(&chunk);
    for (idx_t c = 0; c < ncol; c++) duckdb_destroy_logical_type(&types[c]);
    duckdb_destroy_arrow_options(&aopts);
    duckdb_destroy_result(&res);
}

TEST_CASE("temp_directory configuration applies") {
    Session &s = Session::instance();
    std::string err, v;
    REQUIRE(s.ensure_open());
    REQUIRE_MESSAGE(s.set_threads(4, &err), err);
    REQUIRE_MESSAGE(s.query_scalar("SELECT current_setting('threads')::VARCHAR", &v, &err), err);
    CHECK(v == "4");
}
