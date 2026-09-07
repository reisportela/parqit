/* Engine capability gate: these tests pin down everything parqit assumes about
 * the vendored DuckDB build. If a DuckDB upgrade drops or changes any of it
 * (as 1.5 did when it moved core functions out of the amalgamation), this
 * file fails before any Stata user can be affected. */
#include "doctest.h"

#include <cstdint>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "abi.h" /* Arrow C Data Interface */
#include "duckdb.h"
#include "engine/session.hpp"
#include "test_tmp.hpp"

using parqit::Session;

TEST_CASE("C API aggregate finalizer errors reach the query caller") {
    Session &s=Session::instance();
    REQUIRE(s.ensure_open());
    auto function=duckdb_create_aggregate_function();
    duckdb_aggregate_function_set_name(function,"__parqit_test_finalize_failure");
    auto type=duckdb_create_logical_type(DUCKDB_TYPE_DOUBLE);
    duckdb_aggregate_function_add_parameter(function,type);
    duckdb_aggregate_function_set_return_type(function,type);
    duckdb_destroy_logical_type(&type);
    duckdb_aggregate_function_set_functions(function,
        [](duckdb_function_info)->idx_t { return sizeof(uint64_t); },
        [](duckdb_function_info,duckdb_aggregate_state state) { *reinterpret_cast<uint64_t *>(state)=0; },
        [](duckdb_function_info,duckdb_data_chunk,duckdb_aggregate_state *) {},
        [](duckdb_function_info,duckdb_aggregate_state *,duckdb_aggregate_state *,idx_t) {},
        [](duckdb_function_info info,duckdb_aggregate_state *,duckdb_vector,idx_t,idx_t) {
            duckdb_aggregate_function_set_error(info,"exact invariant test");
        });
    REQUIRE(duckdb_register_aggregate_function(s.con(),function)==DuckDBSuccess);
    duckdb_destroy_aggregate_function(&function);
    duckdb_result result;
    std::string error;
    CHECK_FALSE(s.query("SELECT __parqit_test_finalize_failure(1::DOUBLE)",&result,&error));
    CHECK(error.find("exact invariant test")!=std::string::npos);
}

TEST_CASE("exact covariance geometry survives grouped offsets and sliding windows") {
    Session &s=Session::instance();
    std::string error;
    duckdb_result result;
    const std::string data="(SELECT i, i//3 AS g, (i%3)::DOUBLE AS x, "
        "CASE WHEN i%3=2 THEN '2.000000000001'::DOUBLE ELSE (i%3)::DOUBLE END AS y "
        "FROM range(15009) t(i))";
    REQUIRE_MESSAGE(s.query("SELECT (__parqit_corr(x,y)).rho, (__parqit_corr(x,y)).sine, "
        "(__parqit_stats(x)).variance FROM "+data+" GROUP BY g",&result,&error),error);
    REQUIRE(duckdb_row_count(&result)==5003);
    for (idx_t i=0;i<5003;++i) {
        CHECK(duckdb_value_double(&result,0,i)==1);
        CHECK(duckdb_value_double(&result,1,i)==0x1.450c56efe0508p-42);
        CHECK(duckdb_value_double(&result,2,i)==1);
    }
    duckdb_destroy_result(&result);
    REQUIRE_MESSAGE(s.query("SELECT i, (__parqit_corr(x,y) OVER (ORDER BY i "
        "ROWS BETWEEN 2 PRECEDING AND CURRENT ROW)).sine FROM "+data+
        " ORDER BY i LIMIT 10",&result,&error),error);
    for (idx_t i=2;i<10;++i) CHECK(duckdb_value_double(&result,1,i)==0x1.450c56efe0508p-42);
    duckdb_destroy_result(&result);
}

TEST_CASE("histogram ranks compare exact edges before rounding to doubles") {
    Session &s = Session::instance();
    std::string err;
    duckdb_result result;
    REQUIRE_MESSAGE(s.query(
        "SELECT __parqit_histbin(x, 0::DOUBLE, 1::DOUBLE, 3::UBIGINT) "
        "FROM (VALUES ('0.33333333333333326'::DOUBLE), ('0.3333333333333333'::DOUBLE), "
        "('0.33333333333333337'::DOUBLE), (1::DOUBLE), (NULL::DOUBLE)) t(x)", &result, &err), err);
    const int expected[] = {0, 0, 1, 2};
    for (idx_t row = 0; row < 4; ++row) CHECK(duckdb_value_int64(&result, 0, row) == expected[row]);
    CHECK(duckdb_value_is_null(&result, 0, 4));
    duckdb_destroy_result(&result);
    REQUIRE_MESSAGE(s.query(
        "SELECT __parqit_histbin(x, 9007199254740992::BIGINT, 9007199254740994::BIGINT, 2::UBIGINT) "
        "FROM (VALUES (9007199254740992::BIGINT), (9007199254740993::BIGINT), "
        "(9007199254740994::BIGINT)) t(x)", &result, &err), err);
    CHECK(duckdb_value_int64(&result, 0, 0) == 0);
    CHECK(duckdb_value_int64(&result, 0, 1) == 1);
    CHECK(duckdb_value_int64(&result, 0, 2) == 1);
    duckdb_destroy_result(&result);
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

TEST_CASE("overflow retry preserves finite groups and uses bounded moment states") {
    Session &s = Session::instance();
    std::string err;
    REQUIRE_MESSAGE(s.prepare_stats_fallback(&err), err);
    REQUIRE_MESSAGE(s.prepare_stats_fallback(&err), err);
    duckdb_result res;
    REQUIRE_MESSAGE(s.query(
        "SELECT g, count(x), avg(x), __parqit_sd_fallback(x), "
        "__parqit_var_fallback(x), __parqit_corr_fallback(x,y) FROM (VALUES "
        "(1,1::DOUBLE,3::DOUBLE),(1,2,2),(1,3,1),"
        "(2,1e200,3e200),(2,2e200,2e200),(2,3e200,1e200),"
        "(3,NULL,NULL),(4,7,9),(5,7,9),(5,7,9)) t(g,x,y) GROUP BY g ORDER BY g",
        &res, &err), err);
    REQUIRE(duckdb_row_count(&res) == 5);
    CHECK(duckdb_value_double(&res, 3, 0) == doctest::Approx(1));
    CHECK(duckdb_value_double(&res, 4, 0) == doctest::Approx(1));
    CHECK(duckdb_value_double(&res, 5, 0) == doctest::Approx(-1));
    CHECK(duckdb_value_double(&res, 2, 1) == doctest::Approx(2e200));
    for (idx_t row = 1; row <= 3; ++row)
        for (idx_t col = 3; col <= 5; ++col)
            CHECK(duckdb_value_is_null(&res, col, row));
    CHECK(duckdb_value_double(&res, 3, 4) == 0);
    CHECK(duckdb_value_double(&res, 4, 4) == 0);
    CHECK(duckdb_value_is_null(&res, 5, 4));
    duckdb_destroy_result(&res);

    struct OverflowCase { const char *expression, *message; };
    const OverflowCase errors[] = {
        {"stddev_samp(i*1e200)", "STDDEV_SAMP is out of range!"},
        {"var_samp(i*1e200)", "VARSAMP is out of range!"},
        {"corr(i,i*1e200)", "STDDEV_POP for X is out of range!"},
        {"corr(i*1e200,i)", "STDDEV_POP for Y is out of range!"}
    };
    for (const auto &test : errors) {
        CHECK_FALSE(s.query("SELECT " + std::string(test.expression) +
                                " FROM range(1,5) t(i)", &res, &err));
        CHECK(err == "Out of Range Error: " + std::string(test.message));
    }

    /* More than one result vector exercises the C API finalize offset.
     * The built-in aggregates are an independent oracle on ordinary groups. */
    REQUIRE_MESSAGE(s.query(
        "SELECT max(abs(sd-sdf)), max(abs(v-vf)), max(abs(c-cf)) FROM ("
        "SELECT g, stddev_samp(x) sd, __parqit_sd_fallback(x) sdf, "
        "var_samp(x) v, __parqit_var_fallback(x) vf, "
        "corr(x,y) c, __parqit_corr_fallback(x,y) cf FROM ("
        "SELECT i%2053 g, sin(i*0.123)+1e6 x, cos(i*0.37) y "
        "FROM range(400000) t(i)) GROUP BY g)", &res, &err), err);
    for (idx_t col = 0; col < 3; ++col) {
        CHECK_FALSE(duckdb_value_is_null(&res, col, 0));
        CHECK(duckdb_value_double(&res, col, 0) < 1e-9);
    }
    duckdb_destroy_result(&res);
    s.close();
    REQUIRE_MESSAGE(s.prepare_stats_fallback(&err), err);
}

TEST_CASE("parquet write/read and parqit KV metadata") {
    Session &s = Session::instance();
    std::string err, v;
    const std::string file = parqit_test::tmp_path("parqit_test_session.parquet");
    std::remove(file.c_str());

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

TEST_CASE("STRING-DENORMAL-1: parqit_stata_string renders subnormals like Stata's %9.0g (A3-7)") {
    Session &s = Session::instance();
    std::string err, v;
    REQUIRE(s.ensure_open());
    auto str_of = [&](const char *lit) {
        std::string out;
        REQUIRE_MESSAGE(s.query_scalar(std::string("SELECT parqit_stata_string(") + lit +
                                           "::DOUBLE)",
                                       &out, &err),
                        err);
        return out;
    };
    /* verified natively (StataNow 19.5): string(5e-324) = "4.9e-324" */
    CHECK(str_of("5e-324") == "4.9e-324");
    CHECK(str_of("1e-323") == "9.9e-324");
    CHECK(str_of("1e-310") == "1.0e-310");
    CHECK(str_of("1e-308") == "1.0e-308");
    CHECK(str_of("2.2250738585072014e-308") == "2.2e-308");
    CHECK(str_of("1e300") == "1.0e+300");
    CHECK(str_of("0") == "0");
    CHECK(str_of("-0.0") == "0");
    CHECK(str_of("1.5") == "1.5");
    CHECK(str_of("-2.5e-7") == "-2.50e-07");
}

TEST_CASE("stable generic aggregates preserve numeric input types and scaled moments") {
    Session &s = Session::instance();
    std::string err;
    duckdb_result r;
    REQUIRE_MESSAGE(s.query(
        "SELECT (__parqit_total(x)).sum, (__parqit_total(x)).mean FROM "
        "(VALUES (9007199254740993::BIGINT),(-9007199254740992::BIGINT)) t(x)", &r, &err), err);
    CHECK(duckdb_value_double(&r, 0, 0) == 1);
    CHECK(duckdb_value_double(&r, 1, 0) == .5);
    duckdb_destroy_result(&r);
    REQUIRE_MESSAGE(s.query(
        "SELECT (__parqit_detail(x)).sd, (__parqit_detail(x)).variance, "
        "(__parqit_detail(x)).skewness, (__parqit_detail(x)).kurtosis, "
        "(__parqit_corr(x,y)).rho FROM "
        "(VALUES(1e16::DOUBLE,1),(1e16+2,2),(1e16+4,3),(1e16+6,4)) t(x,y)", &r, &err), err);
    CHECK(duckdb_value_double(&r, 0, 0) == doctest::Approx(std::sqrt(20. / 3)).epsilon(1e-14));
    CHECK(duckdb_value_double(&r, 1, 0) == doctest::Approx(20. / 3).epsilon(1e-14));
    CHECK(duckdb_value_double(&r, 2, 0) == 0);
    CHECK(duckdb_value_double(&r, 3, 0) == doctest::Approx(1.64).epsilon(1e-14));
    CHECK(duckdb_value_double(&r, 4, 0) == 1);
    duckdb_destroy_result(&r);
    REQUIRE_MESSAGE(s.query(
        "SELECT (__parqit_stats(x)).sd, (__parqit_stats(x)).variance, "
        "(__parqit_total(x)).mean FROM (VALUES (1e200),(2e200),(3e200),(4e200)) t(x)", &r, &err), err);
    CHECK(duckdb_value_double(&r, 0, 0) / 1e200 == doctest::Approx(std::sqrt(5. / 3)).epsilon(1e-14));
    CHECK(duckdb_value_is_null(&r, 1, 0));
    CHECK(duckdb_value_double(&r, 2, 0) / 1e200 == doctest::Approx(2.5).epsilon(1e-14));
    duckdb_destroy_result(&r);
    REQUIRE_MESSAGE(s.query(
        "SELECT (__parqit_total(x)).sum, (__parqit_stats(x)).sd FROM "
        "(VALUES(NULL::DOUBLE),(NULL::DOUBLE)) t(x)", &r, &err), err);
    CHECK(duckdb_value_double(&r, 0, 0) == 0);
    CHECK(duckdb_value_is_null(&r, 1, 0));
    duckdb_destroy_result(&r);
}

TEST_CASE("reservoir sampling covers the population including the first row") {
    Session &s = Session::instance();
    std::string value, err;
    int frequencies[3] = {};
    for (int seed = 1; seed <= 512; ++seed) {
        REQUIRE_MESSAGE(s.query_scalar("SELECT i FROM range(1,4) t(i) USING SAMPLE reservoir(1 ROWS) REPEATABLE (" +
                                       std::to_string(seed) + ")", &value, &err), err);
        const int row = std::stoi(value);
        REQUIRE(row >= 1);
        REQUIRE(row <= 3);
        ++frequencies[row - 1];
    }
    for (int count : frequencies) {
        CHECK(count > 80);
        CHECK(count < 260);
    }
    long long maximum = 0;
    for (int seed = 1; seed <= 16; ++seed) {
        REQUIRE_MESSAGE(s.query_scalar("SELECT max(i) FROM (SELECT i FROM range(1,1000001) t(i) "
            "USING SAMPLE reservoir(2 ROWS) REPEATABLE (" + std::to_string(seed) + "))", &value, &err), err);
        maximum = std::max(maximum, std::stoll(value));
    }
    CHECK(maximum > 500000);
    REQUIRE_MESSAGE(s.query_scalar("SELECT count(*) FROM (SELECT i FROM range(1,10001) t(i) "
        "USING SAMPLE reservoir(10%) REPEATABLE(5))", &value, &err), err);
    CHECK(value == "1000");
}

TEST_CASE("windowed stable aggregates support a constant global frame") {
    Session &s = Session::instance();
    std::string err;
    duckdb_result r;
    REQUIRE_MESSAGE(s.query("SELECT (__parqit_stats(x) OVER ()).sd FROM "
        "(VALUES(1e-200),(2e-200),(3e-200),(10e-200)) t(x)", &r, &err), err);
    REQUIRE(duckdb_row_count(&r) == 4);
    for (idx_t i = 0; i < 4; ++i)
        CHECK(duckdb_value_double(&r, 0, i) / 1e-200 == doctest::Approx(std::sqrt(50. / 3)).epsilon(1e-14));
    duckdb_destroy_result(&r);
}

TEST_CASE("parallel integer decimal and boolean aggregate states match centered oracles") {
    Session &s = Session::instance();
    std::string err;
    const std::string query =
        "SELECT g, (__parqit_stats(d)).sd, (__parqit_stats(h)).sd, stddev_samp(r::DOUBLE), "
        "(__parqit_total(b)).mean, avg(b::INTEGER), (__parqit_corr(d,h)).rho FROM "
        "(SELECT i%4 AS g, i%101 AS r, "
        "1234567890123.123456::DECIMAL(38,6)+(i%101)::DECIMAL(38,6) AS d, "
        "9007199254740993::HUGEINT+i%101 AS h, i%3=0 AS b FROM range(2000000) t(i)) "
        "GROUP BY g ORDER BY g";
    for (int threads : {1, 8}) {
        REQUIRE_MESSAGE(s.set_threads(threads, &err), err);
        duckdb_result result;
        REQUIRE_MESSAGE(s.query(query, &result, &err), err);
        REQUIRE(duckdb_row_count(&result) == 4);
        for (idx_t i = 0; i < 4; ++i) {
            const double reference = duckdb_value_double(&result, 3, i);
            CHECK(duckdb_value_double(&result, 1, i) == doctest::Approx(reference).epsilon(1e-12));
            CHECK(duckdb_value_double(&result, 2, i) == doctest::Approx(reference).epsilon(1e-12));
            CHECK(duckdb_value_double(&result, 4, i) == doctest::Approx(duckdb_value_double(&result, 5, i)).epsilon(1e-14));
            CHECK(duckdb_value_double(&result, 6, i) == doctest::Approx(1.).epsilon(1e-14));
        }
        duckdb_destroy_result(&result);
    }
}
