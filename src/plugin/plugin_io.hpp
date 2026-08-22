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
};

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
 * converted to a Parquet bridge in the ado before reaching here. */
Source source_for(const std::vector<std::string> &files, bool relaxed = false,
                  bool csv = false);

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
    parqit::json schema;
    parqit::json vallabs;
    parqit::json chars;
    std::string dtalabel;
    std::vector<std::string> sortedby;
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
};

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
                            const std::function<bool(std::string *)> *pre_publish = nullptr);

/* ---- subcommands ----------------------------------------------------- */

ST_retcode cmd_use_prepare(const std::vector<std::string> &args);
ST_retcode cmd_use_fetch(const std::vector<std::string> &args);
ST_retcode cmd_describe(const std::vector<std::string> &args);
ST_retcode cmd_save_data(const std::vector<std::string> &args);
ST_retcode cmd_save_data_direct(const std::vector<std::string> &args);

} // namespace parqit_plugin
