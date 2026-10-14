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

#include "engine/exprtrans.hpp"

namespace parqit {

struct ViewCol {
    std::string name;      /* engine identifier = the name lazy verbs use */
    /* NAME-CASE-1: the Stata-facing name when it differs from `name` — only
     * for a column whose file name differs merely by case from another
     * column's (DuckDB identifiers are case-insensitive, so the engine holds it
     * under a suffixed alias); "" otherwise. collect/use and save expose this
     * exact name; the compiled SQL never sees it. */
    std::string stata;
    const std::string &exposed() const { return stata.empty() ? name : stata; }
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
    std::string physical_type; /* current numeric SQL type, independent of metadata */
    bool is_float() const {
        return physical_type == "FLOAT" ||
               (physical_type.empty() && meta_type == "float");
    }
};

/* Stata's percentile rule (summarize/_pctile) as a SQL aggregate expression
 * over `ref`: np = n*p/100 on the sorted nonmissing values; integer np →
 * mean of x[np], x[np+1]; else x[ceil(np)]. Shared by collapse and
 * summarize-detail. */
/* Stata's percentile rule over per-group ranks (PCT-WINDOW-1): `prn` ranks
 * the nonmissing values within the group, `pn` counts them. `over` is "" for
 * an aggregate (collapse) and an OVER clause for a window (egen). */
std::string pct_window_sql(const std::string &ref, const std::string &prn,
                           const std::string &pn, double p, const std::string &over = "");

/* KEYFOLD-1: the ONE rule that decides when a merge/joinby key value is
 * "missing" — used by the join itself (key_value in view.cpp) and by every
 * check that must agree with it (the uniqueness contracts behind r(459) and
 * the missing-key disclosure in plugin_view.cpp). A string key is missing when
 * empty; a numeric key when NULL, NaN, ±Inf or |x| >= 2^1023 (a value Stata
 * would read as a missing code). Two implementations of this rule once
 * diverged (the contract folded only NaN), so a using file holding inf or
 * 1e308 in a key passed the uniqueness check while the join matched those rows
 * as one missing key — a 1:1 / m:1 merge could duplicate rows with rc 0.
 * `ref` is an already-quoted SQL reference; kind is 'n' or 's'. */
std::string key_missing_fold_sql(const std::string &ref, char kind);

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
    /* order_cols: ORDER-CARRIER-1 — quoted names of hidden columns of the scan
     * that give each row's physical position in the source (file, row within
     * file); empty when the source cannot provide them */
    void open(const std::string &scan_select, std::vector<ViewCol> cols,
              nlohmann::json vallabs, nlohmann::json chars, std::string dtalabel,
              const std::string &source_desc,
              std::vector<std::string> order_cols = {});

    const std::vector<ViewCol> &cols() const { return cols_; }
    void set_numeric_types(const std::vector<std::string> &types);
    std::string coerce_numeric_column(const std::string &name,
                                      const std::string &type);
    const nlohmann::json &vallabs() const { return vallabs_; }
    const nlohmann::json &chars() const { return chars_; }
    const std::string &dtalabel() const { return dtalabel_; }
    const std::vector<std::string> &sort_keys() const { return sort_; }
    std::vector<std::string> sortedby_names() const;
    const std::vector<PendingRange> &pending_ranges() const { return ranges_; }
    const std::string &source_desc() const { return source_desc_; }
    /* VIEW-COPY-1: a copied view names its origin; display only (views, show) */
    void set_source_desc(const std::string &desc) { source_desc_ = desc; }
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

    /* INT64-PROTECT-1: the int64() the view was OPENED with ("refuse",
     * "round", "string"), carried so `parqit use using f, int64(string)` is
     * honoured by a later `parqit collect` that names no option. "" (the
     * default, restored by close()) means the session setting decides.
     * Stored as the canonical spelling so the engine layer stays free of the
     * read-time type policy. */
    void set_int64_mode(const std::string &mode) { int64_mode_ = mode; }
    const std::string &int64_mode() const { return int64_mode_; }
    /* FILENAME-1: the view column that carries each row's source path
     * (`parqit use …, filename(newvar)`), so a direct-read collect plans the
     * scan knowing it is NOT one of the files' own columns (it must stay out
     * of the leaf alignment and out of the Hive partition keys). "" otherwise;
     * reset by close(), like the source paths. */
    void set_source_filename_column(const std::string &name) {
        src_filename_col_ = name;
    }
    const std::string &source_filename_column() const { return src_filename_col_; }
    /* VIEW-COPY-1: the file columns whose .a-.z codes the lazy view reads as
     * plain . (XMISS-1), recorded at open so a copy can repeat the note */
    void set_xmissing_folded(const std::vector<std::string> &names) { xm_folded_ = names; }
    const std::vector<std::string> &xmissing_folded() const { return xm_folded_; }

    /* expand Stata varlist wildcards (*, ?) against the live schema, in
     * pattern order, deduplicated; "" or an error for a no-match pattern.
     * Public so a multi-stage caller (pivot) can expand once and hand the
     * same literal list to every stage. */
    std::string expand_patterns(const std::vector<std::string> &patterns,
                                std::vector<std::string> *out) const;

    /* WINDOW-NEST-1 (2026-10-13): true once the plan stacks a window over the
     * result of another window of the same partition (egen median/pctile/iqr/
     * mad/mdev/std/mode). DuckDB 1.5.3's window_self_join optimizer rewrites
     * such whole-partition window aggregates into aggregate + self-join and
     * then fails to bind the inner window's column (INTERNAL Error "Failed to
     * bind column reference", or "_Map_base::at"); the plugin disables that
     * one optimizer while a plan with this flag is current (disabling it for
     * every query costs 3-6x on the ordinary single-level windows, measured on
     * 20M rows). A copy, a merge/append/joinby with such a using view and a
     * raw fragment keep the flag. */
    bool fragile_windows() const { return fragile_windows_ || window_stages_ >= 2; }
    /* WINDOW-NEST-1: DuckDB 1.5.3's filter pushdown rewrites the subtree
     * under every partitioned WINDOW operator twice (PushdownWindow rewrites
     * the child, then FinishPushdown rewrites it again), so planning grows
     * ~1.8x per stacked window: 16 stacked means plan in 0.05 s, 20 in 0.6 s,
     * 30 in more than a minute, and the 52 egen stages of v149 never finished
     * (0.08 s with the optimizer off). The view counts the partitioned window
     * terms its stages add — an upper bound of the operators, since terms in
     * one SELECT level plan as one — and from the 18th the plugin turns that
     * optimizer off while the view is current, at the cost of the row-group
     * pruning of the view's own filters. A merge/append/joinby adds the
     * using view's count. */
    bool heavy_filter_pushdown() const { return window_terms_ >= kHeavyWindowTerms; }
    int window_terms() const { return window_terms_; }
    static constexpr int kHeavyWindowTerms = 18;
    /* the partitioned window terms of a SQL text: "OVER (PARTITION BY" in any
     * case and spacing (raw fragments write it as they like); counted for the
     * scan of open() and every stage, so a using view's plan embedded by a
     * two-table verb is counted once, as text */
    static int count_window_terms(const std::string &sql);
    /* TSSET-1: the time-series declaration of the view (parqit tsset/xtset);
     * engine column names, delta > 0. tsset() validates the names and kinds
     * and stores the declaration; the plugin runs the uniqueness/missing probe
     * (tsset_probe_sql) before calling it. A declaration whose variables a
     * later verb removes is cleared; reusing the name cannot revive it. */
    std::string tsset(const std::string &panel, const std::string &time, double delta);
    void clear_tsset() { ts_panel_.clear(); ts_time_.clear(); ts_delta_ = 1.0; }
    bool ts_declared() const;
    std::string check_by_order(const std::vector<std::string> &by,
                               const std::vector<std::string> &sortvars) const;
    const std::string &ts_panel() const { return ts_panel_; }
    const std::string &ts_time() const { return ts_time_; }
    double ts_delta() const { return ts_delta_; }
    /* "<missing time>|<duplicate (panel,time) rows>|<min time>|<max time>|<panels>|<gaps>"
     * over the current plan; panels and gaps are 0 without a panel variable */
    std::string tsset_probe_sql(const std::string &panel, const std::string &time,
                                double delta) const;
    /* the expression translator's view of the live columns, with the
     * time-series declaration when its variables are live */
    ExprSchema expr_schema() const;

    /* ---- verbs; each returns "" or an error message ------------------- */
    /* BY-PREFIX-1: `by` are the keys of a by/bysort prefix (patterns, expanded
     * exactly), `sortvars` the (sortvars) part. The view must already be
     * sorted by by+sortvars — bysort sorts first (the plugin does it), by
     * alone is refused with "not sorted", as native r(5). Under a prefix _n,
     * _N and the subscripts x[...] are windows within each by group. */
    std::string keep_vars(const std::vector<std::string> &patterns);
    std::string drop_vars(const std::vector<std::string> &patterns);
    std::string filter(const std::string &stata_expr, bool drop, bool statamissing,
                       const std::vector<std::string> &by = {},
                       const std::vector<std::string> &sortvars = {});
    std::string gen(const std::string &name, const std::string &type_req,
                    const std::string &expr, const std::string &if_expr,
                    bool statamissing, const std::vector<std::string> &by = {},
                    const std::vector<std::string> &sortvars = {});
    std::string replace(const std::string &name, const std::string &expr,
                        const std::string &if_expr, bool statamissing,
                        const std::vector<std::string> &by = {},
                        const std::vector<std::string> &sortvars = {});
    std::string rename(const std::string &oldn, const std::string &newn);
    std::string rename_many(const std::vector<std::string> &old_names,
                            const std::vector<std::string> &new_names);
    std::string reorder(const std::vector<std::string> &front);
    std::string sort(const std::vector<std::string> &keys,
                     const std::vector<bool> &desc);
    struct CollapseSpec {
        std::string stat; /* mean sum rawsum sd semean sebinomial sepoisson
                             median pNN iqr count percent min max first last
                             firstnm lastnm (COLLAPSE-STATS-2: the full native
                             list; weights are refused upstream) */
        std::string target;
        std::string source;
    };
    std::string collapse(const std::vector<CollapseSpec> &specs,
                         const std::vector<std::string> &by);
    std::string contract(const std::vector<std::string> &by, const std::string &freq);
    /* IF-ALL-1: with if_expr, only the rows the condition selects are
     * deduplicated (the first of each group in the current order stays); every
     * other row is kept, as native `duplicates drop ... if` does. */
    std::string duplicates_drop(const std::vector<std::string> &by, bool force,
                                const std::string &if_expr = "",
                                bool statamissing = false);
    std::string keep_in(long long f, long long l);
    /* drop in f/l: the complement of keep in over the same (declared or
     * engine) order, validated against the real count the same way. */
    std::string drop_in(long long f, long long l);
    std::string sample(double amount, bool is_count, long long seed /* <0 none */);
    /* SAMPLE-DESIGN-1, sample2's philosophy: if_expr defines the sampling frame
     * (rows outside it are kept), by() stratifies, cluster() draws whole
     * clusters (a missing cluster is outside the frame), frame_rule (strict,
     * any, all) places the clusters that if splits, and generate() adds a 0/1
     * indicator instead of dropping rows. Counts per stratum use the rule of
     * sample(); a cluster's priority depends only on the seed and its value. */
    std::string sample_design(double amount, bool is_count, long long seed,
                              const std::string &if_expr,
                              const std::vector<std::string> &by,
                              const std::string &cluster, const std::string &frame_rule,
                              const std::string &generate, bool statamissing);
    /* the verb-time checks of a cluster design over the current plan: one text
     * value "<clusters with several strata>|<split clusters>|<in-frame rows
     * with a missing cluster>" */
    std::string sample_design_probe(const std::string &if_expr,
                                    const std::vector<std::string> &by,
                                    const std::string &cluster, bool statamissing,
                                    std::string *sql);
    /* EGEN-ALL-1 (BPLIM feedback, 2026-10-13): the native egen functions.
     * `expr` is the argument of an expression function (total mean sd min
     * max count median pctile iqr kurt skew mad mdev std pc rank), `vars`
     * the varlist of a varlist function (mode cut anyvalue ends take one;
     * group tag anycount anymatch concat diff row*), `by` the groups of a
     * by() option or by prefix (refused for the functions native refuses it
     * for), `if_expr` the qualifier — the statistic is computed over the
     * selected rows and only they receive a value (tag, anycount and
     * anymatch give 0 and the string functions "" to the others, as native).
     * The options are native egen's. `probe_rows` holds the rows of
     * egen_probe_sql() when a function needs data before it can be compiled
     * (cut's group() cutpoints, group's label texts); the plugin runs it. */
    struct EgenSpec {
        std::string name, fcn, type_req;
        std::string expr;
        std::vector<std::string> vars;
        std::vector<std::string> by;
        std::string if_expr;
        bool has_p = false;
        double p = 50;
        std::vector<double> values;
        std::string punct;      /* concat joins end to end unless punct() is given */
        bool has_punct = false; /* ends() separates by one space unless given */
        std::string format;
        bool decode = false;
        long long maxlength = 0;
        bool missing = false, minmode = false, maxmode = false;
        long long nummode = 0;
        bool field = false, track = false, unique = false;
        bool has_from = false, has_to = false;
        double from = 1, to = 0;
        long long block = 1;
        double mean = 0, sd = 1;
        std::vector<double> at;
        long long group = 0;
        bool icodes = false, label = false;
        bool prop = false, strok = false, head = false, last = false, tail = false, trim = false;
        std::string lname;
        long long truncate = 0;
        std::vector<std::vector<std::string>> probe_rows;
    };
    /* "" when the function needs no data before compiling; otherwise the
     * query whose rows the plugin must put in spec.probe_rows (text cells) */
    std::string egen_probe_sql(const EgenSpec &spec, bool statamissing, std::string *sql) const;
    std::string egen(const EgenSpec &spec, bool statamissing, const std::string &arg_type = "");
    /* the former entry point: an expression function with by() and no if */
    std::string egen(const std::string &name, const std::string &fcn,
                     const std::string &arg_expr, const std::vector<std::string> &by,
                     bool statamissing, const std::string &type_req = "",
                     const std::string &arg_type = "");

    /* ---- two-table verbs (M3); the using side is a boundary-cast SELECT
     * over files that stay on disk ----------------------------------- */
    struct UsingSide {
        std::string select_sql;       /* boundary-cast scan */
        std::vector<ViewCol> cols;
        nlohmann::json vallabs;       /* definitions carried from its parqit.* */
        /* TWOTABLE-CHARS-1: its characteristics (notes included), keyed by
         * exposed name and _dta; notes = false is nonotes, which still takes
         * its other characteristics, as in native Stata */
        nlohmann::json chars;
        bool notes = true;
        bool fragile_windows = false; /* WINDOW-NEST-1: the using view's flags */
        int window_terms = 0;
    };
    /* JOINKEY-1: every join key must exist on both sides with the same kind.
     * merge_with/joinby_with enforce this, but the caller runs the uniqueness
     * contracts BEFORE them, and a key absent from a side turned that query
     * into a raw DuckDB Binder Error quoting internal SQL. Callers that query
     * first must call this themselves; "" means the keys are usable. The
     * optional flag distinguishes native rc 106 type mismatches from rc 111
     * missing-variable errors without duplicating validation in the plugin. */
    std::string join_keys_error(const std::string &op,
                                const std::vector<std::string> &keys,
                                const UsingSide &u,
                                bool *type_mismatch = nullptr) const;
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
    /* A helper-column name that no live column (nor `taken`) collides with —
     * every internal column is generated this way, never a fixed magic name.
     * `taken`: extra names the helper must dodge beyond the live manifest —
     * two-table verbs pass the using side's column names here, so a using
     * column literally named like a generated helper can never make the
     * compiled join reference ambiguous (charter §6.12). The engine-side
     * statistics (tabstat's rank windows) use it too. */
    std::string fresh_helper(const std::string &hint,
                             const std::set<std::string> &taken = {});
    /* DUP-LIST-1: the window that numbers rows as _n does over compile() —
     * the declared sort, or the engine's row order when none is declared */
    std::string row_number_over() const;
    /* DUP-LIST-1: the whole pipeline with one more column, `obs`, numbering
     * the rows as _n does (the physical carrier stays in scope for it) */
    std::string compile_numbered(const std::string &obs) const;

  private:
    int col_index(const std::string &name) const;
    /* NAME-CASE-1: "" when `name` can be created in this view, else the loud
     * refusal: a live column (not in `ignore`) differs from it only by case,
     * and DuckDB would silently bind references to whichever it dedups first.
     * Every verb that introduces a user-chosen name calls this after its exact
     * "already defined" check. */
    std::string ci_guard(const std::string &name,
                         const std::set<std::string> &ignore = {}) const;
    /* FROM-source for a keep/drop projection stage: when the projection is
     * about to remove a sort-key column, bakes the full current ORDER BY into
     * the source subquery (the physical order survives in one hidden row
     * number while the order carrier is live, ORDER-CARRIER-1, and otherwise
     * via DuckDB's default preserve_insertion_order) and truncates sort_ to
     * the longest prefix of keys whose columns all survive — mirroring
     * native Stata, where dropping a sortedby variable keeps the rows'
     * physical order and truncates (or clears) sortedby. Otherwise just the
     * previous stage name. */
    std::string projection_source(const std::vector<ViewCol> &survivors);
    std::string bake_order(const std::string &prev); /* ORDER-CARRIER-1 */
    std::string ctes(size_t nstages) const; /* WITH clause of the scan and stages */
    std::string select_list() const; /* quoted current columns, in order */
    /* ORDER-CARRIER-1: select_list() plus the hidden order columns while the
     * current stage carries them — the list a row-preserving verb selects */
    std::string stage_list() const;
    std::string order_by_sql() const;
    /* ROWCTX-2: wrap `prev` in a subquery computing, as helper columns, every
     * window term the translated expressions need (row_number() for _n — a
     * streaming window — count(*) for _N, lag/lead/nth_value for subscripts,
     * the RANGE windows of L./F./D./S.), each over the by partition `part`
     * (quoted, normalized key SQL) and the current order; the placeholders in
     * each SQL string are substituted by the helper names. Each item carries
     * its own windows, since two translations number theirs independently.
     * Only the terms actually used are emitted, so the common _n-only idiom
     * stays streaming instead of buffering the whole input (PERF-1). Returns
     * `prev` untouched when no item has windows. */
    struct RowCtxItem {
        std::string *sql;
        const std::vector<WindowTerm> *windows;
    };
    std::string rowctx_wrap(const std::vector<RowCtxItem> &items, const std::string &prev,
                            const std::vector<std::string> &part);
    /* BY-PREFIX-1: expand the prefix keys, check the view is sorted by
     * by+sortvars (ascending, in that order) and return the partition SQL */
    std::string by_partition(const std::vector<std::string> &by,
                             const std::vector<std::string> &sortvars,
                             std::vector<std::string> *part) const;
    std::string order_by_sql_reversed() const; /* for x[_N-k] */
    std::string sort_key_sql(const std::string &key) const;
    /* keeps_order: the stage selects stage_list(), so the physical order
     * columns reach the next stage; any other stage drops them */
    void push_stage(const std::string &select_body, const std::string &desc,
                    bool keeps_order = false);
    /* TWOTABLE-CHARS-1: keep only the characteristics of live columns and _dta */
    void prune_chars();

    bool live_ = false;
    std::string scan_;
    std::string source_desc_;
    std::string src_paths_sql_; /* Parquet footer paths, "" unless file-backed */
    std::string int64_mode_;    /* int64() carried from the open; "" = session */
    std::string src_filename_col_; /* FILENAME-1 provenance column, "" if none */
    std::vector<std::string> xm_folded_; /* XMISS-1 columns read as plain . */
    std::string ts_panel_, ts_time_;     /* TSSET-1 declaration ("" = none) */
    double ts_delta_ = 1.0;
    bool fragile_windows_ = false;       /* WINDOW-NEST-1 */
    int window_stages_ = 0;              /* egen stages with a window */
    int window_terms_ = 0;               /* "OVER (PARTITION BY" terms over the stages */
    std::vector<std::string> stages_; /* each a full SELECT … FROM <prev> */
    std::vector<std::string> descs_;
    std::vector<ViewCol> cols_;
    std::vector<std::string> sort_;   /* quoted "name" or "name DESC" */
    /* ORDER-CARRIER-1: hidden columns holding each row's physical position in
     * a Parquet source (file index, row within file), carried through the
     * row-preserving stages; a declared sort breaks its ties by them, so the
     * order within a tie is the source's, as in native Stata, and the same at
     * every evaluation (_n, keep in, list in, collect). order_live_ says the
     * current last stage still exposes them. */
    std::vector<std::string> order_cols_;
    bool order_live_ = false;
    bool order_baked_ = false; /* an explicit past sort, even after its keys vanish */
    std::vector<PendingRange> ranges_;
    nlohmann::json vallabs_;
    nlohmann::json chars_;
    std::string dtalabel_;
    long long helper_counter_ = 0;
};

} // namespace parqit
