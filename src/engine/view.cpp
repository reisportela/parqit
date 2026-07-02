#include "engine/view.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <set>

#include "engine/exprtrans.hpp"
#include "engine/session.hpp"
#include "engine/typemap.hpp"

namespace parqit {

using json = nlohmann::json;

static void denormalize_strings(std::vector<ViewCol> *cols); /* defined below */

/* Stata's gen/egen with an explicit narrow storage type coerces the value the
 * way native Stata does at assignment time: integers TRUNCATE toward zero and
 * an out-of-range value becomes system missing (gen byte x=3.9 -> 3,
 * gen byte x=200 -> ., gen byte x=-2.5 -> -2 — verified against Stata). float
 * targets round to float32 precision. The CAST to the matching DuckDB integer
 * type also makes the collected column size to the requested storage type
 * (byte/int/long), not silently widen to double (EXPR-1). A str# string target
 * truncates the value to the declared byte width (gen str3 x="hello" -> "hel"),
 * matching Stata (STR-GENWIDTH-1); a codepoint split exactly at the boundary
 * yields U+FFFD like parqit's substr(), since the engine keeps valid UTF-8.
 * double/strL/mismatched: no-op. `kind` is the value kind ('n' or 's'). */
static std::string coerce_storage(const std::string &v,
                                  const std::string &type_req, char kind) {
    if (type_req.empty()) return v;
    StType mt;
    int mb = 0;
    if (!sttype_parse(type_req, &mt, &mb)) return v;
    if (kind == 's') {
        if (mt == StType::Str && mb > 0)
            return "parqit_substr_bytes(" + v + ", 1, " + std::to_string(mb) + ")";
        return v; /* strL / numeric-typed target on a string: no width clamp */
    }
    double lo, hi;
    const char *duckint;
    switch (mt) {
    case StType::Byte: lo = kStataByteMin; hi = kStataByteMax; duckint = "TINYINT"; break;
    case StType::Int:  lo = kStataIntMin;  hi = kStataIntMax;  duckint = "SMALLINT"; break;
    case StType::Long: lo = kStataLongMin; hi = kStataLongMax; duckint = "INTEGER"; break;
    case StType::Float: return "CAST(" + v + " AS FLOAT)";
    default: return v; /* double / str# on a numeric value: keep full precision */
    }
    return "(CASE WHEN (" + v + ") IS NULL THEN NULL WHEN trunc(" + v + ") < " +
           dtoa(lo) + " OR trunc(" + v + ") > " + dtoa(hi) +
           " THEN NULL ELSE CAST(trunc(" + v + ") AS " + duckint + ") END)";
}

/* GROUPKEY-1: fold a grouping key's missing encodings together the way the
 * merge/joinby join key_norm and native Stata already do — an empty string and
 * NULL are both the missing string "", a NaN and NULL are both numeric missing.
 * A GROUP BY / PARTITION BY on a bare user key would otherwise split '' from
 * NULL (and NaN from NULL) into separate groups, diverging from Stata and from
 * parqit's own merge. `kind` is the key column's kind ('n' or 's'). */
static std::string norm_group_key(const std::string &ref, char kind) {
    if (kind == 's') return "nullif(" + ref + ", '')";
    return "(CASE WHEN isnan(CAST(" + ref + " AS DOUBLE)) THEN NULL ELSE " + ref +
           " END)";
}

void View::close() { *this = View(); }

void View::open(const std::string &scan_select, std::vector<ViewCol> cols,
                json vallabs, json chars, std::string dtalabel,
                const std::string &source_desc) {
    close();
    live_ = true;
    scan_ = scan_select;
    cols_ = std::move(cols);
    vallabs_ = std::move(vallabs);
    chars_ = std::move(chars);
    dtalabel_ = std::move(dtalabel);
    source_desc_ = source_desc;
}

int View::col_index(const std::string &name) const {
    for (size_t i = 0; i < cols_.size(); i++)
        if (cols_[i].name == name) return static_cast<int>(i);
    return -1;
}

std::string View::fresh_helper(const std::string &hint,
                               const std::set<std::string> &taken) {
    for (;;) {
        std::string cand = "__parqit_" + hint + "_" + std::to_string(++helper_counter_);
        /* charter §6.12: never collide — with the live manifest, nor with any
         * extra names the caller must dodge (a two-table verb's using side) */
        if (col_index(cand) < 0 && !taken.count(cand)) return cand;
    }
}

std::string View::select_list() const {
    std::string out;
    for (size_t i = 0; i < cols_.size(); i++) {
        if (i) out += ", ";
        out += quote_ident(cols_[i].name);
    }
    return out;
}

std::string View::order_by_sql() const {
    if (sort_.empty()) return "";
    std::string o = " ORDER BY ";
    for (size_t i = 0; i < sort_.size(); i++) {
        if (i) o += ", ";
        /* explicit NULLS LAST so ordering (and every _n window, keep-in slice
         * and merge spine built on it) matches Stata's "missing sorts last in
         * every direction" regardless of DuckDB's default null order */
        o += sort_[i] + " NULLS LAST";
    }
    return o;
}

void View::push_stage(const std::string &select_body, const std::string &desc) {
    stages_.push_back(select_body);
    descs_.push_back(desc);
}

/* refer to the previous pipeline step */
static std::string prev_name(size_t k) {
    return k == 0 ? "__parqit_s0" : ("__parqit_s" + std::to_string(k));
}

std::string View::compile_prefix(size_t nstages) const {
    std::string sql = "WITH __parqit_s0 AS (" + scan_ + ")";
    for (size_t k = 0; k < nstages; k++)
        sql += ", __parqit_s" + std::to_string(k + 1) + " AS (" + stages_[k] + ")";
    sql += " SELECT * FROM " + prev_name(nstages);
    return sql;
}

std::string View::compile(bool with_order) const {
    std::string sql = compile_prefix(stages_.size());
    if (with_order) sql += order_by_sql();
    return sql;
}

std::string View::show() const {
    std::string out = "-- source: " + source_desc_ + "\n";
    out += "WITH __parqit_s0 AS (\n  " + scan_ + "\n)";
    for (size_t k = 0; k < stages_.size(); k++) {
        out += ",\n-- " + descs_[k] + "\n__parqit_s" + std::to_string(k + 1) +
               " AS (\n  " + stages_[k] + "\n)";
    }
    out += "\nSELECT * FROM " + prev_name(stages_.size());
    out += order_by_sql();
    return out;
}

/* ------------------------------------------------------------- helpers --- */

static bool glob_match(const std::string &pat, const std::string &name) {
    /* Stata varlist wildcards: * (any run) and ? (one char) */
    size_t p = 0, n = 0, star = std::string::npos, mark = 0;
    while (n < name.size()) {
        if (p < pat.size() && (pat[p] == '?' || pat[p] == name[n])) {
            p++;
            n++;
        } else if (p < pat.size() && pat[p] == '*') {
            star = p++;
            mark = n;
        } else if (star != std::string::npos) {
            p = star + 1;
            n = ++mark;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*') p++;
    return p == pat.size();
}

std::string View::expand_patterns(const std::vector<std::string> &patterns,
                                  std::vector<std::string> *out) const {
    std::set<std::string> seen;
    for (const auto &pat : patterns) {
        bool has_wild = pat.find('*') != std::string::npos ||
                        pat.find('?') != std::string::npos;
        bool hit = false;
        for (const auto &c : cols_) {
            if (has_wild ? glob_match(pat, c.name) : (pat == c.name)) {
                hit = true;
                if (seen.insert(c.name).second) out->push_back(c.name);
            }
        }
        if (!hit) return "variable " + pat + " not found in the view";
    }
    return "";
}

static ExprSchema schema_of(const std::vector<ViewCol> &cols) {
    ExprSchema s;
    for (const auto &c : cols) {
        s.kinds[c.name] = c.kind;
        if (c.normalized) s.normalized.insert(c.name); /* MISS-1 */
    }
    return s;
}

/* replace the _n/_N placeholders; returns true when present */
static bool uses_rowctx(const std::string &sql) {
    return sql.find("__PARQIT_ROW__") != std::string::npos ||
           sql.find("__PARQIT_NROWS__") != std::string::npos;
}
static void substitute(std::string *sql, const std::string &from,
                       const std::string &to) {
    size_t p = 0;
    while ((p = sql->find(from, p)) != std::string::npos) {
        sql->replace(p, from.size(), to);
        p += to.size();
    }
}

std::string View::rowctx_wrap(std::string *sql, const std::string &prev) {
    bool needs_row = sql->find("__PARQIT_ROW__") != std::string::npos;
    bool needs_n = sql->find("__PARQIT_NROWS__") != std::string::npos;
    std::string wins;
    if (needs_row) {
        std::string rn = fresh_helper("rn");
        substitute(sql, "__PARQIT_ROW__", quote_ident(rn));
        std::string ord = order_by_sql();
        std::string over = ord.empty() ? "OVER ()" : "OVER (" + ord.substr(1) + ")";
        wins = "row_number() " + over + " AS " + quote_ident(rn);
    }
    if (needs_n) {
        std::string nn = fresh_helper("nn");
        substitute(sql, "__PARQIT_NROWS__", quote_ident(nn));
        if (!wins.empty()) wins += ", ";
        wins += "count(*) OVER () AS " + quote_ident(nn);
    }
    return "(SELECT *, " + wins + " FROM " + prev + ")";
}

/* ---------------------------------------------------------------- verbs --- */

std::string View::projection_source(const std::vector<ViewCol> &survivors) {
    const std::string prev = prev_name(stages_.size());
    /* A keep/drop that removes a sort-key column must not leave sort_
     * referencing it: every later materialisation would die with a raw DuckDB
     * Binder Error where native Stata succeeds. Native semantics (`sort y`
     * then `drop y`, verified): the rows keep their y-sorted PHYSICAL order
     * and sortedby is truncated. Reproduce that by baking the full current
     * ORDER BY into this stage's source — DuckDB's default
     * preserve_insertion_order carries a subquery's order through subsequent
     * streaming stages — then truncating sort_ to the longest prefix of keys
     * whose columns all survive (dropping the first key clears it entirely,
     * mirroring Stata's sortedby truncation). */
    if (sort_.empty()) return prev;
    std::set<std::string> alive;
    for (const auto &c : survivors) alive.insert(quote_ident(c.name));
    size_t keep = sort_.size();
    for (size_t i = 0; i < sort_.size(); i++) {
        std::string base = sort_[i];
        const std::string desc = " DESC";
        if (base.size() > desc.size() &&
            base.compare(base.size() - desc.size(), desc.size(), desc) == 0)
            base.resize(base.size() - desc.size());
        if (!alive.count(base)) {
            keep = i;
            break;
        }
    }
    if (keep == sort_.size()) return prev; /* every key survives: nothing to do */
    std::string src = "(SELECT * FROM " + prev + order_by_sql() + ")";
    sort_.resize(keep);
    return src;
}

std::string View::keep_vars(const std::vector<std::string> &patterns) {
    std::vector<std::string> keep;
    std::string err = expand_patterns(patterns, &keep);
    if (!err.empty()) return err;
    std::vector<ViewCol> ncols;
    for (const auto &want : keep) ncols.push_back(cols_[col_index(want)]);
    const std::string src = projection_source(ncols); /* may bake a dropped sort */
    cols_ = std::move(ncols);
    push_stage("SELECT " + select_list() + " FROM " + src,
               "keep " + std::to_string(cols_.size()) + " variables");
    return "";
}

std::string View::drop_vars(const std::vector<std::string> &patterns) {
    std::vector<std::string> drop;
    std::string err = expand_patterns(patterns, &drop);
    if (!err.empty()) return err;
    std::set<std::string> dropset(drop.begin(), drop.end());
    if (dropset.size() >= cols_.size())
        return "cannot drop every variable in the view";
    std::vector<ViewCol> ncols;
    for (const auto &c : cols_)
        if (!dropset.count(c.name)) ncols.push_back(c);
    const std::string src = projection_source(ncols); /* may bake a dropped sort */
    cols_ = std::move(ncols);
    push_stage("SELECT " + select_list() + " FROM " + src,
               "drop " + std::to_string(dropset.size()) + " variables");
    return "";
}

std::string View::filter(const std::string &stata_expr, bool drop,
                         bool statamissing) {
    ExprResult r = translate_filter(stata_expr, schema_of(cols_), statamissing);
    if (!r.ok) return r.error;
    /* keep if: WHERE expr (missing ⇒ dropped, exactly Stata's outcome);
     * drop if: keep rows where the condition is NOT TRUE — missing kept */
    std::string cond =
        drop ? "((" + r.sql + ") IS DISTINCT FROM TRUE)" : "(" + r.sql + ")";
    const std::string prev = prev_name(stages_.size());
    if (uses_rowctx(cond)) {
        std::string src = rowctx_wrap(&cond, prev);
        push_stage("SELECT " + select_list() + " FROM " + src + " WHERE " + cond,
                   (drop ? "drop if " : "keep if ") + stata_expr);
    } else {
        push_stage("SELECT " + select_list() + " FROM " + prev + " WHERE " + cond,
                   (drop ? "drop if " : "keep if ") + stata_expr);
    }
    return "";
}

std::string View::gen(const std::string &name, const std::string &type_req,
                      const std::string &expr, const std::string &if_expr,
                      bool statamissing) {
    if (col_index(name) >= 0) return "variable " + name + " already defined";
    ExprResult r = translate_expression(expr, schema_of(cols_), statamissing);
    if (!r.ok) return r.error;
    /* GEN-TYPEFAMILY-1: native Stata rejects an explicit storage type whose
     * family disagrees with the expression (gen str# x = <numeric> and
     * gen <numeric> x = "<string>" both stop with r(109)); coerce_storage()
     * cannot bridge families, so it would silently no-op and leave a column
     * whose metadata contradicts its value kind. Reject it loudly instead. */
    if (!type_req.empty()) {
        StType mt;
        int mb = 0;
        if (sttype_parse(type_req, &mt, &mb)) {
            bool target_string = (mt == StType::Str || mt == StType::StrL);
            bool expr_string = (r.kind == 's');
            if (target_string != expr_string)
                return "type mismatch: cannot generate " + type_req + " " + name +
                       " from a " +
                       std::string(expr_string ? "string" : "numeric") +
                       " expression";
        }
    }
    /* honour an explicit narrow storage type the way native Stata gen does
     * (numeric: truncate toward zero / out-of-range -> missing / float32 rounding,
     * EXPR-1; string: truncate to the declared str# byte width, STR-GENWIDTH-1) */
    std::string vexpr = coerce_storage(r.sql, type_req, r.kind);
    std::string value = vexpr;
    if (!if_expr.empty()) {
        ExprResult c = translate_filter(if_expr, schema_of(cols_), statamissing);
        if (!c.ok) return c.error;
        value = "(CASE WHEN coalesce(" + c.sql + ", FALSE) THEN " + vexpr +
                " ELSE NULL END)";
        if (uses_rowctx(c.sql)) return "_n/_N are not supported in the if "
                                       "qualifier of gen (yet)";
    }
    const std::string prev = prev_name(stages_.size());
    std::string body;
    if (uses_rowctx(value)) {
        std::string src = rowctx_wrap(&value, prev);
        body = "SELECT " + select_list() + ", " + value + " AS " + quote_ident(name) +
               " FROM " + src;
    } else {
        body = "SELECT " + select_list() + ", " + value + " AS " + quote_ident(name) +
               " FROM " + prev;
    }
    ViewCol nc;
    nc.name = name;
    nc.kind = (r.kind == 's' ? 's' : 'n');
    nc.meta_type = type_req; /* requested storage type wins at collect */
    cols_.push_back(nc);
    push_stage(body, "gen " + name + " = " + expr +
                         (if_expr.empty() ? "" : " if " + if_expr));
    return "";
}

std::string View::replace(const std::string &name, const std::string &expr,
                          const std::string &if_expr, bool statamissing) {
    int idx = col_index(name);
    if (idx < 0) return "variable " + name + " not found in the view";
    ExprResult r = translate_expression(expr, schema_of(cols_), statamissing);
    if (!r.ok) return r.error;
    char newkind = (r.kind == 's' ? 's' : 'n');
    if (newkind != cols_[idx].kind)
        return "type mismatch: cannot replace " +
               std::string(cols_[idx].kind == 's' ? "string" : "numeric") + " " +
               name + " with a " + (newkind == 's' ? "string" : "numeric") +
               " expression";
    std::string value = r.sql;
    if (!if_expr.empty()) {
        ExprResult c = translate_filter(if_expr, schema_of(cols_), statamissing);
        if (!c.ok) return c.error;
        if (uses_rowctx(c.sql) || uses_rowctx(r.sql))
            return "_n/_N are not supported in replace (yet)";
        value = "(CASE WHEN coalesce(" + c.sql + ", FALSE) THEN " + r.sql +
                " ELSE " + quote_ident(name) + " END)";
    } else if (uses_rowctx(value)) {
        return "_n/_N are not supported in replace (yet)";
    }
    std::string sel;
    for (size_t i = 0; i < cols_.size(); i++) {
        if (i) sel += ", ";
        if (static_cast<int>(i) == idx)
            sel += value + " AS " + quote_ident(name);
        else
            sel += quote_ident(cols_[i].name);
    }
    /* replacing changes content, not storage intent — but a wider value may
     * no longer fit a saved narrow type; collect re-sizes anyway. MISS-1: the
     * new expression may introduce an IEEE special (e.g. replace f = f*f), so
     * the column is no longer guaranteed clean — drop its normalized flag. */
    cols_[idx].normalized = false;
    push_stage("SELECT " + sel + " FROM " + prev_name(stages_.size()),
               "replace " + name + " = " + expr +
                   (if_expr.empty() ? "" : " if " + if_expr));
    return "";
}

std::string View::rename(const std::string &oldn, const std::string &newn) {
    int idx = col_index(oldn);
    if (idx < 0) return "variable " + oldn + " not found in the view";
    if (col_index(newn) >= 0) return "variable " + newn + " already defined";
    std::string sel;
    for (size_t i = 0; i < cols_.size(); i++) {
        if (i) sel += ", ";
        if (static_cast<int>(i) == idx)
            sel += quote_ident(oldn) + " AS " + quote_ident(newn);
        else
            sel += quote_ident(cols_[i].name);
    }
    cols_[idx].name = newn;
    /* characteristics (and notes, which are characteristics) are keyed by the
     * variable name; follow the rename so they are not dropped on a later
     * save/collect, which serialises only chars whose key is a live column
     * (META-2). Value labels ride on ViewCol.vallab and need no remap. */
    if (chars_.is_object() && chars_.contains(oldn)) {
        chars_[newn] = chars_[oldn];
        chars_.erase(oldn);
    }
    /* sort keys referencing the old name follow the rename */
    for (auto &k : sort_) {
        if (k == quote_ident(oldn)) k = quote_ident(newn);
        if (k == quote_ident(oldn) + " DESC") k = quote_ident(newn) + " DESC";
    }
    push_stage("SELECT " + sel + " FROM " + prev_name(stages_.size()),
               "rename " + oldn + " " + newn);
    return "";
}

std::string View::reorder(const std::vector<std::string> &front) {
    std::vector<std::string> want;
    std::string err = expand_patterns(front, &want);
    if (!err.empty()) return err;
    std::set<std::string> moved(want.begin(), want.end());
    std::vector<ViewCol> ncols;
    for (const auto &w : want) ncols.push_back(cols_[col_index(w)]);
    for (const auto &c : cols_)
        if (!moved.count(c.name)) ncols.push_back(c);
    cols_ = std::move(ncols);
    push_stage("SELECT " + select_list() + " FROM " + prev_name(stages_.size()),
               "order (column reorder)");
    return "";
}

std::string View::sort(const std::vector<std::string> &keys,
                       const std::vector<bool> &desc) {
    std::vector<std::string> names;
    std::string err = expand_patterns(keys, &names);
    if (!err.empty()) return err;
    if (names.size() != keys.size())
        return "wildcards are not allowed in parqit sort";
    sort_.clear();
    for (size_t i = 0; i < names.size(); i++)
        sort_.push_back(quote_ident(names[i]) +
                        (i < desc.size() && desc[i] ? " DESC" : ""));
    /* order is plan state, not a stage — applied at materialisation and to
     * every _n window from here on */
    return "";
}

/* Stata percentile rule (summarize/_pctile): np = n*p/100 over the sorted
 * nonmissing values; integer np → mean of x[np], x[np+1]; else x[ceil(np)]. */
std::string stata_pctile_sql(const std::string &ref, double p) {
    std::string arr =
        "list_sort(list(" + ref + ") FILTER (WHERE " + ref + " IS NOT NULL))";
    std::string n = "len(" + arr + ")";
    std::string np = "(" + n + " * " + dtoa(p) + " / 100.0)";
    return "(CASE WHEN " + n + " = 0 THEN NULL "
           "WHEN " + np + " = floor(" + np + ") THEN (list_extract(" + arr +
           ", CAST(" + np + " AS BIGINT)) + list_extract(" + arr +
           ", least(CAST(" + np + " AS BIGINT) + 1, " + n + "))) / 2.0 "
           "ELSE list_extract(" + arr + ", CAST(ceil(" + np + ") AS BIGINT)) END)";
}

std::string View::collapse(const std::vector<CollapseSpec> &specs,
                           const std::vector<std::string> &by) {
    if (specs.empty()) return "collapse: nothing to compute";
    std::vector<std::string> byn;
    if (!by.empty()) {
        std::string err = expand_patterns(by, &byn);
        if (!err.empty()) return err;
    }
    /* deterministic first/last need a row index over the current order */
    bool needs_rn = false;
    for (const auto &sp : specs)
        needs_rn |= (sp.stat == "first" || sp.stat == "last" ||
                     sp.stat == "firstnm" || sp.stat == "lastnm");
    std::string rn = fresh_helper("rn");
    const std::string prev = prev_name(stages_.size());
    std::string src = prev;
    std::string pre;
    if (needs_rn) {
        std::string ord = order_by_sql();
        if (ord.empty()) {
            /* COLLAPSE-3: (first)/(last) need a defined order. With no pending
             * sort, row_number() OVER () is engine-defined and varies with
             * parallelism / row-group layout. Fall back to a reproducible total
             * order over all columns (NULLS LAST) so the result is at least
             * deterministic; a user who needs Stata's exact "first in current
             * order" should parqit sort first (documented). */
            for (size_t i = 0; i < cols_.size(); i++)
                ord += (i ? ", " : " ORDER BY ") + quote_ident(cols_[i].name) +
                       " NULLS LAST";
        }
        std::string over = "OVER (" + ord.substr(1) + ")";
        src = "(SELECT *, row_number() " + over + " AS " + quote_ident(rn) +
              " FROM " + prev + ")";
    }

    std::vector<ViewCol> ncols;
    std::string sel;
    for (const auto &b : byn) {
        if (!sel.empty()) sel += ", ";
        /* GROUPKEY-1: project the normalized key so the output group value and
         * the GROUP BY below agree (the missing group's key renders as Stata
         * missing, not as a distinct '' / NaN). */
        sel += norm_group_key(quote_ident(b), cols_[col_index(b)].kind) +
               " AS " + quote_ident(b);
        ncols.push_back(cols_[col_index(b)]);
    }
    std::set<std::string> outnames(byn.begin(), byn.end());
    for (const auto &sp : specs) {
        int sidx = col_index(sp.source);
        if (sidx < 0) return "variable " + sp.source + " not found in the view";
        const ViewCol &scol = cols_[sidx];
        std::string tgt = sp.target.empty() ? sp.source : sp.target;
        if (!outnames.insert(tgt).second)
            return "collapse: duplicate output variable " + tgt;
        const std::string ref = quote_ident(sp.source);
        std::string agg;
        char kind = 'n';
        if (sp.stat == "mean") agg = "avg(" + ref + ")";
        else if (sp.stat == "sum") agg = "coalesce(sum(" + ref + "), 0)";
        else if (sp.stat == "sd") agg = "stddev_samp(" + ref + ")";
        else if (sp.stat == "min") agg = "min(" + ref + ")";
        else if (sp.stat == "max") agg = "max(" + ref + ")";
        else if (sp.stat == "count")
            /* Stata counts NONMISSING values; for a string variable a missing
             * value is "" (parqit's NULL≡'' contract), so empty strings must NOT
             * be counted as present. (Stata itself refuses count on strings;
             * parqit extends it to the nonmissing count — documented.) */
            agg = scol.kind == 's'
                      ? "count(*) FILTER (WHERE coalesce(" + ref + ", '') <> '')"
                      : "count(" + ref + ")";
        else if (sp.stat == "median") agg = stata_pctile_sql(ref, 50);
        else if (sp.stat.size() >= 2 && sp.stat[0] == 'p') {
            double p;
            if (!atod(sp.stat.substr(1), &p) || p <= 0 || p >= 100)
                return "collapse: unknown statistic (" + sp.stat + ")";
            agg = stata_pctile_sql(ref, p);
        } else if (sp.stat == "first" || sp.stat == "last") {
            /* include-missing first/last via a struct payload */
            const char *fn = (sp.stat == "first") ? "arg_min" : "arg_max";
            agg = std::string(fn) + "({'v': " + ref + "}, " + quote_ident(rn) +
                  ")['v']";
            kind = scol.kind;
        } else if (sp.stat == "firstnm" || sp.stat == "lastnm") {
            const char *fn = (sp.stat == "firstnm") ? "arg_min" : "arg_max";
            /* first/last NONMISSING value. arg_min/arg_max already skip a NULL
             * payload (numeric missing); for strings, "" is also missing, so
             * filter it out too. */
            agg = std::string(fn) + "(" + ref + ", " + quote_ident(rn) + ")";
            if (scol.kind == 's')
                agg += " FILTER (WHERE coalesce(" + ref + ", '') <> '')";
            kind = scol.kind;
        } else {
            return "collapse: unknown statistic (" + sp.stat + ")";
        }
        if (kind == 'n' && scol.kind == 's' &&
            !(sp.stat == "first" || sp.stat == "last" || sp.stat == "firstnm" ||
              sp.stat == "lastnm" || sp.stat == "count"))
            return "collapse: " + sp.stat + " needs a numeric variable (" +
                   sp.source + " is string)";
        if (!sel.empty()) sel += ", ";
        sel += agg + " AS " + quote_ident(tgt);
        ViewCol nc;
        nc.name = tgt;
        nc.kind = (sp.stat == "count") ? 'n' : kind;
        if (sp.stat == "first" || sp.stat == "last" || sp.stat == "firstnm" ||
            sp.stat == "lastnm") {
            nc.fmt = scol.fmt;
            nc.vallab = scol.vallab;
            nc.meta_type = scol.meta_type;
        }
        ncols.push_back(nc);
    }
    std::string body = "SELECT " + sel + " FROM " + src;
    std::string desc = "collapse";
    if (!byn.empty()) {
        body += " GROUP BY ";
        for (size_t i = 0; i < byn.size(); i++) {
            if (i) body += ", ";
            body += norm_group_key(quote_ident(byn[i]),
                                    cols_[col_index(byn[i])].kind);
        }
        desc += " by(";
        for (size_t i = 0; i < byn.size(); i++)
            desc += (i ? " " : "") + byn[i];
        desc += ")";
    } else {
        /* COLLAPSE-EMPTY-1: a no-by aggregate over zero input rows would emit one
         * fabricated row (mean ., sum 0, count 0). Stata stops with r(2000) "no
         * observations"; emit zero rows instead so the fabricated row never
         * reaches the data, consistent with the by() case (which already yields
         * zero groups on empty input). */
        body += " HAVING count(*) > 0";
    }
    /* MISS-1 sibling: a string by-key groups through nullif(k,''), so an
     * all-missing key group emits SQL NULL; string first/last aggregates can
     * be NULL too — clear the flag so save's coalesce guard applies */
    denormalize_strings(&ncols);
    cols_ = std::move(ncols);
    push_stage(body, desc);
    /* Stata collapse leaves the data sorted by the by-vars; earlier keep-in
     * ranges stay on the validation list (their prefixes are unchanged) */
    sort_.clear();
    for (const auto &b : byn) sort_.push_back(quote_ident(b));
    return "";
}

std::string View::contract(const std::vector<std::string> &by,
                           const std::string &freq) {
    std::vector<std::string> byn;
    std::string err = expand_patterns(by, &byn);
    if (!err.empty()) return err;
    if (byn.empty()) return "contract needs a varlist";
    std::string fname = freq.empty() ? "_freq" : freq;
    for (const auto &b : byn)
        if (b == fname) return "freq() name collides with a contracted variable";
    std::string sel;
    std::vector<ViewCol> ncols;
    for (const auto &b : byn) {
        if (!sel.empty()) sel += ", ";
        /* GROUPKEY-1: normalize the grouped key (''/NaN -> missing), see collapse */
        sel += norm_group_key(quote_ident(b), cols_[col_index(b)].kind) +
               " AS " + quote_ident(b);
        ncols.push_back(cols_[col_index(b)]);
    }
    sel += ", count(*) AS " + quote_ident(fname);
    ViewCol fc;
    fc.name = fname;
    fc.kind = 'n';
    fc.varlab = "Frequency";
    ncols.push_back(fc);
    std::string body = "SELECT " + sel + " FROM " + prev_name(stages_.size()) +
                       " GROUP BY ";
    for (size_t i = 0; i < byn.size(); i++) {
        if (i) body += ", ";
        body += norm_group_key(quote_ident(byn[i]), cols_[col_index(byn[i])].kind);
    }
    /* MISS-1 sibling: string by-keys group through nullif(k,'') — see collapse */
    denormalize_strings(&ncols);
    cols_ = std::move(ncols);
    push_stage(body, "contract");
    sort_.clear();
    for (const auto &b : byn) sort_.push_back(quote_ident(b));
    return "";
}

std::string View::duplicates_drop(const std::vector<std::string> &by, bool force) {
    const std::string prev = prev_name(stages_.size());
    if (by.empty()) {
        /* GROUPKEY-1 for the no-varlist form too: a raw SELECT DISTINCT would
         * keep a ('', NULL) string pair — or a NaN beside a NULL numeric, both
         * reachable mid-pipeline after merge/append — as two rows, while native
         * Stata (and the varlist branch below) sees one duplicate. Dedupe on
         * the normalized keys over ALL columns and emit one representative row.
         * A hash GROUP BY + any_value, not a row_number() window: rows within
         * a group are identical up to their missing encoding, so ANY
         * representative is correct, and the parallel hash aggregate measures
         * ~40% faster than the window on a 10M-row dedupe (PERF-DUP-1). */
        std::string sel, grp;
        for (size_t i = 0; i < cols_.size(); i++) {
            const std::string r = quote_ident(cols_[i].name);
            if (i) { sel += ", "; grp += ", "; }
            sel += "any_value(" + r + ") AS " + r;
            grp += norm_group_key(r, cols_[i].kind);
        }
        push_stage("SELECT " + sel + " FROM " + prev + " GROUP BY " + grp,
                   "duplicates drop");
        return "";
    }
    if (!force) return "duplicates drop with a varlist requires the force option";
    if (sort_.empty())
        return "duplicates drop <varlist> needs a defined order to be "
               "deterministic: run parqit sort first";
    std::vector<std::string> byn;
    std::string err = expand_patterns(by, &byn);
    if (!err.empty()) return err;
    std::string part;
    for (size_t i = 0; i < byn.size(); i++) {
        if (i) part += ", ";
        /* GROUPKEY-1: dedup by the normalized key so a ''-vs-NULL (or NaN-vs-NULL)
         * pair on the by-vars is recognised as a duplicate, like native Stata. */
        part += norm_group_key(quote_ident(byn[i]), cols_[col_index(byn[i])].kind);
    }
    std::string rn = fresh_helper("rn");
    push_stage("SELECT " + select_list() + " FROM (SELECT *, row_number() OVER "
               "(PARTITION BY " + part + order_by_sql() + ") AS " + quote_ident(rn) +
               " FROM " + prev + ") WHERE " + quote_ident(rn) + " = 1",
               "duplicates drop (by varlist, keeping the first in sort order)");
    return "";
}

std::string View::keep_in(long long f, long long l) {
    /* charter §6.13: validate the form loudly; range-vs-N checked at
     * materialisation against the real count */
    if (f < 1 || l < f)
        return "invalid in range: need 1 <= f <= l (negative and inverted "
               "ranges are not supported on a lazy view)";
    const std::string prev = prev_name(stages_.size());
    std::string inner = "SELECT * FROM " + prev + order_by_sql();
    push_stage("SELECT " + select_list() + " FROM (" + inner + " LIMIT " +
                   std::to_string(l - f + 1) + " OFFSET " + std::to_string(f - 1) +
                   ")",
               "keep in " + std::to_string(f) + "/" + std::to_string(l));
    PendingRange pr;
    pr.stage = stages_.size() - 1; /* validate against the count BEFORE this */
    pr.f = f;
    pr.l = l;
    ranges_.push_back(pr);
    return "";
}

std::string View::sample(double amount, bool is_count, long long seed) {
    const std::string prev = prev_name(stages_.size());
    std::string clause;
    if (is_count) {
        if (amount < 0 || amount != std::floor(amount))
            return "sample, count needs a nonnegative integer";
        clause = "reservoir(" + std::to_string(static_cast<long long>(amount)) +
                 " ROWS)";
    } else {
        if (amount <= 0 || amount > 100) return "sample percentage out of range";
        clause = "reservoir(" + dtoa(amount) + "%)";
    }
    if (seed >= 0) clause += " REPEATABLE (" + std::to_string(seed) + ")";
    push_stage("SELECT " + select_list() + " FROM " + prev + " USING SAMPLE " +
                   clause,
               "sample");
    return "";
}

std::string View::egen(const std::string &name, const std::string &fcn,
                       const std::string &arg_expr,
                       const std::vector<std::string> &by, bool statamissing,
                       const std::string &type_req) {
    if (col_index(name) >= 0) return "variable " + name + " already defined";
    ExprResult a = translate_expression(arg_expr, schema_of(cols_), statamissing);
    if (!a.ok) return a.error;
    if (a.kind == 's') return "egen " + fcn + "() needs a numeric expression";
    std::string part;
    if (!by.empty()) {
        std::vector<std::string> byn;
        std::string err = expand_patterns(by, &byn);
        if (!err.empty()) return err;
        for (size_t i = 0; i < byn.size(); i++) {
            if (i) part += ", ";
            /* GROUPKEY-1: per-group window keys fold ''/NaN to missing like Stata */
            part += norm_group_key(quote_ident(byn[i]), cols_[col_index(byn[i])].kind);
        }
    }
    std::string over = part.empty() ? "OVER ()" : "OVER (PARTITION BY " + part + ")";
    std::string agg;
    if (fcn == "total") agg = "coalesce(sum(" + a.sql + ") " + over + ", 0)";
    else if (fcn == "mean") agg = "avg(" + a.sql + ") " + over;
    else if (fcn == "sd") agg = "stddev_samp(" + a.sql + ") " + over;
    else if (fcn == "min") agg = "min(" + a.sql + ") " + over;
    else if (fcn == "max") agg = "max(" + a.sql + ") " + over;
    else if (fcn == "count") agg = "count(" + a.sql + ") " + over;
    else
        return "egen function " + fcn + "() is not supported (yet); supported: "
               "total mean sd min max count";
    /* EGEN-STORAGE-1: an explicit storage type on egen is value semantics, not
     * just metadata, exactly like generate. A string storage type is a type
     * mismatch for these numeric egen functions (native r(109)); a narrow
     * numeric type truncates toward zero and sends out-of-range results to
     * missing (e.g. native `egen byte t = total(x)` with sum 200 is `.`, not
     * 200). Reject strings loudly and coerce the numeric aggregate so the
     * collected value matches native Stata, not just the recorded meta_type. */
    if (!type_req.empty()) {
        StType mt;
        int mb = 0;
        if (sttype_parse(type_req, &mt, &mb) &&
            (mt == StType::Str || mt == StType::StrL))
            return "type mismatch: egen " + fcn +
                   "() produces a numeric result, not " + type_req;
    }
    std::string stored = coerce_storage(agg, type_req, 'n');
    push_stage("SELECT " + select_list() + ", " + stored + " AS " +
                   quote_ident(name) + " FROM " + prev_name(stages_.size()),
               "egen " + name + " = " + fcn + "(...)" +
                   (by.empty() ? "" : ", by(...)"));
    ViewCol nc;
    nc.name = name;
    nc.kind = 'n';
    nc.meta_type = type_req;
    cols_.push_back(nc);
    return "";
}

} // namespace parqit

namespace parqit {

/* ----------------------------------------------------- two-table verbs --- */

/* MISS-1 after a two-table verb: the combine introduces SQL NULLs into carried
 * string columns — merge's FULL/LEFT JOIN nulls the other side's strings on
 * every unmatched row, append's UNION BY NAME fills a source's absent columns
 * with NULL — so no string column of the result is guaranteed NULL-free any
 * more, on the master OR the using side. Clear the normalized flag on every
 * string column of the result manifest, else a lazy `parqit save` would skip
 * the coalesce(ref, '') boundary guard and write SQL NULL strings to disk
 * where the eager path writes '' for the same data. Numeric columns KEEP the
 * flag: a NULL numeric is a legitimate missing on disk, and the flag only
 * promises freedom from NaN/Inf — which a join/union cannot create. */
static void denormalize_strings(std::vector<ViewCol> *cols) {
    for (auto &c : *cols)
        if (c.kind == 's') c.normalized = false;
}

/* merge metadata of brought using columns into the view (labels follow the
 * column; value-label definitions merge name-wise, master wins) */
static void merge_using_meta(View *, nlohmann::json *dst_vallabs,
                             const nlohmann::json &src_vallabs,
                             std::vector<std::string> *warnings) {
    if (!src_vallabs.is_object()) return;
    for (const auto &l : src_vallabs.items()) {
        if (dst_vallabs->contains(l.key())) {
            if ((*dst_vallabs)[l.key()] != l.value())
                warnings->push_back("value label " + l.key() +
                                    " differs between master and using; "
                                    "keeping the master definition");
        } else {
            (*dst_vallabs)[l.key()] = l.value();
        }
    }
}

std::string View::merge_with(const std::string &kind,
                             const std::vector<std::string> &keys, UsingSide u,
                             const std::vector<std::string> &keepusing,
                             int keep_mask, const std::string &gen_name,
                             bool nogen, std::vector<std::string> *warnings) {
    if (keys.empty()) return "merge: key varlist required";
    if (kind != "1:1" && kind != "m:1" && kind != "1:m" && kind != "m:m")
        return "merge: kind must be 1:1, m:1, 1:m or m:m";

    /* keys must exist on both sides with matching kinds */
    std::map<std::string, const ViewCol *> ucols;
    for (const auto &c : u.cols) ucols[c.name] = &c;
    for (const auto &k : keys) {
        int mi = col_index(k);
        if (mi < 0) return "merge: key " + k + " not found in the master view";
        auto it = ucols.find(k);
        if (it == ucols.end()) return "merge: key " + k + " not found in the using data";
        if (cols_[mi].kind != it->second->kind)
            return "merge: key " + k + " is " +
                   (cols_[mi].kind == 's' ? "string" : "numeric") +
                   " in master but " +
                   (it->second->kind == 's' ? "string" : "numeric") + " in using";
    }
    std::set<std::string> keyset(keys.begin(), keys.end());

    /* Normalise a join key to Stata's missing/empty equivalence INSIDE the join
     * comparison (output key values are left untouched). Stata has no string
     * NULL and no NaN: a missing string is "" and a missing numeric is .  — so
     * a key that is "" / NULL / NaN on one side must match the parqit form on the
     * other. Without this, an out-of-core `parqit merge` of a third-party (esp.
     * pandas/pyarrow, which encodes a missing float as NaN) Parquet would give
     * different matches than native Stata, parqit mergein, and parqit collect. */
    auto key_norm = [&](const char *side, const std::string &k) -> std::string {
        std::string ref = std::string(side) + "." + quote_ident(k);
        int mi = col_index(k);
        if (mi >= 0 && cols_[mi].kind == 's') return "nullif(" + ref + ", '')";
        /* numeric: NaN ≡ missing. The CASE returns the original ref in the ELSE
         * branch, so an integer key keeps its exact type/value (no precision
         * loss); only an actual NaN (float/double) becomes NULL. */
        return "(CASE WHEN isnan(CAST(" + ref + " AS DOUBLE)) THEN NULL ELSE " +
               ref + " END)";
    };

    /* which using columns come across: keepusing ∩ (not in master unless key) */
    std::set<std::string> wanted;
    if (!keepusing.empty()) {
        for (const auto &w : keepusing) {
            bool hit = false;
            for (const auto &c : u.cols) {
                bool has_wild = w.find('*') != std::string::npos ||
                                w.find('?') != std::string::npos;
                if (has_wild ? glob_match(w, c.name) : (w == c.name)) {
                    wanted.insert(c.name);
                    hit = true;
                }
            }
            if (!hit) return "merge: keepusing variable " + w + " not found in the using data";
        }
    }
    std::vector<const ViewCol *> brought;
    for (const auto &c : u.cols) {
        if (keyset.count(c.name)) continue;
        if (!keepusing.empty() && !wanted.count(c.name)) continue;
        if (col_index(c.name) >= 0) {
            warnings->push_back("variable " + c.name +
                                " exists in master and using; master values kept");
            continue;
        }
        brought.push_back(&c);
    }

    /* _merge name: never collide silently (charter §6.12) */
    std::string mname = gen_name.empty() ? "_merge" : gen_name;
    if (!nogen && col_index(mname) >= 0)
        return "merge: variable " + mname +
               " already exists (use gen() or nogenerate)";

    const std::string prev = prev_name(stages_.size());
    /* helper names must dodge the USING side's columns too: a using column
     * literally named like a generated helper would make the __u.<helper>
     * references below ambiguous (charter §6.12). */
    std::set<std::string> utaken;
    for (const auto &c : u.cols) utaken.insert(c.name);
    std::string mm = fresh_helper("mm", utaken), um = fresh_helper("um", utaken);
    std::string rnm = fresh_helper("rnm", utaken), rnu = fresh_helper("rnu", utaken);
    std::string nmx = fresh_helper("nmx", utaken), nux = fresh_helper("nux", utaken);

    std::string keypart;
    for (size_t i = 0; i < keys.size(); i++) {
        if (i) keypart += ", ";
        keypart += quote_ident(keys[i]);
    }

    /* master/using prepped relations */
    std::string mrel, urel, joincond;
    const bool seq = (kind == "m:m");
    if (seq) {
        /* Stata's sequential m:m: within key, row i of the spine takes
         * master row min(i, nm) and using row min(i, nu) */
        /* TT-A1: the master within-key i-index must be reproducible. An empty
         * or key-only view sort would leave row_number() OVER (PARTITION BY key)
         * with no/partial ORDER BY -> engine-defined pairing. Honour any user
         * sort first, then break remaining ties by all master columns (NULLS
         * LAST) — mirroring the using side, which already orders by all its
         * columns — so the pairing never depends on row-group/thread order. */
        /* TT-MM-MISSING-1: the within-key windows and the spine partition on the
         * RAW key, but the spine<->side joins use the normalized key (key_norm,
         * ""≡NULL≡NaN). With mixed missing encodings across master/using (a
         * master NULL key, a using NaN key) the raw partition/spine forms two
         * groups that the normalized join then folds together, over-matching.
         * Normalize each key IN PLACE via SELECT * REPLACE so the windows, the
         * spine UNION and the joins all see one missing group; key_norm is
         * idempotent on an already-normalized value, and the output coalesce of
         * the normalized key renders missing exactly as Stata does. */
        std::string krepl = " REPLACE (";
        for (size_t i = 0; i < keys.size(); i++) {
            const std::string ref = quote_ident(keys[i]);
            std::string nk = (cols_[col_index(keys[i])].kind == 's')
                ? "nullif(" + ref + ", '')"
                : "(CASE WHEN isnan(CAST(" + ref +
                      " AS DOUBLE)) THEN NULL ELSE " + ref + " END)";
            krepl += (i ? ", " : "") + nk + " AS " + ref;
        }
        krepl += ")";
        /* TT-A1: the master within-key i-index must be reproducible. An empty
         * or key-only view sort would leave row_number() OVER (PARTITION BY key)
         * with no/partial ORDER BY -> engine-defined pairing. Honour any user
         * sort first, then break remaining ties by all master columns (NULLS
         * LAST) — mirroring the using side, which already orders by all its
         * columns — so the pairing never depends on row-group/thread order. */
        std::string m_order;
        std::string usort = order_by_sql();
        if (!usort.empty()) m_order = usort.substr(1); /* "ORDER BY ..." */
        for (size_t i = 0; i < cols_.size(); i++) {
            m_order += (m_order.empty() ? "ORDER BY " : ", ");
            m_order += quote_ident(cols_[i].name) + " NULLS LAST";
        }
        mrel = "(SELECT *" + krepl + ", TRUE AS " + quote_ident(mm) + ", row_number() OVER (PARTITION BY " +
               keypart + " " + m_order + ") AS " + quote_ident(rnm) +
               ", count(*) OVER (PARTITION BY " + keypart + ") AS " + quote_ident(nmx) +
               " FROM " + prev + ")";
        /* deterministic using-side pairing: order the row_number window by all
         * using columns so m:m pairing is reproducible (not engine-defined).
         * Pre-sort both sides to reproduce a specific native-Stata m:m run. */
        std::string u_order;
        for (size_t i = 0; i < u.cols.size(); i++)
            u_order += (i ? ", " : " ORDER BY ") + quote_ident(u.cols[i].name);
        urel = "(SELECT *" + krepl + ", TRUE AS " + quote_ident(um) + ", row_number() OVER (PARTITION BY " +
               keypart + u_order + ") AS " + quote_ident(rnu) + ", count(*) OVER (PARTITION BY " +
               keypart + ") AS " + quote_ident(nux) + " FROM (" + u.select_sql + "))";
    } else {
        mrel = "(SELECT *, TRUE AS " + quote_ident(mm) + " FROM " + prev + ")";
        urel = "(SELECT *, TRUE AS " + quote_ident(um) + " FROM (" + u.select_sql + "))";
    }
    for (size_t i = 0; i < keys.size(); i++) {
        if (i) joincond += " AND ";
        /* Stata semantics: missing keys match missing keys (incl. ""≡NULL≡NaN) */
        joincond += key_norm("__m", keys[i]) + " IS NOT DISTINCT FROM " +
                    key_norm("__u", keys[i]);
    }

    /* output projection: master cols (coalesced keys), brought using cols,
     * then the merge marker */
    std::vector<ViewCol> ncols;
    std::string sel;
    for (const auto &c : cols_) {
        if (!sel.empty()) sel += ", ";
        if (keyset.count(c.name))
            sel += "coalesce(__m." + quote_ident(c.name) + ", __u." +
                   quote_ident(c.name) + ") AS " + quote_ident(c.name);
        else
            sel += "__m." + quote_ident(c.name) + " AS " + quote_ident(c.name);
        ncols.push_back(c);
    }
    for (const ViewCol *c : brought) {
        sel += ", __u." + quote_ident(c->name) + " AS " + quote_ident(c->name);
        ncols.push_back(*c);
    }
    std::string merge_expr = "(CASE WHEN __u." + quote_ident(um) +
                             " IS NULL THEN 1 WHEN __m." + quote_ident(mm) +
                             " IS NULL THEN 2 ELSE 3 END)";
    std::string mtmp = fresh_helper("mg", utaken);
    sel += ", " + merge_expr + " AS " + quote_ident(mtmp);

    std::string body;
    if (!seq) {
        body = "SELECT " + sel + " FROM " + mrel + " AS __m FULL OUTER JOIN " + urel +
               " AS __u ON " + joincond;
    } else {
        /* spine: per key, i = 1..max(nm, nu) built from both rn sets */
        std::string spine_i = fresh_helper("i", utaken);
        std::string spine =
            "(SELECT " + keypart + ", " + quote_ident(rnm) + " AS " +
            quote_ident(spine_i) + " FROM " + mrel + " AS t UNION SELECT " + keypart +
            ", " + quote_ident(rnu) + " FROM " + urel + " AS t)";
        /* join master on clamped i, using on clamped i */
        std::string jm, ju;
        for (size_t i = 0; i < keys.size(); i++) {
            if (i) { jm += " AND "; ju += " AND "; }
            jm += key_norm("__s", keys[i]) + " IS NOT DISTINCT FROM " +
                  key_norm("__m", keys[i]);
            ju += key_norm("__s", keys[i]) + " IS NOT DISTINCT FROM " +
                  key_norm("__u", keys[i]);
        }
        jm += " AND __m." + quote_ident(rnm) + " = least(__s." + quote_ident(spine_i) +
              ", __m." + quote_ident(nmx) + ")";
        ju += " AND __u." + quote_ident(rnu) + " = least(__s." + quote_ident(spine_i) +
              ", __u." + quote_ident(nux) + ")";
        /* rebuild sel against __m/__u as before but keys come from spine */
        std::string sel2;
        for (const auto &c : cols_) {
            if (!sel2.empty()) sel2 += ", ";
            if (keyset.count(c.name))
                sel2 += "__s." + quote_ident(c.name) + " AS " + quote_ident(c.name);
            else
                sel2 += "__m." + quote_ident(c.name) + " AS " + quote_ident(c.name);
        }
        for (const ViewCol *c : brought)
            sel2 += ", __u." + quote_ident(c->name) + " AS " + quote_ident(c->name);
        sel2 += ", " + merge_expr + " AS " + quote_ident(mtmp);
        body = "SELECT " + sel2 + " FROM " + spine + " AS __s LEFT JOIN " + mrel +
               " AS __m ON " + jm + " LEFT JOIN " + urel + " AS __u ON " + ju;
    }

    /* keep() filter over the marker */
    if (keep_mask != 0 && keep_mask != 7) {
        std::string in;
        if (keep_mask & 1) in += "1";
        if (keep_mask & 2) in += std::string(in.empty() ? "" : ", ") + "2";
        if (keep_mask & 4) in += std::string(in.empty() ? "" : ", ") + "3";
        body = "SELECT * FROM (" + body + ") WHERE " + quote_ident(mtmp) + " IN (" +
               in + ")";
    }

    /* final projection: drop or rename the marker */
    std::string osel;
    for (const auto &c : ncols) {
        if (!osel.empty()) osel += ", ";
        osel += quote_ident(c.name);
    }
    if (!nogen) {
        osel += ", " + quote_ident(mtmp) + " AS " + quote_ident(mname);
        ViewCol mc;
        mc.name = mname;
        mc.kind = 'n';
        mc.varlab = "Matching result from merge";
        mc.meta_type = "byte";
        mc.vallab = "_merge"; /* TT-3 */
        ncols.push_back(mc);
    }
    body = "SELECT " + osel + " FROM (" + body + ")";

    merge_using_meta(this, &vallabs_, u.vallabs, warnings);
    if (!nogen) {
        /* Stata's standard _merge value label (TT-3): tabulate/list show
         * "Master only (1)" etc. like native merge. Numeric values are
         * unchanged. Set after merge_using_meta so a using-side label of the
         * same (reserved) name cannot shadow it. */
        vallabs_["_merge"] = {{"entries", nlohmann::json::array(
            {nlohmann::json::array({1, "Master only (1)"}),
             nlohmann::json::array({2, "Using only (2)"}),
             nlohmann::json::array({3, "Matched (3)"})})}};
    }
    denormalize_strings(&ncols); /* MISS-1: unmatched rows NULL either side's strings */
    cols_ = std::move(ncols);
    push_stage(body, "merge " + kind + " on " + keypart);
    /* native merge leaves the result sorted by the keys */
    sort_.clear();
    for (const auto &k : keys) sort_.push_back(quote_ident(k));
    return "";
}

std::string View::append_with(std::vector<UsingSide> sources,
                              const std::string &gen_name,
                              std::vector<std::string> *warnings) {
    if (sources.empty()) return "append: at least one using file required";
    if (!gen_name.empty() && col_index(gen_name) >= 0)
        return "append: generate() variable " + gen_name + " already exists";
    /* TT-A2: a generate() name colliding with a using-file column would emit a
     * duplicate output name into UNION ALL BY NAME and surface a cryptic DuckDB
     * Binder Error; check the using sides too and fail loudly and clearly. */
    if (!gen_name.empty())
        for (size_t s = 0; s < sources.size(); s++)
            for (const auto &c : sources[s].cols)
                if (c.name == gen_name)
                    return "append: generate() variable " + gen_name +
                           " already exists in using file " + std::to_string(s + 1);

    /* kind conflicts are loud (a column cannot be string here and numeric
     * there); new columns are adopted with the using side's metadata */
    std::vector<ViewCol> ncols = cols_;
    auto find_in = [&](const std::string &n) -> int {
        for (size_t i = 0; i < ncols.size(); i++)
            if (ncols[i].name == n) return static_cast<int>(i);
        return -1;
    };
    for (size_t s = 0; s < sources.size(); s++) {
        for (const auto &c : sources[s].cols) {
            int idx = find_in(c.name);
            if (idx < 0) {
                ncols.push_back(c);
            } else if (ncols[idx].kind != c.kind) {
                return "append: variable " + c.name + " is " +
                       (ncols[idx].kind == 's' ? "string" : "numeric") +
                       " in the view but " + (c.kind == 's' ? "string" : "numeric") +
                       " in using file " + std::to_string(s + 1);
            }
        }
    }
    /* validate-then-mutate (charter §6): merge the sources' value-label
     * definitions only after EVERY source passed the kind checks above — a
     * conflict in source 2 must not leave source 1's labels already merged
     * into a view the failed verb then abandons. */
    for (size_t s = 0; s < sources.size(); s++)
        merge_using_meta(this, &vallabs_, sources[s].vallabs, warnings);

    const std::string prev = prev_name(stages_.size());
    std::string body = "SELECT *" +
                       (gen_name.empty() ? std::string()
                                         : ", 0 AS " + quote_ident(gen_name)) +
                       " FROM " + prev;
    for (size_t s = 0; s < sources.size(); s++) {
        body += " UNION ALL BY NAME SELECT *";
        if (!gen_name.empty())
            body += ", " + std::to_string(s + 1) + " AS " + quote_ident(gen_name);
        body += " FROM (" + sources[s].select_sql + ")";
    }
    if (!gen_name.empty()) {
        ViewCol gc;
        gc.name = gen_name;
        gc.kind = 'n';
        gc.varlab = "Source of the observation (0 = master)";
        gc.meta_type = "byte";
        ncols.push_back(gc);
    }
    /* normalise the column order deterministically */
    std::string osel;
    for (const auto &c : ncols) {
        if (!osel.empty()) osel += ", ";
        osel += quote_ident(c.name);
    }
    body = "SELECT " + osel + " FROM (" + body + ")";

    denormalize_strings(&ncols); /* MISS-1: absent columns fill with NULL */
    cols_ = std::move(ncols);
    push_stage(body, "append (" + std::to_string(sources.size()) + " using file(s))");
    return "";
}

std::string View::joinby_with(const std::vector<std::string> &keys, UsingSide u,
                              std::vector<std::string> *warnings) {
    if (keys.empty()) return "joinby: key varlist required";
    std::map<std::string, const ViewCol *> ucols;
    for (const auto &c : u.cols) ucols[c.name] = &c;
    for (const auto &k : keys) {
        int mi = col_index(k);
        if (mi < 0) return "joinby: key " + k + " not found in the master view";
        auto it = ucols.find(k);
        if (it == ucols.end()) return "joinby: key " + k + " not found in the using data";
        if (cols_[mi].kind != it->second->kind)
            return "joinby: key " + k + " has different types in master and using";
    }
    std::set<std::string> keyset(keys.begin(), keys.end());

    std::vector<ViewCol> ncols = cols_;
    std::vector<const ViewCol *> brought;
    for (const auto &c : u.cols) {
        if (keyset.count(c.name)) continue;
        if (col_index(c.name) >= 0) {
            warnings->push_back("variable " + c.name +
                                " exists in master and using; master values kept");
            continue;
        }
        brought.push_back(&c);
        ncols.push_back(c);
    }

    const std::string prev = prev_name(stages_.size());
    /* normalise join keys to Stata's missing/empty equivalence ("" ≡ NULL,
     * NaN ≡ NULL) so joinby matches the same rows as native Stata (see the
     * matching helper in merge_with). */
    auto key_norm = [&](const char *side, const std::string &k) -> std::string {
        std::string ref = std::string(side) + "." + quote_ident(k);
        int mi = col_index(k);
        if (mi >= 0 && cols_[mi].kind == 's') return "nullif(" + ref + ", '')";
        /* TT-A3: use the same NaN-folding idiom as merge_with/the uniqueness
         * guard (isnan(CAST(... AS DOUBLE))) so the three never diverge. */
        return "(CASE WHEN isnan(CAST(" + ref + " AS DOUBLE)) THEN NULL ELSE " +
               ref + " END)";
    };
    std::string joincond;
    for (size_t i = 0; i < keys.size(); i++) {
        if (i) joincond += " AND ";
        joincond += key_norm("__m", keys[i]) + " IS NOT DISTINCT FROM " +
                    key_norm("__u", keys[i]);
    }
    std::string sel;
    for (const auto &c : cols_) {
        if (!sel.empty()) sel += ", ";
        sel += "__m." + quote_ident(c.name) + " AS " + quote_ident(c.name);
    }
    for (const ViewCol *c : brought)
        sel += ", __u." + quote_ident(c->name) + " AS " + quote_ident(c->name);

    merge_using_meta(this, &vallabs_, u.vallabs, warnings);
    /* MISS-1: joinby is an inner join today, but its result manifest follows
     * the same two-table contract as merge/append — clear the string flags so
     * a future unmatched() option (or any NULL the combine surfaces) can never
     * make a lazy save skip the coalesce('') guard. Conservative: the only
     * cost is a redundant guard on an already-clean column. */
    denormalize_strings(&ncols);
    cols_ = std::move(ncols);
    push_stage("SELECT " + sel + " FROM " + prev + " AS __m JOIN (" + u.select_sql +
                   ") AS __u ON " + joincond,
               "joinby");
    sort_.clear();
    for (const auto &k : keys) sort_.push_back(quote_ident(k));
    return "";
}

} // namespace parqit

namespace parqit {

/* ------------------------------------------------------------- reshape --- */

std::string View::reshape_long(const std::vector<std::string> &stubs,
                               const std::vector<std::string> &ivars,
                               const std::string &jname) {
    if (stubs.empty()) return "reshape long: stub varlist required";
    if (ivars.empty()) return "reshape long: i() required";
    if (jname.empty()) return "reshape long: j() required";
    if (col_index(jname) >= 0)
        return "reshape long: variable " + jname + " already defined";
    for (const auto &iv : ivars)
        if (col_index(iv) < 0) return "reshape long: i variable " + iv + " not found";

    /* discover suffixes: for each stub, columns named stub<suffix>. i()
     * variables are NEVER xij members even if their name starts with a stub
     * (e.g. stub `x`, i-var `x2`) — so an i-var is carried, not consumed. */
    std::set<std::string> ivarset(ivars.begin(), ivars.end());
    auto is_num_suffix = [](const std::string &suf) {
        if (suf.empty()) return false;
        for (char ch : suf)
            if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
        return true;
    };
    auto canonical_num_suffix = [](const std::string &suf) {
        size_t first = suf.find_first_not_of('0');
        return first == std::string::npos ? std::string("0") : suf.substr(first);
    };
    std::map<std::string, std::map<std::string, const ViewCol *>> cand;
    bool any_numeric_suffix = false, any_suffix = false;
    for (const auto &c : cols_) {
        if (ivarset.count(c.name)) continue; /* i() var: never an xij member */
        for (const auto &st : stubs) {
            if (c.name.size() > st.size() && c.name.compare(0, st.size(), st) == 0) {
                std::string suf = c.name.substr(st.size());
                cand[st][suf] = &c;
                any_suffix = true;
                if (is_num_suffix(suf)) any_numeric_suffix = true;
            }
        }
    }
    if (!any_suffix)
        return "reshape long: no variables match stub(s) " + stubs[0] + "…";
    /* If ANY suffix is numeric, j is numeric and ONLY numerically-suffixed
     * columns are xij; a non-numeric prefix match (e.g. `income` under stub
     * `inc`) is an unrelated variable that is carried — matching Stata, which
     * infers j={1,2} and keeps `income`. Otherwise j is a string over all
     * suffixes. */
    bool jnum = any_numeric_suffix;
    std::set<std::string> suffixes;
    std::map<std::string, std::map<std::string, const ViewCol *>> by_stub_suffix;
    std::map<std::string, std::set<std::string>> leading_zero_hint;
    std::set<std::string> stubcols;
    for (const auto &st : stubs) {
        for (const auto &kv : cand[st]) {
            if (jnum && !is_num_suffix(kv.first)) continue; /* carried, not xij */
            std::string suf = jnum ? canonical_num_suffix(kv.first) : kv.first;
            suffixes.insert(suf);
            if (jnum && kv.first != suf) {
                /* Stata treats inc01 as evidence that j=1 exists, but it
                 * carries inc01 as an ordinary column and looks for inc1 as
                 * the xij value. If inc1 is absent, the long stub is missing. */
                leading_zero_hint[st].insert(suf);
                continue;
            }
            by_stub_suffix[st][suf] = kv.second;
            stubcols.insert(kv.second->name);
        }
    }
    if (suffixes.empty())
        return "reshape long: no variables match stub(s) " + stubs[0] + "…";
    /* balance: every stub must have every (kept) suffix (loud, never silent) */
    for (const auto &st : stubs)
        for (const auto &suf : suffixes)
            if (!by_stub_suffix[st].count(suf))
                if (!(jnum && leading_zero_hint[st].count(suf)))
                    return "reshape long: variable " + st + suf +
                           " is missing (unbalanced stubs)";
    /* stub output kind: union of source kinds must agree per stub */
    std::map<std::string, char> stubkind;
    for (const auto &st : stubs) {
        char k0 = 0;
        for (const auto &suf : suffixes) {
            auto it = by_stub_suffix[st].find(suf);
            if (it == by_stub_suffix[st].end()) continue;
            char k = it->second->kind;
            if (k0 == 0) k0 = k;
            else if (k0 != k)
                return "reshape long: " + st + "* mixes string and numeric columns";
        }
        if (k0 == 0) k0 = 'n'; /* only leading-zero hints: native Stata emits . */
        stubkind[st] = k0;
    }

    /* carried columns: everything that is not a stub column; i vars first */
    std::vector<const ViewCol *> carried;
    for (const auto &c : cols_)
        if (!stubcols.count(c.name)) carried.push_back(&c);

    /* No two output columns may share a name. A bare column named exactly like a
     * stub (an `inc` beside `inc1`/`inc2`) or like j would otherwise emit two
     * same-named columns — silent corruption / data loss where Stata stops with
     * rc 110 (RESHAPE-5). */
    {
        std::set<std::string> seen;
        std::vector<std::string> outnames;
        for (const auto *c : carried) outnames.push_back(c->name);
        outnames.push_back(jname);
        for (const auto &st : stubs) outnames.push_back(st);
        for (const auto &nm : outnames)
            if (!seen.insert(nm).second)
                return "reshape long: variable " + nm +
                       " already defined (a kept column collides with a long "
                       "stub or with j)";
    }

    const std::string prev = prev_name(stages_.size());
    std::string body;
    bool first = true;
    for (const auto &suf : suffixes) {
        if (!first) body += " UNION ALL ";
        first = false;
        std::string sel;
        for (const auto *c : carried) {
            if (!sel.empty()) sel += ", ";
            sel += quote_ident(c->name);
        }
        sel += ", " + (jnum ? suf : quote_literal(suf)) + " AS " + quote_ident(jname);
        for (const auto &st : stubs) {
            auto it = by_stub_suffix[st].find(suf);
            std::string val;
            if (it != by_stub_suffix[st].end()) {
                val = quote_ident(it->second->name);
            } else {
                val = stubkind[st] == 's' ? "CAST(NULL AS VARCHAR)"
                                          : "CAST(NULL AS DOUBLE)";
            }
            sel += ", " + val + " AS " + quote_ident(st);
        }
        body += "SELECT " + sel + " FROM " + prev;
    }

    std::vector<ViewCol> ncols;
    for (const auto *c : carried) ncols.push_back(*c);
    ViewCol jc;
    jc.name = jname;
    jc.kind = jnum ? 'n' : 's';
    ncols.push_back(jc);
    for (const auto &st : stubs) {
        ViewCol sc;
        sc.name = st;
        sc.kind = stubkind[st];
        /* metadata rides along from the LOWEST j-value column (numeric order
         * for a numeric j, else lexicographic) — not whatever sorts first as a
         * string ("10" < "2"). */
        const ViewCol *c0 = nullptr;
        if (!by_stub_suffix[st].empty()) {
            c0 = by_stub_suffix[st].begin()->second;
            if (jnum) {
                double best = std::strtod(by_stub_suffix[st].begin()->first.c_str(),
                                          nullptr);
                for (const auto &kv : by_stub_suffix[st]) {
                    double jv = std::strtod(kv.first.c_str(), nullptr);
                    if (jv < best) { best = jv; c0 = kv.second; }
                }
            }
            sc.fmt = c0->fmt;
            sc.vallab = c0->vallab;
            sc.meta_type = c0->meta_type;
        }
        ncols.push_back(sc);
    }
    cols_ = std::move(ncols);
    push_stage("SELECT * FROM (" + body + ")", "reshape long");
    sort_.clear();
    for (const auto &iv : ivars) sort_.push_back(quote_ident(iv));
    sort_.push_back(quote_ident(jname));
    return "";
}

std::string View::reshape_wide(const std::vector<std::string> &stubs,
                               const std::vector<std::string> &ivars,
                               const std::string &jvar,
                               const std::vector<std::string> &jvalues,
                               bool j_is_string) {
    if (stubs.empty()) return "reshape wide: stub varlist required";
    if (ivars.empty()) return "reshape wide: i() required";
    if (jvalues.empty()) return "reshape wide: no j values (empty data?)";
    int ji = col_index(jvar);
    if (ji < 0) return "reshape wide: j variable " + jvar + " not found";
    std::set<std::string> known(ivars.begin(), ivars.end());
    known.insert(jvar);
    for (const auto &st : stubs) {
        if (col_index(st) < 0) return "reshape wide: variable " + st + " not found";
        known.insert(st);
    }
    for (const auto &iv : ivars)
        if (col_index(iv) < 0) return "reshape wide: i variable " + iv + " not found";
    /* Stata contract: every other variable must be named in i() or dropped */
    for (const auto &c : cols_)
        if (!known.count(c.name))
            return "reshape wide: variable " + c.name +
                   " is neither i(), j() nor a stub; drop it or add it to i()";
    /* new column names must be valid Stata names and free. A j value like -1
     * or "2.5" or one with spaces would form an illegal name (inc-1, …); Stata
     * refuses, so do we — loudly, instead of leaning on the collect-time
     * sanitiser to silently rewrite (and possibly collide). */
    auto valid_stata_name = [](const std::string &n) {
        if (n.empty() || n.size() > 32) return false;
        unsigned char c0 = static_cast<unsigned char>(n[0]);
        bool start = (c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z') ||
                     c0 == '_' || c0 >= 0x80;
        if (!start) return false;
        for (unsigned char c : n)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c >= 0x80))
                return false;
        return true;
    };
    for (const auto &st : stubs)
        for (const auto &jv : jvalues) {
            std::string nn = st + jv;
            if (!valid_stata_name(nn))
                return "reshape wide: generated name " + nn +
                       " is not a valid Stata variable name (from j value " + jv +
                       ")";
            if (known.count(nn) || col_index(nn) >= 0)
                return "reshape wide: generated name " + nn + " collides";
        }

    const std::string prev = prev_name(stages_.size());
    std::string sel;
    std::vector<ViewCol> ncols;
    for (const auto &iv : ivars) {
        if (!sel.empty()) sel += ", ";
        sel += quote_ident(iv);
        ncols.push_back(cols_[col_index(iv)]);
    }
    for (const auto &jv : jvalues) {
        for (const auto &st : stubs) {
            const ViewCol &sc = cols_[col_index(st)];
            std::string cond = quote_ident(jvar) +
                               (j_is_string ? " = " + quote_literal(jv)
                                            : " = " + jv);
            sel += ", max(" + quote_ident(st) + ") FILTER (WHERE " + cond +
                   ") AS " + quote_ident(st + jv);
            ViewCol nc = sc;
            nc.name = st + jv;
            ncols.push_back(nc);
        }
    }
    std::string gb;
    for (size_t i = 0; i < ivars.size(); i++) {
        if (i) gb += ", ";
        gb += quote_ident(ivars[i]);
    }
    /* MISS-1 sibling: an unbalanced (i,j) cell's max() FILTER emits SQL NULL
     * into a pivoted string column — clear the flag so save coalesces it */
    denormalize_strings(&ncols);
    cols_ = std::move(ncols);
    push_stage("SELECT " + sel + " FROM " + prev + " GROUP BY " + gb,
               "reshape wide");
    sort_.clear();
    for (const auto &iv : ivars) sort_.push_back(quote_ident(iv));
    return "";
}

std::string View::raw_fragment(const std::string &fragment) {
    if (fragment.empty()) return "query: empty SQL fragment";
    const std::string prev = prev_name(stages_.size());
    push_stage("SELECT " + select_list() + " FROM " + prev + " " + fragment,
               "query " + fragment);
    return "";
}

} // namespace parqit
