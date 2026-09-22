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
#include <cstdlib>
#include <memory>
#include <fstream>
#include <iterator>
#include <string>

#include "abi.h" /* Arrow C Data Interface */
#include "duckdb.h"
#include "engine/session.hpp"
#include "duckdb/common/allocator.hpp"
#include "duckdb/storage/block_allocator.hpp"
#include "test_tmp.hpp"

using parqit::Session;

TEST_CASE("destroying an unrelated block allocator preserves live pool capacity") {
    static size_t fallback_calls;
    fallback_calls = 0;
    duckdb::Allocator fallback(
        [](duckdb::PrivateAllocatorData *, idx_t size) {
            ++fallback_calls;
            return static_cast<duckdb::data_ptr_t>(std::malloc(size));
        },
        [](duckdb::PrivateAllocatorData *, duckdb::data_ptr_t pointer, idx_t) { std::free(pointer); },
        [](duckdb::PrivateAllocatorData *, duckdb::data_ptr_t pointer, idx_t, idx_t size) {
            ++fallback_calls;
            return static_cast<duckdb::data_ptr_t>(std::realloc(pointer, size));
        }, nullptr);
    constexpr idx_t block_size = 4096, blocks = 128;
    auto other = std::make_unique<duckdb::BlockAllocator>(fallback, block_size,
                                                        block_size * blocks, block_size * blocks);
    duckdb::BlockAllocator live(fallback, block_size, block_size * blocks, block_size * blocks);
    std::vector<duckdb::data_ptr_t> pointers;
    pointers.push_back(live.AllocateData(block_size));
    pointers.front()[0] = 42;
    other.reset();
    for (idx_t i = 1; i < blocks; ++i) pointers.push_back(live.AllocateData(block_size));
    CHECK(fallback_calls == 0);
    CHECK(pointers.front()[0] == 42);
    for (auto pointer : pointers) live.FreeData(pointer, block_size);
}

TEST_CASE("session close reopen and process teardown preserve allocator lifetimes") {
    Session &s = Session::instance();
    s.close();
    REQUIRE(s.ensure_open());
    duckdb_result result{};
    std::string error, value;
    CHECK_FALSE(s.query("SELECT CAST(value AS BIGINT) FROM (VALUES ('1'), ('bad')) t(value)",
                        &result, &error));
    CHECK_FALSE(error.empty());
    s.close();
    REQUIRE(s.ensure_open());
    REQUIRE(s.query_scalar("SELECT 1", &value, &error));
    CHECK(value == "1");
    // Leave the final session open: process teardown must handle TLS destruction.
}

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


TEST_CASE("vectorized temporal boundaries preserve finite extrema and reject infinities") {
    Session &s = Session::instance();
    REQUIRE(s.ensure_open());
    struct Function { const char *name; duckdb_type input, output; const char *type, *error; };
    const Function functions[] = {
        {"__parqit_date_days", DUCKDB_TYPE_DATE, DUCKDB_TYPE_BIGINT, "DATE", "infinite DATE"},
        {"__parqit_timestamp_ms", DUCKDB_TYPE_TIMESTAMP, DUCKDB_TYPE_BIGINT, "TIMESTAMP", "infinite TIMESTAMP"},
        {"__parqit_timestamp_ns_ms", DUCKDB_TYPE_TIMESTAMP_NS, DUCKDB_TYPE_BIGINT, "TIMESTAMP_NS", "infinite TIMESTAMP_NS"},
        {"__parqit_timestamp_ns_us", DUCKDB_TYPE_TIMESTAMP_NS, DUCKDB_TYPE_TIMESTAMP, "TIMESTAMP_NS", "infinite TIMESTAMP_NS"}
    };
    auto check_raw = [&](size_t index, int64_t raw, int64_t expected, const char *expected_error = nullptr) {
        const auto &f = functions[index];
        INFO(std::string(f.name) << " raw=" << raw);
        duckdb_prepared_statement stmt = nullptr;
        const std::string sql = "SELECT " + std::string(f.name) + "($1)";
        REQUIRE(duckdb_prepare(s.con(), sql.c_str(), &stmt) == DuckDBSuccess);
        duckdb_value value;
        if (f.input == DUCKDB_TYPE_DATE) value = duckdb_create_date({static_cast<int32_t>(raw)});
        else if (f.input == DUCKDB_TYPE_TIMESTAMP) value = duckdb_create_timestamp({raw});
        else value = duckdb_create_timestamp_ns({raw});
        const auto bound = duckdb_bind_value(stmt, 1, value);
        duckdb_destroy_value(&value);
        CHECK(bound == DuckDBSuccess);
        duckdb_result result{};
        const auto rc = duckdb_execute_prepared(stmt, &result);
        const char *message = duckdb_result_error(&result);
        const std::string error = message ? message : "";
        if (expected_error) {
            INFO("unexpected integer result=" << (rc == DuckDBSuccess && f.output == DUCKDB_TYPE_BIGINT
                 ? duckdb_value_int64(&result, 0, 0) : 0));
            CHECK(rc == DuckDBError);
            CHECK(error.find(expected_error) != std::string::npos);
        } else {
            CHECK_MESSAGE(rc == DuckDBSuccess, error);
            if (rc == DuckDBSuccess) {
                CHECK(duckdb_column_type(&result, 0) == f.output);
                CHECK_FALSE(duckdb_value_is_null(&result, 0, 0));
                if (f.output == DUCKDB_TYPE_TIMESTAMP)
                    CHECK(duckdb_value_timestamp(&result, 0, 0).micros == expected);
                else CHECK(duckdb_value_int64(&result, 0, 0) == expected);
            }
        }
        duckdb_destroy_result(&result);
        duckdb_destroy_prepare(&stmt);
    };
    check_raw(0, -2147483646LL, -2147479993LL);
    check_raw(0, 2147483646LL, 2147487299LL);
    check_raw(0, -3653, 0);
    check_raw(1, -9223372036854775806LL, -9223056417654776LL);
    check_raw(1, 9223372036854774999LL, 9223687656054774LL);
    check_raw(1, -9223372036854775000LL, 0, "not exactly representable");
    check_raw(1, 9223372036854775806LL, 0, "not exactly representable");
    check_raw(2, -9223372036854775806LL, -8907752836855LL);
    check_raw(2, 9223372036854775806LL, 9538991236854LL);
    check_raw(3, -9223372036854775806LL, -9223372036854776LL);
    check_raw(3, 9223372036854775806LL, 9223372036854775LL);
    for (int64_t raw : {-1001LL, -1000LL, -999LL, -1LL, 0LL, 1LL, 999LL, 1000LL, 1001LL}) {
        const int64_t micros = raw < 0 ? -((-raw + 999) / 1000) : raw / 1000;
        const int64_t millis = raw < 0 ? -((-raw + 999999) / 1000000) : raw / 1000000;
        check_raw(1, raw, micros + 315619200000LL);
        check_raw(2, raw, millis + 315619200000LL);
        check_raw(3, raw, micros);
    }
    for (size_t i = 0; i < 4; ++i) {
        const auto &f = functions[i];
        const int64_t limit = i == 0 ? 2147483647LL : 9223372036854775807LL;
        check_raw(i, -limit, 0, f.error);
        check_raw(i, limit, 0, f.error);
        duckdb_result result{};
        std::string error;
        REQUIRE_MESSAGE(s.query("SELECT " + std::string(f.name) + "(NULL::" + f.type + ")",
                                &result, &error), error);
        CHECK(duckdb_column_type(&result, 0) == f.output);
        CHECK(duckdb_value_is_null(&result, 0, 0));
        duckdb_destroy_result(&result);
    }
}

TEST_CASE("vectorized temporal boundaries handle constants nulls and filtered multi-chunk vectors") {
    Session &s = Session::instance();
    std::string error;
    duckdb_result result{};
    REQUIRE_MESSAGE(s.query(
        "SELECT __parqit_date_days(DATE '1960-01-01'), "
        "__parqit_timestamp_ms(TIMESTAMP '1960-01-01'), "
        "__parqit_timestamp_ns_ms(TIMESTAMP_NS '1960-01-01'), "
        "epoch_us(__parqit_timestamp_ns_us(TIMESTAMP_NS '1969-12-31 23:59:59.999999999')) "
        "FROM range(5003)", &result, &error), error);
    CHECK(duckdb_row_count(&result) == 5003);
    for (idx_t row = 0; row < duckdb_row_count(&result); ++row) {
        CHECK(duckdb_value_int64(&result, 0, row) == 0);
        CHECK(duckdb_value_int64(&result, 1, row) == 0);
        CHECK(duckdb_value_int64(&result, 2, row) == 0);
        CHECK(duckdb_value_int64(&result, 3, row) == -1);
    }
    duckdb_destroy_result(&result);
    REQUIRE_MESSAGE(s.query(
        "WITH temporal_values AS MATERIALIZED (SELECT i, "
        "CASE WHEN i%7=0 THEN NULL ELSE DATE '1970-01-01'+CAST(i-4000 AS INTEGER) END d, "
        "CASE WHEN i%7=0 THEN NULL ELSE make_timestamp((i-4000)*1001) END ts, "
        "CASE WHEN i%7=0 THEN NULL ELSE make_timestamp_ns((i-4000)*1001) END ns "
        "FROM range(9001) t(i)) "
        "SELECT i, __parqit_date_days(d), __parqit_timestamp_ms(ts), "
        "__parqit_timestamp_ns_ms(ns), epoch_us(__parqit_timestamp_ns_us(ns)) "
        "FROM temporal_values WHERE i%3<>0 ORDER BY i DESC", &result, &error), error);
    REQUIRE(duckdb_row_count(&result) == 6000);
    for (idx_t row = 0; row < duckdb_row_count(&result); ++row) {
        const int64_t i = duckdb_value_int64(&result, 0, row);
        CHECK(i % 3 != 0);
        for (idx_t col = 1; col <= 4; ++col)
            CHECK(duckdb_value_is_null(&result, col, row) == (i % 7 == 0));
        if (i % 7 == 0) continue;
        const int64_t raw = (i - 4000) * 1001;
        const int64_t us = raw < 0 ? -((-raw + 999) / 1000) : raw / 1000;
        const int64_t ms = raw < 0 ? -((-raw + 999999) / 1000000) : raw / 1000000;
        CHECK(duckdb_value_int64(&result, 1, row) == i - 4000 + 3653);
        CHECK(duckdb_value_int64(&result, 2, row) == us + 315619200000LL);
        CHECK(duckdb_value_int64(&result, 3, row) == ms + 315619200000LL);
        CHECK(duckdb_value_int64(&result, 4, row) == us);
    }
    duckdb_destroy_result(&result);
    CHECK_FALSE(s.query(
        "SELECT sum(__parqit_timestamp_ms(CASE WHEN i=4500 THEN TIMESTAMP '-infinity' "
        "ELSE make_timestamp(i*1000) END)) FROM range(5003) t(i)", &result, &error));
    CHECK(error.find("infinite TIMESTAMP cannot be represented in Stata") != std::string::npos);
    CHECK_FALSE(s.query(
        "SELECT sum(__parqit_timestamp_ns_ms(CASE WHEN i=4500 THEN TIMESTAMP_NS 'infinity' "
        "ELSE make_timestamp_ns(i*1000) END)) FROM range(5003) t(i)", &result, &error));
    CHECK(error.find("infinite TIMESTAMP_NS cannot be represented in Stata") != std::string::npos);
}

TEST_CASE("temporal executors preserve NULL masks at word and chunk boundaries") {
    Session &s = Session::instance();
    REQUIRE(s.ensure_open());
    const idx_t sizes[] = {0, 1, 63, 64, 65, 2048, 2049, 4097};
    for (const idx_t count : sizes) {
        for (const bool mixed_nulls : {true, false}) {
            INFO("count=" << count << " mixed_nulls=" << mixed_nulls);
            const std::string missing = mixed_nulls
                ? "i % 64 IN (0,63) OR i = " + std::to_string(count) + " - 1"
                : "false";
            const std::string sql =
                "WITH inputs AS MATERIALIZED (SELECT i, CASE WHEN " + missing +
                " THEN NULL ELSE DATE '1970-01-01' + CAST(i-32 AS INTEGER) END d, "
                "CASE WHEN " + missing + " THEN NULL ELSE make_timestamp((i-32)*1001) END ts, "
                "CASE WHEN " + missing + " THEN NULL ELSE make_timestamp_ns((i-32)*1001) END ns "
                "FROM range(" + std::to_string(count) + ") t(i)) "
                "SELECT i, __parqit_date_days(d), __parqit_timestamp_ms(ts), "
                "__parqit_timestamp_ns_ms(ns), epoch_us(__parqit_timestamp_ns_us(ns)), "
                "__parqit_date_days(NULL::DATE), __parqit_timestamp_ms(NULL::TIMESTAMP), "
                "__parqit_timestamp_ns_ms(NULL::TIMESTAMP_NS), "
                "epoch_us(__parqit_timestamp_ns_us(NULL::TIMESTAMP_NS)) "
                "FROM inputs ORDER BY i";
            duckdb_result result{};
            std::string error;
            REQUIRE_MESSAGE(s.query(sql, &result, &error), error);
            REQUIRE(duckdb_row_count(&result) == count);
            REQUIRE(duckdb_column_count(&result) == 9);
            for (idx_t col = 1; col < 9; ++col)
                CHECK(duckdb_column_type(&result, col) == DUCKDB_TYPE_BIGINT);
            for (idx_t row = 0; row < count; ++row) {
                const int64_t i = static_cast<int64_t>(row);
                CHECK(duckdb_value_int64(&result, 0, row) == i);
                const bool missing_expected = mixed_nulls &&
                    (row % 64 == 0 || row % 64 == 63 || row + 1 == count);
                for (idx_t col = 1; col <= 4; ++col)
                    CHECK(duckdb_value_is_null(&result, col, row) == missing_expected);
                for (idx_t col = 5; col < 9; ++col)
                    CHECK(duckdb_value_is_null(&result, col, row));
                if (missing_expected) continue;
                const int64_t raw = (i - 32) * 1001;
                const int64_t microseconds = raw < 0 ? -((-raw + 999) / 1000) : raw / 1000;
                const int64_t milliseconds = raw < 0 ? -((-raw + 999999) / 1000000) : raw / 1000000;
                CHECK(duckdb_value_int64(&result, 1, row) == i - 32 + 3653);
                CHECK(duckdb_value_int64(&result, 2, row) == microseconds + 315619200000LL);
                CHECK(duckdb_value_int64(&result, 3, row) == milliseconds + 315619200000LL);
                CHECK(duckdb_value_int64(&result, 4, row) == microseconds);
            }
            duckdb_destroy_result(&result);
        }
    }
}

TEST_CASE("temporal executors skip filtered infinities and recover after selected errors") {
    Session &s = Session::instance();
    REQUIRE(s.ensure_open());
    struct Function {
        const char *name, *type, *finite, *error;
        int64_t expected;
        bool timestamp_result;
    };
    const Function functions[] = {
        {"__parqit_date_days", "DATE", "DATE '1969-12-31'", "infinite DATE", 3652, false},
        {"__parqit_timestamp_ms", "TIMESTAMP", "TIMESTAMP '1969-12-31 23:59:59.999999'",
         "infinite TIMESTAMP", 315619199999LL, false},
        {"__parqit_timestamp_ns_ms", "TIMESTAMP_NS", "TIMESTAMP_NS '1969-12-31 23:59:59.999999999'",
         "infinite TIMESTAMP_NS", 315619199999LL, false},
        {"__parqit_timestamp_ns_us", "TIMESTAMP_NS", "TIMESTAMP_NS '1969-12-31 23:59:59.999999999'",
         "infinite TIMESTAMP_NS", -1, true}
    };
    for (const auto &f : functions) {
        INFO(f.name);
        /* Low-cardinality input contains both infinities. Filtering selects
         * only finite/NULL rows before the fallible projection is evaluated. */
        const std::string source =
            "WITH inputs AS MATERIALIZED (SELECT i, CASE WHEN i%8=0 THEN " +
            std::string(f.type) + " 'infinity' WHEN i%8=1 THEN " + f.type +
            " '-infinity' WHEN i%8=2 THEN NULL::" + f.type + " ELSE " + f.finite +
            " END v FROM range(4101) t(i)) ";
        std::string value = std::string(f.name) + "(v)";
        if (f.timestamp_result) value = "epoch_us(" + value + ")";
        const std::string filtered = source + "SELECT i, " + value +
            " FROM inputs WHERE i%8>=2 ORDER BY i DESC";
        auto check_filtered = [&]() {
            duckdb_result result{};
            std::string error;
            REQUIRE_MESSAGE(s.query(filtered, &result, &error), error);
            REQUIRE(duckdb_row_count(&result) == 3075);
            REQUIRE(duckdb_column_type(&result, 1) == DUCKDB_TYPE_BIGINT);
            int64_t expected_i = 4100;
            for (idx_t row = 0; row < duckdb_row_count(&result); ++row) {
                while (expected_i % 8 < 2) --expected_i;
                CHECK(duckdb_value_int64(&result, 0, row) == expected_i);
                const bool missing = expected_i % 8 == 2;
                CHECK(duckdb_value_is_null(&result, 1, row) == missing);
                if (!missing)
                    CHECK(duckdb_value_int64(&result, 1, row) == f.expected);
                --expected_i;
            }
            duckdb_destroy_result(&result);
        };
        check_filtered();
        for (const int64_t included : {4096LL, 4097LL}) {
            duckdb_result result{};
            std::string error;
            const bool ok = s.query(source + "SELECT sum(" + value +
                ") FROM inputs WHERE i%8>=2 OR i=" + std::to_string(included),
                &result, &error);
            CHECK_FALSE(ok);
            CHECK(error.find(f.error) != std::string::npos);
            if (ok) duckdb_destroy_result(&result);
            check_filtered();
        }
    }
}

TEST_CASE("precision extrema fence reads payload despite dishonest exact footer statistics") {
    Session &s = Session::instance();
    REQUIRE(s.ensure_open());
    const std::string honest = parqit_test::tmp_path("precision_footer_honest.parquet");
    const std::string forged = parqit_test::tmp_path("precision_footer_forged.parquet");
    std::remove(honest.c_str());
    std::remove(forged.c_str());
    std::string error;
    REQUIRE_MESSAGE(s.exec(
        "COPY (SELECT v FROM (VALUES (2147483648::BIGINT), "
        "(9007199254740993::BIGINT)) t(v)) TO " + parqit::quote_literal(honest) +
        " (FORMAT PARQUET, COMPRESSION UNCOMPRESSED)", &error), error);

    auto read_bytes = [](const std::string &path) {
        std::ifstream input(path, std::ios::binary);
        REQUIRE(input.good());
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    };
    const std::string original = read_bytes(honest);
    REQUIRE(original.size() >= 12);
    REQUIRE(original.compare(0, 4, "PAR1") == 0);
    REQUIRE(original.compare(original.size() - 4, 4, "PAR1") == 0);
    uint32_t footer_size = 0;
    for (unsigned i = 0; i < 4; ++i)
        footer_size |= uint32_t(static_cast<unsigned char>(original[original.size() - 8 + i])) << (8 * i);
    REQUIRE(footer_size <= original.size() - 12);
    const size_t footer_begin = original.size() - 8 - footer_size;
    const size_t footer_end = original.size() - 8;
    auto little_endian = [](uint64_t value) {
        std::string bytes(8, '\0');
        for (unsigned i = 0; i < 8; ++i) bytes[i] = static_cast<char>((value >> (8 * i)) & 255);
        return bytes;
    };
    const std::string true_max = little_endian(9007199254740993ULL);
    const std::string false_max = little_endian(9007199254740992ULL);
    std::string altered = original;
    size_t replacements = 0, position = footer_begin;
    while ((position = altered.find(true_max, position)) != std::string::npos &&
           position + true_max.size() <= footer_end) {
        altered.replace(position, true_max.size(), false_max);
        position += false_max.size();
        ++replacements;
    }
    REQUIRE(replacements > 0);
    REQUIRE(altered.substr(0, footer_begin) == original.substr(0, footer_begin));
    REQUIRE(altered.substr(footer_end) == original.substr(footer_end));
    {
        std::ofstream output(forged, std::ios::binary | std::ios::trunc);
        REQUIRE(output.good());
        output.write(altered.data(), static_cast<std::streamsize>(altered.size()));
        output.close();
        REQUIRE(output.good());
    }
    REQUIRE(read_bytes(forged) == altered);

    duckdb_result result{};
    const std::string path = parqit::quote_literal(forged);
    REQUIRE_MESSAGE(s.query(
        "SELECT stats_min_value, stats_max_value, min_is_exact, max_is_exact "
        "FROM parquet_metadata(" + path + ")", &result, &error), error);
    REQUIRE(duckdb_row_count(&result) == 1);
    REQUIRE(duckdb_column_count(&result) == 4);
    for (idx_t column = 0; column < 2; ++column)
        CHECK(duckdb_column_type(&result, column) == DUCKDB_TYPE_VARCHAR);
    const char *expected_stats[] = {"2147483648", "9007199254740992"};
    for (idx_t column = 0; column < 2; ++column) {
        char *text = duckdb_value_varchar(&result, column, 0);
        REQUIRE(text != nullptr);
        CHECK(std::string(text) == expected_stats[column]);
        duckdb_free(text);
    }
    for (idx_t column = 2; column < 4; ++column) {
        CHECK(duckdb_column_type(&result, column) == DUCKDB_TYPE_BOOLEAN);
        CHECK_FALSE(duckdb_value_is_null(&result, column, 0));
        CHECK(duckdb_value_boolean(&result, column, 0));
    }
    duckdb_destroy_result(&result);

    /* The projection reads the actual two values independently of extrema
     * aggregation; the dishonest footer still claims both extrema are exact. */
    REQUIRE_MESSAGE(s.query("SELECT CAST(v AS VARCHAR) FROM read_parquet(" + path + ")",
                            &result, &error), error);
    REQUIRE(duckdb_row_count(&result) == 2);
    REQUIRE(duckdb_column_type(&result, 0) == DUCKDB_TYPE_VARCHAR);
    std::vector<std::string> payload;
    for (idx_t row = 0; row < 2; ++row) {
        char *text = duckdb_value_varchar(&result, 0, row);
        REQUIRE(text != nullptr);
        payload.emplace_back(text);
        duckdb_free(text);
    }
    duckdb_destroy_result(&result);
    std::sort(payload.begin(), payload.end());
    CHECK(payload == std::vector<std::string>{"2147483648", "9007199254740993"});

    std::string bare_max;
    REQUIRE_MESSAGE(s.query_scalar("SELECT max(v)::VARCHAR FROM read_parquet(" + path + ")",
                                   &bare_max, &error), error);
    INFO("unfenced max=" << bare_max << "; a future engine may independently read the true maximum");

    /* This is the production precision predicate and its scan fence. A
     * future optimizer must not replace it with the dishonest footer. */
    REQUIRE_MESSAGE(s.query(
        "SELECT coalesce(min(v)::HUGEINT < -9007199254740992::HUGEINT OR "
        "max(v)::HUGEINT > 9007199254740992::HUGEINT, false), "
        "min(v), max(v), first(1) FROM read_parquet(" + path + ")",
        &result, &error), error);
    REQUIRE(duckdb_row_count(&result) == 1);
    REQUIRE(duckdb_column_count(&result) == 4);
    CHECK(duckdb_column_type(&result, 0) == DUCKDB_TYPE_BOOLEAN);
    CHECK(duckdb_column_type(&result, 1) == DUCKDB_TYPE_BIGINT);
    CHECK(duckdb_column_type(&result, 2) == DUCKDB_TYPE_BIGINT);
    CHECK(duckdb_column_type(&result, 3) == DUCKDB_TYPE_INTEGER);
    for (idx_t column = 0; column < 4; ++column)
        CHECK_FALSE(duckdb_value_is_null(&result, column, 0));
    CHECK(duckdb_value_boolean(&result, 0, 0));
    CHECK(duckdb_value_int64(&result, 1, 0) == 2147483648LL);
    CHECK(duckdb_value_int64(&result, 2, 0) == 9007199254740993LL);
    CHECK(duckdb_value_int32(&result, 3, 0) == 1);
    duckdb_destroy_result(&result);
    CHECK(read_bytes(forged) == altered);
    std::remove(honest.c_str());
    std::remove(forged.c_str());
}
