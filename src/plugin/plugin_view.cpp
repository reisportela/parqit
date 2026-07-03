/* parqit — M2 lazy-view subcommands: open, verbs, show/explain/describe/count,
 * collect (direct for pure reads, otherwise materialise once into a spillable
 * temp table, then reuse the M1 prepare/fetch machinery), save (COPY the
 * compiled pipeline straight to parquet — Stata memory untouched). */
#include "plugin/plugin_view.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <utility>

#include "duckdb.h"

#include "engine/exprtrans.hpp"
#include "engine/hexcodec.hpp"
#include "engine/request.hpp"
#include "engine/sanitize.hpp"
#include "engine/session.hpp"
#include "engine/typemap.hpp"
#include "engine/view.hpp"
#include "plugin/plugin_io.hpp"

namespace parqit_plugin {

using parqit::json;
using parqit::quote_ident;
using parqit::quote_literal;
using parqit::Session;
using parqit::View;
using parqit::ViewCol;

namespace {

constexpr ST_retcode kRcUsage = 198;
constexpr ST_retcode kRcVarNotFound = 111;
constexpr ST_retcode kRcEngine = 920;

void cry(const std::string &s) {
    std::string line = s;
    line.push_back('\n');
    SF_error(const_cast<char *>(line.c_str()));
}
void save_local(const char *name, const std::string &v) {
    SF_macro_save(const_cast<char *>(name), const_cast<char *>(v.c_str()));
}

std::map<std::string, View> g_views;
std::string g_current = "default";
bool g_statamissing = false;
long long g_collect_counter = 0;

/* bridge files written by `parqit open _data` belong to their view: drop the
 * file when the view is closed or replaced so promotions never accumulate
 * in the temp dir (and never alias another view's backing data) */
std::map<std::string, std::string> g_owned_files;

void drop_owned(const std::string &view_name, const std::string &keep = "") {
    auto it = g_owned_files.find(view_name);
    if (it == g_owned_files.end()) return;
    if (it->second != keep) {
        std::error_code ec;
        std::filesystem::remove(it->second, ec); /* best effort */
    }
    g_owned_files.erase(it);
}

/* current view (creates a dead placeholder if absent; require_view guards
 * before any use) */
View &g_view_ref() {
    return g_views[g_current];
}
void close_current_view() {
    drop_owned(g_current);
    g_views.erase(g_current);
}

bool load_req(const std::vector<std::string> &args, json *req, std::string *err) {
    std::string reqpath;
    if (args.size() < 2 || !parqit::hex_decode(args[1], reqpath)) {
        *err = "parqit: malformed request path";
        return false;
    }
    return parqit::load_request(reqpath, req, err);
}

/* the exact integer-ms expression for a timestamp column (negative-safe
 * floor to ms; DuckDB // truncates, so use the positive-modulus form) */
std::string ts_ms_sql(const std::string &inner) {
    std::string us = "epoch_us(" + inner + ")";
    return "((" + us + " - (((" + us + ") % 1000 + 1000) % 1000)) / 1000 + " +
           std::to_string(parqit::kEpochShiftMs) + ")";
}

/* boundary cast for the lazy view: every column becomes a Stata-semantics
 * number (day/ms counts) or string; unsupported types are dropped loudly */
struct BoundaryCol {
    bool dropped = false;
    std::string drop_reason;
    std::string sql;  /* expression over the quoted source name */
    char kind = 'n';
    std::string fmt;
    std::string note;
};

/* The exact float/double -> Stata-missing guard the eager fill (plugin_io
 * fill_column) and the direct in-memory save path already apply: a value that is
 * SQL NULL, NaN, +-Inf, or whose magnitude collides with Stata's missing
 * sentinel (|x| >= SV_missval == 2^1023) cannot be stored as an ordinary Stata
 * number, so it becomes NULL (Stata missing). Applying it at the lazy boundary
 * (and defensively on save) gives lazy views the same missingness, ordering,
 * filtering, statistics, dedup, and saved-payload semantics as the eager
 * `use, clear` path (PQ-AUD-001). `inner` is the value kept when the guard does
 * not fire (the column itself at the boundary; the storage-cast value on save).
 * SV_missval is the live Stata sentinel, identical to the engine-side
 * kStataMissThreshold used by the expression translator. */
std::string float_missing_guard(const std::string &ref, const std::string &inner) {
    char missbuf[64];
    std::snprintf(missbuf, sizeof(missbuf), "%.17g", SV_missval);
    const std::string dref = "CAST(" + ref + " AS DOUBLE)";
    return "CASE WHEN " + ref + " IS NULL OR NOT isfinite(" + dref + ") OR abs(" +
           dref + ") >= " + missbuf + " THEN NULL ELSE " + inner + " END";
}

BoundaryCol boundary_for(const std::string &name, duckdb_logical_type lt) {
    BoundaryCol b;
    const std::string ref = quote_ident(name);
    switch (duckdb_get_type_id(lt)) {
    case DUCKDB_TYPE_BOOLEAN: b.sql = "CAST(" + ref + " AS TINYINT)"; break;
    case DUCKDB_TYPE_TINYINT:
    case DUCKDB_TYPE_SMALLINT:
    case DUCKDB_TYPE_INTEGER:
    case DUCKDB_TYPE_BIGINT:
        b.sql = ref;
        break;
    case DUCKDB_TYPE_FLOAT:
    case DUCKDB_TYPE_DOUBLE:
        /* PQ-AUD-001: a foreign Parquet FLOAT/DOUBLE can carry NaN/+-Inf or a
         * magnitude Stata reads as missing. Normalize at the boundary so every
         * lazy verb sees Stata missing, matching the eager fill (which the
         * verify suite uses as the oracle). Integer columns above cannot hold an
         * IEEE special, so they pay nothing. */
        b.sql = float_missing_guard(ref, ref);
        break;
    case DUCKDB_TYPE_UTINYINT: b.sql = "CAST(" + ref + " AS SMALLINT)"; break;
    case DUCKDB_TYPE_USMALLINT: b.sql = "CAST(" + ref + " AS INTEGER)"; break;
    case DUCKDB_TYPE_UINTEGER: b.sql = "CAST(" + ref + " AS BIGINT)"; break;
    case DUCKDB_TYPE_UBIGINT:
    case DUCKDB_TYPE_HUGEINT:
    case DUCKDB_TYPE_UHUGEINT:
        b.sql = "CAST(" + ref + " AS DOUBLE)";
        b.note = "values beyond 2^53 round to the nearest double";
        break;
    case DUCKDB_TYPE_DECIMAL:
        b.sql = "CAST(" + ref + " AS DOUBLE)";
        b.note = "decimal converted to double";
        break;
    case DUCKDB_TYPE_DATE:
        b.sql = "(" + ref + " - DATE '1960-01-01')";
        b.fmt = "%td";
        break;
    case DUCKDB_TYPE_TIMESTAMP:
        b.sql = ts_ms_sql(ref);
        b.fmt = "%tc";
        break;
    case DUCKDB_TYPE_TIMESTAMP_S:
    case DUCKDB_TYPE_TIMESTAMP_MS:
    case DUCKDB_TYPE_TIMESTAMP_NS:
    case DUCKDB_TYPE_TIMESTAMP_TZ:
        b.sql = ts_ms_sql("CAST(" + ref + " AS TIMESTAMP)");
        b.fmt = "%tc";
        break;
    case DUCKDB_TYPE_TIME:
    case DUCKDB_TYPE_TIME_NS:
    case DUCKDB_TYPE_TIME_TZ: {
        std::string us = "epoch_us(CAST(DATE '1970-01-01' + CAST(" + ref +
                         " AS TIME) AS TIMESTAMP))";
        b.sql = "((" + us + " - (((" + us + ") % 1000 + 1000) % 1000)) / 1000)";
        b.fmt = "%tcHH:MM:SS";
        b.note = "time-of-day as milliseconds since midnight";
        break;
    }
    case DUCKDB_TYPE_VARCHAR:
        /* PQ-AUD-002: Stata has no string NULL; the string missing value is the
         * empty string "". Fold a foreign Parquet NULL to '' at the boundary so
         * it ties with '' for sort / _n / keep-in / duplicates, and never saves
         * back as a SQL NULL — matching the eager fill. */
        b.sql = "coalesce(" + ref + ", '')";
        b.kind = 's';
        break;
    case DUCKDB_TYPE_ENUM:
    case DUCKDB_TYPE_UUID:
        b.sql = "coalesce(CAST(" + ref + " AS VARCHAR), '')";
        b.kind = 's';
        break;
    default: {
        /* NULL-typed columns drop loudly here too (via plan_read_column's
         * drop_reason), never load as an all-missing byte (brief §4, §6.11). */
        parqit::ColumnPlan probe = parqit::plan_read_column(name, lt);
        b.dropped = true;
        b.drop_reason = probe.drop_reason.empty() ? "unsupported type"
                                                  : probe.drop_reason;
        break;
    }
    }
    return b;
}

ST_retcode require_view(std::string *err) {
    auto it = g_views.find(g_current);
    if (it == g_views.end() || !it->second.live()) {
        *err = "no lazy view is open (current: " + g_current +
               "); run parqit use using <files> first";
        return kRcUsage;
    }
    return 0;
}

std::vector<std::string> req_list_or_empty(const json &j, const char *key) {
    std::vector<std::string> out;
    std::string err;
    parqit::req_text_list(j, key, &out, &err, false);
    return out;
}

/* compiled SQL with the parquet-boundary back-casts for saving */
std::string compile_for_save(const View &v) {
    std::string sel;
    const auto &cols = v.cols();
    for (size_t i = 0; i < cols.size(); i++) {
        const ViewCol &c = cols[i];
        const std::string ref = quote_ident(c.name);
        std::string expr = ref;
        switch (parqit::classify_format(c.fmt)) {
        case parqit::FmtClass::Td:
            expr = "(DATE '1960-01-01' + CAST(round(" + ref + ") AS INTEGER))";
            break;
        case parqit::FmtClass::Tc:
            /* epoch_ms(bigint) builds the instant from ms-since-1970 with no
             * overflow. The old `<ms> * INTERVAL 1 MILLISECOND` form silently
             * down-cast the multiplier to INT32, so the disk/view save of any
             * %tc datetime outside ~[1969-12-07, 1970-01-25] failed with
             * rc 920 — including parqit's own files (STR1). */
            expr = "epoch_ms(CAST(round(" + ref + ") AS BIGINT) - " +
                   std::to_string(parqit::kEpochShiftMs) + ")";
            break;
        case parqit::FmtClass::TC:
            expr = "CAST(round(" + ref + ") AS BIGINT)";
            break;
        case parqit::FmtClass::Tm:
        case parqit::FmtClass::Tq:
        case parqit::FmtClass::Th:
        case parqit::FmtClass::Tw:
        case parqit::FmtClass::Ty:
        case parqit::FmtClass::Tb:
            expr = "CAST(round(" + ref + ") AS INTEGER)";
            break;
        default:
            /* PQ-AUD-001/002 defensive final guard for non-date columns. A
             * MISS-1-normalized column (a boundary column carried through
             * unchanged) is already clean — its specials were nulled and string
             * NULLs folded at the boundary — so it pays nothing here. A value
             * generated/recomputed after the boundary (gen/replace/aggregate)
             * can still be an IEEE special (e.g. gen z = exp(10000) -> +Inf) or
             * a SQL NULL string, so guard those to mirror the eager/direct-save
             * paths and never write a NaN/Inf or string NULL. Bounded integer
             * columns (coerce_storage already clamped them) cannot be special. */
            if (c.normalized) {
                break; /* already boundary-normalized; keep raw ref */
            } else if (c.kind == 's') {
                expr = "coalesce(" + ref + ", '')";
            } else {
                parqit::StType mt;
                int mb = 0;
                bool bounded_int =
                    parqit::sttype_parse(c.meta_type, &mt, &mb) &&
                    (mt == parqit::StType::Byte || mt == parqit::StType::Int ||
                     mt == parqit::StType::Long);
                if (!bounded_int) expr = float_missing_guard(ref, ref);
            }
            break;
        }
        if (i) sel += ", ";
        sel += expr + " AS " + ref;
    }
    return "SELECT " + sel + " FROM (" + v.compile(true) + ")";
}

/* parqit.* KV fragment from the view's carried metadata */
std::string view_kv_fragment(const View &v) {
    json schema;
    schema["version"] = 1;
    json jvars = json::array();
    std::set<std::string> used_labs;
    for (const auto &c : v.cols()) {
        json jv;
        jv["name"] = c.name;
        jv["src"] = c.name;
        if (!c.meta_type.empty()) jv["type"] = c.meta_type;
        jv["fmt"] = c.fmt;
        jv["varlab"] = c.varlab;
        jv["vallab"] = c.vallab;
        if (!c.vallab.empty()) used_labs.insert(c.vallab);
        jvars.push_back(jv);
    }
    schema["vars"] = jvars;
    json vallabs = json::object();
    if (v.vallabs().is_object()) {
        for (const auto &l : v.vallabs().items())
            if (used_labs.count(l.key())) vallabs[l.key()] = l.value();
    }
    json chars = json::object();
    if (v.chars().is_object()) {
        std::set<std::string> live;
        for (const auto &c : v.cols()) live.insert(c.name);
        live.insert("_dta");
        for (const auto &t : v.chars().items())
            if (live.count(t.key())) chars[t.key()] = t.value();
    }
    return "KV_METADATA {'parqit.schema': " + quote_literal(schema.dump()) +
           ", 'parqit.vallabs': " + quote_literal(vallabs.dump()) +
           ", 'parqit.chars': " + quote_literal(chars.dump()) +
           ", 'parqit.dtalabel': " + quote_literal(json(v.dtalabel()).dump()) + "}";
}

/* validate pending keep-in ranges against real prefix counts (charter §6.13) */
ST_retcode validate_ranges(Session &s, const View &v, std::string *err) {
    for (const auto &pr : v.pending_ranges()) {
        std::string nstr;
        if (!s.query_scalar("SELECT count(*) FROM (" + v.compile_prefix(pr.stage) +
                                ")",
                            &nstr, err))
            return kRcEngine;
        long long have = std::strtoll(nstr.c_str(), nullptr, 10);
        if (pr.l > have) {
            *err = "in " + std::to_string(pr.f) + "/" + std::to_string(pr.l) +
                   " is out of range: only " + std::to_string(have) +
                   " observations at that step";
            return kRcUsage;
        }
    }
    return 0;
}

/* true for the date/period format classes compile_for_save rounds to a whole
 * day/ms/period count (the exact set the in-memory save_data path also rounds
 * and warns about). */
bool is_rounded_date_class(parqit::FmtClass fc) {
    switch (fc) {
    case parqit::FmtClass::Td:
    case parqit::FmtClass::Tc:
    case parqit::FmtClass::TC:
    case parqit::FmtClass::Tm:
    case parqit::FmtClass::Tq:
    case parqit::FmtClass::Th:
    case parqit::FmtClass::Tw:
    case parqit::FmtClass::Ty:
    case parqit::FmtClass::Tb:
        return true;
    default:
        return false;
    }
}

/* Names of date/period columns whose value compile_for_save's round() would
 * actually change (a non-integer %td/%tc/%tC/period value), so view_save can
 * emit the same "rounded to the nearest unit" note the in-memory save_data
 * path emits (ATOM-2). Best-effort: any query failure yields no note rather
 * than blocking the save.
 *
 * Cost discipline (charter §: no path slower): a column made of integer day/ms
 * counts can never round, so the one aggregate scan is skipped unless a
 * date-formatted column is *floating*-typed in the view — which only happens
 * after a gen/replace makes it fractional, or for a foreign double carrying a
 * date format. Whether each date column is floating is read from a
 * metadata-only LIMIT 0 describe (no row scan); a save with no date columns
 * pays nothing at all. */
std::vector<std::string> detect_frac_rounding(Session &s, const View &v) {
    std::vector<std::string> out;
    std::vector<std::string> dnames;
    for (const auto &c : v.cols())
        if (is_rounded_date_class(parqit::classify_format(c.fmt)))
            dnames.push_back(c.name);
    if (dnames.empty()) return out; /* common case: no date columns, no work */

    const std::string inner = "(" + v.compile(false) + ")";
    std::string err;

    /* metadata-only describe: keep only the floating-typed date columns */
    std::string proj;
    for (size_t i = 0; i < dnames.size(); i++)
        proj += (i ? ", " : "") + quote_ident(dnames[i]);
    duckdb_result desc;
    if (!s.query("SELECT " + proj + " FROM " + inner + " LIMIT 0", &desc, &err))
        return out;
    std::vector<std::string> cand;
    idx_t nc = duckdb_column_count(&desc);
    for (idx_t c = 0; c < nc && c < dnames.size(); c++) {
        duckdb_logical_type lt = duckdb_column_logical_type(&desc, c);
        duckdb_type tid = duckdb_get_type_id(lt);
        duckdb_destroy_logical_type(&lt);
        if (tid == DUCKDB_TYPE_FLOAT || tid == DUCKDB_TYPE_DOUBLE ||
            tid == DUCKDB_TYPE_DECIMAL)
            cand.push_back(dnames[c]);
    }
    duckdb_destroy_result(&desc);
    if (cand.empty()) return out; /* every date column is integer-typed */

    /* one aggregate scan: which floating date column holds a non-integer? */
    std::string sel;
    for (size_t i = 0; i < cand.size(); i++) {
        const std::string r = quote_ident(cand[i]);
        sel += (i ? ", " : "") + std::string("CAST(coalesce(bool_or(round(") +
               r + ") <> " + r + "), false) AS INTEGER)";
    }
    duckdb_result agg;
    if (!s.query("SELECT " + sel + " FROM " + inner, &agg, &err)) return out;
    for (size_t i = 0; i < cand.size(); i++)
        if (duckdb_value_int64(&agg, static_cast<idx_t>(i), 0) != 0)
            out.push_back(cand[i]);
    duckdb_destroy_result(&agg);
    return out;
}

} // namespace

bool view_is_live() {
    auto it = g_views.find(g_current);
    return it != g_views.end() && it->second.live();
}

/* ======================================================== view_open ===== */

ST_retcode cmd_view_open(const std::vector<std::string> &args) {
    std::string err;
    json req;
    if (!load_req(args, &req, &err)) {
        cry(err);
        return kRcUsage;
    }
    std::vector<std::string> files, varlist;
    std::string tmpdir;
    if (!parqit::req_text_list(req, "files", &files, &err) ||
        !parqit::req_text(req, "tmpdir", &tmpdir, &err)) {
        cry(err);
        return kRcUsage;
    }
    varlist = req_list_or_empty(req, "varlist");
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
    duckdb_result res;
    if (!s.query("SELECT * FROM " + src.scan_sql + " LIMIT 0", &res, &err)) {
        cry("parqit use: " + err);
        return kRcEngine;
    }
    idx_t ncol = duckdb_column_count(&res);
    std::vector<std::string> src_names(ncol);
    std::vector<BoundaryCol> bounds(ncol);
    for (idx_t c = 0; c < ncol; c++) {
        const char *nm = duckdb_column_name(&res, c);
        src_names[c] = nm ? nm : "";
        duckdb_logical_type lt = duckdb_column_logical_type(&res, c);
        bounds[c] = boundary_for(src_names[c], lt);
        duckdb_destroy_logical_type(&lt);
    }
    duckdb_destroy_result(&res);

    std::vector<bool> renamed;
    std::vector<std::string> stata_names = parqit::sanitize_unique(src_names, &renamed);

    /* parqit.* metadata rides along: reuse the planner's reader via a tiny
     * plan context (no stats, no varlist) */
    PlanContext meta_ctx;
    {
        std::string merr;
        /* metadata/schema only — no row count needed (PERF-3) */
        plan_columns(s, src, {}, /*with_stats=*/false, &meta_ctx, &merr,
                     /*need_count=*/false);
    }
    std::map<std::string, const json *> meta_by_src;
    if (meta_ctx.meta.present && meta_ctx.meta.schema.contains("vars")) {
        for (const auto &v : meta_ctx.meta.schema["vars"].items()) {
            const json &jv = v.value();
            if (jv.contains("src") && jv["src"].is_string())
                meta_by_src[jv["src"].get<std::string>()] = &jv;
        }
    }
    auto sget = [](const json *j, const char *k) -> std::string {
        return (j && j->contains(k) && (*j)[k].is_string())
                   ? (*j)[k].get<std::string>()
                   : std::string();
    };

    std::string sel;
    std::vector<ViewCol> cols;
    std::vector<std::string> warns, drops;
    /* F8: the eager loader records a sanitised column's original file name in
     * `char var[src_name]`; carry the same provenance on the lazy path through
     * the view's chars, which already round-trip to collect and into the
     * parqit.chars of a view save. */
    json vchars = meta_ctx.meta.present && meta_ctx.meta.chars.is_object()
                      ? meta_ctx.meta.chars
                      : json::object();
    for (idx_t c = 0; c < ncol; c++) {
        if (bounds[c].dropped) {
            drops.push_back("column \"" + src_names[c] + "\" dropped: " +
                            bounds[c].drop_reason);
            continue;
        }
        ViewCol vc;
        vc.name = stata_names[c];
        vc.kind = bounds[c].kind;
        vc.fmt = bounds[c].fmt;
        vc.note = bounds[c].note;
        /* MISS-1: every boundary column is normalized — float/double specials
         * are nulled and string NULLs are folded to "" by boundary_for(). */
        vc.normalized = true;
        const json *jm = nullptr;
        auto it = meta_by_src.find(src_names[c]);
        if (it != meta_by_src.end()) jm = it->second;
        std::string mfmt = sget(jm, "fmt");
        if (!mfmt.empty()) vc.fmt = mfmt;
        vc.varlab = sget(jm, "varlab");
        vc.vallab = sget(jm, "vallab");
        vc.meta_type = sget(jm, "type");
        if (renamed[c]) {
            warns.push_back("column \"" + src_names[c] + "\" is " + vc.name +
                            " in the view");
            vchars[vc.name]["src_name"] = src_names[c];
        }
        if (!sel.empty()) sel += ", ";
        sel += bounds[c].sql + " AS " + quote_ident(vc.name);
        cols.push_back(vc);
    }
    if (cols.empty()) {
        cry("parqit use: no loadable columns (every column was dropped)");
        return kRcUsage;
    }

    std::string vname;
    if (!parqit::req_text(req, "name", &vname, &err, false)) {
        cry(err);
        return kRcUsage;
    }
    if (!vname.empty()) g_current = vname;
    std::string desc;
    for (size_t i = 0; i < files.size(); i++) desc += (i ? " " : "") + files[i];
    /* replacing a view orphans any bridge file it owned (keep the new
     * backing if it happens to be the same path) */
    drop_owned(g_current, files.size() == 1 ? files[0] : "");
    if (req.value("owned", false) && files.size() == 1)
        g_owned_files[g_current] = files[0];
    g_view_ref().open("SELECT " + sel + " FROM " + src.scan_sql, cols,
                meta_ctx.meta.present ? meta_ctx.meta.vallabs : json::object(),
                vchars,
                meta_ctx.meta.present ? meta_ctx.meta.dtalabel : "", desc);
    /* remember the backing Parquet paths: a later pure-passthrough collect
     * can size its columns from row-group statistics (the F2 metadata path
     * `parqit use` uses) instead of a redundant full scan. */
    g_view_ref().set_source_paths(src.paths_sql);

    /* optional initial projection (named columns, named order) */
    if (!varlist.empty()) {
        std::string verr = g_view_ref().keep_vars(varlist);
        if (!verr.empty()) {
            g_view_ref().close();
            cry("parqit use: " + verr);
            return kRcUsage;
        }
    }
    for (const auto &w : warns) cry("note: " + w);
    for (const auto &d : drops) cry("warning: " + d);

    save_local("_parqit_view_k", std::to_string(g_view_ref().cols().size()));
    save_local("_parqit_view_src", parqit::hex_encode(g_view_ref().source_desc()));
    save_local("_parqit_view_name", parqit::hex_encode(g_current));
    return 0;
}

/* ========================================================== view_op ===== */

ST_retcode cmd_view_op(const std::vector<std::string> &args) {
    std::string err;
    if (require_view(&err) != 0) {
        cry("parqit: " + err);
        return kRcUsage;
    }
    json req;
    if (!load_req(args, &req, &err)) {
        cry(err);
        return kRcUsage;
    }
    std::string op;
    if (!parqit::req_text(req, "op", &op, &err)) {
        cry(err);
        return kRcUsage;
    }
    std::string e;
    if (op == "keep_vars") e = g_view_ref().keep_vars(req_list_or_empty(req, "names"));
    else if (op == "drop_vars") e = g_view_ref().drop_vars(req_list_or_empty(req, "names"));
    else if (op == "keep_if" || op == "drop_if") {
        std::string ex;
        if (!parqit::req_text(req, "expr", &ex, &err)) {
            cry(err);
            return kRcUsage;
        }
        e = g_view_ref().filter(ex, op == "drop_if", g_statamissing);
    } else if (op == "gen") {
        std::string name, type, ex, ifex;
        if (!parqit::req_text(req, "name", &name, &err) ||
            !parqit::req_text(req, "type", &type, &err, false) ||
            !parqit::req_text(req, "expr", &ex, &err) ||
            !parqit::req_text(req, "ifexpr", &ifex, &err, false)) {
            cry(err);
            return kRcUsage;
        }
        e = g_view_ref().gen(name, type, ex, ifex, g_statamissing);
    } else if (op == "replace") {
        std::string name, ex, ifex;
        if (!parqit::req_text(req, "name", &name, &err) ||
            !parqit::req_text(req, "expr", &ex, &err) ||
            !parqit::req_text(req, "ifexpr", &ifex, &err, false)) {
            cry(err);
            return kRcUsage;
        }
        e = g_view_ref().replace(name, ex, ifex, g_statamissing);
    } else if (op == "rename") {
        std::string a, b;
        if (!parqit::req_text(req, "old", &a, &err) ||
            !parqit::req_text(req, "new", &b, &err)) {
            cry(err);
            return kRcUsage;
        }
        e = g_view_ref().rename(a, b);
    } else if (op == "order") {
        e = g_view_ref().reorder(req_list_or_empty(req, "names"));
    } else if (op == "sort") {
        std::vector<std::string> keys = req_list_or_empty(req, "keys");
        std::vector<bool> desc;
        if (req.contains("desc") && req["desc"].is_array())
            for (const auto &d : req["desc"]) desc.push_back(d.get<bool>());
        e = g_view_ref().sort(keys, desc);
    } else if (op == "collapse") {
        std::vector<View::CollapseSpec> specs;
        if (req.contains("specs") && req["specs"].is_array()) {
            for (const auto &js : req["specs"]) {
                View::CollapseSpec sp;
                std::string serr;
                if (!parqit::req_text(js, "stat", &sp.stat, &serr) ||
                    !parqit::req_text(js, "target", &sp.target, &serr, false) ||
                    !parqit::req_text(js, "source", &sp.source, &serr)) {
                    cry(serr);
                    return kRcUsage;
                }
                specs.push_back(sp);
            }
        }
        e = g_view_ref().collapse(specs, req_list_or_empty(req, "by"));
    } else if (op == "contract") {
        std::string freq;
        parqit::req_text(req, "freq", &freq, &err, false);
        e = g_view_ref().contract(req_list_or_empty(req, "names"), freq);
    } else if (op == "dupdrop") {
        e = g_view_ref().duplicates_drop(req_list_or_empty(req, "names"),
                                   req.value("force", false));
    } else if (op == "keep_in") {
        e = g_view_ref().keep_in(req.value("f", 0LL), req.value("l", 0LL));
    } else if (op == "sample") {
        e = g_view_ref().sample(req.value("amount", 0.0), req.value("count", false),
                          req.value("seed", -1LL));
    } else if (op == "egen") {
        std::string name, fcn, ex, ty;
        if (!parqit::req_text(req, "name", &name, &err) ||
            !parqit::req_text(req, "fcn", &fcn, &err) ||
            !parqit::req_text(req, "type", &ty, &err, false) ||
            !parqit::req_text(req, "expr", &ex, &err)) {
            cry(err);
            return kRcUsage;
        }
        e = g_view_ref().egen(name, fcn, ex, req_list_or_empty(req, "by"),
                        g_statamissing, ty);
    } else {
        cry("parqit: unknown view operation '" + op + "'");
        return kRcUsage;
    }
    if (!e.empty()) {
        cry("parqit: " + e);
        return kRcUsage;
    }
    save_local("_parqit_view_k", std::to_string(g_view_ref().cols().size()));
    return 0;
}

/* ================================================= info: show/etc ===== */

ST_retcode cmd_view_info(const std::vector<std::string> &args) {
    std::string err;
    if (require_view(&err) != 0) {
        cry("parqit: " + err);
        return kRcUsage;
    }
    std::string what;
    if (args.size() < 2 || !parqit::hex_decode(args[1], what)) {
        cry("parqit: malformed info request");
        return kRcUsage;
    }
    Session &s = Session::instance();

    if (what == "count") {
        ST_retcode rrc = validate_ranges(s, g_view_ref(), &err);
        if (rrc != 0) {
            cry("parqit count: " + err);
            return rrc;
        }
        std::string n;
        if (!s.query_scalar("SELECT count(*) FROM (" + g_view_ref().compile(false) + ")",
                            &n, &err)) {
            cry("parqit count: " + err);
            return kRcEngine;
        }
        save_local("_parqit_n", n);
        return 0;
    }

    /* show / explain / describe write into a response file */
    std::string respfile;
    if (args.size() < 3 || !parqit::hex_decode(args[2], respfile)) {
        cry("parqit: missing response file");
        return kRcUsage;
    }
    parqit::ResponseWriter w;
    if (!w.open(respfile, &err)) {
        cry(err);
        return kRcEngine;
    }
    if (what == "show") {
        w.rec("sql", {}, {g_view_ref().show()});
    } else if (what == "explain") {
        duckdb_result res;
        if (!s.query("EXPLAIN " + g_view_ref().compile(true), &res, &err)) {
            cry("parqit explain: " + err);
            return kRcEngine;
        }
        idx_t n = duckdb_row_count(&res);
        for (idx_t r = 0; r < n; r++) {
            char *k = duckdb_value_varchar(&res, 0, r);
            char *v = duckdb_value_varchar(&res, 1, r);
            w.rec("plan", {}, {k ? k : "", v ? v : ""});
            if (k) duckdb_free(k);
            if (v) duckdb_free(v);
        }
        duckdb_destroy_result(&res);
    } else if (what == "describe") {
        const auto &cols = g_view_ref().cols();
        for (size_t i = 0; i < cols.size(); i++) {
            w.rec("vcol", {std::to_string(i + 1)},
                  {cols[i].name, std::string(1, cols[i].kind), cols[i].fmt,
                   cols[i].varlab});
        }
        save_local("_parqit_view_k", std::to_string(cols.size()));
        save_local("_parqit_view_src", parqit::hex_encode(g_view_ref().source_desc()));
        save_local("_parqit_view_stages", std::to_string(g_view_ref().n_stages()));
    } else {
        cry("parqit: unknown info request");
        return kRcUsage;
    }
    if (!w.close(&err)) {
        cry(err);
        return kRcEngine;
    }
    return 0;
}

/* ============================================== collect (prepare) ===== */

ST_retcode cmd_view_collect_prepare(const std::vector<std::string> &args) {
    std::string err;
    if (require_view(&err) != 0) {
        cry("parqit collect: " + err);
        return kRcUsage;
    }
    json req;
    if (!load_req(args, &req, &err)) {
        cry(err);
        return kRcUsage;
    }
    std::string respfile, strlfile, tmpdir;
    long long limit = req.value("limit", -1LL);
    if (!parqit::req_text(req, "respfile", &respfile, &err) ||
        !parqit::req_text(req, "strlfile", &strlfile, &err, false) ||
        !parqit::req_text(req, "tmpdir", &tmpdir, &err)) {
        cry(err);
        return kRcUsage;
    }
    Session &s = Session::instance();
    s.set_default_temp_dir(tmpdir + parqit::spill_suffix());

    ST_retcode rrc = validate_ranges(s, g_view_ref(), &err);
    if (rrc != 0) {
        cry("parqit collect: " + err);
        return rrc;
    }

    /* materialise ONCE into a spillable temp table; preview forms may slice
     * (in f/l over the view's order), filter (if-expression) and project */
    const std::string table = "_parqit_collect_" + std::to_string(++g_collect_counter);
    std::string sql = g_view_ref().compile(true);
    long long pf = req.value("f", 0LL), pl = req.value("l", 0LL);
    if (pf >= 1 && pl >= pf) {
        /* validate the preview slice against the real row count, exactly like
         * validate_ranges does for keep-in (charter §6.13): a bare LIMIT/OFFSET
         * past the data would print nothing with rc 0 where native
         * `list in f/l` stops with r(198). */
        std::string nstr;
        if (!s.query_scalar("SELECT count(*) FROM (" +
                                g_view_ref().compile(false) + ")",
                            &nstr, &err)) {
            cry("parqit list: " + err);
            return kRcEngine;
        }
        long long have = std::strtoll(nstr.c_str(), nullptr, 10);
        if (pl > have) {
            cry("parqit list: in " + std::to_string(pf) + "/" +
                std::to_string(pl) + " is out of range: only " +
                std::to_string(have) + " observations");
            return kRcUsage;
        }
        sql = "SELECT * FROM (" + sql + " LIMIT " + std::to_string(pl - pf + 1) +
              " OFFSET " + std::to_string(pf - 1) + ")";
    }
    std::string pfilter;
    if (!parqit::req_text(req, "filter", &pfilter, &err, false)) {
        cry(err);
        return kRcUsage;
    }
    if (!pfilter.empty()) {
        parqit::ExprSchema sch;
        for (const auto &c : g_view_ref().cols()) sch.kinds[c.name] = c.kind;
        parqit::ExprResult tr = parqit::translate_filter(pfilter, sch, g_statamissing);
        if (!tr.ok) {
            cry("parqit list: " + tr.error);
            return kRcUsage;
        }
        sql = "SELECT * FROM (" + sql + ") WHERE " + tr.sql;
    }
    std::vector<std::string> pvars;
    if (!parqit::req_text_list(req, "vars", &pvars, &err, false)) {
        cry(err);
        return kRcUsage;
    }
    if (!pvars.empty()) {
        std::string selv;
        for (const auto &v : pvars) {
            bool found = false;
            for (const auto &c : g_view_ref().cols()) found = found || c.name == v;
            if (!found) {
                cry("parqit list: variable " + v + " not found in the view");
                return kRcVarNotFound;
            }
            selv += (selv.empty() ? "" : ", ") + quote_ident(v);
        }
        sql = "SELECT " + selv + " FROM (" + sql + ")";
    }
    if (limit >= 0) sql = "SELECT * FROM (" + sql + ") LIMIT " + std::to_string(limit);

    const bool direct_read =
        g_view_ref().n_stages() == 0 && g_view_ref().sort_keys().empty() &&
        g_view_ref().pending_ranges().empty() && limit < 0 && pf < 1 &&
        pfilter.empty() && pvars.empty();

    Source vsrc;
    vsrc.paths_sql = "[]";
    bool drop_source_after = false;
    if (direct_read) {
        /* Pure `parqit use` + `parqit collect` is a read path. Avoid copying the
         * whole file into a DuckDB temp table before the Arrow→Stata fill. */
        vsrc.scan_sql = "(" + sql + ") AS __parqit_collect_direct";
        /* A full-file passthrough (n_stages()==0, the direct_read guard
         * above) has columns identical to a direct `parqit use`, so its
         * column sizing can come from the Parquet row-group statistics
         * exactly as F2 does on the use path — no redundant full scan.
         * plan_columns still falls back to a real scan for any column the
         * footer cannot size exactly (strings, >2^53, date/timestamp stats
         * that don't cast, files with duplicate names or absent stats), so
         * the result is byte-for-byte the scan-based plan. */
        vsrc.paths_sql = g_view_ref().source_paths_sql();
        if (vsrc.paths_sql.empty()) vsrc.paths_sql = "[]";
    } else {
        if (!s.exec("CREATE TEMP TABLE " + quote_ident(table) + " AS " + sql, &err)) {
            cry("parqit collect: " + err);
            return kRcEngine;
        }
        vsrc.scan_sql = quote_ident(table);
        drop_source_after = true;
    }
    PlanContext ctx;
    ST_retcode rc = plan_columns(s, vsrc, {}, /*with_stats=*/true, &ctx, &err);
    if (rc != 0) {
        std::string derr;
        if (drop_source_after) s.exec("DROP TABLE IF EXISTS " + quote_ident(table), &derr);
        cry("parqit collect: " + err);
        return rc;
    }

    /* overlay the view's carried metadata by position (the compiled SELECT
     * preserves the view's column order and names) */
    std::vector<ViewCol> vcols_all = g_view_ref().cols();
    std::vector<ViewCol> vcols;
    if (!pvars.empty()) {
        for (const auto &v : pvars)
            for (const auto &c : vcols_all)
                if (c.name == v) vcols.push_back(c);
    } else {
        vcols = vcols_all;
    }
    if (ctx.active.size() == vcols.size()) {
        for (size_t i = 0; i < ctx.active.size(); i++) {
            parqit::ColumnPlan &p = ctx.active[i];
            const ViewCol &vc = vcols[i];
            if (!vc.fmt.empty()) p.stata_format = vc.fmt;
            p.varlab = vc.varlab;
            p.vallab = vc.vallab;
            if (!vc.note.empty())
                p.note = p.note.empty() ? vc.note : p.note + "; " + vc.note;
            if (!vc.meta_type.empty()) {
                p.meta_type = vc.meta_type;
                parqit::apply_meta_type(p);
            } else if (parqit::classify_format(p.stata_format) == parqit::FmtClass::Td &&
                       (p.stata_type == parqit::StType::Byte ||
                        p.stata_type == parqit::StType::Int)) {
                /* A bare parquet DATE column is stored Long on the `parqit use`
                 * path (typemap maps DUCKDB_TYPE_DATE -> Long unconditionally).
                 * Here the column reaches the planner already cast to an
                 * integer day-count, so range refinement can shrink it to
                 * int/byte and overflow for dates past ~2049. Match `use`: a
                 * %td date with no recorded Stata type is stored Long. (Parqit-
                 * written files carry meta_type and take the branch above.) */
                p.stata_type = parqit::StType::Long;
            }
        }
    } else {
        /* never skip the overlay silently: a count mismatch (the planner
         * dropped a column the view still carries, or vice versa) means the
         * view's formats/labels/storage types cannot be matched positionally.
         * Say so through the same warn-record channel plan_columns uses, so
         * the collected data arrives loud, not quietly stripped of metadata. */
        ctx.warnings.push_back(
            "view metadata could not be matched to the collected columns (view "
            "carries " + std::to_string(vcols.size()) + ", planned " +
            std::to_string(ctx.active.size()) +
            "); formats, labels and storage types were not restored");
    }
    /* vallab definitions + chars + data label from the view */
    ctx.meta.present = true;
    ctx.meta.vallabs = g_view_ref().vallabs();
    ctx.meta.chars = g_view_ref().chars();
    ctx.meta.dtalabel = g_view_ref().dtalabel();

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

    std::string tag, names;
    for (size_t i = 0; i < ctx.active.size(); i++) {
        if (i) names += " ";
        names += ctx.active[i].stata_name;
    }
    set_prepared_read(vsrc.scan_sql, ctx.active, ctx.nrows, strlfile,
                      drop_source_after, &tag);
    save_local("_parqit_tag", parqit::hex_encode(tag));
    save_local("_parqit_n", std::to_string(ctx.nrows));
    save_local("_parqit_k", std::to_string(ctx.active.size()));
    save_local("_parqit_names", names);
    return 0;
}

/* ======================================================= view_save ===== */

ST_retcode cmd_view_save(const std::vector<std::string> &args) {
    std::string err;
    if (require_view(&err) != 0) {
        cry("parqit save: " + err);
        return kRcUsage;
    }
    json req;
    if (!load_req(args, &req, &err)) {
        cry(err);
        return kRcUsage;
    }
    std::string dest, tmpdir, compression;
    if (!parqit::req_text(req, "dest", &dest, &err) ||
        !parqit::req_text(req, "tmpdir", &tmpdir, &err) ||
        !parqit::req_text(req, "compression", &compression, &err, false)) {
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
    std::vector<std::string> partition_by = req_list_or_empty(req, "partition_by");
    for (const auto &pv : partition_by) {
        bool found = false;
        for (const auto &c : g_view_ref().cols()) found = found || c.name == pv;
        if (!found) {
            cry("parqit save: partition_by(" + pv + ") is not a variable in the view");
            return kRcUsage;
        }
    }

    /* Refuse to overwrite the open view's own backing file. A lazy view rereads
     * its source at fetch time, so writing onto it truncates the data the view
     * (and any other view over the same file) still needs — silently (IO-1).
     * A temp bridge or a different file is fine.
     *
     * SAVE-SELFGLOB-2 (was SAVE-SELFGLOB-1): a glob source is compared by
     * actually MATCHING the destination against the pattern, not by refusing
     * everything under the pattern's base directory — the old containment
     * test rejected any existing destination in the same folder as a
     * `qp_*.parquet` source (a false positive on the documented
     * filter-then-save-next-to-the-source workflow), while a RELATIVE
     * not-yet-existing destination escaped comparison entirely (a real hole:
     * both paths are made absolute first now). The glob dialect is parqit's
     * own (GLOB-2): `*`/`?` never cross a `/`, `**` does, `[` is literal. A
     * directory destination is also refused when the pattern's base lies
     * inside it (a partitioned `replace` renames the tree aside and would
     * delete the source). */
    {
        std::error_code ec;
        std::string dabs =
            std::filesystem::weakly_canonical(
                std::filesystem::absolute(dest, ec), ec)
                .generic_string();
        /* stored patterns carry source_for's escapes — decode "[*]"/"[?]"/
         * "[[]" to literal bytes (sentinels for the two metachars, which no
         * real path uses) so the matcher sees exactly parqit's dialect */
        auto gdecode = [](const std::string &p) {
            std::string out;
            for (size_t i = 0; i < p.size(); i++) {
                if (i + 2 < p.size() && p[i] == '[' && p[i + 2] == ']' &&
                    (p[i + 1] == '*' || p[i + 1] == '?' || p[i + 1] == '[')) {
                    out += (p[i + 1] == '*'   ? '\x01'
                            : p[i + 1] == '?' ? '\x02'
                                              : '[');
                    i += 2;
                } else {
                    out += p[i];
                }
            }
            return out;
        };
        std::function<bool(const char *, const char *)> gmatch =
            [&](const char *p, const char *s) -> bool {
            while (*p) {
                if (*p == '*') {
                    bool deep = (p[1] == '*');
                    const char *rest = deep ? p + 2 : p + 1;
                    for (const char *t = s;; t++) {
                        if (gmatch(rest, t)) return true;
                        if (!*t || (!deep && *t == '/')) break;
                    }
                    return false;
                }
                if (!*s) return false;
                if (*p == '?') {
                    if (*s == '/') return false;
                } else {
                    char pc = *p == '\x01' ? '*' : *p == '\x02' ? '?' : *p;
                    if (pc != *s) return false;
                }
                p++;
                s++;
            }
            return !*s;
        };
        const std::string &psql = g_view_ref().source_paths_sql();
        for (size_t i = 0; i < psql.size();) {
            if (psql[i] != '\'') { i++; continue; }
            std::string sp;
            for (i++; i < psql.size();) {
                if (psql[i] == '\'') {
                    if (i + 1 < psql.size() && psql[i + 1] == '\'') { sp += '\''; i += 2; continue; }
                    i++;
                    break;
                }
                sp += psql[i++];
            }
            std::string spd = gdecode(sp);
            bool is_glob = spd.find('*') != std::string::npos ||
                           spd.find('?') != std::string::npos;
            std::error_code ec2;
            /* absolute pattern/path for the comparison (weakly_canonical keeps
             * non-existing tails, so a fresh destination still compares) */
            std::string sabs =
                std::filesystem::weakly_canonical(
                    std::filesystem::absolute(spd, ec2), ec2)
                    .generic_string();
            auto path_contains = [](const std::string &a, const std::string &b) {
                if (a.empty() || b.empty()) return false;
                if (a == b) return true;
                return b.size() > a.size() && b.compare(0, a.size(), a) == 0 &&
                       b[a.size()] == '/';
            };
            bool clash;
            if (is_glob) {
                /* dest (or anything a dest DIRECTORY could shadow) matches the
                 * source pattern, or the pattern's base dir sits inside dest */
                size_t g = sabs.find_first_of("*?");
                size_t slash = sabs.rfind('/', g);
                std::string base =
                    (slash == std::string::npos) ? sabs : sabs.substr(0, slash);
                clash = gmatch(sabs.c_str(), dabs.c_str()) ||
                        path_contains(dabs, base);
            } else {
                clash = (!sabs.empty() && !dabs.empty() &&
                         (sabs == dabs || path_contains(dabs, sabs)));
            }
            if (clash) {
                cry("parqit save: " + dest +
                    " overlaps the open view's own source (" + sp +
                    "); write to a different path, or parqit collect first");
                return kRcUsage;
            }
        }
    }

    Session &s = Session::instance();
    s.set_default_temp_dir(tmpdir + parqit::spill_suffix());

    ST_retcode rrc = validate_ranges(s, g_view_ref(), &err);
    if (rrc != 0) {
        cry("parqit save: " + err);
        return rrc;
    }

    /* ATOM-2: a view over a Parquet source has no Stata extended missings to
     * collapse, but compile_for_save rounds non-integer date/period values
     * exactly as the in-memory save_data path does. Surface the same
     * "rounded to the nearest unit" note (cost: nothing unless a date column
     * is floating-typed — see detect_frac_rounding). */
    std::vector<std::string> frac = detect_frac_rounding(s, g_view_ref());

    long long written = 0;
    ST_retcode rc = copy_out_parquet(s, compile_for_save(g_view_ref()), dest, replace,
                                     compression, comp_level, partition_by, chunk,
                                     view_kv_fragment(g_view_ref()), &written, &err);
    if (rc != 0) {
        cry("parqit save: " + err);
        return rc;
    }
    std::error_code eca;
    std::string abs = std::filesystem::absolute(dest, eca).string();
    if (eca) abs = dest;
    std::string fraclist;
    for (size_t i = 0; i < frac.size(); i++)
        fraclist += (i ? " " : "") + frac[i];
    save_local("_parqit_written_n", std::to_string(written));
    save_local("_parqit_written_k", std::to_string(g_view_ref().cols().size()));
    save_local("_parqit_dest", parqit::hex_encode(abs));
    save_local("_parqit_frac_dates", fraclist);
    return 0;
}

/* ====================================================== close / set ===== */

/* view_close [<name-hex>|"_all"]: no argument closes the current view */
ST_retcode cmd_view_close(const std::vector<std::string> &args) {
    std::string which;
    if (args.size() >= 2 && !parqit::hex_decode(args[1], which)) {
        cry("parqit close: malformed view name");
        return kRcUsage;
    }
    if (which == "_all") {
        while (!g_owned_files.empty()) drop_owned(g_owned_files.begin()->first);
        g_views.clear();
        return 0;
    }
    if (which.empty()) {
        close_current_view();
        return 0;
    }
    if (!g_views.erase(which)) {
        cry("parqit close: no view named " + which);
        return kRcUsage;
    }
    drop_owned(which);
    return 0;
}

/* view_switch <name-hex>: make an existing view current */
ST_retcode cmd_view_switch(const std::vector<std::string> &args) {
    std::string name;
    if (args.size() < 2 || !parqit::hex_decode(args[1], name) || name.empty()) {
        cry("parqit view: malformed view name");
        return kRcUsage;
    }
    auto it = g_views.find(name);
    if (it == g_views.end() || !it->second.live()) {
        cry("parqit view: no view named " + name + " is open");
        return kRcUsage;
    }
    g_current = name;
    save_local("_parqit_view_k", std::to_string(it->second.cols().size()));
    save_local("_parqit_view_name", parqit::hex_encode(name));
    return 0;
}

/* view_list <respfile-hex>: one record per open view */
ST_retcode cmd_view_list(const std::vector<std::string> &args) {
    std::string respfile, err;
    if (args.size() < 2 || !parqit::hex_decode(args[1], respfile)) {
        cry("parqit views: malformed response path");
        return kRcUsage;
    }
    parqit::ResponseWriter w;
    if (!w.open(respfile, &err)) {
        cry(err);
        return kRcEngine;
    }
    long long n = 0;
    for (const auto &kv : g_views) {
        if (!kv.second.live()) continue;
        n++;
        w.rec("view",
              {kv.first == g_current ? "1" : "0",
               std::to_string(kv.second.cols().size()),
               std::to_string(kv.second.n_stages())},
              {kv.first, kv.second.source_desc()});
    }
    if (!w.close(&err)) {
        cry(err);
        return kRcEngine;
    }
    save_local("_parqit_n_views", std::to_string(n));
    save_local("_parqit_view_name", parqit::hex_encode(g_current));
    return 0;
}

ST_retcode cmd_set(const std::vector<std::string> &args) {
    std::string what, value;
    if (args.size() < 3 || !parqit::hex_decode(args[1], what) ||
        !parqit::hex_decode(args[2], value)) {
        cry("parqit set: malformed request");
        return kRcUsage;
    }
    Session &s = Session::instance();
    std::string err;
    if (what == "statamissing") {
        g_statamissing = (value == "on");
        return 0;
    }
    if (what == "threads") {
        /* SET-THREADS-1/2: parse strictly. strtoll silently truncates "4.5"->4
         * and "4 8"->4, and an out-of-INT32 value reaches DuckDB as a raw
         * INTERNAL cast-overflow assertion + stack trace. Require the whole
         * token to be digits (one optional leading '+') and fit DuckDB's INT32
         * thread count, with a clear message otherwise. */
        size_t p = 0;
        if (p < value.size() && value[p] == '+') p++;
        bool ok = p < value.size();
        for (size_t i = p; ok && i < value.size(); i++)
            if (!std::isdigit(static_cast<unsigned char>(value[i]))) ok = false;
        errno = 0;
        char *end = nullptr;
        long long n = ok ? std::strtoll(value.c_str(), &end, 10) : 0;
        if (!ok || errno == ERANGE || n < 1 || n > 2147483647LL) {
            cry("parqit set threads: value must be a positive integer (1..2147483647), got '" +
                value + "'");
            return kRcUsage;
        }
        if (!s.set_threads(n, &err)) {
            cry("parqit set threads: " + err);
            return kRcUsage;
        }
        return 0;
    }
    if (what == "memory_limit") {
        if (!s.set_memory_limit(value, &err)) {
            cry("parqit set memory_limit: " + err);
            return kRcUsage;
        }
        return 0;
    }
    if (what == "tempdir") {
        if (!s.set_temp_directory(value, &err)) {
            cry("parqit set tempdir: " + err);
            return kRcUsage;
        }
        return 0;
    }
    cry("parqit set: unknown setting '" + what +
        "' (statamissing threads memory_limit tempdir)");
    return kRcUsage;
}

} // namespace parqit_plugin

/* ================================================== two-table verbs ===== */

namespace parqit_plugin {
namespace {

/* Render a numeric column as text the way Stata does: an integer value prints
 * without a trailing ".0" (11, not 11.0). Used by levelsof and tabulate so the
 * two never disagree (TAB-FLOAT-1). `ref` is already a quoted identifier. */
static std::string stata_num_varchar(const std::string &ref) {
    return "(CASE WHEN " + ref + " = trunc(" + ref + ") AND abs(" + ref +
           ") < 1e15 THEN CAST(CAST(" + ref + " AS BIGINT) AS VARCHAR) ELSE CAST(" +
           ref + " AS VARCHAR) END)";
}

/* boundary-cast a using source (files on disk) into a View::UsingSide */
ST_retcode prepare_using(Session &s, const std::vector<std::string> &files,
                         View::UsingSide *out, std::vector<std::string> *drops,
                         std::string *err) {
    const Source src = source_for(files);
    ST_retcode grc =
        strict_schema_gate(s, src, files, /*relaxed=*/false, /*csv=*/false, err);
    if (grc != 0) return grc;
    duckdb_result res;
    if (!s.query("SELECT * FROM " + src.scan_sql + " LIMIT 0", &res, err))
        return kRcEngine;
    idx_t ncol = duckdb_column_count(&res);
    std::vector<std::string> src_names(ncol);
    std::vector<BoundaryCol> bounds(ncol);
    for (idx_t c = 0; c < ncol; c++) {
        const char *nm = duckdb_column_name(&res, c);
        src_names[c] = nm ? nm : "";
        duckdb_logical_type lt = duckdb_column_logical_type(&res, c);
        bounds[c] = boundary_for(src_names[c], lt);
        duckdb_destroy_logical_type(&lt);
    }
    duckdb_destroy_result(&res);

    std::vector<std::string> stata_names = parqit::sanitize_unique(src_names, nullptr);

    PlanContext meta_ctx;
    {
        std::string merr;
        /* metadata/schema only — no row count needed (PERF-3) */
        plan_columns(s, src, {}, /*with_stats=*/false, &meta_ctx, &merr,
                     /*need_count=*/false);
    }
    std::map<std::string, const json *> meta_by_src;
    if (meta_ctx.meta.present && meta_ctx.meta.schema.contains("vars")) {
        for (const auto &v : meta_ctx.meta.schema["vars"].items()) {
            const json &jv = v.value();
            if (jv.contains("src") && jv["src"].is_string())
                meta_by_src[jv["src"].get<std::string>()] = &jv;
        }
    }
    auto sget = [](const json *j, const char *k) -> std::string {
        return (j && j->contains(k) && (*j)[k].is_string())
                   ? (*j)[k].get<std::string>()
                   : std::string();
    };

    std::string sel;
    for (idx_t c = 0; c < ncol; c++) {
        if (bounds[c].dropped) {
            drops->push_back("using column \"" + src_names[c] + "\" dropped: " +
                             bounds[c].drop_reason);
            continue;
        }
        ViewCol vc;
        vc.name = stata_names[c];
        vc.kind = bounds[c].kind;
        vc.fmt = bounds[c].fmt;
        vc.normalized = true; /* MISS-1: boundary-normalized using column */
        const json *jm = nullptr;
        auto it = meta_by_src.find(src_names[c]);
        if (it != meta_by_src.end()) jm = it->second;
        std::string mfmt = sget(jm, "fmt");
        if (!mfmt.empty()) vc.fmt = mfmt;
        vc.varlab = sget(jm, "varlab");
        vc.vallab = sget(jm, "vallab");
        vc.meta_type = sget(jm, "type");
        if (!sel.empty()) sel += ", ";
        sel += bounds[c].sql + " AS " + quote_ident(vc.name);
        out->cols.push_back(vc);
    }
    if (out->cols.empty()) {
        *err = "no loadable columns in the using data";
        return kRcUsage;
    }
    out->select_sql = "SELECT " + sel + " FROM " + src.scan_sql;
    out->vallabs = meta_ctx.meta.present ? meta_ctx.meta.vallabs : json::object();
    return 0;
}

/* a named view as the using side: `using view:<name>`. The view's own
 * pending keep-in ranges are validated before its plan is embedded. */
ST_retcode using_from_view(Session &s, const std::string &vname,
                           View::UsingSide *out, std::string *err) {
    auto it = g_views.find(vname);
    if (it == g_views.end() || !it->second.live()) {
        *err = "no view named " + vname + " is open";
        return kRcUsage;
    }
    ST_retcode rc = validate_ranges(s, it->second, err);
    if (rc != 0) return rc;
    out->select_sql = it->second.compile(true);
    out->cols = it->second.cols();
    out->vallabs = it->second.vallabs();
    return 0;
}

/* dispatch one using entry: a parquet path, or view:<name> */
ST_retcode make_using(Session &s, const std::string &entry, View::UsingSide *out,
                      std::vector<std::string> *drops, std::string *err) {
    if (entry.rfind("view:", 0) == 0)
        return using_from_view(s, entry.substr(5), out, err);
    return prepare_using(s, {entry}, out, drops, err);
}

/* Stata merge uniqueness contracts: 1 side of m:1 / 1:m / both of 1:1. The
 * caller passes group keys ALREADY normalised with the same missing/empty/NaN
 * folding the join uses (key_norm in View::merge_with), so a third-party key
 * that is "" or NaN beside a NULL counts as a single Stata-missing group here
 * too. Without that, the guard could pass while the join still collapses those
 * keys together and over-matches them cartesian-style (MERGE-1). */
ST_retcode check_unique(Session &s, const std::string &rel_sql,
                        const std::vector<std::string> &norm_keys,
                        const char *side, std::string *err) {
    std::string keypart;
    for (size_t i = 0; i < norm_keys.size(); i++) {
        if (i) keypart += ", ";
        keypart += norm_keys[i];
    }
    std::string n;
    if (!s.query_scalar("SELECT count(*) FROM (SELECT " + keypart + " FROM (" +
                            rel_sql + ") GROUP BY " + keypart +
                            " HAVING count(*) > 1 LIMIT 1)",
                        &n, err))
        return kRcEngine;
    if (n != "0") {
        *err = std::string("the key does not uniquely identify observations in the ") +
               side + " data";
        return kRcUsage;
    }
    return 0;
}

} // namespace

ST_retcode cmd_view_twotable(const std::vector<std::string> &args) {
    std::string err;
    if (require_view(&err) != 0) {
        cry("parqit: " + err);
        return kRcUsage;
    }
    json req;
    if (!load_req(args, &req, &err)) {
        cry(err);
        return kRcUsage;
    }
    std::string op, tmpdir;
    std::vector<std::string> files;
    if (!parqit::req_text(req, "op", &op, &err) ||
        !parqit::req_text(req, "tmpdir", &tmpdir, &err) ||
        !parqit::req_text_list(req, "files", &files, &err)) {
        cry(err);
        return kRcUsage;
    }
    Session &s = Session::instance();
    s.set_default_temp_dir(tmpdir + parqit::spill_suffix());

    std::vector<std::string> drops, warns;
    ST_retcode rc;

    if (op == "append") {
        std::string gen;
        if (!parqit::req_text(req, "gen", &gen, &err, false)) {
            cry(err);
            return kRcUsage;
        }
        std::vector<View::UsingSide> sources;
        for (const auto &f : files) {
            View::UsingSide u;
            rc = make_using(s, f, &u, &drops, &err);
            if (rc != 0) {
                cry("parqit append: " + err);
                return rc;
            }
            sources.push_back(std::move(u));
        }
        std::string e = g_view_ref().append_with(std::move(sources), gen, &warns);
        if (!e.empty()) {
            cry("parqit append: " + e);
            return kRcUsage;
        }
    } else if (op == "merge" || op == "joinby") {
        std::vector<std::string> keys, keepusing;
        if (!parqit::req_text_list(req, "keys", &keys, &err)) {
            cry(err);
            return kRcUsage;
        }
        if (files.size() != 1) {
            cry("parqit " + op + ": exactly one using source");
            return kRcUsage;
        }
        View::UsingSide u;
        rc = make_using(s, files[0], &u, &drops, &err);
        if (rc != 0) {
            cry("parqit " + op + ": " + err);
            return rc;
        }
        if (op == "joinby") {
            std::string e = g_view_ref().joinby_with(keys, std::move(u), &warns);
            if (!e.empty()) {
                cry("parqit joinby: " + e);
                return kRcUsage;
            }
        } else {
            std::string kind, gen;
            bool nogen = req.value("nogen", false);
            int keep_mask = static_cast<int>(req.value("keep_mask", 0LL));
            if (!parqit::req_text(req, "kind", &kind, &err) ||
                !parqit::req_text(req, "gen", &gen, &err, false) ||
                !parqit::req_text_list(req, "keepusing", &keepusing, &err, false)) {
                cry(err);
                return kRcUsage;
            }
            /* uniqueness contracts before any plan mutation (loud, Stata-true).
             * Normalise each key exactly as the join does so a "" / NaN key that
             * the join folds to Stata-missing is folded here too (MERGE-1). */
            auto norm_key = [](const std::string &k, bool is_str) -> std::string {
                std::string q = quote_ident(k);
                if (is_str) return "nullif(" + q + ", '')";
                return "(CASE WHEN isnan(CAST(" + q +
                       " AS DOUBLE)) THEN NULL ELSE " + q + " END)";
            };
            std::map<std::string, char> mkind, ukind;
            for (const auto &c : g_view_ref().cols()) mkind[c.name] = c.kind;
            for (const auto &c : u.cols) ukind[c.name] = c.kind;
            std::vector<std::string> mkeys, ukeys;
            for (const auto &k : keys) {
                auto mit = mkind.find(k);
                auto uit = ukind.find(k);
                mkeys.push_back(norm_key(k, mit != mkind.end() && mit->second == 's'));
                ukeys.push_back(norm_key(k, uit != ukind.end() && uit->second == 's'));
            }
            if (kind == "1:1" || kind == "1:m") {
                rc = check_unique(s, g_view_ref().compile(false), mkeys, "master", &err);
                if (rc != 0) {
                    cry("parqit merge: " + err);
                    return rc;
                }
            }
            if (kind == "1:1" || kind == "m:1") {
                rc = check_unique(s, u.select_sql, ukeys, "using", &err);
                if (rc != 0) {
                    cry("parqit merge: " + err);
                    return rc;
                }
            }
            std::string e = g_view_ref().merge_with(kind, keys, std::move(u), keepusing,
                                              keep_mask, gen, nogen, &warns);
            if (!e.empty()) {
                cry("parqit merge: " + e);
                return kRcUsage;
            }
        }
    } else {
        cry("parqit: unknown two-table operation '" + op + "'");
        return kRcUsage;
    }

    for (const auto &d : drops) cry("warning: " + d);
    for (const auto &w : warns) cry("note: " + w);
    save_local("_parqit_view_k", std::to_string(g_view_ref().cols().size()));
    return 0;
}

} // namespace parqit_plugin

/* ======================================================= M4 commands ===== */

namespace parqit_plugin {

/* The wide-pivot j scan shared by reshape wide and pivot, run against the
 * current compiled view. Contract (pinned by v34 and the reshape suites):
 *   - a missing j has no wide column, so its stub values would silently
 *     vanish from the j-value scan. Native Stata stops with r(498)
 *     "variable <j> contains missing values" (for a NULL, NaN or "" j alike —
 *     verified natively); do the same, loudly, before building the pivot;
 *   - with check_dup, Stata's (i, j) uniqueness contract is enforced
 *     (reshape wide needs it; pivot's aggregation guarantees it);
 *   - RESHAPE-WIDE-COLORDER: numeric j must order numerically (2,10,11), not
 *     by the lexicographic VARCHAR cast (10,11,2), to match native reshape
 *     wide; string j keeps alphabetic order (Stata does too); a numeric
 *     cast's trailing ".0" is stripped so suffixes read 2019, not 2019.0;
 *   - more than 2000 distinct values refuse to run (cap_hint says what to
 *     do about it, per verb).
 * Messages cry under `verb` (engine failures) / `wide_verb` (contract
 * violations), with `jdesc` naming the pivoted values in the cap message.
 * On success fills jvals/j_is_string and returns 0. */
static ST_retcode wide_j_scan(const std::string &jname,
                              const std::vector<std::string> &ivars,
                              bool check_dup, const std::string &verb,
                              const std::string &wide_verb,
                              const std::string &jdesc,
                              const std::string &cap_hint,
                              std::vector<std::string> *jvals,
                              bool *j_is_string) {
    Session &s = Session::instance();
    std::string err;
    *j_is_string = false;
    for (const auto &c : g_view_ref().cols())
        if (c.name == jname) *j_is_string = (c.kind == 's');

    const std::string base = "(" + g_view_ref().compile(false) + ")";
    const std::string jq = quote_ident(jname);

    {
        std::string jmiss = *j_is_string
                                ? "coalesce(" + jq + ", '') = ''"
                                : jq + " IS NULL OR isnan(CAST(" + jq +
                                      " AS DOUBLE))";
        std::string nmiss;
        if (!s.query_scalar("SELECT count(*) FROM " + base + " WHERE " + jmiss,
                            &nmiss, &err)) {
            cry("parqit " + verb + ": " + err);
            return kRcEngine;
        }
        if (nmiss != "0") {
            cry("parqit " + wide_verb + ": variable " + jname +
                " contains missing values");
            return kRcUsage;
        }
    }

    if (check_dup) {
        std::string ipart;
        for (size_t i = 0; i < ivars.size(); i++)
            ipart += (i ? ", " : "") + quote_ident(ivars[i]);
        std::string dup;
        if (!s.query_scalar("SELECT count(*) FROM (SELECT 1 FROM " + base +
                                " GROUP BY " + ipart + ", " + jq +
                                " HAVING count(*) > 1 LIMIT 1)",
                            &dup, &err)) {
            cry("parqit " + verb + ": " + err);
            return kRcEngine;
        }
        if (dup != "0") {
            cry("parqit " + wide_verb + ": values of " + jname +
                " are not unique within i() groups");
            return kRcUsage;
        }
    }

    duckdb_result res;
    std::string jquery =
        *j_is_string
            ? "SELECT DISTINCT CAST(" + jq + " AS VARCHAR) AS v FROM " + base +
                  " WHERE " + jq + " IS NOT NULL ORDER BY v"
            : "SELECT DISTINCT CAST(" + jq + " AS VARCHAR) AS v, " + jq +
                  " AS jn FROM " + base + " WHERE " + jq +
                  " IS NOT NULL ORDER BY jn";
    if (!s.query(jquery, &res, &err)) {
        cry("parqit " + verb + ": " + err);
        return kRcEngine;
    }
    jvals->clear();
    idx_t n = duckdb_row_count(&res);
    for (idx_t r = 0; r < n; r++) {
        char *v = duckdb_value_varchar(&res, 0, r);
        if (v) {
            jvals->push_back(v);
            duckdb_free(v);
        }
    }
    duckdb_destroy_result(&res);
    if (jvals->size() > 2000) {
        cry("parqit " + wide_verb + ": " + std::to_string(jvals->size()) +
            " distinct " + jdesc + " would create too many columns" + cap_hint);
        return kRcUsage;
    }
    /* numeric j: strip any trailing .0 the VARCHAR cast may add */
    if (!*j_is_string) {
        for (auto &v : *jvals) {
            size_t dot = v.find(".0");
            if (dot != std::string::npos && dot + 2 == v.size()) v.resize(dot);
        }
    }
    return 0;
}

ST_retcode cmd_view_reshape(const std::vector<std::string> &args) {
    std::string err;
    if (require_view(&err) != 0) {
        cry("parqit reshape: " + err);
        return kRcUsage;
    }
    json req;
    if (!load_req(args, &req, &err)) {
        cry(err);
        return kRcUsage;
    }
    std::string dir, jname;
    std::vector<std::string> stubs, ivars;
    if (!parqit::req_text(req, "dir", &dir, &err) ||
        !parqit::req_text(req, "j", &jname, &err) ||
        !parqit::req_text_list(req, "stubs", &stubs, &err) ||
        !parqit::req_text_list(req, "i", &ivars, &err)) {
        cry(err);
        return kRcUsage;
    }
    if (dir == "long") {
        /* Stata's contract: i() uniquely identifies the wide observations.
         * Accepting duplicates would silently fabricate long data, so check
         * eagerly (one aggregation pass), exactly like wide does for (i,j). */
        if (!ivars.empty()) {
            Session &si = Session::instance();
            std::string ipart;
            for (size_t i = 0; i < ivars.size(); i++)
                ipart += (i ? ", " : "") + quote_ident(ivars[i]);
            std::string dup;
            if (!si.query_scalar(
                    "SELECT count(*) FROM (SELECT 1 FROM (" +
                        g_view_ref().compile(false) + ") GROUP BY " + ipart +
                        " HAVING count(*) > 1 LIMIT 1)",
                    &dup, &err)) {
                cry("parqit reshape: " + err);
                return kRcEngine;
            }
            if (dup != "0") {
                cry("parqit reshape long: i() variables do not uniquely "
                    "identify the observations (Stata's reshape contract); "
                    "deduplicate or collapse first");
                return kRcUsage;
            }
        }
        std::string e = g_view_ref().reshape_long(stubs, ivars, jname);
        if (!e.empty()) {
            cry("parqit reshape: " + e);
            return kRcUsage;
        }
        save_local("_parqit_view_k", std::to_string(g_view_ref().cols().size()));
        return 0;
    }
    if (dir != "wide") {
        cry("parqit reshape: direction must be long or wide");
        return kRcUsage;
    }

    /* wide needs the j values and Stata's (i, j) uniqueness contract */
    std::vector<std::string> jvals;
    bool j_is_string = false;
    ST_retcode jrc =
        wide_j_scan(jname, ivars, /*check_dup=*/true, "reshape", "reshape wide",
                    "j values", "; collapse first", &jvals, &j_is_string);
    if (jrc != 0) return jrc;
    std::string e = g_view_ref().reshape_wide(stubs, ivars, jname, jvals, j_is_string);
    if (!e.empty()) {
        cry("parqit reshape: " + e);
        return kRcUsage;
    }
    save_local("_parqit_view_k", std::to_string(g_view_ref().cols().size()));
    return 0;
}

/* ========================================================= view_pivot ===== */

/* parqit pivot — an Excel-style pivot table as sugar over the two verbs the
 * engine already trusts: collapse the (stat) specs by (rows, col), then
 * spread col's values into wide columns (reshape wide of the collapse
 * targets, i(rows) j(col)). Aggregating first guarantees reshape's (i, j)
 * uniqueness contract by construction, so only the missing-col and
 * column-count scans run here. The two stages land atomically: the view is
 * snapshotted up front and restored on any failure (an illegal generated
 * name, >2000 columns, missing col values), never left half-pivoted. */
ST_retcode cmd_view_pivot(const std::vector<std::string> &args) {
    std::string err;
    if (require_view(&err) != 0) {
        cry("parqit pivot: " + err);
        return kRcUsage;
    }
    json req;
    if (!load_req(args, &req, &err)) {
        cry(err);
        return kRcUsage;
    }
    std::string col;
    std::vector<std::string> rows;
    if (!parqit::req_text(req, "col", &col, &err) ||
        !parqit::req_text_list(req, "rows", &rows, &err)) {
        cry(err);
        return kRcUsage;
    }
    std::vector<View::CollapseSpec> specs;
    if (req.contains("specs") && req["specs"].is_array()) {
        for (const auto &js : req["specs"]) {
            View::CollapseSpec sp;
            std::string serr;
            if (!parqit::req_text(js, "stat", &sp.stat, &serr) ||
                !parqit::req_text(js, "target", &sp.target, &serr, false) ||
                !parqit::req_text(js, "source", &sp.source, &serr)) {
                cry(serr);
                return kRcUsage;
            }
            specs.push_back(sp);
        }
    }
    if (specs.empty()) {
        cry("parqit pivot: nothing to compute — give at least one "
            "(statistic) source variable");
        return kRcUsage;
    }
    if (rows.empty()) {
        cry("parqit pivot: rows() required");
        return kRcUsage;
    }

    /* rows() may hold wildcards (as collapse by() does); reshape's i() takes
     * literal names, so expand once and hand both stages the same list */
    std::vector<std::string> rowv;
    std::string e = g_view_ref().expand_patterns(rows, &rowv);
    if (!e.empty()) {
        cry("parqit pivot: " + e);
        return kRcUsage;
    }
    for (const auto &rv : rowv)
        if (rv == col) {
            cry("parqit pivot: variable " + col +
                " cannot be in both rows() and cols()");
            return kRcUsage;
        }

    View saved = g_view_ref(); /* both stages land, or neither */

    std::vector<std::string> by = rowv;
    by.push_back(col);
    e = g_view_ref().collapse(specs, by);
    if (!e.empty()) {
        g_view_ref() = saved;
        cry("parqit pivot: " + e);
        return kRcUsage;
    }

    std::vector<std::string> jvals;
    bool j_is_string = false;
    ST_retcode jrc = wide_j_scan(col, {}, /*check_dup=*/false, "pivot", "pivot",
                                 "cols() values",
                                 "; use a coarser cols() variable", &jvals,
                                 &j_is_string);
    if (jrc != 0) {
        g_view_ref() = saved;
        return jrc;
    }

    std::vector<std::string> stubs;
    for (const auto &sp : specs)
        stubs.push_back(sp.target.empty() ? sp.source : sp.target);
    e = g_view_ref().reshape_wide(stubs, rowv, col, jvals, j_is_string);
    if (!e.empty()) {
        g_view_ref() = saved;
        cry("parqit pivot: " + e);
        return kRcUsage;
    }
    save_local("_parqit_view_k", std::to_string(g_view_ref().cols().size()));
    return 0;
}

ST_retcode cmd_view_sql(const std::vector<std::string> &args) {
    std::string err;
    json req;
    if (!load_req(args, &req, &err)) {
        cry(err);
        return kRcUsage;
    }
    std::string sql, tmpdir, vname;
    if (!parqit::req_text(req, "sql", &sql, &err) ||
        !parqit::req_text(req, "name", &vname, &err, false) ||
        !parqit::req_text(req, "tmpdir", &tmpdir, &err)) {
        cry(err);
        return kRcUsage;
    }
    if (!vname.empty()) g_current = vname;
    Session &s = Session::instance();
    s.set_default_temp_dir(tmpdir + parqit::spill_suffix());

    /* probe the query, then boundary-cast its result like any source */
    duckdb_result res;
    if (!s.query("SELECT * FROM (" + sql + ") LIMIT 0", &res, &err)) {
        cry("parqit sql: " + err);
        return kRcEngine;
    }
    idx_t ncol = duckdb_column_count(&res);
    std::vector<std::string> src_names(ncol);
    std::vector<BoundaryCol> bounds(ncol);
    for (idx_t c = 0; c < ncol; c++) {
        const char *nm = duckdb_column_name(&res, c);
        src_names[c] = nm ? nm : "";
        duckdb_logical_type lt = duckdb_column_logical_type(&res, c);
        bounds[c] = boundary_for(src_names[c], lt);
        duckdb_destroy_logical_type(&lt);
    }
    duckdb_destroy_result(&res);

    std::vector<std::string> stata_names = parqit::sanitize_unique(src_names, nullptr);
    std::string sel;
    std::vector<ViewCol> cols;
    std::vector<std::string> drops;
    for (idx_t c = 0; c < ncol; c++) {
        if (bounds[c].dropped) {
            drops.push_back("column \"" + src_names[c] + "\" dropped: " +
                            bounds[c].drop_reason);
            continue;
        }
        ViewCol vc;
        vc.name = stata_names[c];
        vc.kind = bounds[c].kind;
        vc.fmt = bounds[c].fmt;
        vc.note = bounds[c].note;
        vc.normalized = true; /* MISS-1: boundary-normalized query/sql column */
        if (!sel.empty()) sel += ", ";
        sel += bounds[c].sql + " AS " + quote_ident(vc.name);
        cols.push_back(vc);
    }
    if (cols.empty()) {
        cry("parqit sql: the query produced no loadable columns");
        return kRcUsage;
    }
    g_view_ref().open("SELECT " + sel + " FROM (" + sql + ")", cols, json::object(),
                json::object(), "", "parqit sql query");
    for (const auto &d : drops) cry("warning: " + d);
    save_local("_parqit_view_k", std::to_string(g_view_ref().cols().size()));
    save_local("_parqit_view_name", parqit::hex_encode(g_current));
    return 0;
}

ST_retcode cmd_view_query(const std::vector<std::string> &args) {
    std::string err;
    if (require_view(&err) != 0) {
        cry("parqit query: " + err);
        return kRcUsage;
    }
    json req;
    if (!load_req(args, &req, &err)) {
        cry(err);
        return kRcUsage;
    }
    std::string frag;
    if (!parqit::req_text(req, "fragment", &frag, &err)) {
        cry(err);
        return kRcUsage;
    }
    View candidate = g_view_ref();
    std::string e = candidate.raw_fragment(frag);
    if (!e.empty()) {
        cry("parqit query: " + e);
        return kRcUsage;
    }
    /* fail fast on a staged copy: the fragment must compile before the live
     * view is mutated, so a bad exploratory query never discards the user's
     * existing lazy pipeline. */
    Session &s = Session::instance();
    std::string verr;
    duckdb_result res;
    if (!s.query("SELECT * FROM (" + candidate.compile(false) + ") LIMIT 0", &res,
                 &verr)) {
        cry("parqit query: the fragment does not compile: " + verr);
        return kRcUsage;
    }
    duckdb_destroy_result(&res);
    g_view_ref() = std::move(candidate);
    save_local("_parqit_view_k", std::to_string(g_view_ref().cols().size()));
    return 0;
}

ST_retcode cmd_view_stats(const std::vector<std::string> &args) {
    std::string err;
    if (require_view(&err) != 0) {
        cry("parqit: " + err);
        return kRcUsage;
    }
    json req;
    if (!load_req(args, &req, &err)) {
        cry(err);
        return kRcUsage;
    }
    std::string what, respfile;
    std::vector<std::string> vars;
    if (!parqit::req_text(req, "what", &what, &err) ||
        !parqit::req_text(req, "respfile", &respfile, &err) ||
        !parqit::req_text_list(req, "vars", &vars, &err, false)) {
        cry(err);
        return kRcUsage;
    }
    Session &s = Session::instance();
    const std::string base = "(" + g_view_ref().compile(false) + ")";
    parqit::ResponseWriter w;
    if (!w.open(respfile, &err)) {
        cry(err);
        return kRcEngine;
    }

    if (what == "summarize") {
        std::vector<std::string> targets;
        if (vars.empty()) {
            for (const auto &c : g_view_ref().cols())
                if (c.kind == 'n') targets.push_back(c.name);
        } else {
            for (const auto &v : vars) {
                bool found = false, isnum = false;
                for (const auto &c : g_view_ref().cols())
                    if (c.name == v) {
                        found = true;
                        isnum = (c.kind == 'n');
                    }
                if (!found) {
                    cry("parqit summarize: variable " + v + " not found in the view");
                    return kRcVarNotFound;
                }
                if (isnum) targets.push_back(v);
            }
        }
        std::string sel;
        for (const auto &t : targets) {
            const std::string r = quote_ident(t);
            if (!sel.empty()) sel += ", ";
            sel += "count(" + r + "), avg(" + r + "), stddev_samp(" + r +
                   "), min(" + r + "), max(" + r + ")";
        }
        if (targets.empty()) {
            cry("parqit summarize: no numeric variables to summarize");
            return kRcUsage;
        }
        duckdb_result res;
        if (!s.query("SELECT " + sel + " FROM " + base, &res, &err)) {
            cry("parqit summarize: " + err);
            return kRcEngine;
        }
        for (size_t t = 0; t < targets.size(); t++) {
            auto cell = [&](idx_t c) -> std::string {
                if (duckdb_value_is_null(&res, t * 5 + c, 0)) return ".";
                char *v = duckdb_value_varchar(&res, t * 5 + c, 0);
                std::string out = v ? v : ".";
                if (v) duckdb_free(v);
                return out;
            };
            w.rec("stat", {cell(0), cell(1), cell(2), cell(3), cell(4)},
                  {targets[t]});
        }
        duckdb_destroy_result(&res);
    } else if (what == "misstable") {
        /* missing per variable: numerics count NULLs; strings count ""
         * (NULL≡"" by parqit's string contract) */
        std::vector<std::pair<std::string, char>> targets;
        for (const auto &c : g_view_ref().cols()) {
            if (vars.empty()) {
                targets.push_back({c.name, c.kind});
            } else {
                for (const auto &v : vars)
                    if (v == c.name) targets.push_back({c.name, c.kind});
            }
        }
        if (targets.empty()) {
            cry("parqit misstable: no matching variables in the view");
            return kRcVarNotFound;
        }
        std::string sel = "count(*)";
        std::string allnm;
        for (const auto &t : targets) {
            const std::string r = quote_ident(t.first);
            if (t.second == 's') {
                sel += ", count(*) FILTER (WHERE " + r + " IS NULL OR " + r +
                       " = '')";
                allnm += (allnm.empty() ? "" : " AND ") + ("NOT (" + r +
                         " IS NULL OR " + r + " = '')");
            } else {
                sel += ", count(*) - count(" + r + ")";
                allnm += (allnm.empty() ? "" : " AND ") + r + " IS NOT NULL";
            }
        }
        /* complete observations: rows non-missing across ALL selected vars */
        sel += ", count(*) FILTER (WHERE " + allnm + ")";
        duckdb_result res;
        if (!s.query("SELECT " + sel + " FROM " + base, &res, &err)) {
            cry("parqit misstable: " + err);
            return kRcEngine;
        }
        long long total = duckdb_value_int64(&res, 0, 0);
        for (size_t t = 0; t < targets.size(); t++) {
            w.rec("miss",
                  {std::to_string(duckdb_value_int64(&res, t + 1, 0)),
                   std::to_string(total)},
                  {targets[t].first});
        }
        long long ncomplete =
            duckdb_value_int64(&res, targets.size() + 1, 0);
        duckdb_destroy_result(&res);
        save_local("_parqit_n", std::to_string(total));
        save_local("_parqit_n_complete", std::to_string(ncomplete));
    } else if (what == "detail") {
        /* summarize, detail: Stata's exact percentile rule plus central
         * moments computed against a per-variable mean subquery (no
         * catastrophic cancellation from raw-moment expansion) */
        std::vector<std::string> targets;
        if (vars.empty()) {
            for (const auto &c : g_view_ref().cols())
                if (c.kind == 'n') targets.push_back(c.name);
        } else {
            for (const auto &v : vars) {
                bool found = false, isnum = false;
                for (const auto &c : g_view_ref().cols())
                    if (c.name == v) {
                        found = true;
                        isnum = (c.kind == 'n');
                    }
                if (!found) {
                    cry("parqit summarize: variable " + v + " not found in the view");
                    return kRcVarNotFound;
                }
                if (!isnum) {
                    cry("parqit summarize: " + v + " is a string variable");
                    return kRcUsage;
                }
                targets.push_back(v);
            }
        }
        if (targets.empty()) {
            cry("parqit summarize, detail: no numeric variables in the view");
            return kRcUsage;
        }
        static const int kPcts[] = {1, 5, 10, 25, 50, 75, 90, 95, 99};
        /* PERF-DET-1: two scans TOTAL for all variables, was 1+ scans per
         * variable with a per-variable mean subquery and NINE single-threaded
         * list_sort(list(x)) materialisations. Pass 1 gets count/mean/min/max
         * for every variable in one scan; pass 2 computes the central moments
         * with the mean injected as an exact dtoa literal (identical two-pass
         * math, no cancellation) and the Stata percentiles as exact order
         * statistics via quantile_disc — verified against the pinned DuckDB
         * source (quantile_sort_tree.hpp): the discrete index is
         * n - floor(n - n*q), so q = (k - 0.5)/n selects exactly the k-th
         * smallest with a 0.5 float-safety margin, computed by a parallel
         * nth_element instead of a full sort. Values are byte-identical to
         * the list_sort form: the same data elements, and the Stata rule
         * (np = n*p/100; integral -> mean of neighbours, else ceil) is applied
         * in exact integer arithmetic below. CAST to DOUBLE loses nothing
         * observable: every summarize result lands in a double r() scalar. */
        std::string sel1;
        for (const auto &t : targets) {
            const std::string r = quote_ident(t);
            if (!sel1.empty()) sel1 += ", ";
            sel1 += "count(" + r + "), avg(" + r + "), min(" + r + "), max(" +
                    r + ")";
        }
        duckdb_result res1;
        if (!s.query("SELECT " + sel1 + " FROM " + base, &res1, &err)) {
            cry("parqit summarize: " + err);
            return kRcEngine;
        }
        struct DetVar {
            long long n = 0;
            std::string cnt = ".", mean = ".", mn = ".", mx = ".";
            double mu = 0;
            /* per percentile: the 1-based ranks the Stata rule needs */
            std::vector<std::pair<long long, long long>> ranks; /* k1,k2 (k2=0: single) */
            std::vector<long long> want;                        /* deduped rank list */
        };
        std::vector<DetVar> dv(targets.size());
        auto txt1 = [&](idx_t c) -> std::string {
            if (duckdb_value_is_null(&res1, c, 0)) return ".";
            char *v = duckdb_value_varchar(&res1, c, 0);
            std::string out = v ? v : ".";
            if (v) duckdb_free(v);
            return out;
        };
        for (size_t t = 0; t < targets.size(); t++) {
            dv[t].n = duckdb_value_int64(&res1, t * 4, 0);
            dv[t].cnt = txt1(t * 4);
            dv[t].mean = txt1(t * 4 + 1);
            dv[t].mn = txt1(t * 4 + 2);
            dv[t].mx = txt1(t * 4 + 3);
            dv[t].mu = duckdb_value_double(&res1, t * 4 + 1, 0);
            const long long n = dv[t].n;
            if (n <= 0) continue;
            for (int p : kPcts) {
                const long long np100 = n * p; /* exact: n*99 < 2^63 for any real n */
                long long k1, k2 = 0;
                if (np100 % 100 == 0) {
                    k1 = np100 / 100;
                    k2 = k1 + 1 > n ? k1 : k1 + 1;
                    if (k2 == k1) k2 = 0;
                } else {
                    k1 = np100 / 100 + 1; /* ceil */
                }
                dv[t].ranks.push_back({k1, k2});
                dv[t].want.push_back(k1);
                if (k2) dv[t].want.push_back(k2);
            }
            std::sort(dv[t].want.begin(), dv[t].want.end());
            dv[t].want.erase(std::unique(dv[t].want.begin(), dv[t].want.end()),
                             dv[t].want.end());
        }
        duckdb_destroy_result(&res1);

        /* pass 2: central moments for every variable with data, one scan */
        std::vector<size_t> live;
        std::string sel2;
        for (size_t t = 0; t < targets.size(); t++) {
            if (dv[t].n <= 0) continue;
            live.push_back(t);
            const std::string r = quote_ident(targets[t]);
            const std::string mu = parqit::dtoa(dv[t].mu);
            if (!sel2.empty()) sel2 += ", ";
            sel2 += "stddev_samp(" + r + "), var_samp(" + r + "), avg(pow(" +
                    r + " - " + mu + ", 2)), avg(pow(" + r + " - " + mu +
                    ", 3)), avg(pow(" + r + " - " + mu + ", 4))";
        }
        duckdb_result res2;
        bool have2 = false;
        if (!live.empty()) {
            if (!s.query("SELECT " + sel2 + " FROM " + base, &res2, &err)) {
                cry("parqit summarize: " + err);
                return kRcEngine;
            }
            have2 = true;
        }

        /* order statistics per variable: a CTAS through the PARALLEL sort
         * operator plus O(1) rowid point-picks — measured ~6x faster than a
         * quantile_disc/list_sort aggregate, whose finalize is effectively
         * single-threaded on 10M rows. rowid on a fresh CTAS is insertion
         * order, and preserve_insertion_order (engine default, untouched)
         * makes insertion order the ORDER BY order. A multi-stage pipeline is
         * materialised ONCE into a scratch projection so k variables do not
         * re-run the whole pipeline k times. */
        std::string psrc = base;
        const bool staged = g_view_ref().n_stages() > 0;
        if (staged && !live.empty()) {
            std::string cols;
            for (size_t t : live) {
                if (!cols.empty()) cols += ", ";
                cols += quote_ident(targets[t]);
            }
            std::string derr;
            s.exec("DROP TABLE IF EXISTS __parqit_sumdet_src", &derr);
            if (!s.exec("CREATE TEMP TABLE __parqit_sumdet_src AS SELECT " +
                            cols + " FROM " + base,
                        &err)) {
                cry("parqit summarize: " + err);
                return kRcEngine;
            }
            psrc = "__parqit_sumdet_src";
        }
        std::map<size_t, std::map<long long, double>> osall;
        for (size_t t : live) {
            const std::string r = quote_ident(targets[t]);
            std::string derr;
            s.exec("DROP TABLE IF EXISTS __parqit_sumdet_srt", &derr);
            if (!s.exec("CREATE TEMP TABLE __parqit_sumdet_srt AS SELECT " + r +
                            " AS x FROM " + psrc + " WHERE " + r +
                            " IS NOT NULL ORDER BY " + r,
                        &err)) {
                cry("parqit summarize: " + err);
                return kRcEngine;
            }
            std::string ids;
            for (long long k : dv[t].want) {
                if (!ids.empty()) ids += ", ";
                ids += std::to_string(k - 1); /* rowid is 0-based */
            }
            duckdb_result pres;
            if (!s.query("SELECT rowid, x FROM __parqit_sumdet_srt WHERE "
                         "rowid IN (" + ids + ")",
                         &pres, &err)) {
                cry("parqit summarize: " + err);
                return kRcEngine;
            }
            idx_t pn = duckdb_row_count(&pres);
            for (idx_t i = 0; i < pn; i++)
                osall[t][duckdb_value_int64(&pres, 0, i) + 1] =
                    duckdb_value_double(&pres, 1, i);
            duckdb_destroy_result(&pres);
            s.exec("DROP TABLE IF EXISTS __parqit_sumdet_srt", &derr);
        }
        if (staged && !live.empty()) {
            std::string derr;
            s.exec("DROP TABLE IF EXISTS __parqit_sumdet_src", &derr);
        }

        idx_t col2 = 0;
        auto txt2 = [&](idx_t c) -> std::string {
            if (duckdb_value_is_null(&res2, c, 0)) return ".";
            char *v = duckdb_value_varchar(&res2, c, 0);
            std::string out = v ? v : ".";
            if (v) duckdb_free(v);
            return out;
        };
        for (size_t t = 0; t < targets.size(); t++) {
            if (dv[t].n <= 0) {
                /* all-missing variable: everything but the zero count is . */
                std::vector<std::string> plain = {dv[t].cnt, ".", ".", ".",
                                                  ".",       ".", ".", "."};
                for (size_t p = 0; p < 9; p++) plain.push_back(".");
                w.rec("det", plain, {targets[t]});
                continue;
            }
            const std::string sd = txt2(col2 + 0), var = txt2(col2 + 1);
            double m2 = duckdb_value_double(&res2, col2 + 2, 0);
            bool m2null = duckdb_value_is_null(&res2, col2 + 2, 0);
            double m3 = duckdb_value_double(&res2, col2 + 3, 0);
            double m4 = duckdb_value_double(&res2, col2 + 4, 0);
            /* locale-independent: a comma-decimal locale would otherwise
             * break the Stata-side strtoreal of these returned statistics */
            std::string skew = ".", kurt = ".";
            if (!m2null && m2 > 0) {
                skew = parqit::dtoa(m3 / std::pow(m2, 1.5));
                kurt = parqit::dtoa(m4 / (m2 * m2));
            }
            std::map<long long, double> &os = osall[t];
            std::vector<std::string> plain = {dv[t].cnt, dv[t].mean, sd,
                                              var,       skew,      kurt,
                                              dv[t].mn,  dv[t].mx};
            for (const auto &rk : dv[t].ranks) {
                double v = rk.second ? (os[rk.first] + os[rk.second]) / 2.0
                                     : os[rk.first];
                plain.push_back(parqit::dtoa(v));
            }
            w.rec("det", plain, {targets[t]});
            col2 += 5;
        }
        if (have2) duckdb_destroy_result(&res2);
    } else if (what == "levelsof") {
        if (vars.size() != 1) {
            cry("parqit levelsof: exactly one variable");
            return kRcUsage;
        }
        char kind = 0;
        for (const auto &c : g_view_ref().cols())
            if (c.name == vars[0]) kind = c.kind;
        if (kind == 0) {
            cry("parqit levelsof: variable " + vars[0] + " not found in the view");
            return kRcVarNotFound;
        }
        long long limit = req.value("limit", 5000LL);
        const std::string r = quote_ident(vars[0]);
        /* numeric levels print like Stata: integers without a trailing .0 */
        std::string expr = (kind == 's') ? r : stata_num_varchar(r);
        std::string where = (kind == 's') ? (r + " IS NOT NULL AND " + r + " <> ''")
                                          : (r + " IS NOT NULL");
        duckdb_result res;
        if (!s.query("SELECT DISTINCT " + expr + " AS v FROM " + base + " WHERE " +
                         where + " ORDER BY " + r + " LIMIT " +
                         std::to_string(limit + 1),
                     &res, &err)) {
            cry("parqit levelsof: " + err);
            return kRcEngine;
        }
        idx_t n = duckdb_row_count(&res);
        if (static_cast<long long>(n) > limit) {
            cry("parqit levelsof: more than " + std::to_string(limit) +
                " distinct levels; raise limit() or collapse instead");
            duckdb_destroy_result(&res);
            return kRcUsage;
        }
        for (idx_t i = 0; i < n; i++) {
            char *v = duckdb_value_varchar(&res, 0, i);
            w.rec("lvl", {}, {v ? v : ""});
            if (v) duckdb_free(v);
        }
        duckdb_destroy_result(&res);
        save_local("_parqit_n_levels", std::to_string(n));
        save_local("_parqit_lvl_kind", std::string(1, kind ? kind : 'n'));
    } else if (what == "tab2") {
        if (vars.size() != 2) {
            cry("parqit tabulate: two variables for a two-way table");
            return kRcUsage;
        }
        for (const auto &v : vars) {
            bool found = false;
            for (const auto &c : g_view_ref().cols()) found = found || c.name == v;
            if (!found) {
                cry("parqit tabulate: variable " + v + " not found in the view");
                return kRcVarNotFound;
            }
        }
        const std::string r1 = quote_ident(vars[0]), r2 = quote_ident(vars[1]);
        bool incmiss = req.value("missing", false);
        char k1 = 'n', k2 = 'n';
        for (const auto &c : g_view_ref().cols()) {
            if (c.name == vars[0]) k1 = c.kind;
            if (c.name == vars[1]) k2 = c.kind;
        }
        std::string base2 = base;
        if (!incmiss) {
            auto nn = [&](const std::string &rr, char kk) {
                return (kk == 's') ? rr + " IS NOT NULL AND " + rr + " <> ''"
                                   : rr + " IS NOT NULL";
            };
            base2 = "(SELECT * FROM " + base + " WHERE " + nn(r1, k1) + " AND " +
                    nn(r2, k2) + ")";
        }
        /* TAB-FLOAT-1: integer-valued numeric labels print without a .0 */
        const std::string v1 = (k1 == 's') ? r1 : stata_num_varchar(r1);
        const std::string v2 = (k2 == 's') ? r2 : stata_num_varchar(r2);
        duckdb_result res;
        if (!s.query("SELECT " + v1 + ", " + v2 +
                         ", count(*) FROM " + base2 + " GROUP BY " + r1 +
                         ", " + r2 + " ORDER BY " + r1 + " NULLS LAST, " + r2 +
                         " NULLS LAST",
                     &res, &err)) {
            cry("parqit tabulate: " + err);
            return kRcEngine;
        }
        idx_t n = duckdb_row_count(&res);
        /* PERF-TAB2-PRECOUNT-1: derive the distinct-r2 count from the already
         * materialised (cell-bounded) GROUP BY result instead of a second full
         * scan (count(DISTINCT) over the data). count(DISTINCT) ignores NULL, so
         * skip NULL column-1 cells to match the prior >30 check exactly. */
        {
            std::set<std::string> distinct_c;
            for (idx_t i = 0; i < n; i++) {
                if (duckdb_value_is_null(&res, 1, i)) continue;
                char *v = duckdb_value_varchar(&res, 1, i);
                if (v) { distinct_c.insert(v); duckdb_free(v); }
            }
            if (distinct_c.size() > 30) {
                cry("parqit tabulate: " + vars[1] + " has more than 30 distinct values; "
                    "swap the variables or collapse instead");
                duckdb_destroy_result(&res);
                return kRcUsage;
            }
        }
        if (n > 10000) {
            cry("parqit tabulate: more than 10,000 cells; collapse instead");
            duckdb_destroy_result(&res);
            return kRcUsage;
        }
        for (idx_t i = 0; i < n; i++) {
            auto sv = [&](idx_t c) -> std::string {
                if (duckdb_value_is_null(&res, c, i)) return ".";
                char *v = duckdb_value_varchar(&res, c, i);
                std::string out = v ? v : ".";
                if (v) duckdb_free(v);
                return out;
            };
            w.rec("t2", {std::to_string(duckdb_value_int64(&res, 2, i))},
                  {sv(0), sv(1)});
        }
        duckdb_destroy_result(&res);
    } else if (what == "tabulate") {
        if (vars.size() != 1) {
            cry("parqit tabulate: exactly one variable");
            return kRcUsage;
        }
        bool found = false;
        for (const auto &c : g_view_ref().cols()) found = found || c.name == vars[0];
        if (!found) {
            cry("parqit tabulate: variable " + vars[0] + " not found in the view");
            return kRcVarNotFound;
        }
        const std::string r = quote_ident(vars[0]);
        bool incmiss = req.value("missing", false);
        char k1 = 'n';
        for (const auto &c : g_view_ref().cols())
            if (c.name == vars[0]) k1 = c.kind;
        std::string where = incmiss ? std::string(" ")
                            : (k1 == 's' ? " WHERE " + r + " IS NOT NULL AND " + r +
                                               " <> '' "
                                         : " WHERE " + r + " IS NOT NULL ");
        /* TAB-FLOAT-1: render integer-valued numeric levels without a trailing
         * .0 (11, not 11.0), matching native tabulate and parqit levelsof. */
        const std::string vexpr = (k1 == 's') ? r : stata_num_varchar(r);
        duckdb_result res;
        if (!s.query("SELECT " + vexpr + " AS v, count(*) AS n FROM " +
                         base + where + " GROUP BY " + r + " ORDER BY " + r +
                         " NULLS LAST",
                     &res, &err)) {
            cry("parqit tabulate: " + err);
            return kRcEngine;
        }
        idx_t n = duckdb_row_count(&res);
        if (n > 10000) {
            cry("parqit tabulate: more than 10,000 distinct values; collapse instead");
            duckdb_destroy_result(&res);
            return kRcUsage;
        }
        for (idx_t i = 0; i < n; i++) {
            std::string val = ".";
            if (!duckdb_value_is_null(&res, 0, i)) {
                char *v = duckdb_value_varchar(&res, 0, i);
                val = v ? v : ".";
                if (v) duckdb_free(v);
            }
            w.rec("tab", {std::to_string(duckdb_value_int64(&res, 1, i))}, {val});
        }
        duckdb_destroy_result(&res);
    } else if (what == "countif") {
        std::string expr;
        if (!parqit::req_text(req, "expr", &expr, &err)) {
            cry(err);
            return kRcUsage;
        }
        parqit::ExprSchema sch;
        for (const auto &c : g_view_ref().cols()) sch.kinds[c.name] = c.kind;
        parqit::ExprResult tr = parqit::translate_filter(expr, sch, g_statamissing);
        if (!tr.ok) {
            cry("parqit count: " + tr.error);
            return kRcUsage;
        }
        std::string n;
        if (!s.query_scalar("SELECT count(*) FROM " + base + " WHERE " + tr.sql, &n,
                            &err)) {
            cry("parqit count: " + err);
            return kRcEngine;
        }
        save_local("_parqit_n", n);
    } else if (what == "codebook") {
        std::vector<std::pair<std::string, char>> targets;
        for (const auto &c : g_view_ref().cols()) {
            if (vars.empty()) targets.push_back({c.name, c.kind});
            else
                for (const auto &v : vars)
                    if (v == c.name) targets.push_back({c.name, c.kind});
        }
        if (targets.empty()) {
            cry("parqit codebook: no matching variables");
            return kRcVarNotFound;
        }
        /* PERF-CODEBOOK-KSCAN: one combined scan instead of one per variable
         * (mirrors summarize/distinct). count(*) appears once; each target
         * contributes 4 columns (miss, count(DISTINCT), min, max). */
        std::string sel = "count(*)";
        for (const auto &t : targets) {
            const std::string r = quote_ident(t.first);
            std::string miss = (t.second == 's')
                                   ? "count(*) FILTER (WHERE " + r +
                                         " IS NULL OR " + r + " = '')"
                                   : "count(*) - count(" + r + ")";
            sel += ", " + miss + ", count(DISTINCT " + r + "), CAST(min(" + r +
                   ") AS VARCHAR), CAST(max(" + r + ") AS VARCHAR)";
        }
        duckdb_result res;
        if (!s.query("SELECT " + sel + " FROM " + base, &res, &err)) {
            cry("parqit codebook: " + err);
            return kRcEngine;
        }
        auto cell = [&](idx_t c) -> std::string {
            if (duckdb_value_is_null(&res, c, 0)) return ".";
            char *v = duckdb_value_varchar(&res, c, 0);
            std::string out = v ? v : ".";
            if (v) duckdb_free(v);
            return out;
        };
        const std::string ntot = cell(0);
        for (size_t ti = 0; ti < targets.size(); ti++) {
            const idx_t off = 1 + static_cast<idx_t>(ti) * 4;
            std::string fmt, lab;
            for (const auto &c : g_view_ref().cols())
                if (c.name == targets[ti].first) {
                    fmt = c.fmt;
                    lab = c.varlab;
                }
            w.rec("cb",
                  {ntot, cell(off), cell(off + 1),
                   std::string(1, targets[ti].second)},
                  {targets[ti].first, cell(off + 2), cell(off + 3), lab});
        }
        duckdb_destroy_result(&res);
    } else if (what == "distinct") {
        std::vector<std::string> targets = vars;
        if (targets.empty())
            for (const auto &c : g_view_ref().cols()) targets.push_back(c.name);
        std::string sel = "count(*)";
        for (const auto &t : targets) {
            bool found = false;
            for (const auto &c : g_view_ref().cols()) found = found || c.name == t;
            if (!found) {
                cry("parqit distinct: variable " + t + " not found in the view");
                return kRcVarNotFound;
            }
            sel += ", count(DISTINCT " + quote_ident(t) + ")";
        }
        bool joint = req.value("joint", false) && targets.size() > 1;
        if (joint) {
            std::string tup;
            for (size_t i = 0; i < targets.size(); i++)
                tup += (i ? ", " : "") + quote_ident(targets[i]);
            sel += ", count(DISTINCT (" + tup + "))";
        }
        duckdb_result res;
        if (!s.query("SELECT " + sel + " FROM " + base, &res, &err)) {
            cry("parqit distinct: " + err);
            return kRcEngine;
        }
        long long total = duckdb_value_int64(&res, 0, 0);
        for (size_t t = 0; t < targets.size(); t++)
            w.rec("dst",
                  {std::to_string(duckdb_value_int64(&res, t + 1, 0)),
                   std::to_string(total)},
                  {targets[t]});
        if (joint)
            w.rec("dstj",
                  {std::to_string(duckdb_value_int64(&res, targets.size() + 1, 0)),
                   std::to_string(total)},
                  {});
        duckdb_destroy_result(&res);
        save_local("_parqit_n", std::to_string(total));
    } else if (what == "dupreport" || what == "duplist") {
        if (vars.empty()) {
            cry("parqit duplicates: a key varlist is required");
            return kRcUsage;
        }
        std::string keys;
        for (size_t i = 0; i < vars.size(); i++) {
            bool found = false;
            for (const auto &c : g_view_ref().cols()) found = found || c.name == vars[i];
            if (!found) {
                cry("parqit duplicates: variable " + vars[i] + " not found in the view");
                return kRcVarNotFound;
            }
            keys += (i ? ", " : "") + quote_ident(vars[i]);
        }
        if (what == "dupreport") {
            duckdb_result res;
            if (!s.query("SELECT c AS copies, count(*) AS groups FROM (SELECT "
                         "count(*) AS c FROM " + base + " GROUP BY " + keys +
                             ") GROUP BY c ORDER BY c",
                         &res, &err)) {
                cry("parqit duplicates report: " + err);
                return kRcEngine;
            }
            idx_t n = duckdb_row_count(&res);
            for (idx_t i = 0; i < n; i++)
                w.rec("dupr",
                      {std::to_string(duckdb_value_int64(&res, 0, i)),
                       std::to_string(duckdb_value_int64(&res, 1, i))},
                      {});
            duckdb_destroy_result(&res);
        } else {
            long long limit = req.value("limit", 20LL);
            std::string sel;
            const auto &cols = g_view_ref().cols();
            for (size_t i = 0; i < cols.size(); i++)
                sel += (i ? ", " : "") + std::string("CAST(") +
                       quote_ident(cols[i].name) + " AS VARCHAR)";
            duckdb_result res;
            if (!s.query("SELECT " + sel + " FROM " + base +
                             " QUALIFY count(*) OVER (PARTITION BY " + keys +
                             ") > 1 ORDER BY " + keys + " LIMIT " +
                             std::to_string(limit),
                         &res, &err)) {
                cry("parqit duplicates list: " + err);
                return kRcEngine;
            }
            std::string hdr;
            for (size_t i = 0; i < cols.size(); i++)
                hdr += (i ? "\t" : "") + cols[i].name;
            w.rec("duph", {}, {hdr});
            idx_t n = duckdb_row_count(&res);
            for (idx_t i = 0; i < n; i++) {
                std::string row;
                for (idx_t c = 0; c < duckdb_column_count(&res); c++) {
                    if (c) row += "\t";
                    if (duckdb_value_is_null(&res, c, i)) row += ".";
                    else {
                        char *v = duckdb_value_varchar(&res, c, i);
                        row += v ? v : ".";
                        if (v) duckdb_free(v);
                    }
                }
                w.rec("dupl", {}, {row});
            }
            duckdb_destroy_result(&res);
        }
    } else if (what == "misspatterns") {
        std::vector<std::pair<std::string, char>> targets;
        for (const auto &c : g_view_ref().cols()) {
            if (vars.empty()) targets.push_back({c.name, c.kind});
            else
                for (const auto &v : vars)
                    if (v == c.name) targets.push_back({c.name, c.kind});
        }
        if (targets.empty() || targets.size() > 14) {
            cry("parqit misstable patterns: between 1 and 14 variables (got " +
                std::to_string(targets.size()) + ")");
            return kRcUsage;
        }
        std::string inds, names;
        for (size_t i = 0; i < targets.size(); i++) {
            const std::string r = quote_ident(targets[i].first);
            std::string miss = (targets[i].second == 's')
                                   ? "(" + r + " IS NULL OR " + r + " = '')"
                                   : "(" + r + " IS NULL)";
            inds += (i ? " || " : "") +
                    ("(CASE WHEN " + miss + " THEN '.' ELSE '+' END)");
            names += (i ? " " : "") + targets[i].first;
        }
        duckdb_result res;
        if (!s.query("SELECT " + inds + " AS pat, count(*) FROM " + base +
                         " GROUP BY pat ORDER BY count(*) DESC, pat LIMIT 100",
                     &res, &err)) {
            cry("parqit misstable patterns: " + err);
            return kRcEngine;
        }
        w.rec("mph", {}, {names});
        idx_t n = duckdb_row_count(&res);
        for (idx_t i = 0; i < n; i++) {
            char *p = duckdb_value_varchar(&res, 0, i);
            w.rec("mpat", {std::to_string(duckdb_value_int64(&res, 1, i))},
                  {p ? p : ""});
            if (p) duckdb_free(p);
        }
        duckdb_destroy_result(&res);
    } else if (what == "tabstat") {
        std::vector<std::string> stats;
        if (!parqit::req_text_list(req, "stats", &stats, &err, false)) {
            cry(err);
            return kRcUsage;
        }
        if (stats.empty()) stats = {"mean"};
        std::string by;
        if (!parqit::req_text(req, "by", &by, &err, false)) {
            cry(err);
            return kRcUsage;
        }
        std::vector<std::string> targets;
        for (const auto &v : vars) {
            bool found = false, isnum = false;
            for (const auto &c : g_view_ref().cols())
                if (c.name == v) {
                    found = true;
                    isnum = (c.kind == 'n');
                }
            if (!found || !isnum) {
                cry("parqit tabstat: " + v + " is not a numeric view variable");
                return kRcUsage;
            }
            targets.push_back(v);
        }
        if (targets.empty()) {
            cry("parqit tabstat: a numeric varlist is required");
            return kRcUsage;
        }
        if (!by.empty()) {
            bool found = false;
            for (const auto &c : g_view_ref().cols()) found = found || c.name == by;
            if (!found) {
                cry("parqit tabstat: by() variable " + by + " not found");
                return kRcVarNotFound;
            }
            std::string ng;
            if (!s.query_scalar("SELECT count(DISTINCT " + quote_ident(by) +
                                    ") FROM " + base,
                                &ng, &err) ||
                std::strtoll(ng.c_str(), nullptr, 10) > 200) {
                cry("parqit tabstat: by() has too many groups (max 200); collapse instead");
                return kRcUsage;
            }
        }
        auto stat_sql = [&](const std::string &st,
                            const std::string &r) -> std::string {
            if (st == "n" || st == "count") return "count(" + r + ")";
            if (st == "mean") return "avg(" + r + ")";
            if (st == "sd") return "stddev_samp(" + r + ")";
            if (st == "var") return "var_samp(" + r + ")";
            if (st == "sum") return "coalesce(sum(" + r + "), 0)";
            if (st == "min") return "min(" + r + ")";
            if (st == "max") return "max(" + r + ")";
            if (st == "range") return "max(" + r + ") - min(" + r + ")";
            if (st == "median" || st == "p50") return parqit::stata_pctile_sql(r, 50);
            if (st.size() > 1 && st[0] == 'p') {
                double p;
                if (parqit::atod(st.substr(1), &p) && p > 0 && p < 100)
                    return parqit::stata_pctile_sql(r, p);
            }
            return "";
        };
        std::string sel;
        for (const auto &t : targets)
            for (const auto &st : stats) {
                std::string a = stat_sql(st, quote_ident(t));
                if (a.empty()) {
                    cry("parqit tabstat: unknown statistic " + st);
                    return kRcUsage;
                }
                sel += (sel.empty() ? "" : ", ") + a;
            }
        std::string gsel = by.empty() ? "" : "CAST(" + quote_ident(by) +
                                                 " AS VARCHAR) AS __g, ";
        std::string tail = by.empty() ? "" : " GROUP BY " + quote_ident(by) +
                                                 " ORDER BY " + quote_ident(by) +
                                                 " NULLS LAST";
        duckdb_result res;
        if (!s.query("SELECT " + gsel + sel + " FROM " + base + tail, &res, &err)) {
            cry("parqit tabstat: " + err);
            return kRcEngine;
        }
        idx_t nrows = duckdb_row_count(&res);
        idx_t off = by.empty() ? 0 : 1;
        for (idx_t g = 0; g < nrows; g++) {
            std::string gv = "";
            if (!by.empty()) {
                if (duckdb_value_is_null(&res, 0, g)) gv = ".";
                else {
                    char *v = duckdb_value_varchar(&res, 0, g);
                    gv = v ? v : ".";
                    if (v) duckdb_free(v);
                }
            }
            for (size_t t = 0; t < targets.size(); t++) {
                std::vector<std::string> plain;
                for (size_t st = 0; st < stats.size(); st++) {
                    idx_t c = off + t * stats.size() + st;
                    if (duckdb_value_is_null(&res, c, g)) plain.push_back(".");
                    else {
                        char *v = duckdb_value_varchar(&res, c, g);
                        plain.push_back(v ? v : ".");
                        if (v) duckdb_free(v);
                    }
                }
                w.rec("ts", plain, {targets[t], gv});
            }
        }
        duckdb_destroy_result(&res);
    } else if (what == "corr") {
        bool pairwise = req.value("pairwise", false);
        std::vector<std::string> targets;
        for (const auto &v : vars) {
            bool found = false, isnum = false;
            for (const auto &c : g_view_ref().cols())
                if (c.name == v) {
                    found = true;
                    isnum = (c.kind == 'n');
                }
            if (!found || !isnum) {
                cry("parqit correlate: " + v + " is not a numeric view variable");
                return kRcUsage;
            }
            targets.push_back(v);
        }
        if (targets.size() < 2) {
            cry("parqit correlate: at least two numeric variables");
            return kRcUsage;
        }
        std::string base3 = base;
        if (!pairwise) { /* Stata correlate: listwise (complete cases) */
            std::string wh;
            for (size_t i = 0; i < targets.size(); i++)
                wh += (i ? " AND " : "") + quote_ident(targets[i]) + " IS NOT NULL";
            base3 = "(SELECT * FROM " + base + " WHERE " + wh + ")";
        }
        std::string sel;
        for (size_t i = 0; i < targets.size(); i++)
            for (size_t j = 0; j <= i; j++) {
                const std::string a = quote_ident(targets[i]),
                                  b = quote_ident(targets[j]);
                sel += (sel.empty() ? "" : ", ") + std::string("corr(") + a + ", " +
                       b + ")";
                sel += ", count(*) FILTER (WHERE " + a + " IS NOT NULL AND " + b +
                       " IS NOT NULL)";
            }
        duckdb_result res;
        if (!s.query("SELECT " + sel + " FROM " + base3, &res, &err)) {
            cry("parqit correlate: " + err);
            return kRcEngine;
        }
        idx_t c = 0;
        for (size_t i = 0; i < targets.size(); i++)
            for (size_t j = 0; j <= i; j++) {
                std::string rv = ".";
                if (!duckdb_value_is_null(&res, c, 0)) {
                    char *v = duckdb_value_varchar(&res, c, 0);
                    rv = v ? v : ".";
                    if (v) duckdb_free(v);
                }
                long long nn = duckdb_value_int64(&res, c + 1, 0);
                w.rec("cor", {std::to_string(i + 1), std::to_string(j + 1), rv,
                              std::to_string(nn)},
                      {targets[i], targets[j]});
                c += 2;
            }
        duckdb_destroy_result(&res);
    } else if (what == "hist") {
        if (vars.size() != 1) {
            cry("parqit histogram: exactly one numeric variable");
            return kRcUsage;
        }
        char kind = 0;
        for (const auto &c : g_view_ref().cols())
            if (c.name == vars[0]) kind = c.kind;
        if (kind != 'n') {
            cry("parqit histogram: " + vars[0] + " is not a numeric view variable");
            return kRcUsage;
        }
        const std::string r = quote_ident(vars[0]);
        duckdb_result mres;
        if (!s.query("SELECT min(" + r + ")::DOUBLE, max(" + r +
                         ")::DOUBLE, count(" + r + ") FROM " + base,
                     &mres, &err)) {
            cry("parqit histogram: " + err);
            return kRcEngine;
        }
        if (duckdb_value_is_null(&mres, 0, 0)) {
            duckdb_destroy_result(&mres);
            cry("parqit histogram: no nonmissing values");
            return kRcUsage;
        }
        double lo = duckdb_value_double(&mres, 0, 0);
        double hi = duckdb_value_double(&mres, 1, 0);
        long long nn = duckdb_value_int64(&mres, 2, 0);
        duckdb_destroy_result(&mres);
        long long bins = req.value("bins", 0LL);
        if (bins <= 0) {
            bins = static_cast<long long>(std::ceil(std::sqrt(
                static_cast<double>(nn))));
            if (bins > 50) bins = 50;
            if (bins < 1) bins = 1;
        }
        if (bins > 1000) bins = 1000;
        if (hi <= lo) bins = 1;
        double width = (hi - lo) / static_cast<double>(bins);
        /* full-precision literals: std::to_string is %.6f and can round lo
         * past the true minimum, producing bin -1 */
        /* full-precision, locale-independent literals (dtoa = shortest exact
         * round-trip; printf/%g would round and honour LC_NUMERIC) */
        std::string lobuf = parqit::dtoa(lo), wbuf = parqit::dtoa(width);
        std::string bexpr =
            (bins == 1)
                ? "0"
                : "greatest(least(CAST(floor((" + r + " - (" + lobuf + ")) / (" +
                      wbuf + ")) AS BIGINT), " + std::to_string(bins - 1) +
                      "), 0)";
        duckdb_result res;
        if (!s.query("SELECT " + bexpr + " AS b, count(*) FROM " + base + " WHERE " +
                         r + " IS NOT NULL GROUP BY b ORDER BY b",
                     &res, &err)) {
            cry("parqit histogram: " + err);
            return kRcEngine;
        }
        idx_t n = duckdb_row_count(&res);
        for (idx_t i = 0; i < n; i++)
            w.rec("hb",
                  {std::to_string(duckdb_value_int64(&res, 0, i)),
                   std::to_string(duckdb_value_int64(&res, 1, i))},
                  {});
        duckdb_destroy_result(&res);
        save_local("_parqit_hist_lo", lobuf);
        save_local("_parqit_hist_width", wbuf);
        save_local("_parqit_hist_bins", std::to_string(bins));
        save_local("_parqit_n", std::to_string(nn));
    } else {
        cry("parqit: unknown stats request");
        return kRcUsage;
    }
    if (!w.close(&err)) {
        cry(err);
        return kRcEngine;
    }
    return 0;
}

ST_retcode cmd_path(const std::vector<std::string> &args) {
    std::string p;
    if (args.size() < 2 || !parqit::hex_decode(args[1], p)) {
        cry("parqit path: malformed argument");
        return kRcUsage;
    }
    std::error_code ec;
    std::string abs = std::filesystem::absolute(p, ec).string();
    if (ec) abs = p;
    bool exists = std::filesystem::exists(abs, ec);
    save_local("_parqit_path", parqit::hex_encode(abs));
    save_local("_parqit_path_exists", exists ? "1" : "0");
    return 0;
}

} // namespace parqit_plugin

namespace parqit_plugin {
std::string view_current_name() {
    auto it = g_views.find(g_current);
    return (it != g_views.end() && it->second.live()) ? g_current : "";
}
} // namespace parqit_plugin
