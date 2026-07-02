/* parqit — the lazy view: a source scan plus a chain of verb stages compiled
 * to one DuckDB query (CTE pipeline), in dbplyr's architecture with Stata's
 * vocabulary.
 *
 * Inside the view every column is a Stata-semantics value: numerics are
 * numbers (dates = day counts, datetimes = ms counts — the parquet boundary
 * converts), strings are strings. The view tracks, per column: name, kind
 * (n/s), display format, variable label and value-label name, plus the
 * value-label definitions and characteristics carried from the source's
 * parqit.* metadata, so materialisers can restore everything.
 *
 * Sort order is plan *state*, applied once at materialisation (SQL
 * subquery ORDER BY is not otherwise preserved); _n/_N compile to window
 * functions over the current order. Internal helper columns use generated
 * names checked against the live schema (charter §6.12).
 */
#pragma once

#include <set>
#include <string>
#include <vector>

#include "json.hpp"

namespace parqit {

struct ViewCol {
    std::string name;      /* engine + Stata name inside the pipeline */
    char kind = 'n';       /* 'n' numeric, 's' string */
    std::string fmt;       /* Stata display format ("" = default) */
    std::string varlab;
    std::string vallab;    /* value-label name attached */
    std::string meta_type; /* original Stata storage type ("" unknown) */
    std::string note;      /* per-column loud note carried to collect */
    /* MISS-1: true once the column is guaranteed free of IEEE specials and
     * (for strings) of SQL NULL — i.e. it came through the lazy boundary's
     * float/string normalization and has not since been recomputed by a
     * gen/replace/aggregate. Lets missing() and lazy save skip a redundant
     * per-row finite/coalesce guard on already-clean columns. Carried verbs
     * (keep/drop/order/rename) preserve it; recomputing verbs leave it false. */
    bool normalized = false;
};

/* Stata's percentile rule (summarize/_pctile) as a SQL aggregate expression
 * over `ref`: np = n*p/100 on the sorted nonmissing values; integer np →
 * mean of x[np], x[np+1]; else x[ceil(np)]. Shared by collapse and
 * summarize-detail. */
std::string stata_pctile_sql(const std::string &ref, double p);

struct PendingRange { /* keep in f/l — validated against real counts at
                         materialisation (charter §6.13) */
    size_t stage;     /* index of the stage the LIMIT was applied to */
    long long f = 1, l = 1;
};

class View {
  public:
    bool live() const { return live_; }
    void close();

    /* opens over a prepared source SELECT (boundary casts already applied) */
    void open(const std::string &scan_select, std::vector<ViewCol> cols,
              nlohmann::json vallabs, nlohmann::json chars, std::string dtalabel,
              const std::string &source_desc);

    const std::vector<ViewCol> &cols() const { return cols_; }
    const nlohmann::json &vallabs() const { return vallabs_; }
    const nlohmann::json &chars() const { return chars_; }
    const std::string &dtalabel() const { return dtalabel_; }
    const std::vector<std::string> &sort_keys() const { return sort_; }
    const std::vector<PendingRange> &pending_ranges() const { return ranges_; }
    const std::string &source_desc() const { return source_desc_; }
    size_t n_stages() const { return stages_.size(); }

    /* Footer paths of the backing Parquet file(s), as a DuckDB list literal
     * (e.g. ['a.parquet']), set only when the view is opened directly over
     * Parquet files. Empty for SQL/bridge sources. Lets a pure full-file
     * passthrough collect size columns from row-group statistics (the same
     * F2 metadata path `parqit use` uses) instead of re-scanning. Reset by
     * close() (which re-default-constructs). */
    void set_source_paths(const std::string &paths_sql) {
        src_paths_sql_ = paths_sql;
    }
    const std::string &source_paths_sql() const { return src_paths_sql_; }

    /* expand Stata varlist wildcards (*, ?) against the live schema, in
     * pattern order, deduplicated; "" or an error for a no-match pattern.
     * Public so a multi-stage caller (pivot) can expand once and hand the
     * same literal list to every stage. */
    std::string expand_patterns(const std::vector<std::string> &patterns,
                                std::vector<std::string> *out) const;

    /* ---- verbs; each returns "" or an error message ------------------- */
    std::string keep_vars(const std::vector<std::string> &patterns);
    std::string drop_vars(const std::vector<std::string> &patterns);
    std::string filter(const std::string &stata_expr, bool drop, bool statamissing);
    std::string gen(const std::string &name, const std::string &type_req,
                    const std::string &expr, const std::string &if_expr,
                    bool statamissing);
    std::string replace(const std::string &name, const std::string &expr,
                        const std::string &if_expr, bool statamissing);
    std::string rename(const std::string &oldn, const std::string &newn);
    std::string reorder(const std::vector<std::string> &front);
    std::string sort(const std::vector<std::string> &keys,
                     const std::vector<bool> &desc);
    struct CollapseSpec {
        std::string stat; /* mean sum sd median pNN count min max first last
                             firstnm lastnm */
        std::string target;
        std::string source;
    };
    std::string collapse(const std::vector<CollapseSpec> &specs,
                         const std::vector<std::string> &by);
    std::string contract(const std::vector<std::string> &by, const std::string &freq);
    std::string duplicates_drop(const std::vector<std::string> &by, bool force);
    std::string keep_in(long long f, long long l);
    std::string sample(double amount, bool is_count, long long seed /* <0 none */);
    std::string egen(const std::string &name, const std::string &fcn,
                     const std::string &arg_expr, const std::vector<std::string> &by,
                     bool statamissing, const std::string &type_req = "");

    /* ---- two-table verbs (M3); the using side is a boundary-cast SELECT
     * over files that stay on disk ----------------------------------- */
    struct UsingSide {
        std::string select_sql;       /* boundary-cast scan */
        std::vector<ViewCol> cols;
        nlohmann::json vallabs;       /* definitions carried from its parqit.* */
    };
    /* kind: "1:1", "m:1", "1:m", "m:m" (uniqueness already validated by the
     * caller — the engine layer cannot run queries). keep_mask: bit 1 = keep
     * _merge==1 rows, bit 2 = ==2, bit 4 = ==3; 0 means keep all. */
    std::string merge_with(const std::string &kind,
                           const std::vector<std::string> &keys, UsingSide u,
                           const std::vector<std::string> &keepusing,
                           int keep_mask, const std::string &gen_name, bool nogen,
                           std::vector<std::string> *warnings);
    std::string append_with(std::vector<UsingSide> sources,
                            const std::string &gen_name,
                            std::vector<std::string> *warnings);
    std::string joinby_with(const std::vector<std::string> &keys, UsingSide u,
                            std::vector<std::string> *warnings);

    /* ---- reshape (M4) ------------------------------------------------- */
    /* long: stubs' suffixed columns melt into rows; j becomes a variable
     * (numeric when every suffix is an integer, else string). */
    std::string reshape_long(const std::vector<std::string> &stubs,
                             const std::vector<std::string> &ivars,
                             const std::string &jname);
    /* wide: j's distinct values (supplied by the caller, who scanned them —
     * the engine layer cannot query) become column suffixes. The caller has
     * already validated (i, j) uniqueness. */
    std::string reshape_wide(const std::vector<std::string> &stubs,
                             const std::vector<std::string> &ivars,
                             const std::string &jvar,
                             const std::vector<std::string> &jvalues,
                             bool j_is_string);

    /* ---- raw SQL stages (M4) ------------------------------------------ */
    /* append `SELECT * FROM prev <fragment>` (QUALIFY/WHERE/USING SAMPLE…);
     * the caller validates compilability. */
    std::string raw_fragment(const std::string &fragment);

    /* ---- compilation -------------------------------------------------- */
    /* full pipeline; with_order: append the final ORDER BY */
    std::string compile(bool with_order = true) const;
    /* SQL of the pipeline up to and including stage k (for validation) */
    std::string compile_prefix(size_t stages) const;
    /* dbplyr-style pretty form for parqit show */
    std::string show() const;

  private:
    int col_index(const std::string &name) const;
    /* `taken`: extra names the helper must dodge beyond the live manifest —
     * two-table verbs pass the using side's column names here, so a using
     * column literally named like a generated helper can never make the
     * compiled join reference ambiguous (charter §6.12). */
    std::string fresh_helper(const std::string &hint,
                             const std::set<std::string> &taken = {});
    /* FROM-source for a keep/drop projection stage: when the projection is
     * about to remove a sort-key column, bakes the full current ORDER BY into
     * the source subquery (the physical order survives via DuckDB's default
     * preserve_insertion_order) and truncates sort_ to the longest prefix of
     * keys whose columns all survive — mirroring native Stata, where dropping
     * a sortedby variable keeps the rows' physical order and truncates (or
     * clears) sortedby. Otherwise just the previous stage name. */
    std::string projection_source(const std::vector<ViewCol> &survivors);
    std::string select_list() const; /* quoted current columns, in order */
    std::string order_by_sql() const;
    /* wrap `prev` in a subquery exposing ONLY the row-context windows *sql
     * references: row_number() for _n (a streaming window) and count(*) OVER ()
     * for _N (a blocking window). Placeholders in *sql are substituted in place.
     * Emitting the blocking count only when _N is actually used keeps the common
     * _n-only idiom streaming instead of buffering the whole input (PERF-1). */
    std::string rowctx_wrap(std::string *sql, const std::string &prev);
    void push_stage(const std::string &select_body, const std::string &desc);

    bool live_ = false;
    std::string scan_;
    std::string source_desc_;
    std::string src_paths_sql_; /* Parquet footer paths, "" unless file-backed */
    std::vector<std::string> stages_; /* each a full SELECT … FROM <prev> */
    std::vector<std::string> descs_;
    std::vector<ViewCol> cols_;
    std::vector<std::string> sort_;   /* quoted "name" or "name DESC" */
    std::vector<PendingRange> ranges_;
    nlohmann::json vallabs_;
    nlohmann::json chars_;
    std::string dtalabel_;
    long long helper_counter_ = 0;
};

} // namespace parqit
