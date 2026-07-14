/* parqit — plain I/O subcommands + the machinery shared with the lazy view.
 *
 * Charter disciplines wired in structurally:
 *  - §6.1/6.2: one column manifest travels from plan to fill; the engine is
 *    keyed by source names; the SELECT's column order IS the manifest order
 *    IS the plugin-call varlist order, and use_fetch/save_data cross-check
 *    SF_nvars() and per-position string-ness before touching any cell.
 *  - §6.3/6.5: storage classes come from format *prefixes*; period formats
 *    are written as INTEGER (BIGINT for %tC); display tokens never change
 *    storage.
 *  - §6.6/6.11: integer/string sizing via an observed-range pass; DECIMAL →
 *    double; unrepresentable types dropped-with-message.
 *  - §6.8: every failure path returns a nonzero ST_retcode plus SF_error
 *    text; written parquet is verified before success is reported.
 *  - §6.9: fetch fills a tempframe the ado swaps in only on success.
 */
#include "plugin/plugin_io.hpp"

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include "abi.h"
#include "duckdb.h"

#include "engine/hexcodec.hpp"
#include "engine/sanitize.hpp"

namespace parqit_plugin {

using parqit::ColumnPlan;
using parqit::ColumnStats;
using parqit::FmtClass;
using parqit::json;
using parqit::quote_ident;
using parqit::quote_literal;
using parqit::Session;
using parqit::StType;
using parqit::Transfer;

namespace {

constexpr ST_retcode kRcUsage = 198;
/* Stata's "no room to add more observations" — the SPI obs index (ST_int)
 * is a 32-bit int, so one dataset can address at most 2^31-1 rows */
constexpr ST_retcode kRcNoRoomObs = 901;
constexpr long long kSpiMaxObs = 2147483647LL;
constexpr ST_retcode kRcVarNotFound = 111;
constexpr ST_retcode kRcMismatch = 459;
constexpr ST_retcode kRcFileExists = 602;
constexpr ST_retcode kRcEngine = 920;

void cry(const std::string &s) {
    std::string line = s;
    line.push_back('\n');
    SF_error(const_cast<char *>(line.c_str()));
}
void save_local(const char *name, const std::string &v) {
    SF_macro_save(const_cast<char *>(name), const_cast<char *>(v.c_str()));
}

bool regular_file_fingerprint(const std::string &path, std::string *abs,
                              std::string *size, std::string *mtime) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path p(path);
    fs::path ap = fs::absolute(p, ec);
    if (ec) return false;
    if (!fs::is_regular_file(ap, ec) || ec) return false;
    auto sz = fs::file_size(ap, ec);
    if (ec) return false;
    auto ft = fs::last_write_time(ap, ec);
    if (ec) return false;
    *abs = ap.string();
    *size = std::to_string(static_cast<unsigned long long>(sz));
    /* file_time_type's rep is implementation-defined; cast to a concrete type
     * so std::to_string is unambiguous on libc++ (macOS) as well as libstdc++. */
    *mtime = std::to_string(
        static_cast<long long>(ft.time_since_epoch().count()));
    return true;
}

/* Every output transaction owns two filesystem objects and only those two:
 *
 *   - <dest>.parqit_lock is created atomically and never removed unless this
 *     process created it.  A pre-existing file/directory therefore blocks the
 *     save loudly instead of being mistaken for package-owned scratch state.
 *   - a 128-bit-random sibling directory contains the staged output and, when
 *     replace is requested, the old target while the swap is in progress.
 *
 * Keeping both objects beside the destination guarantees same-filesystem
 * renames.  The unique directory is the ownership proof: cleanup never touches
 * the historical predictable .parqit_tmp/.parqit_old names (REL-001). */
struct OutputTransaction {
    std::filesystem::path root;
    std::filesystem::path lock;
    bool owns_lock = false;
    bool retain_root = false;

    OutputTransaction() = default;
    OutputTransaction(const OutputTransaction &) = delete;
    OutputTransaction &operator=(const OutputTransaction &) = delete;
    ~OutputTransaction() {
        std::error_code ec;
        /* If publication and rollback both fail, the old payload may live only
         * under root/old.  That root becomes a recovery object whose exact
         * path is reported to the caller; deleting it here would convert a
         * loud publication failure into loss of the previously valid data. */
        if (!retain_root && !root.empty()) std::filesystem::remove_all(root, ec);
        if (owns_lock && !lock.empty()) {
            ec.clear();
            /* remove(), deliberately not remove_all(): an unexpected entrant
             * cannot make us recursively delete bytes we did not create. */
            std::filesystem::remove(lock, ec);
        }
    }
};

std::atomic<unsigned long long> g_output_counter{0};

long long output_process_id() {
#ifdef _WIN32
    return static_cast<long long>(_getpid());
#else
    return static_cast<long long>(getpid());
#endif
}

std::string output_nonce() {
    std::random_device rd;
    std::ostringstream os;
    os << std::hex << std::setfill('0');
    for (int i = 0; i < 4; i++) os << std::setw(8) << rd();
    return os.str();
}

bool output_test_hook(const char *name) {
    const char *value = std::getenv(name);
    return value && std::strcmp(value, "1") == 0;
}

bool reserve_output_transaction(const std::string &dest, OutputTransaction *tx,
                                std::string *err) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path target = fs::absolute(fs::u8path(dest), ec);
    if (ec) {
        *err = "could not resolve output path " + dest + ": " + ec.message();
        return false;
    }
    fs::path parent = target.parent_path();
    if (!fs::is_directory(parent, ec) || ec) {
        *err = "output directory does not exist or is not a directory: " +
               parent.u8string();
        return false;
    }

    tx->lock = fs::u8path(target.u8string() + ".parqit_lock");
    ec.clear();
    if (!fs::create_directory(tx->lock, ec)) {
        if (ec) {
            *err = "could not reserve output lock " + tx->lock.u8string() +
                   ": " + ec.message();
        } else {
            *err = "output " + dest +
                   " is already being written, or its package lock path " +
                   tx->lock.u8string() + " already exists; parqit will not "
                   "remove an object it did not create";
        }
        return false;
    }
    tx->owns_lock = true;

    try {
        for (int attempt = 0; attempt < 128; attempt++) {
            const unsigned long long seq = ++g_output_counter;
            const std::string leaf = target.filename().u8string() +
                                     ".parqit_txn_" +
                                     std::to_string(output_process_id()) + "_" +
                                     std::to_string(seq) + "_" + output_nonce();
            tx->root = parent / fs::u8path(leaf);
            ec.clear();
            if (fs::create_directory(tx->root, ec)) {
                /* Deterministic test-only contention hook.  It is inert unless
                 * explicitly set, bounded to 30 seconds, and holds the real
                 * exclusive lock so the two-process regression exercises the
                 * production ownership path rather than a mock. */
                if (const char *hold =
                        std::getenv("PARQIT_TEST_HOLD_OUTPUT_LOCK_MS")) {
                    char *end = nullptr;
                    long ms = std::strtol(hold, &end, 10);
                    if (end != hold && *end == '\0' && ms > 0 && ms <= 30000)
                        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                }
                return true;
            }
            if (ec) {
                *err = "could not reserve package-owned output staging: " +
                       ec.message();
                tx->root.clear();
                return false;
            }
        }
    } catch (const std::exception &e) {
        *err = std::string("could not reserve package-owned output staging: ") +
               e.what();
        tx->root.clear();
        return false;
    }
    *err = "could not reserve unique output staging after 128 attempts";
    tx->root.clear();
    return false;
}

} // namespace

/* ---------------------------------------------------------------- sources */

static std::string glob_escape(const std::string &p) {
    std::string out;
    out.reserve(p.size());
    for (char c : p) {
        if (c == '*') out += "[*]";
        else if (c == '?') out += "[?]";
        else if (c == '[') out += "[[]";
        else out.push_back(c);
    }
    return out;
}

/* GLOB-2: user-facing wildcards are `*` and `?` ONLY. A `[` is always a
 * literal byte of the filename: bracket names are common (browser download
 * copies like `data[1].parquet`), bracket character-classes in a Stata
 * filename are not — and an unescaped class silently read a DIFFERENT file
 * (`data[1].parquet` matched `data1.parquet`, rc 0, wrong data). */
static std::string glob_escape_brackets(const std::string &p) {
    std::string out;
    out.reserve(p.size());
    for (char c : p) {
        if (c == '[') out += "[[]";
        else out.push_back(c);
    }
    return out;
}

Source source_for(const std::vector<std::string> &files, bool relaxed,
                  bool csv) {
    std::string list;
    bool any_dir = false;
    for (size_t i = 0; i < files.size(); i++) {
        std::string f = files[i];
        std::error_code ec;
        if (!csv && std::filesystem::is_directory(f, ec)) {
            any_dir = true;
            /* the directory name itself may contain glob metacharacters */
            f = glob_escape(f);
            if (!f.empty() && f.back() != '/' && f.back() != '\\') f += "/";
            f += "**/*.parquet";
        } else if (std::filesystem::exists(f, ec)) {
            /* a path that names an existing file is that file, never a
             * pattern — every metacharacter is literal (GLOB-2) */
            f = glob_escape(f);
        } else {
            /* pattern: `*`/`?` stay live, `[` is literal (GLOB-2) */
            f = glob_escape_brackets(f);
        }
        if (i) list += ", ";
        list += quote_literal(f);
    }
    Source s;
    const std::string paths = "[" + list + "]";
    std::string opts;
    if (any_dir) opts += ", hive_partitioning = true";
    /* relaxed: union the files by column name (missing -> NULL) instead of
     * requiring one schema. The metadata-sizing fast path (F2) stays correct
     * because a column absent from some files arrives as NULL under
     * union_by_name, and a NULL cannot widen the min/max range — so footer-stat
     * sizing is right even for a partially-present column. (NOT, as an earlier
     * comment claimed, because count(stats) < count(*): parquet_metadata only
     * emits rows for columns that exist in a file, so that guard would pass. An
     * all-null row group is still caught and falls back to a scan.) */
    if (relaxed) opts += ", union_by_name = true";
    if (csv) {
        /* delimited text: auto-detect schema/delimiter. No Parquet footer, so
         * leave paths_sql "[]" — every parquet_* metadata probe is skipped and
         * columns size from the data scan (correct for CSV). */
        s.paths_sql = "[]";
        s.scan_sql = "read_csv_auto(" + paths + opts + ")";
    } else {
        s.paths_sql = paths;
        s.scan_sql = "read_parquet(" + paths + opts + ")";
    }
    return s;
}

/* SCH1/SCH2: without `relaxed`, DuckDB's plain read_parquet takes the FIRST
 * file's schema and casts every later file to it — a same-named column that
 * widened in a later file (int → double, date → timestamp: the canonical
 * schema-evolution layout) was silently down-cast, destroying data with rc 0
 * and glob-order-dependent results, while the help promised a loud error;
 * a column extra in a later file silently vanished the same way. The gate:
 * one footer-only fingerprint query over parquet_schema; when fingerprints
 * differ, physically-different-but-identically-resolving files (INT96 vs
 * TIMESTAMP timestamps, converted- vs logical-type annotations) are rescued
 * by DESCRIBE-ing one representative file per fingerprint — only a real
 * difference in the resolved schema (column set or type, compared
 * case-insensitively, order-insensitively, exactly as read_parquet binds)
 * refuses, naming the column, both types and both files. */
ST_retcode strict_schema_gate(Session &s, const Source &src,
                              const std::vector<std::string> &files,
                              bool relaxed, bool csv, std::string *err) {
    if (csv || src.paths_sql == "[]" || src.paths_sql.empty()) return 0;

    /* NM1: a NUL byte inside a parquet column name is invisible to the SPI's
     * C-string name APIs — duckdb_column_name truncates "col\0hidden" to
     * "col", it collides with a real sibling "col", and the rebuilt fetch
     * SELECT binds the same physical column twice: one column's data was
     * silently LOST and another's duplicated, rc 0. The name cannot be
     * carried faithfully anywhere downstream, so refuse the file loudly
     * (relaxed included; a single file bites exactly the same way). DuckDB's
     * own VARCHARs are length-counted, so the footer query sees the NUL. */
    {
        duckdb_result nres;
        if (!s.query("SELECT replace(name, chr(0), '<NUL>') FROM "
                     "parquet_schema(" + src.paths_sql +
                         ") WHERE contains(name, chr(0)) LIMIT 1",
                     &nres, err))
            return kRcEngine;
        if (duckdb_row_count(&nres) > 0) {
            char *nm = duckdb_value_varchar(&nres, 0, 0);
            std::string shown = nm ? nm : "";
            if (nm) duckdb_free(nm);
            duckdb_destroy_result(&nres);
            *err = "column name \"" + shown +
                   "\" contains a NUL byte, which Stata's plugin interface "
                   "cannot carry (it would silently collide with a sibling "
                   "column and lose data); rename the column upstream";
            return kRcUsage;
        }
        duckdb_destroy_result(&nres);
    }

    if (relaxed) return 0;
    if (files.size() == 1 && files[0].find('*') == std::string::npos &&
        files[0].find('?') == std::string::npos) {
        /* one literal file has no second schema to disagree with */
        std::error_code ec;
        if (!std::filesystem::is_directory(files[0], ec)) return 0;
    }

    duckdb_result res;
    if (!s.query("SELECT file_name, string_agg(lower(name) || ':' || type || "
                 "':' || coalesce(converted_type, '') || ':' || "
                 "coalesce(logical_type, '') || ':' || coalesce(precision, -1) "
                 "|| ':' || coalesce(scale, -1), '|' ORDER BY lower(name), "
                 "name) FROM parquet_schema(" + src.paths_sql +
                     ") WHERE num_children IS NULL OR num_children = 0 "
                     "GROUP BY file_name",
                 &res, err))
        return kRcEngine;
    std::map<std::string, std::string> rep_of; /* fingerprint -> first file */
    idx_t n = duckdb_row_count(&res);
    for (idx_t r = 0; r < n; r++) {
        char *f = duckdb_value_varchar(&res, 0, r);
        char *fp = duckdb_value_varchar(&res, 1, r);
        if (f && fp && !rep_of.count(fp)) rep_of[fp] = f;
        if (f) duckdb_free(f);
        if (fp) duckdb_free(fp);
    }
    duckdb_destroy_result(&res);
    if (rep_of.size() <= 1) return 0;

    struct Rep {
        std::string file;
        std::vector<std::pair<std::string, std::string>> cols; /* lower, type */
        std::map<std::string, std::string> by_name;
        std::map<std::string, std::string> display; /* lower -> as written */
    };
    std::vector<Rep> reps;
    for (const auto &g : rep_of) {
        Rep rep;
        rep.file = g.second;
        duckdb_result d;
        if (!s.query("DESCRIBE SELECT * FROM read_parquet(" +
                         quote_literal(rep.file) + ")",
                     &d, err))
            return kRcEngine;
        idx_t dn = duckdb_row_count(&d);
        for (idx_t r = 0; r < dn; r++) {
            char *nm = duckdb_value_varchar(&d, 0, r);
            char *ty = duckdb_value_varchar(&d, 1, r);
            if (nm && ty) {
                std::string low = nm;
                std::transform(low.begin(), low.end(), low.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                rep.cols.emplace_back(low, ty);
                rep.by_name[low] = ty;
                rep.display[low] = nm;
            }
            if (nm) duckdb_free(nm);
            if (ty) duckdb_free(ty);
        }
        duckdb_destroy_result(&d);
        reps.push_back(std::move(rep));
    }
    const Rep &a = reps[0];
    for (size_t i = 1; i < reps.size(); i++) {
        const Rep &b = reps[i];
        for (const auto &ca : a.cols) {
            auto it = b.by_name.find(ca.first);
            if (it == b.by_name.end()) {
                *err = "the matched files do not share one schema: column \"" +
                       a.display.at(ca.first) + "\" (" + a.file +
                       ") is missing from " + b.file +
                       "; pass relaxed to union by name (an absent column "
                       "null-fills)";
                return kRcUsage;
            }
            if (it->second != ca.second) {
                *err = "the matched files do not share one schema: column \"" +
                       a.display.at(ca.first) + "\" is " + ca.second + " in " +
                       a.file + " but " + it->second + " in " + b.file +
                       "; strict mode refuses to guess — pass relaxed to "
                       "union by name with safe recasts";
                return kRcUsage;
            }
        }
        for (const auto &cb : b.cols) {
            if (!a.by_name.count(cb.first)) {
                *err = "the matched files do not share one schema: column \"" +
                       b.display.at(cb.first) + "\" (" + b.file +
                       ") is missing from " + a.file +
                       "; pass relaxed to union by name (an absent column "
                       "null-fills)";
                return kRcUsage;
            }
        }
    }
    return 0; /* resolved schemas agree — the difference was physical only */
}

/* ------------------------------------------------------- parqit KV metadata */

namespace {

/* Best-effort: restore only when every matched file carries an identical
 * parqit.schema (a heterogeneous glob keeps plain types, loudly). */
ParqitMeta read_parqit_meta(Session &s, const std::string &paths_sql,
                        std::vector<std::string> *warnings) {
    ParqitMeta m;
    if (paths_sql == "[]" || paths_sql.empty()) return m; /* view results */
    duckdb_result res;
    std::string err;
    /* META-010: establish the complete matched-file universe independently of
     * the keys being validated.  A file with no parqit.* rows must participate
     * in the equality check instead of disappearing from it. */
    std::set<std::string> all_files;
    if (!s.query("SELECT DISTINCT file_name FROM parquet_file_metadata(" +
                     paths_sql + ") ORDER BY file_name",
                 &res, &err))
        return m;
    for (idx_t r = 0; r < duckdb_row_count(&res); r++) {
        char *f = duckdb_value_varchar(&res, 0, r);
        if (f) all_files.insert(f);
        if (f) duckdb_free(f);
    }
    duckdb_destroy_result(&res);
    if (all_files.empty()) return m;
    /* META-B: try(decode()) — a strict decode() THROWS on any invalid-UTF8
     * key/value anywhere in the file's KV metadata, and the whole query
     * failure was swallowed as "no metadata channel", silently dropping every
     * label/format because a third-party writer added one binary sidecar key.
     * try() nulls just the undecodable entry; the parqit.* keys (valid UTF-8
     * by construction) survive. */
    if (!s.query("SELECT file_name, try(decode(key)) AS k, "
                 "try(decode(value)) AS v FROM "
                 "parquet_kv_metadata(" + paths_sql + ") "
                 "WHERE try(decode(key)) LIKE 'parqit.%'",
                 &res, &err))
        return m; /* no metadata channel — fine */
    std::map<std::string, std::map<std::string, std::string>> per_file;
    for (const auto &f : all_files) per_file[f] = {};
    idx_t nrow = duckdb_row_count(&res);
    for (idx_t r = 0; r < nrow; r++) {
        char *f = duckdb_value_varchar(&res, 0, r);
        char *k = duckdb_value_varchar(&res, 1, r);
        char *v = duckdb_value_varchar(&res, 2, r);
        if (f && k && v) per_file[f][k] = v;
        if (f) duckdb_free(f);
        if (k) duckdb_free(k);
        if (v) duckdb_free(v);
    }
    duckdb_destroy_result(&res);
    const auto &first = per_file.begin()->second;
    for (const auto &pf : per_file) {
        if (pf.second != first) {
            warnings->push_back("parqit metadata differs across matched files; "
                                "labels/formats not restored");
            return m;
        }
    }
    bool malformed = false;
    auto get = [&](const char *key) -> json {
        auto it = first.find(key);
        if (it == first.end()) return json();
        json j = json::parse(it->second, nullptr, false);
        if (j.is_discarded()) {
            warnings->push_back(std::string("malformed JSON in ") + key +
                                "; all parqit metadata skipped");
            malformed = true;
            return json();
        }
        return j;
    };
    json schema = get("parqit.schema");
    json vallabs = get("parqit.vallabs");
    json chars = get("parqit.chars");
    json dl = get("parqit.dtalabel");
    if (malformed) return ParqitMeta();
    if (!schema.is_null() && !schema.is_object()) {
        warnings->push_back("parqit.schema is not a JSON object; all parqit "
                            "metadata skipped");
        return ParqitMeta();
    }
    if (!vallabs.is_null() && !vallabs.is_object()) {
        warnings->push_back("parqit.vallabs is not a JSON object; all parqit "
                            "metadata skipped");
        return ParqitMeta();
    }
    if (!chars.is_null() && !chars.is_object()) {
        warnings->push_back("parqit.chars is not a JSON object; all parqit "
                            "metadata skipped");
        return ParqitMeta();
    }
    if (!dl.is_null() && !dl.is_string()) {
        warnings->push_back("parqit.dtalabel is not a JSON string; all parqit "
                            "metadata skipped");
        return ParqitMeta();
    }
    m.schema = std::move(schema);
    m.vallabs = vallabs.is_null() ? json::object() : std::move(vallabs);
    m.chars = chars.is_null() ? json::object() : std::move(chars);
    if (dl.is_string()) m.dtalabel = dl.get<std::string>();
    if (m.schema.contains("sortedby")) {
        if (!m.schema["sortedby"].is_array()) {
            warnings->push_back("parqit.schema sortedby is not an array; all "
                                "parqit metadata skipped");
            return ParqitMeta();
        }
        for (const auto &v : m.schema["sortedby"]) {
            if (!v.is_string()) {
                warnings->push_back("parqit.schema sortedby contains a non-string; "
                                    "all parqit metadata skipped");
                return ParqitMeta();
            }
            m.sortedby.push_back(v.get<std::string>());
        }
    }
    m.present = !m.schema.is_null();
    return m;
}

} // namespace

/* ------------------------------------------------------------- planning */

ST_retcode plan_columns(Session &s, const Source &src,
                        const std::vector<std::string> &varlist, bool with_stats,
                        PlanContext *ctx, std::string *err, bool need_count) {
    duckdb_result res;
    if (!s.query("SELECT * FROM " + src.scan_sql + " LIMIT 0", &res, err)) {
        return kRcEngine;
    }
    idx_t ncol = duckdb_column_count(&res);
    std::vector<std::string> src_names(ncol);
    std::vector<ColumnPlan> plans;
    plans.reserve(ncol);
    for (idx_t c = 0; c < ncol; c++) {
        const char *nm = duckdb_column_name(&res, c);
        src_names[c] = nm ? nm : "";
        duckdb_logical_type lt = duckdb_column_logical_type(&res, c);
        plans.push_back(parqit::plan_read_column(src_names[c], lt));
        duckdb_destroy_logical_type(&lt);
    }
    duckdb_destroy_result(&res);

    /* duplicate column names: read_parquet has already renamed them (dup,
     * dup_1, …). Recover the true parquet names positionally from
     * parquet_schema so the rename is loud and reversible (charter §6.10).
     * N2/SCH5: the positional leaf-vs-scan alignment misfires when nested
     * columns expose child leaves ("element", struct fields) or a Hive glob's
     * per-file leaf rows coincide with the scan width — stamping bogus
     * warnings and src_name onto unrelated columns. A genuine DuckDB dedup
     * rename is always `<leaf>` -> `<leaf>_<digits>`, so recover ONLY pairs
     * matching that shape and skip any other positional mismatch. */
    if (src.paths_sql != "[]" && !src.paths_sql.empty()) {
        auto is_dedup_of = [](const std::string &scan, const std::string &leaf) {
            if (scan.size() <= leaf.size() + 1) return false;
            if (scan.compare(0, leaf.size(), leaf) != 0) return false;
            if (scan[leaf.size()] != '_') return false;
            for (size_t i = leaf.size() + 1; i < scan.size(); i++)
                if (scan[i] < '0' || scan[i] > '9') return false;
            return true;
        };
        duckdb_result sres;
        std::string serr;
        if (s.query("SELECT name FROM parquet_schema(" + src.paths_sql +
                        ") WHERE num_children = 0 OR num_children IS NULL",
                    &sres, &serr)) {
            idx_t n = duckdb_row_count(&sres);
            if (n == ncol) {
                for (idx_t c = 0; c < ncol; c++) {
                    char *nm = duckdb_value_varchar(&sres, 0, c);
                    if (nm && plans[c].source_name != nm &&
                        is_dedup_of(plans[c].source_name, nm)) {
                        ctx->warnings.push_back(
                            "duplicate column name \"" + std::string(nm) +
                            "\" in the file; loaded as " + plans[c].source_name);
                        ctx->parquet_names[plans[c].source_name] = nm;
                    }
                    if (nm) duckdb_free(nm);
                }
            }
            duckdb_destroy_result(&sres);
        }
    }

    /* sanitised Stata names, deterministic, collision-free */
    std::vector<bool> renamed;
    std::vector<std::string> stata_names = parqit::sanitize_unique(src_names, &renamed);
    for (size_t i = 0; i < plans.size(); i++) {
        plans[i].stata_name = stata_names[i];
        if (renamed[i] && !plans[i].dropped)
            ctx->warnings.push_back("column \"" + src_names[i] + "\" loaded as " +
                                    stata_names[i] +
                                    " (original name kept in char varname[src_name])");
    }

    /* parqit.* metadata: original types/formats/labels ride along (period
     * formats stay integers with their true format — charter §6.3) */
    ctx->meta = read_parqit_meta(s, src.paths_sql, &ctx->warnings);
    if (ctx->meta.present && ctx->meta.schema.contains("vars") &&
        ctx->meta.schema["vars"].is_array()) {
        std::map<std::string, const json *> by_src;
        for (const auto &v : ctx->meta.schema["vars"].items()) {
            const json &jv = v.value();
            if (jv.contains("src") && jv["src"].is_string())
                by_src[jv["src"].get<std::string>()] = &jv;
        }
        auto sget = [](const json &j, const char *k) -> std::string {
            return (j.contains(k) && j[k].is_string()) ? j[k].get<std::string>()
                                                       : std::string();
        };
        for (auto &p : plans) {
            std::string meta_name = p.source_name;
            auto pn = ctx->parquet_names.find(p.source_name);
            if (pn != ctx->parquet_names.end()) meta_name = pn->second;
            auto it = by_src.find(meta_name);
            if (it == by_src.end()) continue;
            const json &jv = *it->second;
            std::string f = sget(jv, "fmt");
            if (!f.empty()) p.stata_format = f;
            p.meta_type = sget(jv, "type");
            p.varlab = sget(jv, "varlab");
            p.vallab = sget(jv, "vallab");
        }
    }

    /* varlist selection: named columns, named order (charter §6.1) */
    if (!varlist.empty()) {
        std::map<std::string, size_t> index;
        for (size_t i = 0; i < plans.size(); i++) index[plans[i].stata_name] = i;
        std::vector<ColumnPlan> picked;
        for (const auto &want : varlist) {
            auto it = index.find(want);
            if (it == index.end()) {
                *err = "variable " + want + " not found in the file(s)";
                return kRcVarNotFound;
            }
            picked.push_back(plans[it->second]);
        }
        plans.swap(picked);
    }

    /* split drops out, loudly */
    for (auto &p : plans) {
        if (p.dropped)
            ctx->drops.emplace_back(p.source_name, p.drop_reason);
        else
            ctx->active.push_back(p);
    }
    if (ctx->active.empty()) {
        *err = "no loadable columns (every column was dropped: unsupported types)";
        return kRcUsage;
    }

    /* observed-range pass: sizes integers and strings exactly. Parquet
     * carries exact per-row-group min/max in its footer, so the integer/
     * float bounds can come from metadata for FREE — a column only needs a
     * real data scan when metadata cannot answer exactly:
     *   - strings: parquet stores min/max VALUES, not max byte length;
     *   - >2^53 detection: needs the true magnitude (double would round);
     *   - floats whose metadata bound exceeds Stata's float range: must
     *     distinguish a finite overflow (widen) from a mere ±Inf (keep);
     *   - duplicate column names / files lacking stats: fall back to a scan.
     * Bounds taken from metadata are EXACT for the columns that use them:
     * needs_minmax columns are all ≤32-bit (64-bit ints carry needs_big53
     * and are excluded), and float values are a subset of double. */
    if (with_stats) {
        std::vector<ColumnStats> stats(ctx->active.size());

        /* metadata stats keyed by source (parquet) name; only trusted when
         * every row group of the column carries a non-null min AND max */
        struct MetaStat { bool has = false; double mn = 0, mx = 0; };
        std::map<std::string, MetaStat> meta_stats;
        /* only worth a footer read if some column could actually be sized from
         * it — a column that already needs a strlen/>2^53 scan cannot, so an
         * all-string file pays nothing extra (never increases read time) */
        bool any_meta_candidate = false;
        for (const auto &p : ctx->active)
            any_meta_candidate |= (p.needs_minmax || p.needs_float_range) &&
                                  !p.needs_strlen && !p.needs_big53;
        const bool dup_free = ctx->parquet_names.empty();
        if (any_meta_candidate && dup_free && src.paths_sql != "[]" &&
            !src.paths_sql.empty()) {
            duckdb_result mres;
            std::string merr;
            if (s.query("SELECT path_in_schema, "
                        "min(TRY_CAST(stats_min_value AS DOUBLE)), "
                        "max(TRY_CAST(stats_max_value AS DOUBLE)), "
                        "count(*), count(stats_min_value), count(stats_max_value) "
                        "FROM parquet_metadata(" + src.paths_sql +
                            ") GROUP BY path_in_schema",
                        &mres, &merr)) {
                idx_t mn = duckdb_row_count(&mres);
                for (idx_t r = 0; r < mn; r++) {
                    char *nm = duckdb_value_varchar(&mres, 0, r);
                    if (nm) {
                        long long ng = duckdb_value_int64(&mres, 3, r);
                        long long ngmin = duckdb_value_int64(&mres, 4, r);
                        long long ngmax = duckdb_value_int64(&mres, 5, r);
                        MetaStat ms;
                        ms.has = ng > 0 && ngmin == ng && ngmax == ng &&
                                 !duckdb_value_is_null(&mres, 1, r) &&
                                 !duckdb_value_is_null(&mres, 2, r);
                        if (ms.has) {
                            ms.mn = duckdb_value_double(&mres, 1, r);
                            ms.mx = duckdb_value_double(&mres, 2, r);
                        }
                        meta_stats[nm] = ms;
                        duckdb_free(nm);
                    }
                }
                duckdb_destroy_result(&mres);
            }
        }

        std::string sel;
        std::vector<std::pair<size_t, char>> slots;
        auto add = [&](const std::string &expr, size_t pi, char kind) {
            if (!sel.empty()) sel += ", ";
            sel += expr;
            slots.emplace_back(pi, kind);
        };
        for (size_t i = 0; i < ctx->active.size(); i++) {
            ColumnPlan &p = ctx->active[i];
            const std::string ref = quote_ident(p.source_name);
            /* try metadata first, but never for a column that also needs a
             * strlen or >2^53 scan (those force a scan anyway, and the
             * metadata bound for a >2^53 column would be a rounded double) */
            bool from_meta = false;
            if ((p.needs_minmax || p.needs_float_range) && !p.needs_strlen &&
                !p.needs_big53) {
                auto it = meta_stats.find(p.source_name);
                if (it != meta_stats.end() && it->second.has) {
                    if (p.needs_minmax) {
                        stats[i].has_minmax = true;
                        stats[i].min = it->second.mn;
                        stats[i].max = it->second.mx;
                        from_meta = true;
                    } else if (std::fabs(it->second.mn) <= parqit::kStataFloatMax &&
                               std::fabs(it->second.mx) <= parqit::kStataFloatMax) {
                        /* metadata proves every value fits Stata's float */
                        stats[i].has_minmax = true;
                        stats[i].min = it->second.mn;
                        stats[i].max = it->second.mx;
                        from_meta = true;
                    }
                }
            }
            if (from_meta) continue;
            if (p.needs_minmax) {
                add("min(" + ref + ")::DOUBLE", i, 'm');
                add("max(" + ref + ")::DOUBLE", i, 'x');
            }
            if (p.needs_float_range) {
                /* NaN sorts above everything in the engine and Inf is not a
                 * range: only finite values decide float-vs-double storage */
                add("(min(" + ref + ") FILTER (WHERE isfinite(" + ref +
                        ")))::DOUBLE", i, 'm');
                add("(max(" + ref + ") FILTER (WHERE isfinite(" + ref +
                        ")))::DOUBLE", i, 'x');
            }
            if (p.needs_strlen) {
                /* strlen = BYTES in DuckDB (length = chars). Size over the
                 * CASTED projection, not the raw column: UUID/ENUM have no
                 * strlen on their native type (it is a binder error), so the
                 * eager `use` path must size them exactly as the fetch SELECT
                 * and the lazy collect path do — otherwise the two materialisers
                 * disagree on the same file. */
                const std::string sref = p.cast_sql.empty() ? ref : p.cast_sql;
                add("coalesce(max(strlen(" + sref + ")), 0)::BIGINT", i, 'l');
            }
            if (p.needs_big53)
                add("coalesce(max(abs(" + ref +
                        "::HUGEINT)) > 9007199254740992::HUGEINT, false)",
                    i, 'b');
        }
        if (!sel.empty()) {
            duckdb_result sres;
            if (!s.query("SELECT " + sel + " FROM " + src.scan_sql, &sres, err))
                return kRcEngine;
            for (size_t k = 0; k < slots.size(); k++) {
                size_t pi = slots[k].first;
                bool isnull = duckdb_value_is_null(&sres, k, 0);
                switch (slots[k].second) {
                case 'm':
                    if (!isnull) {
                        stats[pi].has_minmax = true;
                        stats[pi].min = duckdb_value_double(&sres, k, 0);
                    }
                    break;
                case 'x':
                    if (!isnull) stats[pi].max = duckdb_value_double(&sres, k, 0);
                    break;
                case 'l':
                    stats[pi].max_strlen = isnull ? 0 : duckdb_value_int64(&sres, k, 0);
                    break;
                case 'b':
                    stats[pi].any_beyond_2p53 =
                        !isnull && duckdb_value_boolean(&sres, k, 0);
                    break;
                }
            }
            duckdb_destroy_result(&sres);
        }
        for (size_t i = 0; i < ctx->active.size(); i++) {
            parqit::refine_plan(ctx->active[i], stats[i]);
            /* the saved Stata type round-trips (§4), widened if needed */
            parqit::apply_meta_type(ctx->active[i]);
        }
    } else {
        /* no range pass (describe): the saved type is the honest display;
         * foreign columns keep the conservative default mapping */
        for (auto &p : ctx->active) {
            if (p.meta_type.empty()) continue;
            parqit::StType mt;
            int mb = 0;
            if (parqit::sttype_parse(p.meta_type, &mt, &mb)) {
                p.stata_type = mt;
                p.str_bytes = mb;
            }
        }
    }

    /* Row count: exact (parquet footer makes it cheap). Skipped for pure
     * metadata/schema probes that never read ctx->nrows, so a CSV/non-parquet
     * lazy open or using-side metadata read no longer forces a full scan
     * (PERF-3). */
    ctx->nrows = 0;
    if (need_count) {
        std::string nstr;
        if (!s.query_scalar("SELECT count(*) FROM " + src.scan_sql, &nstr, err))
            return kRcEngine;
        ctx->nrows = std::strtoll(nstr.c_str(), nullptr, 10);
    }
    return 0;
}

void write_var_records(parqit::ResponseWriter &w, const PlanContext &ctx) {
    for (size_t i = 0; i < ctx.active.size(); i++) {
        const ColumnPlan &p = ctx.active[i];
        std::string original = p.source_name;
        auto pn = ctx.parquet_names.find(p.source_name);
        if (pn != ctx.parquet_names.end()) original = pn->second;
        w.rec("var", {std::to_string(i + 1)},
              {p.stata_name, original, parqit::sttype_code(p.stata_type, p.str_bytes),
               p.stata_format, p.varlab, p.vallab});
        /* NUM2: the per-column note is now emitted by cmd_use_fetch via
         * SF_error (survives `quietly`), not as a suppressible ado printf, so
         * it is no longer written as a response 'warn' record here. General
         * structural warnings (ctx.warnings, below) keep the record path. */
    }
    if (ctx.meta.present && ctx.meta.vallabs.is_object()) {
        /* stringify a JSON scalar without throwing on a foreign file that
         * stored a value-label key/text as a number instead of a string */
        auto js = [](const nlohmann::json &x) -> std::string {
            return x.is_string() ? x.get<std::string>() : x.dump();
        };
        for (const auto &lab : ctx.meta.vallabs.items()) {
            if (!lab.value().is_object() || !lab.value().contains("entries")) continue;
            for (const auto &e : lab.value()["entries"]) {
                if (!e.is_array() || e.size() != 2) continue;
                /* the value-label key travels HEX-ENCODED (a text field), like
                 * every other user-originated field, so a foreign/corrupt file
                 * whose key contains a '|' or newline can never shift the
                 * delimited protocol (INJ-1). The ado validates it is an
                 * integer before applying it. */
                w.rec("vlab", {}, {js(e[0]), lab.key(), js(e[1])});
            }
        }
    }
    if (ctx.meta.present && ctx.meta.chars.is_object()) {
        /* PARQIT-CHAR-01: emit a char/note only for _dta or a column that
         * survives in the result. A projection (subset use / contract /
         * collapse / keep / drop / reshape …) can drop the variable that
         * carried the char; an orphan record makes the ado's st_global abort
         * rc 3300 on a non-existent target. Mirror the save path's live-column
         * filter (plugin_view.cpp). */
        std::set<std::string> live;
        for (const auto &p : ctx.active) live.insert(p.stata_name);
        for (const auto &tgt : ctx.meta.chars.items()) {
            if (tgt.key() != "_dta" && !live.count(tgt.key())) continue;
            if (!tgt.value().is_object()) continue;
            for (const auto &c : tgt.value().items()) {
                if (c.value().is_string())
                    w.rec("char", {}, {tgt.key(), c.key(), c.value().get<std::string>()});
            }
        }
    }
    if (!ctx.meta.dtalabel.empty()) w.rec("dlabel", {}, {ctx.meta.dtalabel});
    if (!ctx.meta.sortedby.empty()) {
        std::string keys;
        for (const auto &saved : ctx.meta.sortedby) {
            bool found = false;
            for (const auto &p : ctx.active) {
                if (p.source_name == saved || p.stata_name == saved) {
                    if (!keys.empty()) keys += " ";
                    keys += p.stata_name;
                    found = true;
                    break;
                }
            }
            /* A subset that removes a later sorted key retains only the valid
             * prefix, exactly like Stata's sortedby marker. */
            if (!found) break;
        }
        if (!keys.empty()) w.rec("sortedby", {}, {keys});
    }
    for (const auto &d : ctx.drops)
        w.rec("drop", {}, {d.first, d.second});
    for (const auto &wmsg : ctx.warnings)
        w.rec("warn", {}, {wmsg});
}

/* --------------------------------------------------- prepared-read state */

namespace {

struct PreparedRead {
    bool live = false;
    std::string tag;
    std::string source_sql;
    std::string strl_path;
    bool drop_source_after = false; /* DROP TABLE <source_sql> when done */
    std::vector<ColumnPlan> plans;
    long long nrows = 0;
};
PreparedRead g_prepared;
long long g_tag_counter = 0;

} // namespace

/* Escape glob metacharacters in a LITERAL path so DuckDB read_parquet/read_csv
 * (which treat a quoted path as a glob) read exactly that file, not a sibling
 * matching the bracket class: '*'->'[*]', '?'->'[?]', '['->'[[]'. Verified
 * against the vendored DuckDB: read_parquet('out[[]9].parquet') reads the
 * literal 'out[9].parquet' (GLOB-1a/GLOB-1b). */
void set_prepared_read(const std::string &source_scan_sql,
                       std::vector<ColumnPlan> plans, long long nrows,
                       const std::string &strl_path, bool drop_source_after,
                       std::string *tag_out) {
    /* ATOM-3: a prior prepare that was never fetched (e.g. a collect that
     * aborted between prepare and fetch) leaves its spill temp table orphaned
     * for the session. Drop it before overwriting the prepared-read state, so
     * spill tables cannot accumulate. DROP ... IF EXISTS is idempotent, so a
     * table the fetch already dropped (the happy path) is harmless here. */
    if (g_prepared.live && g_prepared.drop_source_after &&
        !g_prepared.source_sql.empty()) {
        std::string derr;
        Session::instance().exec("DROP TABLE IF EXISTS " + g_prepared.source_sql,
                                 &derr);
    }
    g_prepared.live = true;
    g_prepared.tag = "t" + std::to_string(++g_tag_counter);
    g_prepared.source_sql = source_scan_sql;
    g_prepared.strl_path = strl_path;
    g_prepared.drop_source_after = drop_source_after;
    g_prepared.plans = std::move(plans);
    g_prepared.nrows = nrows;
    *tag_out = g_prepared.tag;
}

/* ----------------------------------------------- verified parquet writer */

ST_retcode copy_out_parquet(Session &s, const std::string &query_sql,
                            const std::string &dest, bool replace,
                            const std::string &compression, long long comp_level,
                            const std::vector<std::string> &partition_by,
                            long long row_group_size,
                            const std::string &kv_metadata_sql_fragment,
                            long long *written, std::string *err) {
    namespace fs = std::filesystem;
    static const std::set<std::string> kCodecs = {
        "snappy", "zstd", "gzip", "uncompressed", "lz4", "lz4_raw", "brotli"};
    if (!compression.empty() && !kCodecs.count(compression)) {
        *err = "unknown compression codec '" + compression +
               "' (snappy zstd gzip lz4 lz4_raw brotli uncompressed)";
        return kRcUsage;
    }
    std::error_code ec;
    const bool exists = fs::exists(dest, ec);
    if (partition_by.empty()) {
        if (exists && !replace) {
            *err = "file " + dest + " already exists; specify replace";
            return kRcFileExists;
        }
        if (exists && fs::is_directory(dest, ec)) {
            *err = dest + " is a directory";
            return kRcFileExists;
        }
    } else if (exists) {
        if (!replace) {
            *err = "partitioned target " + dest + " already exists; specify "
                   "replace, or remove it yourself";
            return kRcFileExists;
        }
        if (!fs::is_directory(dest, ec)) {
            *err = "partitioned target " + dest + " already exists as a file; "
                   "remove it yourself or write elsewhere";
            return kRcFileExists;
        }
        /* an existing partition-tree directory with replace is removed just
         * before the rename below, after the new tree is built and verified
         * (IO-3); parqit never removes a non-directory it might not own. */
    }

    std::string copts = "FORMAT PARQUET";
    if (!kv_metadata_sql_fragment.empty()) copts += ", " + kv_metadata_sql_fragment;
    if (!compression.empty()) copts += ", COMPRESSION " + quote_literal(compression);
    if (comp_level >= 0) copts += ", COMPRESSION_LEVEL " + std::to_string(comp_level);
    if (row_group_size > 0)
        copts += ", ROW_GROUP_SIZE " + std::to_string(row_group_size);

    auto run_copy = [&](const std::string &target,
                        const std::string &extra) -> bool {
        duckdb_result res;
        if (!s.query("COPY (" + query_sql + ") TO " + quote_literal(target) + " (" +
                         copts + extra + ")",
                     &res, err))
            return false;
        /* the engine reports the written row count */
        *written = -1;
        if (duckdb_row_count(&res) > 0) *written = duckdb_value_int64(&res, 0, 0);
        duckdb_destroy_result(&res);
        return true;
    };
    auto verify = [&](const std::string &globsql) -> bool {
        std::string nstr;
        if (!s.query_scalar("SELECT count(*) FROM read_parquet(" + globsql + ")",
                            &nstr, err))
            return false;
        long long ondisk = std::strtoll(nstr.c_str(), nullptr, 10);
        if (ondisk != *written) {
            *err = "verification failed: engine reported " +
                   std::to_string(*written) + " rows written but the file scans " +
                   std::to_string(ondisk);
            return false;
        }
        return true;
    };

    OutputTransaction tx;
    if (!reserve_output_transaction(dest, &tx, err)) return kRcEngine;

    if (partition_by.empty()) {
        const std::string tmpdest = (tx.root / "new.parquet").u8string();
        const std::string oldaside = (tx.root / "old.parquet").u8string();
        std::error_code ec2;
        if (!run_copy(tmpdest, "")) {
            return kRcEngine;
        }
        /* GLOB-1a: read_parquet globs its argument; escape so the verify reads
         * exactly the staged file, never a glob-matching sibling. */
        if (!verify(quote_literal(glob_escape(tmpdest)))) {
            std::error_code ec3;
            fs::remove(tmpdest, ec3);
            return kRcEngine;
        }
#ifdef _WIN32
        /* IO-2: Windows rename() fails if the target exists, so we must clear it
         * first — but rename the old file ASIDE rather than deleting it, so a
         * crash or rename failure in the window never leaves no file at all. */
        if (exists) {
            std::error_code ecm;
            fs::rename(dest, oldaside, ecm);
            if (ecm) {
                fs::remove(tmpdest, ec2);
                *err = "could not set aside existing file " + dest + ": " + ecm.message();
                return kRcEngine;
            }
        }
        std::error_code ec4;
        if (output_test_hook("PARQIT_TEST_FAIL_OUTPUT_PUBLISH"))
            ec4 = std::make_error_code(std::errc::io_error);
        else
            fs::rename(tmpdest, dest, ec4);
        if (ec4) {
            std::error_code ecb;
            if (exists) {
                if (output_test_hook("PARQIT_TEST_FAIL_OUTPUT_ROLLBACK"))
                    ecb = std::make_error_code(std::errc::io_error);
                else
                    fs::rename(oldaside, dest, ecb);
            }
            fs::remove(tmpdest, ec2);
            *err = "could not move temporary file onto " + dest + ": " +
                   ec4.message();
            if (ecb) {
                tx.retain_root = true;
                *err += "; rollback also failed: the previous file was NOT "
                        "deleted and is retained for recovery at " +
                        oldaside + " (" + ecb.message() + ")";
            }
            return kRcEngine;
        }
        if (exists) fs::remove(oldaside, ec2);
#else
        std::error_code ec4;
        if (!replace) {
            /* An atomic hard link is the portable no-clobber commit for a
             * regular file: if another process created dest after the initial
             * existence check, this fails instead of replacing it. */
            fs::create_hard_link(tmpdest, dest, ec4);
            if (!ec4) fs::remove(tmpdest, ec2);
        } else {
            fs::rename(tmpdest, dest, ec4); /* atomic replace on POSIX */
        }
        if (ec4) {
            *err = (!replace && fs::exists(dest)
                        ? "file " + dest +
                              " appeared while it was being written; refusing "
                              "to replace it without replace"
                        : "could not move temporary file onto " + dest + ": " +
                              ec4.message());
            return !replace && fs::exists(dest) ? kRcFileExists : kRcEngine;
        }
#endif
    } else {
        std::string pby;
        for (size_t i = 0; i < partition_by.size(); i++) {
            if (i) pby += ", ";
            pby += quote_ident(partition_by[i]);
        }
        const std::string tmpdest = (tx.root / "new").u8string();
        const std::string oldaside = (tx.root / "old").u8string();
        std::error_code ec2;
        auto cleanup_tmp = [&]() {
            std::error_code ec3;
            fs::remove_all(tmpdest, ec3);
        };
        if (!run_copy(tmpdest, ", PARTITION_BY (" + pby + ")")) {
            cleanup_tmp();
            return kRcEngine;
        }
        /* GLOB-1a: escape the staged-dir prefix so only the intentional
         * recursive-glob tail appended below is a real glob; a bracket char in
         * the dest path must not turn the prefix into a pattern matching a
         * sibling tree. */
        std::string glob = glob_escape(tmpdest);
        if (!glob.empty() && glob.back() != '/' && glob.back() != '\\') glob += "/";
        glob += "**/*.parquet";
        if (!verify(quote_literal(glob) + ", hive_partitioning = true")) {
            cleanup_tmp();
            return kRcEngine;
        }
        /* honour replace, but rename the old tree ASIDE rather than deleting it
         * before the swap: a crash or rename failure in the gap between a
         * remove and the rename would otherwise leave NEITHER tree (ATOM-PART-1).
         * The aside copy is removed only after the swap succeeds, and restored
         * if the swap fails. */
        bool saved_aside = false;
        if (exists && replace) {
            std::error_code ecr;
            fs::rename(dest, oldaside, ecr);
            if (ecr) {
                cleanup_tmp();
                *err = "could not set aside existing partition tree " + dest +
                       ": " + ecr.message();
                return kRcEngine;
            }
            saved_aside = true;
        }
        std::error_code ec4;
        if (output_test_hook("PARQIT_TEST_FAIL_OUTPUT_PUBLISH"))
            ec4 = std::make_error_code(std::errc::io_error);
        else
            fs::rename(tmpdest, dest, ec4);
        if (ec4) {
            std::error_code ecb;
            if (saved_aside) {
                if (output_test_hook("PARQIT_TEST_FAIL_OUTPUT_ROLLBACK"))
                    ecb = std::make_error_code(std::errc::io_error);
                else
                    fs::rename(oldaside, dest, ecb);
            }
            cleanup_tmp();
            *err = "could not move temporary partition tree onto " + dest +
                   ": " + ec4.message();
            if (ecb) {
                tx.retain_root = true;
                *err += "; rollback also failed: the previous partition tree "
                        "was NOT deleted and is retained for recovery at " +
                        oldaside + " (" + ecb.message() + ")";
            }
            return kRcEngine;
        }
        if (saved_aside) {
            std::error_code ecd;
            fs::remove_all(oldaside, ecd); /* swap done: drop the old tree */
        }
    }
    return 0;
}

/* ===================================================== use_prepare ===== */

ST_retcode cmd_use_prepare(const std::vector<std::string> &args) {
    std::string reqpath, err;
    if (args.size() < 2 || !parqit::hex_decode(args[1], reqpath)) {
        cry("parqit use: malformed request path");
        return kRcUsage;
    }
    json req;
    if (!parqit::load_request(reqpath, &req, &err)) {
        cry(err);
        return kRcUsage;
    }
    std::vector<std::string> files, varlist;
    std::string respfile, tmpdir, strlfile;
    if (!parqit::req_text_list(req, "files", &files, &err) ||
        !parqit::req_text_list(req, "varlist", &varlist, &err, false) ||
        !parqit::req_text(req, "respfile", &respfile, &err) ||
        !parqit::req_text(req, "strlfile", &strlfile, &err, false) ||
        !parqit::req_text(req, "tmpdir", &tmpdir, &err)) {
        cry(err);
        return kRcUsage;
    }
    if (files.empty()) {
        cry("parqit use: no input files");
        return kRcUsage;
    }

    Session &s = Session::instance();
    s.set_default_temp_dir(tmpdir + parqit::spill_suffix());

    const Source src = source_for(files, req.value("relaxed", false),
                                  req.value("csv", false));
    ST_retcode grc = strict_schema_gate(s, src, files, req.value("relaxed", false),
                                        req.value("csv", false), &err);
    if (grc != 0) {
        cry("parqit use: " + err);
        return grc;
    }
    PlanContext ctx;
    ST_retcode rc = plan_columns(s, src, varlist, /*with_stats=*/true, &ctx, &err);
    if (rc != 0) {
        cry("parqit use: " + err);
        return rc;
    }

    parqit::ResponseWriter w;
    if (!w.open(respfile, &err)) {
        cry(err);
        return kRcEngine;
    }
    write_var_records(w, ctx);
    if (!w.close(&err)) {
        cry(err);
        return kRcEngine;
    }

    /* N-2G31: the SPI's observation index (ST_int in SF_vstore/SF_sstore) is
     * a 32-bit int, so a file beyond 2,147,483,647 rows cannot be read into
     * one Stata dataset (live find: a 2.67B-row trades glob died as the
     * ado's bare `option n() invalid`). Refuse loudly with the out-of-core
     * remedies instead of failing in the arg parser. */
    if (ctx.nrows > kSpiMaxObs) {
        cry("parqit use: the file(s) hold " + std::to_string(ctx.nrows) +
            " rows — more than the 2,147,483,647 observations the Stata "
            "plugin interface can address; open them lazily (parqit use "
            "using ...) and aggregate or filter (parqit collapse, parqit "
            "keep if/in) before collecting, or process them with parqit save");
        return kRcNoRoomObs;
    }
    std::string tag;
    std::string names;
    for (size_t i = 0; i < ctx.active.size(); i++) {
        if (i) names += " ";
        names += ctx.active[i].stata_name;
    }
    set_prepared_read(src.scan_sql, ctx.active, ctx.nrows, strlfile, false, &tag);
    save_local("_parqit_tag", parqit::hex_encode(tag));
    save_local("_parqit_n", std::to_string(ctx.nrows));
    save_local("_parqit_k", std::to_string(ctx.active.size()));
    save_local("_parqit_names", names);
    save_local("_parqit_fast_source_ok", "0");
    save_local("_parqit_fast_source_path", "");
    save_local("_parqit_fast_source_size", "");
    save_local("_parqit_fast_source_mtime", "");
    if (!req.value("csv", false) && !req.value("relaxed", false) &&
        files.size() == 1) {
        std::string abs, fsize, fmtime;
        if (regular_file_fingerprint(files[0], &abs, &fsize, &fmtime)) {
            save_local("_parqit_fast_source_ok", "1");
            save_local("_parqit_fast_source_path", parqit::hex_encode(abs));
            save_local("_parqit_fast_source_size", fsize);
            save_local("_parqit_fast_source_mtime", fmtime);
        }
    }
    return 0;
}

/* ======================================================= use_fetch ===== */

namespace {

/* Walk one Arrow column of one chunk into Stata variable i (1-based),
 * starting at observation base+1. strL cells go to the binary sidecar
 * (the SPI has no strL write call); Mata pours them in afterwards. */
bool fill_column(const ColumnPlan &p, int i, long long base, const ArrowArray *col,
                 std::FILE *strl_spill, long long *inf_seen, long long *nul_seen,
                 long long *rng_seen, long long *subms_seen, std::string *err) {
    const uint8_t *validity = static_cast<const uint8_t *>(col->buffers[0]);
    const int64_t off = col->offset;
    auto valid = [&](int64_t r) -> bool {
        if (!validity) return true;
        int64_t pos = off + r;
        return (validity[pos >> 3] >> (pos & 7)) & 1;
    };
    /* NUM1/IO1 (+T2): the storable window of the PLANNED Stata type. A value
     * outside it is silently converted to missing by SF_vstore — which is how
     * a file whose footer statistics understate the true range (the F2 fast
     * path trusts them; honest writers' stats are exact) or a date beyond
     * Stata's %td long turned real values into `.` with rc 0 and no message.
     * Count every such cell; the caller refuses the whole load afterwards
     * (loud, never a silently-missing real value). Double/strings have no
     * narrower window and skip the check. */
    double rlo = 0, rhi = 0;
    bool ranged = true;
    switch (p.stata_type) {
    case StType::Byte: rlo = parqit::kStataByteMin; rhi = parqit::kStataByteMax; break;
    case StType::Int: rlo = parqit::kStataIntMin; rhi = parqit::kStataIntMax; break;
    case StType::Long: rlo = parqit::kStataLongMin; rhi = parqit::kStataLongMax; break;
    case StType::Float: rlo = -parqit::kStataFloatMax; rhi = parqit::kStataFloatMax; break;
    default: ranged = false; break;
    }
    auto in_range = [&](double d) -> bool {
        if (!ranged || (d >= rlo && d <= rhi)) return true;
        if (rng_seen) (*rng_seen)++;
        return false;
    };
    auto store_num = [&](int64_t r, double value) -> bool {
        ST_retcode rc = SF_vstore(i, base + r + 1, value);
        if (rc != 0) {
            *err = "could not store " + p.stata_name + "[" +
                   std::to_string(base + r + 1) + "] (rc=" +
                   std::to_string(rc) + ")";
            return false;
        }
        return true;
    };
    auto store_str = [&](int64_t r, char *value) -> bool {
        ST_retcode rc = SF_sstore(i, base + r + 1, value);
        if (rc != 0) {
            *err = "could not store " + p.stata_name + "[" +
                   std::to_string(base + r + 1) + "] (rc=" +
                   std::to_string(rc) + ")";
            return false;
        }
        return true;
    };
    switch (p.transfer) {
    case Transfer::Int8: {
        const int8_t *v = static_cast<const int8_t *>(col->buffers[1]);
        for (int64_t r = 0; r < col->length; r++)
            if (valid(r)) {
                double d = static_cast<double>(v[off + r]);
                if (!store_num(r, in_range(d) ? d : SV_missval)) return false;
            }
        return true;
    }
    case Transfer::Int16: {
        const int16_t *v = static_cast<const int16_t *>(col->buffers[1]);
        for (int64_t r = 0; r < col->length; r++)
            if (valid(r)) {
                double d = static_cast<double>(v[off + r]);
                if (!store_num(r, in_range(d) ? d : SV_missval)) return false;
            }
        return true;
    }
    case Transfer::Int32: {
        const int32_t *v = static_cast<const int32_t *>(col->buffers[1]);
        for (int64_t r = 0; r < col->length; r++)
            if (valid(r)) {
                double d = static_cast<double>(v[off + r]);
                if (!store_num(r, in_range(d) ? d : SV_missval)) return false;
            }
        return true;
    }
    case Transfer::Int64: {
        const int64_t *v = static_cast<const int64_t *>(col->buffers[1]);
        for (int64_t r = 0; r < col->length; r++)
            if (valid(r)) {
                double d = static_cast<double>(v[off + r]);
                if (!store_num(r, in_range(d) ? d : SV_missval)) return false;
            }
        return true;
    }
    case Transfer::Float32: {
        const float *v = static_cast<const float *>(col->buffers[1]);
        for (int64_t r = 0; r < col->length; r++)
            if (valid(r)) {
                double d = static_cast<double>(v[off + r]);
                /* NaN is the de-facto float NA (silent). A value Stata cannot
                 * hold — ±Inf, or a finite magnitude >= Stata's missing
                 * sentinel (|d| >= SV_missval ~8.99e307, which Stata would read
                 * back as a MISSING value) — is stored as missing and counted
                 * so the load says so, never a silent wrong number. A finite
                 * value beyond the planned float window (lying stats sized the
                 * column float; the data says double) counts as out-of-range. */
                bool unstorable = std::isnan(d) || std::isinf(d) ||
                                  std::fabs(d) >= SV_missval;
                if (!std::isnan(d) && unstorable && inf_seen) (*inf_seen)++;
                if (!unstorable && !in_range(d)) unstorable = true;
                if (!store_num(r, unstorable ? SV_missval : d)) return false;
            }
        return true;
    }
    case Transfer::Float64: {
        const double *v = static_cast<const double *>(col->buffers[1]);
        for (int64_t r = 0; r < col->length; r++)
            if (valid(r)) {
                double d = v[off + r];
                bool unstorable = std::isnan(d) || std::isinf(d) ||
                                  std::fabs(d) >= SV_missval;
                if (!std::isnan(d) && unstorable && inf_seen) (*inf_seen)++;
                if (!unstorable && !in_range(d)) unstorable = true;
                if (!store_num(r, unstorable ? SV_missval : d)) return false;
            }
        return true;
    }
    case Transfer::Date32: {
        const int32_t *v = static_cast<const int32_t *>(col->buffers[1]);
        for (int64_t r = 0; r < col->length; r++)
            if (valid(r)) {
                double d = static_cast<double>(v[off + r]) +
                           parqit::kEpochShiftDays;
                if (!store_num(r, in_range(d) ? d : SV_missval)) return false;
            }
        return true;
    }
    case Transfer::TimestampUs: {
        const int64_t *v = static_cast<const int64_t *>(col->buffers[1]);
        for (int64_t r = 0; r < col->length; r++)
            if (valid(r)) {
                /* T1: a plain us TIMESTAMP silently dropped sub-ms precision
                 * with no note (unlike the NS/TZ paths). Count a real drop so
                 * the load says so — only when it actually happens, never a
                 * blanket note on every ms-exact us column. */
                if (p.note_subms && v[off + r] % 1000 != 0 && subms_seen)
                    (*subms_seen)++;
                long long ms = parqit::floordiv(v[off + r], 1000);
                const long long stata_ms = ms + parqit::kEpochShiftMs;
                const double stored = static_cast<double>(stata_ms);
                if (static_cast<long long>(stored) != stata_ms) {
                    *err = "column " + p.stata_name + " observation " +
                           std::to_string(base + r + 1) +
                           " has a timestamp millisecond count that is not "
                           "exactly representable in Stata binary64; refusing "
                           "to move the instant silently";
                    return false;
                }
                if (!store_num(r, stored))
                    return false;
            }
        return true;
    }
    case Transfer::TimeUs: {
        const int64_t *v = static_cast<const int64_t *>(col->buffers[1]);
        for (int64_t r = 0; r < col->length; r++)
            if (valid(r)) {
                if (p.note_subms && v[off + r] % 1000 != 0 && subms_seen)
                    (*subms_seen)++;
                long long ms = parqit::floordiv(v[off + r], 1000);
                if (!store_num(r, static_cast<double>(ms))) return false;
            }
        return true;
    }
    case Transfer::Utf8: {
        /* This walker assumes Arrow `utf8`: 3 buffers (validity, int32 offsets,
         * data). DuckDB 1.5.3 emits exactly that for VARCHAR (the canonical
         * transfer type). Guard the layout so a future engine change to
         * large_utf8 / string-view (different buffer count) fails loudly here
         * instead of silently reading garbage offsets (STR-2). */
        if (col->n_buffers != 3) {
            *err = "internal: unexpected Arrow string layout (" +
                   std::to_string(col->n_buffers) + " buffers) for " +
                   p.stata_name;
            return false;
        }
        const int32_t *offs = static_cast<const int32_t *>(col->buffers[1]);
        const char *chars = static_cast<const char *>(col->buffers[2]);
        const bool is_strl = (p.stata_type == StType::StrL);
        std::string buf;
        for (int64_t r = 0; r < col->length; r++) {
            if (!valid(r)) continue; /* NULL string → "" (already "") */
            int32_t b = offs[off + r], e = offs[off + r + 1];
            if (e <= b) continue;
            if (is_strl) {
                if (!strl_spill) {
                    *err = "internal: no strL sidecar for " + p.stata_name;
                    return false;
                }
                /* fixed 35-byte header: var(10) + obs(13) + len(12). The obs
                 * field is 13 digits so it spans Stata-MP's full ~1.1e12 row
                 * capacity (a 10-digit field overflowed past 1e10 rows). The
                 * Mata reader (_parqit_apply_strl) parses the same offsets. */
                char hdr[64];
                std::snprintf(hdr, sizeof(hdr), "%010d%013lld%012lld", i,
                              static_cast<long long>(base + r + 1),
                              static_cast<long long>(e - b));
                if (std::fwrite(hdr, 1, 35, strl_spill) != 35 ||
                    std::fwrite(chars + b, 1, static_cast<size_t>(e - b),
                                strl_spill) != static_cast<size_t>(e - b)) {
                    *err = "could not write the strL sidecar (disk full?)";
                    return false;
                }
            } else {
                /* SF_sstore is a C string: an embedded NUL truncates the
                 * cell — count it so the load reports the loss loudly */
                if (std::memchr(chars + b, '\0', static_cast<size_t>(e - b)) &&
                    nul_seen)
                    (*nul_seen)++;
                buf.assign(chars + b, chars + e);
                if (!store_str(r, const_cast<char *>(buf.c_str()))) return false;
            }
        }
        return true;
    }
    }
    *err = "internal: unexpected transfer type for column " + p.stata_name;
    return false;
}

/* --------------------------------------------------------------- parallel fill
 * The per-cell SPI store (SF_vstore/SF_sstore) writes one pre-allocated cell of
 * the staged frame at an explicit (variable, observation) address, so concurrent
 * stores to *disjoint* cells never alias — the prior-art `stata_parquet_io` (pq)
 * calls the identical SF_vstore/SF_sstore from many worker threads in production,
 * each owning a disjoint row range, which is what proves the store is reentrant
 * for distinct cells.
 *
 * parqit runs a producer/consumer pipeline. The calling thread is the *producer*:
 * it drives DuckDB (duckdb_fetch_chunk) and converts each chunk to an Arrow array
 * (both must stay single-threaded — one result cursor), then hands the chunk to a
 * bounded queue. N *worker* threads each pull a whole chunk and fill all of its
 * columns with fill_column — reused byte-for-byte, so every type/missing/Inf/NUL
 * rule is identical to the serial path; only the scheduling differs. Because two
 * workers never hold the same chunk, and chunks cover disjoint observation
 * ranges, no two threads ever touch the same cell. This overlaps the ~1 s of
 * fetch+convert with the fill instead of serialising them, and processes each
 * chunk while its Arrow buffers are still cache-warm.
 *
 * Race-freedom of the shared state: the strL sidecar FILE is written under
 * strl_mu (rare, position-encoded headers, so the lock is cheap and order does
 * not matter); the Inf/NUL tallies are per-worker vectors reduced after the join;
 * the queue, abort flag and first-error string are guarded by the queue mutex. */

struct ChunkSlot {
    ArrowArray arr;
    duckdb_data_chunk chunk;
    long long base; /* global 0-based obs offset; SF row = base + r + 1 */
};

/* Rows below this stay on the unchanged serial path: thread setup would cost
 * more than it saves, and small-read latency must not regress. */
constexpr long long kParallelMinRows = 50'000;
/* Cap on fill workers. The pipeline's single producer (DuckDB fetch + Arrow
 * convert, necessarily serial) is the bottleneck once the fill is hidden behind
 * it, and the SPI store does not scale linearly anyway; measured on a 48-core box
 * the read is flat from ~4 workers and best near 8, then regresses past ~12 from
 * oversubscription. 8 captures the win while staying light on shared HPC nodes;
 * PARQIT_FILL_THREADS overrides it for atypical (very wide / string-heavy) reads. */
constexpr int kFillThreadCap = 8;

/* Workers to use for the fill of an n-row read (1 == serial path). Honours
 * PARQIT_FILL_THREADS (0/1 disables; >1 forces a count) for tuning and as an escape
 * hatch, else scales to the hardware up to kFillThreadCap. */
int fill_thread_count(long long nrows) {
    /* An explicit override wins even below the row threshold: 0/1 forces the
     * serial path, >1 forces that many workers regardless of n — the latter lets
     * the test suite drive the parallel path through every small-data invariant
     * (0/1 row, 1 var, strL, dup names, locale, float extremes, …). */
    if (const char *e = std::getenv("PARQIT_FILL_THREADS")) {
        char *end = nullptr;
        long v = std::strtol(e, &end, 10);
        if (end != e && v >= 0)
            return v <= 1 ? 1 : static_cast<int>(std::min<long>(v, 1024));
    }
    if (nrows < kParallelMinRows) return 1;
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    int t = static_cast<int>(std::min<unsigned>(hw, kFillThreadCap));
    return t < 1 ? 1 : t;
}

/* Shared state of the fill pipeline: a bounded queue of converted chunks plus the
 * producer-done / aborted flags and the first error seen by any worker. */
struct FillQueue {
    std::mutex m;
    std::condition_variable cv_not_full;
    std::condition_variable cv_not_empty;
    std::deque<ChunkSlot> q;
    size_t cap = 64;
    bool producer_done = false;
    bool aborted = false;
    std::string err;
    std::mutex strl_mu; /* serialises the shared strL sidecar FILE */
};

/* One fill worker: pull whole chunks and fill every column until the producer is
 * done and the queue drains, or someone aborts. Regular columns fill lock-free
 * (disjoint cells); strL columns fill under strl_mu. inf/nul tally into this
 * worker's own vectors. */
void fill_worker(FillQueue &fq, const std::vector<ColumnPlan> &plans,
                 const std::vector<int> &regular_cols,
                 const std::vector<int> &strl_cols, std::FILE *strl_spill,
                 std::vector<long long> &inf_local,
                 std::vector<long long> &nul_local,
                 std::vector<long long> &rng_local,
                 std::vector<long long> &subms_local) {
    /* This runs on a worker thread, so NO exception may escape it — an exception
     * crossing a std::thread boundary calls std::terminate and would take the
     * whole Stata process down (the charter rule the SPI catch-all enforces on
     * the main thread). A fill that throws (e.g. std::bad_alloc on a huge string
     * slice under memory pressure) is funnelled into the same first-error/abort
     * path a soft fill_column failure uses, which the producer reconciles into a
     * loud nonzero rc — exactly what the serial path does via the catch-all. */
    for (;;) {
        ChunkSlot slot;
        bool owns = false;
        std::string lerr;
        bool ok = true;
        try {
            {
                std::unique_lock<std::mutex> lk(fq.m);
                fq.cv_not_empty.wait(lk, [&] {
                    return !fq.q.empty() || fq.producer_done || fq.aborted;
                });
                if (fq.aborted) return;
                if (fq.q.empty()) {
                    if (fq.producer_done) return;
                    continue;
                }
                slot = fq.q.front();
                fq.q.pop_front();
                owns = true;
                fq.cv_not_full.notify_one();
            }

            for (int col : regular_cols)
                if (!fill_column(plans[col], col + 1, slot.base,
                                 slot.arr.children[col], /*strl_spill=*/nullptr,
                                 &inf_local[col], &nul_local[col], &rng_local[col],
                                 &subms_local[col], &lerr)) {
                    ok = false;
                    break;
                }
            if (ok && !strl_cols.empty()) {
                std::lock_guard<std::mutex> g(fq.strl_mu);
                for (int col : strl_cols)
                    if (!fill_column(plans[col], col + 1, slot.base,
                                     slot.arr.children[col], strl_spill,
                                     &inf_local[col], &nul_local[col],
                                     &rng_local[col], &subms_local[col], &lerr)) {
                        ok = false;
                        break;
                    }
            }
        } catch (const std::exception &e) {
            lerr = std::string("fill failed: ") + e.what();
            ok = false;
        } catch (...) {
            lerr = "fill failed: unknown error";
            ok = false;
        }

        if (owns) {
            if (slot.arr.release) slot.arr.release(&slot.arr);
            duckdb_destroy_data_chunk(&slot.chunk);
        }

        if (!ok) {
            try {
                std::lock_guard<std::mutex> lk(fq.m);
                if (fq.err.empty()) fq.err = lerr;
                fq.aborted = true;
                fq.cv_not_empty.notify_all();
                fq.cv_not_full.notify_all();
            } catch (...) { /* mutex unusable: nothing safe left to do */
            }
            return;
        }
    }
}

} // namespace

ST_retcode cmd_use_fetch(const std::vector<std::string> &args) {
    std::string tag, err;
    if (args.size() < 2 || !parqit::hex_decode(args[1], tag)) {
        cry("parqit use: malformed fetch tag");
        return kRcUsage;
    }
    if (!g_prepared.live || g_prepared.tag != tag) {
        cry("parqit use: no prepared read (was the session reset between prepare "
            "and fetch?)");
        return kRcUsage;
    }
    PreparedRead prep = std::move(g_prepared);
    g_prepared = PreparedRead();

    const int k = static_cast<int>(prep.plans.size());
    if (SF_nvars() != k) {
        cry("parqit use: internal manifest mismatch (variable count)");
        return kRcMismatch;
    }
    for (int i = 0; i < k; i++) {
        bool want_str = (prep.plans[i].transfer == Transfer::Utf8);
        if (static_cast<bool>(SF_var_is_string(i + 1)) != want_str) {
            cry("parqit use: internal manifest mismatch (type of " +
                prep.plans[i].stata_name + ")");
            return kRcMismatch;
        }
    }
    if (SF_in1() != 1 || SF_in2() != prep.nrows) {
        cry("parqit use: internal manifest mismatch (observation range)");
        return kRcMismatch;
    }

    std::string sel;
    for (int i = 0; i < k; i++) {
        const ColumnPlan &p = prep.plans[i];
        if (i) sel += ", ";
        sel += p.cast_sql.empty() ? quote_ident(p.source_name) : p.cast_sql;
        sel += " AS " + quote_ident(p.source_name);
    }
    Session &s = Session::instance();
    duckdb_result res;
    if (!s.query("SELECT " + sel + " FROM " + prep.source_sql, &res, &err)) {
        cry("parqit use: " + err);
        return kRcEngine;
    }

    std::FILE *strl_spill = nullptr;
    bool any_strl = false;
    for (const auto &p : prep.plans) any_strl |= (p.stata_type == StType::StrL);
    if (any_strl) {
        if (prep.strl_path.empty()) {
            cry("parqit use: internal: missing strL sidecar path");
            duckdb_destroy_result(&res);
            return kRcUsage;
        }
        strl_spill = std::fopen(prep.strl_path.c_str(), "wb");
        if (!strl_spill) {
            cry("parqit use: could not create the strL sidecar file");
            duckdb_destroy_result(&res);
            return kRcEngine;
        }
    }

    duckdb_arrow_options aopts;
    duckdb_connection_get_arrow_options(s.con(), &aopts);

    std::vector<long long> inf_counts(k, 0), nul_counts(k, 0), rng_counts(k, 0),
        subms_counts(k, 0);
    long long base = 0;
    ST_retcode rc = 0;
    const int nthreads = fill_thread_count(prep.nrows);

    if (nthreads <= 1) {
        /* Serial path — unchanged: one chunk at a time, every column inline.
         * Used for small reads and as the PARQIT_FILL_THREADS=0 escape hatch. */
        while (true) {
            duckdb_data_chunk chunk = duckdb_fetch_chunk(res);
            if (!chunk) break;
            ArrowArray arr;
            std::memset(&arr, 0, sizeof(arr));
            duckdb_error_data ed = duckdb_data_chunk_to_arrow(aopts, chunk, &arr);
            if (ed) {
                err = duckdb_error_data_message(ed);
                duckdb_destroy_error_data(&ed);
                duckdb_destroy_data_chunk(&chunk);
                rc = kRcEngine;
                break;
            }
            if (arr.n_children != k) {
                err = "internal: chunk column count mismatch";
                rc = kRcMismatch;
            } else {
                for (int i = 0; i < k && rc == 0; i++) {
                    if (!fill_column(prep.plans[i], i + 1, base, arr.children[i],
                                     strl_spill, &inf_counts[i], &nul_counts[i],
                                     &rng_counts[i], &subms_counts[i], &err))
                        rc = kRcEngine;
                }
            }
            base += arr.length;
            if (arr.release) arr.release(&arr);
            duckdb_destroy_data_chunk(&chunk);
            if (rc != 0) break;
            if (SW_stopflag) {
                err = "break";
                rc = 1;
                break;
            }
        }
    } else {
        /* Parallel path — producer/consumer pipeline. This thread produces
         * (fetch + Arrow-convert, both necessarily single-threaded) and the
         * worker pool consumes (fill whole chunks), so the ~1 s of fetch+convert
         * overlaps the fill instead of running before it. */
        std::vector<int> regular_cols, strl_cols;
        for (int i = 0; i < k; i++) {
            if (prep.plans[i].stata_type == StType::StrL)
                strl_cols.push_back(i);
            else
                regular_cols.push_back(i);
        }

        FillQueue fq;
        /* Deep enough that the faster fill never starves the producer, shallow
         * enough that in-flight Arrow chunks stay to tens of MB (a chunk is
         * ≤2048 rows). */
        fq.cap = static_cast<size_t>(std::max(nthreads * 4, 32));

        std::vector<std::vector<long long>> inf_locals(
            nthreads, std::vector<long long>(k, 0));
        std::vector<std::vector<long long>> nul_locals(
            nthreads, std::vector<long long>(k, 0));
        std::vector<std::vector<long long>> rng_locals(
            nthreads, std::vector<long long>(k, 0));
        std::vector<std::vector<long long>> subms_locals(
            nthreads, std::vector<long long>(k, 0));
        std::vector<std::thread> workers;
        workers.reserve(nthreads);
        /* LIFE-018: std::thread's destructor terminates the process while it is
         * joinable.  If creating worker N throws after workers 0..N-1 exist,
         * signal those workers, join them, and return a normal loud error. */
        try {
            int fail_at = -1;
            if (const char *inject =
                    std::getenv("PARQIT_TEST_FAIL_THREAD_AT")) {
                char *end = nullptr;
                long v = std::strtol(inject, &end, 10);
                if (end != inject && *end == '\0' && v >= 0 && v < nthreads)
                    fail_at = static_cast<int>(v);
            }
            for (int t = 0; t < nthreads; t++) {
                if (t == fail_at)
                    throw std::runtime_error(
                        "deterministic test injection at worker " +
                        std::to_string(t));
                workers.emplace_back(
                    fill_worker, std::ref(fq), std::cref(prep.plans),
                    std::cref(regular_cols), std::cref(strl_cols), strl_spill,
                    std::ref(inf_locals[t]), std::ref(nul_locals[t]),
                    std::ref(rng_locals[t]), std::ref(subms_locals[t]));
            }
        } catch (const std::exception &e) {
            std::lock_guard<std::mutex> lk(fq.m);
            fq.aborted = true;
            fq.producer_done = true;
            fq.err = std::string("could not create fill worker: ") + e.what();
            rc = kRcEngine;
        } catch (...) {
            std::lock_guard<std::mutex> lk(fq.m);
            fq.aborted = true;
            fq.producer_done = true;
            fq.err = "could not create fill worker: unknown error";
            rc = kRcEngine;
        }
        if (rc != 0) {
            fq.cv_not_empty.notify_all();
            fq.cv_not_full.notify_all();
        }

        /* The producer runs under try/catch too: a throw here (e.g. a deque
         * push_back std::bad_alloc under memory pressure) must not unwind past the
         * still-joinable workers — that calls std::terminate. It is converted to
         * the same abort path the soft producer errors use, so the uniform join +
         * reconcile below turns it into a loud nonzero rc (the ado then drops the
         * staged frame, preserving the live data — parity with the serial path). */
        if (rc == 0) {
            try {
                for (;;) {
                {
                    std::lock_guard<std::mutex> lk(fq.m);
                    if (fq.aborted) break;
                }
                if (SW_stopflag) {
                    std::lock_guard<std::mutex> lk(fq.m);
                    if (fq.err.empty()) fq.err = "break";
                    fq.aborted = true;
                    rc = 1;
                    fq.cv_not_empty.notify_all();
                    break;
                }
                duckdb_data_chunk chunk = duckdb_fetch_chunk(res);
                if (!chunk) break; /* end of stream */
                ChunkSlot slot;
                std::memset(&slot.arr, 0, sizeof(slot.arr));
                duckdb_error_data ed =
                    duckdb_data_chunk_to_arrow(aopts, chunk, &slot.arr);
                if (ed) {
                    err = duckdb_error_data_message(ed);
                    duckdb_destroy_error_data(&ed);
                    duckdb_destroy_data_chunk(&chunk);
                    std::lock_guard<std::mutex> lk(fq.m);
                    if (fq.err.empty()) fq.err = err;
                    fq.aborted = true;
                    rc = kRcEngine;
                    fq.cv_not_empty.notify_all();
                    break;
                }
                if (slot.arr.n_children != k) {
                    if (slot.arr.release) slot.arr.release(&slot.arr);
                    duckdb_destroy_data_chunk(&chunk);
                    std::lock_guard<std::mutex> lk(fq.m);
                    if (fq.err.empty())
                        fq.err = "internal: chunk column count mismatch";
                    fq.aborted = true;
                    rc = kRcMismatch;
                    fq.cv_not_empty.notify_all();
                    break;
                }
                slot.chunk = chunk;
                slot.base = base;
                base += slot.arr.length;

                std::unique_lock<std::mutex> lk(fq.m);
                fq.cv_not_full.wait(
                    lk, [&] { return fq.q.size() < fq.cap || fq.aborted; });
                if (fq.aborted) {
                    lk.unlock();
                    if (slot.arr.release) slot.arr.release(&slot.arr);
                    duckdb_destroy_data_chunk(&slot.chunk);
                    break;
                }
                fq.q.push_back(slot);
                fq.cv_not_empty.notify_one();
                }
            } catch (const std::exception &e) {
                std::lock_guard<std::mutex> lk(fq.m);
                fq.aborted = true;
                if (fq.err.empty())
                    fq.err = std::string("read failed: ") + e.what();
            } catch (...) {
                std::lock_guard<std::mutex> lk(fq.m);
                fq.aborted = true;
                if (fq.err.empty()) fq.err = "read failed: unknown error";
            }
        }

        {
            std::lock_guard<std::mutex> lk(fq.m);
            fq.producer_done = true;
        }
        fq.cv_not_empty.notify_all();
        fq.cv_not_full.notify_all();
        for (auto &w : workers)
            if (w.joinable()) w.join();

        /* Anything still queued when a worker aborted must be freed here. */
        for (ChunkSlot &slot : fq.q) {
            if (slot.arr.release) slot.arr.release(&slot.arr);
            duckdb_destroy_data_chunk(&slot.chunk);
        }
        fq.q.clear();

        if (fq.aborted && rc == 0) {
            rc = kRcEngine;
            err = fq.err.empty() ? "fill failed" : fq.err;
        } else if (rc != 0 && err.empty()) {
            err = fq.err;
        }

        for (int t = 0; t < nthreads; t++)
            for (int c = 0; c < k; c++) {
                inf_counts[c] += inf_locals[t][c];
                nul_counts[c] += nul_locals[t][c];
                rng_counts[c] += rng_locals[t][c];
                subms_counts[c] += subms_locals[t][c];
            }
    }
    duckdb_destroy_arrow_options(&aopts);
    duckdb_destroy_result(&res);
    if (strl_spill) {
        bool flush_ok = (std::fflush(strl_spill) == 0);
        flush_ok = (std::fclose(strl_spill) == 0) && flush_ok;
        if (!flush_ok && rc == 0) {
            err = "could not flush the strL sidecar (disk full?)";
            rc = kRcEngine;
        }
    }
    if (prep.drop_source_after) {
        std::string derr;
        Session::instance().exec("DROP TABLE IF EXISTS " + prep.source_sql, &derr);
    }

    if (rc != 0) {
        cry("parqit use: load failed: " + err);
        return rc;
    }
    if (base != prep.nrows) {
        cry("parqit use: row count changed between prepare and fetch (" +
            std::to_string(prep.nrows) + " → " + std::to_string(base) +
            "); files modified concurrently?");
        return kRcEngine;
    }
    /* NUM1/IO1 (+T2): a narrowing fill that had to convert real values to
     * missing means the plan's range was wrong — footer statistics that
     * understate the data (a spec-violating file; honest writers' stats are
     * exact) or a date beyond Stata's %td window. Storing `.` where the file
     * holds a real value is silent corruption, so the whole load is refused
     * loudly; the ado's staged swap keeps the in-memory dataset intact. */
    for (int i = 0; i < k; i++) {
        if (rng_counts[i] > 0) {
            cry("parqit use: column " + prep.plans[i].stata_name + " has " +
                std::to_string(rng_counts[i]) +
                " value(s) outside the storable range of its planned " +
                parqit::sttype_code(prep.plans[i].stata_type,
                                    prep.plans[i].str_bytes) +
                " type — the file's Parquet statistics understate the data "
                "(a spec-violating file, whose stats also mislead DuckDB's "
                "own predicate pushdown), or a value genuinely exceeds Stata's "
                "range (e.g. a date far outside the %td window). Refusing to "
                "load real values as missing; rewrite the file with correct "
                "statistics, or correct the offending values at the source.");
            return kRcEngine;
        }
    }
    for (int i = 0; i < k; i++) {
        /* NUM2: the per-column plan note ('decimal converted to double',
         * 'values beyond 2^53 rounded', 'nanosecond timestamp truncated', an
         * all-missing byte, …) is emitted here via SF_error, like the inf/nul
         * notes — so it survives `quietly parqit use`/`collect` instead of
         * being swallowed by the ado's suppressible printf. */
        if (!prep.plans[i].note.empty())
            cry("note: " + prep.plans[i].stata_name + ": " + prep.plans[i].note);
        if (inf_counts[i] > 0)
            cry("note: " + prep.plans[i].stata_name + ": " +
                std::to_string(inf_counts[i]) +
                " value(s) outside Stata's storable range stored as missing "
                "(infinity, or a finite magnitude >= 8.99e+307)");
        if (nul_counts[i] > 0)
            cry("note: " + prep.plans[i].stata_name + ": " +
                std::to_string(nul_counts[i]) +
                " string value(s) truncated at an embedded NUL byte "
                "(Stata strings cannot hold NUL)");
        if (subms_counts[i] > 0)
            cry("note: " + prep.plans[i].stata_name + ": " +
                std::to_string(subms_counts[i]) +
                " value(s) truncated to Stata's millisecond resolution "
                "(sub-millisecond precision is not representable)");
    }
    save_local("_parqit_fetched_n", std::to_string(base));
    return 0;
}

/* ======================================================== describe ===== */

ST_retcode cmd_describe(const std::vector<std::string> &args) {
    std::string reqpath, err;
    if (args.size() < 2 || !parqit::hex_decode(args[1], reqpath)) {
        cry("parqit describe: malformed request path");
        return kRcUsage;
    }
    json req;
    if (!parqit::load_request(reqpath, &req, &err)) {
        cry(err);
        return kRcUsage;
    }
    std::vector<std::string> files;
    std::string respfile, tmpdir;
    if (!parqit::req_text_list(req, "files", &files, &err) ||
        !parqit::req_text(req, "respfile", &respfile, &err) ||
        !parqit::req_text(req, "tmpdir", &tmpdir, &err)) {
        cry(err);
        return kRcUsage;
    }
    Session &s = Session::instance();
    s.set_default_temp_dir(tmpdir + parqit::spill_suffix());

    const Source src = source_for(files);
    ST_retcode grc = strict_schema_gate(s, src, files, /*relaxed=*/false,
                                        /*csv=*/false, &err);
    if (grc != 0) {
        /* a describe of a mixed-schema glob must not print the first file's
         * schema as if it were THE schema (the plausible-but-fake preview) */
        cry("parqit describe: " + err);
        return grc;
    }
    PlanContext ctx;
    ST_retcode rc = plan_columns(s, src, {}, /*with_stats=*/false, &ctx, &err);
    if (rc != 0) {
        cry("parqit describe: " + err);
        return rc;
    }

    parqit::ResponseWriter w;
    if (!w.open(respfile, &err)) {
        cry(err);
        return kRcEngine;
    }
    duckdb_result dres;
    if (s.query("DESCRIBE SELECT * FROM " + src.scan_sql, &dres, &err)) {
        idx_t n = duckdb_row_count(&dres);
        for (idx_t r = 0; r < n; r++) {
            char *nm = duckdb_value_varchar(&dres, 0, r);
            char *ty = duckdb_value_varchar(&dres, 1, r);
            if (nm && ty) w.rec("dtype", {}, {nm, ty});
            if (nm) duckdb_free(nm);
            if (ty) duckdb_free(ty);
        }
        duckdb_destroy_result(&dres);
    }
    write_var_records(w, ctx);
    if (!w.close(&err)) {
        cry(err);
        return kRcEngine;
    }

    std::string ngroups = "1", nfiles = "1";
    s.query_scalar("SELECT count(*) FROM (SELECT DISTINCT file_name, row_group_id "
                   "FROM parquet_metadata(" + src.paths_sql + "))",
                   &ngroups, &err);
    s.query_scalar("SELECT count(DISTINCT file_name) FROM parquet_metadata(" +
                       src.paths_sql + ")",
                   &nfiles, &err);

    save_local("_parqit_n", std::to_string(ctx.nrows));
    save_local("_parqit_k", std::to_string(ctx.active.size()));
    save_local("_parqit_row_groups", ngroups);
    save_local("_parqit_n_files", nfiles);
    save_local("_parqit_has_meta", ctx.meta.present ? "1" : "0");
    return 0;
}

/* ======================================================= save_data ===== */

namespace {

struct SaveVar {
    std::string name;
    std::string source;
    StType st = StType::Double;
    int str_bytes = 0;
    std::string fmt;
    FmtClass fcls = FmtClass::None;
    std::string varlab, vallab;
};

bool build_save_kv_metadata(const std::vector<SaveVar> &vars, const json &req,
                            const std::string &dtalabel, std::string *kv,
                            std::string *err) {
    json schema;
    schema["version"] = 1;
    json jvars = json::array();
    for (const auto &v : vars) {
        json jv;
        jv["name"] = v.name;
        jv["src"] = v.name;
        jv["type"] = parqit::sttype_code(v.st, v.str_bytes);
        jv["fmt"] = v.fmt;
        jv["varlab"] = v.varlab;
        jv["vallab"] = v.vallab;
        jvars.push_back(jv);
    }
    schema["vars"] = jvars;
    schema["sortedby"] = json::array();
    std::vector<std::string> sortedby;
    if (!parqit::req_text_list(req, "sortedby", &sortedby, err, false))
        return false;
    if (!sortedby.empty()) {
        std::set<std::string> live;
        for (const auto &v : vars) live.insert(v.name);
        for (const auto &key : sortedby)
            if (live.count(key))
                schema["sortedby"].push_back(key);
            else
                break;
    }

    json vallabs = json::object();
    if (req.contains("vallabs") && req["vallabs"].is_array()) {
        for (const auto &jl : req["vallabs"]) {
            std::string lname;
            if (!parqit::req_text(jl, "name", &lname, err)) return false;
            json entries = json::array();
            if (jl.contains("entries") && jl["entries"].is_array()) {
                for (const auto &e : jl["entries"]) {
                    if (!e.is_array() || e.size() != 2) continue;
                    std::string txt;
                    if (!e[1].is_string() ||
                        !parqit::hex_decode(e[1].get<std::string>(), txt))
                        continue;
                    entries.push_back(json::array({e[0].get<std::string>(), txt}));
                }
            }
            vallabs[lname] = {{"entries", entries}};
        }
    }
    json chars = json::object();
    if (req.contains("chars") && req["chars"].is_array()) {
        for (const auto &c : req["chars"]) {
            if (!c.is_array() || c.size() != 3) continue;
            std::string tgt, nm, val;
            if (!c[0].is_string() || !parqit::hex_decode(c[0].get<std::string>(), tgt))
                continue;
            if (!c[1].is_string() || !parqit::hex_decode(c[1].get<std::string>(), nm))
                continue;
            if (!c[2].is_string() || !parqit::hex_decode(c[2].get<std::string>(), val))
                continue;
            chars[tgt][nm] = val;
        }
    }

    *kv = "KV_METADATA {'parqit.schema': " + quote_literal(schema.dump()) +
          ", 'parqit.vallabs': " + quote_literal(vallabs.dump()) +
          ", 'parqit.chars': " + quote_literal(chars.dump()) +
          ", 'parqit.dtalabel': " + quote_literal(json(dtalabel).dump()) +
          "}";
    return true;
}

/* ---- write-conversion machinery shared by the staged (temp-table) and the
 * Arrow-scan save paths, so both convert a cell identically. --------------- */

enum WKind {
    WStr, WStrL, WDate, WTs, WTC, WPeriod, WI8, WI16, WI32, WFloat, WDouble
};

inline int wkind_width(WKind w) {
    switch (w) {
    case WI8: return 1;
    case WI16: return 2;
    case WDate:
    case WPeriod:
    case WI32:
    case WFloat: return 4;
    case WStr:
    case WStrL: return 0;
    default: return 8; /* WTs, WTC, WDouble */
    }
}

/* Strict, well-formed UTF-8 validation (rejects overlong forms, UTF-16
 * surrogates and code points > U+10FFFF — the same boundary conditions as the
 * engine's utf8_lossy walker). Arrow/DuckDB/Parquet VARCHAR must be valid UTF-8:
 * a Stata string carrying raw Latin-1/legacy bytes would otherwise be written
 * verbatim into a UTF-8-typed column, producing a Parquet file that no reader —
 * parqit included — can decode (the Arrow path) or that silently nulls the cell
 * (the staged path). We refuse loudly at the offending cell instead. */
inline bool parqit_is_valid_utf8(const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n;) {
        unsigned char c = p[i];
        if (c < 0x80) { i += 1; continue; }
        size_t extra;
        unsigned char low = 0x80, high = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) { extra = 1; }
        else if (c == 0xE0) { extra = 2; low = 0xA0; }
        else if (c >= 0xE1 && c <= 0xEC) { extra = 2; }
        else if (c == 0xED) { extra = 2; high = 0x9F; }
        else if (c >= 0xEE && c <= 0xEF) { extra = 2; }
        else if (c == 0xF0) { extra = 3; low = 0x90; }
        else if (c >= 0xF1 && c <= 0xF3) { extra = 3; }
        else if (c == 0xF4) { extra = 3; high = 0x8F; }
        else return false; /* 0x80-0xC1, 0xF5-0xFF: not a valid lead byte */
        if (i + extra >= n) return false;
        if (p[i + 1] < low || p[i + 1] > high) return false;
        for (size_t kk = 2; kk <= extra; kk++)
            if (p[i + kk] < 0x80 || p[i + kk] > 0xBF) return false;
        i += extra + 1;
    }
    return true;
}

WKind wkind_for(const SaveVar &v) {
    if (v.st == StType::StrL) return WStrL;
    if (v.st == StType::Str) return WStr;
    switch (v.fcls) {
    case FmtClass::Td: return WDate;
    case FmtClass::Tc: return WTs;
    case FmtClass::TC: return WTC;
    case FmtClass::Tm:
    case FmtClass::Tq:
    case FmtClass::Th:
    case FmtClass::Tw:
    case FmtClass::Ty:
    case FmtClass::Tb: return WPeriod;
    default:
        switch (v.st) {
        case StType::Byte: return WI8;
        case StType::Int: return WI16;
        case StType::Long: return WI32;
        case StType::Float: return WFloat;
        default: return WDouble;
        }
    }
}

/* Native Stata's round(x) resolves exact half ties toward +infinity. C/C++
 * nearbyint() instead uses the process rounding mode (normally ties-to-even),
 * while DuckDB round() uses half-away-from-zero. Temporal values are integer
 * day/ms/period counts, so use one explicit rule in both physical writers and
 * in compile_for_save(): floor(x + .5). */
inline double stata_round_unit(double d) { return std::floor(d + 0.5); }

/* Convert one NON-missing Stata value `d` for column kind `wk` and write the
 * physical value into dest[idx]. Sets *frac for any rounded temporal value.
 * Returns 0, or kRcUsage with *err on a value outside the on-disk range. Both
 * the Arrow and staged writers call this helper, so they stay byte-identical. */
int convert_save_numeric(WKind wk, double d, void *dest, idx_t idx, bool *frac,
                         const std::string &vname, long long j,
                         std::string *err) {
    (void)j;
    switch (wk) {
    case WDate: {
        double r = stata_round_unit(d);
        if (r != d) *frac = true;
        double disk = r - static_cast<double>(parqit::kEpochShiftDays);
        if (disk < -2147483648.0 || disk > 2147483647.0) {
            *err = "parqit save: " + vname +
                   " has a %td date value out of range for the on-disk 32-bit "
                   "day count";
            return kRcUsage;
        }
        static_cast<int32_t *>(dest)[idx] = static_cast<int32_t>(
            static_cast<long long>(r) - parqit::kEpochShiftDays);
        return 0;
    }
    case WTs: {
        double r = stata_round_unit(d);
        if (r != d) *frac = true;
        double ms = r - static_cast<double>(parqit::kEpochShiftMs);
        /* DT-001: bound the microsecond product directly against +-2^63 (which
         * is exactly representable as a double, 0x1p63). The older
         * `ms > 9.22..e15` ms-literal rounds UP one ulp, leaving a one-ulp hole
         * at the int64 ceiling where llround(ms*1000) reaches 2^63 (undefined ->
         * INT64_MIN) and is written with rc 0. Strict bounds also keep the
         * result off the INT64_MIN sentinel the contract forbids on disk. */
        double us = ms * 1000.0;
        if (!(us > -0x1p63 && us < 0x1p63)) {
            *err = "parqit save: " + vname +
                   " has a %tc datetime value out of range for the on-disk "
                   "64-bit microsecond count";
            return kRcUsage;
        }
        static_cast<int64_t *>(dest)[idx] =
            static_cast<int64_t>(std::llround(us));
        return 0;
    }
    case WTC: {
        double r = stata_round_unit(d);
        if (r != d) *frac = true;
        if (r < -9.007199254740992e15 || r > 9.007199254740992e15) {
            *err = "parqit save: " + vname +
                   " has a %tC value out of the exactly-representable range for "
                   "the on-disk 64-bit count";
            return kRcUsage;
        }
        static_cast<int64_t *>(dest)[idx] = static_cast<int64_t>(r);
        return 0;
    }
    case WPeriod: {
        double r = stata_round_unit(d);
        if (r != d) *frac = true;
        if (r < -2147483648.0 || r > 2147483647.0) {
            *err = "parqit save: " + vname +
                   " has a period (%tm/%tq/…) value out of range for the on-disk "
                   "32-bit period count";
            return kRcUsage;
        }
        static_cast<int32_t *>(dest)[idx] = static_cast<int32_t>(r);
        return 0;
    }
    case WI8:
        static_cast<int8_t *>(dest)[idx] = static_cast<int8_t>(d);
        return 0;
    case WI16:
        static_cast<int16_t *>(dest)[idx] = static_cast<int16_t>(d);
        return 0;
    case WI32:
        static_cast<int32_t *>(dest)[idx] = static_cast<int32_t>(d);
        return 0;
    case WFloat:
        static_cast<float *>(dest)[idx] = static_cast<float>(d);
        return 0;
    default:
        static_cast<double *>(dest)[idx] = d;
        return 0;
    }
}

inline const char *arrow_format_for(WKind w) {
    switch (w) {
    case WStr:
    case WStrL: return "u";   /* utf8 */
    case WDate: return "tdD"; /* date32 (days) */
    case WTs: return "tsu:";  /* timestamp micros, no tz */
    case WTC: return "l";     /* int64 */
    case WPeriod: return "i"; /* int32 */
    case WI8: return "c";
    case WI16: return "s";
    case WI32: return "i";
    case WFloat: return "f";
    default: return "g"; /* float64 */
    }
}

void noop_arrow_schema_release(ArrowSchema *s) { s->release = nullptr; }
void noop_arrow_array_release(ArrowArray *a) { a->release = nullptr; }

/* Assemble the whole result as one zero-copy Arrow struct array and COPY it
 * straight to Parquet via a registered Arrow scan — the "assemble each column
 * once, then write" model (as Polars/pq do). This skips the DuckDB temp-table
 * round-trip that the staged path pays, which a microbenchmark showed costs
 * ~2× on the write assembly for both strings and numerics. Conversions go
 * through convert_save_numeric, so the staged rows are byte-identical.
 *
 * Uses duckdb_arrow_array_scan (deprecated in DuckDB but present in the pinned
 * 1.5.x; pinned by ASSUMPTIONS #48 and the engine-capability test). Full-range
 * only (save_data never has if/in), so the Arrow length equals the row count
 * and the buffer index equals the obs index. */
ST_retcode save_assemble_arrow(
    Session &s, const std::vector<SaveVar> &vars, const std::vector<WKind> &wk,
    const std::string &dest, bool replace, const std::string &compression,
    long long comp_level, const std::vector<std::string> &partition_by,
    long long chunk, const std::string &kv, long long *written_out,
    long long *appended_out, std::vector<std::string> &frac_warned,
    std::vector<std::string> &ext_missing, std::string *err) {
    constexpr int64_t kArrowFlagNullable = 2;
    const int k = static_cast<int>(vars.size());
    const ST_int in1 = SF_in1();
    const ST_int in2 = SF_in2();
    /* Enforced, not assumed: buffers below index by `obs - in1`, so a
     * restricted range or if-filtered row would leave a phantom 0 and a
     * non-monotonic string offset (silent corruption). The staged fallback
     * handles restrictions; this path refuses loudly (SAVE-RANGE-1). */
    if (in1 != 1 || in2 != SF_nobs()) {
        *err = "internal: direct save requires the full observation range";
        return kRcEngine;
    }
    const long long N = static_cast<long long>(in2) - in1 + 1;

    struct ACol {
        bool is_str = false;
        int width = 0;
        std::vector<uint8_t> data;  /* numeric: N*width */
        std::vector<uint8_t> valid; /* validity bitmap, all-valid init */
        long long null_count = 0;
        std::vector<int32_t> offs;  /* strings: N+1 offsets */
        std::vector<char> bytes;    /* strings: contiguous payload */
    };
    std::vector<ACol> cols(k);
    const long long vbytes = (N + 7) / 8;
    for (int i = 0; i < k; i++) {
        if (wk[i] == WStr || wk[i] == WStrL) {
            cols[i].is_str = true;
            cols[i].offs.assign(static_cast<size_t>(N) + 1, 0);
        } else {
            cols[i].width = wkind_width(wk[i]);
            cols[i].data.assign(static_cast<size_t>(N) * cols[i].width, 0);
            cols[i].valid.assign(static_cast<size_t>(vbytes), 0xFF);
        }
    }

    std::vector<bool> warned_frac(k, false), warned_ext(k, false);
    std::string strbuf(8192, '\0');
    ST_retcode rc = 0;
    for (ST_int j = in1; j <= in2 && rc == 0; j++) {
        if (!SF_ifobs(j)) continue;
        const long long idx = static_cast<long long>(j) - in1;
        for (int i = 0; i < k && rc == 0; i++) {
            const SaveVar &v = vars[i];
            ACol &c = cols[i];
            if (c.is_str) {
                if (SF_var_is_binary(i + 1, j)) {
                    *err = "parqit save: " + v.name + "[" + std::to_string(j) +
                           "] contains binary data (binary strLs are not "
                           "supported; see help parqit)";
                    rc = kRcUsage;
                    break;
                }
                ST_int len = SF_sdatalen(i + 1, j);
                if (len < 0) len = 0;
                if (static_cast<size_t>(len) + 1 > strbuf.size())
                    strbuf.resize(static_cast<size_t>(len) + 1);
                if (wk[i] == WStrL) {
                    SF_strldata(i + 1, j, &strbuf[0],
                                static_cast<ST_int>(strbuf.size()));
                } else if (SF_sdata(i + 1, j, &strbuf[0]) != 0) {
                    *err = "parqit save: could not read " + v.name + "[" +
                           std::to_string(j) + "]";
                    rc = kRcEngine;
                    break;
                }
                if (!parqit_is_valid_utf8(
                        reinterpret_cast<const unsigned char *>(strbuf.data()),
                        static_cast<size_t>(len))) {
                    *err = "parqit save: " + v.name + "[" + std::to_string(j) +
                           "] contains invalid UTF-8 (Parquet/Arrow strings must "
                           "be valid UTF-8; if the data is Latin-1/legacy, run "
                           "-unicode translate- first; see help parqit)";
                    rc = kRcUsage;
                    break;
                }
                c.bytes.insert(c.bytes.end(), strbuf.data(),
                               strbuf.data() + len);
                c.offs[static_cast<size_t>(idx) + 1] =
                    static_cast<int32_t>(c.bytes.size());
            } else {
                double d = 0;
                if (SF_vdata(i + 1, j, &d) != 0) {
                    *err = "parqit save: could not read " + v.name + "[" +
                           std::to_string(j) + "]";
                    rc = kRcEngine;
                    break;
                }
                if (SF_is_missing(d)) {
                    if (d > SV_missval && !warned_ext[i]) {
                        warned_ext[i] = true;
                        ext_missing.push_back(v.name);
                    }
                    c.valid[static_cast<size_t>(idx) >> 3] &=
                        static_cast<uint8_t>(~(1u << (idx & 7)));
                    c.null_count++;
                } else {
                    bool fd = false;
                    int crc = convert_save_numeric(wk[i], d, c.data.data(),
                                                   static_cast<idx_t>(idx), &fd,
                                                   v.name, j, err);
                    if (fd && !warned_frac[i]) {
                        warned_frac[i] = true;
                        frac_warned.push_back(v.name);
                    }
                    if (crc != 0) {
                        rc = static_cast<ST_retcode>(crc);
                        break;
                    }
                }
            }
        }
        if (rc != 0) break;
        if (SW_stopflag) {
            *err = "parqit save: interrupted";
            rc = 1;
            break;
        }
    }
    if (rc != 0) return rc;

    for (int i = 0; i < k; i++)
        if (cols[i].is_str && cols[i].bytes.size() > 2147483647ULL) {
            *err = "parqit save: string column " + vars[i].name +
                   " exceeds 2 GiB on disk (not supported by the Arrow writer)";
            return kRcUsage;
        }

    /* Wrap the buffers as an Arrow struct array (record batch). All structs and
     * buffer-pointer arrays are locals that outlive the synchronous COPY. */
    std::vector<ArrowSchema> child_s(k);
    std::vector<ArrowSchema *> child_s_ptr(k);
    std::vector<ArrowArray> child_a(k);
    std::vector<ArrowArray *> child_a_ptr(k);
    std::vector<std::vector<const void *>> bufp(k);
    for (int i = 0; i < k; i++) {
        child_s[i] = ArrowSchema{};
        child_s[i].format = arrow_format_for(wk[i]);
        child_s[i].name = vars[i].name.c_str();
        child_s[i].flags = kArrowFlagNullable;
        child_s[i].release = noop_arrow_schema_release;
        child_s_ptr[i] = &child_s[i];

        child_a[i] = ArrowArray{};
        child_a[i].length = N;
        child_a[i].offset = 0;
        child_a[i].release = noop_arrow_array_release;
        if (cols[i].is_str) {
            child_a[i].null_count = 0;
            child_a[i].n_buffers = 3;
            bufp[i] = {nullptr, cols[i].offs.data(), cols[i].bytes.data()};
        } else {
            child_a[i].null_count = cols[i].null_count;
            child_a[i].n_buffers = 2;
            const void *validity =
                cols[i].null_count ? cols[i].valid.data() : nullptr;
            bufp[i] = {validity, cols[i].data.data()};
        }
        child_a[i].buffers = bufp[i].data();
        child_a_ptr[i] = &child_a[i];
    }

    ArrowSchema struct_s = ArrowSchema{};
    struct_s.format = "+s";
    struct_s.name = "";
    struct_s.n_children = k;
    struct_s.children = child_s_ptr.data();
    struct_s.release = noop_arrow_schema_release;

    const void *struct_bufs[1] = {nullptr};
    ArrowArray struct_a = ArrowArray{};
    struct_a.length = N;
    struct_a.null_count = 0;
    struct_a.offset = 0;
    struct_a.n_buffers = 1;
    struct_a.buffers = struct_bufs;
    struct_a.n_children = k;
    struct_a.children = child_a_ptr.data();
    struct_a.release = noop_arrow_array_release;

    static long long arrow_counter = 0;
    const std::string view = "_parqit_arrow_" + std::to_string(++arrow_counter);
    duckdb_arrow_stream stream = nullptr;
    duckdb_state st = duckdb_arrow_array_scan(
        s.con(), view.c_str(), reinterpret_cast<duckdb_arrow_schema>(&struct_s),
        reinterpret_cast<duckdb_arrow_array>(&struct_a), &stream);
    if (st != DuckDBSuccess) {
        if (stream) duckdb_destroy_arrow_stream(&stream);
        *err = "parqit save: could not register the Arrow result for writing";
        return kRcEngine;
    }

    long long written = 0;
    ST_retcode crc = copy_out_parquet(s, "SELECT * FROM " + quote_ident(view),
                                      dest, replace, compression, comp_level,
                                      partition_by, chunk, kv, &written, err);
    if (stream) duckdb_destroy_arrow_stream(&stream);
    {
        std::string e2;
        s.exec("DROP VIEW IF EXISTS " + quote_ident(view), &e2);
    }
    if (crc != 0) return crc;

    *written_out = written;
    *appended_out = N;
    return 0;
}

} // namespace

ST_retcode cmd_save_data(const std::vector<std::string> &args) {
    std::string reqpath, err;
    if (args.size() < 2 || !parqit::hex_decode(args[1], reqpath)) {
        cry("parqit save: malformed request path");
        return kRcUsage;
    }
    json req;
    if (!parqit::load_request(reqpath, &req, &err)) {
        cry(err);
        return kRcUsage;
    }
    std::string dest, tmpdir, dtalabel;
    if (!parqit::req_text(req, "dest", &dest, &err) ||
        !parqit::req_text(req, "tmpdir", &tmpdir, &err) ||
        !parqit::req_text(req, "dtalabel", &dtalabel, &err, false)) {
        cry(err);
        return kRcUsage;
    }
    bool replace = req.value("replace", false);
    long long comp_level = req.value("compression_level", static_cast<long long>(-1));
    long long chunk = req.value("chunk", static_cast<long long>(-1));
    if (chunk != -1 && chunk <= 0) {
        cry("parqit save: chunk() must be a positive number of rows per row group");
        return kRcUsage;
    }
    std::string compression;
    if (!parqit::req_text(req, "compression", &compression, &err, false)) {
        cry(err);
        return kRcUsage;
    }
    std::vector<std::string> partition_by;
    if (!parqit::req_text_list(req, "partition_by", &partition_by, &err, false)) {
        cry(err);
        return kRcUsage;
    }

    if (!req.contains("vars") || !req["vars"].is_array() || req["vars"].empty()) {
        cry("parqit save: empty variable manifest");
        return kRcUsage;
    }
    std::vector<SaveVar> vars;
    for (const auto &jv : req["vars"]) {
        SaveVar v;
        std::string code;
        if (!parqit::req_text(jv, "name", &v.name, &err) ||
            !parqit::req_text(jv, "type", &code, &err) ||
            !parqit::req_text(jv, "fmt", &v.fmt, &err, false) ||
            !parqit::req_text(jv, "varlab", &v.varlab, &err, false) ||
            !parqit::req_text(jv, "vallab", &v.vallab, &err, false) ||
            !parqit::req_text(jv, "source", &v.source, &err, false)) {
            cry(err);
            return kRcUsage;
        }
        if (v.source.empty()) v.source = v.name;
        if (!parqit::sttype_parse(code, &v.st, &v.str_bytes)) {
            cry("parqit save: bad type code '" + code + "' for " + v.name);
            return kRcUsage;
        }
        v.fcls = parqit::classify_format(v.fmt);
        vars.push_back(std::move(v));
    }
    const int k = static_cast<int>(vars.size());

    if (SF_nvars() != k) {
        cry("parqit save: internal manifest mismatch (variable count)");
        return kRcMismatch;
    }
    for (int i = 0; i < k; i++) {
        bool is_str = (vars[i].st == StType::Str || vars[i].st == StType::StrL);
        if (static_cast<bool>(SF_var_is_string(i + 1)) != is_str) {
            cry("parqit save: internal manifest mismatch (type of " + vars[i].name + ")");
            return kRcMismatch;
        }
    }
    for (const auto &pv : partition_by) {
        bool found = false;
        for (const auto &v : vars) found = found || v.name == pv;
        if (!found) {
            cry("parqit save: partition_by(" + pv + ") is not a variable being saved");
            return kRcVarNotFound;
        }
    }

    Session &s = Session::instance();
    s.set_default_temp_dir(tmpdir + parqit::spill_suffix());
    if (!s.ensure_open()) {
        cry("parqit save: " + s.last_error());
        return kRcEngine;
    }

    std::vector<std::string> frac_warned, ext_missing;
    long long appended = 0;
    long long written = 0;
    ST_retcode rc = 0;

    std::vector<WKind> wk(k);
    for (int i = 0; i < k; i++) wk[i] = wkind_for(vars[i]);

    std::string kv;
    if (!build_save_kv_metadata(vars, req, dtalabel, &kv, &err)) {
        cry(err);
        return kRcUsage;
    }

    /* Default write path: assemble each column once as an Arrow array and COPY
     * straight from a registered Arrow scan — measured ~2× faster on the write
     * assembly than staging through a DuckDB temp table (the temp-table
     * VARCHAR/data round-trip is the cost), for both strings and numerics.
     * PARQIT_SAVE_NOARROW forces the staged path below, which avoids the
     * deprecated Arrow ingestion API and is the safety fallback. */
    if (!std::getenv("PARQIT_SAVE_NOARROW")) {
        rc = save_assemble_arrow(s, vars, wk, dest, replace, compression,
                                 comp_level, partition_by, chunk, kv, &written,
                                 &appended, frac_warned, ext_missing, &err);
        if (rc != 0) {
            cry(err);
            return rc;
        }
    } else {
    /* staging temp table */
    static long long stage_counter = 0;
    const std::string stage = "_parqit_stage_" + std::to_string(++stage_counter);
    std::string cols;
    for (int i = 0; i < k; i++) {
        if (i) cols += ", ";
        cols += quote_ident(vars[i].name) + " " +
                parqit::duck_type_for(vars[i].st, vars[i].fcls);
    }
    if (!s.exec("CREATE TEMP TABLE " + quote_ident(stage) + " (" + cols + ")", &err)) {
        cry("parqit save: " + err);
        return kRcEngine;
    }
    struct StageGuard {
        Session &s;
        std::string name;
        ~StageGuard() {
            std::string e;
            s.exec("DROP TABLE IF EXISTS " + quote_ident(name), &e);
        }
    } guard{s, stage};

    duckdb_appender app = nullptr;
    if (duckdb_appender_create_ext(s.con(), "temp", "main", stage.c_str(), &app) !=
        DuckDBSuccess) {
        std::string m = app ? duckdb_appender_error(app) : "appender create failed";
        if (app) duckdb_appender_destroy(&app);
        cry("parqit save: " + m);
        return kRcEngine;
    }

    std::vector<bool> warned_frac(k, false), warned_ext(k, false);
    std::string strbuf(8192, '\0');

    /* Bulk column-vector transfer: fill DuckDB data chunks (2048 rows at a
     * time) and append them whole, instead of one duckdb_append_* per cell.
     * Every type / date / period / missing conversion is byte-identical to the
     * Arrow path; only the destination (a vector slot vs an appender) differs. */
    auto wkind_type = [](WKind w) -> duckdb_type {
        switch (w) {
        case WStr:
        case WStrL: return DUCKDB_TYPE_VARCHAR;
        case WDate: return DUCKDB_TYPE_DATE;
        case WTs: return DUCKDB_TYPE_TIMESTAMP;
        case WTC: return DUCKDB_TYPE_BIGINT;
        case WPeriod: return DUCKDB_TYPE_INTEGER;
        case WI8: return DUCKDB_TYPE_TINYINT;
        case WI16: return DUCKDB_TYPE_SMALLINT;
        case WI32: return DUCKDB_TYPE_INTEGER;
        case WFloat: return DUCKDB_TYPE_FLOAT;
        default: return DUCKDB_TYPE_DOUBLE;
        }
    };
    std::vector<duckdb_logical_type> ltypes(k, nullptr);
    for (int i = 0; i < k; i++)
        ltypes[i] = duckdb_create_logical_type(wkind_type(wk[i]));

    duckdb_data_chunk dchunk = duckdb_create_data_chunk(ltypes.data(),
                                                       static_cast<idx_t>(k));
    const idx_t CAP = duckdb_vector_size();
    idx_t filln = 0;
    std::vector<void *> vdata(k, nullptr);
    auto begin_chunk = [&]() {
        for (int i = 0; i < k; i++) {
            if (wk[i] == WStr || wk[i] == WStrL) continue;
            vdata[i] = duckdb_vector_get_data(
                duckdb_data_chunk_get_vector(dchunk, static_cast<idx_t>(i)));
        }
    };
    auto flush_chunk = [&]() -> bool {
        if (filln == 0) return true;
        duckdb_data_chunk_set_size(dchunk, filln);
        if (duckdb_append_data_chunk(app, dchunk) != DuckDBSuccess) {
            const char *m = duckdb_appender_error(app);
            cry("parqit save: append failed: " + std::string(m ? m : "unknown"));
            return false;
        }
        duckdb_data_chunk_reset(dchunk);
        filln = 0;
        return true;
    };
    begin_chunk();

    for (ST_int j = SF_in1(); j <= SF_in2() && rc == 0; j++) {
        if (!SF_ifobs(j)) continue;
        for (int i = 0; i < k && rc == 0; i++) {
            const SaveVar &v = vars[i];
            if (wk[i] == WStr || wk[i] == WStrL) {
                if (SF_var_is_binary(i + 1, j)) {
                    cry("parqit save: " + v.name + "[" + std::to_string(j) +
                        "] contains binary data (binary strLs are not supported; "
                        "see help parqit)");
                    rc = kRcUsage;
                    break;
                }
                ST_int len = SF_sdatalen(i + 1, j);
                if (len < 0) len = 0;
                if (static_cast<size_t>(len) + 1 > strbuf.size())
                    strbuf.resize(static_cast<size_t>(len) + 1);
                if (wk[i] == WStrL) {
                    SF_strldata(i + 1, j, &strbuf[0], static_cast<ST_int>(strbuf.size()));
                } else if (SF_sdata(i + 1, j, &strbuf[0]) != 0) {
                    cry("parqit save: could not read " + v.name + "[" +
                        std::to_string(j) + "]");
                    rc = kRcEngine;
                    break;
                }
                if (!parqit_is_valid_utf8(
                        reinterpret_cast<const unsigned char *>(strbuf.data()),
                        static_cast<size_t>(len))) {
                    cry("parqit save: " + v.name + "[" + std::to_string(j) +
                        "] contains invalid UTF-8 (Parquet/Arrow strings must be "
                        "valid UTF-8; if the data is Latin-1/legacy, run "
                        "-unicode translate- first; see help parqit)");
                    rc = kRcUsage;
                    break;
                }
                duckdb_vector_assign_string_element_len(
                    duckdb_data_chunk_get_vector(dchunk, static_cast<idx_t>(i)),
                    filln, strbuf.data(), static_cast<idx_t>(len));
            } else {
                double d = 0;
                if (SF_vdata(i + 1, j, &d) != 0) {
                    cry("parqit save: could not read " + v.name + "[" +
                        std::to_string(j) + "]");
                    rc = kRcEngine;
                    break;
                }
                if (SF_is_missing(d)) {
                    if (d > SV_missval && !warned_ext[i]) {
                        warned_ext[i] = true;
                        ext_missing.push_back(v.name);
                    }
                    duckdb_vector vec =
                        duckdb_data_chunk_get_vector(dchunk, static_cast<idx_t>(i));
                    duckdb_vector_ensure_validity_writable(vec);
                    duckdb_validity_set_row_invalid(duckdb_vector_get_validity(vec),
                                                    filln);
                } else {
                    bool fd = false;
                    int crc = convert_save_numeric(wk[i], d, vdata[i], filln,
                                                   &fd, v.name, j, &err);
                    if (fd && !warned_frac[i]) {
                        warned_frac[i] = true;
                        frac_warned.push_back(v.name);
                    }
                    if (crc != 0) {
                        cry(err);
                        rc = static_cast<ST_retcode>(crc);
                        break;
                    }
                }
            }
        }
        if (rc != 0) break;
        filln++;
        appended++;
        if (filln == CAP) {
            if (!flush_chunk()) { rc = kRcEngine; break; }
            begin_chunk();
        }
        if (SW_stopflag) {
            cry("parqit save: interrupted");
            rc = 1;
            break;
        }
    }
    if (rc == 0 && !flush_chunk()) rc = kRcEngine;

    duckdb_destroy_data_chunk(&dchunk);
    for (auto &lt : ltypes)
        if (lt) duckdb_destroy_logical_type(&lt);
    if (duckdb_appender_destroy(&app) != DuckDBSuccess && rc == 0) {
        cry("parqit save: failed to flush staged rows");
        rc = kRcEngine;
    }
    if (rc != 0) return rc;

    ST_retcode crc = copy_out_parquet(s, "SELECT * FROM " + quote_ident(stage),
                                      dest, replace, compression, comp_level,
                                      partition_by, chunk, kv, &written, &err);
    if (crc != 0) {
        cry("parqit save: " + err);
        return crc;
    }
    if (written != appended) {
        cry("parqit save: engine wrote " + std::to_string(written) +
            " rows but " + std::to_string(appended) + " were staged");
        return kRcEngine;
    }
    } /* end staged (PARQIT_SAVE_NOARROW) fallback */

    std::error_code eca;
    std::string abs = std::filesystem::absolute(dest, eca).string();
    if (eca) abs = dest;

    std::string extlist, fraclist;
    for (size_t i = 0; i < ext_missing.size(); i++)
        extlist += (i ? " " : "") + ext_missing[i];
    for (size_t i = 0; i < frac_warned.size(); i++)
        fraclist += (i ? " " : "") + frac_warned[i];
    save_local("_parqit_written_n", std::to_string(appended));
    save_local("_parqit_written_k", std::to_string(k));
    save_local("_parqit_dest", parqit::hex_encode(abs));
    save_local("_parqit_ext_missing", extlist);
    save_local("_parqit_frac_dates", fraclist);
    return 0;
}

ST_retcode cmd_save_data_direct(const std::vector<std::string> &args) {
    save_local("_parqit_direct_done", "0");

    std::string reqpath, err;
    if (args.size() < 2 || !parqit::hex_decode(args[1], reqpath)) {
        cry("parqit save: malformed request path");
        return kRcUsage;
    }
    json req;
    if (!parqit::load_request(reqpath, &req, &err)) {
        cry(err);
        return kRcUsage;
    }

    std::string dest, tmpdir, dtalabel, source_file, expect_size, expect_mtime;
    if (!parqit::req_text(req, "dest", &dest, &err) ||
        !parqit::req_text(req, "tmpdir", &tmpdir, &err) ||
        !parqit::req_text(req, "dtalabel", &dtalabel, &err, false) ||
        !parqit::req_text(req, "source_file", &source_file, &err) ||
        !parqit::req_text(req, "source_size", &expect_size, &err) ||
        !parqit::req_text(req, "source_mtime", &expect_mtime, &err)) {
        cry(err);
        return kRcUsage;
    }
    bool replace = req.value("replace", false);
    long long comp_level = req.value("compression_level", static_cast<long long>(-1));
    long long chunk = req.value("chunk", static_cast<long long>(-1));
    long long expect_n = req.value("nobs", static_cast<long long>(-1));
    if (chunk != -1 && chunk <= 0) {
        cry("parqit save: chunk() must be a positive number of rows per row group");
        return kRcUsage;
    }
    std::string compression;
    if (!parqit::req_text(req, "compression", &compression, &err, false)) {
        cry(err);
        return kRcUsage;
    }
    std::vector<std::string> partition_by;
    if (!parqit::req_text_list(req, "partition_by", &partition_by, &err, false)) {
        cry(err);
        return kRcUsage;
    }
    if (!req.contains("vars") || !req["vars"].is_array() || req["vars"].empty()) {
        cry("parqit save: empty variable manifest");
        return kRcUsage;
    }

    std::vector<SaveVar> vars;
    for (const auto &jv : req["vars"]) {
        SaveVar v;
        std::string code;
        if (!parqit::req_text(jv, "name", &v.name, &err) ||
            !parqit::req_text(jv, "type", &code, &err) ||
            !parqit::req_text(jv, "fmt", &v.fmt, &err, false) ||
            !parqit::req_text(jv, "varlab", &v.varlab, &err, false) ||
            !parqit::req_text(jv, "vallab", &v.vallab, &err, false) ||
            !parqit::req_text(jv, "source", &v.source, &err, false)) {
            cry(err);
            return kRcUsage;
        }
        if (v.source.empty()) v.source = v.name;
        if (!parqit::sttype_parse(code, &v.st, &v.str_bytes)) {
            cry("parqit save: bad type code '" + code + "' for " + v.name);
            return kRcUsage;
        }
        v.fcls = parqit::classify_format(v.fmt);
        vars.push_back(std::move(v));
    }
    const int k = static_cast<int>(vars.size());
    if (SF_nvars() != k) {
        cry("parqit save: internal manifest mismatch (variable count)");
        return kRcMismatch;
    }
    auto direct_fmt_supported = [](FmtClass c) {
        switch (c) {
        case FmtClass::None:
        case FmtClass::Td:
        case FmtClass::TC:
        case FmtClass::Tm:
        case FmtClass::Tq:
        case FmtClass::Th:
        case FmtClass::Tw:
        case FmtClass::Ty:
        case FmtClass::Tb:
            return true;
        default:
            return false;
        }
    };
    for (int i = 0; i < k; i++) {
        bool is_str = (vars[i].st == StType::Str || vars[i].st == StType::StrL);
        if (static_cast<bool>(SF_var_is_string(i + 1)) != is_str) {
            cry("parqit save: internal manifest mismatch (type of " + vars[i].name + ")");
            return kRcMismatch;
        }
        /* If the file had pathological/sanitised/duplicate names, the source
         * identifier visible to DuckDB may differ from the Stata name. Keep
         * those on the fully general path until they have a dedicated proof. */
        if (vars[i].source != vars[i].name) return 0;
        if (!direct_fmt_supported(vars[i].fcls)) return 0;
    }
    /* DATA-004: the fast unchanged-source path used a C-string expression and
     * silently cut a binary strL at its first NUL, while both general writers
     * reject the same cell.  Apply the identical fail-loud policy before any
     * output transaction is started. */
    for (int i = 0; i < k; i++) {
        if (vars[i].st != StType::StrL) continue;
        for (ST_int j = 1; j <= SF_nobs(); j++) {
            if (SF_var_is_binary(i + 1, j)) {
                cry("parqit save: " + vars[i].name + "[" +
                    std::to_string(j) +
                    "] contains binary data (binary strLs are not supported; "
                    "the fast and general save paths both refuse it)");
                return kRcUsage;
            }
        }
    }
    for (const auto &pv : partition_by) {
        bool found = false;
        for (const auto &v : vars) found = found || v.name == pv;
        if (!found) {
            cry("parqit save: partition_by(" + pv + ") is not a variable being saved");
            return kRcVarNotFound;
        }
    }

    std::string abs, fsize, fmtime;
    if (!regular_file_fingerprint(source_file, &abs, &fsize, &fmtime)) return 0;
    if (abs != source_file || fsize != expect_size || fmtime != expect_mtime)
        return 0;

    Session &s = Session::instance();
    s.set_default_temp_dir(tmpdir + parqit::spill_suffix());
    if (!s.ensure_open()) {
        cry("parqit save: " + s.last_error());
        return kRcEngine;
    }
    if (expect_n >= 0) {
        std::string nstr;
        /* GLOB-1b: the fast path re-reads the source; escape glob metachars so a
         * sibling matching a bracket class can never be read in its place. */
        if (!s.query_scalar("SELECT count(*) FROM read_parquet(" +
                                quote_literal(glob_escape(source_file)) + ")",
                            &nstr, &err)) {
            cry("parqit save: " + err);
            return kRcEngine;
        }
        if (std::strtoll(nstr.c_str(), nullptr, 10) != expect_n) return 0;
    }

    char missbuf[64];
    std::snprintf(missbuf, sizeof(missbuf), "%.17g", SV_missval);
    const std::string miss = missbuf;
    auto direct_expr = [&](const SaveVar &v) -> std::string {
        const std::string ref = quote_ident(v.source);
        switch (v.fcls) {
        case FmtClass::Td:
            return "CAST(" + ref + " AS DATE)";
        case FmtClass::TC:
            return "CAST(" + ref + " AS BIGINT)";
        case FmtClass::Tm:
        case FmtClass::Tq:
        case FmtClass::Th:
        case FmtClass::Tw:
        case FmtClass::Ty:
        case FmtClass::Tb:
            return "CAST(" + ref + " AS INTEGER)";
        default:
            break;
        }
        if (v.st == StType::Str || v.st == StType::StrL) {
            const std::string sref = "CAST(" + ref + " AS VARCHAR)";
            const std::string nulpos = "instr(" + sref + ", chr(0))";
            return "coalesce(CASE WHEN " + nulpos + " > 0 THEN substring(" +
                   sref + ", 1, " + nulpos + " - 1) ELSE " + sref + " END, '')";
        }
        const std::string dtype = parqit::duck_type_for(v.st, v.fcls);
        if (v.st == StType::Float || v.st == StType::Double) {
            const std::string dref = "CAST(" + ref + " AS DOUBLE)";
            return "CASE WHEN " + ref + " IS NULL OR NOT isfinite(" + dref +
                   ") OR abs(" + dref + ") >= " + miss + " THEN NULL ELSE CAST(" +
                   ref + " AS " + dtype + ") END";
        }
        return "CAST(" + ref + " AS " + dtype + ")";
    };

    std::string sel;
    for (int i = 0; i < k; i++) {
        if (i) sel += ", ";
        sel += direct_expr(vars[i]) + " AS " + quote_ident(vars[i].name);
    }

    std::string kv;
    if (!build_save_kv_metadata(vars, req, dtalabel, &kv, &err)) {
        cry(err);
        return kRcUsage;
    }
    long long written = 0;
    ST_retcode crc =
        copy_out_parquet(s,
                         "SELECT " + sel + " FROM read_parquet(" +
                             quote_literal(glob_escape(source_file)) + ")",
                         dest, replace, compression, comp_level, partition_by,
                         chunk, kv, &written, &err);
    if (crc != 0) {
        cry("parqit save: " + err);
        return crc;
    }
    if (expect_n >= 0 && written != expect_n) {
        cry("parqit save: engine wrote " + std::to_string(written) +
            " rows but the unchanged in-memory source had " +
            std::to_string(expect_n));
        return kRcEngine;
    }

    std::error_code eca;
    std::string destabs = std::filesystem::absolute(dest, eca).string();
    if (eca) destabs = dest;
    save_local("_parqit_written_n", std::to_string(written));
    save_local("_parqit_written_k", std::to_string(k));
    save_local("_parqit_dest", parqit::hex_encode(destabs));
    save_local("_parqit_ext_missing", "");
    save_local("_parqit_frac_dates", "");
    save_local("_parqit_direct_done", "1");
    return 0;
}

} // namespace parqit_plugin
