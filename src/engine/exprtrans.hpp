/* parqit — Stata expression → DuckDB SQL translator (build brief §7).
 *
 * Inside a parqit pipeline every column is a *number or a string*, exactly as
 * in Stata: %td columns are day counts, %tc columns are millisecond counts
 * (the parquet boundary converts to/from DATE/TIMESTAMP). That makes Stata
 * arithmetic on dates translate verbatim.
 *
 * Missing-value semantics:
 *   default (SQL) mode — missing is NULL: comparisons with NULL are
 *     unknown, so `keep if x > 5` drops missings (NULL ⇒ not kept). Note
 *     native Stata KEEPS a missing x there (missing sorts above every
 *     number) — the SQL default is a deliberate, documented divergence;
 *     `parqit set statamissing on` restores Stata's ordering. Explicit
 *     `x == .`, `x != .`, `x < .`, `x >= .` are rewritten to IS NULL tests,
 *     and missing(x)/mi(x) understands strings ("" or NULL).
 *   statamissing mode — emulates "missing is larger than every number":
 *     every ordering comparison is expanded with IS NULL arms.
 *
 * The translator is type-aware (numeric vs string per column) and fails
 * loudly with a position-anchored message on anything it cannot translate
 * faithfully — it never guesses (charter §7 "no fabrication").
 */
#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

namespace parqit {

struct ExprSchema {
    /* column name → kind: 'n' numeric (includes all date/period counts),
     * 's' string */
    std::map<std::string, char> kinds;
    std::set<std::string> float_columns;
    std::map<std::string, std::string> numeric_types;
    /* MISS-1: columns already guaranteed free of IEEE specials (NaN/±Inf/
     * out-of-Stata-range) — the lazy boundary normalized them. missing()/mi()
     * on a bare reference to such a column needs only the cheap `IS NULL` test,
     * not a per-row isfinite scan. A column absent here (e.g. a gen/replace
     * result, an aggregate, or any compound expression) gets the full check. */
    std::set<std::string> normalized;
    /* USE-IF-1: SQL to emit for a bare reference to a column instead of its
     * quoted name — the eager `parqit use ... if` filters the raw scan, whose
     * dates, booleans and floats are not yet Stata values, so each reference
     * becomes its boundary cast (a day count, 0/1, a finite double). Absent
     * columns use quote_ident(name). */
    std::map<std::string, std::string> column_sql;
    /* TSSET-1: the view's time-series declaration (parqit tsset/xtset), engine
     * column names; ts_time empty means none is declared and the operators
     * L. F. D. S. and tin()/twithin() are refused. ts_time_fmt is the time
     * variable's Stata display format (%td, %tm, ...), which decides how a
     * bare date literal in tin()/twithin() is read. */
    std::string ts_panel;
    std::string ts_time;
    double ts_delta = 1.0;
    std::string ts_time_fmt;
};

/* ROWCTX-2 (BPLIM feedback, 2026-10-13): a window term the expression needs.
 * The translator never emits a window function inline — a window cannot sit
 * in a WHERE clause, inside an aggregate or inside another window — it emits a
 * placeholder and records the window's SQL here; the view compiler computes
 * each term as a helper column of a subquery wrapped around the previous stage
 * and substitutes the helper's name. The SQL of a term that follows the
 * current order or the by groups carries one of the tokens
 *   __PARQIT_OVER_ORD__       OVER (PARTITION BY by-keys ORDER BY order)
 *   __PARQIT_OVER_PART__      OVER (PARTITION BY by-keys)
 *   __PARQIT_OVER_FULL__      the ordered window over the whole partition
 *   __PARQIT_OVER_FULL_DESC__ the same, the order reversed
 * which only the view compiler resolves (the by keys come from a by/bysort
 * prefix). A time-series term (L. F. D. S.) is complete SQL over the declared
 * panel and time variables. */
struct WindowTerm {
    std::string placeholder; /* the token in ExprResult::sql */
    std::string sql;         /* the window expression computing it */
};

struct ExprResult {
    bool ok = false;
    std::string sql;
    char kind = 'n'; /* 'n' numeric, 's' string, 'b' boolean */
    std::string error;
    /* ROWCTX-1: the expression consumed _n/_N, so `sql` carries the
     * __PARQIT_ROW__/__PARQIT_NROWS__ placeholders that only the view compiler
     * resolves (View::gen, keep if/drop if). A caller that cannot run that
     * substitution — the read-only stats and preview filters — must refuse the
     * expression itself; letting the placeholder reach DuckDB produced a raw
     * Binder Error naming an internal token (charter §5/§6.12). */
    bool uses_rowctx = false;
    /* ROWCTX-2: the window terms behind the placeholders, in first-use order;
     * uses_rowctx is true exactly when this is nonempty */
    std::vector<WindowTerm> windows;
};

/* Translate a Stata expression. */
ExprResult translate_expression(const std::string &expr, const ExprSchema &schema,
                                bool statamissing);

/* Translate an expression used as a filter condition: boolean results pass
 * through; a bare numeric result x becomes (x IS NULL OR (x) <> 0) — Stata's
 * "true is nonzero OR missing" (`keep if x` keeps missing x, since missing
 * counts as true). String results are an error. */
ExprResult translate_filter(const std::string &expr, const ExprSchema &schema,
                            bool statamissing);

/* STATAMISS-WARN-1: the comparisons of `expr` whose result under the default
 * SQL missing-value rules can differ from native Stata's when a compared value
 * is missing (Stata orders missing above every number; SQL leaves such a
 * comparison unknown). `filter` is true for a condition (keep/drop if,
 * count/list if, an if qualifier) and false for an assigned value. A
 * condition differs only where Stata would make the comparison true (false
 * under an odd number of !); a value differs wherever an operand can be
 * missing. Idioms that settle the missing rows in both modes are recognised:
 * x < ., x != . or !missing(x) in the same & chain, missing(x), x == . or
 * x >= . in the same | chain, and — for a value — those nonmissing tests in
 * the top-level & chain of its if qualifier `guard`. Returns the comparisons as
 * written, in order and without duplicates; empty when none can differ or the
 * expression does not parse (its translation reports that). Static analysis:
 * no data is read, and the translation itself is never affected. */
std::vector<std::string> missing_rule_differences(const std::string &expr,
                                                  const ExprSchema &schema, bool filter,
                                                  const std::string &guard = "");

} // namespace parqit
