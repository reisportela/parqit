/* parqit — Stata expression → DuckDB SQL translator (build brief §7).
 *
 * Inside a parqit pipeline every column is a *number or a string*, exactly as
 * in Stata: %td columns are day counts, %tc columns are millisecond counts
 * (the parquet boundary converts to/from DATE/TIMESTAMP). That makes Stata
 * arithmetic on dates translate verbatim.
 *
 * Missing-value semantics:
 *   default (SQL) mode — missing is NULL: comparisons with NULL are
 *     unknown, so `keep if x > 5` drops missings (NULL ⇒ not kept), which
 *     coincides with Stata's outcome for keep-if; explicit `x == .`,
 *     `x != .`, `x < .`, `x >= .` are rewritten to IS NULL tests, and
 *     missing(x)/mi(x) understands strings ("" or NULL).
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

namespace parqit {

struct ExprSchema {
    /* column name → kind: 'n' numeric (includes all date/period counts),
     * 's' string */
    std::map<std::string, char> kinds;
    /* MISS-1: columns already guaranteed free of IEEE specials (NaN/±Inf/
     * out-of-Stata-range) — the lazy boundary normalized them. missing()/mi()
     * on a bare reference to such a column needs only the cheap `IS NULL` test,
     * not a per-row isfinite scan. A column absent here (e.g. a gen/replace
     * result, an aggregate, or any compound expression) gets the full check. */
    std::set<std::string> normalized;
};

struct ExprResult {
    bool ok = false;
    std::string sql;
    char kind = 'n'; /* 'n' numeric, 's' string, 'b' boolean */
    std::string error;
};

/* Translate a Stata expression. */
ExprResult translate_expression(const std::string &expr, const ExprSchema &schema,
                                bool statamissing);

/* Translate an expression used as a filter condition: boolean results pass
 * through; numeric results x become (x) <> 0 AND x IS NOT NULL — Stata's
 * "true is nonzero nonmissing". String results are an error. */
ExprResult translate_filter(const std::string &expr, const ExprSchema &schema,
                            bool statamissing);

} // namespace parqit
