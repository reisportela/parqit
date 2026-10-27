#include "engine/view.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <random>
#include <set>
#include <stdexcept>

#include "engine/exprtrans.hpp"
#include "engine/sanitize.hpp"
#include "engine/session.hpp"
#include "engine/typemap.hpp"

namespace parqit {

using json = nlohmann::json;

static void denormalize_strings(std::vector<ViewCol> *cols); /* defined below */
static std::string prev_name(size_t k); /* defined below */

/* Stata's gen/egen with an explicit narrow storage type coerces the value the
 * way native Stata does at assignment time: integers TRUNCATE toward zero and
 * an out-of-range value becomes system missing (gen byte x=3.9 -> 3,
 * gen byte x=200 -> ., gen byte x=-2.5 -> -2 — verified against Stata). float
 * targets round to float32 precision, with values outside Stata's non-missing
 * float range mapped to missing instead of letting DuckDB's narrowing CAST
 * abort the later materialisation. The CAST to the matching DuckDB integer type
 * also makes the collected column size to the requested storage type
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
    case StType::Float:
        return "(CASE WHEN (" + v + ") IS NULL THEN NULL WHEN "
               "abs(CAST((" + v + ") AS DOUBLE)) > " + dtoa(kStataFloatMax) +
               " THEN NULL ELSE CAST((" + v + ") AS FLOAT) END)";
    case StType::Double: return "parqit_finite(__parqit_double(" + v + "))";
    default: return v; /* string-typed numeric targets are rejected by caller */
    }
    return "(CASE WHEN (" + v + ") IS NULL THEN NULL WHEN trunc(" + v + ") < " +
           dtoa(lo) + " OR trunc(" + v + ") > " + dtoa(hi) +
           " THEN NULL ELSE CAST(trunc(" + v + ") AS " + duckint + ") END)";
}

/* GROUPKEY-1: fold a grouping key's missing encodings together the way the
 * merge/joinby join and native Stata already do — an empty string and NULL
 * are both the missing string "", and NaN, ±Inf, a magnitude of 2^1023 or
 * more and NULL are all numeric missing. A GROUP BY / PARTITION BY on a bare
 * user key would otherwise split them into separate groups, diverging from
 * Stata and from parqit's own merge. KEYFOLD-1: the rule is the join's own
 * (key_missing_fold_sql), so the two cannot drift apart again; every column a
 * view holds today is boundary-normalised or finite-guarded, so the wider
 * fold changes no reachable result — it removes a second implementation.
 * `kind` is the key column's kind ('n' or 's'). */
static std::string norm_group_key(const std::string &ref, char kind) {
    return key_missing_fold_sql(ref, kind);
}

void View::close() { *this = View(); }

std::string View::coerce_numeric_column(const std::string &name,
                                        const std::string &type) {
    const int idx = col_index(name);
    if (idx < 0) return "variable " + name + " not found in the view";
    if (cols_[static_cast<size_t>(idx)].kind != 'n')
        return "variable " + name + " is not numeric";
    if (type != "float" && type != "double")
        return "internal numeric storage must be float or double";

    std::string sel;
    for (size_t i = 0; i < cols_.size(); i++) {
        if (i) sel += ", ";
        const std::string ref = quote_ident(cols_[i].name);
        if (static_cast<int>(i) == idx)
            sel += coerce_storage(ref, type, 'n') + " AS " + ref;
        else
            sel += ref;
    }
    if (order_live_) /* ORDER-CARRIER-1: a row-preserving stage */
        for (const auto &c : order_cols_) sel += ", " + c;
    cols_[static_cast<size_t>(idx)].meta_type = type;
    cols_[static_cast<size_t>(idx)].physical_type = type == "float" ? "FLOAT" : "DOUBLE";
    push_stage("SELECT " + sel + " FROM " + prev_name(stages_.size()),
               "commit " + name + " as " + type + " storage", true);
    return "";
}

void View::open(const std::string &scan_select, std::vector<ViewCol> cols,
                json vallabs, json chars, std::string dtalabel,
                const std::string &source_desc, std::vector<std::string> order_cols) {
    close();
    live_ = true;
    scan_ = scan_select;
    window_terms_ = count_window_terms(scan_select); /* WINDOW-NEST-1: a raw scan's windows */
    order_cols_ = std::move(order_cols); /* ORDER-CARRIER-1 */
    order_live_ = !order_cols_.empty();
    cols_ = std::move(cols);
    vallabs_ = std::move(vallabs);
    chars_ = std::move(chars);
    prune_chars(); /* a column left out at open takes its characteristics along */
    dtalabel_ = std::move(dtalabel);
    source_desc_ = source_desc;
}

int View::col_index(const std::string &name) const {
    for (size_t i = 0; i < cols_.size(); i++)
        if (cols_[i].name == name) return static_cast<int>(i);
    return -1;
}

/* ASCII case-insensitive equality of two DIFFERENT names — the clash DuckDB's
 * identifier resolution cannot tell apart (it folds ASCII case only). */
static bool ci_clash(const std::string &a, const std::string &b) {
    if (a == b || a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        unsigned char x = static_cast<unsigned char>(a[i]);
        unsigned char y = static_cast<unsigned char>(b[i]);
        if (x >= 'A' && x <= 'Z') x = static_cast<unsigned char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<unsigned char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

/* An engine alias is not a column identity across independently opened views.
 * Refuse ambiguous mappings before a join/union can combine different columns
 * or duplicate one column under two names. Aligned aliases remain valid. */
static std::string alias_identity_error(const std::vector<ViewCol> &cols,
                                         const ViewCol &incoming) {
    for (const auto &c : cols) {
        if ((c.name == incoming.name) != (c.exposed() == incoming.exposed()))
            return "column " + incoming.exposed() + " has an incompatible engine alias (" +
                   incoming.name + ") beside column " + c.exposed() + " (" + c.name +
                   "); rename the columns consistently in the source views before combining";
    }
    return "";
}

std::string View::ci_guard(const std::string &name,
                           const std::set<std::string> &ignore) const {
    for (const auto &c : cols_) {
        if (ignore.count(c.name)) continue;
        if (c.name != name && c.exposed() == name)
            return "variable " + name + " already exists under engine alias " +
                   c.name + "; use that alias or rename it first";
        if (ci_clash(c.name, name))
            return "variable " + name + " differs only by case from " + c.name +
                   ", which the engine cannot hold in one view (DuckDB column names "
                   "are case-insensitive); choose another name, or collect first "
                   "and create it in Stata";
    }
    return "";
}

std::string View::fresh_helper(const std::string &hint,
                               const std::set<std::string> &taken) {
    for (;;) {
        std::string cand = "__parqit_" + hint + "_" + std::to_string(++helper_counter_);
        /* charter §6.12: never collide — with the live manifest, nor with any
         * extra names the caller must dodge (a two-table verb's using side) */
        const auto collides = [&](const std::string &name) {
            return name == cand || ci_clash(name, cand);
        };
        const bool live_collision = std::any_of(cols_.begin(), cols_.end(),
            [&](const ViewCol &c) { return collides(c.name); });
        if (!live_collision && !std::any_of(taken.begin(), taken.end(), collides))
            return cand;
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

std::string View::stage_list() const {
    std::string out = select_list();
    if (order_live_)
        for (const auto &c : order_cols_) out += ", " + c;
    return out;
}

std::string View::sort_key_sql(const std::string &key) const {
    for (const auto &c : cols_) {
        if (c.kind != 's' || c.normalized) continue;
        const std::string q = quote_ident(c.name);
        if (key == q) return "coalesce(" + q + ", '')";
        if (key == q + " DESC") return "coalesce(" + q + ", '') DESC";
    }
    return key;
}

std::string View::order_by_sql() const {
    if (sort_.empty() && !(order_live_ && order_baked_)) return "";
    std::string o = " ORDER BY ";
    for (size_t i = 0; i < sort_.size(); i++) {
        if (i) o += ", ";
        /* explicit NULLS LAST so ordering (and every _n window, keep-in slice
         * and merge spine built on it) matches Stata's "missing sorts last in
         * every direction" regardless of DuckDB's default null order */
        o += sort_key_sql(sort_[i]) + " NULLS LAST";
    }
    /* ORDER-CARRIER-1: a tie in the declared keys keeps the source's physical
     * order — native Stata's order within a tie after use — and the same one
     * at every evaluation, so _n, keep in, list in and collect agree */
    if (order_live_) {
        bool first = sort_.empty();
        for (const auto &c : order_cols_) {
            o += std::string(first ? "" : ", ") + c;
            first = false;
        }
    }
    return o;
}

std::vector<std::string> View::sortedby_names() const {
    std::vector<std::string> out;
    for (const auto &key : sort_) {
        if (key.size() >= 5 && key.compare(key.size() - 5, 5, " DESC") == 0)
            return {}; /* gsort is ordered, but not Stata sortedby metadata */
        if (key.size() < 2 || key.front() != '"' || key.back() != '"')
            return {};
        /* A2-9 (audit 2026-08-22): the marker names the EXPOSED (Stata/file)
         * columns, never an engine alias — a view save writes it into
         * parqit.schema.sortedby and collect hands it to Stata's sort */
        const std::string engine = key.substr(1, key.size() - 2);
        const int i = col_index(engine);
        out.push_back(i < 0 ? engine : cols_[static_cast<size_t>(i)].exposed());
    }
    return out;
}

int View::count_window_terms(const std::string &sql) {
    int n = 0;
    const size_t len = sql.size();
    auto lower = [&](size_t k) { return static_cast<char>(std::tolower(static_cast<unsigned char>(sql[k]))); };
    for (size_t i = 0; i + 4 <= len; i++) {
        if (lower(i) != 'o' || lower(i + 1) != 'v' || lower(i + 2) != 'e' || lower(i + 3) != 'r') continue;
        if (i > 0 && (std::isalnum(static_cast<unsigned char>(sql[i - 1])) || sql[i - 1] == '_'))
            continue; /* ...cover (... */
        size_t j = i + 4;
        while (j < len && std::isspace(static_cast<unsigned char>(sql[j]))) j++;
        if (j >= len || sql[j] != '(') continue;
        j++;
        while (j < len && std::isspace(static_cast<unsigned char>(sql[j]))) j++;
        static const char kw[] = "partition";
        size_t k = 0;
        while (k < 9 && j + k < len && lower(j + k) == kw[k]) k++;
        if (k == 9 && j + 9 < len && std::isspace(static_cast<unsigned char>(sql[j + 9]))) n++;
    }
    return n;
}

void View::push_stage(const std::string &select_body, const std::string &desc,
                      bool keeps_order) {
    file_collapse_.reset();
    stages_.push_back(select_body);
    superseded_at_.push_back(0);
    egen_stages_.emplace_back();
    descs_.push_back(desc);
    /* WINDOW-NEST-1: the partitioned window terms this stage adds (a using
     * plan embedded by a two-table verb is in this text, counted here once) */
    window_terms_ += count_window_terms(select_body);
    /* ORDER-CARRIER-1: the order columns survive only a stage that declares
     * it keeps them AND selects them; any other stage ends the carrier, and
     * the view falls back to ordering by its declared keys alone */
    if (!keeps_order) order_live_ = false;
    for (const auto &c : order_cols_)
        if (select_body.find(c) == std::string::npos) order_live_ = false;
    if (!order_live_) order_baked_ = false;
    /* A removed key ends the declaration permanently. Reusing its name in a
     * later stage must not attach the old declaration to a different column. */
    if (!ts_time_.empty() && !ts_declared()) clear_tsset();
    prune_chars();
}

/* TWOTABLE-CHARS-1 (2026-10-01): native Stata drops a variable's
 * characteristics, notes included, with the variable. The view kept them after
 * drop/keep (a save only writes the live ones), so a later gen, egen or new
 * using variable of the same name took the dropped one's notes, and merge and
 * append would have treated them as the master's. Every verb names its
 * columns before its stage (egen adds its column after, once nothing is
 * stale), so pruning here, and when a view opens, keeps exactly the live
 * owners; engine names are kept too, as rename reads them. */
void View::prune_chars() {
    if (!chars_.is_object()) return;
    std::set<std::string> live{"_dta"};
    for (const auto &c : cols_) {
        live.insert(c.exposed());
        live.insert(c.name);
    }
    for (auto it = chars_.begin(); it != chars_.end();)
        it = live.count(it.key()) ? std::next(it) : chars_.erase(it);
}

/* refer to the previous pipeline step */
static std::string prev_name(size_t k) {
    return k == 0 ? "__parqit_s0" : ("__parqit_s" + std::to_string(k));
}

void View::append_stage_sql(std::string &sql, size_t stage) const {
    const auto &batch = egen_stages_[stage];
    if (!batch.first || batch.first == stage + 1) {
        sql += stages_[stage];
        return;
    }
    /* Keep each historical definition once. Retaining an expanded fused
     * SELECT at every prefix would add quadratic expression storage. */
    const auto &first = egen_stages_[batch.first - 1];
    sql += "SELECT " + first.columns;
    for (size_t k = batch.first - 1; k <= stage; ++k)
        sql += ", " + egen_stages_[k].definition;
    sql += " FROM " + first.from;
}

std::string View::ctes(size_t nstages) const {
    std::string sql = "WITH __parqit_s0 AS (" + scan_ + ")";
    for (size_t k = 0; k < nstages; k++) {
        if (superseded_at_[k] && superseded_at_[k] <= nstages) continue;
        sql += ", __parqit_s" + std::to_string(k + 1) + " AS (";
        append_stage_sql(sql, k);
        sql += ")";
    }
    return sql;
}

std::string View::compile_prefix(size_t nstages) const {
    return ctes(nstages) + " SELECT * FROM " + prev_name(nstages);
}

std::string View::compile(bool with_order) const {
    /* ORDER-CARRIER-1: the result exposes the view's columns only; the hidden
     * order columns, while the last stage still has them, may still order it */
    const std::string last = prev_name(stages_.size());
    std::string sql = order_cols_.empty() ? compile_prefix(stages_.size())
                                          : ctes(stages_.size()) + " SELECT " +
                                                select_list() + " FROM " + last;
    if (with_order) sql += order_by_sql();
    return sql;
}

std::vector<std::string> View::compact_sort_keys() const {
    if (subset_stages_ != stages_.size() || sort_.empty()) return {};
    std::vector<std::string> result;
    std::set<std::string> seen;
    for (const auto &key : sort_) {
        for (const auto &column : cols_) {
            const auto q = quote_ident(column.name);
            if (column.kind == 'n' && (key == q || key == q + " DESC") &&
                seen.insert(q).second) result.push_back(q);
        }
    }
    if (order_live_)
        for (const auto &q : order_cols_)
            if (seen.insert(q).second) result.push_back(q);
    return result;
}

std::string View::compile_compact_sort(const std::map<std::string, SortCast> &casts) const {
    if (casts.empty() || subset_stages_ != stages_.size() || sort_.empty()) return compile(true);
    std::string narrow, restore;
    auto append = [&](const std::string &q, bool visible) {
        const auto found = casts.find(q);
        if (!narrow.empty()) narrow += ", ";
        narrow += found == casts.end() ? q : found->second.function +
            "(CAST(" + q + " AS DOUBLE)) AS " + q;
        if (!visible) return;
        if (!restore.empty()) restore += ", ";
        if (found == casts.end()) restore += q;
        else if (!found->second.restore_function.empty())
            restore += found->second.restore_function + "(" + q + ") AS " + q;
        else restore += "CAST(" + q + " AS " + found->second.original_type + ") AS " + q;
    };
    for (const auto &column : cols_) append(quote_ident(column.name), true);
    if (order_live_) for (const auto &q : order_cols_) append(q, false);
    // Restore ABOVE the sort: restoring below it would reintroduce the DOUBLE
    // payload copy that DuckDB 2.0 retains for non-injective floating keys.
    // DEDUP-SORT-1: over the certified row-subset stages, or the source itself.
    return ctes(stages_.size()) + " SELECT " + restore + " FROM (SELECT * FROM (SELECT " +
        narrow + " FROM " + prev_name(stages_.size()) + ") __parqit_compact" + order_by_sql() +
        ") __parqit_sorted";
}

std::string View::compile_numbered(const std::string &obs) const {
    return ctes(stages_.size()) + " SELECT " + select_list() + ", row_number() " +
           row_number_over() + " AS " + obs + " FROM " + prev_name(stages_.size());
}

std::string View::show() const {
    std::string out = "-- source: " + source_desc_ + "\n";
    out += "WITH __parqit_s0 AS (\n  " + scan_ + "\n)";
    for (size_t k = 0; k < stages_.size(); k++) {
        if (superseded_at_[k]) continue;
        out += ",\n-- ";
        const size_t first = egen_stages_[k].first ? egen_stages_[k].first - 1 : k;
        for (size_t j = first; j <= k; ++j) out += (j == first ? "" : "; ") + descs_[j];
        out += "\n__parqit_s" + std::to_string(k + 1) + " AS (\n  ";
        append_stage_sql(out, k);
        out += "\n)";
    }
    out += "\nSELECT " + (order_cols_.empty() ? std::string("*") : select_list()) +
           " FROM " + prev_name(stages_.size());
    out += order_by_sql();
    return out;
}

/* ------------------------------------------------------------- helpers --- */

std::string View::expand_patterns(const std::vector<std::string> &patterns,
                                  std::vector<std::string> *out) const {
    std::vector<std::string> names, aliases;
    for (const auto &c : cols_) { names.push_back(c.name); aliases.push_back(c.stata); }
    std::vector<size_t> picked;
    const std::string error = resolve_varlist(patterns, names, aliases, varabbrev_, &picked);
    if (!error.empty()) return error + " in the view";
    for (size_t i : picked) out->push_back(names[i]);
    return "";
}

std::string View::resolve_name(const std::string &input, std::string *name) const {
    /* Individual variable positions never expand a wildcard to many columns. */
    for (const auto &c : cols_)
        if (input == c.name || (!c.stata.empty() && input == c.stata)) {
            *name = c.name;
            return "";
        }
    const ViewCol *match = nullptr;
    if (varabbrev_ && !input.empty())
        for (const auto &c : cols_)
            if (c.name.compare(0, input.size(), input) == 0 ||
                (!c.stata.empty() && c.stata.compare(0, input.size(), input) == 0)) {
                if (match) return input + " ambiguous abbreviation";
                match = &c;
            }
    if (!match) return "variable " + input + " not found in the view";
    *name = match->name;
    return "";
}

std::string View::set_formats(const std::vector<std::string> &names,
                              const std::vector<std::string> &formats) {
    if (names.empty() || names.size() != formats.size()) return "format: invalid format list";
    std::vector<int> indices;
    for (size_t i = 0; i < names.size(); ++i) {
        const int k = col_index(names[i]);
        if (k < 0) return "variable " + names[i] + " not found in the view";
        const auto &fmt = formats[i];
        const bool string_fmt = fmt.size() > 2 && fmt.back() == 's' &&
            fmt.find_first_not_of("0123456789-~", 1) == fmt.size() - 1;
        if (fmt.size() < 2 || fmt.front() != '%' ||
            string_fmt != (cols_[k].kind == 's'))
            return "format: incompatible display format for " + names[i];
        indices.push_back(k);
    }
    for (size_t i = 0; i < indices.size(); ++i) cols_[indices[i]].fmt = formats[i];
    return "";
}

/* NAME-CASE-1 invariant (audit 2026-08-22, A2-3): `ViewCol.stata` travels ONLY
 * with a column that keeps its engine name. Every verb that renames or derives
 * a column goes through here: the user's new name IS the Stata-facing name
 * (already ci-guarded), so the alias is cleared. */
static ViewCol derive_col(const ViewCol &src, const std::string &name) {
    ViewCol c = src;
    c.name = name;
    c.stata.clear();
    return c;
}

/* R1 / A2-4: characteristics and notes of a column are keyed by its EXPOSED
 * name (`NUEMP`, set at open/collect), so a rename must move the entry keyed
 * by the old exposed name — and, for safety, any entry keyed by the old
 * engine name — to the new name, or an aliased column's notes would be
 * orphaned and dropped on the next collect/save (META-2). */
static void move_chars(nlohmann::json *chars, const ViewCol &old_col,
                       const std::string &newn) {
    if (!chars->is_object()) return;
    nlohmann::json merged = nlohmann::json::object();
    for (const std::string &key : {old_col.exposed(), old_col.name}) {
        if (!chars->contains(key)) continue;
        if ((*chars)[key].is_object())
            for (const auto &kv : (*chars)[key].items())
                if (!merged.contains(kv.key())) merged[kv.key()] = kv.value();
        chars->erase(key);
    }
    /* a stale entry under the new name (a column dropped by an earlier
     * projection) must never attach its notes to the renamed column */
    chars->erase(newn);
    if (!merged.empty()) (*chars)[newn] = merged;
}

void View::set_numeric_types(const std::vector<std::string> &types) {
    for (size_t i = 0; i < cols_.size() && i < types.size(); ++i) {
        cols_[i].physical_type = types[i];
        if (types[i] == "DOUBLE" && cols_[i].meta_type == "float")
            cols_[i].meta_type = "double";
    }
}

static ExprSchema schema_of(const std::vector<ViewCol> &cols) {
    ExprSchema s;
    for (const auto &c : cols) {
        s.kinds[c.name] = c.kind;
        if (!c.stata.empty()) s.aliases[c.stata] = c.name;
        s.numeric_types[c.name] = c.physical_type;
        if (c.is_float()) s.float_columns.insert(c.name);
        if (c.normalized) s.normalized.insert(c.name); /* MISS-1 */
    }
    return s;
}

bool View::ts_declared() const {
    return !ts_time_.empty() && col_index(ts_time_) >= 0 &&
           (ts_panel_.empty() || col_index(ts_panel_) >= 0);
}

ExprSchema View::expr_schema() const {
    ExprSchema s = schema_of(cols_);
    s.varabbrev = varabbrev_;
    if (ts_declared()) { /* TSSET-1 */
        s.ts_panel = ts_panel_;
        s.ts_time = ts_time_;
        s.ts_delta = ts_delta_;
        s.ts_time_fmt = cols_[static_cast<size_t>(col_index(ts_time_))].fmt;
    }
    return s;
}

std::string View::tsset(const std::string &panel, const std::string &time, double delta) {
    std::vector<std::string> names;
    std::string err = expand_patterns({time}, &names);
    if (!err.empty()) return err;
    if (names.size() != 1 || time.find('*') != std::string::npos ||
        time.find('?') != std::string::npos)
        return "tsset: the time variable must be one variable, named exactly";
    const std::string tname = names[0];
    if (cols_[static_cast<size_t>(col_index(tname))].kind != 's')
        ; /* numeric: fine */
    else
        return "tsset: time variable " + time + " is a string; a time variable is numeric";
    std::string pname;
    if (!panel.empty()) {
        std::vector<std::string> pn;
        err = expand_patterns({panel}, &pn);
        if (!err.empty()) return err;
        if (pn.size() != 1 || panel.find('*') != std::string::npos ||
            panel.find('?') != std::string::npos)
            return "tsset: the panel variable must be one variable, named exactly";
        pname = pn[0];
        if (pname == tname) return "tsset: the panel and time variables must differ";
    }
    if (!(delta > 0) || !std::isfinite(delta)) return "tsset: delta() must be a positive number";
    ts_panel_ = pname;
    ts_time_ = tname;
    ts_delta_ = delta;
    return "";
}

std::string View::tsset_probe_sql(const std::string &panel, const std::string &time,
                                  double delta) const {
    const std::string t = quote_ident(time);
    const std::string p = panel.empty() ? std::string() : quote_ident(panel);
    /* the fields, in order: missing times (allowed, as native tsset allows
     * them: the operators are missing on those rows); fractional times
     * (native: "time variable must contain only integer values"); duplicates:
     * nonmissing times minus distinct (panel, time) pairs among them; min,
     * max, panels; gaps: a panel whose distinct times are fewer than its span
     * allows; periods below delta: consecutive nonmissing times closer than
     * delta (native: "time values with period less than delta() found") */
    const std::string pair = p.empty() ? t : "CASE WHEN " + t + " IS NOT NULL THEN (" + p + ", " + t + ") END";
    const std::string lagw = p.empty() ? "OVER (ORDER BY " + t + ")"
                                       : "OVER (PARTITION BY " + p + " ORDER BY " + t + ")";
    const std::string period = "(SELECT count(*) FROM (SELECT " + t + " - lag(" + t + ") " + lagw +
                               " AS d FROM (" + compile(false) + ") WHERE " + t +
                               " IS NOT NULL) WHERE d < " + dtoa(delta) + ")";
    std::string sql = "SELECT format('{}|{}|{}|{}|{}|{}|{}|{}', "
                      "count(*) FILTER (WHERE " + t + " IS NULL), "
                      "count(*) FILTER (WHERE " + t + " <> floor(" + t + ")), "
                      "count(" + t + ") - count(DISTINCT " + pair + "), "
                      /* every time missing: a NULL would void format() */
                      "coalesce(CAST(min(" + t + ") AS VARCHAR), ''), "
                      "coalesce(CAST(max(" + t + ") AS VARCHAR), ''), ";
    if (p.empty())
        sql += "0, CASE WHEN count(DISTINCT " + t + ") < floor((max(" + t + ") - min(" + t +
               ")) / " + dtoa(delta) + " + 1e-9) + 1 THEN 1 ELSE 0 END, " + period + ") FROM (" +
               compile(false) + ")";
    else
        sql += "count(DISTINCT " + p + "), (SELECT count(*) FROM (SELECT " + p + ", count(DISTINCT " +
               t + ") AS nt, min(" + t + ") AS lo, max(" + t + ") AS hi FROM (" + compile(false) +
               ") WHERE " + t + " IS NOT NULL GROUP BY " + p + ") WHERE nt < floor((hi - lo) / " +
               dtoa(delta) + " + 1e-9) + 1), " + period + ") FROM (" + compile(false) + ")";
    return sql;
}

static void substitute(std::string *sql, const std::string &from,
                       const std::string &to) {
    if (sql->find(from) == std::string::npos) return;
    /* Only bare generated tokens are placeholders. SQL literals and quoted
     * identifiers may contain the same bytes, including escaped quotes. */
    size_t p = 0;
    while (p < sql->size()) {
        const char ch = (*sql)[p];
        if (ch == '\'' || ch == '"') {
            const char quote = ch;
            p++;
            while (p < sql->size()) {
                if ((*sql)[p++] != quote) continue;
                if (p < sql->size() && (*sql)[p] == quote) p++;
                else break;
            }
        } else if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_') {
            const size_t begin = p++;
            while (p < sql->size() &&
                   (std::isalnum(static_cast<unsigned char>((*sql)[p])) || (*sql)[p] == '_'))
                p++;
            if (sql->compare(begin, p - begin, from) == 0) {
                sql->replace(begin, p - begin, to);
                p = begin + to.size();
            }
        } else {
            p++;
        }
    }
}

std::string View::row_number_over() const {
    const std::string ord = order_by_sql();
    return ord.empty() ? "OVER ()" : "OVER (" + ord.substr(1) + ")";
}

std::string View::order_by_sql_reversed() const {
    if (sort_.empty() && !order_live_) return "";
    std::string o = " ORDER BY ";
    bool first = true;
    for (const auto &key : sort_) {
        const std::string desc = " DESC";
        std::string k = key;
        bool was_desc = false;
        if (k.size() > desc.size() && k.compare(k.size() - desc.size(), desc.size(), desc) == 0) {
            k.resize(k.size() - desc.size());
            was_desc = true;
        }
        /* ascending NULLS LAST reversed is descending NULLS FIRST, and vice versa */
        o += std::string(first ? "" : ", ") + sort_key_sql(k) + (was_desc ? " NULLS FIRST" : " DESC NULLS FIRST");
        first = false;
    }
    if (order_live_)
        for (const auto &c : order_cols_) {
            o += std::string(first ? "" : ", ") + c + " DESC";
            first = false;
        }
    return o;
}

std::string View::check_by_order(const std::vector<std::string> &by,
                                  const std::vector<std::string> &sortvars) const {
    std::vector<std::string> part;
    return by_partition(by, sortvars, &part);
}

std::string View::by_partition(const std::vector<std::string> &by,
                               const std::vector<std::string> &sortvars,
                               std::vector<std::string> *part) const {
    part->clear();
    if (by.empty()) return "";
    std::vector<std::string> keys, svars;
    std::string err = expand_patterns(by, &keys);
    if (!err.empty()) return err;
    if (!sortvars.empty()) {
        err = expand_patterns(sortvars, &svars);
        if (!err.empty()) return err;
    }
    /* native by: requires the data sorted by the keys (and the sortvars) —
     * ascending, in that order; a longer sort is fine */
    std::vector<std::string> need = keys;
    need.insert(need.end(), svars.begin(), svars.end());
    bool sorted = sort_.size() >= need.size();
    for (size_t i = 0; sorted && i < need.size(); i++)
        sorted = sort_[i] == quote_ident(need[i]);
    if (!sorted) {
        std::string list;
        for (size_t i = 0; i < need.size(); i++) list += (i ? " " : "") + need[i];
        const std::string suggestion = ts_declared() && need.size() == 1 && need[0] == ts_panel_
                                          ? list + " " + ts_time_ : list;
        return "not sorted: by " + list + ": needs the view sorted by " + list +
               " (parqit sort " + suggestion + " first, or use parqit bysort)";
    }
    for (const auto &k : keys)
        part->push_back(norm_group_key(quote_ident(k), cols_[static_cast<size_t>(col_index(k))].kind));
    return "";
}

std::string View::rowctx_wrap(const std::vector<RowCtxItem> &items, const std::string &prev,
                              const std::vector<std::string> &part) {
    bool any = false;
    for (const auto &it : items) any = any || !it.windows->empty();
    if (!any) return prev;
    std::string partition;
    for (size_t i = 0; i < part.size(); i++) partition += (i ? ", " : "") + part[i];
    const std::string ord = order_by_sql(), rord = order_by_sql_reversed();
    auto over = [&](const std::string &o, const std::string &frame) {
        std::string body;
        if (!partition.empty()) body += "PARTITION BY " + partition;
        if (!o.empty()) body += (body.empty() ? "" : " ") + o.substr(1);
        if (!frame.empty()) body += (body.empty() ? "" : " ") + frame;
        return "OVER (" + body + ")";
    };
    const std::string full = "ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING";
    const std::string over_ord = over(ord, ""), over_part = over("", ""),
                      over_full = over(ord, full), over_full_desc = over(rord, full);
    std::string sel;
    for (const auto &it : items) {
        for (const auto &w : *it.windows) {
            const std::string helper = quote_ident(fresh_helper("win"));
            std::string wsql = w.sql;
            substitute(&wsql, "__PARQIT_OVER_ORD__", over_ord);
            substitute(&wsql, "__PARQIT_OVER_PART__", over_part);
            substitute(&wsql, "__PARQIT_OVER_FULL_DESC__", over_full_desc);
            substitute(&wsql, "__PARQIT_OVER_FULL__", over_full);
            sel += ", " + wsql + " AS " + helper;
            substitute(it.sql, w.placeholder, helper);
        }
    }
    return "(SELECT *" + sel + " FROM " + prev + ")";
}

static std::string by_desc(const std::vector<std::string> &by) {
    if (by.empty()) return "";
    std::string d = "by ";
    for (size_t i = 0; i < by.size(); i++) d += (i ? " " : "") + by[i];
    return d + ": ";
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
    std::string src = bake_order(prev);
    sort_.resize(keep);
    return src;
}

/* ORDER-CARRIER-1: bake the current order into the input of a stage that is
 * about to shorten or clear the declared sort (SEM-006, the projection above).
 * One ordinal freezes the full old order, including its ties, for this command
 * and later windows. A bare ORDER BY subquery is not enough: another window
 * may reorder it, particularly on CSV sources with no physical carrier. */
std::string View::bake_order(const std::string &prev) {
    const std::string ord = order_by_sql();
    /* a space in the name, which no Stata variable name can have, so no later
     * gen or rename can collide with the hidden column; it still dodges the
     * live columns, since a Parquet source may use any name */
    std::string name = "__parqit order " + std::to_string(++helper_counter_);
    while (std::any_of(cols_.begin(), cols_.end(), [&](const ViewCol &c) {
        return c.name == name || ci_clash(c.name, name);
    }))
        name += "_";
    const std::string rn = quote_ident(name);
    order_cols_ = {rn};
    order_live_ = true;
    order_baked_ = true;
    return "(SELECT *, row_number() OVER (" + ord.substr(1) + ") AS " + rn + " FROM " +
           prev + ")";
}

std::string View::keep_vars(const std::vector<std::string> &patterns) {
    std::vector<std::string> keep;
    std::string err = expand_patterns(patterns, &keep);
    if (!err.empty()) return err;
    std::vector<ViewCol> ncols;
    for (const auto &want : keep) ncols.push_back(cols_[col_index(want)]);
    const std::string src = projection_source(ncols); /* may bake a dropped sort */
    cols_ = std::move(ncols);
    push_stage("SELECT " + stage_list() + " FROM " + src,
               "keep " + std::to_string(cols_.size()) + " variables", true);
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
    push_stage("SELECT " + stage_list() + " FROM " + src,
               "drop " + std::to_string(dropset.size()) + " variables", true);
    return "";
}

std::string View::filter(const std::string &stata_expr, bool drop,
                         bool statamissing, const std::vector<std::string> &by,
                         const std::vector<std::string> &sortvars) {
    std::vector<std::string> part;
    std::string err = by_partition(by, sortvars, &part);
    if (!err.empty()) return err;
    ExprResult r = translate_filter(stata_expr, expr_schema(), statamissing);
    if (!r.ok) return r.error;
    /* keep if: WHERE expr (missing ⇒ dropped, exactly Stata's outcome);
     * drop if: keep rows where the condition is NOT TRUE — missing kept */
    std::string cond =
        drop ? "((" + r.sql + ") IS DISTINCT FROM TRUE)" : "(" + r.sql_where + ")";
    const std::string prev = prev_name(stages_.size());
    const std::string src = rowctx_wrap({{&cond, &r.windows}}, prev, part);
    push_stage("SELECT " + stage_list() + " FROM " + src + " WHERE " + cond,
               by_desc(by) + (drop ? "drop if " : "keep if ") + stata_expr, true);
    return "";
}

std::string View::gen(const std::string &name, const std::string &type_req,
                      const std::string &expr, const std::string &if_expr,
                      bool statamissing, const std::vector<std::string> &by,
                      const std::vector<std::string> &sortvars) {
    if (col_index(name) >= 0) return "variable " + name + " already defined";
    {
        const std::string cig = ci_guard(name);
        if (!cig.empty()) return cig;
    }
    std::vector<std::string> part;
    std::string perr = by_partition(by, sortvars, &part);
    if (!perr.empty()) return perr;
    ExprResult r = translate_expression(expr, expr_schema(), statamissing);
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
    const std::string effective_type =
        (type_req.empty() && r.kind != 's') ? "double" : type_req;
    std::string vexpr = coerce_storage(r.sql, effective_type, r.kind);
    std::string cond;
    ExprResult c;
    if (!if_expr.empty()) {
        c = translate_filter(if_expr, expr_schema(), statamissing);
        if (!c.ok) return c.error;
        cond = c.sql;
    }
    const std::string prev = prev_name(stages_.size());
    /* ROWCTX-2: the value's and the condition's window terms (each numbered
     * by its own translation) become helper columns of one wrapped source */
    const std::string src = rowctx_wrap({{&vexpr, &r.windows}, {&cond, &c.windows}}, prev, part);
    const std::string value = if_expr.empty()
                                  ? vexpr
                                  : "(CASE WHEN coalesce(" + cond + ", FALSE) THEN " + vexpr +
                                        " ELSE NULL END)";
    const std::string body = "SELECT " + stage_list() + ", " + value + " AS " +
                             quote_ident(name) + " FROM " + src;
    ViewCol nc;
    nc.name = name;
    nc.kind = (r.kind == 's' ? 's' : 'n');
    nc.meta_type = effective_type; /* untyped numeric gen is contractual double */
    nc.normalized = nc.kind == 'n'; /* coerce_storage already normalizes numeric results */
    cols_.push_back(nc);
    /* DERIVED-COLLAPSE-1: a gen without by() whose value and condition are
     * repeatable keeps the run of certified stages a compact collapse may
     * re-evaluate; push_stage alone never extends it */
    const bool repeatable = part.empty() && r.repeatable && (if_expr.empty() || c.repeatable);
    const bool extends = repeatable && repeatable_stages_ == stages_.size();
    push_stage(body, by_desc(by) + "gen " + name + " = " + expr +
                         (if_expr.empty() ? "" : " if " + if_expr), true);
    if (extends) repeatable_stages_ = stages_.size();
    return "";
}

std::string View::replace(const std::string &input_name, const std::string &expr,
                          const std::string &if_expr, bool statamissing,
                          const std::vector<std::string> &by,
                          const std::vector<std::string> &sortvars) {
    std::string name;
    const std::string nerr = resolve_name(input_name, &name);
    if (!nerr.empty()) return nerr;
    int idx = col_index(name);
    if (idx < 0) return "variable " + name + " not found in the view";
    std::vector<std::string> part;
    std::string perr = by_partition(by, sortvars, &part);
    if (!perr.empty()) return perr;
    ExprResult r = translate_expression(expr, expr_schema(), statamissing);
    if (!r.ok) return r.error;
    char newkind = (r.kind == 's' ? 's' : 'n');
    if (newkind != cols_[idx].kind)
        return "type mismatch: cannot replace " +
               std::string(cols_[idx].kind == 's' ? "string" : "numeric") + " " +
               name + " with a " + (newkind == 's' ? "string" : "numeric") +
               " expression";
    /* Native replace widens storage when the new value no longer fits (for
     * example byte -> int or str3 -> str6).  Keep the expression untruncated
     * in the lazy plan; collect-time type planning and lazy-save meta_type
     * casting preserve the old type when it still fits and promote when it
     * does not. */
    std::string assigned = r.sql, cond;
    ExprResult c;
    if (!if_expr.empty()) {
        c = translate_filter(if_expr, expr_schema(), statamissing);
        if (!c.ok) return c.error;
        cond = c.sql;
    }
    /* REPLACE-SELF-1: native replace is sequential — `replace x = x[_n-1] if
     * missing(x)` carries each new value into the next row (the fill-down
     * idiom), and so does L.x on the replaced variable. A lazy plan computes
     * every row from the values before the command, so such a replace is
     * refused rather than returned wrong; a new variable, or parqit sql, is
     * the way to express it (Codex review, 2026-10-13). */
    {
        const std::string self = quote_ident(name);
        auto reads_self = [&](const ExprResult &side) {
            return std::any_of(side.windows.begin(), side.windows.end(),
                               [&](const WindowTerm &win) {
                                   return win.sql.find(self) != std::string::npos;
                               });
        };
        if (reads_self(r) || reads_self(c))
            return "replace " + name + " from its own earlier or later rows (" + expr +
                   (if_expr.empty() ? "" : " if " + if_expr) +
                   ") is sequential in native Stata — each new value feeds the next row — "
                   "and a lazy plan computes every row from the values before the command; "
                   "generate a new variable instead, or use parqit sql";
    }
    /* SEM-006: sort_ describes the values at the moment sort ran.  Replacing a
     * key must first bake that old order into the input, then truncate Stata's
     * sortedby marker before the changed key.  Applying the stale key only at
     * final materialisation associated subsequent _n/keep-in with wrong rows. */
    std::string source = prev_name(stages_.size());
    size_t changed_sort = sort_.size();
    const std::string qname = quote_ident(name);
    for (size_t i = 0; i < sort_.size(); i++) {
        if (sort_[i] == qname || sort_[i] == qname + " DESC") {
            changed_sort = i;
            break;
        }
    }
    /* Freeze once before evaluating positions: the command and every later
     * window share the same tie resolution, even when no sort key survives. */
    if (changed_sort < sort_.size()) source = bake_order(source);
    source = rowctx_wrap({{&assigned, &r.windows}, {&cond, &c.windows}}, source, part);
    if (changed_sort < sort_.size()) sort_.resize(changed_sort);
    const std::string value =
        if_expr.empty() ? assigned
                        : "(CASE WHEN coalesce(" + cond + ", FALSE) THEN " + assigned +
                              " ELSE " + quote_ident(name) + " END)";

    std::string sel;
    for (size_t i = 0; i < cols_.size(); i++) {
        if (i) sel += ", ";
        if (static_cast<int>(i) == idx)
            sel += value + " AS " + quote_ident(name);
        else
            sel += quote_ident(cols_[i].name);
    }
    if (order_live_) /* ORDER-CARRIER-1: a row-preserving stage */
        for (const auto &c : order_cols_) sel += ", " + c;
    /* replacing changes content, and a wider value may no longer fit the
     * saved narrow type. MISS-1: the new expression may also introduce an IEEE
     * special (e.g. replace f = f*f), so drop its normalized flag. */
    cols_[idx].normalized = false;
    /* Never serialize a stale narrow integer/string type into parqit.schema;
     * those results are re-sized at materialisation. FLOAT is retained here
     * provisionally: the plugin's bind probe keeps it only when the resulting
     * physical family is range-safe, otherwise it promotes the intent to
     * DOUBLE. An existing double stays contractual double. */
    if (!cols_[idx].meta_type.empty() && cols_[idx].meta_type != "float" &&
             cols_[idx].meta_type != "double")
        cols_[idx].meta_type.clear();
    push_stage("SELECT " + sel + " FROM " + source,
               by_desc(by) + "replace " + name + " = " + expr +
                   (if_expr.empty() ? "" : " if " + if_expr), true);
    return "";
}

std::string View::rename(const std::string &input_name, const std::string &newn) {
    std::string oldn;
    const std::string nerr = resolve_name(input_name, &oldn);
    if (!nerr.empty()) return nerr;
    int idx = col_index(oldn);
    if (idx < 0) return "variable " + oldn + " not found in the view";
    if (col_index(newn) >= 0) return "variable " + newn + " already defined";
    {
        const std::string cig = ci_guard(newn, {oldn});
        if (!cig.empty()) return cig;
    }
    std::string sel;
    for (size_t i = 0; i < cols_.size(); i++) {
        if (i) sel += ", ";
        if (static_cast<int>(i) == idx)
            sel += quote_ident(oldn) + " AS " + quote_ident(newn);
        else
            sel += quote_ident(cols_[i].name);
    }
    /* characteristics (and notes, which are characteristics) are keyed by the
     * variable's EXPOSED name; follow the rename so they are not dropped on a
     * later save/collect, which serialises only chars whose key is a live
     * column (META-2). Value labels ride on ViewCol.vallab and need no remap.
     * R1/A2-4: the renamed column loses its case alias — the new name is the
     * Stata-facing name. */
    move_chars(&chars_, cols_[idx], newn);
    cols_[idx] = derive_col(cols_[idx], newn);
    /* sort keys referencing the old name follow the rename */
    for (auto &k : sort_) {
        if (k == quote_ident(oldn)) k = quote_ident(newn);
        if (k == quote_ident(oldn) + " DESC") k = quote_ident(newn) + " DESC";
    }
    if (ts_time_ == oldn) ts_time_ = newn;   /* TSSET-1: the declaration follows */
    if (ts_panel_ == oldn) ts_panel_ = newn;
    if (order_live_) /* ORDER-CARRIER-1: a row-preserving stage */
        for (const auto &c : order_cols_) sel += ", " + c;
    push_stage("SELECT " + sel + " FROM " + prev_name(stages_.size()),
               "rename " + oldn + " " + newn, true);
    return "";
}

std::string View::rename_many(const std::vector<std::string> &input_names,
                              const std::vector<std::string> &new_names) {
    std::vector<std::string> old_names;
    for (const auto &input : input_names) {
        std::string name;
        const std::string err = resolve_name(input, &name);
        if (!err.empty()) return err;
        old_names.push_back(name);
    }
    if (old_names.empty() || old_names.size() != new_names.size())
        return "rename: old and new lists must have the same nonzero length";
    std::set<std::string> oldset, newset;
    std::map<std::string, std::string> mapping;
    for (size_t i = 0; i < old_names.size(); i++) {
        if (!oldset.insert(old_names[i]).second)
            return "rename: variable " + old_names[i] +
                   " appears more than once in the old list";
        if (col_index(old_names[i]) < 0)
            return "variable " + old_names[i] + " not found in the view";
        if (!newset.insert(new_names[i]).second)
            return "rename: target variable " + new_names[i] +
                   " appears more than once";
        mapping[old_names[i]] = new_names[i];
    }
    for (const auto &nn : new_names)
        if (col_index(nn) >= 0 && !oldset.count(nn))
            return "variable " + nn + " already defined";
    for (const auto &nn : new_names) {
        /* NAME-CASE-1: against the survivors, and among the new names */
        const std::string cig = ci_guard(nn, oldset);
        if (!cig.empty()) return cig;
        for (const auto &other : new_names)
            if (other != nn && ci_clash(other, nn))
                return "rename: target variables " + nn + " and " + other +
                       " differ only by case, which the engine cannot hold in one view";
    }

    /* Build every replacement off to the side.  This single projection also
     * supports cycles (a b)->(b a); no intermediate public name exists. */
    std::vector<ViewCol> ncols = cols_;
    std::string sel;
    for (size_t i = 0; i < cols_.size(); i++) {
        if (i) sel += ", ";
        auto it = mapping.find(cols_[i].name);
        if (it == mapping.end()) {
            sel += quote_ident(cols_[i].name);
        } else {
            sel += quote_ident(cols_[i].name) + " AS " + quote_ident(it->second);
            ncols[i] = derive_col(cols_[i], it->second); /* R1: alias cleared */
        }
    }

    /* chars follow the rename, keyed by the EXPOSED name (R1/A2-4); collect
     * every moved entry first (a cycle (a b)->(b a) must not let b's entry be
     * read after a's was written over it), then reinsert under the new names */
    json nchars = chars_;
    if (nchars.is_object()) {
        std::map<std::string, json> moved;
        for (const auto &c : cols_) {
            auto it = mapping.find(c.name);
            if (it == mapping.end()) continue;
            json merged = json::object();
            for (const std::string &key : {c.exposed(), c.name}) {
                if (!nchars.contains(key)) continue;
                if (nchars[key].is_object())
                    for (const auto &kv : nchars[key].items())
                        if (!merged.contains(kv.key())) merged[kv.key()] = kv.value();
                nchars.erase(key);
            }
            moved[it->second] = merged;
        }
        for (const auto &m : moved) {
            nchars.erase(m.first); /* never inherit a stale entry under the new name */
            if (!m.second.empty()) nchars[m.first] = m.second;
        }
    }
    std::vector<std::string> nsort = sort_;
    for (auto &key : nsort) {
        for (const auto &m : mapping) {
            const std::string qold = quote_ident(m.first);
            if (key == qold) {
                key = quote_ident(m.second);
                break;
            } else if (key == qold + " DESC") {
                key = quote_ident(m.second) + " DESC";
                break;
            }
        }
    }

    cols_ = std::move(ncols);
    chars_ = std::move(nchars);
    sort_ = std::move(nsort);
    /* Map each original identity once, as for columns and sort keys. */
    const auto panel = mapping.find(ts_panel_), time = mapping.find(ts_time_);
    if (panel != mapping.end()) ts_panel_ = panel->second;
    if (time != mapping.end()) ts_time_ = time->second;
    if (order_live_) /* ORDER-CARRIER-1: a row-preserving stage */
        for (const auto &c : order_cols_) sel += ", " + c;
    push_stage("SELECT " + sel + " FROM " + prev_name(stages_.size()),
               "rename group (" + std::to_string(old_names.size()) + " variables)", true);
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
    push_stage("SELECT " + stage_list() + " FROM " + prev_name(stages_.size()),
               "order (column reorder)", true);
    return "";
}

std::string View::sort(const std::vector<std::string> &keys,
                       const std::vector<bool> &desc) {
    /* SORT-WILD-1: sort/gsort take explicit names — a sort key list is an
     * ordered contract, not a set, so a pattern's expansion order would be a
     * silent second contract. Detect the pattern itself: the old count-based
     * check below only fired when the expansion changed the key COUNT, so a
     * wildcard matching exactly one column slipped through and sorted by
     * whatever it happened to match. */
    for (const auto &k : keys)
        if (k.find('*') != std::string::npos || k.find('?') != std::string::npos)
            return "wildcards are not allowed in parqit sort";
    std::vector<std::string> names;
    std::string err = expand_patterns(keys, &names);
    if (!err.empty()) return err;
    if (names.size() != keys.size())
        return "wildcards are not allowed in parqit sort"; /* belt: duplicates */
    sort_.clear();
    for (size_t i = 0; i < names.size(); i++)
        sort_.push_back(quote_ident(names[i]) +
                        (i < desc.size() && desc[i] ? " DESC" : ""));
    /* order is plan state, not a stage — applied at materialisation and to
     * every _n window from here on */
    return "";
}

/* PCT-WINDOW-1: Stata's percentile rule (summarize/_pctile) over per-group
 * ranks. `prn` is the 1-based rank of the row's nonmissing value within its
 * group (NULL for a missing value), `pn` the group's nonmissing count;
 * np = pn*p/100, integral np averages x[np] and x[np+1] (x[n] when np == n),
 * otherwise x[ceil(np)]. A typed midpoint rounds once, so the non-integral
 * branch returns the element itself; an all-missing group has no
 * matching rank and yields NULL. The rank window spills to the temp
 * directory, which the per-group list_sort(list(x)) it replaced never did. */
std::string pct_window_sql(const std::string &ref, const std::string &prn,
                           const std::string &pn, double p, const std::string &over) {
    const std::string n = quote_ident(pn), rn = quote_ident(prn);
    const std::string w = over.empty() ? "" : " " + over;
    if (p == std::trunc(p)) {
        const std::string product = "(CAST(" + n + " AS HUGEINT) * " + std::to_string(static_cast<int>(p)) + ")";
        const std::string quotient = "(" + product + " // 100)", remainder = "(" + product + " % 100)";
        const std::string first = "greatest(1, " + quotient + " + CASE WHEN " + remainder + " = 0 THEN 0 ELSE 1 END)";
        const std::string second = "greatest(1, least(" + quotient + " + 1, " + n + "))";
        return "__parqit_midpoint(max(CASE WHEN " + rn + " = " + first + " THEN " + ref +
               " END)" + w + ", max(CASE WHEN " + rn + " = " + second + " THEN " + ref + " END)" + w + ")";
    }
    const std::string np = "(" + n + " * " + dtoa(p) + " / 100.0)";
    const std::string integral = np + " = floor(" + np + ")";
    const std::string k1 = "greatest(1, CASE WHEN " + integral + " THEN " + np + " ELSE ceil(" + np + ") END)";
    const std::string k2 = "(CASE WHEN " + integral + " THEN least(" + np + " + 1, " + n +
                           ") ELSE ceil(" + np + ") END)";
    return "__parqit_midpoint(max(CASE WHEN " + rn + " = " + k1 + " THEN " + ref +
           " END)" + w + ", max(CASE WHEN " + rn + " = greatest(1, " + k2 + ") THEN " + ref + " END)" + w + ")";
}

std::string View::collapse(const std::vector<CollapseSpec> &input_specs,
                           const std::vector<std::string> &by,
                           const std::map<std::string, ExactSum> &exact) {
    std::vector<CollapseSpec> specs = input_specs;
    for (auto &sp : specs) {
        const std::string err = resolve_name(sp.source, &sp.source);
        if (!err.empty()) return err;
    }
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
    /* NAME-CASE-1: every output name must be case-insensitively unique, or
     * DuckDB would dedup the aggregate's result and later references would
     * bind to the wrong column silently */
    {
        std::vector<std::string> outs(byn);
        for (const auto &sp : specs)
            outs.push_back(sp.target.empty() ? sp.source : sp.target);
        for (size_t i = 0; i < outs.size(); i++)
            for (size_t j = i + 1; j < outs.size(); j++)
                if (ci_clash(outs[i], outs[j]))
                    return "collapse: " + outs[i] + " and " + outs[j] +
                           " differ only by case, which the engine cannot hold in "
                           "one view";
    }
    std::string rn = fresh_helper("rn");
    const std::string prev = prev_name(stages_.size());
    std::string src = prev;
    std::string pre;
    std::string winsel; /* window columns the aggregate stage needs */
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
        winsel += ", row_number() " + over + " AS " + quote_ident(rn);
    }
    /* PCT-WINDOW-1 (audit 2026-09-01, T9): Stata's percentile rule needs the
     * k-th smallest nonmissing value of each group. The former
     * list_sort(list(x)) aggregate copied every group's values into an
     * in-memory list (not spillable, three copies in the SQL text) — bounded
     * by RAM on a huge group. Rank the nonmissing values per group with one
     * window per source column (the window operator sorts and can spill),
     * carry the group's nonmissing count alongside, and let the aggregate
     * stage pick the ranks it needs (pct_window_sql). Same values, same
     * arithmetic as before: (x[np] + x[np+1])/2 when np is integral, else
     * x[ceil(np)]; NULL for an all-missing group. */
    std::map<std::string, std::pair<std::string, std::string>> pct_helpers;
    {
        std::set<std::string> pct_sources;
        for (const auto &sp : specs) {
            double p = 0;
            /* COLLAPSE-STATS-2: iqr is p75 - p25 over the same ranks */
            const bool is_pct = sp.stat == "median" || sp.stat == "iqr" ||
                                (sp.stat.size() >= 2 && sp.stat[0] == 'p' &&
                                 atod(sp.stat.substr(1), &p) && p > 0 && p < 100);
            if (is_pct && col_index(sp.source) >= 0 &&
                cols_[col_index(sp.source)].kind == 'n')
                pct_sources.insert(sp.source);
        }
        std::string part;
        for (size_t i = 0; i < byn.size(); i++)
            part += (i ? ", " : "") +
                    norm_group_key(quote_ident(byn[i]), cols_[col_index(byn[i])].kind);
        for (const auto &sc : pct_sources) {
            const std::string prn = fresh_helper("prn"), pn = fresh_helper("pn");
            const std::string ref = quote_ident(sc);
            const std::string nullpart = "(" + ref + " IS NULL)";
            winsel += ", (CASE WHEN " + ref + " IS NULL THEN NULL ELSE row_number() OVER "
                      "(PARTITION BY " + (part.empty() ? nullpart : part + ", " + nullpart) +
                      " ORDER BY " + ref + ") END) AS " + quote_ident(prn);
            winsel += ", count(" + ref + ") OVER (" +
                      (part.empty() ? std::string() : "PARTITION BY " + part) + ") AS " +
                      quote_ident(pn);
            pct_helpers[sc] = {prn, pn};
        }
    }
    if (!winsel.empty()) src = "(SELECT *" + winsel + " FROM " + prev + ")";

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
    std::set<std::string> outnames(byn.begin(), byn.end()), dispersion;
    for (const auto &sp : specs)
        if (sp.stat == "sd" || sp.stat == "semean") dispersion.insert(sp.source);
    for (const auto &sp : specs) {
        int sidx = col_index(sp.source);
        if (sidx < 0) return "variable " + sp.source + " not found in the view";
        const ViewCol &scol = cols_[sidx];
        std::string tgt = sp.target.empty() ? sp.source : sp.target;
        if (!outnames.insert(tgt).second)
            return "collapse: duplicate output variable " + tgt;
        const std::string ref = quote_ident(sp.source);
        const auto promotion = exact.find(sp.source);
        const bool promoted = promotion != exact.end();
        const bool distinct = sp.stat == "unique" || sp.stat == "nunique";
        const bool cardinality = sp.stat == "count" || distinct;
        std::string agg;
        char kind = 'n';
        const std::string stat = std::string("(") + (dispersion.count(sp.source) ?
                                  "__parqit_stats(" : "__parqit_total(") + ref + "))";
        const bool floating = scol.physical_type == "FLOAT" || scol.physical_type == "DOUBLE" ||
                              (scol.physical_type.empty() && (scol.meta_type == "float" || scol.meta_type == "double"));
        /* COLLAPSE-STATS-2 (BPLIM feedback, 2026-10-13): the native list is
         * complete — semean = sd/sqrt(n), sebinomial = sqrt(p(1-p)/n) on 0/1
         * observations, sepoisson = sqrt(round(sum))/n on nonnegative data,
         * rawsum = sum (weights are not
         * supported, so the two coincide), percent = the group's share of the
         * nonmissing observations over every group, iqr = p75 - p25 under the
         * percentile rule. Invalid observations make the group's standard
         * error missing even when its mean is in range (v171). */
        if (sp.stat == "mean") {
            if (!promoted) agg = stat + ".mean";
            else if (promotion->second.integer32)
                agg = "__parqit_integer_mean(sum(" + ref + "), count(" + ref + "))";
            else
                agg = "__parqit_scaled_mean(sum(" + ref + "), count(" + ref + "), " +
                      std::to_string(promotion->second.exponent) + ")";
        }
        else if (sp.stat == "sum" || sp.stat == "rawsum")
            agg = promoted ? "coalesce(" + std::string(promotion->second.integer32 ?
                "__parqit_double(sum(" + ref + "))" : "__parqit_scaled_sum(sum(" + ref + "), " +
                std::to_string(promotion->second.exponent) + ")") + ", 0)" :
                floating ? stat + ".sum" : "coalesce(sum(" + ref + "), 0)";
        else if (sp.stat == "sd") agg = stat + ".sd";
        else if (sp.stat == "semean")
            agg = "parqit_finite(" + stat + ".sd / sqrt(CAST(" + stat + ".n AS DOUBLE)))";
        else if (sp.stat == "sebinomial" || sp.stat == "sepoisson")
            agg = "(__parqit_" + sp.stat + "(" + ref + ")).value";
        else if (sp.stat == "percent") {
            /* native: 100 * (nonmissing in the group) / (nonmissing over all
             * groups) — a window over the aggregate; a string source counts
             * its nonempty values, as (count) does */
            const std::string cnt = scol.kind == 's'
                ? "count(*) FILTER (WHERE coalesce(" + ref + ", '') <> '')"
                : "count(" + ref + ")";
            /* native's evaluation order is (n/N)*100 — 1/3 then *100 gives
             * 33.333333333333329, (100*1)/3 gives ...336: one ulp apart (the
             * oracle: collapse (percent) on 1/3, 5/7, 3/11, 8/11) */
            /* no nonmissing value anywhere: 0/0 is NaN under IEEE division,
             * missing after the finite guard — native collapse gives missing */
            agg = "parqit_finite((CAST(" + cnt + " AS DOUBLE) / CAST(sum(" + cnt +
                  ") OVER () AS DOUBLE)) * 100.0)";
        }
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
        else if (distinct) {
            /* COLLAPSE-UNIQUE-1: exact distinct values, with one missing
             * level, as in parqit unique. Fold NULL/empty strings and numeric
             * missing encodings with the same rule; retain the original key
             * type so adjacent BIGINTs above 2^53 cannot become one value. */
            const std::string key = norm_group_key(ref, scol.kind);
            agg = "count(DISTINCT " + key + ") + CASE WHEN count(*) > count(" +
                  key + ") THEN 1 ELSE 0 END";
        }
        else if (sp.stat == "median" || sp.stat == "iqr" ||
                 (sp.stat.size() >= 2 && sp.stat[0] == 'p')) {
            double p = 50;
            if (sp.stat != "median" && sp.stat != "iqr" &&
                (!atod(sp.stat.substr(1), &p) || p <= 0 || p >= 100))
                return "collapse: unknown statistic (" + sp.stat + ")";
            if (scol.kind != 'n')
                return "collapse: " + sp.stat + " needs a numeric variable (" +
                       sp.source + " is string)";
            const auto &h = pct_helpers.at(sp.source);
            agg = sp.stat == "iqr"
                      ? "__parqit_difference(" + pct_window_sql(ref, h.first, h.second, 75) +
                            ", " + pct_window_sql(ref, h.first, h.second, 25) + ")"
                      : pct_window_sql(ref, h.first, h.second, p);
            /* Native collapse stores a float source's percentile as float;
             * tabstat uses the unrounded double statistic. typeof is resolved
             * by the binder, including for a foreign FLOAT without metadata. */
            agg = "(CASE WHEN typeof(max(" + ref + ")) = 'FLOAT' THEN CAST(" +
                  agg + " AS FLOAT) ELSE " + agg + " END)";
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
              sp.stat == "lastnm" || cardinality))
            return "collapse: " + sp.stat + " needs a numeric variable (" +
                   sp.source + " is string)";
        if (!sel.empty()) sel += ", ";
        sel += agg + " AS " + quote_ident(tgt);
        ViewCol nc;
        nc.name = tgt;
        /* A2-5: a default target keeps the source column's identity, alias
         * included (collect/save expose `NUEMP`, not `NUEMP_1`); an explicit
         * target is a new Stata-facing name (ci-guarded above). */
        if (sp.target.empty()) nc.stata = scol.stata;
        nc.kind = cardinality ? 'n' : kind;
        /* COLLAPSE-META-1 (audit 2026-08-22, A3-8): native collapse labels the
         * result "(stat) source" and keeps the source's display format; a
         * (count) is a long. first/last/firstnm/lastnm keep the value label
         * and storage type too (they carry a source value). */
        nc.varlab = "(" + sp.stat + ") " + scol.exposed();
        /* COUNT-FMT-1 (audit 2026-09-01, F5): native collapse keeps the
         * source's display format on every target, count included (verified:
         * (count) n = price keeps %12.2f) — but a count of a STRING source
         * (parqit's extension; native refuses it) must not inherit a %s
         * format Stata rejects on a numeric variable, which made every
         * collect print "skipping display format %9s" */
        nc.fmt = (cardinality && scol.kind == 's') ? "%8.0g" : scol.fmt;
        if (sp.stat == "first" || sp.stat == "last" || sp.stat == "firstnm" ||
            sp.stat == "lastnm") {
            nc.vallab = scol.vallab;
            nc.meta_type = scol.meta_type;
        } else if (cardinality) {
            nc.meta_type = "long";
        }
        if (sp.stat == "mean" || sp.stat == "sd" || sp.stat == "semean" ||
            sp.stat == "sebinomial" || sp.stat == "sepoisson" || sp.stat == "percent" ||
            ((sp.stat == "sum" || sp.stat == "rawsum") && (floating || promoted))) {
            nc.physical_type = "DOUBLE";
            nc.normalized = true;
        } else if (cardinality) {
            nc.physical_type = "BIGINT";
            nc.normalized = true;
        } else if (sp.stat == "min" || sp.stat == "max" || sp.stat == "first" ||
                   sp.stat == "last" || sp.stat == "firstnm" || sp.stat == "lastnm") {
            nc.physical_type = scol.physical_type;
            nc.normalized = scol.normalized;
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
    // Retain a structured plan only for a first, order-independent collapse
    // over one literal file. Later stages invalidate it in push_stage. A glob
    // must not acquire additional files between an aggregate proof and use.
    // DERIVED-COLLAPSE-1: certified row-local gens may precede it; the proof
    // and the aggregate then evaluate the same compiled prefix.
    bool eligible = repeatable_stages_ == stages_.size() && source_single_file_ && ranges_.empty() &&
                    !src_paths_sql_.empty() && !byn.empty() && exact.empty();
    for (const auto &sp : specs)
        eligible &= sp.stat == "mean" || sp.stat == "sum" || sp.stat == "rawsum" || sp.stat == "count";
    std::shared_ptr<const FileCollapse> saved;
    if (eligible) saved = std::make_shared<FileCollapse>(FileCollapse{cols_, specs, byn, stages_.size()});
    cols_ = std::move(ncols);
    push_stage(body, desc);
    file_collapse_ = std::move(saved);
    /* Stata collapse leaves the data sorted by the by-vars; earlier keep-in
     * ranges stay on the validation list (their prefixes are unchanged) */
    sort_.clear();
    for (const auto &b : byn) sort_.push_back(quote_ident(b));
    return "";
}

std::vector<std::string> View::compact_collapse_columns() const {
    if (!file_collapse_ || stages_.size() != file_collapse_->prefix + 1 || !ranges_.empty()) return {};
    const std::set<std::string> groups(file_collapse_->by.begin(), file_collapse_->by.end());
    std::set<std::string> seen;
    std::vector<std::string> result;
    for (const auto &spec : file_collapse_->specs) {
        if (spec.stat == "count" || groups.count(spec.source)) continue;
        if (seen.insert(spec.source).second) result.push_back(spec.source);
    }
    return result;
}

std::vector<std::string> View::compact_collapse_groups() const {
    return compact_collapse_columns().empty() ? std::vector<std::string>{} : file_collapse_->by;
}

std::string View::compact_collapse_source() const {
    return compile_prefix(file_collapse_ ? file_collapse_->prefix : 0);
}

std::string View::compile_compact_collapse(const std::map<std::string, ExactSum> &exact) const {
    if (exact.empty() || compact_collapse_columns().empty()) return compile(true);
    auto columns = file_collapse_->columns;
    std::string select;
    for (auto &column : columns) {
        if (!select.empty()) select += ", ";
        const auto q = quote_ident(column.name);
        const auto promotion = exact.find(column.name);
        if (promotion == exact.end()) select += q;
        else {
            select += promotion->second.integer32 ? "__parqit_sort_i32(" + q + ")" :
                "__parqit_scaled_integer(" + q + ", " + std::to_string(promotion->second.exponent) + ")";
            select += " AS " + q;
            column.physical_type = promotion->second.integer32 ? "INTEGER" : "HUGEINT";
        }
    }
    View temporary;
    const std::string input = file_collapse_->prefix ? compile_prefix(file_collapse_->prefix) : scan_;
    temporary.open("SELECT " + select + " FROM (" + input + ") __parqit_source", std::move(columns),
                   {}, {}, "", source_desc_);
    const auto error = temporary.collapse(file_collapse_->specs, file_collapse_->by, exact);
    if (!error.empty()) throw std::logic_error("internal compact collapse plan: " + error);
    temporary.sort_ = sort_; // a later sort/gsort declaration is still authoritative
    return temporary.compile(true);
}

std::string View::contract(const std::vector<std::string> &by,
                           const std::string &freq) {
    std::vector<std::string> byn;
    std::string err = expand_patterns(by, &byn);
    if (!err.empty()) return err;
    if (byn.empty()) return "contract needs a varlist";
    std::string fname = freq.empty() ? "_freq" : freq;
    for (const auto &b : byn) {
        if (b == fname) return "freq() name collides with a contracted variable";
        if (ci_clash(b, fname))
            return "freq() name " + fname + " differs only by case from " + b +
                   ", which the engine cannot hold in one view";
    }
    /* CONTRACT-FREQ-1 (audit 2026-08-22, A2-15.4): native contract stops with
     * r(110) when the frequency variable already exists; parqit silently
     * projected the old column away. Refuse loudly (a live column, by alias or
     * exact name, that is not itself a by-variable). */
    for (const auto &c : cols_)
        if ((c.name == fname || c.exposed() == fname) &&
            std::find(byn.begin(), byn.end(), c.name) == byn.end())
            return "contract: variable " + fname +
                   " already defined; name the frequency variable with freq()";
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

std::string View::duplicates_drop(const std::vector<std::string> &by, bool force,
                                  const std::string &if_expr, bool statamissing) {
    const std::string prev = prev_name(stages_.size());
    if (!if_expr.empty()) {
        /* IF-ALL-1: native `duplicates drop [varlist] if exp` deduplicates the
         * rows the condition selects — the first of each group in the current
         * order stays — and keeps every other row where it is. Number the
         * selected rows within their group (the condition is part of the
         * partition, so an unselected row never takes a selected group's
         * first place) and keep rn = 1 or the rows outside the condition.
         * A missing condition is false, as in native if. */
        ExprResult c = translate_filter(if_expr, expr_schema(), statamissing);
        if (!c.ok) return c.error;
        if (!c.windows.empty())
            return "_n/_N are not supported in the if of duplicates drop";
        std::vector<std::string> byn;
        if (by.empty()) {
            for (const auto &col : cols_) byn.push_back(col.name);
        } else {
            if (!force) return "duplicates drop with a varlist requires the force option";
            if (sort_.empty())
                return "duplicates drop <varlist> needs a defined order to be "
                       "deterministic: run parqit sort first";
            std::string err = expand_patterns(by, &byn);
            if (!err.empty()) return err;
        }
        const std::string cond = quote_ident(fresh_helper("dupif"));
        const std::string rn = quote_ident(fresh_helper("rn"));
        std::string part;
        for (size_t i = 0; i < byn.size(); i++) {
            if (i) part += ", ";
            part += norm_group_key(quote_ident(byn[i]), cols_[col_index(byn[i])].kind);
        }
        push_stage("SELECT " + stage_list() + " FROM (SELECT *, " + cond +
                       ", row_number() OVER (PARTITION BY " + part + ", " + cond +
                       order_by_sql() + ") AS " + rn + " FROM (SELECT *, coalesce(" +
                       c.sql + ", FALSE) AS " + cond + " FROM " + prev + ")) WHERE NOT " +
                       cond + " OR " + rn + " = 1",
                   "duplicates drop" + std::string(by.empty() ? "" : " (by varlist)") +
                       " if " + if_expr, true);
        return "";
    }
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
        /* DEDUP-SORT-1: every output value is one of its group's source values */
        const bool extends = subset_stages_ == stages_.size();
        push_stage("SELECT " + sel + " FROM " + prev + " GROUP BY " + grp,
                   "duplicates drop");
        if (extends) subset_stages_ = stages_.size();
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
    push_stage("SELECT " + stage_list() + " FROM (SELECT *, row_number() OVER "
               "(PARTITION BY " + part + order_by_sql() + ") AS " + quote_ident(rn) +
               " FROM " + prev + ") WHERE " + quote_ident(rn) + " = 1",
               "duplicates drop (by varlist, keeping the first in sort order)", true);
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
    push_stage("SELECT " + stage_list() + " FROM (" + inner + " LIMIT " +
                   std::to_string(l - f + 1) + " OFFSET " + std::to_string(f - 1) +
                   ")",
               "keep in " + std::to_string(f) + "/" + std::to_string(l), true);
    PendingRange pr;
    pr.stage = stages_.size() - 1; /* validate against the count BEFORE this */
    pr.f = f;
    pr.l = l;
    ranges_.push_back(pr);
    return "";
}

std::string View::drop_in(long long f, long long l) {
    /* DROP-IN-1 (audit 2026-09-01, F10): native `drop in f/l` removes the
     * observations f..l of the current order and keeps every other row in
     * place. keep_in slices with LIMIT/OFFSET; the complement numbers the rows
     * (over the declared order, or the engine order when none is declared —
     * the same caveat as _n and keep in) and keeps the rest. The range is
     * validated against the real count at materialisation like keep in
     * (charter §6.13). */
    if (f < 1 || l < f)
        return "invalid in range: need 1 <= f <= l (negative and inverted "
               "ranges are not supported on a lazy view)";
    const std::string prev = prev_name(stages_.size());
    const std::string rn = fresh_helper("rn");
    const std::string ord = order_by_sql();
    const std::string over = ord.empty() ? "OVER ()" : "OVER (" + ord.substr(1) + ")";
    push_stage("SELECT " + stage_list() + " FROM (SELECT *, row_number() " + over +
                   " AS " + quote_ident(rn) + " FROM " + prev + ") WHERE " +
                   quote_ident(rn) + " < " + std::to_string(f) + " OR " +
                   quote_ident(rn) + " > " + std::to_string(l),
               "drop in " + std::to_string(f) + "/" + std::to_string(l), true);
    PendingRange pr;
    pr.stage = stages_.size() - 1; /* validate against the count BEFORE this */
    pr.f = f;
    pr.l = l;
    ranges_.push_back(pr);
    return "";
}

std::string View::sample(double amount, bool is_count, long long seed) {
    if (!std::isfinite(amount)) return "sample amount must be finite";
    const std::string prev = prev_name(stages_.size());
    std::string clause;
    if (is_count) {
        if (amount < 0 || amount >= 0x1p63 || amount != std::floor(amount))
            return "sample, count needs a nonnegative integer below 2^63";
        clause = "reservoir(" + std::to_string(static_cast<long long>(amount)) +
                 " ROWS)";
    } else {
        if (amount <= 0 || amount > 100) return "sample percentage out of range";
    }
    if (seed < 0) {
        static std::mt19937 seeds(std::random_device{}());
        seed = seeds() & 0x7fffffffU;
    }
    if (!is_count) {
        if (amount == 100) return "";
        const std::string data = quote_ident(fresh_helper("sample_source"));
        const std::string row = quote_ident(fresh_helper("sample_row"));
        const std::string order = order_by_sql();
        const std::string over = order.empty() ? "" : order.substr(1);
        // Capture once; round the global count once. Block percentage reservoirs
        // floor their per-block counts and can reach an invalid zero-size state.
        push_stage("WITH " + data + " AS MATERIALIZED (SELECT " + stage_list() +
            ", row_number() OVER (" + over + ") AS " + row + " FROM " + prev +
            ") SELECT " + stage_list() + " FROM " + data +
            " ORDER BY hash(hash(" + row + ", " + std::to_string(seed) + ")), " + row +
            " LIMIT (SELECT __parqit_sample_count(CAST(count(*) AS UBIGINT), CAST(" +
            quote_literal(dtoa(amount)) + " AS DOUBLE)) FROM " + data + ")", "sample", true);
        return "";
    }
    clause += " REPEATABLE (" + std::to_string(seed) + ")";
    push_stage("SELECT " + stage_list() + " FROM " + prev + " USING SAMPLE " +
                   clause,
               "sample", true);
    return "";
}

std::string View::egen(const std::string &name, const std::string &fcn,
                       const std::string &arg_expr,
                       const std::vector<std::string> &by, bool statamissing,
                       const std::string &type_req, const std::string &arg_type) {
    EgenSpec sp;
    sp.name = name;
    sp.fcn = fcn;
    sp.expr = arg_expr;
    sp.by = by;
    sp.type_req = type_req;
    return egen(sp, statamissing, arg_type);
}

namespace {

/* the function families of egen (EGEN-ALL-1) */
bool egen_expr_fcn(const std::string &f) {
    static const std::set<std::string> k = {"total", "sum", "mean", "sd", "min", "max",
        "count", "median", "pctile", "iqr", "kurt", "skew", "mad", "mdev", "std", "pc", "rank"};
    return k.count(f) != 0;
}
bool egen_row_fcn(const std::string &f) {
    return f.rfind("row", 0) == 0 && f.size() > 3;
}
/* functions native egen refuses with by */
bool egen_no_by(const std::string &f) {
    static const std::set<std::string> k = {"anycount", "anymatch", "anyvalue", "concat",
        "cut", "diff", "ends", "fill", "group", "mtr", "tag"};
    return k.count(f) != 0 || egen_row_fcn(f);
}
/* Stata's %w.df / %0w.df as a printf format; "" for anything else */
std::string printf_of_stata_fixed(const std::string &fmt) {
    if (fmt.size() < 4 || fmt[0] != '%' || fmt.back() != 'f') return "";
    const std::string body = fmt.substr(1, fmt.size() - 2);
    const size_t dot = body.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= body.size()) return "";
    for (size_t i = 0; i < body.size(); i++)
        if (i != dot && !std::isdigit(static_cast<unsigned char>(body[i]))) return "";
    const bool zero = body[0] == '0';
    return std::string("%") + (zero ? body : body.substr(dot)) + "f";
}
std::string integer_label_text(std::string value) {
    const size_t dot = value.find('.');
    if (dot != std::string::npos && dot + 1 < value.size() &&
        std::all_of(value.begin() + dot + 1, value.end(), [](char c) { return c == '0'; }))
        value.resize(dot);
    return value == "-0" ? "0" : value;
}

std::string label_text_of(const nlohmann::json &vallabs, const std::string &lab,
                          const std::string &value, bool *found) {
    *found = false;
    if (lab.empty() || !vallabs.is_object() || !vallabs.contains(lab)) return "";
    const auto &def = vallabs.at(lab);
    if (!def.is_object() || !def.contains("entries") || !def["entries"].is_array()) return "";
    for (const auto &e : def["entries"]) {
        if (!e.is_array() || e.size() < 2) continue;
        std::string k = e[0].is_string() ? e[0].get<std::string>() : e[0].dump();
        /* Compare integer spellings exactly, without rounding wide integers
         * or decimals onto a label belonging to a different value. */
        if (integer_label_text(k) == integer_label_text(value) || (value.empty() && k == ".")) {
            *found = true;
            return e[1].is_string() ? e[1].get<std::string>() : e[1].dump();
        }
    }
    return "";
}
std::string utf8_prefix(const std::string &s, long long n) {
    if (n <= 0) return s;
    long long count = 0;
    size_t i = 0;
    while (i < s.size() && count < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        i += len;
        count++;
    }
    return s.substr(0, std::min(i, s.size()));
}

/* GROUP-TRUNCATE-1: what native egen group(), label truncate(#) keeps of a
 * text, as Stata 17 and later do with ustrtrim(udsubstr(s, 1, #)): the first
 * characters that fit in # display columns — one column below U+2E80 and for
 * U+FFFD, two from U+2E80 on, as udstrlen() counts them (checked for every
 * code point on Stata 19.5) — less the Unicode white space at either end (the
 * 25 characters ustrtrim() removes). A byte that is not UTF-8 is one column. */
std::string display_prefix_trimmed(const std::string &s, long long cols) {
    struct Char {
        size_t at, len;
        uint32_t cp;
    };
    std::vector<Char> chars;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        uint32_t cp = 0xFFFD;
        bool ok = len > 0 && i + len <= s.size();
        if (ok) {
            cp = len == 1 ? c : c & (0x7F >> len);
            for (size_t k = 1; k < len && ok; k++) {
                const unsigned char d = static_cast<unsigned char>(s[i + k]);
                ok = (d >> 6) == 2;
                cp = (cp << 6) | (d & 0x3F);
            }
        }
        if (!ok) chars.push_back({i, 1, 0xFFFD});
        else chars.push_back({i, len, cp});
        i += ok ? len : 1;
    }
    size_t n = 0;
    long long used = 0;
    for (; n < chars.size(); n++) {
        const long long w = chars[n].cp < 0x2E80 || chars[n].cp == 0xFFFD ? 1 : 2;
        if (used + w > cols) break;
        used += w;
    }
    auto space = [](uint32_t c) {
        return (c >= 0x09 && c <= 0x0D) || c == 0x20 || c == 0x85 || c == 0xA0 || c == 0x1680 ||
               (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F ||
               c == 0x3000;
    };
    size_t first = 0, last = n;
    while (first < last && space(chars[first].cp)) first++;
    while (last > first && space(chars[last - 1].cp)) last--;
    if (first == last) return std::string();
    return s.substr(chars[first].at, chars[last - 1].at + chars[last - 1].len - chars[first].at);
}

} // namespace

std::string View::egen_probe_sql(const EgenSpec &sp, bool statamissing, std::string *sql) const {
    sql->clear();
    if (sp.fcn == "cut" && sp.group > 0) {
        /* native: pctile x, nq(#) — the # - 1 quantiles under the percentile
         * rule, over the selected nonmissing values; plus min and max */
        if (sp.vars.size() != 1) return "egen cut() takes one variable";
        std::vector<std::string> v;
        std::string err = expand_patterns(sp.vars, &v);
        if (!err.empty()) return err;
        if (cols_[static_cast<size_t>(col_index(v[0]))].kind != 'n')
            return "egen cut() needs a numeric variable";
        if (sp.group < 2 || sp.group > 100000) return "egen cut(): group() must be between 2 and 100000";
        std::string where = "TRUE";
        if (!sp.if_expr.empty()) {
            ExprResult c = translate_filter(sp.if_expr, expr_schema(), statamissing);
            if (!c.ok) return c.error;
            if (!c.windows.empty()) return "_n/_N are not supported in the if of egen cut()";
            where = "coalesce(" + c.sql + ", FALSE)";
        }
        const std::string x = quote_ident(v[0]);
        std::string sel = "min(" + x + "), max(" + x + ")";
        for (long long j = 1; j < sp.group; j++)
            sel += ", " + pct_window_sql(x, "prn", "pn", 100.0 * static_cast<double>(j) /
                                                            static_cast<double>(sp.group));
        *sql = "SELECT " + sel + " FROM (SELECT " + x + ", row_number() OVER (ORDER BY " + x +
               ") AS \"prn\", count(*) OVER () AS \"pn\" FROM (" + compile(false) + ") WHERE " + where +
               " AND " + x + " IS NOT NULL)";
        return "";
    }
    if (sp.fcn == "group" && sp.label) {
        std::vector<std::string> keys;
        std::string err = expand_patterns(sp.vars, &keys);
        if (!err.empty()) return err;
        if (keys.empty()) return "egen group() needs a varlist";
        std::string where = "TRUE";
        if (!sp.if_expr.empty()) {
            ExprResult c = translate_filter(sp.if_expr, expr_schema(), statamissing);
            if (!c.ok) return c.error;
            if (!c.windows.empty()) return "_n/_N are not supported in the if of egen group()";
            where = "coalesce(" + c.sql + ", FALSE)";
        }
        std::string sel, grp, ord, nonmiss;
        for (size_t i = 0; i < keys.size(); i++) {
            const ViewCol &c = cols_[static_cast<size_t>(col_index(keys[i]))];
            const std::string k = norm_group_key(quote_ident(keys[i]), c.kind);
            sel += (i ? ", " : "") + (c.kind == 's' ? "coalesce(" + k + ", '')"
                                                     : "CAST(" + k + " AS VARCHAR)");
            grp += (i ? ", " : "") + k;
            ord += (i ? ", " : "") + (c.kind == 's' ? "coalesce(" + k + ", '')" : k);
            nonmiss += (i ? " AND " : "") + k + " IS NOT NULL";
        }
        /* the groups in key order, missing groups last only with missing */
        *sql = "SELECT " + sel + " FROM (" + compile(false) + ") WHERE " + where +
               (sp.missing ? "" : " AND " + nonmiss) + " GROUP BY " + grp + " ORDER BY " + ord +
               " LIMIT 65537";
        return "";
    }
    return "";
}

std::string View::egen(const EgenSpec &sp, bool statamissing, const std::string &arg_type) {
    const std::string &name = sp.name, &fcn = sp.fcn, &type_req = sp.type_req;
    if (col_index(name) >= 0) return "variable " + name + " already defined";
    {
        const std::string cig = ci_guard(name);
        if (!cig.empty()) return cig;
    }
    if (fcn == "fill" || fcn == "mtr")
        return "egen function " + fcn + "() is not available in a lazy view" +
               std::string(fcn == "fill" ? " (its pattern is inferred from a numlist; generate the "
                                           "sequence with _n or egen seq() instead)"
                                         : " (it needs the US marginal tax rate table)");
    const bool expr_fn = egen_expr_fcn(fcn), row_fn = egen_row_fcn(fcn);
    static const std::set<std::string> known = {"anycount", "anymatch", "anyvalue", "concat",
        "cut", "diff", "ends", "group", "mode", "seq", "tag"};
    if (!expr_fn && !row_fn && !known.count(fcn))
        return "egen function " + fcn + "() is not supported; see help parqit for the list";
    static const std::set<std::string> rows = {"rowfirst", "rowlast", "rowmax", "rowmean",
        "rowmedian", "rowmin", "rowmiss", "rownonmiss", "rowpctile", "rowsd", "rowtotal"};
    if (row_fn && !rows.count(fcn))
        return "egen function " + fcn + "() is not supported; see help parqit for the list";
    if (!sp.by.empty() && egen_no_by(fcn))
        return "egen " + fcn + "() may not be combined with by";

    /* ---- the groups and the condition ---------------------------------- */
    std::vector<std::string> byn, part;
    if (!sp.by.empty()) {
        std::string err = expand_patterns(sp.by, &byn);
        if (!err.empty()) return err;
        for (const auto &b : byn)
            part.push_back(norm_group_key(quote_ident(b), cols_[static_cast<size_t>(col_index(b))].kind));
    }
    ExprResult cond;
    std::string cond_sql = "TRUE";
    if (!sp.if_expr.empty()) {
        cond = translate_filter(sp.if_expr, expr_schema(), statamissing);
        if (!cond.ok) return cond.error;
        cond_sql = "coalesce(" + cond.sql + ", FALSE)";
    }
    /* ---- the argument ---------------------------------------------------- */
    ExprResult arg;
    std::vector<std::string> vars;
    char vkind = 'n';
    if (expr_fn) {
        if (sp.expr.empty()) return "egen " + fcn + "() needs an expression";
        arg = translate_expression(sp.expr, expr_schema(), statamissing);
        if (!arg.ok) return arg.error;
        if (arg.kind == 's') return "egen " + fcn + "() needs a numeric expression";
    } else if (fcn != "seq") {
        if (sp.vars.empty()) return "egen " + fcn + "() needs a variable list";
        std::string err = expand_patterns(sp.vars, &vars);
        if (!err.empty()) return err;
        const bool single = fcn == "mode" || fcn == "cut" || fcn == "anyvalue" || fcn == "ends";
        if (single && vars.size() != 1) return "egen " + fcn + "() takes one variable";
        for (const auto &v : vars) {
            const ViewCol &c = cols_[static_cast<size_t>(col_index(v))];
            const bool numeric_only = fcn == "cut" || fcn == "anyvalue" || fcn == "anycount" ||
                                      fcn == "anymatch" ||
                                      (row_fn && !(fcn == "rownonmiss" && sp.strok));
            if (numeric_only && c.kind == 's')
                return "egen " + fcn + "(): " + v + " is a string variable" +
                       (fcn == "rownonmiss" ? "; use the strok option" : "");
            if (fcn == "ends" && c.kind != 's') return "egen ends() needs a string variable";
        }
        if (single) vkind = cols_[static_cast<size_t>(col_index(vars[0]))].kind;
    }
    if (!type_req.empty()) {
        StType mt;
        int mb = 0;
        const bool string_result = fcn == "concat" || fcn == "ends" ||
                                   ((fcn == "mode" || fcn == "anyvalue") && vkind == 's');
        if (sttype_parse(type_req, &mt, &mb) &&
            (mt == StType::Str || mt == StType::StrL) != string_result)
            return "type mismatch: egen " + fcn + "() produces a " +
                   (string_result ? "string" : "numeric") + " result, not " + type_req;
    }

    /* ---- level 0: the previous stage with the window terms of the
     * expression and the condition as helper columns; level 1: the condition
     * (c, only with an if) and the argument (e) as helper columns, so every
     * later window is a plain reference.
     * WINDOW-PART-1: the condition is never a PARTITION BY key. Partitioning
     * a window by a computed boolean column makes DuckDB 1.5.3's planner
     * exponential in the number of stacked stages (8 mode() stages: 30 s on
     * 74 rows; none of ORDER BY, CASE or constant folding mattered) and, bare,
     * fails with an internal error. So an aggregate window takes the
     * condition as FILTER (WHERE c), a ranking window orders the selected rows
     * first (c DESC) so they get the ranks 1..n, and the final CASE masks the
     * rows outside the condition; without an if none of this is emitted. */
    const bool has_if = !sp.if_expr.empty();
    std::string c_sql = cond_sql, e_sql = expr_fn ? arg.sql : "NULL";
    std::string lvl = rowctx_wrap({{&c_sql, &cond.windows}, {&e_sql, &arg.windows}},
                                  prev_name(stages_.size()), part);
    const std::string c = has_if ? quote_ident(fresh_helper("c")) : "TRUE";
    /* the argument is a helper column only where several levels read it (the
     * multi-level functions); a single-level aggregate takes the expression
     * inline, the plan shape of 0.3.3 — a helper projection under every
     * stacked window made DuckDB 1.5.3's planner superlinear in the number of
     * stages (12 stacked mean(): 2.8 s on 74 rows, 0.07 s inline) */
    static const std::set<std::string> multi_level = {"median", "pctile", "iqr", "mad", "mdev",
                                                      "std", "mode"};
    const bool helper_e = expr_fn && multi_level.count(fcn) != 0;
    const std::string e = helper_e ? quote_ident(fresh_helper("e")) : "(" + e_sql + ")";
    {
        std::string defs;
        if (has_if) defs += c_sql + " AS " + c;
        if (helper_e) defs += std::string(defs.empty() ? "" : ", ") + e_sql + " AS " + e;
        lvl = defs.empty() ? lvl : "(SELECT *, " + defs + " FROM " + lvl + ")";
    }
    auto level = [&](const std::string &defs) { lvl = "(SELECT *, " + defs + " FROM " + lvl + ")"; };
    /* the window over the by groups (plus `extra` keys), ordered by `order` */
    auto over = [&](const std::string &extra, const std::string &order) {
        std::string body;
        for (const auto &k : part) body += (body.empty() ? "" : ", ") + k;
        if (!extra.empty()) body += (body.empty() ? "" : ", ") + extra;
        std::string o = order;
        if (has_if && !o.empty()) o = c + " DESC, " + o; /* the selected rows first */
        else if (has_if) o = c + " DESC";
        return "OVER (" + (body.empty() ? std::string() : "PARTITION BY " + body) +
               (o.empty() ? std::string() : std::string(body.empty() ? "" : " ") + "ORDER BY " + o) + ")";
    };
    const std::string filt = has_if ? " FILTER (WHERE " + c + ")" : "";
    const std::string ord = order_by_sql(); /* " ORDER BY ..." or "" */
    const std::string row_order = ord.empty() ? "" : ord.substr(10); /* after " ORDER BY " */
    /* an aggregate window over the whole group (no order) */
    std::string wbody;
    for (const auto &k : part) wbody += (wbody.empty() ? "" : ", ") + k;
    const std::string w = "OVER (" + (wbody.empty() ? std::string() : "PARTITION BY " + wbody) + ")";
    auto wagg = [&](const std::string &agg) { return agg + filt + " " + w; };
    auto wagg_extra = [&](const std::string &agg, const std::string &extra) {
        return agg + filt + " OVER (PARTITION BY " + (wbody.empty() ? "" : wbody + ", ") + extra + ")";
    };
    auto mask = [&](const std::string &r) {
        return has_if ? "CASE WHEN " + c + " THEN " + r + " END" : r;
    };
    auto mask_else = [&](const std::string &r, const std::string &other) {
        return has_if ? "CASE WHEN " + c + " THEN " + r + " ELSE " + other + " END" : r;
    };
    std::string result;
    char rkind = 'n';
    bool normalized = true;
    std::string cast; /* a value-label name carried to the column */

    if (expr_fn) {
        const std::string eref = e;
        if (fcn == "total" || fcn == "sum") {
            const bool exact_storage = type_req.empty() && !arg_type.empty() &&
                                       arg_type != "FLOAT" && arg_type != "DOUBLE";
            const std::string sum = exact_storage ? wagg("sum(" + eref + ")")
                                                  : "(" + wagg("__parqit_total(" + eref + ")") + ").sum";
            result = sp.missing ? "CASE WHEN " + wagg("count(" + eref + ")") + " = 0 THEN NULL ELSE " + sum + " END"
                                : "coalesce(" + sum + ", 0)";
        } else if (fcn == "mean") {
            result = "(" + wagg("__parqit_total(" + eref + ")") + ").mean";
        } else if (fcn == "sd") {
            result = "(" + wagg("__parqit_stats(" + eref + ")") + ").sd";
        } else if (fcn == "min") {
            result = wagg("min(" + eref + ")");
        } else if (fcn == "max") {
            result = sp.missing ? "CASE WHEN " + wagg("count(*)") + " > " + wagg("count(" + eref + ")") +
                                      " THEN NULL ELSE " + wagg("max(" + eref + ")") + " END"
                                : wagg("max(" + eref + ")");
        } else if (fcn == "count") {
            result = wagg("count(" + eref + ")");
        } else if (fcn == "kurt" || fcn == "skew") {
            result = "(" + wagg("__parqit_detail(" + eref + ")") + ")." +
                     (fcn == "kurt" ? "kurtosis" : "skewness");
        } else if (fcn == "mdev") {
            const std::string m = quote_ident(fresh_helper("m"));
            level("(" + wagg("__parqit_total(" + eref + ")") + ").mean AS " + m);
            result = "(" + wagg("__parqit_total(abs(__parqit_double(" + eref + ") - " + m + "))") + ").mean";
        } else if (fcn == "std") {
            const std::string m = quote_ident(fresh_helper("m")), sdv = quote_ident(fresh_helper("s"));
            if (part.empty()) {
                // A whole-input window cannot use DuckDB's partition self-join
                // rewrite. At large N this plan limits parallelism and makes
                // ordered CTAS expensive. Compute the identical state with a
                // parallel aggregate and broadcast its single row. Materialise
                // the argument/qualifier once, including volatile/row-context work.
                const std::string input = quote_ident(fresh_helper("std_source"));
                const std::string aggregate = quote_ident(fresh_helper("std_aggregate"));
                const std::string state = quote_ident(fresh_helper("std_state"));
                lvl = "(WITH " + input + " AS MATERIALIZED (SELECT * FROM " + lvl + "), " +
                      aggregate + " AS (SELECT __parqit_stats(" + eref + ")" + filt +
                      " AS " + state + " FROM " + input + ") SELECT " + input + ".*, (" +
                      aggregate + "." + state + ").mean AS " + m + ", (" + aggregate + "." +
                      state + ").sd AS " + sdv + " FROM " + input + " CROSS JOIN " + aggregate + ")";
            } else {
                level("(" + wagg("__parqit_stats(" + eref + ")") + ").mean AS " + m + ", (" +
                      wagg("__parqit_stats(" + eref + ")") + ").sd AS " + sdv);
            }
            result = "parqit_finite((__parqit_double(" + eref + ") - " + m + ") / " + sdv + " * " +
                     dtoa(sp.sd) + " + " + dtoa(sp.mean) + ")";
        } else if (fcn == "pc") {
            result = "parqit_finite(__parqit_double(" + eref + ") * " + (sp.prop ? "1.0" : "100.0") +
                     " / (" + wagg("__parqit_total(" + eref + ")") + ").sum)";
        } else if (fcn == "rank") {
            if ((sp.field ? 1 : 0) + (sp.track ? 1 : 0) + (sp.unique ? 1 : 0) > 1)
                return "egen rank(): field, track and unique are mutually exclusive";
            const std::string nn = "(" + eref + " IS NULL)";
            std::string r;
            if (sp.unique)
                r = "row_number() " + over(nn, eref + (row_order.empty() ? "" : ", " + row_order));
            else if (sp.field)
                r = "rank() " + over(nn, eref + " DESC");
            else if (sp.track)
                r = "rank() " + over(nn, eref);
            else
                r = "(2 * rank() " + over(nn, eref) + " + " + wagg_extra("count(*)", eref) +
                    " - 1) / 2.0";
            result = "CASE WHEN " + eref + " IS NOT NULL THEN " + r + " END";
        } else { /* median pctile iqr mad */
            double p = 50;
            if (fcn == "pctile") {
                p = sp.has_p ? sp.p : 50;
                if (!(p > 0 && p < 100)) return "egen pctile(): p() must be between 0 and 100";
            }
            /* the rank of each selected nonmissing value within its group and
             * the group's count of them; a row outside the condition has no
             * rank, so the picks below never see it */
            auto rank_level = [&](const std::string &x, std::string *prn, std::string *pn) {
                *prn = fresh_helper("prn");
                *pn = fresh_helper("pn");
                level("(CASE WHEN " + x + " IS NULL" + (has_if ? " OR NOT " + c : "") +
                      " THEN NULL ELSE row_number() " + over("(" + x + " IS NULL)", x) + " END) AS " +
                      quote_ident(*prn) + ", " + wagg("count(" + x + ")") + " AS " + quote_ident(*pn));
            };
            std::string prn, pn;
            rank_level(eref, &prn, &pn);
            if (fcn == "iqr")
                result = "__parqit_difference(" + pct_window_sql(eref, prn, pn, 75, w) + ", " +
                         pct_window_sql(eref, prn, pn, 25, w) + ")";
            else if (fcn == "mad") {
                const std::string m = quote_ident(fresh_helper("m")), d = quote_ident(fresh_helper("d"));
                level(pct_window_sql(eref, prn, pn, 50, w) + " AS " + m);
                level("abs(__parqit_double(" + eref + ") - " + m + ") AS " + d);
                std::string prn2, pn2;
                rank_level(d, &prn2, &pn2);
                result = pct_window_sql(d, prn2, pn2, 50, w);
            } else
                result = pct_window_sql(eref, prn, pn, p, w);
        }
        result = mask(result);
        normalized = true;
    } else if (fcn == "seq") {
        const std::string rn = quote_ident(fresh_helper("rn"));
        level("row_number() " + over("", row_order) + " AS " + rn);
        if (sp.block < 1) return "egen seq(): block() must be a positive whole number";
        const std::string k = "((" + rn + " - 1) // " + std::to_string(sp.block) + ")";
        std::string v;
        if (!sp.has_to) {
            /* SEQ-TO-1: native to() is _N, every row of the data whatever if()
             * and by(): from() up to it, then again; down from from() when it
             * is the larger (_gseq.ado swaps them) */
            const std::string nn = quote_ident(fresh_helper("nn")), f = dtoa(sp.from);
            level("count(*) OVER () AS " + nn);
            v = "CASE WHEN " + f + " <= " + nn + " THEN " + f + " + (" + k + " % (" + nn + " - " + f +
                " + 1)) ELSE " + f + " - (" + k + " % (" + f + " - " + nn + " + 1)) END";
        } else if (sp.to >= sp.from)
            v = dtoa(sp.from) + " + (" + k + " % " + dtoa(sp.to - sp.from + 1) + ")";
        else
            v = dtoa(sp.from) + " - (" + k + " % " + dtoa(sp.from - sp.to + 1) + ")";
        result = mask(v);
    } else if (fcn == "mode") {
        const std::string v = quote_ident(vars[0]);
        if ((sp.minmode ? 1 : 0) + (sp.maxmode ? 1 : 0) + (sp.nummode > 0 ? 1 : 0) > 1)
            return "egen mode(): minmode, maxmode and nummode() are mutually exclusive";
        const std::string vv = vkind == 's' ? "nullif(" + v + ", '')" : v;
        const std::string f = quote_ident(fresh_helper("f"));
        /* the frequency of each value among the selected rows; 0 for a
         * missing value unless missing counts it */
        level((sp.missing ? wagg_extra("count(*)", vv)
                          : "CASE WHEN " + vv + " IS NULL THEN 0 ELSE " + wagg_extra("count(*)", vv) + " END") +
              " AS " + f);
        const std::string fm = quote_ident(fresh_helper("fm")), md = quote_ident(fresh_helper("md"));
        level(wagg("max(" + f + ")") + " AS " + fm);
        level("(" + (has_if ? c + " AND " : "") + f + " = " + fm + " AND " + fm + " > 0) AS " + md);
        const std::string lo = quote_ident(fresh_helper("lo")), hi = quote_ident(fresh_helper("hi")),
                          nm = quote_ident(fresh_helper("nm"));
        std::string defs = "min(CASE WHEN " + md + " THEN " + vv + " END) " + w + " AS " + lo +
                           ", max(CASE WHEN " + md + " THEN " + vv + " END) " + w + " AS " + hi +
                           ", max(CASE WHEN " + md + " AND " + vv + " IS NULL THEN 1 ELSE 0 END) " + w +
                           " AS " + nm;
        std::string pick;
        if (sp.nummode > 0) {
            /* the modal values ranked from the lowest: the modal rows sort
             * first, so their dense ranks are 1..K and the others are masked */
            const std::string dr = quote_ident(fresh_helper("dr"));
            defs += ", (CASE WHEN " + md + " THEN dense_rank() OVER (" +
                    (wbody.empty() ? "" : "PARTITION BY " + wbody + " ") + "ORDER BY " + md +
                    " DESC, " + vv + ") END) AS " + dr;
            level(defs);
            pick = quote_ident(fresh_helper("pk"));
            level("max(CASE WHEN " + dr + " = " + std::to_string(sp.nummode) + " THEN " + vv +
                  " END) " + w + " AS " + pick);
        } else
            level(defs);
        std::string r;
        if (sp.minmode) r = lo;
        else if (sp.maxmode) r = "CASE WHEN " + nm + " = 1 THEN NULL ELSE " + hi + " END";
        else if (sp.nummode > 0) r = pick;
        else r = "CASE WHEN " + nm + " = 0 AND " + lo + " = " + hi + " THEN " + lo + " END";
        result = mask(r);
        rkind = vkind;
        normalized = false;
    } else if (fcn == "group" || fcn == "tag") {
        std::string keys, rank_keys, anymiss;
        for (size_t i = 0; i < vars.size(); i++) {
            const ViewCol &col = cols_[static_cast<size_t>(col_index(vars[i]))];
            const std::string k = norm_group_key(quote_ident(vars[i]), col.kind);
            keys += (i ? ", " : "") + k;
            rank_keys += (i ? ", " : "") + (col.kind == 's' ? "coalesce(" + k + ", '')" : k);
            anymiss += (i ? " OR " : "") + k + " IS NULL";
        }
        /* a row outside the condition, or with a missing key (unless missing
         * counts it), takes no group: it sorts after every row that does, so
         * the numbering of those is 1..G, and the CASE masks it */
        std::string out_of = sp.missing ? "FALSE" : "(" + anymiss + ")";
        if (has_if) out_of = "(NOT " + c + (sp.missing ? "" : " OR " + anymiss) + ")";
        if (fcn == "group") {
            result = "CASE WHEN NOT " + out_of + " THEN dense_rank() OVER (ORDER BY " +
                     (out_of == "FALSE" ? "" : out_of + ", ") + rank_keys + ") END";
            if (sp.label) {
                /* the group texts, from the probe: decoded labels or values */
                if (sp.probe_rows.size() > 65536)
                    return "egen group(), label: more than 65,536 groups; omit label";
                std::string lab = sp.lname.empty() ? name : sp.lname;
                if (vallabs_.is_object() && vallabs_.contains(lab) && sp.lname.empty()) {
                    int k = 1;
                    while (vallabs_.contains(lab + std::to_string(k))) k++;
                    lab += std::to_string(k);
                }
                nlohmann::json entries = nlohmann::json::array();
                long long code = 0;
                for (const auto &row : sp.probe_rows) {
                    std::string text;
                    for (size_t i = 0; i < vars.size() && i < row.size(); i++) {
                        const ViewCol &col = cols_[static_cast<size_t>(col_index(vars[i]))];
                        bool found = false;
                        std::string piece = col.kind == 'n'
                            ? label_text_of(vallabs_, col.vallab, row[i], &found) : row[i];
                        if (!found && col.kind == 'n') piece = row[i].empty() ? "." : integer_label_text(row[i]);
                        /* GROUP-TRUNCATE-1: native truncate() cuts a string's
                         * text or a value label's, never a number written out */
                        if (sp.truncate > 0 && (col.kind != 'n' || found))
                            piece = display_prefix_trimmed(piece, sp.truncate);
                        text += (i ? " " : "") + piece;
                    }
                    entries.push_back(nlohmann::json::array({std::to_string(++code), text}));
                }
                if (!vallabs_.is_object()) vallabs_ = nlohmann::json::object();
                vallabs_[lab] = {{"entries", entries}};
                cast = lab; /* carried to the column below */
            }
        } else {
            result = "CASE WHEN NOT " + out_of + " AND row_number() OVER (PARTITION BY " + keys +
                     " ORDER BY " + (out_of == "FALSE" ? "" : out_of + (row_order.empty() ? "" : ", ")) +
                     row_order + ") = 1 THEN 1 ELSE 0 END";
            if (out_of == "FALSE" && row_order.empty())
                result = "CASE WHEN row_number() OVER (PARTITION BY " + keys + ") = 1 THEN 1 ELSE 0 END";
            result = "CAST(" + result + " AS TINYINT)";
        }
    } else if (fcn == "anycount" || fcn == "anymatch" || fcn == "anyvalue") {
        if (sp.values.empty()) return "egen " + fcn + "(): values() is required";
        std::string list;
        for (size_t i = 0; i < sp.values.size(); i++) list += (i ? ", " : "") + dtoa(sp.values[i]);
        if (fcn == "anyvalue") {
            const std::string v = quote_ident(vars[0]);
            result = mask("CASE WHEN " + v + " IN (" + list + ") THEN " + v + " END");
        } else {
            std::string sum;
            for (size_t i = 0; i < vars.size(); i++)
                sum += (i ? " + " : "") + std::string("(CASE WHEN ") + quote_ident(vars[i]) +
                       " IN (" + list + ") THEN 1 ELSE 0 END)";
            result = fcn == "anycount" ? mask_else("(" + sum + ")", "0")
                                       : mask_else("(CASE WHEN (" + sum + ") > 0 THEN 1 ELSE 0 END)", "0");
            result = "CAST(" + result + " AS TINYINT)";
        }
    } else if (fcn == "concat") {
        std::string pf;
        if (!sp.format.empty()) {
            pf = printf_of_stata_fixed(sp.format);
            if (pf.empty())
                return "egen concat(): format() takes a fixed format such as %9.2f or %03.0f";
        }
        std::string joined;
        for (size_t i = 0; i < vars.size(); i++) {
            const ViewCol &col = cols_[static_cast<size_t>(col_index(vars[i]))];
            const std::string v = quote_ident(vars[i]);
            std::string piece;
            if (col.kind == 's') piece = "coalesce(" + v + ", '')";
            else if (sp.decode && !col.vallab.empty() && vallabs_.is_object() &&
                     vallabs_.contains(col.vallab) && vallabs_[col.vallab].contains("entries")) {
                std::string cases;
                const std::string compared = col.physical_type == "FLOAT" || col.meta_type == "float"
                                                 ? "CAST(" + v + " AS DOUBLE)" : v;
                for (const auto &en : vallabs_[col.vallab]["entries"]) {
                    if (!en.is_array() || en.size() < 2) continue;
                    const std::string k = en[0].is_string() ? en[0].get<std::string>() : en[0].dump();
                    std::string t = en[1].is_string() ? en[1].get<std::string>() : en[1].dump();
                    if (sp.maxlength > 0) t = utf8_prefix(t, sp.maxlength);
                    if (k == ".") {
                        cases += " WHEN " + v + " IS NULL THEN " + quote_literal(t);
                    } else if (k.size() == 2 && k[0] == '.' && k[1] >= 'a' && k[1] <= 'z') {
                        continue; /* the lazy view has folded these codes to ordinary missing */
                    } else {
                        double code = 0;
                        if (!atod(k, &code) || !std::isfinite(code))
                            return "egen concat(): invalid numeric value-label code " + k;
                        cases += " WHEN " + compared + " = " + k + " THEN " + quote_literal(t);
                    }
                }
                const std::string fallback = "parqit_stata_string(__parqit_double(" + v + "))";
                piece = cases.empty() ? fallback : "(CASE" + cases + " ELSE " + fallback + " END)";
            } else if (!pf.empty())
                piece = "(CASE WHEN " + v + " IS NULL THEN '.' ELSE printf(" + quote_literal(pf) +
                        ", __parqit_double(" + v + ")) END)";
            else
                piece = "parqit_stata_string(__parqit_double(" + v + "))";
            joined += (i ? " || " + quote_literal(sp.punct) + " || " : "") + piece;
        }
        result = mask_else("(" + joined + ")", "''");
        rkind = 's';
        normalized = false;
    } else if (fcn == "cut") {
        const std::string x = quote_ident(vars[0]);
        std::vector<double> at = sp.at;
        bool icodes = sp.icodes || sp.label;
        if (!at.empty() && sp.group > 0) return "egen cut(): cannot specify both at() and group()";
        if (at.empty() && sp.group == 0) return "egen cut(): one of at() or group() must be specified";
        if (sp.group > 0) {
            if (sp.probe_rows.size() != 1 || sp.probe_rows[0].size() != static_cast<size_t>(sp.group + 1))
                return "egen cut(): the cutpoint probe returned nothing (no selected nonmissing values?)";
            const auto &row = sp.probe_rows[0];
            if (row[0].empty()) return "egen cut(): no nonmissing values to cut";
            /* Fable final audit, A2: parsed whole and in the C locale (strtod
             * took any text, "." included, as a number, and follows
             * LC_NUMERIC); "." is a probe over no nonmissing value, where
             * every result is missing whatever the cut, as before */
            std::string bad;
            auto cutpoint = [&](const std::string &t) {
                double v = 0;
                if (t != "." && !atod(t, &v) && bad.empty()) bad = t;
                return v;
            };
            at.clear();
            at.push_back(cutpoint(row[0]));
            for (long long j = 1; j < sp.group; j++) at.push_back(cutpoint(row[static_cast<size_t>(1 + j)]));
            at.push_back(cutpoint(row[1]) + 1);
            if (!bad.empty()) return "egen cut(): the cutpoint probe returned \"" + bad + "\", not a number";
            icodes = true;
        }
        for (size_t i = 1; i < at.size(); i++)
            if (!(at[i] >= at[i - 1])) return "egen cut(): at() must be in ascending order";
        if (at.size() < 2) return "egen cut(): at() needs at least two cutpoints";
        std::string cases = "CASE";
        for (size_t i = at.size() - 1; i-- > 0;) {
            cases += " WHEN " + x + " >= " + dtoa(at[i]) + " THEN " +
                     (icodes ? std::to_string(i) : dtoa(at[i]));
        }
        cases += " END";
        result = mask("CASE WHEN " + x + " IS NOT NULL AND " + x + " < " + dtoa(at.back()) + " THEN " +
                      cases + " END");
        if (sp.label) {
            std::string lab = sp.lname.empty() ? name : sp.lname;
            nlohmann::json entries = nlohmann::json::array();
            for (size_t i = 0; i + 1 < at.size(); i++)
                entries.push_back(nlohmann::json::array({std::to_string(i), dtoa(at[i]) + "-"}));
            if (!vallabs_.is_object()) vallabs_ = nlohmann::json::object();
            vallabs_[lab] = {{"entries", entries}};
            cast = lab;
        }
    } else if (fcn == "diff") {
        std::string cmp;
        for (size_t i = 1; i < vars.size(); i++) {
            const ViewCol &a = cols_[static_cast<size_t>(col_index(vars[0]))];
            const ViewCol &b = cols_[static_cast<size_t>(col_index(vars[i]))];
            if (a.kind != b.kind) return "egen diff(): " + vars[0] + " and " + vars[i] +
                                        " are not both numeric or both string";
            const std::string l = a.kind == 's' ? "coalesce(" + quote_ident(vars[0]) + ", '')"
                                                : quote_ident(vars[0]);
            const std::string r = b.kind == 's' ? "coalesce(" + quote_ident(vars[i]) + ", '')"
                                                : quote_ident(vars[i]);
            cmp += (cmp.empty() ? "" : " OR ") + l + " IS DISTINCT FROM " + r;
        }
        if (cmp.empty()) cmp = "FALSE";
        result = mask("(CASE WHEN " + cmp + " THEN 1 ELSE 0 END)");
        result = "CAST(" + result + " AS TINYINT)";
    } else if (fcn == "ends") {
        if ((sp.head ? 1 : 0) + (sp.last ? 1 : 0) + (sp.tail ? 1 : 0) > 1)
            return "egen ends(): head, last and tail are mutually exclusive";
        const std::string str = "coalesce(" + quote_ident(vars[0]) + ", '')";
        const std::string pch = quote_literal(sp.has_punct ? sp.punct : " ");
        std::string v;
        if (sp.last) v = "list_extract(string_split(" + str + ", " + pch + "), -1)";
        else if (sp.tail)
            v = "CASE WHEN strpos(" + str + ", " + pch + ") > 0 THEN substr(" + str + ", strpos(" + str +
                ", " + pch + ") + length(" + pch + ")) ELSE '' END";
        else v = "list_extract(string_split(" + str + ", " + pch + "), 1)";
        if (sp.trim) v = "trim(" + v + ")";
        result = mask_else("(" + v + ")", "''");
        rkind = 's';
        normalized = false;
    } else { /* the row functions */
        std::string list, nonmiss, total, firsts, lasts;
        for (size_t i = 0; i < vars.size(); i++) {
            const ViewCol &col = cols_[static_cast<size_t>(col_index(vars[i]))];
            const std::string v = quote_ident(vars[i]);
            const std::string present = col.kind == 's' ? "coalesce(" + v + ", '') <> ''" : v + " IS NOT NULL";
            nonmiss += (i ? " + " : "") + std::string("(CASE WHEN ") + present + " THEN 1 ELSE 0 END)";
            if (col.kind == 'n') {
                const std::string d = "__parqit_double(" + v + ")";
                list += (list.empty() ? "" : ", ") + d;
                total += (total.empty() ? "" : " + ") + ("coalesce(" + d + ", 0)");
                firsts += (firsts.empty() ? "" : ", ") + d;
            }
        }
        for (size_t i = vars.size(); i-- > 0;) {
            if (cols_[static_cast<size_t>(col_index(vars[i]))].kind != 'n') continue;
            lasts += (lasts.empty() ? "" : ", ") + ("__parqit_double(" + quote_ident(vars[i]) + ")");
        }
        const std::string l = "list_filter([" + list + "], lambda x : x IS NOT NULL)";
        const std::string n = "(" + nonmiss + ")";
        std::string v;
        if (fcn == "rowtotal")
            v = "CASE WHEN " + n + " = 0 THEN " + (sp.missing ? "NULL" : "0") + " ELSE parqit_finite(" +
                total + ") END";
        else if (fcn == "rowmean")
            v = "CASE WHEN " + n + " = 0 THEN NULL ELSE parqit_finite((" + total + ") / " + n + ") END";
        else if (fcn == "rowmin") v = "least(" + list + ")";
        else if (fcn == "rowmax") v = "greatest(" + list + ")";
        else if (fcn == "rowmiss") v = std::to_string(vars.size()) + " - " + n;
        else if (fcn == "rownonmiss") v = n;
        else if (fcn == "rowfirst") v = "coalesce(" + firsts + ")";
        else if (fcn == "rowlast") v = "coalesce(" + lasts + ")";
        else if (fcn == "rowsd")
            v = "CASE WHEN " + n + " < 2 THEN NULL ELSE list_aggregate(" + l + ", 'stddev_samp') END";
        else { /* rowmedian, rowpctile: the percentile rule over the sorted values */
            double p = 50;
            if (fcn == "rowpctile") {
                p = sp.has_p ? sp.p : 50;
                if (!(p > 0 && p < 100)) return "egen rowpctile(): p() must be between 0 and 100";
            }
            const std::string ls = "list_sort(" + l + ")";
            const std::string np = "(" + n + " * " + dtoa(p) + " / 100.0)";
            v = "CASE WHEN " + n + " = 0 THEN NULL WHEN " + np + " = floor(" + np + ") THEN __parqit_midpoint(list_extract(" +
                ls + ", CAST(" + np + " AS INTEGER)), list_extract(" + ls + ", CAST(least(" + np + " + 1, " + n +
                ") AS INTEGER))) ELSE list_extract(" + ls + ", CAST(ceil(" + np + ") AS INTEGER)) END";
        }
        result = mask("(" + v + ")");
    }

    /* EGEN-STORAGE-1: an explicit storage type is value semantics */
    std::string stored = result;
    if (!type_req.empty()) {
        const bool finite_double = rkind == 'n';
        stored = (type_req == "double" && finite_double) ? "parqit_finite(__parqit_double(" + result + "))"
                                                         : coerce_storage(result, type_req, rkind);
    }
    /* WINDOW-NEST-1: these functions stack a window over a window's result;
     * every window stage counts, since DuckDB 1.5.3's window_self_join
     * rewrite also makes a stack of single-level windows plan exponentially
     * (12 stacked mean(): 2.5 s on 74 rows, 0.016 s without it) */
    if (fcn == "median" || fcn == "pctile" || fcn == "iqr" || fcn == "mad" || fcn == "mdev" ||
        fcn == "std" || fcn == "mode")
        fragile_windows_ = true;
    if (expr_fn || fcn == "seq" || fcn == "mode" || fcn == "group" || fcn == "tag") window_stages_++;
    std::string desc = "egen " + name + " = " + fcn + "(...)";
    if (!sp.by.empty()) desc += ", by(...)";
    if (!sp.if_expr.empty()) desc += " if " + sp.if_expr;
    /* EGEN-FUSE-1: independent aggregates on the same partition can share
     * one window operator. Keep the exact aggregate/storage expressions;
     * only their SELECT level changes. A bare input column, no condition and
     * no positional window make dependency checking explicit. Everything
     * else keeps the general sequential plan, including a reference to an
     * earlier output of this batch. Never inline or duplicate volatile work. */
    static const std::set<std::string> batch_fcns = {
        "mean", "total", "sum", "sd", "min", "max", "count"};
    std::string batch_source = sp.expr;
    bool batchable = batch_fcns.count(fcn) && !has_if && arg.windows.empty() &&
                     lvl == prev_name(stages_.size());
    if (batchable)
        batchable = col_index(batch_source) >= 0 ||
                    resolve_name(sp.expr, &batch_source).empty();
    const std::string definition = stored + " AS " + quote_ident(name);
    if (batchable) {
        const bool extends = egen_batch_.end == stages_.size() && egen_batch_.end > 0 &&
                             egen_batch_.by == byn && !egen_batch_.outputs.count(batch_source) &&
                             std::none_of(byn.begin(), byn.end(), [&](const std::string &b) {
                                 return egen_batch_.outputs.count(b) != 0;
                             });
        if (!extends) {
            egen_batch_ = EgenBatch{};
            egen_batch_.first = stages_.size() + 1;
            egen_batch_.by = byn;
            egen_batch_.prior_window_terms = window_terms_;
        }
        const std::string columns = stage_list();
        push_stage("SELECT " + columns + ", " + definition + " FROM " + lvl, desc, true);
        auto &stage = egen_stages_.back();
        stage.first = egen_batch_.first;
        stage.definition = definition;
        if (!extends) {
            stage.columns = columns;
            stage.from = lvl;
            egen_batch_.window_terms = count_window_terms("SELECT " + columns + " FROM " + lvl);
        }
        egen_batch_.window_terms += count_window_terms(definition);
        if (extends) {
            superseded_at_[egen_batch_.end - 1] = stages_.size();
            /* Preserve the existing optimiser policy for multiple logical
             * windows. Fusion removes repeated window work without newly
             * enabling a self-join that could duplicate a costly child. */
        }
        window_terms_ = egen_batch_.prior_window_terms + egen_batch_.window_terms;
        egen_batch_.end = stages_.size();
        egen_batch_.outputs.insert(name);
    } else {
        push_stage("SELECT " + stage_list() + ", " + definition + " FROM " + lvl, desc, true);
    }
    ViewCol nc;
    nc.name = name;
    nc.kind = rkind;
    nc.meta_type = type_req;
    nc.normalized = rkind == 'n' && normalized;
    if (fcn == "group" || fcn == "cut") {
        if (!cast.empty()) nc.vallab = cast;
        if (fcn == "group") nc.varlab = "group(" + [&] {
            std::string t;
            for (size_t i = 0; i < vars.size(); i++) t += (i ? " " : "") + vars[i];
            return t;
        }() + ")";
    } else if (fcn == "tag") {
        nc.varlab = "tag(" + [&] {
            std::string t;
            for (size_t i = 0; i < vars.size(); i++) t += (i ? " " : "") + vars[i];
            return t;
        }() + ")";
        nc.meta_type = type_req.empty() ? "byte" : type_req;
    } else if (fcn == "std") {
        nc.varlab = "Standardized values of " + sp.expr;
    } else if (fcn == "rank") {
        nc.varlab = std::string(sp.field ? "field " : sp.track ? "track " : sp.unique ? "unique " : "") +
                    "rank of (" + sp.expr + ")" + (byn.empty() ? "" : " by " + [&] {
            std::string t;
            for (size_t i = 0; i < byn.size(); i++) t += (i ? " " : "") + byn[i];
            return t;
        }());
    } else if (fcn == "anycount" || fcn == "anymatch" || fcn == "anyvalue") {
        std::string t, vl;
        for (size_t i = 0; i < vars.size(); i++) t += (i ? " " : "") + vars[i];
        for (size_t i = 0; i < sp.values.size(); i++) vl += (i ? " " : "") + dtoa(sp.values[i]);
        nc.varlab = t + (fcn == "anyvalue" ? " if " : " == ") + vl;
        if (fcn != "anyvalue") nc.meta_type = type_req.empty() ? "byte" : type_req;
    } else if (fcn == "diff") {
        std::string t;
        for (size_t i = 0; i < vars.size(); i++) t += (i ? " " : "") + vars[i];
        nc.varlab = "diff " + t;
        nc.meta_type = type_req.empty() ? "byte" : type_req;
    }
    cols_.push_back(nc);
    return "";
}

} // namespace parqit

namespace parqit {

/* ----------------------------------------------------- two-table verbs --- */

static std::string numeric_type(const ViewCol &c) {
    if (!c.physical_type.empty()) return c.physical_type;
    if (c.meta_type == "float") return "FLOAT";
    if (c.meta_type == "double") return "DOUBLE";
    if (c.meta_type == "byte") return "TINYINT";
    if (c.meta_type == "int") return "SMALLINT";
    if (c.meta_type == "long") return "INTEGER";
    return "";
}

static int decimal_scale(const std::string &type) {
    if (type.rfind("DECIMAL(", 0) != 0) return -1;
    return std::stoi(type.substr(type.find(',') + 1));
}

static bool wide_numeric(const std::string &type) {
    return type == "BIGINT" || type == "UBIGINT" || type == "HUGEINT" ||
           type == "UHUGEINT" || decimal_scale(type) >= 0;
}

static std::string common_numeric_type(const std::vector<const ViewCol *> &columns) {
    bool floating = false, real = false, needs_double = false, uhuge = false;
    int scale = -1;
    std::set<std::string> types;
    for (const auto *c : columns) {
        const std::string type = numeric_type(*c);
        types.insert(type);
        floating |= type == "FLOAT";
        real |= type == "DOUBLE";
        uhuge |= type == "UHUGEINT";
        needs_double |= type != "FLOAT" && type != "TINYINT" && type != "SMALLINT";
        scale = std::max(scale, decimal_scale(type));
    }
    if (real || (floating && needs_double)) return "DOUBLE";
    if (floating) return "FLOAT";
    if (types.size() <= 1) return "";
    if (scale >= 0) return "DECIMAL(38," + std::to_string(scale) + ")";
    if (uhuge) return "HUGEINT";
    return ""; // DuckDB's common signed/unsigned <=64-bit integer type is exact.
}

static std::string numeric_output(const std::string &ref, const ViewCol &c,
                                  const std::string &target, const std::string &operation) {
    if (target.empty() || numeric_type(c) == target) return ref;
    const std::string converted = target == "DOUBLE" ? "__parqit_double(" + ref + ")"
        : "TRY_CAST(" + ref + " AS " + target + ")";
    if (target == "FLOAT" || (target == "DOUBLE" && !wide_numeric(numeric_type(c))))
        return converted;
    const std::string exact = target == "DOUBLE" ? "__parqit_compare(" + ref + ", " + converted + ") = 0"
        : converted + " IS NOT NULL";
    return "(CASE WHEN " + ref + " IS NULL THEN NULL WHEN " + exact + " THEN " + converted +
           " ELSE error(" + quote_literal(operation + ": " + c.exposed() +
           " cannot be combined without losing precision; cast explicitly first") + ") END)";
}

std::string key_missing_fold_sql(const std::string &ref, char kind) {
    if (kind == 's') return "nullif(" + ref + ", '')";
    const std::string number = "__parqit_double(" + ref + ")";
    return "(CASE WHEN isfinite(" + number + ") AND abs(" + number + ") < " +
           dtoa(kStataMissThreshold) + " THEN " + ref + " ELSE NULL END)";
}

static std::string key_value(const std::string &ref, const ViewCol &c) {
    /* KEYFOLD-1: one rule, shared with the contracts in plugin_view.cpp. A
     * normalized column (MISS-1) already holds no special or out-of-range
     * value, so the guard is skipped there; the rule is unchanged. */
    if (c.kind != 's' && c.normalized) return ref;
    return key_missing_fold_sql(ref, c.kind);
}

/* SAMPLE-DESIGN-1: the SQL pieces shared by sample_design and its probe. The
 * frame is coalesce(if, FALSE): a row whose condition is missing is outside
 * the frame and kept, as in sample2. Strata and the cluster are folded keys
 * (KEYFOLD-1), so '' and NULL, or NaN and NULL, are one missing value. */
struct SampleDesign {
    std::string frame = "TRUE";
    std::vector<std::string> strata;
    std::string cluster;
    char cluster_kind = 'n';
};

static std::string sample_design_parts(const View &v, const std::string &if_expr,
                                       const std::vector<std::string> &by,
                                       const std::string &cluster, bool statamissing,
                                       SampleDesign *out) {
    const auto &cols = v.cols();
    auto column = [&](const std::string &name) -> const ViewCol & {
        for (const auto &c : cols)
            if (c.name == name) return c;
        return cols.front(); /* unreachable: expand_patterns returns live names */
    };
    if (!if_expr.empty()) {
        ExprResult c = translate_filter(if_expr, v.expr_schema(), statamissing);
        if (!c.ok) return c.error;
        if (!c.windows.empty()) return "_n and _N are not supported in the if of sample";
        out->frame = "coalesce(" + c.sql + ", FALSE)";
    }
    if (!by.empty()) {
        std::vector<std::string> names;
        std::string err = v.expand_patterns(by, &names);
        if (!err.empty()) return err;
        for (const auto &n : names) out->strata.push_back(key_value(quote_ident(n), column(n)));
    }
    if (!cluster.empty()) {
        std::vector<std::string> names;
        std::string err = v.expand_patterns({cluster}, &names);
        if (!err.empty()) return err;
        if (names.size() != 1) return "cluster() takes one variable";
        out->cluster = key_value(quote_ident(names[0]), column(names[0]));
        out->cluster_kind = column(names[0]).kind;
    }
    return "";
}

std::string View::sample_design_probe(const std::string &if_expr,
                                      const std::vector<std::string> &by,
                                      const std::string &cluster, bool statamissing,
                                      std::string *sql) {
    SampleDesign d;
    std::string err = sample_design_parts(*this, if_expr, by, cluster, statamissing, &d);
    if (!err.empty()) return err;
    if (d.cluster.empty()) return "the sample design probe needs cluster()";
    std::string tuple = "0";
    if (!d.strata.empty()) {
        tuple = "row(";
        for (size_t i = 0; i < d.strata.size(); i++) tuple += (i ? ", " : "") + d.strata[i];
        tuple += ")";
    }
    const std::string p = quote_ident(fresh_helper("sample_probe")),
                      c = quote_ident(fresh_helper("sample_cluster")),
                      f = quote_ident(fresh_helper("sample_frame")),
                      s = quote_ident(fresh_helper("sample_stratum"));
    *sql = "WITH " + p + " AS (SELECT " + d.cluster + " AS " + c + ", " + d.frame + " AS " + f +
           ", " + tuple + " AS " + s + " FROM (" + compile(false) + ")) SELECT "
           "CAST((SELECT count(*) FROM (SELECT " + c + " FROM " + p + " WHERE " + c +
           " IS NOT NULL GROUP BY " + c + " HAVING count(DISTINCT " + s + ") > 1)) AS VARCHAR)"
           " || '|' || CAST((SELECT count(*) FROM (SELECT " + c + " FROM " + p + " WHERE " + c +
           " IS NOT NULL GROUP BY " + c + " HAVING bool_or(" + f + ") <> bool_and(" + f +
           "))) AS VARCHAR) || '|' || CAST((SELECT count(*) FROM " + p + " WHERE " + c +
           " IS NULL AND " + f + ") AS VARCHAR)";
    return "";
}

std::string View::sample_design(double amount, bool is_count, long long seed,
                                const std::string &if_expr,
                                const std::vector<std::string> &by,
                                const std::string &cluster, const std::string &frame_rule,
                                const std::string &generate, bool statamissing) {
    if (!std::isfinite(amount)) return "sample amount must be finite";
    if (is_count) {
        if (amount < 0 || amount >= 0x1p63 || amount != std::floor(amount))
            return "sample, count needs a nonnegative integer below 2^63";
    } else if (amount <= 0 || amount > 100) {
        return "sample percentage out of range";
    }
    if (frame_rule != "strict" && frame_rule != "any" && frame_rule != "all")
        return "unknown sampling frame rule " + frame_rule;
    std::set<std::string> taken; /* no helper may take the indicator's name */
    if (!generate.empty()) {
        if (generate == "_n" || generate == "_N")
            return generate + " is a reserved word: expressions read it as the row number";
        if (col_index(generate) >= 0) return "variable " + generate + " already defined";
        const std::string cig = ci_guard(generate);
        if (!cig.empty()) return cig;
        taken.insert(generate);
    }
    SampleDesign d;
    std::string err = sample_design_parts(*this, if_expr, by, cluster, statamissing, &d);
    if (!err.empty()) return err;
    if (seed < 0) {
        static std::mt19937 seeds(std::random_device{}());
        seed = seeds() & 0x7fffffffU;
    }
    /* units to draw in a stratum of n: sample()'s rule (#134) or the count */
    auto quota = [&](const std::string &n) {
        return is_count ? "least(" + n + ", " + std::to_string(static_cast<long long>(amount)) + ")"
                        : "__parqit_sample_count(CAST(" + n + " AS UBIGINT), CAST(" +
                              quote_literal(dtoa(amount)) + " AS DOUBLE))";
    };
    const std::string prev = prev_name(stages_.size());
    std::vector<std::string> s;
    std::string partition;
    for (size_t i = 0; i < d.strata.size(); i++) {
        s.push_back(quote_ident(fresh_helper("sample_stratum", taken)));
        partition += (i ? ", " : "PARTITION BY ") + s[i];
    }
    const std::string lead = partition.empty() ? "" : partition + " ";
    std::string body, kept, from;
    if (!d.cluster.empty()) {
        /* whole clusters: one row per cluster, ranked by a key that depends
         * only on the seed and the cluster's value, joined back to the rows */
        const std::string units = quote_ident(fresh_helper("sample_units", taken)),
                          ranked = quote_ident(fresh_helper("sample_ranked", taken)),
                          flags = quote_ident(fresh_helper("sample_flags", taken)),
                          c = quote_ident(fresh_helper("sample_cluster", taken)),
                          a = quote_ident(fresh_helper("sample_any", taken)),
                          l = quote_ident(fresh_helper("sample_all", taken)),
                          r = quote_ident(fresh_helper("sample_rank", taken)),
                          n = quote_ident(fresh_helper("sample_n", taken)),
                          drawn = quote_ident(fresh_helper("sample_drawn", taken));
        std::string strata;
        for (size_t i = 0; i < s.size(); i++)
            strata += ", any_value(" + d.strata[i] + ") AS " + s[i];
        const std::string key = d.cluster_kind == 's'
            ? "__parqit_sample_key_s(CAST(" + std::to_string(seed) + " AS UBIGINT), CAST(" + c + " AS VARCHAR))"
            : "__parqit_sample_key_d(CAST(" + std::to_string(seed) + " AS UBIGINT), __parqit_double(" + c + "))";
        body = "WITH " + units + " AS (SELECT " + d.cluster + " AS " + c + strata +
               ", bool_or(" + d.frame + ") AS " + a + ", bool_and(" + d.frame + ") AS " + l +
               " FROM " + prev + " WHERE " + d.cluster + " IS NOT NULL GROUP BY 1), " +
               ranked + " AS (SELECT " + c + ", row_number() OVER (" + lead + "ORDER BY " + key +
               ", " + c + ") AS " + r + ", count(*) OVER (" + partition + ") AS " + n + " FROM " +
               units + " WHERE " + (frame_rule == "any" ? a : l) + "), " +
               flags + " AS (SELECT " + c + ", (" + r + " <= " + quota(n) + ") AS " + drawn +
               " FROM " + ranked + ") ";
        kept = "(" + flags + "." + c + " IS NULL OR " + flags + "." + drawn + ")";
        from = " FROM " + prev + " LEFT JOIN " + flags + " ON " + d.cluster + " = " + flags + "." + c;
    } else {
        /* rows: the priority of sample()'s percentage form over the same row
         * numbers, so generate() alone flags exactly the rows it keeps */
        const std::string source = quote_ident(fresh_helper("sample_source", taken)),
                          ranked = quote_ident(fresh_helper("sample_ranked", taken)),
                          picked = quote_ident(fresh_helper("sample_picked", taken)),
                          f = quote_ident(fresh_helper("sample_frame", taken)),
                          row = quote_ident(fresh_helper("sample_row", taken)),
                          q = quote_ident(fresh_helper("sample_rank", taken)),
                          n = quote_ident(fresh_helper("sample_n", taken));
        std::string strata;
        for (size_t i = 0; i < s.size(); i++) strata += ", " + d.strata[i] + " AS " + s[i];
        const std::string order = order_by_sql();
        body = "WITH " + source + " AS MATERIALIZED (SELECT " + select_list() + ", " + d.frame +
               " AS " + f + strata + ", row_number() OVER (" +
               (order.empty() ? "" : order.substr(1)) + ") AS " + row + " FROM " + prev + "), " +
               ranked + " AS (SELECT " + row + ", row_number() OVER (" + lead +
               "ORDER BY hash(hash(" + row + ", " + std::to_string(seed) + ")), " + row +
               ") AS " + q + ", count(*) OVER (" + partition + ") AS " + n + " FROM " + source +
               " WHERE " + f + "), " + picked + " AS (SELECT " + row + " FROM " + ranked +
               " WHERE " + q + " <= " + quota(n) + ") ";
        kept = "(NOT " + f + " OR " + row + " IN (SELECT " + row + " FROM " + picked + "))";
        from = " FROM " + source;
    }
    std::string desc = "sample " + dtoa(amount) + (is_count ? ", count" : "") +
                       " (design" + (if_expr.empty() ? "" : ", if") +
                       (by.empty() ? "" : ", by") + (cluster.empty() ? "" : ", cluster " + frame_rule) +
                       (generate.empty() ? "" : ", generate " + generate) +
                       ", seed " + std::to_string(seed) + ")";
    if (generate.empty()) {
        body += "SELECT " + select_list() + from + " WHERE " + kept;
    } else {
        body += "SELECT " + select_list() + ", " +
                coerce_storage("CASE WHEN " + kept + " THEN 1 ELSE 0 END", "byte", 'n') + " AS " +
                quote_ident(generate) + from;
        ViewCol nc;
        nc.name = generate;
        nc.kind = 'n';
        nc.meta_type = "byte";
        nc.normalized = true;
        cols_.push_back(nc);
    }
    push_stage(body, desc);
    return "";
}

static std::string key_equality(const std::string &left, const ViewCol &lc,
                                const std::string &right, const ViewCol &rc) {
    const std::string a = key_value(left, lc), b = key_value(right, rc);
    const std::string lt = numeric_type(lc), rt = numeric_type(rc);
    if (lc.kind == 's' || lt == rt)
        return a + " IS NOT DISTINCT FROM " + b;
    const bool lf = lt == "FLOAT" || lt == "DOUBLE", rf = rt == "FLOAT" || rt == "DOUBLE";
    std::string x = a, y = b, guards;
    auto checked = [&](const std::string &value, const std::string &converted) {
        guards += " AND (" + value + " IS NULL OR " + converted + " IS NOT NULL)";
        return converted;
    };
    if (lf || rf) {
        x = lf ? "__parqit_double(" + a + ")" : checked(a, "__parqit_exact_double_key(" + a + ")");
        y = rf ? "__parqit_double(" + b + ")" : checked(b, "__parqit_exact_double_key(" + b + ")");
    } else if (decimal_scale(lt) >= 0 || decimal_scale(rt) >= 0) {
        const std::string type = "DECIMAL(38," + std::to_string(std::max(decimal_scale(lt), decimal_scale(rt))) + ")";
        x = checked(a, "TRY_CAST(" + a + " AS " + type + ")");
        y = checked(b, "TRY_CAST(" + b + " AS " + type + ")");
    } else if (lt == "UHUGEINT" || rt == "UHUGEINT") {
        x = checked(a, "TRY_CAST(" + a + " AS UHUGEINT)");
        y = checked(b, "TRY_CAST(" + b + " AS UHUGEINT)");
    }
    // Equality remains between unary expressions, so DuckDB can use a hash join.
    return "(" + x + " IS NOT DISTINCT FROM " + y + guards + ")";
}

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

/* TWOTABLE-CHARS-1: a characteristic named note# is a note; note0 holds the
 * highest number, which `notes drop` does not lower (it leaves gaps) */
static bool note_number(const std::string &name, long long *n) {
    if (name.size() < 5 || name.compare(0, 4, "note") != 0) return false;
    if (name.size() > 5 && name[4] == '0') return false;
    long long v = 0;
    for (size_t i = 4; i < name.size(); i++) {
        const char ch = name[i];
        if (ch < '0' || ch > '9' || v > 99999999LL) return false;
        v = v * 10 + (ch - '0');
    }
    *n = v;
    return true;
}

/* TWOTABLE-CHARS-1 (2026-10-01): what native merge, append and joinby take
 * from the using data for each owner they keep from it — _dta, the keys, the
 * common and the new variables — as {result name, using name} pairs. Each
 * characteristic the master lacks comes across (the master's wins on a name
 * clash); unless nonotes, so do the notes, after the master's in their order
 * and numbered on from its note0, skipping a text the owner already has
 * (compared exactly; notes taken from an earlier using file count), while a
 * note repeated within the using data comes across each time. Read off
 * StataNow 19.5 (ASSUMPTIONS #178). */
static void merge_using_chars(
    nlohmann::json *chars, const nlohmann::json &src,
    const std::vector<std::pair<std::string, std::string>> &owners, bool notes) {
    if (!src.is_object()) return;
    for (const auto &o : owners) {
        const auto s = src.find(o.second);
        if (s == src.end() || !s->is_object() || s->empty()) continue;
        if (!chars->is_object()) *chars = nlohmann::json::object();
        nlohmann::json &dst = (*chars)[o.first];
        if (!dst.is_object()) dst = nlohmann::json::object();
        std::set<std::string> have;
        long long last = 0;
        for (const auto &kv : dst.items()) {
            long long n = 0;
            if (!note_number(kv.key(), &n) || !kv.value().is_string()) continue;
            const std::string text = kv.value().get<std::string>();
            if (n == 0)
                last = std::max(last, std::strtoll(text.c_str(), nullptr, 10));
            else {
                last = std::max(last, n);
                have.insert(text);
            }
        }
        std::map<long long, std::string> incoming; /* in note order */
        for (const auto &kv : s->items()) {
            long long n = 0;
            if (!note_number(kv.key(), &n)) {
                if (!dst.contains(kv.key())) dst[kv.key()] = kv.value();
            } else if (notes && n > 0 && kv.value().is_string()) {
                incoming[n] = kv.value().get<std::string>();
            }
        }
        bool added = false;
        for (const auto &in : incoming) {
            if (have.count(in.second)) continue;
            dst["note" + std::to_string(++last)] = in.second;
            added = true;
        }
        if (added) dst["note0"] = std::to_string(last);
        if (dst.empty()) chars->erase(o.first);
    }
}

/* JOINKEY-1: the one place that decides whether a join key list is usable.
 * merge_with/joinby_with call it, and so does the plugin BEFORE it runs the
 * uniqueness contracts — those queries reference the keys, so an unknown key
 * used to come back as "Binder Error: Referenced column ... not found" with
 * the generated SQL attached, instead of parqit naming the key and the side. */
std::string View::join_keys_error(const std::string &op,
                                  const std::vector<std::string> &keys,
                                  const UsingSide &u,
                                  bool *type_mismatch) const {
    if (type_mismatch) *type_mismatch = false;
    std::map<std::string, const ViewCol *> ucols;
    for (const auto &c : u.cols) ucols[c.name] = &c;
    for (const auto &k : keys) {
        int mi = col_index(k);
        if (mi < 0) return op + ": key " + k + " not found in the master view";
        auto it = ucols.find(k);
        if (it == ucols.end())
            return op + ": key " + k + " not found in the using data";
        if (cols_[mi].exposed() != it->second->exposed())
            return op + ": key " + k + " identifies different columns in master (" +
                   cols_[mi].exposed() + ") and using (" + it->second->exposed() +
                   "); rename the keys consistently before combining";
        if (cols_[mi].kind != it->second->kind) {
            if (type_mismatch) *type_mismatch = true;
            return op + ": key " + k + " is " +
                   (cols_[mi].kind == 's' ? "string" : "numeric") +
                   " in master but " +
                   (it->second->kind == 's' ? "string" : "numeric") + " in using";
        }
    }
    return "";
}

std::string View::merge_with(const std::string &kind,
                             const std::vector<std::string> &keys, UsingSide u,
                             const std::vector<std::string> &keepusing,
                             int keep_mask, const std::string &gen_name,
                             bool nogen, std::vector<std::string> *warnings) {
    if (keys.empty()) return "merge: key varlist required";
    fragile_windows_ = fragile_windows_ || u.fragile_windows; /* WINDOW-NEST-1 */
    if (kind != "1:1" && kind != "m:1" && kind != "1:m" && kind != "m:m")
        return "merge: kind must be 1:1, m:1, 1:m or m:m";

    /* keys must exist on both sides with matching kinds */
    std::string kerr = join_keys_error("merge", keys, u);
    if (!kerr.empty()) return kerr;
    std::set<std::string> keyset(keys.begin(), keys.end());
    std::map<std::string, const ViewCol *> ucols;
    for (const auto &c : u.cols) ucols[c.name] = &c;

    /* which using columns come across: keepusing ∩ (not in master unless key) */
    std::set<std::string> wanted;
    if (!keepusing.empty()) {
        std::vector<std::string> names, aliases;
        for (const auto &c : u.cols) { names.push_back(c.name); aliases.push_back(c.stata); }
        std::vector<size_t> indices;
        const std::string err = resolve_varlist(keepusing, names, aliases, varabbrev_, &indices);
        if (!err.empty()) return "merge: keepusing " + err + " in the using data";
        for (size_t i : indices) wanted.insert(names[i]);
    }
    std::vector<const ViewCol *> brought;
    /* MERGE-COMMON-1 (audit 2026-08-22, A3-1): a non-key column present on
     * BOTH sides and kept from using keeps the master value on matched and
     * master-only rows, but on a using-only row (_merge==2) native Stata
     * carries the USING value — parqit left it missing. Track such columns so
     * the projection below can coalesce them by the master marker. */
    std::map<std::string, const ViewCol *> common; /* engine name -> using col */
    for (const auto &c : u.cols) {
        if (keyset.count(c.name)) continue;
        if (!keepusing.empty() && !wanted.count(c.name)) continue;
        const std::string identity = alias_identity_error(cols_, c);
        if (!identity.empty()) return "merge: " + identity;
        int mi = col_index(c.name);
        if (mi >= 0) {
            /* native merge stops with r(106) when a common variable is string
             * on one side and numeric on the other; silently keeping the
             * master would leave the using-only rows unfillable */
            if (cols_[mi].kind != c.kind)
                return "merge: variable " + c.name + " is " +
                       (cols_[mi].kind == 's' ? "string" : "numeric") +
                       " in master but " + (c.kind == 's' ? "string" : "numeric") +
                       " in using";
            warnings->push_back("variable " + c.name +
                                " exists in master and using; master values kept");
            common[c.name] = &c;
            continue;
        }
        {
            const std::string cig = ci_guard(c.name);
            if (!cig.empty()) return "merge: using " + cig;
        }
        brought.push_back(&c);
    }

    std::map<std::string, std::string> output_types;
    for (const auto &c : cols_)
        if (c.kind == 'n' && (keyset.count(c.name) || common.count(c.name)))
            output_types[c.name] = common_numeric_type({&c, ucols.at(c.name)});
    auto output_value = [&](const std::string &side, const ViewCol &c) {
        const auto target = output_types.find(c.name);
        const std::string ref = side + "." + quote_ident(c.name);
        return target == output_types.end() ? ref : numeric_output(ref, c, target->second, "merge");
    };
    auto spine_column = [&](const std::string &key) {
        ViewCol c = cols_[col_index(key)];
        if (!output_types[key].empty()) c.physical_type = output_types[key];
        c.normalized = true;
        return c;
    };

    /* _merge name: never collide silently (charter §6.12) */
    std::string mname = gen_name.empty() ? "_merge" : gen_name;
    if (!nogen && col_index(mname) >= 0)
        return "merge: variable " + mname +
               " already exists (use gen() or nogenerate)";
    if (!nogen) {
        const std::string cig = ci_guard(mname);
        if (!cig.empty()) return "merge: " + cig;
        for (const auto *c : brought) {
            if (c->name == mname || c->exposed() == mname)
                return "merge: variable " + mname +
                       " already exists in using data (use gen() or nogenerate)";
            if (ci_clash(c->name, mname))
                return "merge: variable " + mname + " differs only by case from the "
                       "using variable " + c->name +
                       ", which the engine cannot hold in one view (use gen())";
        }
    }

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
        auto normalized_source = [&](const std::string &source, bool master) {
            std::string replace;
            for (const auto &key : keys) {
                const ViewCol &c = master ? cols_[col_index(key)] : *ucols.at(key);
                const std::string ref = quote_ident(key);
                const std::string value = numeric_output(key_value(ref, c), c, output_types[key], "merge m:m");
                replace += (replace.empty() ? "" : ", ") + value + " AS " + ref;
            }
            return "(SELECT * REPLACE (" + replace + ") FROM " + source + ")";
        };
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
        mrel = "(SELECT *, TRUE AS " + quote_ident(mm) + ", row_number() OVER (PARTITION BY " +
               keypart + " " + m_order + ") AS " + quote_ident(rnm) +
               ", count(*) OVER (PARTITION BY " + keypart + ") AS " + quote_ident(nmx) +
               " FROM " + normalized_source(prev, true) + ")";
        /* Deterministic using-side pairing: order the row_number window by all
         * using columns so m:m pairing is reproducible (not engine-defined).
         * A lazy file/view does not carry physical within-key row identity, so
         * this preserves Stata's clamped sequential rule but cannot promise the
         * same payload pairing as a particular native input order (MM-ORDER-1). */
        std::string u_order;
        for (size_t i = 0; i < u.cols.size(); i++)
            u_order += (i ? ", " : " ORDER BY ") + quote_ident(u.cols[i].name);
        urel = "(SELECT *, TRUE AS " + quote_ident(um) + ", row_number() OVER (PARTITION BY " +
               keypart + u_order + ") AS " + quote_ident(rnu) + ", count(*) OVER (PARTITION BY " +
               keypart + ") AS " + quote_ident(nux) + " FROM " + normalized_source("(" + u.select_sql + ")", false) + ")";
    } else {
        mrel = "(SELECT *, TRUE AS " + quote_ident(mm) + " FROM " + prev + ")";
        urel = "(SELECT *, TRUE AS " + quote_ident(um) + " FROM (" + u.select_sql + "))";
    }
    for (size_t i = 0; i < keys.size(); i++) {
        if (i) joincond += " AND ";
        /* Stata semantics: missing keys match missing keys (incl. ""≡NULL≡NaN) */
        joincond += key_equality("__m." + quote_ident(keys[i]), cols_[col_index(keys[i])],
                                 "__u." + quote_ident(keys[i]), *ucols.at(keys[i]));
    }

    /* output projection: master cols (coalesced keys), brought using cols,
     * then the merge marker */
    /* one projection builder for both branches: master columns (coalesced
     * keys; common columns filled from using on using-only rows), brought
     * using columns. `keysrc` is the side the keys come from ("__m" with a
     * coalesce against __u in the join branch, "__s" = the spine in m:m). */
    auto project_master = [&](const char *keysrc) -> std::string {
        std::string out;
        for (const auto &c : cols_) {
            if (!out.empty()) out += ", ";
            const std::string q = quote_ident(c.name);
            if (keyset.count(c.name)) {
                out += std::string(keysrc) == "__m"
                           ? "coalesce(" + output_value("__m", c) + ", " +
                             output_value("__u", *ucols.at(c.name)) + ") AS " + q
                           : std::string(keysrc) + "." + q + " AS " + q;
            } else if (common.count(c.name)) {
                /* MERGE-COMMON-1: using-only row -> using value */
                out += "(CASE WHEN __m." + quote_ident(mm) + " IS NULL THEN " +
                       output_value("__u", *common.at(c.name)) + " ELSE " +
                       output_value("__m", c) + " END) AS " + q;
            } else {
                out += "__m." + q + " AS " + q;
            }
        }
        return out;
    };
    std::vector<ViewCol> ncols;
    for (const auto &c : cols_) {
        ncols.push_back(c);
        if (!output_types[c.name].empty()) {
            ncols.back().physical_type = output_types[c.name];
            ncols.back().meta_type = output_types[c.name] == "DOUBLE" ? "double" :
                                    output_types[c.name] == "FLOAT" ? "float" : "";
        }
        /* MISS-1: a common column may now carry a using-side value; it stays
         * normalized only when the using column is normalized too */
        auto it = common.find(c.name);
        if (it != common.end() && !it->second->normalized) ncols.back().normalized = false;
    }
    std::string sel = project_master("__m");
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
            const auto c = spine_column(keys[i]);
            jm += key_equality("__s." + quote_ident(keys[i]), c, "__m." + quote_ident(keys[i]), c);
            ju += key_equality("__s." + quote_ident(keys[i]), c, "__u." + quote_ident(keys[i]), c);
        }
        jm += " AND __m." + quote_ident(rnm) + " = least(__s." + quote_ident(spine_i) +
              ", __m." + quote_ident(nmx) + ")";
        ju += " AND __u." + quote_ident(rnu) + " = least(__s." + quote_ident(spine_i) +
              ", __u." + quote_ident(nux) + ")";
        /* rebuild sel against __m/__u as before but keys come from spine */
        std::string sel2 = project_master("__s");
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
        mc.fmt = "%23.0g"; /* native's _merge format (A3-8): labels display */
        mc.vallab = "_merge"; /* TT-3 */
        ncols.push_back(mc);
    }
    body = "SELECT " + osel + " FROM (" + body + ")";

    merge_using_meta(this, &vallabs_, u.vallabs, warnings);
    {
        std::vector<std::pair<std::string, std::string>> owners{{"_dta", "_dta"}};
        for (const auto &k : keys)
            owners.push_back({cols_[col_index(k)].exposed(), ucols.at(k)->exposed()});
        for (const auto &cm : common)
            owners.push_back({cols_[col_index(cm.first)].exposed(), cm.second->exposed()});
        for (const ViewCol *c : brought) owners.push_back({c->exposed(), c->exposed()});
        merge_using_chars(&chars_, u.chars, owners, u.notes);
    }
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
    for (const auto &u : sources) {
        fragile_windows_ = fragile_windows_ || u.fragile_windows;
    }
    if (!gen_name.empty() && col_index(gen_name) >= 0)
        return "append: generate() variable " + gen_name + " already exists";
    if (!gen_name.empty()) {
        const std::string cig = ci_guard(gen_name);
        if (!cig.empty()) return "append: " + cig;
    }
    /* TT-A2: a generate() name colliding with a using-file column would emit a
     * duplicate output name into UNION ALL BY NAME and surface a cryptic DuckDB
     * Binder Error; check the using sides too and fail loudly and clearly. */
    if (!gen_name.empty())
        for (size_t s = 0; s < sources.size(); s++)
            for (const auto &c : sources[s].cols) {
                if (c.name == gen_name || c.exposed() == gen_name)
                    return "append: generate() variable " + gen_name +
                           " already exists in using file " + std::to_string(s + 1);
                /* A2-6 (audit 2026-08-22): a case-only clash with a using column
                 * made UNION BY NAME bind the marker onto that column and
                 * collect die with a raw Binder Error — refuse before mutation,
                 * like merge's gen() does. */
                if (ci_clash(c.name, gen_name))
                    return "append: generate() variable " + gen_name +
                           " differs only by case from variable " + c.name +
                           " of using file " + std::to_string(s + 1) +
                           ", which the engine cannot hold in one view";
            }

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
            const std::string identity = alias_identity_error(ncols, c);
            if (!identity.empty()) return "append: " + identity;
            for (const auto &existing : ncols)
                if (ci_clash(existing.name, c.name))
                    return "append: variable " + c.exposed() + " differs only by case from " +
                           existing.exposed() + "; rename the columns consistently in the "
                           "source views before appending";
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
    std::map<std::string, std::string> casts;
    for (auto &out : ncols) {
        if (out.kind != 'n') continue;
        std::vector<const ViewCol *> inputs;
        auto inspect = [&](const std::vector<ViewCol> &columns) {
            for (const auto &c : columns) {
                if (c.name != out.name) continue;
                inputs.push_back(&c);
            }
        };
        inspect(cols_);
        for (const auto &side : sources) inspect(side.cols);
        const std::string type = common_numeric_type(inputs);
        if (!type.empty()) {
            casts[out.name] = type;
            out.meta_type = type == "DOUBLE" ? "double" : type == "FLOAT" ? "float" : "";
            out.physical_type = type;
        }
        /* Every non-normalized numeric input is guarded in its projection. */
        out.normalized = true;
    }
    auto projection = [&](const std::vector<ViewCol> &columns) {
        std::string select;
        for (const auto &c : columns) {
            const std::string ref = quote_ident(c.name);
            std::string value = c.kind == 'n' ? key_value(ref, c) : ref;
            auto cast = casts.find(c.name);
            if (cast != casts.end()) value = numeric_output(value, c, cast->second, "append");
            if (!select.empty()) select += ", ";
            select += value + " AS " + ref;
        }
        return select;
    };
    /* validate-then-mutate (charter §6): merge the sources' value-label
     * definitions only after EVERY source passed the kind checks above — a
     * conflict in source 2 must not leave source 1's labels already merged
     * into a view the failed verb then abandons. */
    for (size_t s = 0; s < sources.size(); s++) {
        merge_using_meta(this, &vallabs_, sources[s].vallabs, warnings);
        /* TWOTABLE-CHARS-1: file by file, as native append (keep() already
         * narrowed each source's columns) */
        std::vector<std::pair<std::string, std::string>> owners{{"_dta", "_dta"}};
        for (const auto &c : sources[s].cols)
            owners.push_back({ncols[find_in(c.name)].exposed(), c.exposed()});
        merge_using_chars(&chars_, sources[s].chars, owners, sources[s].notes);
    }

    std::string prev = prev_name(stages_.size());
    if (!sort_.empty())
        prev = "(SELECT * FROM " + prev + order_by_sql() + ")";
    std::string body = "SELECT " + projection(cols_) +
                       (gen_name.empty() ? std::string()
                                         : ", 0 AS " + quote_ident(gen_name)) +
                       " FROM " + prev;
    for (size_t s = 0; s < sources.size(); s++) {
        body += " UNION ALL BY NAME SELECT " + projection(sources[s].cols);
        if (!gen_name.empty())
            body += ", " + std::to_string(s + 1) + " AS " + quote_ident(gen_name);
        body += " FROM (" + sources[s].select_sql + ")";
    }
    if (!gen_name.empty()) {
        ViewCol gc;
        gc.name = gen_name;
        gc.kind = 'n';
        /* APPEND-GEN-LABEL-1: as native append — "Dataset source", labelled
         * _append (0 "Master", k "Appended dataset k"), or a fresh __append#
         * when that label name is taken, with a display format wide enough
         * for the longest label */
        gc.varlab = "Dataset source";
        gc.meta_type = "byte";
        std::string lab = "_append";
        for (int k = 1; vallabs_.is_object() && vallabs_.contains(lab); k++)
            lab = "__append" + std::to_string(k);
        nlohmann::json entries = nlohmann::json::array();
        entries.push_back(nlohmann::json::array({"0", "Master"}));
        for (size_t s = 0; s < sources.size(); s++)
            entries.push_back(nlohmann::json::array(
                {std::to_string(s + 1), "Appended dataset " + std::to_string(s + 1)}));
        if (!vallabs_.is_object()) vallabs_ = nlohmann::json::object();
        vallabs_[lab] = {{"entries", entries}};
        gc.vallab = lab;
        gc.fmt = "%" + std::to_string(17 + std::to_string(sources.size()).size()) + ".0g";
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
    /* Native append concatenates the already-sorted master with each using
     * dataset and clears sortedby; it never re-sorts the union by the master's
     * stale key. */
    sort_.clear();
    return "";
}

std::string View::joinby_with(const std::vector<std::string> &keys, UsingSide u,
                              std::vector<std::string> *warnings) {
    if (keys.empty()) return "joinby: key varlist required";
    fragile_windows_ = fragile_windows_ || u.fragile_windows; /* WINDOW-NEST-1 */
    std::string kerr = join_keys_error("joinby", keys, u);
    if (!kerr.empty()) return kerr;
    std::set<std::string> keyset(keys.begin(), keys.end());

    std::vector<ViewCol> ncols = cols_;
    std::vector<const ViewCol *> brought, common;
    for (const auto &c : u.cols) {
        if (keyset.count(c.name)) continue;
        const std::string identity = alias_identity_error(cols_, c);
        if (!identity.empty()) return "joinby: " + identity;
        if (col_index(c.name) >= 0) {
            warnings->push_back("variable " + c.name +
                                " exists in master and using; master values kept");
            common.push_back(&c);
            continue;
        }
        {
            const std::string cig = ci_guard(c.name);
            if (!cig.empty()) return "joinby: using " + cig;
        }
        brought.push_back(&c);
        ncols.push_back(c);
    }

    const std::string prev = prev_name(stages_.size());
    std::map<std::string, const ViewCol *> ucols;
    for (const auto &c : u.cols) ucols[c.name] = &c;
    std::string joincond;
    for (size_t i = 0; i < keys.size(); i++) {
        if (i) joincond += " AND ";
        joincond += key_equality("__m." + quote_ident(keys[i]), cols_[col_index(keys[i])],
                                 "__u." + quote_ident(keys[i]), *ucols.at(keys[i]));
    }
    std::string sel;
    for (const auto &c : cols_) {
        if (!sel.empty()) sel += ", ";
        sel += "__m." + quote_ident(c.name) + " AS " + quote_ident(c.name);
    }
    for (const ViewCol *c : brought)
        sel += ", __u." + quote_ident(c->name) + " AS " + quote_ident(c->name);

    merge_using_meta(this, &vallabs_, u.vallabs, warnings);
    {
        /* TWOTABLE-CHARS-1: native joinby takes the using data's
         * characteristics as merge does (it has no nonotes) */
        std::vector<std::pair<std::string, std::string>> owners{{"_dta", "_dta"}};
        for (const auto &k : keys)
            owners.push_back({cols_[col_index(k)].exposed(), ucols.at(k)->exposed()});
        for (const ViewCol *c : common)
            owners.push_back({cols_[col_index(c->name)].exposed(), c->exposed()});
        for (const ViewCol *c : brought) owners.push_back({c->exposed(), c->exposed()});
        merge_using_chars(&chars_, u.chars, owners, u.notes);
    }
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
    {
        const std::string cig = ci_guard(jname);
        if (!cig.empty()) return "reshape long: " + cig;
    }
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

    std::map<std::string, std::string> stubtypes;
    for (const auto &st : stubs) {
        if (stubkind[st] != 'n') continue;
        std::vector<const ViewCol *> inputs;
        for (const auto &kv : by_stub_suffix[st]) inputs.push_back(kv.second);
        stubtypes[st] = common_numeric_type(inputs);
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
        std::set<std::string> exposed;
        std::map<std::string, std::string> engine_names;
        for (const auto *c : carried) {
            engine_names.emplace(ascii_lower(c->name), c->name);
            exposed.insert(c->exposed());
        }
        std::vector<std::string> generated{jname};
        generated.insert(generated.end(), stubs.begin(), stubs.end());
        for (const auto &nm : generated) {
            if (!exposed.insert(nm).second)
                return "reshape long: variable " + nm +
                       " already defined (a kept column collides with a long "
                       "stub or with j)";
            const auto inserted = engine_names.emplace(ascii_lower(nm), nm);
            if (!inserted.second)
                return "reshape long: generated name " + nm +
                       " collides with " + inserted.first->second +
                       " under the engine's case-insensitive identifiers";
        }
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
                if (stubkind[st] == 'n')
                    val = numeric_output(val, *it->second, stubtypes[st], "reshape long");
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
        if (!stubtypes[st].empty()) {
            sc.physical_type = stubtypes[st];
            sc.meta_type = stubtypes[st] == "DOUBLE" ? "double" : stubtypes[st] == "FLOAT" ? "float" : "";
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
    /* A2-3 (audit 2026-08-22): the generated name derives from the stub's
     * EXPOSED (Stata) name — an aliased stub `X_1` (Stata `X`) spreads into
     * `X1`, `X2`, not `X_11` — and the new columns never carry the alias
     * (derive_col), so two generated columns can never share one exposed name
     * and a view save can never write a duplicate-name file. */
    auto stub_base = [&](const std::string &st) -> std::string {
        const int si = col_index(st);
        return si < 0 ? st : cols_[si].exposed();
    };
    /* the generated names must also be case-insensitively unique AMONG
     * THEMSELVES (stubs `x` and `X` would spread into x1/X1, which DuckDB
     * binds to one column): refuse loudly */
    {
        std::vector<std::string> gen;
        for (const auto &st : stubs)
            for (const auto &jv : jvalues) gen.push_back(stub_base(st) + jv);
        for (size_t a = 0; a < gen.size(); a++)
            for (size_t b = a + 1; b < gen.size(); b++)
                if (gen[a] == gen[b] || ci_clash(gen[a], gen[b]))
                    return "reshape wide: generated names " + gen[a] + " and " +
                           gen[b] + " differ only by case (or coincide), which the "
                           "engine cannot hold in one view";
    }
    for (const auto &st : stubs)
        for (const auto &jv : jvalues) {
            std::string nn = stub_base(st) + jv;
            if (!valid_stata_name(nn))
                return "reshape wide: generated name " + nn +
                       " is not a valid Stata variable name (from j value " + jv +
                       ")";
            if (known.count(nn) || col_index(nn) >= 0)
                return "reshape wide: generated name " + nn + " collides";
            /* an i() variable survives the pivot: its exposed name must not be
             * taken by a generated column either (alias-aware) */
            for (const auto &iv : ivars) {
                const int ii = col_index(iv);
                if (ii >= 0 && cols_[ii].exposed() == nn)
                    return "reshape wide: generated name " + nn + " collides";
            }
            /* NAME-CASE-1: j values differing only by case (A/a) would generate
             * names DuckDB cannot hold apart in one relation */
            for (const auto &kn : known)
                if (ci_clash(kn, nn))
                    return "reshape wide: generated names " + nn + " and " + kn +
                           " differ only by case, which the engine cannot hold in one "
                           "view";
            {
                const std::string cig = ci_guard(nn);
                if (!cig.empty()) return "reshape wide: " + cig;
            }
        }

    const std::string prev = prev_name(stages_.size());
    std::string sel;
    std::vector<ViewCol> ncols;
    for (const auto &iv : ivars) {
        if (!sel.empty()) sel += ", ";
        sel += norm_group_key(quote_ident(iv), cols_[col_index(iv)].kind) +
               " AS " + quote_ident(iv);
        ncols.push_back(cols_[col_index(iv)]);
    }
    for (const auto &jv : jvalues) {
        for (const auto &st : stubs) {
            const ViewCol &sc = cols_[col_index(st)];
            std::string cond = quote_ident(jvar) +
                               (j_is_string ? " = " + quote_literal(jv)
                                            : " = " + jv);
            const std::string nn = stub_base(st) + jv; /* A2-3: from the exposed name */
            sel += ", max(" + quote_ident(st) + ") FILTER (WHERE " + cond +
                   ") AS " + quote_ident(nn);
            /* RESHAPE-WIDE-LABEL (A3-8): native labels the spread column
             * "<j value> <stub name>" (verified: stub `inc` labelled "income"
             * spreads into inc1 labelled "1 inc"); the display format is kept */
            ViewCol nc = derive_col(sc, nn);
            nc.varlab = jv + " " + sc.exposed();
            ncols.push_back(nc);
        }
    }
    std::string gb;
    for (size_t i = 0; i < ivars.size(); i++) {
        if (i) gb += ", ";
        gb += norm_group_key(quote_ident(ivars[i]),
                             cols_[col_index(ivars[i])].kind);
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
