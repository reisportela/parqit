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

#include <string>

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

    /* Runs SQL, discards the result. Returns false + error message. */
    bool exec(const std::string &sql, std::string *err);

    /* Runs SQL expecting a result; caller must duckdb_destroy_result.
     * Returns false + error message on failure (result already destroyed). */
    bool query(const std::string &sql, duckdb_result *out, std::string *err);

    /* Single string scalar convenience (first row, first column; "" for
     * NULL). */
    bool query_scalar(const std::string &sql, std::string *value, std::string *err);

    duckdb_connection con() const { return con_; }
    const std::string &last_error() const { return last_error_; }

    /* Default temp spill directory to use when none has been configured;
     * set once by the plugin from Stata's tmpdir before first open. */
    void set_default_temp_dir(const std::string &dir) { default_temp_dir_ = dir; }

    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

  private:
    Session() = default;
    ~Session();

    duckdb_database db_ = nullptr;
    duckdb_connection con_ = nullptr;
    std::string last_error_;
    std::string default_temp_dir_;

    /* pending configuration (applied on open; SET when already open) */
    long long threads_ = 0;            /* 0 = engine default */
    std::string memory_limit_;         /* "" = engine default */
    std::string temp_directory_;       /* "" = default_temp_dir_ */
};

/* SQL single-quote escaping for string literals (doubles embedded quotes).
 * Identifiers are quoted with quote_ident. */
std::string quote_literal(const std::string &s);
std::string quote_ident(const std::string &s);

/* Locale-INDEPENDENT double<->text. std::to_string / printf("%g") / strtod all
 * honour LC_NUMERIC, so under a comma-decimal locale they would emit/parse
 * "3,14" and corrupt generated SQL or numeric parsing. dtoa() always uses '.'
 * and the shortest round-trippable form; atod() parses a full numeric string
 * (with optional leading blanks) using '.'. */
std::string dtoa(double v);
bool atod(const std::string &s, double *out);

} // namespace parqit
