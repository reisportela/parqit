#include "doctest.h"
#include "engine/session.hpp"
#include "abi.h"
#include "test_tmp.hpp"
#include <algorithm>
#include <string>
#ifdef PARQIT_DUCKDB_200
#include "duckdb/main/connection.hpp"
#include "duckdb/main/client_config.hpp"

namespace {
struct NativeSettingScope {
    parqit::Session &s = parqit::Session::instance();
    NativeSettingScope() {
        s.close();
        std::string error;
        REQUIRE(s.set_threads(2, &error));
        REQUIRE(s.set_memory_limit("64MB", &error));
        REQUIRE(s.set_temp_directory(parqit_test::tmp_path("native-settings"), &error));
        REQUIRE(s.ensure_open());
    }
    ~NativeSettingScope() {
        std::string error;
        std::vector<parqit::DuckDBSetting> settings;
        s.native_settings(&settings, &error);
        s.set_optimizers(true, true, &error);
        s.close();
        for (const auto &item : settings) s.release_native_setting(item.canonical);
        s.set_threads(1, &error);
        s.set_memory_limit("", &error);
        s.set_temp_directory("", &error);
    }
};

std::string current(parqit::Session &s, const std::string &key) {
    std::string result, error;
    REQUIRE_MESSAGE(s.query_scalar("SELECT current_setting('" + key + "')::VARCHAR", &result, &error), error);
    return result;
}
} // namespace

TEST_CASE("native setting discovery resolves engine aliases and refuses invalid requests") {
    NativeSettingScope scope;
    auto &s = scope.s;
    std::string error;
    parqit::DuckDBSetting item;
    REQUIRE(s.exec("CREATE TEMP TABLE native_sentinel AS SELECT 7 AS k", &error));
    REQUIRE(s.native_setting_get("worker_threads", &item, &error));
    CHECK(item.canonical == "threads");
    CHECK(item.scope == "GLOBAL");
    CHECK(item.value == std::optional<std::string>("2"));
    REQUIRE(s.native_setting_get("memory_limit", &item, &error));
    CHECK(item.canonical == "max_memory");
    REQUIRE(s.native_setting_get("streaming_buffer_size", &item, &error));
    CHECK(item.canonical == "max_streaming_buffer_size");
    CHECK(item.scope == "LOCAL");
    CHECK_FALSE(s.native_setting_get("not_a_registered_option", &item, &error));
    CHECK(error.find("not registered") != std::string::npos);
    CHECK_FALSE(s.native_setting_set("standard_vector_size", "2048", false, &item, &error));
    CHECK(error.find("read-only") != std::string::npos);
    CHECK_FALSE(s.native_setting_set("threads; DROP TABLE native_sentinel", "2", false, &item, &error));
    CHECK_FALSE(s.native_setting_set("threads", "2; DROP TABLE native_sentinel", false, &item, &error));
    CHECK(current(s, "threads") == "2");
    std::string count;
    REQUIRE(s.query_scalar("SELECT k::VARCHAR FROM native_sentinel", &count, &error));
    CHECK(count == "7");
}

TEST_CASE("native literal values preserve quotes emptiness and SQL NULL") {
    NativeSettingScope scope;
    auto &s = scope.s;
    std::string error;
    parqit::DuckDBSetting item;
    const std::string text = parqit_test::tmp_path("synthetic O'Brien UTF-8 \xc3\xa9");
    CHECK_FALSE(s.native_setting_set("custom_user_agent", text, false, &item, &error));
    CHECK(error.find("while database is running") != std::string::npos);
    REQUIRE_MESSAGE(s.native_setting_set("temp_directory", text, false, &item, &error), error);
    CHECK(item.value == std::optional<std::string>(text));
    s.close();
    REQUIRE(s.ensure_open());
    REQUIRE(s.native_setting_get("temp_directory", &item, &error));
    CHECK(item.value == std::optional<std::string>(text));
    REQUIRE_MESSAGE(s.native_setting_set("temp_directory", "", false, &item, &error), error);
    CHECK(item.value.has_value());
    CHECK(item.value->empty());
    REQUIRE_MESSAGE(s.native_setting_set("operator_memory_limit", std::nullopt, false, &item, &error), error);
    CHECK_FALSE(item.value.has_value());
    CHECK(item.explicit_override);
    REQUIRE(s.native_setting_set("late_materialization_max_rows", "9007199254740993", false, &item, &error));
    CHECK(item.value == std::optional<std::string>("9007199254740993"));
    s.close();
    REQUIRE(s.ensure_open());
    REQUIRE(s.native_setting_get("temp_directory", &item, &error));
    CHECK(item.value == std::optional<std::string>(""));
}

TEST_CASE("native asynchronous threads survive legacy CPU changes and rejected values") {
    NativeSettingScope scope;
    auto &s = scope.s;
    std::string error;
    parqit::DuckDBSetting item;
    REQUIRE(s.native_setting_set("async_threads", "5", false, &item, &error));
    REQUIRE(s.set_threads(3, &error));
    CHECK(current(s, "async_threads") == "5");
    REQUIRE(s.native_setting_set("worker_threads", "2", false, &item, &error));
    CHECK(s.threads() == 2);
    CHECK(current(s, "async_threads") == "5");
    CHECK_FALSE(s.native_setting_set("threads", "-1", false, &item, &error));
    CHECK(s.threads() == 2);
    s.close();
    REQUIRE(s.ensure_open());
    CHECK(current(s, "threads") == "2");
    CHECK(current(s, "async_threads") == "5");
}

TEST_CASE("native connection settings propagate without rounding their original literals") {
    NativeSettingScope scope;
    auto &s = scope.s;
    std::string error;
    parqit::DuckDBSetting item;
    REQUIRE(s.native_setting_set("operator_memory_limit", "7000001B", false, &item, &error));
    REQUIRE(s.native_setting_set("dynamic_or_filter_threshold", "17", false, &item, &error));
    REQUIRE(s.native_setting_set("late_materialization_max_rows", "33", false, &item, &error));
    REQUIRE(s.native_setting_set("streaming_buffer_size", "1234567B", false, &item, &error));
    CHECK(s.native_streaming_override());
    const auto expected = current(s, "operator_memory_limit");
    const auto results = s.query_text_many(std::vector<std::string>(6,
        "SELECT current_setting('operator_memory_limit')::VARCHAR,"
        "current_setting('dynamic_or_filter_threshold')::VARCHAR,"
        "current_setting('late_materialization_max_rows')::VARCHAR"), 3);
    REQUIRE(results.size() == 6);
    for (const auto &result : results) {
        REQUIRE_MESSAGE(result.err.empty(), result.err);
        REQUIRE(result.rows.size() == 1);
        CHECK(result.rows[0][0] == std::optional<std::string>(expected));
        CHECK(result.rows[0][1] == std::optional<std::string>("17"));
        CHECK(result.rows[0][2] == std::optional<std::string>("33"));
    }
    s.close();
    REQUIRE(s.ensure_open());
    auto &context = *reinterpret_cast<duckdb::Connection *>(s.con())->context;
    CHECK(duckdb::ClientConfig::GetConfig(context).max_streaming_buffer_size == 1234567);
    REQUIRE(s.native_setting_set("streaming_buffer_size", std::nullopt, true, &item, &error));
    CHECK(s.native_streaming_override());
    CHECK(duckdb::ClientConfig::GetConfig(context).max_streaming_buffer_size == 1000000);
    s.release_native_setting("max_streaming_buffer_size");
    CHECK_FALSE(s.native_streaming_override());
}

TEST_CASE("native optimizer choices compose with the existing plan guards") {
    NativeSettingScope scope;
    auto &s = scope.s;
    std::string error;
    parqit::DuckDBSetting item;
    REQUIRE(s.set_optimizers(false, false, &error));
    REQUIRE(s.native_setting_set("disabled_optimizers", "join_order", false, &item, &error));
    REQUIRE(s.set_optimizers(false, false, &error));
    auto value = current(s, "disabled_optimizers");
    CHECK(value.find("join_order") != std::string::npos);
    CHECK(value.find("window_self_join") != std::string::npos);
    CHECK(value.find("filter_pushdown") != std::string::npos);
    REQUIRE(s.set_optimizers(true, true, &error));
    CHECK(current(s, "disabled_optimizers") == "join_order");
    REQUIRE(s.native_setting_set("disabled_optimizers", std::nullopt, true, &item, &error));
    REQUIRE(s.set_optimizers(false, false, &error));
    CHECK(current(s, "disabled_optimizers").find("window_self_join") != std::string::npos);
    REQUIRE(s.set_optimizers(true, true, &error));
    CHECK(current(s, "disabled_optimizers").empty());
}

TEST_CASE("native RESET can recover a setting that prevents introspection queries") {
    NativeSettingScope scope;
    auto &s = scope.s;
    std::string error;
    parqit::DuckDBSetting item;
    REQUIRE(s.exec("CREATE TEMP TABLE reset_sentinel AS SELECT 23 AS k", &error));
    const bool changed = s.native_setting_set("max_expression_depth", "1", false, &item, &error);
    std::string blocked;
    const bool queried = s.query_scalar("SELECT k::VARCHAR FROM reset_sentinel", &blocked, &error);
    const bool restored = s.native_setting_set("max_expression_depth", std::nullopt, true, &item, &error);
    REQUIRE_MESSAGE(restored, error);
    CHECK(changed);
    CHECK_FALSE(queried);
    std::string value;
    REQUIRE(s.query_scalar("SELECT k::VARCHAR FROM reset_sentinel", &value, &error));
    CHECK(value == "23");
}

TEST_CASE("native Arrow profiles select regular large and view string layouts") {
    NativeSettingScope scope;
    auto &s = scope.s;
    std::string error;
    parqit::DuckDBSetting item;
    auto format = [&]() {
        duckdb_result result{};
        REQUIRE(s.query("SELECT 'synthetic text' AS s", &result, &error));
        parqit::ResultGuard result_guard{&result};
        auto options = duckdb_result_get_arrow_options(&result);
        auto type = duckdb_column_logical_type(&result, 0);
        const char *name = "s";
        ArrowSchema schema{};
        auto failure = duckdb_to_arrow_schema(options, &type, &name, 1, &schema);
        std::string message = failure ? duckdb_error_data_message(failure) : "";
        if (failure) duckdb_destroy_error_data(&failure);
        std::string text = schema.n_children == 1 ? schema.children[0]->format : "";
        if (schema.release) schema.release(&schema);
        duckdb_destroy_logical_type(&type);
        duckdb_destroy_arrow_options(&options);
        REQUIRE_MESSAGE(message.empty(), message);
        return text;
    };
    CHECK(format() == "u");
    REQUIRE(s.native_setting_set("arrow_large_buffer_size", "true", false, &item, &error));
    CHECK(s.native_arrow_layout());
    CHECK(format() == "U");
    REQUIRE(s.native_setting_set("arrow_output_version", "1.4", false, &item, &error));
    REQUIRE(s.native_setting_set("produce_arrow_string_view", "true", false, &item, &error));
    CHECK(format() == "vu");
}
#endif
