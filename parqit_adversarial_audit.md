# parqit v0.1.10 adversarial audit report

**Bundle audited:** `/mnt/data/parqit_audit_bundle.zip`, unpacked to `/mnt/data/parqit_audit`.

**Source snapshot confirmed:** `_AUDIT_BUNDLE_README.txt` identifies the bundle as `parqit` source bundle for package version **0.1.10**, with hand-written source, tests, docs, and prior audits included and generated/binary/vendor-heavy artifacts excluded. `CHANGELOG.md` records version **0.1.10 — 2026-06-23** and states that prior Stata/C++ suites were green. `ASSUMPTIONS.md` explicitly keeps strL save return-code checking, percentile/list-detail performance, and reshape i()/j() key folding as deferred items.

**Verification status:** I performed a static adversarial audit from the actual source in the bundle. I did **not** run Stata or the full CMake/DuckDB build in this environment, so runtime outputs in the reproductions are code-derived predictions. Every finding below includes a self-contained verification task for the coding agent, including a native Stata or pyarrow/DuckDB oracle where relevant.

---

## 1. Executive summary

### Finding counts

| severity | count | meaning in this report |
|---|---:|---|
| S1 | 5 | Silent wrong result or silent Stata-semantics violation in ordinary lazy workflows |
| S2 | 0 | High-impact gap/precision loss not already covered by S1 |
| S3 | 2 | Loud-but-wrong, functional/determinism/contract defect, or medium-confidence semantic gap |
| S4 | 0 | Minor/cosmetic/latent only |
| **total** | **7** | 6 high-confidence, 1 medium-confidence |

### Top 5 must-fix issues

1. **PQ-AUD-001 — lazy views do not normalize NaN/±Inf/out-of-Stata-range floating values to missing.** This breaks `missing()`, filters, statistics, lazy `save`, and SQL/query-derived views over foreign Parquet in a way the eager `use, clear` and in-memory `save data` paths already avoid.
2. **PQ-AUD-002 — lazy views preserve Parquet string NULL instead of Stata `""`.** This creates deterministic sort/order divergences, wrong `_n`/`keep in` slices, and stale NULLs on lazy `save`.
3. **PQ-AUD-003 — `replace` ignores the existing variable storage type.** A `byte` can become 200 instead of `.` and a `str3` can widen instead of truncating, despite native Stata storing into the existing slot.
4. **PQ-AUD-004 — `egen` accepts storage declarations but treats them as metadata only.** `egen byte total()` can collect 200 rather than `.` and string storage declarations are not rejected.
5. **PQ-AUD-006 — `duplicates drop` with no varlist bypasses the missing-key normalization machinery.** `NULL` and `""`, or `NaN` and `NULL`, can survive as distinct rows and then collect as duplicate Stata rows.

### Overall risk verdict

`parqit` v0.1.10 appears substantially hardened by the prior audit rounds, but it is **not yet Stata-faithful for lazy operations over foreign Parquet that contains IEEE specials or string NULLs**, and it is **not yet faithful to native Stata storage semantics for `replace`/`egen` with narrow or string-declared storage types**. These are not documentation nits: they can silently change row order, group membership, missingness tests, aggregation results, and collected values while returning `rc 0`.

The fixes are local and should not require removing features or materially increasing computation time. The highest-risk change is the floating-boundary normalization, because it changes lazy SQL expressions for every float/double column. That change is nevertheless the same semantic guard already used by the eager fill and direct in-memory save path, so the regression risk is manageable and the correctness win is large.

---

## 2. Methodology & scope

### What I read

I inspected the full source paths identified by the user prompt and bundle map:

- `src/ado/p/parqit.ado`: dispatch, ado parsing, lazy/eager command routing, `save`, `use`, bridge temp files, two-table verbs, stats/introspection front ends, and `mergein`/`appendin`.
- `src/ado/p/parqit.sthlp`: public contract and documented supported functions/verbs.
- `src/engine/exprtrans.cpp`: lexer, parser, operators, Stata-function lowering, date literals, row-context translation, filter truthiness, and `statamissing` mode.
- `src/engine/view.cpp`: lazy plan compiler for core verbs and storage coercion.
- `src/engine/typemap.cpp`: metadata-driven Stata type reconstruction and DuckDB/Arrow write type mapping.
- `src/engine/session.cpp`, `sanitize.cpp`, `hexcodec.cpp`, `request.cpp`: scalar registration, locale-independent number parsing/printing, identifier and protocol surfaces.
- `src/plugin/parqit_plugin.cpp`, `plugin_io.cpp`, `plugin_view.cpp`: plugin dispatch, exception boundary, eager fill, save paths, lazy view materialization/save/query/stats/reshape, and request/response emission.
- `tests/`, especially `verify_suite/`, prior audit repros, and current `CHANGELOG.md`/`ASSUMPTIONS.md`, to avoid re-reporting fixed or intentionally deferred items.

### How I verified claims

For each candidate, I traced the code path from ado command to plugin request to C++ handler to generated SQL or fill logic. I then tried to refute the candidate by looking for an upstream guard, downstream normalization, prior locked verify test, or documented assumption.

I treated comments as non-authoritative. For example, I did not rely on the comment in `View::order_by_sql()` claiming “missing sorts last in every direction”; I checked whether string NULL is actually normalized before sorting and found it is not.

### What I could not verify here

- I did not execute Stata. Native Stata behaviour stated below is based on Stata semantics needed by the package contract and on the package’s own oracle style. The coding agent must run the exact repro do-files.
- I did not build DuckDB from source or run `ctest`. For floating aggregate propagation and `isfinite(NaN)` details, the agent must confirm DuckDB’s exact behaviour in the bundled build. The simpler `missing(x)` reproductions do not depend on DuckDB aggregate propagation.
- I did not benchmark. Performance impact estimates are based on the changed SQL shape and data-path position, not measured timings.

---

## 3. Findings table

| ID | severity (S1-S4) | dimension | file:line | one-line title | confidence |
|---|---|---|---|---|---|
| PQ-AUD-001 | S1 | missing semantics / lazy-vs-eager / save fidelity | `src/plugin/plugin_view.cpp:118-120`; `src/engine/exprtrans.cpp:749-758`; `src/plugin/plugin_view.cpp:196-230` | Lazy float/double columns keep NaN/±Inf/out-of-range values instead of Stata missing | high |
| PQ-AUD-002 | S1 | missing semantics / determinism / string fidelity | `src/plugin/plugin_view.cpp:160-164`; `src/engine/view.cpp:102-112` | Lazy string columns keep SQL NULL instead of normalizing to Stata empty string | high |
| PQ-AUD-003 | S1 | storage semantics / precision / type fidelity | `src/engine/view.cpp:321-352`; `src/engine/typemap.cpp:325-355` | `replace` does not coerce into the existing variable storage type | high |
| PQ-AUD-004 | S1 | storage semantics / metadata fidelity | `src/engine/view.cpp:683-722`; `src/engine/typemap.cpp:346-354` | `egen` storage type requests are metadata-only, so narrow types are not enforced | high |
| PQ-AUD-006 | S1 | duplicates / missing folding / positional correctness | `src/engine/view.cpp:615-620`; `src/engine/view.cpp:53-62` | `duplicates drop` with no varlist bypasses normalized missing equality | high |
| PQ-AUD-005 | S3 | type checking / loud errors | `src/engine/view.cpp:28-50`; `src/engine/view.cpp:282-315` | `gen` silently accepts explicit storage types incompatible with the expression kind | high |
| PQ-AUD-007 | S3 | date/time literal validation | `src/engine/exprtrans.cpp:182-211`; `src/engine/exprtrans.cpp:240-273`; `src/engine/exprtrans.cpp:672-712` | Literal parser accepts impossible calendar dates and 60-second `tc()` times | medium |

---

## 4. Detailed findings

### PQ-AUD-001 — Lazy float/double columns keep NaN/±Inf/out-of-range values instead of Stata missing

**Severity:** S1 silent wrong result  
**Confidence:** high

**Location:**

- `src/plugin/plugin_view.cpp:109-120`, function `boundary_for`
- `src/engine/exprtrans.cpp:749-758`, function lowering for `missing()`/`mi()`
- `src/plugin/plugin_view.cpp:196-230`, function `compile_for_save`
- Cross-check guard that exists only in another path: `src/plugin/plugin_io.cpp:928-956`, function `fill_column`; `src/plugin/plugin_io.cpp:2588-2600`, direct in-memory save expression builder

**What the code does:**

At the lazy view boundary, DuckDB `FLOAT` and `DOUBLE` columns are passed through raw:

```cpp
// src/plugin/plugin_view.cpp:112-120
case DUCKDB_TYPE_INTEGER:
case DUCKDB_TYPE_BIGINT:
case DUCKDB_TYPE_FLOAT:
case DUCKDB_TYPE_DOUBLE:
    b.sql = ref;
    break;
```

The expression translator tests numeric missingness only with SQL `IS NULL`:

```cpp
// src/engine/exprtrans.cpp:749-758
if (fname == "missing" || fname == "mi") {
    ...
    if (args[k].kind == 's')
        sql += "(coalesce(" + args[k].sql + ", '') = '')";
    else
        sql += "(" + as_num(args[k]) + " IS NULL)";
}
```

Lazy `save` compiles the view and writes the resulting numeric values as-is unless the format is a date/period class:

```cpp
// src/plugin/plugin_view.cpp:196-230
std::string expr = ref;
switch (parqit::classify_format(c.fmt)) {
case parqit::FmtClass::Td:
    expr = "(DATE '1960-01-01' + CAST(round(" + ref + ") AS INTEGER))";
    break;
...
default:
    break;
}
sel += expr + " AS " + ref;
```

The eager Arrow-to-Stata path already has the correct guard:

```cpp
// src/plugin/plugin_io.cpp:945-953
case Transfer::Float64: {
    double d = v[off + r];
    bool unstorable = std::isnan(d) || std::isinf(d) ||
                      std::fabs(d) >= SV_missval;
    if (!std::isnan(d) && unstorable && inf_seen) (*inf_seen)++;
    if (!store_num(r, unstorable ? SV_missval : d)) return false;
}
```

The direct in-memory save path also has the correct guard:

```cpp
// src/plugin/plugin_io.cpp:2593-2599
if (v.st == StType::Float || v.st == StType::Double) {
    const std::string dref = "CAST(" + ref + " AS DOUBLE)";
    return "CASE WHEN " + ref + " IS NULL OR NOT isfinite(" + dref +
           ") OR abs(" + dref + ") >= " + miss +
           " THEN NULL ELSE CAST(" + ref + " AS " + dtype + ") END";
}
```

The lazy view path therefore lacks a guard that the eager and direct-save paths already rely on.

**What native Stata does:**

Stata has no IEEE NaN or infinity numeric values in a dataset variable. When foreign input contains `NaN`, `+Inf`, `-Inf`, or a double whose magnitude collides with Stata’s missing-value sentinel range, the package’s own eager path maps it to Stata missing. Native Stata then evaluates `missing(x)`/`mi(x)` as true, excludes the value from ordinary numeric aggregates, and writes missing rather than an IEEE special on export.

For values generated inside Stata expressions, overflow or non-finite results should likewise be missing, not ordinary numeric values. The package already enforces that design for division and power in `exprtrans.cpp`; the problem here is that imported float specials and some generated float specials can remain non-NULL in the lazy SQL plan.

**Why it is a bug:**

A user running the same source through `parqit use, clear` and through lazy `parqit use` can get different missingness, filters, statistics, and saved Parquet payloads. The lazy path returns `rc 0` while preserving values that Stata cannot represent.

Concrete divergences from the current source:

- `parqit gen m = missing(x)` over a lazy foreign Parquet `x = NaN` compiles to `(x IS NULL)`, which is false for IEEE NaN; eager/native Stata sees `x` as missing and returns `1`.
- `parqit keep if missing(x)` drops NaN/Inf rows in lazy mode while eager/native keeps them.
- `parqit save` from a lazy view can write NaN/Inf back to Parquet because `compile_for_save()` does not reuse the finite/sentinel guard.
- Aggregates and stats operate on raw IEEE specials. The agent must confirm DuckDB’s exact aggregate propagation, but the `missing()` repro is enough to prove the semantic gap.

**Reproduction:**

Create `tests/verify_suite/v37_lazy_boundary_semantics.do` with this first block. It uses pyarrow to create a hostile Parquet input and native/eager Stata as the oracle.

```stata
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
local fails 0
tempfile t

python:
import pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
b = Macro.getLocal("t")
pq.write_table(
    pa.table({
        "id": pa.array([1,2,3,4], type=pa.int32()),
        "x":  pa.array([1.0, float("nan"), float("inf"), None], type=pa.float64())
    }),
    b + "_float_specials.parquet"
)
end

* Oracle: eager path maps NaN/Inf/NULL to Stata missing.
parqit use using `"`t'_float_specials.parquet"', clear
gen byte m_oracle = missing(x)
sort id
tempfile oracle
save `"`oracle'"', replace
capture assert m_oracle[1] == 0 & m_oracle[2] == 1 & m_oracle[3] == 1 & m_oracle[4] == 1
if (_rc) di as err "FAIL PQ-AUD-001 oracle setup: eager load did not map specials as expected"
local fails = `fails' + (_rc != 0)

* Lazy path under current code: missing(x) is x IS NULL, so NaN/Inf are not missing.
parqit use using `"`t'_float_specials.parquet"'
parqit gen m_lazy = missing(x)
parqit collect, clear
sort id
capture assert m_lazy[1] == 0 & m_lazy[2] == 1 & m_lazy[3] == 1 & m_lazy[4] == 1
if (_rc) di as err "FAIL PQ-AUD-001: lazy missing(x) failed to treat NaN/Inf as Stata missing"
local fails = `fails' + (_rc != 0)

* Independent on-disk oracle: lazy save should not preserve IEEE specials.
parqit use using `"`t'_float_specials.parquet"'
parqit save `"`t'_float_specials_out.parquet"', replace
python:
import pyarrow.parquet as pq, math
from sfi import Macro
b = Macro.getLocal("t")
vals = pq.read_table(b + "_float_specials_out.parquet")["x"].to_pylist()
# Expected after fix: [1.0, None, None, None] or an equivalent nullable array.
bad = any((isinstance(v, float) and (math.isnan(v) or math.isinf(v))) for v in vals if v is not None)
Macro.setLocal("has_ieee_special", "1" if bad else "0")
end
capture assert `has_ieee_special' == 0
if (_rc) di as err "FAIL PQ-AUD-001: lazy save preserved NaN/Inf on disk"
local fails = `fails' + (_rc != 0)

if (`fails' == 0) di as txt "VERDICT(PQ_AUD_001_LAZY_FLOAT_SPECIALS): PASS"
else {
    di as err "VERDICT(PQ_AUD_001_LAZY_FLOAT_SPECIALS): FAIL (`fails' failures)"
    exit 459
}
```

Expected native/eager result: `m_oracle = 0,1,1,1`.  
Expected current lazy result from source: `m_lazy` is false for NaN/Inf because the SQL is `x IS NULL`; the pyarrow read of the lazy-saved file can still contain IEEE specials.

**Proposed fix:**

Apply normalization at the **lazy boundary**, not only during collect, so every lazy verb sees Stata-semantics values.

Minimal edit in `src/plugin/plugin_view.cpp`, `boundary_for()`:

```cpp
case DUCKDB_TYPE_FLOAT:
case DUCKDB_TYPE_DOUBLE: {
    const std::string dref = "CAST(" + ref + " AS DOUBLE)";
    b.sql = "CASE WHEN " + ref + " IS NULL OR NOT isfinite(" + dref +
            ") OR abs(" + dref + ") >= " + std::to_string(SV_missval) +
            " THEN NULL ELSE " + ref + " END";
    break;
}
```

The agent must confirm in the bundled DuckDB that `NOT isfinite(NaN)` is true. If DuckDB treats `isfinite(NaN)` unexpectedly, use:

```sql
... OR isnan(CAST(ref AS DOUBLE)) OR NOT isfinite(CAST(ref AS DOUBLE)) ...
```

Also consider applying the same wrapper in `compile_for_save()` as a defensive final guard for numeric float/double output columns, especially for specials generated after the boundary by expressions such as `exp()`.

**regression_risk:** medium. This changes lazy SQL for all float/double columns and may change results for users who unknowingly relied on raw NaN/Inf semantics. That reliance is incompatible with Stata dataset semantics and with the eager path. Tests around `v06`, `v11`, `v15`, `v19`, `v33`, `v35` should be re-run.

**perf_impact:** low to medium. The boundary adds a `CASE/isfinite/abs` expression for float/double columns referenced in lazy scans. It is vectorized and equivalent to a guard already used in direct save. It may slightly reduce pushdown for those columns; the correctness gain is necessary, and unaffected columns do not pay the cost.

**🤖 AI Agent Instructions:**

1. Reproduce by adding the do-file block above as `tests/verify_suite/v37_lazy_boundary_semantics.do` and running `bash tests/run_stata.sh v37_lazy_boundary_semantics` against the current tree.
2. Confirm the generated SQL by adding a temporary `parqit explain` after `parqit gen m_lazy = missing(x)`; verify it contains `x IS NULL` before the fix and a boundary `CASE` after the fix.
3. In `src/plugin/plugin_view.cpp`, edit `boundary_for()` so `DUCKDB_TYPE_FLOAT` and `DUCKDB_TYPE_DOUBLE` are not grouped with integer pass-through. Wrap them in the finite/sentinel guard shown above. Include `SV_missval` via the same header already used by `plugin_io.cpp`, or expose a small shared helper to avoid duplicating the literal.
4. Add a defensive unit or verify test for expression-generated specials: `parqit gen z = exp(10000)` then `parqit gen mz = missing(z)` and lazy `save`; the agent must confirm DuckDB’s `exp(10000)` behaviour before finalizing this subcase.
5. Run:
   ```bash
   cmake --preset dev && cmake --build build/dev -j
   ctest --preset dev
   bash tests/run_stata.sh v37_lazy_boundary_semantics
   bash tests/run_stata.sh
   bash tests/release_lint.sh
   ```
6. Update `CHANGELOG.md` with `PQ-AUD-001`, add a short note in `ASSUMPTIONS.md` only if a DuckDB `isfinite()` nuance is documented, and bump the four release-version surfaces only if cutting a release.

---

### PQ-AUD-002 — Lazy string columns keep SQL NULL instead of normalizing to Stata empty string

**Severity:** S1 silent wrong result  
**Confidence:** high

**Location:**

- `src/plugin/plugin_view.cpp:160-164`, function `boundary_for`
- `src/engine/view.cpp:102-112`, function `View::order_by_sql`
- Related no-varlist duplicate effect is separately tracked as PQ-AUD-006.

**What the code does:**

Lazy string boundary handling passes nullable SQL strings through raw:

```cpp
// src/plugin/plugin_view.cpp:160-164
case DUCKDB_TYPE_VARCHAR: b.sql = ref; b.kind = 's'; break;
case DUCKDB_TYPE_ENUM:
case DUCKDB_TYPE_UUID:
    b.sql = "CAST(" + ref + " AS VARCHAR)";
    b.kind = 's';
```

Sorting then unconditionally uses SQL NULL ordering:

```cpp
// src/engine/view.cpp:102-112
std::string View::order_by_sql() const {
    if (sort_.empty()) return "";
    std::string o = " ORDER BY ";
    ...
    o += sort_[i] + " NULLS LAST";
    return o;
}
```

Several expression functions coalesce strings locally, for example `missing(s)` uses `coalesce(s,'') = ''`, but the raw column value remains SQL NULL for ordering, row numbering, `keep in`, materialized lazy saves, `SELECT DISTINCT`, and arbitrary SQL/query outputs.

**What native Stata does:**

Stata has no distinct string NULL. The string missing value is the zero-length string `""`. Therefore a foreign Parquet string NULL and a Parquet empty string must become the same Stata value. Native/eager Stata sorting places `""` according to string ordering, not as an SQL NULL sentinel. With `sort s id`, rows where `s` is NULL and rows where `s` is `""` are tied on `s` after load and are ordered by `id`.

**Why it is a bug:**

The lazy and eager paths can return different row order and different rows for `_n`-sensitive commands.

Example from the code:

- Lazy boundary keeps `s = NULL` and `s = ""` distinct.
- `parqit sort s id` compiles to `ORDER BY "s" NULLS LAST, "id" NULLS LAST`.
- Native/eager Stata has two `s == ""` rows, so those rows sort together before `"a"` and are secondarily ordered by `id`.

This affects not only display order but also `head`, `list`, `keep in`, `_n` expressions, deterministic first/last choices, and lazy `save` payloads.

**Reproduction:**

Append this block to `tests/verify_suite/v37_lazy_boundary_semantics.do` or create `tests/verify_suite/v38_lazy_string_null.do`.

```stata
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
local fails 0
tempfile t

python:
import pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
b = Macro.getLocal("t")
pq.write_table(
    pa.table({
        "id": pa.array([1,2,3], type=pa.int32()),
        "s":  pa.array([None, "a", ""], type=pa.string())
    }),
    b + "_string_null.parquet"
)
end

* Oracle: eager/native Stata has s=="" for id 1 and id 3.
parqit use using `"`t'_string_null.parquet"', clear
sort s id
local oracle_order = string(id[1]) + "," + string(id[2]) + "," + string(id[3])
capture assert "`oracle_order'" == "1,3,2"
if (_rc) di as err "FAIL PQ-AUD-002 oracle setup: eager string NULL normalization/order unexpected: `oracle_order'"
local fails = `fails' + (_rc != 0)

* Lazy current code sorts SQL NULL last.
parqit use using `"`t'_string_null.parquet"'
parqit sort s id
parqit collect, clear
local lazy_order = string(id[1]) + "," + string(id[2]) + "," + string(id[3])
capture assert "`lazy_order'" == "1,3,2"
if (_rc) di as err "FAIL PQ-AUD-002: lazy sort order was `lazy_order', expected 1,3,2"
local fails = `fails' + (_rc != 0)

* Independent payload check: lazy save should write no string NULLs.
parqit use using `"`t'_string_null.parquet"'
parqit save `"`t'_string_null_out.parquet"', replace
python:
import pyarrow.parquet as pq
from sfi import Macro
b = Macro.getLocal("t")
vals = pq.read_table(b + "_string_null_out.parquet")["s"].to_pylist()
Macro.setLocal("has_null_string", "1" if any(v is None for v in vals) else "0")
end
capture assert `has_null_string' == 0
if (_rc) di as err "FAIL PQ-AUD-002: lazy save preserved string NULL on disk"
local fails = `fails' + (_rc != 0)

if (`fails' == 0) di as txt "VERDICT(PQ_AUD_002_LAZY_STRING_NULL): PASS"
else {
    di as err "VERDICT(PQ_AUD_002_LAZY_STRING_NULL): FAIL (`fails' failures)"
    exit 459
}
```

Expected native/eager order: `1,3,2`.  
Expected current lazy order from source: `3,2,1`, because `"" < "a" < NULL` with `NULLS LAST`.

**Proposed fix:**

Normalize at the lazy boundary:

```cpp
case DUCKDB_TYPE_VARCHAR:
    b.sql = "coalesce(" + ref + ", '')";
    b.kind = 's';
    break;
case DUCKDB_TYPE_ENUM:
case DUCKDB_TYPE_UUID:
    b.sql = "coalesce(CAST(" + ref + " AS VARCHAR), '')";
    b.kind = 's';
    break;
```

This is better than special-casing sort, because it also fixes `_n`, `keep in`, lazy `save`, duplicates, string comparisons, and arbitrary query/view manifests.

**regression_risk:** low to medium. SQL NULL strings are not representable in Stata, and the eager path already collapses them to `""`. Risk is mostly around users who expected lazy SQL to preserve a foreign-tool distinction that the Stata API cannot express.

**perf_impact:** low. `coalesce()` on string columns is vectorized and only applies to selected/used columns. It may slightly affect string predicate pushdown for raw source columns, but the semantic normalization is required.

**🤖 AI Agent Instructions:**

1. Add and run the reproduction block above with `bash tests/run_stata.sh v38_lazy_string_null` or as part of `v37_lazy_boundary_semantics`.
2. Confirm the pre-fix SQL via `parqit explain` contains raw `"s"` in the boundary projection and `ORDER BY "s" NULLS LAST`.
3. Edit `src/plugin/plugin_view.cpp::boundary_for()` as shown so `VARCHAR`, `ENUM`, and `UUID` normalize NULL to `''`.
4. Extend the verify test with a `parqit keep in 1/1` after `parqit sort s id`; assert that the kept `id` is `1` after the fix.
5. Run:
   ```bash
   cmake --preset dev && cmake --build build/dev -j
   ctest --preset dev
   bash tests/run_stata.sh v38_lazy_string_null
   bash tests/run_stata.sh
   bash tests/release_lint.sh
   ```
6. Document the fix in `CHANGELOG.md`. Do not alter the documented `NULL≡""` assumption except to mention that lazy views now enforce it at the boundary.

---

### PQ-AUD-003 — `replace` does not coerce into the existing variable storage type

**Severity:** S1 silent wrong result  
**Confidence:** high

**Location:**

- `src/engine/view.cpp:321-352`, function `View::replace`
- `src/engine/view.cpp:28-50`, helper `coerce_storage()` that `gen` uses but `replace` does not
- `src/engine/typemap.cpp:325-355`, function `apply_meta_type()`

**What the code does:**

`gen` applies explicit storage coercion:

```cpp
// src/engine/view.cpp:288-292
std::string vexpr = coerce_storage(r.sql, type_req, r.kind);
std::string value = vexpr;
```

The coercion helper does the native-like truncation and range-to-missing for integer storage and string byte truncation for `str#`:

```cpp
// src/engine/view.cpp:34-50
if (kind == 's') {
    if (mt == StType::Str && mb > 0)
        return "parqit_substr_bytes(" + v + ", 1, " + std::to_string(mb) + ")";
    return v;
}
...
return "(CASE WHEN (" + v + ") IS NULL THEN NULL WHEN trunc(" + v + ") < " +
       dtoa(lo) + " OR trunc(" + v + ") > " + dtoa(hi) +
       " THEN NULL ELSE CAST(trunc(" + v + ") AS " + duckint + ") END)";
```

`replace` validates only string-vs-numeric family and then inserts the raw translated expression:

```cpp
// src/engine/view.cpp:321-340
ExprResult r = translate_expression(expr, schema_of(cols_), statamissing);
...
char newkind = (r.kind == 's' ? 's' : 'n');
if (newkind != cols_[idx].kind) return "type mismatch...";
std::string value = r.sql;
...
value = "(CASE WHEN coalesce(" + c.sql + ", FALSE) THEN " + r.sql +
        " ELSE " + quote_ident(name) + " END)";
```

`apply_meta_type()` cannot rescue this later. It deliberately widens to the observed range rather than narrowing values back into a requested or saved type:

```cpp
// src/engine/typemap.cpp:346-354
if (mt == StType::Float &&
    (p.stata_type == StType::Float || int_rank(p.stata_type) > 0)) {
    if (p.stata_type != StType::Double) p.stata_type = StType::Float;
    return;
}
int mr = int_rank(mt), pr = int_rank(p.stata_type);
if (mr > 0 && pr > 0 && mr > pr) p.stata_type = mt;
```

**What native Stata does:**

`replace` writes into an existing Stata storage slot. It does not dynamically widen a `byte`, `int`, `long`, `float`, or `str#` variable. Numeric replacement into integer storage truncates toward zero; if the truncated value is outside the storage range, the result is missing. String replacement into `str#` is truncated to the declared byte width.

Examples:

- `byte b = 1`; `replace b = 200` stores `.` because 200 is outside Stata byte range.
- `byte b = 1`; `replace b = 3.9` stores `3`.
- `str3 s = "zz"`; `replace s = "abcdef"` stores `"abc"`.

**Why it is a bug:**

The lazy plan can silently change both values and effective storage width. The user asked to mutate an existing typed Stata variable; parqit returns a representable wider value instead of the value native Stata would store in the existing column.

This violates the exact class of invariant already fixed for `gen byte/int/long/float` and `gen str#`: storage semantics must be value semantics, not just metadata.

**Reproduction:**

Create `tests/verify_suite/v39_replace_storage_semantics.do`:

```stata
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
local fails 0
tempfile t oracle

clear
input byte b str3 s
1 "zz"
end
parqit save `"`t'_typed.parquet"', replace data

* Native oracle.
clear
input byte b str3 s
1 "zz"
end
replace b = 200
replace s = "abcdef"
save `"`oracle'"', replace
capture assert missing(b) & s == "abc"
if (_rc) di as err "FAIL PQ-AUD-003 oracle setup: native replace semantics unexpected"
local fails = `fails' + (_rc != 0)

* Lazy parqit under current code inserts raw 200 and raw "abcdef".
parqit use using `"`t'_typed.parquet"'
parqit replace b = 200
parqit replace s = "abcdef"
parqit collect, clear
capture assert missing(b) & s == "abc"
if (_rc) di as err "FAIL PQ-AUD-003: lazy replace did not store into existing byte/str3 semantics"
local fails = `fails' + (_rc != 0)

if (`fails' == 0) di as txt "VERDICT(PQ_AUD_003_REPLACE_STORAGE): PASS"
else {
    di as err "VERDICT(PQ_AUD_003_REPLACE_STORAGE): FAIL (`fails' failures)"
    exit 459
}
```

Expected native result: `b == .` and `s == "abc"`.  
Expected current lazy result from source: `b == 200` and `s == "abcdef"` or widened string storage, because the replacement expression is not coerced.

**Proposed fix:**

In `View::replace()`, coerce replacement values using the existing column’s intended storage type.

Minimal shape:

```cpp
std::string target_type = cols_[idx].meta_type;
std::string new_value = coerce_storage(r.sql, target_type, r.kind);
std::string value = new_value;
...
if (!if_expr.empty()) {
    value = "(CASE WHEN coalesce(" + c.sql + ", FALSE) THEN " + new_value +
            " ELSE " + quote_ident(name) + " END)";
}
```

For source columns whose `meta_type` is empty, add a helper that maps `ViewCol`/current schema to a Stata storage type. Otherwise `replace` will still fail to enforce widths for variables opened from foreign Parquet where metadata is absent. The simplest safe approach is:

1. Preserve `meta_type` from `boundary_for()`/`plan_columns()` whenever a Stata storage type is known.
2. For strings, store observed Stata width in metadata when possible; if unknown, native Stata would have a concrete width after load, so the collect planner’s inferred width can be used for a conservative test case.
3. For numeric foreign columns, enforce explicit saved Stata types and typed variables generated by parqit; for pure foreign numeric types with no Stata type metadata, document whether they behave as `double` at the lazy boundary.

The immediate high-value fix is to enforce `cols_[idx].meta_type` for typed `gen`/Stata-saved columns, which is enough for the repro above.

**regression_risk:** medium. Existing lazy workflows that accidentally widened typed columns will change. That change is required for native Stata parity. Main risk is in conditional `replace`: the `ELSE` branch must preserve the old value without re-coercing it incorrectly.

**perf_impact:** low. Coercion adds a `CASE/trunc/cast` for replaced columns only. It does not add scans or joins.

**🤖 AI Agent Instructions:**

1. Add `tests/verify_suite/v39_replace_storage_semantics.do` from the reproduction above and run `bash tests/run_stata.sh v39_replace_storage_semantics` to confirm failure.
2. Confirm diagnosis by running `parqit explain` after `parqit replace b = 200`; verify the pre-fix projection contains `200 AS "b"` rather than a `CASE WHEN trunc(200) ... THEN NULL` coercion.
3. Edit `src/engine/view.cpp::View::replace()` so the replacement expression is wrapped by `coerce_storage()` using the existing column storage intent. Apply the coerced expression in both unconditional and `if` forms.
4. Add subcases for `replace b = 3.9`, `replace int i = 40000`, `replace long l = 3e10`, `replace float f = 1/3`, and `replace str3 s = "abcdef"`. The float subcase should verify storage/precision using native Stata, not a hand-written tolerance.
5. Run:
   ```bash
   cmake --preset dev && cmake --build build/dev -j
   ctest --preset dev
   bash tests/run_stata.sh v39_replace_storage_semantics
   bash tests/run_stata.sh
   bash tests/release_lint.sh
   ```
6. Update `CHANGELOG.md` with `PQ-AUD-003`. If the final fix intentionally treats pure foreign numeric columns as double until explicitly typed, record that in `ASSUMPTIONS.md`.

---

### PQ-AUD-004 — `egen` storage type requests are metadata-only, so narrow types are not enforced

**Severity:** S1 silent wrong result  
**Confidence:** high

**Location:**

- `src/engine/view.cpp:683-722`, function `View::egen`
- `src/engine/typemap.cpp:346-354`, function `apply_meta_type`

**What the code does:**

`View::egen()` accepts a `type_req` parameter, builds a window aggregate, and stores the requested type only as metadata:

```cpp
// src/engine/view.cpp:683-722
std::string View::egen(..., const std::string &type_req) {
    ...
    if (fcn == "total") agg = "coalesce(sum(" + a.sql + ") " + over + ", 0)";
    else if (fcn == "mean") agg = "avg(" + a.sql + ") " + over;
    ...
    push_stage("SELECT " + select_list() + ", " + agg + " AS " + quote_ident(name) +
                   " FROM " + prev_name(stages_.size()), ...);
    ViewCol nc;
    nc.name = name;
    nc.kind = 'n';
    nc.meta_type = type_req;
    cols_.push_back(nc);
    return "";
}
```

Unlike `gen`, it never calls `coerce_storage(agg, type_req, 'n')` and never rejects a string storage request.

As in PQ-AUD-003, `apply_meta_type()` later refuses to narrow an observed integer range to a too-small requested integer type:

```cpp
// src/engine/typemap.cpp:353-354
int mr = int_rank(mt), pr = int_rank(p.stata_type);
if (mr > 0 && pr > 0 && mr > pr) p.stata_type = mt;
```

If the aggregate value requires a wider type than `byte`, the planner keeps the wider observed type. It does not replace out-of-range values with missing.

**What native Stata does:**

`egen` with a storage type creates a variable of that storage type. Numeric storage semantics are the same as for `generate`: integer storage truncates toward zero and out-of-range values become missing; `float` rounds to float precision; string storage types are not valid for numeric `egen total/mean/sd/min/max/count` outputs.

Example: after two observations with `x = 100, 100`, native `egen byte t = total(x)` stores missing in `t`, because 200 is outside byte range.

**Why it is a bug:**

The user’s explicit storage declaration changes metadata only, not values. That is a silent wrong result: collected data can contain 200 where native Stata has `.`. It also compromises metadata fidelity, because the output may not actually have the requested type.

**Reproduction:**

Create `tests/verify_suite/v40_egen_storage_semantics.do`:

```stata
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
local fails 0
tempfile t

clear
input double x
100
100
end
parqit save `"`t'_egen_src.parquet"', replace data

* Native oracle.
clear
input double x
100
100
end
egen byte t = total(x)
capture assert missing(t[1]) & missing(t[2])
if (_rc) di as err "FAIL PQ-AUD-004 oracle setup: native egen byte total did not go missing"
local fails = `fails' + (_rc != 0)

* Lazy parqit under current code computes raw coalesce(sum(...),0) over () = 200.
parqit use using `"`t'_egen_src.parquet"'
parqit egen byte t = total(x)
parqit collect, clear
capture assert missing(t[1]) & missing(t[2])
if (_rc) di as err "FAIL PQ-AUD-004: parqit egen byte total did not enforce byte range"
local fails = `fails' + (_rc != 0)

* String storage request should be a loud type error for numeric egen.
parqit use using `"`t'_egen_src.parquet"'
capture noisily parqit egen str3 s = total(x)
capture assert _rc != 0
if (_rc) di as err "FAIL PQ-AUD-004: parqit accepted egen str3 s = total(x)"
local fails = `fails' + (_rc != 0)

if (`fails' == 0) di as txt "VERDICT(PQ_AUD_004_EGEN_STORAGE): PASS"
else {
    di as err "VERDICT(PQ_AUD_004_EGEN_STORAGE): FAIL (`fails' failures)"
    exit 459
}
```

Expected native result: `t` is missing in both rows; `egen str3 s = total(x)` is a type error.  
Expected current lazy result from source: `t == 200` and string storage request is accepted as a numeric output with `meta_type = "str3"`.

**Proposed fix:**

In `View::egen()`:

1. Parse `type_req` with `sttype_parse()`.
2. Reject `StType::Str` and `StType::StrL` for the supported numeric `egen` functions.
3. Wrap `agg` with `coerce_storage(agg, type_req, 'n')` before emitting the stage.
4. Keep `nc.meta_type = type_req` after value coercion so metadata and values agree.

Pseudo-patch:

```cpp
if (!type_req.empty()) {
    StType mt; int mb = 0;
    if (sttype_parse(type_req, &mt, &mb) && (mt == StType::Str || mt == StType::StrL))
        return "type mismatch: egen " + fcn + "() produces numeric results";
}
std::string stored = coerce_storage(agg, type_req, 'n');
push_stage("SELECT " + select_list() + ", " + stored + " AS " + quote_ident(name) +
           " FROM " + prev_name(stages_.size()), ...);
```

**regression_risk:** medium. Existing `egen byte/int/long/float` outputs may change where they previously widened. That is exactly the Stata-parity fix. String-type rejection may break scripts that accidentally requested `str#` and ignored the mismatch.

**perf_impact:** low. The fix adds scalar casts/guards to the `egen` expression only. It does not add scans; the window aggregate is unchanged.

**🤖 AI Agent Instructions:**

1. Add the reproduction as `tests/verify_suite/v40_egen_storage_semantics.do` and run `bash tests/run_stata.sh v40_egen_storage_semantics` to confirm failure.
2. Confirm the diagnosis with `parqit explain`; the pre-fix `egen byte` SQL should show `coalesce(sum("x") OVER (), 0)` without range guard.
3. Edit `src/engine/view.cpp::View::egen()` to reject string storage requests and call `coerce_storage()` on the aggregate SQL for numeric storage requests.
4. Add tests for `egen int`, `egen long`, `egen float`, and `egen byte, by(g)` to ensure grouped window output also respects storage.
5. Run:
   ```bash
   cmake --preset dev && cmake --build build/dev -j
   ctest --preset dev
   bash tests/run_stata.sh v40_egen_storage_semantics
   bash tests/run_stata.sh
   bash tests/release_lint.sh
   ```
6. Update `CHANGELOG.md` under a new unreleased fix entry. No assumption change is needed unless unsupported `egen` type syntax is documented more narrowly.

---

### PQ-AUD-006 — `duplicates drop` with no varlist bypasses normalized missing equality

**Severity:** S1 silent wrong result  
**Confidence:** high

**Location:**

- `src/engine/view.cpp:615-620`, function `View::duplicates_drop`
- Normalization helper exists at `src/engine/view.cpp:53-62`, function `norm_group_key`
- Varlist branch uses normalization at `src/engine/view.cpp:629-639`; no-varlist branch does not.

**What the code does:**

The no-varlist implementation is raw SQL `DISTINCT` over the current projected columns:

```cpp
// src/engine/view.cpp:615-620
std::string View::duplicates_drop(const std::vector<std::string> &by, bool force) {
    const std::string prev = prev_name(stages_.size());
    if (by.empty()) {
        push_stage("SELECT DISTINCT " + select_list() + " FROM " + prev,
                   "duplicates drop");
        return "";
    }
```

The codebase already has a normalization helper for Stata group-key equality:

```cpp
// src/engine/view.cpp:53-62
std::string norm_group_key(const std::string &ref, char kind) {
    if (kind == 's') return "nullif(" + ref + ", '')";
    return "(CASE WHEN " + ref + " IS NULL THEN NULL "
           "WHEN isnan(CAST(" + ref + " AS DOUBLE)) THEN NULL "
           "ELSE " + ref + " END)";
}
```

And the varlist branch uses it:

```cpp
// src/engine/view.cpp:629-639
part += norm_group_key(quote_ident(byn[i]), cols_[col_index(byn[i])].kind);
push_stage("SELECT " + select_list() + " FROM (SELECT *, row_number() OVER "
           "(PARTITION BY " + part + order_by_sql() + ") AS " + quote_ident(rn) +
           " FROM " + prev + ") WHERE " + quote_ident(rn) + " = 1", ...);
```

The no-varlist path is therefore the only duplicates path that does not apply the package’s own `NULL≡""`/`NaN≡NULL` group-key contract.

**What native Stata does:**

Native Stata has one string missing (`""`) and one base numeric missing concept for ordinary data loaded from foreign Parquet (`.`), so two observations that differ only by Parquet NULL vs empty string, or IEEE NaN vs SQL NULL, are duplicates after load. `duplicates drop` with no varlist considers all variables and drops duplicate observations.

**Why it is a bug:**

`SELECT DISTINCT` can preserve foreign distinctions that collapse when collected into Stata. The result can contain duplicate Stata observations after `parqit collect, clear`, even though the user ran `parqit duplicates drop` with no varlist and got `rc 0`.

This remains a bug even if PQ-AUD-001 and PQ-AUD-002 are fixed at the boundary, because `duplicates drop` should not depend on raw SQL distinctness where Stata equality semantics are required. The varlist branch already shows the correct design.

**Reproduction:**

Create `tests/verify_suite/v41_duplicates_no_varlist_missing.do`:

```stata
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
local fails 0
tempfile t

python:
import pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
b = Macro.getLocal("t")
pq.write_table(
    pa.table({
        "s": pa.array([None, ""], type=pa.string()),
        "x": pa.array([1, 1], type=pa.int32())
    }),
    b + "_dup_string_missing.parquet"
)
pq.write_table(
    pa.table({
        "g": pa.array([float("nan"), None], type=pa.float64()),
        "x": pa.array([1, 1], type=pa.int32())
    }),
    b + "_dup_numeric_missing.parquet"
)
end

* Oracle: after eager load, NULL and "" are the same Stata string value.
parqit use using `"`t'_dup_string_missing.parquet"', clear
duplicates drop
capture assert _N == 1
if (_rc) di as err "FAIL PQ-AUD-006 oracle string setup: native/eager duplicates did not drop to 1"
local fails = `fails' + (_rc != 0)

* Lazy no-varlist path uses SELECT DISTINCT and can keep both rows.
parqit use using `"`t'_dup_string_missing.parquet"'
parqit duplicates drop
parqit collect, clear
capture assert _N == 1
if (_rc) di as err "FAIL PQ-AUD-006: lazy duplicates drop kept NULL-vs-empty string duplicate rows"
local fails = `fails' + (_rc != 0)

* Numeric NaN/NULL variant.
parqit use using `"`t'_dup_numeric_missing.parquet"', clear
duplicates drop
capture assert _N == 1
if (_rc) di as err "FAIL PQ-AUD-006 oracle numeric setup: native/eager duplicates did not drop to 1"
local fails = `fails' + (_rc != 0)

parqit use using `"`t'_dup_numeric_missing.parquet"'
parqit duplicates drop
parqit collect, clear
capture assert _N == 1
if (_rc) di as err "FAIL PQ-AUD-006: lazy duplicates drop kept NaN-vs-NULL duplicate rows"
local fails = `fails' + (_rc != 0)

if (`fails' == 0) di as txt "VERDICT(PQ_AUD_006_DUPLICATES_NOVAR_MISSING): PASS"
else {
    di as err "VERDICT(PQ_AUD_006_DUPLICATES_NOVAR_MISSING): FAIL (`fails' failures)"
    exit 459
}
```

Expected native/eager result: `_N == 1` in both cases.  
Expected current lazy result from source: `_N == 2` can survive because SQL `DISTINCT` sees NULL distinct from `""` and NaN distinct from NULL, then collect maps both rows to the same Stata representation.

**Proposed fix:**

Do not use raw `SELECT DISTINCT` for no-varlist duplicates. Use the same normalized partition-key approach as the varlist branch, with a deterministic row survivor.

Minimal shape:

```cpp
if (by.empty()) {
    std::string part;
    for (size_t i = 0; i < cols_.size(); i++) {
        if (i) part += ", ";
        part += norm_group_key(quote_ident(cols_[i].name), cols_[i].kind);
    }
    std::string rn = fresh_helper("rn");
    std::string order = sort_.empty() ? stable_all_column_order_sql() : order_by_sql();
    push_stage("SELECT " + select_list() + " FROM (SELECT *, row_number() OVER "
               "(PARTITION BY " + part + order + ") AS " + quote_ident(rn) +
               " FROM " + prev + ") WHERE " + quote_ident(rn) + " = 1",
               "duplicates drop");
    return "";
}
```

If there is no existing sort order, native Stata keeps the first observation in current order. Lazy SQL has no physical row order unless defined. The agent should use the same deterministic fallback used elsewhere in the package for `first/last`: an order over all columns plus a helper row number if needed. If exact “first current row” is impossible without a prior sort, document the deterministic fallback; but do not keep raw SQL `DISTINCT`.

Boundary fixes in PQ-AUD-001/PQ-AUD-002 will reduce exposure, but this no-varlist code path should still be made semantically robust.

**regression_risk:** medium. Replacing `SELECT DISTINCT` with a window partition can change survivor order and may be slower on wide rows. However it is required to respect Stata missing equality and to avoid duplicate rows after collect.

**perf_impact:** medium for wide datasets. `SELECT DISTINCT` is generally optimized; a `row_number()` partition over normalized expressions may be heavier. The implementation can specialize: after boundary normalization is fixed, string NULL/NaN collapse may already hold, so a normalized-distinct projection may be possible. Measure on representative wide data before choosing the final SQL.

**🤖 AI Agent Instructions:**

1. Add the reproduction as `tests/verify_suite/v41_duplicates_no_varlist_missing.do` and run `bash tests/run_stata.sh v41_duplicates_no_varlist_missing`.
2. Confirm the pre-fix SQL via `parqit explain`; it should contain `SELECT DISTINCT` with raw projected columns.
3. Edit `src/engine/view.cpp::View::duplicates_drop()` so the no-varlist branch partitions or distincts on `norm_group_key()` for every variable. Ensure string and numeric columns are normalized consistently with `collapse`, `contract`, and varlist `duplicates`.
4. Add a determinism subtest with three duplicate rows and a preceding `parqit sort id` to verify the kept row matches native Stata’s sorted first row. Add a no-sort subtest and document the deterministic fallback if exact original order is unavailable.
5. Run:
   ```bash
   cmake --preset dev && cmake --build build/dev -j
   ctest --preset dev
   bash tests/run_stata.sh v41_duplicates_no_varlist_missing
   bash tests/run_stata.sh
   bash tests/release_lint.sh
   ```
6. Update `CHANGELOG.md`. If the no-sort survivor order is intentionally deterministic rather than native-current-order, record the exact contract in `ASSUMPTIONS.md` and `parqit.sthlp`.

---

### PQ-AUD-005 — `gen` silently accepts explicit storage types incompatible with the expression kind

**Severity:** S3 loud-error contract defect  
**Confidence:** high

**Location:**

- `src/engine/view.cpp:28-50`, helper `coerce_storage()`
- `src/engine/view.cpp:282-315`, function `View::gen`

**What the code does:**

The storage coercion helper explicitly no-ops on mismatched type families:

```cpp
// src/engine/view.cpp:34-47
if (kind == 's') {
    if (mt == StType::Str && mb > 0)
        return "parqit_substr_bytes(" + v + ", 1, " + std::to_string(mb) + ")";
    return v; /* strL / numeric-typed target on a string: no width clamp */
}
...
case StType::Float: return "CAST(" + v + " AS FLOAT)";
default: return v; /* double / str# on a numeric value: keep full precision */
```

`View::gen()` then accepts the output and records `meta_type = type_req`, without checking that `type_req` is compatible with the translated expression kind:

```cpp
// src/engine/view.cpp:286-315
ExprResult r = translate_expression(expr, schema_of(cols_), statamissing);
...
std::string vexpr = coerce_storage(r.sql, type_req, r.kind);
...
nc.kind = (r.kind == 's' ? 's' : 'n');
nc.meta_type = type_req; /* requested storage type wins at collect */
```

So `gen str3 s = 123` becomes a numeric column named `s` with string metadata that cannot force numeric-to-string conversion, and `gen byte b = "abc"` becomes a string column named `b` with numeric metadata that cannot force string-to-numeric conversion.

**What native Stata does:**

Native Stata rejects incompatible storage declarations and expression kinds with a type mismatch. Numeric expressions cannot initialize `str#` variables through `generate str#`, and string expressions cannot initialize numeric storage types. The command should be loud and nonzero, not accepted with contradictory metadata.

**Why it is a bug:**

This is not always a wrong data value immediately, but it is a contract violation and a metadata-fidelity hazard. A command that native Stata rejects can succeed with `rc 0`, creating a variable whose name and metadata imply one type family while its SQL expression and collected type imply another.

**Reproduction:**

Create `tests/verify_suite/v42_gen_type_mismatch.do`:

```stata
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
local fails 0
tempfile t

clear
set obs 1
gen x = 1
parqit save `"`t'_one.parquet"', replace data

* Native oracle: both are type mismatches.
clear
set obs 1
capture noisily gen str3 s = 123
local rc_native_num_to_str = _rc
capture noisily gen byte b = "abc"
local rc_native_str_to_num = _rc
capture assert `rc_native_num_to_str' != 0 & `rc_native_str_to_num' != 0
if (_rc) di as err "FAIL PQ-AUD-005 oracle setup: native gen mismatch did not fail"
local fails = `fails' + (_rc != 0)

* Lazy parqit should also fail; current code accepts.
parqit use using `"`t'_one.parquet"'
capture noisily parqit gen str3 s = 123
local rc_pq_num_to_str = _rc
capture assert `rc_pq_num_to_str' != 0
if (_rc) di as err "FAIL PQ-AUD-005: parqit accepted gen str3 s = 123"
local fails = `fails' + (_rc != 0)

parqit use using `"`t'_one.parquet"'
capture noisily parqit gen byte b = "abc"
local rc_pq_str_to_num = _rc
capture assert `rc_pq_str_to_num' != 0
if (_rc) di as err "FAIL PQ-AUD-005: parqit accepted gen byte b = \"abc\""
local fails = `fails' + (_rc != 0)

if (`fails' == 0) di as txt "VERDICT(PQ_AUD_005_GEN_TYPE_MISMATCH): PASS"
else {
    di as err "VERDICT(PQ_AUD_005_GEN_TYPE_MISMATCH): FAIL (`fails' failures)"
    exit 459
}
```

Expected native result: both commands fail.  
Expected current lazy result from source: both commands can return `rc 0` because mismatched coercion is a no-op.

**Proposed fix:**

Add explicit type-family validation in `View::gen()` after expression translation and before `coerce_storage()`:

```cpp
if (!type_req.empty()) {
    StType mt; int mb = 0;
    if (sttype_parse(type_req, &mt, &mb)) {
        bool target_string = (mt == StType::Str || mt == StType::StrL);
        bool expr_string = (r.kind == 's');
        if (target_string != expr_string)
            return "type mismatch: cannot generate " + type_req + " " + name +
                   " from a " + (expr_string ? "string" : "numeric") + " expression";
    }
}
```

Keep existing `gen str#` truncation and numeric narrow coercion after the validation.

**regression_risk:** low. This changes silently accepted invalid commands into loud errors matching native Stata. Scripts that depended on the bug should fail early.

**perf_impact:** none. This is compile-time validation only.

**🤖 AI Agent Instructions:**

1. Add `tests/verify_suite/v42_gen_type_mismatch.do` from the reproduction above and run `bash tests/run_stata.sh v42_gen_type_mismatch`.
2. Confirm with `parqit explain` that pre-fix commands are accepted and produce mismatched projected columns.
3. Edit `src/engine/view.cpp::View::gen()` to parse `type_req` and reject string/numeric family mismatches before calling `coerce_storage()`.
4. Extend tests to prove valid cases still pass: `gen str3 s = "abcdef"` truncates to `"abc"`; `gen byte b = 3.9` stores `3`; `gen double d = 123` remains valid.
5. Run:
   ```bash
   cmake --preset dev && cmake --build build/dev -j
   ctest --preset dev
   bash tests/run_stata.sh v42_gen_type_mismatch
   bash tests/run_stata.sh
   bash tests/release_lint.sh
   ```
6. Update `CHANGELOG.md`. No `ASSUMPTIONS.md` change is needed.

---

### PQ-AUD-007 — Literal parser accepts impossible calendar dates and 60-second `tc()` times

**Severity:** S3 functional semantic gap  
**Confidence:** medium

**Location:**

- `src/engine/exprtrans.cpp:182-211`, helper `parse_dmy`
- `src/engine/exprtrans.cpp:240-273`, helper `parse_hms`
- `src/engine/exprtrans.cpp:672-712`, date-literal lowering for `td`, `tc`, and `tC`

**What the code does:**

`parse_dmy()` validates only that the day is in `1..31`; it does not validate month length or leap years:

```cpp
// src/engine/exprtrans.cpp:182-211
bool parse_dmy(const std::string &raw, int *dd, int *mm, int *yy) {
    ...
    if (p != t.size() || yd != 4) return false;
    if (d < 1 || d > 31) return false;
    *dd = d;
    *mm = m;
    *yy = y;
    return true;
}
```

The literal lowering then converts the unchecked triple to a day count:

```cpp
// src/engine/exprtrans.cpp:672-675
if (fname == "td") {
    if (!parse_dmy(raw, &d, &m, &y))
        return fail("td(): expected ddmonyyyy, got '" + raw + "'");
    value = stata_days(y, m, d);
}
```

`parse_hms()` allows seconds up to, but not including, `61.0`:

```cpp
// src/engine/exprtrans.cpp:240-273
if (p != t.size() || h > 23 || m > 59 || sec >= 61.0) return false;
*ms_out = static_cast<long long>(((h * 60 + m) * 60) * 1000 +
                                 static_cast<long long>(sec * 1000.0 + 0.5));
```

`tc()` and `tC()` both call this parser and then add milliseconds:

```cpp
// src/engine/exprtrans.cpp:697-712
if (!parse_dmy(dpart, &d, &m, &y) || !parse_hms(tpart, &ms))
    return fail(fname + "(): expected ddmonyyyy hh:mm:ss, got '" + raw + "'");
value = stata_days(y, m, d) * 86400000LL + ms;
/* %tC literals: parqit stores tC as the same count ... */
```

Therefore literals such as `td(31feb2020)` and `tc(01jan2020 00:00:60)` are accepted and normalized by arithmetic rather than rejected or returned as missing.

**What native Stata does:**

Native Stata date/time conversion does not treat impossible calendar dates as ordinary valid dates. A month/day combination such as 31 February should be invalid rather than normalized to a later day. For `%tc`, seconds are ordinary clock seconds; `60` is not a valid `tc()` second. `%tC` leap-second semantics are separately documented by Stata, while parqit’s own comment states that `tC()` is stored as the same count as `tc()` and leap seconds are not added.

The coding agent must confirm the exact native Stata result for literal syntax: whether Stata returns a missing value, emits a syntax/type error, or rejects the expression at parse time. The bug is that parqit currently accepts and silently converts impossible input to a valid numeric date/time count.

**Why it is a bug:**

The date literal parser is a compile-time constant path. Silently rolling impossible dates forward creates wrong dates with no warning. This can affect filters (`keep if d >= td(31feb2020)`), generated date variables, joins on date keys, and saved Parquet date casts.

The confidence is medium only because the exact native Stata return code for date-literal syntax was not run here. The source-side defect is clear: validation is incomplete.

**Reproduction:**

Create `tests/verify_suite/v43_date_literal_validation.do` and have the agent confirm native outcomes before hard-coding the exact assertions.

```stata
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
local fails 0
tempfile t

clear
set obs 1
gen x = 1
parqit save `"`t'_one.parquet"', replace data

* Native oracle: agent must confirm exact rc/missing behaviour in Stata 16+.
clear
set obs 1
capture noisily gen d_bad = td(31feb2020)
local rc_native_d = _rc
local native_d_missing = 0
if (`rc_native_d' == 0) {
    capture assert missing(d_bad)
    local native_d_missing = (_rc == 0)
}

capture noisily gen double c_bad = tc(01jan2020 00:00:60)
local rc_native_c = _rc
local native_c_missing = 0
if (`rc_native_c' == 0) {
    capture assert missing(c_bad)
    local native_c_missing = (_rc == 0)
}

* Native must not accept either as an ordinary nonmissing date/time.
capture assert (`rc_native_d' != 0 | `native_d_missing' == 1) & ///
               (`rc_native_c' != 0 | `native_c_missing' == 1)
if (_rc) di as err "FAIL PQ-AUD-007 oracle setup: native Stata accepted an invalid literal as nonmissing"
local fails = `fails' + (_rc != 0)

* parqit should match native by erroring or producing missing, not a valid count.
parqit use using `"`t'_one.parquet"'
capture noisily parqit gen d_bad = td(31feb2020)
local rc_pq_d = _rc
if (`rc_pq_d' == 0) {
    parqit collect, clear
    capture assert missing(d_bad)
    if (_rc) di as err "FAIL PQ-AUD-007: parqit accepted td(31feb2020) as nonmissing"
    local fails = `fails' + (_rc != 0)
}

parqit use using `"`t'_one.parquet"'
capture noisily parqit gen double c_bad = tc(01jan2020 00:00:60)
local rc_pq_c = _rc
if (`rc_pq_c' == 0) {
    parqit collect, clear
    capture assert missing(c_bad)
    if (_rc) di as err "FAIL PQ-AUD-007: parqit accepted tc(...:60) as nonmissing"
    local fails = `fails' + (_rc != 0)
}

if (`fails' == 0) di as txt "VERDICT(PQ_AUD_007_DATE_LITERAL_VALIDATION): PASS"
else {
    di as err "VERDICT(PQ_AUD_007_DATE_LITERAL_VALIDATION): FAIL (`fails' failures)"
    exit 459
}
```

Expected native result: invalid literals are not ordinary nonmissing dates.  
Expected current parqit result from source: `td(31feb2020)` and `tc(...:60)` can compile to nonmissing numeric constants.

**Proposed fix:**

1. Add a calendar validator to `parse_dmy()`:

```cpp
static bool leap(int y) { return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0); }
static int mdays(int y, int m) {
    static const int d[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    if (m == 2 && leap(y)) return 29;
    return d[m];
}
...
if (d < 1 || d > mdays(y, m)) return false;
```

2. For `tc()`, reject `sec >= 60.0`. For `tC()`, either implement real leap-second validation or reject `sec >= 60.0` consistently with the current documented simplification that `tC()` is treated like `tc()`.
3. Decide after running native Stata whether invalid literals should be parse errors or constant missing. The existing `fail()` pattern suggests parse error is easiest and safest for compile-time literals.

**regression_risk:** low to medium. It can break scripts that used invalid literals and unknowingly relied on rollover arithmetic. That behaviour is not a valid Stata contract.

**perf_impact:** none. This is parser-time validation.

**🤖 AI Agent Instructions:**

1. Add `tests/verify_suite/v43_date_literal_validation.do` from the skeleton above.
2. First run only the native oracle block in Stata 16+ to record the exact return code/missing behaviour for `td(31feb2020)`, `td(29feb2019)`, `td(29feb2020)`, `tc(01jan2020 00:00:60)`, and `tC(01jan2020 00:00:60)`. Update the assertions to match native Stata exactly.
3. Confirm pre-fix parqit accepts at least `td(31feb2020)` as a nonmissing constant by collecting and displaying `%td d_bad`.
4. Edit `src/engine/exprtrans.cpp`: add month-length/leap-year validation to `parse_dmy()` and tighten `parse_hms()` to the native second range. Handle `tC()` explicitly according to the native oracle and the package’s documented no-leap-second assumption.
5. Add C++ unit tests in `tests/unit/test_exprtrans.cpp` for invalid and valid leap-day literals.
6. Run:
   ```bash
   cmake --preset dev && cmake --build build/dev -j
   ctest --preset dev
   bash tests/run_stata.sh v43_date_literal_validation
   bash tests/run_stata.sh
   bash tests/release_lint.sh
   ```
7. Update `CHANGELOG.md`; update `ASSUMPTIONS.md` only if `tC()` remains deliberately simplified.

---

## 5. False positives considered and rejected

| candidate | why it looked risky | refutation from current source/tests |
|---|---|---|
| `open _data` named-view overwrite | Prior repro existed for bridge temp view clobbering user views. | Current ado bridge path uses `c(pid)` plus a monotonically incremented `__parqit_open_seq` to create unique temp filenames/views in `parqit.ado:1357-1363`. I did not re-report. |
| `reshape long` duplicate `i()` silently corrupts | Prior repro existed. | Current `cmd_view_reshape()` checks `count(*)` vs `count(DISTINCT i...)` and errors if `i()` does not uniquely identify wide rows before reshaping (`plugin_view.cpp:1457-1475`). I did not re-report. |
| `reshape wide` duplicate `i(),j()` silently corrupts | Duplicate wide keys are a classic reshape hazard. | Current code checks the long-row count against `count(DISTINCT i,j)` and errors on duplicates (`plugin_view.cpp:1494-1515`). I did not re-report. |
| `misstable summarize` stale `r(n_complete)` | Prior audit/repro targeted complete-count logic. | Current `cmd_view_stats()` misstable branch computes `parqit_n_complete` with a sum over a per-row conjunction of nonmissing checks and returns it through `response.add_scalar()` (`plugin_view.cpp:1746-1794`). I did not re-report. |
| `gen str#` truncation and `gen byte/int/long/float` range handling | These were explicitly listed as already fixed. | `View::gen()` calls `coerce_storage()`, and `coerce_storage()` implements `parqit_substr_bytes` for `str#`, trunc/range guards for integer storage, and `CAST(... AS FLOAT)` for `float` (`view.cpp:28-50`, `282-315`). I reported only the mismatch gap and the missing reuse in `replace`/`egen`. |
| Group-key folding for `collapse`, `contract`, varlist `duplicates`, and `egen, by()` | Missing-key folding was a repeated prior defect class. | Current code uses `norm_group_key()` for collapse by keys, contract keys, varlist duplicates partition keys, and egen by partitions. The no-varlist duplicates bypass is reported separately because it uses raw `SELECT DISTINCT`. |
| `save` over own source / glob containment | Self-overwrite can leave stale or corrupt files. | Current `cmd_view_save()` resolves file existence and checks source paths/globs before writing, including directory/glob containment guards around `plugin_view.cpp:930-984`. I did not re-report. |
| Partitioned write not fully transactional | Prompt lists this as known-deferred. | `ASSUMPTIONS.md:637-641` lists deferred items and the prompt explicitly says not to pad the report with them unless real harm is shown. I did not add a new finding. |
| strL save return codes unchecked | Prompt lists this as known-deferred. | Deferred in `ASSUMPTIONS.md:637-641`; no new independently harmful repro was established in this static pass. |
| `summarize, detail` and multi-percentile performance | Prompt lists these as known-deferred. | I did not benchmark; no new correctness defect was found. |
| SQL-default comparison semantics for missing values | Prompt says SQL-default missing-comparison semantics are intentional unless `parqit set statamissing on`. | I did not propose changing default comparison semantics. PQ-AUD-001 concerns imported IEEE specials that should already have become Stata missing values at the boundary. |
| Unsupported common Stata functions (`sign`, `reldif`, `word`, `regexr`, `proper`, `char`, `uchar`, `autocode`, `recode`, `clip`) | Unsupported functions are tempting to list as gaps. | `exprtrans.cpp` returns a loud “function ... is not supported” error for unknown functions. Because `parqit.sthlp` documents a finite supported function list, I did not report unsupported functions as correctness defects. |
| `merge m:m`, missing keys, `_merge` labels | Prior audit cluster fixed and locked. | Current code has key normalization helpers and prior verify tests (`v25`, `v27`, `v28`, `v33`, `v35`). I did not re-report. |
| `collapse (sum)` all-missing groups | SQL `sum(NULL)` is a common trap. | Current code uses `coalesce(sum(ref), 0)` for collapse sum (`view.cpp:493-495`), matching the prior audit note that all-missing sum should be zero. I did not report. |

---

## 6. Coverage matrix

Legend: `audited-clean` means no new finding survived static refutation in this pass. `finding(s)` links to the finding IDs. `not audited` is used only where I did not trace the code path deeply enough to make a clean statement. Runtime status remains subject to the agent running the verify suites.

### Public subcommands (`src/ado/p/parqit.ado`)

| subcommand | status | notes |
|---|---|---|
| `version` | audited-clean | Dispatch/plugin version path checked statically; no new issue. |
| `selftest` | audited-clean | Plugin selftest path checked at dispatch level; no new issue. |
| `use` | finding(s) | Lazy `use` boundary has PQ-AUD-001 and PQ-AUD-002; eager `use, clear` is the oracle for those findings. |
| `save` | finding(s) | Lazy `view_save` can preserve unnormalized float specials/string NULLs: PQ-AUD-001, PQ-AUD-002. Direct in-memory save path has correct guards. |
| `describe` | audited-clean | Manifest/metadata display path checked; no new issue beyond boundary metadata sources. |
| `glimpse` | audited-clean | Display/introspection path checked statically. |
| `open` | audited-clean | Prior temp-view overwrite repro refuted by unique bridge naming. |
| `close` | audited-clean | View-close routing/resource release checked statically. |
| `keep` | finding(s) | `keep if missing(x)` affected by PQ-AUD-001; `keep in` after string sort affected by PQ-AUD-002. |
| `drop` | audited-clean | Pattern expansion and projection path checked; no new issue. |
| `gen` | finding(s) | PQ-AUD-005; also affected by PQ-AUD-001 when expressions inspect imported specials. |
| `egen` | finding(s) | PQ-AUD-004; by-key missing folding otherwise checked. |
| `replace` | finding(s) | PQ-AUD-003. |
| `rename` | audited-clean | Collision/sanitized-name paths checked statically; no new issue. |
| `order` | audited-clean | Column order/projection path checked; no new issue. |
| `sort` | finding(s) | String NULL ordering divergence: PQ-AUD-002. |
| `gsort` | finding(s) | Same boundary string NULL/float special concerns as sort/filter contexts: PQ-AUD-001, PQ-AUD-002. Prior numeric missing-order repro was not re-reported. |
| `collapse` | finding(s) | Imported float specials can enter aggregates before missing normalization: PQ-AUD-001. Group-key folding otherwise checked. |
| `contract` | finding(s) | Boundary string/float normalization can affect key equality and output sort: PQ-AUD-001, PQ-AUD-002. Contract’s explicit group-key normalization was otherwise checked. |
| `duplicates` | finding(s) | PQ-AUD-006 for no-varlist; varlist branch uses normalization. |
| `sample` | audited-clean | Parsing and deterministic seed request path checked statically; no new issue. |
| `collect` | finding(s) | Final fill normalizes specials, but lazy operations before collect can already be wrong: PQ-AUD-001, PQ-AUD-002. Atomic temp-frame swap checked and not re-reported. |
| `count` | finding(s) | `count if missing(x)` can be wrong for lazy NaN/Inf: PQ-AUD-001. |
| `head` | finding(s) | Depends on lazy sort/current order; affected by PQ-AUD-002. |
| `list` | finding(s) | Display after lazy sort/current order affected by PQ-AUD-002; numeric missing tests by PQ-AUD-001. |
| `show` | audited-clean | Show/explain-like path checked statically. |
| `explain` | audited-clean | Useful for verifying findings; no new issue. |
| `set` | audited-clean | `statamissing`, threads, and path options checked at parser level; no new issue. |
| `merge` | audited-clean | Missing-key and deterministic m:m fixes are covered by existing tests; no new issue found. Boundary payload values can still be affected by PQ-AUD-001/002 before merge, but merge implementation itself was not flagged. |
| `append` | audited-clean | Type-conflict/generate-collision guards were checked against prior tests; no new issue. |
| `joinby` | audited-clean | Missing-key folding and two-table routing checked statically; no new issue. |
| `reshape` | audited-clean | Long/wide uniqueness guards checked; reshape i()/j() key folding remains documented deferred and was not reported. |
| `sql` | finding(s) | Arbitrary SQL result boundary uses `boundary_for()`, so PQ-AUD-001/002 apply. |
| `query` | finding(s) | Same as `sql`: result manifest uses lazy boundary normalization path. |
| `summarize` | finding(s) | Float specials not normalized before lazy stats: PQ-AUD-001. Known detail-scan performance deferred, not re-reported. |
| `tabulate` | finding(s) | Lazy stats over string NULL/float specials affected by PQ-AUD-001/002; single-scan integer rendering prior fix not re-reported. |
| `path` | audited-clean | Plugin path get/set request path checked; no new issue. |
| `view` | audited-clean | View switch/info/list wrappers checked; substantive lazy boundary findings apply to opened views. |
| `views` | audited-clean | Listing path checked statically. |
| `misstable` | finding(s) | Imported IEEE specials not normalized before stats unless boundary fixed: PQ-AUD-001. Prior `r(n_complete)` bug refuted. |
| `levelsof` | finding(s) | String NULL and numeric NaN equivalence can affect lazy distinct levels: PQ-AUD-001, PQ-AUD-002. |
| `ds` | audited-clean | Name/pattern listing path checked. |
| `lookfor` | audited-clean | Metadata/search display path checked. |
| `codebook` | finding(s) | Distinct/missing counts over lazy specials affected by PQ-AUD-001/002; single-scan prior fix not re-reported. |
| `distinct` | finding(s) | Distinct over raw NULL/NaN/string NULL affected by PQ-AUD-001/002 and related to PQ-AUD-006. |
| `tabstat` | finding(s) | Float specials not normalized before stats: PQ-AUD-001; known percentile performance deferred. |
| `correlate` | finding(s) | Lazy stats over IEEE specials affected by PQ-AUD-001. |
| `pwcorr` | finding(s) | Same as correlate. |
| `histogram` | finding(s) | Numeric input domain can include unnormalized specials: PQ-AUD-001. |
| `mergein` | audited-clean | Eager in-memory/native merge bridge path checked; no new issue. |
| `appendin` | audited-clean | Eager in-memory append bridge path checked; no new issue. |

### Expression translator functions and operators (`src/engine/exprtrans.cpp`)

| item | status | notes |
|---|---|---|
| `missing`, `mi` | finding(s) | PQ-AUD-001: numeric path uses `IS NULL`, so lazy IEEE specials are not missing unless boundary fixed. |
| `abs` | finding(s) | Non-finite imported/generated values fall under PQ-AUD-001; no separate function-specific defect proven. |
| `exp` | finding(s) | Overflow-generated non-finite values should be tested under PQ-AUD-001; direct code uses `num1("exp")`. |
| `ln`, `log`, `log10` | finding(s) | Domain guards exist; non-finite input/output boundary still under PQ-AUD-001. |
| `sqrt` | finding(s) | Domain guard exists; non-finite input boundary under PQ-AUD-001. |
| `floor`, `ceil`, `int`, `trunc` | audited-clean | Static lowering checked; no new issue. |
| `round` | audited-clean | Prior tie-to-+∞ fix not re-reported. |
| `mod` | audited-clean | Prior y≤0-to-missing fix not re-reported. |
| `min`, `max` | audited-clean | Lowering checked; prior parity tests cover missing behaviour. No new issue. |
| `cond` | audited-clean | Arity/type-family checks inspected; no new issue. |
| `inrange` | audited-clean | Prior missing-bound semantics not re-reported. |
| `inlist` | audited-clean | Static lowering checked; no new issue. |
| `strlen`, `length`, `ustrlen` | audited-clean | String/numeric type checks inspected; prior numeric-length error fix not re-reported. |
| `upper`, `strupper`, `ustrupper` | audited-clean | ASCII vs Unicode distinction was prior-fixed/documented. |
| `lower`, `strlower`, `ustrlower` | audited-clean | Same as upper. |
| `trim`, `strtrim`, `ltrim`, `rtrim` | audited-clean | Static lowering checked. |
| `substr` | audited-clean | Byte-indexed path and plugin scalar checked; prior tests cover. |
| `strpos` | audited-clean | Empty-needle and byte-indexed prior fixes not re-reported. |
| `subinstr` | audited-clean | Static lowering checked; no new issue. |
| `string`, `strofreal` | audited-clean | `%9.0g` prior fix not re-reported. |
| `real` | audited-clean | Prior `inf`/`nan` to missing fix not re-reported. |
| `regexm` | audited-clean | Supported regexm path checked; unsupported `regexr`/`regexs` are loud, not reported. |
| `year`, `month`, `day`, `quarter`, `dow`, `doy` | audited-clean | Date extraction checked statically; no new issue. |
| `mdy`, `dofm`, `mofd`, `yofd` | audited-clean | Row-local `try()` guards for `mdy`/`dofm` checked; no new issue. |
| Date literals `td` | finding(s) | PQ-AUD-007 for impossible day/month combinations. |
| Date literals `tm`, `tq`, `th`, `tw`, `ty` | audited-clean | Period parser range checks inspected; no new issue in this pass. |
| Date literals `tc`, `tC` | finding(s) | PQ-AUD-007 for second `60` and calendar validation. |
| Operators `+ - * / ^` | finding(s) | Division/power guards checked; imported/generated non-finite values fall under PQ-AUD-001. No separate arithmetic bug proven. |
| Operators `& | !` | audited-clean | Missing-as-true semantics from prior tests not re-reported. |
| Operators `== != < > <= >=` | finding(s) | Comparisons over lazy NaN/Inf depend on PQ-AUD-001; intentional SQL-default missing semantics not challenged. |
| Unary sign | audited-clean | Static parser path checked. |
| Precedence/associativity | audited-clean | Prior power associativity and chained comparison fixes not re-reported. |
| `_n`, `_N` row context | finding(s) | Row context after lazy sort can be wrong when string NULL order is wrong: PQ-AUD-002. Core row-context wrapper otherwise checked. |
| String vs numeric typing | finding(s) | PQ-AUD-005 for `gen` type declarations; expression translator type checks otherwise loud. |
| `statamissing` mode | audited-clean | Mode dispatch checked; PQ-AUD-001 is precondition normalization, not a request to change SQL-default comparison semantics. |
| `sign` | audited-clean | Loud unsupported; not promised by help. |
| `reldif`, `mreldif` | audited-clean | Loud unsupported; not promised by help. |
| `word` | audited-clean | Loud unsupported; not promised by help. |
| `regexr`, `regexs` | audited-clean | Loud unsupported; not promised by help. |
| `proper` | audited-clean | Loud unsupported; not promised by help. |
| `char`, `uchar` | audited-clean | Loud unsupported; not promised by help. |
| `autocode`, `recode`, `clip` | audited-clean | Loud unsupported; not promised by help. |
| `floor/ceil/int` of missing | audited-clean | SQL NULL propagation matches missing; no new issue. |
| `exp/ln` overflow | finding(s) | Treat as PQ-AUD-001 follow-up: expression-generated specials should be normalized before missing tests/save. |
| `min()/max()` ignore missing like Stata | audited-clean | Static check plus prior audit coverage; no new issue. |

### Plugin entry points (`src/plugin/parqit_plugin.cpp` dispatch and handlers)

| entry point | status | notes |
|---|---|---|
| `ping` | audited-clean | Dispatch/response path checked. |
| `echo` | audited-clean | Hex/request echo path checked at high level. |
| `selftest` | audited-clean | Dispatch path checked. |
| `version` | audited-clean | Version response path checked. |
| `use_prepare` | audited-clean | Eager prepare/manifest path checked; lazy boundary issues are in `view_open`. |
| `use_fetch` | audited-clean | Eager fill normalizes float specials; this is the oracle for PQ-AUD-001. |
| `describe` | audited-clean | Manifest/metadata path checked. |
| `save_data` | audited-clean | In-memory save path has finite/string guards; no new issue. |
| `save_data_direct` | audited-clean | Direct unchanged-source fast path has finite/string guards; no new issue. |
| `view_open` | finding(s) | Boundary normalization defects: PQ-AUD-001, PQ-AUD-002. |
| `view_op` | finding(s) | Lazy operation compiler contains PQ-AUD-003, PQ-AUD-004, PQ-AUD-005, PQ-AUD-006. |
| `view_collect_prepare` | finding(s) | Collect final fill normalizes, but value/ordering errors can be baked in before collect; storage metadata cannot rescue PQ-AUD-003/004. |
| `view_save` | finding(s) | Lazy save lacks final normalization: PQ-AUD-001, PQ-AUD-002. |
| `view_info` | audited-clean | View info path checked. |
| `view_close` | audited-clean | Resource release checked statically. |
| `view_switch` | audited-clean | View switching path checked. |
| `view_list` | audited-clean | Listing path checked. |
| `view_twotable` | audited-clean | Merge/append/joinby path checked against prior tests; no new issue. |
| `view_reshape` | audited-clean | Uniqueness guards checked; reshape key-folding remains documented deferred. |
| `view_query` | finding(s) | Query result boundary uses `boundary_for()`: PQ-AUD-001/002. |
| `view_sql` | finding(s) | Same as query. |
| `view_stats` | finding(s) | Stats over unnormalized lazy specials affected by PQ-AUD-001/002. |
| `view_alive` | audited-clean | Liveness path checked. |
| `set` | audited-clean | Options path checked. |
| `path` | audited-clean | Plugin path path checked. |
| `extern "C"` exception boundary | audited-clean | Catch-all boundary inspected at dispatch level; no new issue. |

### Data paths and cross-cutting concerns

| surface | status | notes |
|---|---|---|
| collect vs save | finding(s) | Lazy collect final fill can normalize too late; lazy save can preserve unnormalized values: PQ-AUD-001/002. |
| lazy view vs eager `use, clear` | finding(s) | Core divergence: PQ-AUD-001/002. |
| unchanged-source `save_data_direct` fast path | audited-clean | Has string and finite numeric guards; no new issue. |
| partitioned save | audited-clean | Self-overwrite guards inspected; nontransactionality is known deferred. |
| strL binary sidecar | audited-clean | No new issue beyond known-deferred save return codes. |
| parallel fill (>=50k rows) | audited-clean | Fill normalization logic applies per column; no new race found in static pass. |
| temp-Parquet bridge for `.dta`/`.xls(x)`/`.csv` and `open _data` | audited-clean | Unique bridge naming refutes prior overwrite issue. |
| column manifest / positional vs by-name discipline | audited-clean | Manifest overlay and sanitize provenance checked; no new issue. |
| type map on read | finding(s) | Lazy read boundary lacks float/string normalization: PQ-AUD-001/002. Eager read path checked. |
| type map on save | finding(s) | Lazy save lacks final normalization and storage enforcement after replace/egen: PQ-AUD-001/003/004. |
| variable labels/value labels/notes/formats/characteristics/dataset label | finding(s) | Storage metadata not value-enforced for replace/egen: PQ-AUD-003/004. Other metadata surfaces checked. |
| `parqit.*` metadata namespace | audited-clean | Source-name/value-label roundtrip prior fix not re-reported. |
| hex wire protocol | audited-clean | Request/response line protocol and hex codec tests inspected; no new non-hex field exposure found. |
| sanitizer | audited-clean | Reserved/long/unicode/collision paths covered by unit tests; no new issue. |
| locale independence | audited-clean | `dtoa`/`atod` path checked; no new issue. |
| determinism | finding(s) | Sort/current row order over string NULL diverges: PQ-AUD-002; no-varlist duplicates survivor semantics need normalized deterministic implementation: PQ-AUD-006. |
| resource hygiene | audited-clean | Temp-frame swap and aborted collect/drop paths inspected against prior fixes; no new issue. |
| `r()`/`c()` correctness | audited-clean | Prior `misstable` scalar issue refuted; no new scalar/c macro issue found statically. |
| `c(changed)`, `c(filename)` | audited-clean | Save/use ado paths inspected at high level; no new issue. |

---

## 7. Prioritised remediation plan

### Batch A — Normalize lazy boundary semantics first

Fix **PQ-AUD-001** and **PQ-AUD-002** together in `src/plugin/plugin_view.cpp::boundary_for()`. They share the same architectural cause: lazy views expose foreign SQL semantics instead of Stata dataset semantics. This batch should be first because it reduces the blast radius across `keep`, `sort`, `collapse`, `stats`, `distinct`, `levelsof`, `sql/query`, `collect`, and `save`.

Recommended sequence:

1. Add `v37_lazy_boundary_semantics.do` covering float specials and string NULL.
2. Patch float/double boundary guards and string `coalesce()` boundary guards.
3. Add a defensive lazy-save oracle using pyarrow for both numeric specials and string NULLs.
4. Run C++ and Stata suites.

### Batch B — Enforce storage semantics in mutating verbs

Fix **PQ-AUD-003**, **PQ-AUD-004**, and **PQ-AUD-005** in `src/engine/view.cpp`. These are all storage/type-contract defects around `coerce_storage()`.

Recommended sequence:

1. First add type-family validation to `gen` (PQ-AUD-005). It is compile-time and low risk.
2. Reuse `coerce_storage()` in `replace` (PQ-AUD-003), with careful handling of conditional `replace`.
3. Reuse `coerce_storage()` and reject string storage in `egen` (PQ-AUD-004).
4. Add one combined `v39_v40_v42_storage_semantics.do` or three small verify files. A combined file is efficient because it can reuse one source Parquet.
5. Run all prior `v33`/`v35` storage tests to confirm no regression.

### Batch C — Replace raw SQL distinctness where Stata equality is required

Fix **PQ-AUD-006** after Batch A, because boundary normalization may simplify the SQL. The preferred design is still to avoid raw `SELECT DISTINCT` for no-varlist `duplicates drop`, since Stata equality is not identical to SQL equality.

Recommended sequence:

1. Add `v41_duplicates_no_varlist_missing.do`.
2. Implement normalized partitioning or normalized distinctness over all columns.
3. Measure the wide-row performance impact. If it is material, add a fast path that uses raw `DISTINCT` only when all columns are known already normalized and not float/double nullable; otherwise use normalized equality.
4. Document deterministic survivor order if exact current-row order cannot be represented without a prior sort.

### Batch D — Validate date/time literals

Fix **PQ-AUD-007** last because it is medium-confidence until native Stata return codes are confirmed. The fix is parser-only and should not affect performance.

Recommended sequence:

1. Run native Stata oracle probes for invalid `td`, `tc`, and `tC` literals.
2. Add month-length/leap-year validation to `parse_dmy()`.
3. Tighten seconds validation in `parse_hms()` according to the oracle and the package’s `tC` simplification.
4. Add both C++ `test_exprtrans.cpp` cases and a Stata verify test.

### Release checklist after remediation

After all batches:

```bash
cmake --preset dev && cmake --build build/dev -j
ctest --preset dev
bash tests/run_stata.sh
bash tests/release_lint.sh
```

Then update:

- `CHANGELOG.md` with each `PQ-AUD-*` fix and the verify-suite names.
- `ASSUMPTIONS.md` only for intentional survivor-order or `%tC` simplification details.
- `src/ado/p/parqit.sthlp` if user-facing semantics become more explicit.
- The four version surfaces only if preparing a new release.
