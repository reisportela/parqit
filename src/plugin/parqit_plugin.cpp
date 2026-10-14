/* parqit — Stata plugin entry point and subcommand dispatch.
 *
 * The ado side always invokes this plugin as
 *     plugin call parqit_plugin [varlist] [in], <subcommand> [hex-args...]
 * Every argument beyond the subcommand is lowercase hex (see
 * engine/hexcodec.hpp), so arbitrary paths and text cross the boundary
 * intact. Results travel back through SF_macro_save into caller locals
 * (`_parqit_*`), arbitrary text hex-encoded.
 *
 * Charter rule (pq audit finding 8): every path out of stata_call returns a
 * real ST_retcode, and every failure also emits an SF_error message. Nothing
 * returns 0 unless the requested operation completed.
 */
#include "stplugin.h"
#include "parqit_build_identity.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "engine/hexcodec.hpp"
#include "engine/legacy_encoding.hpp"
#include "engine/session.hpp"
#include "plugin/plugin_io.hpp"
#include "plugin/plugin_rdata.hpp"
#include "plugin/plugin_spss.hpp"
#include "plugin/plugin_view.hpp"

#ifdef _OPENMP
#error "parqit uses DuckDB's scheduler and must not link an OpenMP runtime"
#endif

#ifndef PARQIT_VERSION
#define PARQIT_VERSION "0.0.0-dev"
#endif

namespace {

constexpr ST_retcode kRcUsage = 198;   /* invalid syntax / malformed request */
constexpr ST_retcode kRcEngine = 920;  /* engine (DuckDB) failure */

[[maybe_unused]] void say(const std::string &s) { SF_display(const_cast<char *>(s.c_str())); }
void cry(const std::string &s) {
    std::string line = parqit::with_encoding_hint(s); /* CSV-ENC-1 */
    line.push_back('\n');
    SF_error(const_cast<char *>(line.c_str()));
}

void save_local(const char *name, const std::string &value) {
    SF_macro_save(const_cast<char *>(name), const_cast<char *>(value.c_str()));
}

[[maybe_unused]] void save_local_hex(const char *name, const std::string &value) {
    save_local(name, parqit::hex_encode(value));
}

bool arg_text(const std::vector<std::string> &args, size_t i, std::string *out) {
    if (i >= args.size()) return false;
    return parqit::hex_decode(args[i], *out);
}

/* --- subcommands ------------------------------------------------------- */

ST_retcode cmd_ping(const std::vector<std::string> &args) {
    if (args.size() != 2 || args[1] != "03") {
        cry("parqit: ado and plugin numerical revisions do not match; install matching files and restart Stata");
        return kRcUsage;
    }
    save_local("_parqit_pong", "1");
    save_local("_parqit_numeric_contract", PARQIT_NUMERIC_CONTRACT);
    save_local("_parqit_build_id", PARQIT_BUILD_ID);
    /* PLUGIN-VERSION-1: the ado-files compare it with their own release */
    save_local("_parqit_plugin_version", PARQIT_VERSION);
    return 0;
}

/* echo <hex>: decode and re-encode — lets the ado verify both hex codecs
 * against each other in `parqit selftest`. */
ST_retcode cmd_echo(const std::vector<std::string> &args) {
    std::string raw;
    if (!arg_text(args, 1, &raw)) {
        cry("parqit echo: malformed hex argument");
        return kRcUsage;
    }
    save_local_hex("_parqit_echo", raw);
    return 0;
}

ST_retcode cmd_version(const std::vector<std::string> &) {
    save_local("_parqit_plugin_version", PARQIT_VERSION);
    save_local("_parqit_numeric_contract", PARQIT_NUMERIC_CONTRACT);
    save_local("_parqit_build_id", PARQIT_BUILD_ID);
    save_local("_parqit_duckdb_version", duckdb_library_version());
    save_local("_parqit_spi_version", "3.0");
    save_local("_parqit_parallel_backend", "duckdb");
    /* FILL-THREADS-SET-1: the session's fill-worker setting, "auto" unless
     * `parqit set fill_threads` chose a number (the environment variable and
     * the automatic rule are not reported here: they are decided per fetch). */
    {
        const int ft = parqit_plugin::fill_threads_session();
        save_local("_parqit_fill_threads", ft < 0 ? std::string("auto") : std::to_string(ft));
        /* CPUS-1 / STREAM-BUFFER-SET-1: the CPUs available to this process, the
         * engine thread count in force (the user's, else those CPUs) and the
         * streaming-buffer setting ("auto" unless parqit set chose a number) */
        save_local("_parqit_cpus", std::to_string(parqit::available_cpus()));
        save_local("_parqit_threads",
                   std::to_string(parqit::Session::instance().threads()));
        const long long sb = parqit_plugin::stream_buffer_session();
        save_local("_parqit_stream_buffer_mb",
                   sb < 0 ? std::string("auto") : std::to_string(sb));
    }
    // Retain the public diagnostic fields introduced in 0.1.36.
    save_local("_parqit_openmp", "0");
    save_local("_parqit_openmp_version", "0");
    save_local("_parqit_openmp_max_threads", "0");
    return 0;
}

/* Probe the embedded optimiser in a separate in-memory database. Its sequence
 * and settings cannot change a user's views, SQL objects or session settings. */
bool verify_window_contract(std::string *err) {
    struct Database {
        duckdb_database db = nullptr;
        duckdb_connection con = nullptr;
        ~Database() {
            if (con) duckdb_disconnect(&con);
            if (db) duckdb_close(&db);
        }
    } probe;
    duckdb_config config = nullptr;
    if (duckdb_create_config(&config) != DuckDBSuccess ||
        duckdb_set_config(config, "threads", "1") != DuckDBSuccess ||
        duckdb_set_config(config, "memory_limit", "64MB") != DuckDBSuccess) {
        duckdb_destroy_config(&config);
        *err = "could not configure the numerical selftest";
        return false;
    }
    char *open_error = nullptr;
    const auto opened = duckdb_open_ext(nullptr, &probe.db, config, &open_error);
    duckdb_destroy_config(&config);
    if (open_error) {
        *err = open_error;
        duckdb_free(open_error);
    }
    if (opened != DuckDBSuccess || duckdb_connect(probe.db, &probe.con) != DuckDBSuccess) {
        if (err->empty()) *err = "could not open the numerical selftest database";
        return false;
    }
    auto query = [&](const std::string &sql, bool check) {
        duckdb_result result{};
        const auto rc = duckdb_query(probe.con, sql.c_str(), &result);
        bool ok = rc == DuckDBSuccess;
        if (!ok) {
            const char *message = duckdb_result_error(&result);
            *err = message ? message : "numerical selftest query failed";
        } else if (check) {
            ok = duckdb_row_count(&result) == 1 && !duckdb_value_is_null(&result, 0, 0) &&
                 duckdb_value_boolean(&result, 0, 0);
            if (!ok) *err = "the embedded engine duplicated a volatile window input";
        }
        duckdb_destroy_result(&result);
        return ok;
    };
    if (!query("CREATE SEQUENCE parqit_probe", false)) return false;
    for (const char *value : {"nextval('parqit_probe')",
                             "list_transform([1], x -> nextval('parqit_probe'))[1]"}) {
        const std::string sql =
            "SELECT count(*)=2 AND bool_and(n=500 AND m=expected) FROM ("
            "SELECT g,count(*) n,min(m) m,avg(x) expected FROM ("
            "SELECT g,x,avg(x) OVER(PARTITION BY g) m FROM ("
            "SELECT i%2 g,(" + std::string(value) + ")::DOUBLE x FROM range(1000) t(i)"
            ")) GROUP BY g)";
        if (!query(sql, true)) return false;
    }
    return query("SELECT count(*)=1000 AND bool_and(i=m) FROM ("
                 "SELECT i,avg(i) OVER(PARTITION BY i*10000+nextval('parqit_probe')) m "
                 "FROM range(1000) t(i))", true);
}

/* selftest <hex tmpdir>: end-to-end engine check inside the Stata process —
 * open the database, write a parquet file with parqit KV metadata, read it
 * back, verify payload and metadata, delete the file. */
ST_retcode cmd_selftest(const std::vector<std::string> &args) {
    /* fault injection: prove the stata_call catch-all turns an uncaught
     * exception into a loud nonzero rc instead of killing Stata */
    if (args.size() >= 2 && args[1] == "throw")
        throw std::runtime_error("deliberate selftest exception");
    std::string tmpdir;
    if (!arg_text(args, 1, &tmpdir) || tmpdir.empty()) {
        cry("parqit selftest: missing tmpdir argument");
        return kRcUsage;
    }
    save_local("_parqit_openmp_threads", "0");
    parqit::Session &s = parqit::Session::instance();
    s.set_default_temp_dir(tmpdir);

    std::string file = tmpdir + "/_parqit_selftest.parquet";
    std::string err, got;

    if (!s.exec("COPY (SELECT range AS i, 'r' || range::VARCHAR AS s FROM range(5)) TO " +
                    parqit::quote_literal(file) +
                    " (FORMAT PARQUET, KV_METADATA {'parqit.selftest': 'ok'})",
                &err)) {
        cry("parqit selftest: parquet write failed: " + err);
        return kRcEngine;
    }
    bool ok =
        s.query_scalar("SELECT count(*)::VARCHAR || '/' || sum(i)::VARCHAR FROM read_parquet(" +
                           parqit::quote_literal(file) + ")",
                       &got, &err) &&
        got == "5/10";
    if (ok)
        ok = s.query_scalar("SELECT decode(value) FROM parquet_kv_metadata(" +
                                parqit::quote_literal(file) +
                                ") WHERE decode(key) = 'parqit.selftest'",
                            &got, &err) &&
             got == "ok";
    std::remove(file.c_str());
    if (!ok) {
        cry("parqit selftest: engine verification failed: " + (err.empty() ? got : err));
        return kRcEngine;
    }
    if (!verify_window_contract(&err)) {
        cry("parqit selftest: numerical contract failed: " + err);
        return kRcEngine;
    }
    save_local("_parqit_selftest", "ok");
    return 0;
}

} // namespace

/* The whole plugin is compiled -fvisibility=hidden so the embedded DuckDB
 * can never clash with another plugin's; the two SPI entry points must be
 * re-exported explicitly (a version script cannot promote hidden symbols). */
#if defined(_WIN32)
#define PARQIT_EXPORT extern "C" __declspec(dllexport)
#else
#define PARQIT_EXPORT extern "C" __attribute__((visibility("default")))
#endif

/* No C++ exception may ever cross the extern "C" SPI boundary (it would
 * take the whole Stata process down); expected failures return ST_retcode,
 * and anything unexpected is converted here. */
PARQIT_EXPORT ST_retcode stata_call(int argc, char *argv[]) try {
    std::vector<std::string> args(argv, argv + argc);
    if (args.empty()) {
        cry("parqit plugin: no subcommand");
        return kRcUsage;
    }
    const std::string &cmd = args[0];
    if (cmd == "ping") return cmd_ping(args);
    if (cmd == "echo") return cmd_echo(args);
    if (cmd == "version") return cmd_version(args);
    if (cmd == "selftest") return cmd_selftest(args);
    parqit_plugin::begin_call(); /* WINDOW-NEST-1: a previous join's using needs end here */
    /* WINDOW-NEST-1: an eager subcommand plans nothing of the current view —
     * it must not inherit the optimizers a heavy view switched off */
    if (cmd == "use_prepare" || cmd == "use_fetch" || cmd == "describe" ||
        cmd == "save_data" || cmd == "save_data_direct" || cmd == "bridge_new" ||
        cmd == "text_prepare" || cmd == "spss_convert" || cmd == "rdata_convert")
        parqit_plugin::eager_optimizers();
    if (cmd == "use_prepare") return parqit_plugin::cmd_use_prepare(args);
    if (cmd == "use_fetch") return parqit_plugin::cmd_use_fetch(args);
    if (cmd == "describe") return parqit_plugin::cmd_describe(args);
    if (cmd == "save_data") return parqit_plugin::cmd_save_data(args);
    if (cmd == "save_data_direct") return parqit_plugin::cmd_save_data_direct(args);
    if (cmd == "view_open") return parqit_plugin::cmd_view_open(args);
    if (cmd == "view_copy") return parqit_plugin::cmd_view_copy(args);
    if (cmd == "view_op") return parqit_plugin::cmd_view_op(args);
    if (cmd == "view_twotable") return parqit_plugin::cmd_view_twotable(args);
    if (cmd == "view_reshape") return parqit_plugin::cmd_view_reshape(args);
    if (cmd == "view_pivot") return parqit_plugin::cmd_view_pivot(args);
    if (cmd == "view_sql") return parqit_plugin::cmd_view_sql(args);
    if (cmd == "view_query") return parqit_plugin::cmd_view_query(args);
    if (cmd == "view_stats") return parqit_plugin::cmd_view_stats(args);
    if (cmd == "path") return parqit_plugin::cmd_path(args);
    if (cmd == "view_info") return parqit_plugin::cmd_view_info(args);
    if (cmd == "view_collect_prepare") return parqit_plugin::cmd_view_collect_prepare(args);
    if (cmd == "view_save") return parqit_plugin::cmd_view_save(args);
    if (cmd == "view_close") return parqit_plugin::cmd_view_close(args);
    if (cmd == "view_switch") return parqit_plugin::cmd_view_switch(args);
    if (cmd == "view_commit") return parqit_plugin::cmd_view_commit(args);
    if (cmd == "view_list") return parqit_plugin::cmd_view_list(args);
    if (cmd == "bridge_new") return parqit_plugin::cmd_bridge_new(args);
    if (cmd == "bridge_discard") return parqit_plugin::cmd_bridge_discard(args);
    if (cmd == "text_prepare") return parqit_plugin::cmd_text_prepare(args);
    if (cmd == "spss_convert") return parqit_plugin::cmd_spss_convert(args);
    if (cmd == "rdata_convert") return parqit_plugin::cmd_rdata_convert(args);
    if (cmd == "view_alive") {
        save_local("_parqit_view_alive", parqit_plugin::view_is_live() ? "1" : "0");
        save_local("_parqit_view_current",
                   parqit::hex_encode(parqit_plugin::view_current_name()));
        return 0;
    }
    if (cmd == "set") return parqit_plugin::cmd_set(args);

    cry("parqit plugin: unknown subcommand '" + cmd + "'");
    return kRcUsage;
} catch (const std::exception &e) {
    cry(std::string("parqit plugin: internal error: ") + e.what());
    return kRcEngine;
} catch (...) {
    cry("parqit plugin: unknown internal error");
    return kRcEngine;
}
