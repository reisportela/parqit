/* parqit — source-column-name → Stata-variable-name sanitiser.
 *
 * The documented, reversible scheme of charter §6.2/§6.10/§6.14:
 *   1. Unicode letters (plus digits/marks after the first character) and "_"
 *      pass; every other code point becomes "_";
 *   2. a leading digit or exact reserved word gains a "_" prefix;
 *   3. names are truncated to 32 Unicode code points;
 *   4. collisions (including with names already taken) get a numbered
 *      suffix, deterministically, case-sensitively;
 *   5. an empty result becomes v<position>.
 * The original name always travels in the manifest and in parqit.* metadata —
 * sanitisation never loses it.
 */
#pragma once

#include <set>
#include <string>
#include <vector>

namespace parqit {

bool is_reserved_stata_name(const std::string &name);

/* ASCII-only lowercase fold — the comparison DuckDB applies to identifiers. */
std::string ascii_lower(const std::string &s);

/* NAME-CASE-1: DuckDB identifiers are case-insensitive, so two columns whose
 * names differ only by case cannot coexist in one relation, while Stata keeps
 * them distinct. Returns one engine alias per name: a name whose lowercase form
 * is unique among `names` and absent from `taken_lower` keeps itself; every
 * later case-clash gets the smallest `_<k>` suffix (k from 1) that is unique
 * case-insensitively against every name, every alias chosen so far and the
 * taken set. Deterministic; the alias always starts with the original name. */
std::vector<std::string> engine_unique_ci(const std::vector<std::string> &names,
                                          const std::set<std::string> &taken_lower = {});

/* Stata varlist wildcard matching over Unicode code points: * matches any run
 * and ? exactly one character. Invalid UTF-8 never matches. */
bool glob_match(const std::string &pattern, const std::string &name);

/* Sanitise one candidate (steps 1–3 + 5); no uniqueness handling. */
std::string sanitize_stata_name(const std::string &source, size_t position_1based);

/* Sanitise a whole column list with deterministic dedup (step 4).
 * renamed[i] is true when out[i] != sources[i]. */
std::vector<std::string> sanitize_unique(const std::vector<std::string> &sources,
                                         std::vector<bool> *renamed = nullptr);

/* ---- exact replicas of the engine's own naming rules (audit 2026-08-22,
 * A2-2 relaxed / A2-15 empty names / V2.6 hive clash). Each mirrors the
 * fetched DuckDB v1.5.3 source line by line so parqit can PREDICT the scan
 * names a read produces and map them back to the true Parquet leaf names;
 * pinned by unit tests. -------------------------------------------------- */

/* DUCKDB-DEDUP-1: the Parquet reader renames case-insensitive duplicate leaf
 * names inside one file (extension/parquet/parquet_reader.cpp,
 * ParseSchemaRecursive): a running per-name ASCII-case-insensitive counter; a
 * clashing name gains "_<counter>" repeatedly until it is new
 * (`dup, dup, DUP` -> `dup, dup_1, DUP_2`; `a, a_1, a_1_1, A` -> `A_1_1_1`).
 * Empty names are left empty (the binder later calls them C<index>). */
std::vector<std::string> duckdb_reader_dedup(const std::vector<std::string> &leaves);

/* DUCKDB-UNION-1: read_parquet(..., union_by_name=true) combines the files'
 * (already deduped) column names in file order, case-insensitively
 * (UnionByName::CombineUnionTypes): the first file's names, then every later
 * file's name with no case-insensitive match so far. `names` are the scan
 * names; `owner[i]` is (file, column) of the FIRST contributor of union
 * column i; `member_of[f][c]` is the union column file f's column c flows
 * into. */
struct UnionByNamePlan {
    std::vector<std::string> names;
    std::vector<std::pair<size_t, size_t>> owner;
    std::vector<std::vector<size_t>> member_of;
};
UnionByNamePlan duckdb_union_by_name(const std::vector<std::vector<std::string>> &per_file);

/* DUCKDB-HIVE-1: the Hive partition keys DuckDB parses from a file path
 * (src/common/hive_partitioning.cpp, HivePartitioning::Parse): every directory
 * component (separated by '/' or '\\') holding exactly one '=' yields the key
 * before it, in path order; a component with two '=' or a '?'/newline is not a
 * partition; the file's own basename never is. */
std::vector<std::string> duckdb_hive_keys(const std::string &path);

/* The binder's name for a scan column whose name is empty: "C" + 0-based
 * column index (src/planner/binder/tableref/bind_table_function.cpp). */
std::string duckdb_empty_column_name(size_t index_0based);

} // namespace parqit
