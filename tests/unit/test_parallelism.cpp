// Observe DuckDB workers directly, independently of OpenMP or timing estimates.
#include "doctest.h"
#include "duckdb.h"
#include "engine/session.hpp"
#include "test_tmp.hpp"
#include <cstdio>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace {
std::mutex worker_mutex;
std::set<std::thread::id> workers;
}

TEST_CASE("DuckDB executes real workers and preserves results without OpenMP") {
    auto &session = parqit::Session::instance();
    std::string error, previous;
    REQUIRE(session.query_scalar("SELECT current_setting('threads')", &previous, &error));
    struct RestoreThreads {
        long long count;
        ~RestoreThreads() { parqit::Session::instance().set_threads(count, nullptr); }
    } restore{std::stoll(previous)};

    auto function = duckdb_create_aggregate_function();
    duckdb_aggregate_function_set_name(function, "__parqit_test_worker_rows");
    auto argument = duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);
    auto result_type = duckdb_create_logical_type(DUCKDB_TYPE_UBIGINT);
    duckdb_aggregate_function_add_parameter(function, argument);
    duckdb_aggregate_function_set_return_type(function, result_type);
    duckdb_destroy_logical_type(&argument);
    duckdb_destroy_logical_type(&result_type);
    duckdb_aggregate_function_set_functions(function,
        [](duckdb_function_info) -> idx_t { return sizeof(uint64_t); },
        [](duckdb_function_info, duckdb_aggregate_state state) {
            *reinterpret_cast<uint64_t *>(state) = 0;
        },
        [](duckdb_function_info, duckdb_data_chunk input, duckdb_aggregate_state *states) {
            {
                std::lock_guard<std::mutex> lock(worker_mutex);
                workers.insert(std::this_thread::get_id());
            }
            for (idx_t i = 0; i < duckdb_data_chunk_get_size(input); ++i)
                ++*reinterpret_cast<uint64_t *>(states[i]);
        },
        [](duckdb_function_info, duckdb_aggregate_state *source,
           duckdb_aggregate_state *target, idx_t count) {
            for (idx_t i = 0; i < count; ++i)
                *reinterpret_cast<uint64_t *>(target[i]) += *reinterpret_cast<uint64_t *>(source[i]);
        },
        [](duckdb_function_info, duckdb_aggregate_state *states,
           duckdb_vector output, idx_t count, idx_t offset) {
            auto *values = static_cast<uint64_t *>(duckdb_vector_get_data(output));
            for (idx_t i = 0; i < count; ++i)
                values[offset + i] = *reinterpret_cast<uint64_t *>(states[i]);
        });
    const auto registered = duckdb_register_aggregate_function(session.con(), function);
    duckdb_destroy_aggregate_function(&function);
    REQUIRE(registered == DuckDBSuccess);

    // Parquet row groups provide independent scan tasks; range() itself is serial.
    const auto path = parqit_test::tmp_path("parqit_test_workers.parquet");
    REQUIRE_MESSAGE(session.exec("COPY (SELECT i FROM range(4194304) t(i)) TO " +
        parqit::quote_literal(path) + " (FORMAT PARQUET, ROW_GROUP_SIZE 65536)", &error), error);
    struct RemoveFixture {
        std::string path;
        ~RemoveFixture() { std::remove(path.c_str()); }
    } fixture{path};

    for (long long threads : {1, 4}) {
        REQUIRE(session.set_threads(threads, &error));
        workers.clear();
        duckdb_result result{};
        REQUIRE_MESSAGE(session.query(
            "SELECT __parqit_test_worker_rows(i), sum(i::HUGEINT) "
            "FROM read_parquet(" + parqit::quote_literal(path) + ")", &result, &error), error);
        CHECK(duckdb_value_uint64(&result, 0, 0) == 4194304);
        CHECK(duckdb_value_int64(&result, 1, 0) == 8796090925056LL);
        duckdb_destroy_result(&result);
        CAPTURE(threads);
        CAPTURE(workers.size());
        CHECK(workers.size() >= (threads == 1 ? 1 : 2));
        CHECK(workers.size() <= static_cast<size_t>(threads));
    }
}
