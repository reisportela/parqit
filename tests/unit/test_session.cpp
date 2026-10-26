/* Engine capability gate: these tests pin down everything parqit assumes about
 * the vendored DuckDB build. If a DuckDB upgrade drops or changes any of it
 * (as 1.5 did when it moved core functions out of the amalgamation), this
 * file fails before any Stata user can be affected. */
#include "doctest.h"

#include <cstdint>
#include <algorithm>
#include <atomic>
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
#include "engine/typemap.hpp"
#include "engine/scalar_registration.hpp"
#include "json.hpp"
#include "duckdb/common/allocator.hpp"
#include "duckdb/storage/block_allocator.hpp"
#include "test_env.hpp"
#include "test_tmp.hpp"
#include "engine/test_fault.hpp"
#ifdef PARQIT_DUCKDB_200
#include "duckdb/main/connection.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/function/scalar_function.hpp"
#endif

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
    // Exercise worker/TLS teardown with a bounded pool under Valgrind as well.
    REQUIRE(s.set_threads(4, nullptr));
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

#ifdef PARQIT_DUCKDB_200
TEST_CASE("histogram scalar proof reuses dictionaries only for verified literal bounds") {
    auto &s = Session::instance();
    s.close();
    std::string error, value;
    REQUIRE(s.set_threads(4, &error));
    REQUIRE(s.ensure_open());
    static std::atomic<uint64_t> rows;
    auto function = duckdb_create_scalar_function();
    auto any = duckdb_create_logical_type(DUCKDB_TYPE_ANY);
    auto count = duckdb_create_logical_type(DUCKDB_TYPE_UBIGINT);
    auto result_type = duckdb_create_logical_type(DUCKDB_TYPE_DOUBLE);
    duckdb_scalar_function_set_name(function, "__histogram_bound_probe");
    for (int i = 0; i < 3; ++i) duckdb_scalar_function_add_parameter(function, any);
    duckdb_scalar_function_add_parameter(function, count);
    duckdb_scalar_function_set_return_type(function, result_type);
    duckdb_scalar_function_set_special_handling(function);
    // This total identity satisfies the histogram proof's narrower contract;
    // actual histogram values and error ordering are checked separately below.
    duckdb_scalar_function_set_function(function, [](duckdb_function_info, duckdb_data_chunk input, duckdb_vector out) {
        rows += duckdb_data_chunk_get_size(input);
        duckdb_vector_reference_vector(out, duckdb_data_chunk_get_vector(input, 0));
    });
    REQUIRE(parqit::register_known_scalar(s.con(), function, parqit::ScalarErrorPolicy::HistogramBounds) == DuckDBSuccess);
    duckdb_destroy_scalar_function(&function);
    duckdb_destroy_logical_type(&any); duckdb_destroy_logical_type(&count); duckdb_destroy_logical_type(&result_type);
    const auto file = parqit_test::tmp_path("histogram_bound_dictionary.parquet");
    REQUIRE_MESSAGE(s.exec("COPY (SELECT (i%8)::DOUBLE/8 x FROM range(10007) t(i)) TO " +
        parqit::quote_literal(file) + " (FORMAT PARQUET)", &error), error);
    const auto from = " FROM read_parquet(" + parqit::quote_literal(file) + ")";
    rows = 0;
    REQUIRE_MESSAGE(s.query_scalar("SELECT sum(__histogram_bound_probe(x, CAST('0' AS DOUBLE), "
        "CAST('1' AS DOUBLE), 8::UBIGINT))" + from, &value, &error), error);
    CHECK(value == "4377.625");
    CHECK(rows.load() > 0);
    CHECK(rows.load() < 1000);
    rows = 0;
    REQUIRE_MESSAGE(s.query_scalar("SELECT sum(__histogram_bound_probe(x, x, "
        "CAST('1' AS DOUBLE), 8::UBIGINT))" + from, &value, &error), error);
    CHECK(value == "4377.625");
    CHECK(rows.load() == 10007);
    // Bare fractional bins do not implicitly cast to UBIGINT. An explicit
    // conversion to zero must not acquire the totality annotation either.
    duckdb_result fractional{};
    CHECK_FALSE(s.query("SELECT __histogram_bound_probe(x, 0::DOUBLE, 1::DOUBLE, 0.4)" +
        from, &fractional, &error));
    rows = 0;
    REQUIRE_MESSAGE(s.query_scalar("SELECT sum(__histogram_bound_probe(x, 0::DOUBLE, "
        "1::DOUBLE, CAST(0.4 AS UBIGINT)))" + from, &value, &error), error);
    CHECK(value == "4377.625");
    CHECK(rows.load() == 10007);
    // A data-dependent lower bound includes invalid dictionary entries which
    // the WHERE clause removes. Eager dictionary evaluation must not see them.
    REQUIRE_MESSAGE(s.query_scalar("SELECT sum(__parqit_histbin(0.25::DOUBLE, x, "
        "0.5::DOUBLE, 8::UBIGINT))" + from + " WHERE x<=0.5", &value, &error), error);
    CHECK(value == "7506");
    for (const auto &expression : {
            "__parqit_histbin(x, 1::DOUBLE, 0::DOUBLE, 8::UBIGINT)",
            "__parqit_histbin(x, 0::DOUBLE, 1::DOUBLE, 0::UBIGINT)",
            "__parqit_histbin(x, 0::DOUBLE, 1::DOUBLE, 1001::UBIGINT)",
            "__parqit_histbin(x, 0::DOUBLE, 1::DOUBLE, CAST(0.4 AS UBIGINT))",
            "__parqit_histbin(x, CAST('bad' AS DOUBLE), 1::DOUBLE, 8::UBIGINT)",
            "__parqit_histbin(x, 0::BIGINT, 1::DOUBLE, 8::UBIGINT)"}) {
        duckdb_result result{};
        CHECK_FALSE(s.query("SELECT " + std::string(expression) + from, &result, &error));
        REQUIRE_FALSE(error.empty());
        REQUIRE_MESSAGE(s.query("SELECT " + std::string(expression) + from + " WHERE false", &result, &error), error);
        CHECK(duckdb_row_count(&result) == 0);
        duckdb_destroy_result(&result);
    }
    REQUIRE(s.exec("PREPARE __hist_parameter AS SELECT __parqit_histbin(0.25::DOUBLE, "
        "$1::DOUBLE, 1::DOUBLE, 8::UBIGINT)", &error));
    REQUIRE(s.query_scalar("EXECUTE __hist_parameter(0)", &value, &error));
    CHECK(value == "2");
    duckdb_result bad{};
    CHECK_FALSE(s.query("EXECUTE __hist_parameter(2)", &bad, &error));
    REQUIRE(s.query_scalar("EXECUTE __hist_parameter(0)", &value, &error));
    CHECK(value == "2");
    // Integer/decimal bounds use the same exact reader/comparator as runtime.
    REQUIRE(s.query_scalar("SELECT __parqit_histbin(9007199254740993::BIGINT, "
        "'9007199254740992'::BIGINT, '9007199254740994'::BIGINT, 2::UBIGINT)", &value, &error));
    CHECK(value == "1");
    REQUIRE(s.query_scalar("SELECT coalesce(__parqit_histbin(0::DOUBLE, 'NaN'::DOUBLE, "
        "1::DOUBLE, 10::UBIGINT)::VARCHAR, 'NULL')", &value, &error));
    CHECK(value == "NULL");
    REQUIRE(s.query_scalar("SELECT __parqit_histbin(0::BIGINT, "
        "'-9223372036854775808'::BIGINT, '9223372036854775807'::BIGINT, 10::UBIGINT)", &value, &error));
    CHECK(value == "5");
    CHECK_FALSE(s.query("SELECT __parqit_histbin(1::UBIGINT, '18446744073709551615'::UBIGINT, "
        "5::UBIGINT, 10::UBIGINT)", &bad, &error));
    REQUIRE(s.query_scalar("SELECT __parqit_histbin('0.50'::DECIMAL(38,2), "
        "'0.00'::DECIMAL(38,2), '1.00'::DECIMAL(38,2), 2::UBIGINT)", &value, &error));
    CHECK(value == "1");
    std::remove(file.c_str());
    s.close();
}

TEST_CASE("proved scalar contracts retain dictionaries without weakening errors") {
    auto &s = Session::instance();
    s.close();
    std::string error, value;
    REQUIRE(s.set_threads(4, &error));
    REQUIRE(s.ensure_open());
    CHECK(parqit::register_known_scalar(s.con(), nullptr, parqit::ScalarErrorPolicy::Total) == DuckDBError);
    static std::atomic<uint64_t> callback_rows;
    auto add_probe = [&](const char *name, bool known) {
        auto f = duckdb_create_scalar_function();
        auto type = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
        duckdb_scalar_function_set_name(f, name);
        duckdb_scalar_function_add_parameter(f, type);
        duckdb_scalar_function_set_return_type(f, type);
        duckdb_destroy_logical_type(&type);
        duckdb_scalar_function_set_special_handling(f);
        duckdb_scalar_function_set_function(f, [](duckdb_function_info, duckdb_data_chunk input, duckdb_vector out) {
            callback_rows += duckdb_data_chunk_get_size(input);
            duckdb_vector_reference_vector(out, duckdb_data_chunk_get_vector(input, 0));
        });
        auto &native = *reinterpret_cast<duckdb::ScalarFunction *>(f);
        const auto original_binder = native.GetBindCallback();
        const auto rc = known ? parqit::register_known_scalar(s.con(), f, parqit::ScalarErrorPolicy::Total)
                              : duckdb_register_scalar_function(s.con(), f);
        CHECK(native.GetBindCallback() == original_binder);
        if (known) {
            // Reusing the C handle must retain the delegated binder contract.
            duckdb_scalar_function_set_name(f, (std::string(name) + "_reused").c_str());
            CHECK(parqit::register_known_scalar(s.con(), f, parqit::ScalarErrorPolicy::Total) == DuckDBSuccess);
            native.SetBindCallback(nullptr);
            CHECK(parqit::register_known_scalar(s.con(), f, parqit::ScalarErrorPolicy::Total) == DuckDBError);
            CHECK(native.GetBindCallback() == nullptr);
        }
        duckdb_destroy_scalar_function(&f);
        REQUIRE(rc == DuckDBSuccess);
    };
    add_probe("__known_scalar_probe", true);
    add_probe("__ordinary_scalar_probe", false);
    const auto fixture = parqit_test::tmp_path("scalar_dictionary_contract.parquet");
    REQUIRE_MESSAGE(s.exec("COPY (SELECT CASE WHEN i%9=0 THEN NULL ELSE 'v'||(i%8)::VARCHAR END c "
        "FROM range(10007) t(i)) TO " + parqit::quote_literal(fixture) + " (FORMAT PARQUET)", &error), error);
    callback_rows = 0;
    REQUIRE_MESSAGE(s.query_scalar("SELECT count(x)::VARCHAR||':'||sum(strlen(x))::VARCHAR FROM "
        "(SELECT __known_scalar_probe(c) x FROM read_parquet(" + parqit::quote_literal(fixture) + "))",
        &value, &error), error);
    CHECK(value == "8895:17790");
    CHECK(callback_rows.load() > 0);
    CHECK(callback_rows.load() < 1000);
    callback_rows = 0;
    REQUIRE_MESSAGE(s.query_scalar("SELECT count(x)::VARCHAR||':'||sum(strlen(x))::VARCHAR FROM "
        "(SELECT __ordinary_scalar_probe(c) x FROM read_parquet(" + parqit::quote_literal(fixture) + "))",
        &value, &error), error);
    CHECK(value == "8895:17790");
    CHECK(callback_rows.load() == 10007);
    std::remove(fixture.c_str());
    for (const auto &expression : {"__parqit_double(v)", "__parqit_midpoint(1::BIGINT, v::DOUBLE)",
                                   "__parqit_sample_count(1, v::DOUBLE)"}) {
        const std::string data = std::string(expression).find("double(v)") != std::string::npos
            ? "(VALUES ('bad')) t(v)" : "(VALUES (-1)) t(v)";
        duckdb_result result{};
        CHECK_FALSE(s.query("SELECT " + std::string(expression) + " FROM " + data, &result, &error));
        REQUIRE_FALSE(error.empty());
        REQUIRE_MESSAGE(s.query("SELECT " + std::string(expression) + " FROM " + data + " WHERE false",
                                &result, &error), error);
        CHECK(duckdb_row_count(&result) == 0);
        duckdb_destroy_result(&result);
    }
    REQUIRE_MESSAGE(s.exec("PREPARE __typed_scalar_parameter AS SELECT __parqit_double($1)", &error), error);
    REQUIRE(s.query_scalar("EXECUTE __typed_scalar_parameter(123)", &value, &error));
    CHECK(value == "123.0");
    duckdb_result invalid{};
    CHECK_FALSE(s.query("EXECUTE __typed_scalar_parameter('bad')", &invalid, &error));
    REQUIRE(s.query_scalar("EXECUTE __typed_scalar_parameter(456)", &value, &error));
    CHECK(value == "456.0");
    REQUIRE(s.query_scalar("SELECT coalesce(__parqit_midpoint(NULL, 1::DOUBLE)::VARCHAR,'NULL')", &value, &error));
    CHECK(value == "NULL");
    s.close();
}

TEST_CASE("numeric scalar proofs cover widths scales extremes and null types") {
    auto &s = Session::instance();
    s.close();
    std::string error, value;
    REQUIRE(s.set_threads(4, &error));
    struct Type { const char *sql; bool floating; unsigned scale; };
    const Type types[] = {{"BOOLEAN",false,0},{"TINYINT",false,0},{"SMALLINT",false,0},
        {"INTEGER",false,0},{"BIGINT",false,0},{"HUGEINT",false,0},
        {"UTINYINT",false,0},{"USMALLINT",false,0},{"UINTEGER",false,0},
        {"UBIGINT",false,0},{"UHUGEINT",false,0},{"FLOAT",true,0},{"DOUBLE",true,0},
        {"DECIMAL(4,0)",false,0},{"DECIMAL(18,0)",false,0},{"DECIMAL(38,0)",false,0},
        {"DECIMAL(4,2)",false,2},{"DECIMAL(18,2)",false,2},{"DECIMAL(38,2)",false,2}};
    for (const auto &a : types) for (const auto &b : types) {
        CAPTURE(a.sql);
        CAPTURE(b.sql);
        const std::string args = "1::" + std::string(a.sql) + ",0::" + b.sql;
        duckdb_result result{};
        const bool ok = s.query("SELECT __parqit_midpoint(" + args + "),__parqit_difference(" + args + ")",
                                &result, &error);
        if (a.floating == b.floating && a.scale == b.scale) {
            REQUIRE_MESSAGE(ok, error);
            CHECK(duckdb_value_double(&result, 0, 0) == 0.5);
            CHECK(duckdb_value_double(&result, 1, 0) == 1.0);
            duckdb_destroy_result(&result);
        } else CHECK_FALSE(ok);
    }
    duckdb_result result{};
    REQUIRE_MESSAGE(s.query("SELECT __parqit_midpoint('-170141183460469231731687303715884105728'::HUGEINT, "
        "'170141183460469231731687303715884105727'::HUGEINT), "
        "__parqit_difference('170141183460469231731687303715884105727'::HUGEINT, "
        "'-170141183460469231731687303715884105728'::HUGEINT), "
        "__parqit_midpoint('99999999999999999999999999999999999999'::DECIMAL(38,0), "
        "'-99999999999999999999999999999999999999'::DECIMAL(38,0)), "
        "__parqit_difference('99999999999999999999999999999999999999'::DECIMAL(38,0), "
        "'-99999999999999999999999999999999999999'::DECIMAL(38,0))", &result, &error), error);
    CHECK(duckdb_value_double(&result, 0, 0) == -0.5);
    CHECK(duckdb_value_double(&result, 1, 0) == std::ldexp(1.0,128));
    CHECK(duckdb_value_double(&result, 2, 0) == 0.0);
    CHECK(duckdb_value_double(&result, 3, 0) == 2e38);
    duckdb_destroy_result(&result);
    REQUIRE_MESSAGE(s.query_scalar("SELECT hex(encode(parqit_substr_bytes('a'||chr(0)||'b',1,20))) || ':' || "
        "strlen(parqit_substr_bytes('abc',-9223372036854775808::DOUBLE,9223372036854775807::DOUBLE))::VARCHAR || ':' || "
        "parqit_stata_string('NaN'::DOUBLE) || ':' || coalesce(__parqit_hms('Infinity'::DOUBLE,0,0)::VARCHAR,'NULL')",
        &value, &error), error);
    CHECK(value == "610062:0:.:NULL");
    s.close();
}

TEST_CASE("asynchronous I/O workers follow the requested query thread budget") {
    Session &s = Session::instance();
    s.close();
    std::string error, value;
    REQUIRE(s.set_threads(1, &error));
    REQUIRE(s.ensure_open());
    REQUIRE(s.query_scalar("SELECT current_setting('async_threads')::VARCHAR", &value, &error));
    CHECK(value == "1");
    auto &context = *reinterpret_cast<duckdb::Connection *>(s.con())->context;
    CHECK(duckdb::TaskScheduler::GetScheduler(context).NumberOfAsyncThreads() == 1);
    REQUIRE(s.set_threads(4, &error));
    REQUIRE(s.query_scalar("SELECT current_setting('async_threads')::VARCHAR", &value, &error));
    CHECK(value == "4");
    CHECK(duckdb::TaskScheduler::GetScheduler(context).NumberOfAsyncThreads() == 4);
    REQUIRE(s.set_threads(1, &error));
    CHECK(duckdb::TaskScheduler::GetScheduler(context).NumberOfAsyncThreads() == 1);
    REQUIRE_MESSAGE(s.query_scalar("SELECT sum(i)::VARCHAR FROM range(100007) t(i)", &value, &error), error);
    CHECK(value == "5000650021");
    s.close();
}

TEST_CASE("streaming default and reset retain the legacy memory budget") {
    auto &s = Session::instance();
    s.close();
    std::string error;
    REQUIRE(s.ensure_open());
    auto &context = *reinterpret_cast<duckdb::Connection *>(s.con())->context;
    auto &config = duckdb::ClientConfig::GetConfig(context);
    CHECK(config.max_streaming_buffer_size == 1000000);
    REQUIRE(s.set_streaming_buffer_bytes(2000000, &error));
    CHECK(config.max_streaming_buffer_size == 2000000);
    REQUIRE(s.exec("RESET streaming_buffer_size", &error));
    CHECK(config.max_streaming_buffer_size == 1000000);
    REQUIRE(s.exec("SET memory_limit='500KB'; RESET streaming_buffer_size", &error));
    // RESET uses the client default, also under a smaller global database
    // limit: independently confirmed against the pinned 1.5.6 runtime.
    CHECK(config.max_streaming_buffer_size == 1000000);
    s.close();
}
#endif

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

TEST_CASE("outer join bounds preserve empty-side sums and wide integer accumulation") {
    auto &s = Session::instance();
    s.close();
    std::string error, value;
    REQUIRE(s.set_threads(4, &error));
    REQUIRE(s.exec("CREATE TABLE __parqit200_left(v INTEGER); "
                   "INSERT INTO __parqit200_left VALUES(-1),(2); "
                   "CREATE TABLE __parqit200_right(k BOOLEAN); "
                   "CREATE TABLE __parqit200_wide(v BIGINT); "
                   "INSERT INTO __parqit200_wide VALUES(9223372036854775807),(9223372036854775807)", &error));
    const char *joins[] = {"LEFT", "RIGHT", "FULL OUTER", "SEMI", "ANTI"};
    const char *empty_expected[] = {"1:true:2", "NULL:NULL:0", "1:true:2", "NULL:NULL:0", "1:true:2"};
    const char *false_expected[] = {"1:true:2", "NULL:NULL:1", "1:true:3", "NULL:NULL:0", "1:true:2"};
    for (int empty = 1; empty >= 0; --empty) {
        if (!empty) REQUIRE(s.exec("INSERT INTO __parqit200_right VALUES(false)", &error));
        for (size_t j = 0; j < 5; ++j) {
            const std::string sql = "SELECT coalesce(sum(v)::VARCHAR,'NULL') || ':' || "
                "coalesce((sum(v)>0)::VARCHAR,'NULL') || ':' || count(*)::VARCHAR "
                "FROM __parqit200_left t " + std::string(joins[j]) +
                " JOIN __parqit200_right s ON s.k";
            REQUIRE_MESSAGE(s.query_scalar(sql, &value, &error), error);
            CHECK(value == (empty ? empty_expected[j] : false_expected[j]));
        }
        REQUIRE_MESSAGE(s.query_scalar("SELECT sum(v)::VARCHAR FROM __parqit200_wide t "
            "LEFT JOIN __parqit200_right s ON s.k", &value, &error), error);
        CHECK(value == "18446744073709551614");
    }
    s.close();
}

#ifdef PARQIT_DUCKDB_200
TEST_CASE("volatile window keys are evaluated once per row") {
    auto &s = Session::instance();
    std::string error, value;
    for (int threads : {1, 4}) {
        s.close();
        REQUIRE(s.set_threads(threads, &error));
        REQUIRE(s.exec("CREATE SEQUENCE __parqit200_sequence", &error));
        REQUIRE_MESSAGE(s.query_scalar("SELECT count(*)::VARCHAR || ':' || min(n)::VARCHAR || ':' || max(n)::VARCHAR "
            "FROM (SELECT count(*) OVER(PARTITION BY (nextval('__parqit200_sequence')-1)//2) n "
            "FROM range(10000))", &value, &error), error);
        CHECK(value == "10000:2:2");
        REQUIRE(s.query_scalar("SELECT currval('__parqit200_sequence')::VARCHAR", &value, &error));
        CHECK(value == "10000");
        REQUIRE(s.exec("CREATE SEQUENCE __parqit200_order_sequence", &error));
        REQUIRE_MESSAGE(s.query_scalar("SELECT count(*)::VARCHAR || ':' || min(n)::VARCHAR || ':' || max(n)::VARCHAR "
            "FROM (SELECT row_number() OVER(ORDER BY nextval('__parqit200_order_sequence')) n "
            "FROM range(10000))", &value, &error), error);
        CHECK(value == "10000:1:10000");
        REQUIRE(s.query_scalar("SELECT currval('__parqit200_order_sequence')::VARCHAR", &value, &error));
        CHECK(value == "10000");
    }
    s.close();
}

TEST_CASE("materialised volatile keys preserve partitions peers frames and bindings") {
    auto &s = Session::instance();
    std::string error, value;
    constexpr int n = 10007; // Deliberately not aligned with a data vector.
    for (int threads : {1, 4}) {
        s.close();
        REQUIRE(s.set_threads(threads, &error));
        REQUIRE(s.exec("CREATE SEQUENCE key_partition; CREATE SEQUENCE key_peers; "
                       "CREATE SEQUENCE key_both; CREATE SEQUENCE key_range; "
                       "CREATE SEQUENCE key_offset; CREATE SEQUENCE key_plan", &error));
        duckdb_result actual{}, expected{};
        REQUIRE_MESSAGE(s.query("SELECT n, count(*) AS rows FROM (SELECT count(*) OVER "
            "(PARTITION BY hash(nextval('key_partition'))%17) n FROM range(10007)) "
            "GROUP BY n ORDER BY n", &actual, &error), error);
        // An independent GROUP BY on known sequence values; no window operator.
        REQUIRE_MESSAGE(s.query("SELECT n, count(*)*n AS rows FROM "
            "(SELECT count(*) n FROM range(1,10008) t(i) GROUP BY hash(i)%17) "
            "GROUP BY n ORDER BY n", &expected, &error), error);
        REQUIRE(duckdb_row_count(&actual) == duckdb_row_count(&expected));
        for (idx_t row = 0; row < duckdb_row_count(&actual); ++row) {
            CHECK(duckdb_value_int64(&actual, 0, row) == duckdb_value_int64(&expected, 0, row));
            CHECK(duckdb_value_int64(&actual, 1, row) == duckdb_value_int64(&expected, 1, row));
        }
        duckdb_destroy_result(&actual);
        duckdb_destroy_result(&expected);
        REQUIRE(s.query_scalar("SELECT currval('key_partition')::VARCHAR", &value, &error));
        CHECK(value == std::to_string(n));

        REQUIRE_MESSAGE(s.query_scalar("SELECT count(*)::VARCHAR || ':' || "
            "count(DISTINCT rn)::VARCHAR || ':' || bool_and(rn=r AND r=d)::VARCHAR "
            "FROM (SELECT row_number() OVER w rn, rank() OVER w r, dense_rank() OVER w d "
            "FROM range(10007) WINDOW w AS (ORDER BY nextval('key_peers')))", &value, &error), error);
        CHECK(value == "10007:10007:true");
        REQUIRE(s.query_scalar("SELECT currval('key_peers')::VARCHAR", &value, &error));
        CHECK(value == std::to_string(n));

        REQUIRE_MESSAGE(s.query_scalar("SELECT count(*)::VARCHAR || ':' || "
            "min(r)::VARCHAR || ':' || max(r)::VARCHAR FROM "
            "(SELECT row_number() OVER(PARTITION BY (nextval('key_both')-1)//2 "
            "ORDER BY nextval('key_both')) r FROM range(10007))", &value, &error), error);
        CHECK(value == "10007:1:2");
        REQUIRE(s.query_scalar("SELECT currval('key_both')::VARCHAR", &value, &error));
        CHECK(value == std::to_string(2*n));

        REQUIRE_MESSAGE(s.query_scalar("SELECT sum(n)::VARCHAR FROM (SELECT count(*) OVER "
            "(ORDER BY nextval('key_range') RANGE BETWEEN 1.5 PRECEDING AND CURRENT ROW) n "
            "FROM range(10007))", &value, &error), error);
        CHECK(value == std::to_string(2*n-1));
        REQUIRE(s.query_scalar("SELECT currval('key_range')::VARCHAR", &value, &error));
        CHECK(value == std::to_string(n));
        REQUIRE_MESSAGE(s.query_scalar("SELECT sum(n)::VARCHAR FROM (SELECT count(*) OVER "
            "(ORDER BY nextval('key_offset') RANGE BETWEEN nextval('key_offset') PRECEDING "
            "AND CURRENT ROW) n FROM range(10007))", &value, &error), error);
        CHECK(value == std::to_string(int64_t(n)*(n+1)/2));
        REQUIRE(s.query_scalar("SELECT currval('key_offset')::VARCHAR", &value, &error));
        CHECK(value == std::to_string(2*n));

        REQUIRE_MESSAGE(s.query("EXPLAIN (FORMAT JSON) SELECT i, count(*) OVER "
            "(PARTITION BY hash(nextval('key_plan'))%17) n FROM range(10007) t(i)", &actual, &error), error);
        REQUIRE(duckdb_row_count(&actual) == 1);
        char *raw = duckdb_value_varchar(&actual, 1, 0);
        REQUIRE(raw != nullptr);
        auto plan = nlohmann::json::parse(raw);
        duckdb_free(raw);
        duckdb_destroy_result(&actual);
        int projected_calls = 0, window_nodes = 0;
        auto visit = [&](auto &&self, const nlohmann::json &nodes, bool below_window) -> void {
            for (const auto &node : nodes) {
                const auto name = node.at("name").get<std::string>();
                const auto info = node.at("extra_info").dump();
                if (name == "WINDOW") {
                    ++window_nodes;
                    CHECK(info.find("nextval") == std::string::npos);
                }
                if (info.find("nextval") != std::string::npos) {
                    CHECK(name == "PROJECTION");
                    CHECK(below_window);
                    ++projected_calls;
                }
                self(self, node.at("children"), below_window || name == "WINDOW");
            }
        };
        visit(visit, plan, false);
        CHECK(window_nodes == 1);
        CHECK(projected_calls == 1);
    }
    s.close();
}
#endif

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

    /* Fetch actual extrema with the production scan fence, then compare in
     * C++: SQL statistics must not be allowed to fold the protection itself. */
    REQUIRE_MESSAGE(s.query(
        "SELECT min(v)::HUGEINT, max(v)::HUGEINT, first(1) FROM read_parquet(" + path + ")",
        &result, &error), error);
    REQUIRE(duckdb_row_count(&result) == 1);
    REQUIRE(duckdb_column_count(&result) == 3);
    CHECK(duckdb_column_type(&result, 0) == DUCKDB_TYPE_HUGEINT);
    CHECK(duckdb_column_type(&result, 1) == DUCKDB_TYPE_HUGEINT);
    CHECK(duckdb_column_type(&result, 2) == DUCKDB_TYPE_INTEGER);
    for (idx_t column = 0; column < 3; ++column)
        CHECK_FALSE(duckdb_value_is_null(&result, column, 0));
    CHECK_FALSE(parqit::beyond_stata_integer_guard(duckdb_value_hugeint(&result, 0, 0)));
    CHECK(parqit::beyond_stata_integer_guard(duckdb_value_hugeint(&result, 1, 0)));
    CHECK(duckdb_value_hugeint(&result, 0, 0).lower == 2147483648ULL);
    CHECK(duckdb_value_hugeint(&result, 1, 0).lower == 9007199254740993ULL);
    CHECK(duckdb_value_int32(&result, 2, 0) == 1);
    duckdb_destroy_result(&result);
    CHECK(read_bytes(forged) == altered);
    // Stronger hostile metadata: min=max=0 must not make the scan fence a
    // constant. Keep data pages byte-identical and change only footer extrema.
    std::string constant_footer = original;
    for (const auto &truth : {little_endian(2147483648ULL), true_max}) {
        size_t count = 0, at = footer_begin;
        while ((at = constant_footer.find(truth, at)) != std::string::npos && at + 8 <= footer_end) {
            constant_footer.replace(at, 8, little_endian(0));
            at += 8;
            ++count;
        }
        REQUIRE(count > 0);
    }
    REQUIRE(constant_footer.substr(0, footer_begin) == original.substr(0, footer_begin));
    REQUIRE(constant_footer.substr(footer_end) == original.substr(footer_end));
    const auto constant_path = parqit_test::tmp_path("precision_footer_constant.parquet");
    {
        std::ofstream output(constant_path, std::ios::binary | std::ios::trunc);
        output.write(constant_footer.data(), static_cast<std::streamsize>(constant_footer.size()));
        REQUIRE(output.good());
    }
    const auto fence = "SELECT min(v)::HUGEINT, max(v)::HUGEINT, first(1) FROM read_parquet(" +
        parqit::quote_literal(constant_path) + ")";
    REQUIRE_MESSAGE(s.query(fence, &result, &error), error);
    CHECK(duckdb_value_hugeint(&result, 0, 0).lower == 2147483648ULL);
    CHECK(duckdb_value_hugeint(&result, 1, 0).lower == 9007199254740993ULL);
    CHECK(parqit::beyond_stata_integer_guard(duckdb_value_hugeint(&result, 1, 0)));
    duckdb_destroy_result(&result);
    REQUIRE_MESSAGE(s.query("EXPLAIN (FORMAT JSON) " + fence, &result, &error), error);
    char *plan_text = duckdb_value_varchar(&result, 1, 0);
    REQUIRE(plan_text != nullptr);
    const std::string plan(plan_text);
    CHECK(plan.find("READ_PARQUET") != std::string::npos);
    CHECK(plan.find("DUMMY_SCAN") == std::string::npos);
    duckdb_free(plan_text);
    duckdb_destroy_result(&result);
    CHECK(read_bytes(constant_path) == constant_footer);
    std::remove(constant_path.c_str());
    std::remove(honest.c_str());
    std::remove(forged.c_str());
}

TEST_CASE("PART-SCHEMA-PARALLEL-1: queries on several connections return what serial queries return") {
    using parqit::quote_literal;
    parqit::Session &s = parqit::Session::instance();
    std::string err;
    std::vector<std::string> files;
    for (int i = 0; i < 37; i++) {
        const std::string f = parqit_test::tmp_path("qtm_" + std::to_string(i) + ".parquet");
        REQUIRE_MESSAGE(s.exec("COPY (SELECT " + std::to_string(i) + " AS a, 'x' AS b, NULL::VARCHAR AS c) TO " +
                                   quote_literal(f) + " (FORMAT parquet, KV_METADATA {'parqit.test': '" +
                                   std::to_string(i % 3) + "'})",
                               &err),
                        err);
        files.push_back(f);
    }
    {
        std::ofstream cut(files[22], std::ios::binary | std::ios::trunc);
        cut << "PAR1 cut";
    }
    std::vector<std::string> sqls;
    for (size_t at = 0; at < files.size(); at += 5) {
        std::string l = "[";
        for (size_t i = at; i < files.size() && i < at + 5; i++) l += (i > at ? ", " : "") + quote_literal(files[i]);
        l += "]";
        sqls.push_back("SELECT file_name, name, duckdb_type FROM parquet_schema(" + l + ")");
        sqls.push_back("SELECT file_name, decode(key), decode(value) FROM parquet_kv_metadata(" + l + ")");
    }
    sqls.insert(sqls.begin() + 3, "SELECT FROM WHERE");          /* an error among them */
    sqls.push_back("SELECT NULL::VARCHAR AS n, 'v' AS t");          /* NULL is not "" */
    auto sorted = [](std::vector<parqit::Session::TextResult> r) {
        for (auto &q : r) std::sort(q.rows.begin(), q.rows.end());
        return r;
    };
    const auto one = sorted(s.query_text_many(sqls, 1));
    REQUIRE(one.size() == sqls.size());
    CHECK(!one[3].err.empty());
    CHECK(one[0].err.empty());
    size_t failed = 0;
    for (const auto &q : one) failed += !q.err.empty();
    CHECK(failed == 3); /* the syntax error and the two queries of the cut file's chunk */
    CHECK(one.back().rows.size() == 1);
    CHECK(!one.back().rows[0][0].has_value());
    CHECK(*one.back().rows[0][1] == "v");
    for (int threads : {2, 8, 64}) {
        INFO("threads " << threads);
        const auto many = sorted(s.query_text_many(sqls, threads));
        REQUIRE(many.size() == one.size());
        for (size_t k = 0; k < one.size(); k++) {
            INFO("query " << k);
            CHECK(many[k].err == one[k].err);
            CHECK(many[k].rows == one[k].rows);
        }
    }
    CHECK(s.query_text_many({}, 8).empty());
    for (const auto &f : files) std::remove(f.c_str());
}

TEST_CASE("Codex finding 2: a failing query worker is reported in its place, never ending the process") {
    /* std::bad_alloc thrown before query k (PARQIT_TEST_FAULT=text_many:<k>) on a worker or on the calling
     * thread: query k reports it, the others run, on 1 and 8 connections */
    parqit::Session &s = parqit::Session::instance();
    const std::string bad_alloc_message = std::bad_alloc().what();
    std::vector<std::string> sqls;
    for (int i = 0; i < 12; i++) sqls.push_back("SELECT " + std::to_string(i) + " AS v");
    for (int threads : {1, 8})
        for (int k : {0, 5, 11}) {
            INFO("threads " << threads << " query " << k);
            parqit_test::ScopedEnv env("PARQIT_TEST_FAULT", "text_many:" + std::to_string(k));
            const auto r = s.query_text_many(sqls, threads);
            REQUIRE(r.size() == sqls.size());
            for (size_t i = 0; i < r.size(); i++) {
                if (static_cast<int>(i) == k) {
                    CHECK(r[i].err == bad_alloc_message);
                    CHECK(r[i].rows.empty());
                } else {
                    CHECK(r[i].err.empty());
                    REQUIRE(r[i].rows.size() == 1);
                    CHECK(*r[i].rows[0][0] == std::to_string(i));
                }
            }
        }
    /* Codex final audit: fail inside result_varchar, so reverting its guard
     * makes the query silently succeed with a missing metadata cell. */
    {
        const std::vector<std::string> meta = {
            "SELECT 'parqit.k' || CAST(i AS VARCHAR) AS k, 'v' || CAST(i AS VARCHAR) AS v "
            "FROM range(3) t(i) ORDER BY i"};
        for (int threads : {1, 8}) {
            parqit::test_text_copy_armed().store(true);
            const auto r = s.query_text_many(meta, threads);
            REQUIRE(r.size() == 1);
            CHECK(r[0].err == bad_alloc_message);
            CHECK(r[0].rows.empty());
        }
        const auto clean = s.query_text_many(meta, 1);
        REQUIRE(clean[0].rows.size() == 3);
        CHECK(*clean[0].rows[2][1] == "v2");
    }
    /* round 2: the wording fails too, after the worker's failure: it throws
     * on the calling thread once the workers have stopped */
    for (int k : {0, 5}) {
        parqit_test::ScopedEnv env("PARQIT_TEST_FAULT",
                                   "text_many:" + std::to_string(k) + ",text_many_word:" + std::to_string(k));
        CHECK_THROWS_AS(s.query_text_many(sqls, 8), std::bad_alloc);
    }
}

TEST_CASE("Codex final finding 3: failed C text copies throw for every supported cell") {
    Session &s = Session::instance();
    const std::vector<std::string> values = {
        "'text'::VARCHAR", "42::INTEGER", "encode('blob')",
        "TIMESTAMP '2026-10-25 12:34:56'"};
    for (const auto &value : values) {
        INFO(value);
        duckdb_result res;
        std::string err;
        REQUIRE_MESSAGE(s.query("SELECT " + value, &res, &err), err);
        const parqit::ResultGuard guard{&res};
        parqit::test_text_copy_armed().store(true);
        CHECK_THROWS_AS(parqit::result_text(&res, 0, 0), std::bad_alloc);
        parqit::test_text_copy_armed().store(true);
        CHECK_THROWS_AS(parqit::text_cell(res, 0), std::bad_alloc);
        CHECK_FALSE(parqit::test_text_copy_armed().load());
        CHECK_FALSE(parqit::text_cell(res, 0).empty());
    }
    duckdb_result res;
    std::string err;
    REQUIRE_MESSAGE(s.query("SELECT encode(''), [1,2], NULL::VARCHAR, encode('a' || chr(0) || 'b')",
                             &res, &err), err);
    const parqit::ResultGuard guard{&res};
    const parqit::CText empty = parqit::result_text(&res, 0, 0);
    REQUIRE(empty != nullptr);
    CHECK(std::string(empty.get()).empty());
    CHECK(parqit::text_cell(res, 0) == "");
    parqit::test_text_copy_armed().store(true);
    CHECK(parqit::text_cell(res, 0) == ""); /* legitimate malloc(0) */
    CHECK(parqit::result_text(&res, 1, 0) == nullptr);
    parqit::test_text_copy_armed().store(true);
    CHECK(parqit::result_text(&res, 1, 0) == nullptr);
    CHECK(parqit::result_text(&res, 2, 0) == nullptr);
    CHECK(parqit::text_cell(res, 3) == std::string("a\0b", 3));
}
