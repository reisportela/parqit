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

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "engine/hexcodec.hpp"
#include "engine/session.hpp"
#include "plugin/plugin_io.hpp"
#include "plugin/plugin_view.hpp"

#ifndef PARQIT_VERSION
#define PARQIT_VERSION "0.0.0-dev"
#endif

namespace {

constexpr ST_retcode kRcUsage = 198;   /* invalid syntax / malformed request */
constexpr ST_retcode kRcEngine = 920;  /* engine (DuckDB) failure */

[[maybe_unused]] void say(const std::string &s) { SF_display(const_cast<char *>(s.c_str())); }
void cry(const std::string &s) {
    std::string line = s;
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

ST_retcode cmd_ping(const std::vector<std::string> &) {
    save_local("_parqit_pong", "1");
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
    save_local("_parqit_duckdb_version", duckdb_library_version());
    save_local("_parqit_spi_version", "3.0");
    return 0;
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
    if (cmd == "use_prepare") return parqit_plugin::cmd_use_prepare(args);
    if (cmd == "use_fetch") return parqit_plugin::cmd_use_fetch(args);
    if (cmd == "describe") return parqit_plugin::cmd_describe(args);
    if (cmd == "save_data") return parqit_plugin::cmd_save_data(args);
    if (cmd == "save_data_direct") return parqit_plugin::cmd_save_data_direct(args);
    if (cmd == "view_open") return parqit_plugin::cmd_view_open(args);
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
    if (cmd == "view_list") return parqit_plugin::cmd_view_list(args);
    if (cmd == "bridge_new") return parqit_plugin::cmd_bridge_new(args);
    if (cmd == "bridge_discard") return parqit_plugin::cmd_bridge_discard(args);
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
