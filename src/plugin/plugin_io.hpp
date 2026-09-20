/* parqit — plugin I/O subcommands and the machinery shared with the lazy
 * view (M2): source construction, column planning, response records, the
 * prepared-read handoff and the verified parquet writer. */
#pragma once

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "json.hpp"
#include "stplugin.h"

#include "engine/request.hpp"
#include "engine/session.hpp"
#include "engine/typemap.hpp"

namespace parqit_plugin {

/* ---- shared machinery ---------------------------------------------- */

struct Source {
    std::string paths_sql; /* ['f1', 'f2'] — for parquet_* table functions */
    std::string scan_sql;  /* read_parquet([...]) or any SELECT-able ref   */
    bool hive = false;     /* a directory source: hive_partitioning = true, so
                            * the scan appends the partition-key columns after
                            * the files' own leaves */
    bool relaxed = false;  /* union_by_name = true: the scan's columns are the
                            * case-insensitive union of the files' (deduped)
                            * names in DuckDB's file order (A2-2 relaxed) */
    std::string csv_first_sql; /* CSV-HEADER-1: the first delimited-text path
                                * or pattern as a quoted SQL literal so
                                * plan_columns can read the raw header names
                                * the CSV reader deduplicated; "" for
                                * Parquet/temp-table sources */
    /* FILENAME-1: the name of the provenance column the scan carries for
     * `filename(newvar)` (DuckDB `filename = '<name>'`); "" when off. DuckDB
     * appends it AFTER the files' own columns and BEFORE the Hive partition
     * keys, so every leaf alignment counts the scan without it and no Hive or
     * name-clash check may see it. */
    std::string filename_column;
    /* CSV-OPT-1: the user's forced CSV dialect, so the CSV-HEADER-1
     * raw-header probe reads the file exactly as the scan does */
    struct CsvDialect {
        bool has_delim = false, has_quote = false, has_escape = false;
        std::string delim, quote, escape;
        int header = -1; /* -1 not forced, 0 = header(off), 1 = header(on) */
    } csv_dialect;
};

/* CSV-OPT-1: the validated `csv(...)` sub-options of `parqit use`. `sql` is
 * the option fragment spliced into read_csv_auto(...) — every user string is
 * a quote_literal'd SQL literal and every key comes from the whitelist in
 * csv_options_from_request(). */
struct CsvOptions {
    std::string sql;
    Source::CsvDialect dialect;
};

/* Decode and validate req["csvopts"] (hex-encoded values, whitelisted keys).
 * Returns 198 with a message naming the offending key/value, 0 when absent.
 * Needs the session: a types() type name is proved against the engine here,
 * so an unknown type is parqit's own message and not the binder's SQL dump. */
ST_retcode csv_options_from_request(parqit::Session &s, const parqit::json &req,
                                    CsvOptions *out, std::string *err);

/* FILENAME-1: refuse `filename(name)` when the source already exposes a column
 * of that name (case-insensitively: DuckDB resolves identifiers
 * case-insensitively, so two such columns could not both be addressed).
 * `plain` must be the source built WITHOUT the filename option — the engine's
 * own clash message names DuckDB syntax the Stata user never typed. */
ST_retcode check_provenance_name(parqit::Session &s, const Source &plain,
                                 const std::string &name, std::string *err);

/* FP-2 (audit 2026-08-22, A4-2/A4-3): the identity of a source file. size +
 * mtime alone are not content-sensitive (a same-size rewrite with a restored
 * mtime — cp -p, rsync -a, tar -x — passed the old fingerprint): the identity
 * also carries the device/inode and the ctime (which utime(2) cannot restore)
 * and, for the copysource save, a digest of the Parquet footer bytes. */
struct FileIdentity {
    std::string abs;
    std::string size;   /* decimal */
    std::string mtime;  /* file_time_type rep, decimal */
    std::string ctime;  /* POSIX st_ctim (s.ns); "0" where unavailable */
    std::string inode;  /* "dev:ino"; "0:0" where unavailable */
    std::string footer; /* 16 hex FNV-1a over the footer bytes + length; "" if not requested */
};
bool file_identity(const std::string &path, FileIdentity *id, bool with_footer);
/* "" when identical on every compared field, else the differing field names */
std::string identity_diff(const FileIdentity &a, const FileIdentity &b, bool with_footer);
std::string parquet_footer_digest(const std::string &abs);

/* A4-7 (audit 2026-08-22): rewrite the few raw engine messages a user cannot
 * act on (a foreign Hive tree whose partition values contain '=' …) into a
 * parqit message that names the cause and the remedy; other text passes
 * through unchanged. */
std::string friendly_engine_error(const std::string &err);
/* relaxed: read a heterogeneous-schema glob/file-set with union_by_name
 * (columns matched by name, absent ones filled with missing) — mirrors pq's
 * `relaxed`. Default off: a schema mismatch across files is loud.
 * csv: scan delimited text with read_csv_auto instead of read_parquet. CSV
 * carries no Parquet footer, so paths_sql is left "[]" (the parquet_* metadata
 * paths — dup-name recovery, parqit.* labels, F2 stats sizing — are skipped and
 * columns size from a scan). .dta/.xlsx are not engine-scannable and are
 * converted to a Parquet bridge in the ado before reaching here.
 * filename_column: FILENAME-1 — add the provenance column under this name.
 * csv_opts: CSV-OPT-1 — the validated csv(...) fragment (csv sources only). */
Source source_for(const std::vector<std::string> &files, bool relaxed = false,
                  bool csv = false,
                  const std::string &filename_column = std::string(),
                  const CsvOptions &csv_opts = CsvOptions());

/* Parquet source gates. NM1 (all modes): a column name containing a NUL
 * byte is refused loudly — the SPI's C-string name APIs would truncate it
 * into a silent collision with a sibling column (data lost/duplicated).
 * SCH1/SCH2 (strict only): without `relaxed` the matched files must agree
 * on the resolved schema — DuckDB's plain read_parquet otherwise takes the
 * first file's schema and silently casts (or drops columns of) every later
 * file. One footer-only fingerprint query; a physical-only difference
 * (INT96 vs TIMESTAMP, annotation style) is rescued by resolving one
 * representative per fingerprint; a real column-set or type difference
 * returns a loud rc with the column and both files named. No-op for csv
 * sources; the schema part also skips relaxed and a single literal file. */
ST_retcode strict_schema_gate(parqit::Session &s, const Source &src,
                              const std::vector<std::string> &files,
                              bool relaxed, bool csv, std::string *err);

struct ParqitMeta {
    bool present = false;
    std::string refusal; /* value metadata that cannot safely be discarded */
    parqit::json schema;
    parqit::json vallabs;
    parqit::json chars;
    std::string dtalabel;
    std::vector<std::string> sortedby;
    /* XMISS-1: parqit.xmissing — primary column (true parquet name) ->
     * companion column holding its extended-missing codes. Empty when the
     * key is absent. */
    std::map<std::string, std::string> xmissing;
};

struct PlanContext {
    std::vector<parqit::ColumnPlan> active;
    std::vector<std::string> warnings;
    std::vector<std::pair<std::string, std::string>> drops;
    std::map<std::string, std::string> parquet_names;
    ParqitMeta meta;
    long long nrows = 0;
    /* every matched Parquet file (from the footer pass; empty for CSV / temp
     * tables) — the torn-read guard re-stats them at fetch time (A4-3) */
    std::vector<std::string> files;
    /* scan names of the Hive partition-key columns of a directory source (the
     * columns beyond the files' own leaves), so their recorded Stata type can
     * be restored from the manifest (A1-3) */
    std::set<std::string> hive_columns;
    /* A source that must be REFUSED on every path (eager, lazy, describe): a
     * foreign Hive tree whose partition key clashes only by case with a file
     * column (the engine silently replaces the column's values with the key,
     * V2.6), or a relaxed union whose case-insensitive name matching would
     * cross-wire case-distinct columns of different files (A2-2). plan_columns
     * returns rc 198 with this text; the lazy callers, which otherwise ignore
     * the metadata-only plan's rc, check it explicitly. */
    std::string refusal;
    /* FLOAT-EXACT-1 / TYPE-PARITY-1 on the lazy collect path: the view's
     * carried Stata type and display format per engine column name, handed to
     * the planner BEFORE its range pass (the compiled SELECT carries no
     * parqit.* manifest), so the float-exactness scan and the %td/%tc
     * range-sizing run in-plan exactly as on the eager path. Consulted only
     * for a column the manifest did not describe. */
    std::map<std::string, std::pair<std::string, std::string>> meta_hint; /* type, fmt */
    /* INT64-PROTECT-1 / BINARY-DECODE-1 (INPUTS, set by the caller before
     * plan_columns): what to do with an integer column whose values exceed
     * 2^53, and with a BLOB column. The defaults are the protective ones —
     * refuse the read, drop the blob — so every planner that does not know
     * about the options (describe, the metadata-only probes) keeps the safe
     * behaviour. */
    parqit::Int64Mode int64_mode = parqit::Int64Mode::Refuse;
    parqit::BinaryMode binary_mode = parqit::BinaryMode::Drop;
    /* FILENAME-1: the scan's provenance column (filename()), copied from
     * Source::filename_column. It is a first-class known column: excluded from
     * the leaf alignment count, from Hive tagging, from the parqit.* manifest
     * and from the Hive clash checks, and carries a Stata note instead. "" when
     * the option is off or the column is not in this scan (a projection). */
    std::string provenance_column;
    /* XMISS-1: the Stata names of the active columns whose extended missings
     * the eager fill restores from a companion column (ColumnPlan::xm_source
     * set). The eager readers say so; the lazy open does NOT restore them and
     * prints its own note instead. */
    std::vector<std::string> xmissing_restored;
};

/* INT64-PROTECT-1: the session default for int64() (`parqit set int64
 * refuse|round|string`), consulted by every read that does not carry an
 * explicit option. Refuse until the user changes it. A reference to one
 * function-local static: no static-init order to reason about, and both
 * translation units (cmd_set lives with the views) see the same value. */
parqit::Int64Mode &int64_session_default();

/* FILL-THREADS-SET-1: the session's fill-worker count (`parqit set
 * fill_threads auto|#`): -1 = not set (PARQIT_FILL_THREADS, then the automatic
 * rule, decide), 0/1 = serial, n = that many workers (<= 1024). Same
 * function-local-static idiom as int64_session_default(); read by
 * fill_thread_count() at every fetch and reported by `parqit version`. */
int &fill_threads_session();

/* STREAM-BUFFER-SET-1: the session's streaming-buffer setting (`parqit set
 * stream_buffer_mb auto|#`): -1 = not set (PARQIT_STREAM_BUFFER_MB, then the
 * per-read estimate, decide), 0 = the engine's own default buffer, n = a cap of
 * n megabytes. Same idiom as fill_threads_session(). */
long long &stream_buffer_session();

/* Read the `int64` / `binary` fields of a request into ctx (absent int64 =
 * the session default; absent binary = drop). False with *err set when a
 * field is present but not one of the documented values — the ado validates
 * them first, so that can only be a corrupted request. */
bool read_type_options(const parqit::json &req, PlanContext *ctx, std::string *err);

/* BINARY-DECODE-1: decode() raises on an invalid UTF-8 byte sequence — the
 * loud failure we want, but the engine's message names the function instead of
 * the column and offers SQL advice (`try(decode(…))`, `decode(…, 'replace')`)
 * that a Stata user cannot act on, followed by the generated query. Rewrite it
 * into parqit's own remedy at every point where a binary(text) read can fail:
 * the planner's sizing pass and the collect that materialises a multi-stage
 * view into a temp table. Signature-matched — it returns false and leaves
 * *err untouched for every other engine failure, which keeps its own message
 * and its own rc (v69: no raw engine text on any public surface). */
bool rewrite_decode_failure(std::string *err);

/* Plan the columns of src (schema probe, sanitise, parqit.* metadata, range
 * pass when with_stats). paths_sql == "[]" skips file-metadata lookups —
 * that is how view results (temp tables) reuse this. */
ST_retcode plan_columns(parqit::Session &s, const Source &src,
                        const std::vector<std::string> &varlist, bool with_stats,
                        PlanContext *ctx, std::string *err, bool need_count = true);

void write_var_records(parqit::ResponseWriter &w, const PlanContext &ctx);

/* The manifest's sortedby as Stata names of the ACTIVE columns (the valid
 * prefix, like Stata's sortedby marker); "" when none. */
std::string stata_sortedby_names(const PlanContext &ctx);

/* A1-3: for a Hive partition-key column (arriving as VARCHAR text) whose
 * manifest records a numeric/%tc Stata type, the lazy boundary expression,
 * format and value kind ('n') that restore it; false when the column is not
 * such a key. */
bool hive_boundary_override(const PlanContext &meta_ctx, const std::string &scan_name,
                            std::string *sql, std::string *fmt, char *kind);

/* TORN-READ-1 (A4-3): the identity of every Parquet file a paths list matches,
 * taken BEFORE any schema/count probe runs on it (so a replace landing between
 * the probes and the fetch is detected against the pre-plan state). Empty for
 * CSV / temp-table sources. */
std::vector<FileIdentity> snapshot_source_files(parqit::Session &s,
                                                const std::string &paths_sql);

/* Hand a prepared read to use_fetch. drop_source_after: DROP TABLE the
 * scan (temp collect table) once fetched. `files` are the pre-plan identities
 * the fetch re-checks before and after reading (A4-3). */
void set_prepared_read(const std::string &source_scan_sql,
                       std::vector<parqit::ColumnPlan> plans, long long nrows,
                       const std::string &strl_path, bool drop_source_after,
                       std::string *tag_out,
                       const std::vector<FileIdentity> &files = {});

/* COPY query_sql out to dest as parquet with options + parqit KV metadata;
 * verifies the written payload (engine-reported write count must equal a
 * fresh scan of the destination), writes plain files via tmp+rename.
 * Returns rc; fills *written. */
/* NAME-CASE-1: the Stata-name basis for each scanned column. DuckDB case-dedups
 * the scan names (NUEMP -> NUEMP_1, dup -> dup_1); plan_columns recovers the
 * true parquet names positionally (parquet_names: scan name -> true name). A
 * true name that differs from the scan name ONLY by case is restored (Stata
 * keeps nuemp/NUEMP apart); an EXACT duplicate keeps DuckDB's documented
 * dup, dup_1 so both payloads stay addressable (v10/v52/v60). */
std::vector<std::string> stata_name_basis(const std::vector<std::string> &scan_names,
                                          const std::map<std::string, std::string> &parquet_names);

ST_retcode copy_out_parquet(parqit::Session &s, const std::string &query_sql,
                            const std::string &dest, bool replace,
                            const std::string &compression, long long comp_level,
                            const std::vector<std::string> &partition_by,
                            long long row_group_size,
                            const std::string &kv_metadata_sql_fragment,
                            long long *written, std::string *err,
                            /* NAME-CASE-1: exact leaf names to restore in the
                             * written file's footer (positional; the query's
                             * output names may have been case-deduped by
                             * DuckDB). nullptr = nothing to restore. Plain
                             * single-file targets only. */
                            const std::vector<std::string> *leaf_names = nullptr,
                            /* COPYSOURCE-1: a last check run after the staged
                             * output is verified and BEFORE it is published;
                             * returning false (with a message) discards the
                             * staged output and fails the save loudly. */
                            const std::function<bool(std::string *)> *pre_publish = nullptr,
                            /* PART-MODE-1: "" (whole tree, today's semantics),
                             * "replace" (each partition in the result replaces
                             * its namesake in an existing tree, others stay) or
                             * "append" (new files added into the partitions).
                             * `note` receives the lines the caller prints. */
                            const std::string &partition_mode = std::string(),
                            std::string *note = nullptr);

/* ---- subcommands ----------------------------------------------------- */

ST_retcode cmd_use_prepare(const std::vector<std::string> &args);
ST_retcode cmd_use_fetch(const std::vector<std::string> &args);
ST_retcode cmd_describe(const std::vector<std::string> &args);
ST_retcode cmd_save_data(const std::vector<std::string> &args);
ST_retcode cmd_save_data_direct(const std::vector<std::string> &args);

} // namespace parqit_plugin
