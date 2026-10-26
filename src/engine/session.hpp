/* parqit — the embedded DuckDB session.
 *
 * One in-memory DuckDB instance per Stata session, opened lazily on first
 * use and reconfigurable through `parqit set`. The temp_directory setting is
 * what makes the in-memory instance spill to disk, i.e. what keeps pipelines
 * out-of-core; it is always set (defaulting to a parqit subdirectory of
 * Stata's tmpdir passed in by the ado).
 *
 * No Stata API in this translation unit: the engine layer stays linkable
 * from plain unit tests.
 */
#pragma once

#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "duckdb.h"

namespace parqit {

class Session {
  public:
    static Session &instance();

    /* Opens the database if not yet open. Returns false and sets last_error
     * on failure. Safe to call repeatedly. */
    bool ensure_open();

    /* Closes and re-opens on the next ensure_open(); pending settings are
     * applied then. */
    void close();

    /* Configuration requested before/while open. Applied via SET when the
     * database is live (empty string = engine default). */
    bool set_threads(long long n, std::string *err);
    bool set_memory_limit(const std::string &limit, std::string *err);
    bool set_temp_directory(const std::string &dir, std::string *err);
    /* CPUS-1: the engine thread count in force — the user's `parqit set
     * threads`, else the CPUs available to this process (the default applied
     * at open). Does not open the engine. */
    long long threads() const;

    /* PERF-STREAM-1: how many bytes of result the engine may keep buffered
     * ahead of a streaming fetch (`streaming_buffer_size`, a LOCAL/connection
     * setting). Sized per fetch from the result's estimated size — see
     * estimate_transfer_bytes and the note on query_streaming below — and only
     * ever while no stream is live. */
    bool set_streaming_buffer_bytes(long long bytes, std::string *err);

    /* Runs SQL, discards the result. Returns false + error message. */
    bool exec(const std::string &sql, std::string *err);

    /* Runs SQL expecting a result; caller must duckdb_destroy_result.
     * Returns false + error message on failure (result already destroyed). */
    bool query(const std::string &sql, duckdb_result *out, std::string *err);

    /* Runs SQL as a STREAMING result: the engine's pipeline produces chunks on
     * demand for duckdb_fetch_chunk instead of materialising the whole result
     * first (which is what duckdb_query does — duckdb.h:1209). On success the
     * caller owns *out and must duckdb_destroy_result it.
     * A NULL chunk from duckdb_fetch_chunk means end-of-stream OR an error set
     * on the result (capi/stream-c.cpp:17-37): the caller MUST check
     * duckdb_result_error(out) before destroying the result.
     * While the stream is live no other statement may run on this connection —
     * the next one cancels it (client_context.cpp:689-693, 311-318). A stream
     * abandoned before end-of-stream should therefore be followed by one
     * trivial statement so its parked tasks and file handles are released now. */
    bool query_streaming(const std::string &sql, duckdb_result *out, std::string *err);

    /* Single string scalar convenience (first row, first column; "" for
     * NULL). */
    bool query_scalar(const std::string &sql, std::string *value, std::string *err);

    /* PART-SCHEMA-PARALLEL-1: independent read-only queries run on up to
     * `threads` connections to this database at once (this session's and
     * others opened for the call), for work the engine does serially within
     * a query, such as reading many Parquet footers. Each query's rows as
     * text (an empty optional for NULL), or its error, in the order given:
     * what running them one after another returns. */
    struct TextResult {
        std::string err; /* empty: the query ran */
        std::vector<std::vector<std::optional<std::string>>> rows;
        std::exception_ptr failure; /* internal: a worker's failure, worded after the join */
    };
    std::vector<TextResult> query_text_many(const std::vector<std::string> &sqls, int threads);

    /* Register nonthrowing moment aggregates only for an overflow retry. */
    bool prepare_stats_fallback(std::string *err);

    duckdb_connection con() const { return con_; }
    const std::string &last_error() const { return last_error_; }

    /* Default temp spill directory to use when none has been configured;
     * set once by the plugin from Stata's tmpdir before first open. */
    void set_default_temp_dir(const std::string &dir) { default_temp_dir_ = dir; }
    /* WINDOW-NEST-1: enable or disable DuckDB's window_self_join optimizer
     * for the session (SET/RESET disabled_optimizers, issued only on a
     * change); see View::fragile_windows(). */
    bool set_window_self_join(bool enabled, std::string *err) {
        return set_optimizers(enabled, filter_pushdown_, err);
    }
    /* WINDOW-NEST-1: the two optimizers parqit turns off per view */
    bool set_optimizers(bool window_self_join, bool filter_pushdown, std::string *err);

    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

  private:
    Session() = default;
    ~Session();

    duckdb_database db_ = nullptr;
    duckdb_connection con_ = nullptr;
    std::string last_error_;
    std::string default_temp_dir_;
    bool window_self_join_ = true; /* WINDOW-NEST-1: the optimizers' state */
    bool filter_pushdown_ = true;
    bool stats_fallback_ready_ = false;

    /* pending configuration (applied on open; SET when already open) */
    long long threads_ = 0;            /* 0 = engine default */
    std::string memory_limit_;         /* "" = engine default */
    std::string temp_directory_;       /* "" = default_temp_dir_ */
};

/* SQL single-quote escaping for string literals (doubles embedded quotes).
 * Identifiers are quoted with quote_ident. */
std::string quote_literal(const std::string &s);
std::string quote_ident(const std::string &s);

/* duckdb_value_varchar, except that a cell that is not NULL but comes back
 * without text throws std::bad_alloc instead of reading as NULL: the C API
 * returns nullptr for both, its copy failing to allocate
 * (capi/cast/utils.hpp ToCStringCastWrapper, generic.hpp TryCastCInternal;
 * Codex audit, round 2, finding 2). nullptr still means SQL NULL; the caller
 * frees the text with duckdb_free. A type the C API has no text for keeps
 * its answer (c_api_text_type). */
char *result_varchar(duckdb_result *res, idx_t col, idx_t row);

/* Length-bearing text extraction shared with the plugin and its unit tests
 * (Codex final audit, finding 3). */
std::string text_cell(duckdb_result &res, idx_t col, idx_t row = 0,
                      const std::string &missing = ".");

/* result_varchar's text, freed with duckdb_free when it goes out of scope,
 * and a materialised result destroyed when its guard does, so that a failure
 * between two reads leaks neither (Fable final audit, A7) */
using CText = std::unique_ptr<char, void (*)(void *)>;
inline CText result_text(duckdb_result *res, idx_t col, idx_t row) {
    return CText(result_varchar(res, col, row), duckdb_free);
}
struct ResultGuard {
    duckdb_result *res;
    ~ResultGuard() { reset(); }
    void reset() {
        if (res) duckdb_destroy_result(res);
        res = nullptr;
    }
};

/* whether the old C API turns a cell of this type into text: the 21 types of
 * capi/cast/generic.hpp GetInternalCValue, the same in 1.5.6 and 2.0.0 (Fable
 * final audit, A2: the guard covered VARCHAR only) */
bool c_api_text_type(duckdb_type t);

/* Locale-INDEPENDENT double<->text. std::to_string / printf("%g") / strtod all
 * honour LC_NUMERIC, so under a comma-decimal locale they would emit/parse
 * "3,14" and corrupt generated SQL or numeric parsing. dtoa() always uses '.'
 * and a 17-significant-digit round-trippable form; atod() parses a full numeric string
 * (with optional leading blanks) using '.'. */
std::string dtoa(double v);
bool atod(const std::string &s, double *out);

/* CPUS-1: the CPUs available to THIS process — on Linux the affinity mask
 * (what a SLURM/cgroup allocation or `taskset` leaves visible; `nproc` and
 * Stata's c(processors_mach) agree with it), std::thread::hardware_concurrency()
 * elsewhere and as the fallback; never below 1. Every thread count in parqit
 * (the engine's `threads`, the fill workers) defaults to it and is clamped to
 * it: the machine is the only limit, nothing is hard-coded. DuckDB's own
 * default ignores the mask (48 under `taskset -c 0-7` on a 48-core box), which
 * is why the session applies this number at open. */
int available_cpus();

/* Per-process spill subdirectory ("/_parqit_spill_<pid>"). The default spill
 * lives under c(tmpdir), which is shared by every Stata instance on the
 * machine (and every user on an HPC login node) — two concurrent parqit
 * sessions pointing DuckDB at the SAME temp_directory can interfere, so the
 * pid keeps each session's spill private (SPILL-1). */
std::string spill_suffix();

} // namespace parqit
