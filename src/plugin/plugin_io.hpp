/* parqit — plugin I/O subcommands and the machinery shared with the lazy
 * view (M2): source construction, column planning, response records, the
 * prepared-read handoff and the verified parquet writer. */
#pragma once

#include <map>
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
};
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
};

struct PlanContext {
    std::vector<parqit::ColumnPlan> active;
    std::vector<std::string> warnings;
    std::vector<std::pair<std::string, std::string>> drops;
    std::map<std::string, std::string> parquet_names;
    ParqitMeta meta;
    long long nrows = 0;
};

/* Plan the columns of src (schema probe, sanitise, parqit.* metadata, range
 * pass when with_stats). paths_sql == "[]" skips file-metadata lookups —
 * that is how view results (temp tables) reuse this. */
ST_retcode plan_columns(parqit::Session &s, const Source &src,
                        const std::vector<std::string> &varlist, bool with_stats,
                        PlanContext *ctx, std::string *err, bool need_count = true);

void write_var_records(parqit::ResponseWriter &w, const PlanContext &ctx);

/* Hand a prepared read to use_fetch. drop_source_after: DROP TABLE the
 * scan (temp collect table) once fetched. */
void set_prepared_read(const std::string &source_scan_sql,
                       std::vector<parqit::ColumnPlan> plans, long long nrows,
                       const std::string &strl_path, bool drop_source_after,
                       std::string *tag_out);

/* COPY query_sql out to dest as parquet with options + parqit KV metadata;
 * verifies the written payload (engine-reported write count must equal a
 * fresh scan of the destination), writes plain files via tmp+rename.
 * Returns rc; fills *written. */
ST_retcode copy_out_parquet(parqit::Session &s, const std::string &query_sql,
                            const std::string &dest, bool replace,
                            const std::string &compression, long long comp_level,
                            const std::vector<std::string> &partition_by,
                            long long row_group_size,
                            const std::string &kv_metadata_sql_fragment,
                            long long *written, std::string *err);

/* ---- subcommands ----------------------------------------------------- */

ST_retcode cmd_use_prepare(const std::vector<std::string> &args);
ST_retcode cmd_use_fetch(const std::vector<std::string> &args);
ST_retcode cmd_describe(const std::vector<std::string> &args);
ST_retcode cmd_save_data(const std::vector<std::string> &args);
ST_retcode cmd_save_data_direct(const std::vector<std::string> &args);

} // namespace parqit_plugin
