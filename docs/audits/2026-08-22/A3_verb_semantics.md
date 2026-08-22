# A3 — Lazy verb & expression semantics vs native Stata (adversarial audit, 2026-08-22)

Scope: `src/engine/view.cpp` verb→SQL compiler, `src/engine/exprtrans.cpp` translator and the
read-only stats verbs, exercised through `parqit use using … ; parqit <verb> … ; parqit collect, clear`
and compared cell-for-cell against native Stata run on the same data in the same process.

Environment: StataNow/MP 19.5 (Linux), parqit build of today in `ado/plus/p/` (`*! version 0.1.27
9aug2026` + today's unreleased NAME-CASE-1/ENC-2 changes; HEAD ddc7140), DuckDB 1.5.3 embedded.

Method: nine self-contained do-files under `local/audit_2026-08-22/A3/` (`A3_01_expr.do` …
`A3_09_alias_keepif.do`, plus `A3_10_real_probe.do`), each generating its data with `set seed`,
saving the native result (`save …dta`) and the Parquet source (`parqit save …, replace data`), then
`cf _all using` / `assert` / `reldif`-tolerance comparisons with explicit `PASS/FAIL <id>` lines
(logs `A3_0*.log`, consolidated in `A3/all_results.txt`: 285 PASS, 80 FAIL lines, 66 INFO lines).
Every FAIL was inspected and classified below as (a) a defect, (b) a documented/contractual
difference, or (c) a test artefact. ~200 individual checks over ~60 hypotheses.

No tracked file was modified; all artefacts live in `local/audit_2026-08-22/A3/`.

---

## Findings

### A3-1 — S0 — `merge` leaves every common non-key variable **missing on using-only rows** (`_merge==2`); native fills them from the using data

* Repro: `A3_08_followups.do` §1 (also `A3_04_twotable.do` tests 1a/2/3b). Master `(k c sc a)`,
  using `(k c sc b)`; `c`/`sc` exist on both sides.
  ```
  parqit use using A3_08_m.parquet
  parqit merge 1:1 k using A3_08_u.parquet        // default keep(), keepusing()
  parqit sort k
  parqit collect, clear
  ```
* Observed (parqit) vs expected (native `merge 1:1 k using A3_08_u.dta`):
  ```
  k=4  parqit:  c=.    sc=""   a=.  b=40  _merge=2
  k=4  native:  c=444  sc="ud" a=.  b=40  _merge=2
  ```
  Matched rows correctly keep the MASTER value (including a master missing — `A3-08-3` PASS);
  master-only rows are right; only `_merge==2` rows lose the using values of common columns.
  Fails identically with `keep(using)`, `keep(match using)`, `keepusing(c sc)`,
  `keep(using) keepusing(c)`, and in the `m:1` direction (`A3-08-2`); passes with `keepusing(b)`
  because native then drops using's `c` as well. A lazy `parqit save` of the same view writes the same
  (wrong) payload since it is the same plan. rc 0, only the existing "master values kept" note.
* Native Stata: for `_merge==2` observations every kept using variable, common or not, carries the
  using value ([D] merge; verified live).
* Regression from today: no (projection logic predates today's changes).
* Fix: `src/engine/view.cpp` `View::merge_with`, output projection (`for (const auto &c : cols_)` …
  `sel += "__m." + quote_ident(c.name)`): for a non-key master column that also exists in `u.cols`
  **and** is in the kept-using set (`keepusing` empty or matched), emit
  `CASE WHEN __m.<mm> IS NULL THEN __u.c ELSE __m.c END AS c` (the `seq` branch likewise). Pin with
  a verify test mirroring `A3_08` §1 (all keep()/keepusing() variants, string + numeric common vars,
  master-missing-on-match stays missing).

### A3-2 — S0 — `real()` accepts DuckDB digit-group underscores and rejects Stata's `d` exponent

* Repro: `A3_10_real_probe.do`, `A3_07_types_sql_misc.do` §6, `A3_01` expr 26.
  ```
  parqit gen double p = real(s)        // s = "2019_01", "12_345_678", "1_000.5", "1d3", "1.5d2"
  ```
* Observed vs expected (native `real()`):
  | s | parqit | native |
  |---|---|---|
  | `2019_01` | 201901 | . |
  | `12_345_678` | 12345678 | . |
  | `1_000.5` | 1000.5 | . |
  | `1e1_0` | 1e10 | . |
  | `1d3` / `1D3` | . | 1000 |
  | `1d-3` / `1.5d2` | . | .001 / 150 |
  All other probed forms agree (" 2 ", "+3", ".5", "5.", "1e", "0x1A", "1,5", "inf", "nan", "1e400",
  "1,000", "$5", "--1", "1.5.5", full-width digits …). Realistic hazard: period/ID strings such as
  `2019_01` silently become numbers where Stata yields missing. Help says "real() returns missing for
  invalid … text" — `2019_01` is invalid in Stata.
* Regression from today: no.
* Fix: `src/engine/exprtrans.cpp` `real()` — validate the trimmed text against Stata's literal grammar
  before `TRY_CAST` (e.g. `regexp_matches(trim(s), '^[+-]?([0-9]+\.?[0-9]*|\.[0-9]+)([eEdD][+-]?[0-9]+)?$')`,
  with `translate(…,'dD','ee')` applied for the cast); unit-test in `tests/unit` translator suite.

### A3-3 — S0 (narrow domain) — date functions return values outside Stata's date domain where native returns missing

* Repro: `A3_07_types_sql_misc.do` §4, `A3_01` exprs 48–57.
  | expression | input | parqit | native |
  |---|---|---|---|
  | `year(d)` (also month/day/dow/doy/quarter/mofd/yofd) | d = 2936550 (01jan10000) … ~2.146e9; d = -679351 | 10000 … (values) | . |
  | `year(d)` | d = 3000000 | 10173 | . |
  | `mdy(m,d,y)` | y = 99, 0, -1, 10000 (A3_01 row 1/9/10/4; A3_07 rows 2/4) | -679715, -715875, -716240, 2936550 (mdy(6,15,99) = -679550, mdy(6,15,10000) = 2936716) | . |
  | `dofm(m)` | m = 96480 (jan 10000), -22321, -1234567, 123456.79 | 2936550, -679381, -37576362, 3757615 | . |
  Day counts that do not fit INT32 (1e10, 1e15) correctly give missing (the `try(CAST … AS INTEGER)`);
  the gap is the band between Stata's ceiling (31dec9999 = 2936549 / 01jan0100 = -679350) and
  DuckDB's DATE range. Exposure: corrupted or mis-scaled counts (e.g. a `%tm`/`%tc` value fed to a
  `%td` function) silently produce a plausible-looking year instead of `.`.
* Regression from today: no.
* Fix: `exprtrans.cpp` `day_arg()` → `CASE WHEN floor(x) BETWEEN -679350 AND 2936549 THEN DATE
  '1960-01-01' + CAST(floor(x) AS INTEGER) END`; `mdy()` → guard `floor(y) BETWEEN 100 AND 9999`;
  `dofm()` → guard the month count to [-22320, 96479] (= 01jan0100 … 31dec9999). The same guard
  closes A3-4.

### A3-4 — S1 — a day count just below 2^31 aborts the whole `collect` (`r(920)` "Date out of range") instead of row-local missing

* Repro: `A3_07_types_sql_misc.do` §4z: `d = 2147483647` (also 2147483648 / -2147483648 cast fine);
  `parqit gen double y = year(d)`; `parqit collect, clear` →
  `parqit collect: Out of Range Error: Date out of range  r(920)`.
* Native: `year(2147483647)` = `.`, rc 0. Help promises "an out-of-range argument is row-local
  missing". `try()` wraps only the INTEGER cast, not the `DATE + INTEGER` addition.
* Regression from today: no. Fix: as A3-3 (range guard), or wrap the whole date expression in `try()`.

### A3-5 — S1 — `parqit levelsof` of a numeric variable literally named `v` returns lexicographic (text) order

* Repro: `A3_09_alias_keepif.do` §b. `v` = {0.1, 0.5, 1.5, 2, 1e-7, 123456789.123}.
  `parqit levelsof v` → `r(levels)` = `0.1 0.5 1.5 123456789.123 1e-07 2` (string order);
  the same values in a column named `w` → `1e-07 0.1 0.5 1.5 2 123456789.123` (correct).
  Native: `1.00000000000e-07 .1 .5 1.5 2 123456789.123`.
* Cause: `src/plugin/plugin_view.cpp` (levelsof branch): `SELECT DISTINCT <render(v)> AS v FROM … ORDER BY "v"`
  — DuckDB binds the bare `ORDER BY` identifier to the SELECT alias `v` (the VARCHAR rendering) rather
  than the source column; string columns are unaffected (same ordering), `tabulate` is unaffected
  (it orders by a normalising CASE expression). rc 0, `r(r)` right, order wrong — a `foreach l of
  local levels` loop that assumes ascending levels silently misbehaves.
* Regression from today: no.
* Fix: alias the rendered value with a reserved helper name (`AS __parqit_lvl`) or `ORDER BY` the
  source expression, and add `v`/`n`-named columns to a verify test (v66/v67 family).

### A3-6 — S2 — `levelsof`/`tabulate` render non-integer numbers differently from native

* Repro: `A3_06_stats.do` 5c, `A3_09` 2d, `A3_07` §8. parqit `0.30000000000000004 1.5 2`,
  `1e-07 0.1 0.5 …`; native `.3 1.5 2`, `1.00000000000e-07 .1 .5 …` (native levelsof puts
  non-integers through macro formatting; `tabulate` uses the variable's display format). Integers,
  strings (compound-quoted, `""` excluded) and counts all match. Text-only contract gap; numerically
  equivalent for `if x == \`l'` loops, but `r(levels)` string comparisons and displays differ.
* Fix: `plugin_view.cpp` `stata_num_varchar()` — render non-integers with a Stata-style general
  format (strip the leading zero, `%g`-like mantissa), or document the DuckDB shortest-round-trip text.

### A3-7 — S2 — `string()`/`strofreal()` of a denormal double renders `infe-324`

* Repro: `A3_07_types_sql_misc.do` §7: `string(x)` for x = 5e-324 → parqit `infe-324`, native
  `4.9e-324`; x = 1e-323 → `infe-324` vs `9.9e-324`; 1e-310 and 1e-308 agree.
* Cause: `src/engine/session.cpp` `format_sci()`: `mant = a / std::pow(10.0, exp)` with exp = -324
  underflows `pow` to 0 → mantissa = inf. Tiny domain (denormals below ~1e-323) but rc 0 and a
  visibly wrong string. Fix: scale in two steps (`a * 1e300 / pow(10, exp+300)`) or use `%.*e`
  formatting then re-trim to Stata's width rule.

### A3-8 — S2 — collapse/merge/reshape result metadata differs from native (labels, formats, storage type)

* `collapse` (A3_03 test 6): native labels `(mean) v`, `(sum) v`, `(count) v`, `(max) bv`,
  `(first) hs` and keeps the source display format (`%9.2f`); parqit: no variable label, `%10.0g`
  for mean/sum, `(count)` collected as `byte` (native `long`). Values all agree (tol 1e-12).
* `merge`: `_merge` format `%8.0g` (native `%23.0g`); values/label/value-label match.
* `reshape wide` (A3_05 4b): parqit labels `inc1`/`inc2` with the stub's label ("income"); native
  `1 inc`, `2 inc`. `reshape long` keeps the first wide column's format (`%9.2f`) where native resets
  to `%10.0g` — arguably better, noted for completeness.
* Not promised by the help; listed so the contract can be stated or the metadata aligned (fix
  location `view.cpp` `collapse()`/`merge_with()`/`reshape_wide()` ViewCol decoration).

### A3-9 — S2 — `keep in f/l` letter tokens not accepted

* `parqit keep in 50/l` → r(198) (loud) where native accepts `l`/`f`. Help documents only numeric
  `f/l`; worth one sentence ("numeric bounds only") or a tiny ado change (`_parqit_op_keepin`).

---

## Documented / contractual differences confirmed (not defects — listed so they are not re-reported)

* SQL-mode (`statamissing off`) comparisons with a missing operand yield missing: `x > 5`, `x >= y`,
  `x == y`, `x != y`, `!(x > 5)`, `(x > 5) + (y > 5)`, `cond(x > 5, 1, 0)` → 0 (native 1),
  `replace … if x > 100` leaves a missing `x` untouched (native changes it) — exactly as the help's
  "Missing-value semantics" paragraph states; every one of them matches native under
  `parqit set statamissing on` (A3_01 mode=on all PASS for these). Note for the docs: the sentence
  "three-argument cond() treats a missing numeric condition as true" does not apply to a *comparison*
  condition in SQL mode (`cond(x > 5, a, b)` → `b`); the adjacent sentence about comparison operands
  covers it, but a reader may not connect the two.
* Untyped `gen`/`egen` results are `double` (native `float`): `egen m = mean(v)` differs in the 8th
  digit (A3-03-5); documented in help (#54).
* `substr()` slices that split a UTF-8 code point return U+FFFD (native raw bytes) — documented (#44b).
* `^`, `log10()`, `mod()` with huge operands differ from native only at ULP level (max reldif
  2.1e-15 over 300 random `x^e`; `sqrt`/`exp`/`ln` exact) — libm noise, not a defect.
* `collapse (count)` on a string is a documented parqit extension (native r(109)).
* Type-mismatch expressions (`h == 1`, `inlist(h,1,2)`, `v + h`) refuse with r(198) (native r(109)).
* `correlate`/`pwcorr` return only `r(rho)`/`r(N)` (documented) — my `r(C)` checks were test artefacts.

---

## PASS/FAIL table — all hypotheses tested

Legend: PASS = identical to native; DOC = differs only as documented; DEF = defect (finding id);
ART = test artefact (re-run confirmed in a later file).

| Area | Hypothesis / experiment (file: id) | Result |
|---|---|---|
| Expressions | literal missing idioms `x < .`, `. < x`, `. <= x` (A3_01: 5–7) both modes | PASS |
| Expressions | `x > 5`, `x >= y`, `x == y`, `x != y`, `!(x>5)`, `(x>5)+(y>5)`, `cond(x>5,…)` SQL mode (1–4,10,11,61,62) | DOC |
| Expressions | same seven under `statamissing on` | PASS |
| Expressions | `max(x,y)`, `min(x,y)`, `min(x,2.5)`, `max(x,y,3)` with column missings (8,9,94,95) | PASS |
| Expressions | `inlist` numeric/string, `inrange` numeric/string (12,13,69,70) | PASS |
| Expressions | `round(x,0.1/5/0.01/3)`, `round(x)` (14–17,87) | PASS |
| Expressions | `mod(x,3)` incl. negatives (18); `mod(1e20,0.3)` (19) | PASS / float noise |
| Expressions | `x^2`, `0^x`, `x^-1` (20,22,23); `x^0.5`, `2^x` (21,24) | PASS / ULP (A3_08-4 max 2.1e-15) |
| Expressions | `exp`, `sqrt`, `ln`, `abs(x)-1`, `x/y` (y=0, .), `int(x/3)`, `x*x` (63–65,90–92) | PASS |
| Expressions | `string(x)` ordinary values (25, A3_07-7 rows 3–5,8–10) | PASS |
| Expressions | `string(x)` denormals 5e-324, 1e-323 | DEF A3-7 |
| Expressions | `real(s)` 30+ literal forms (26, A3_07-6, A3_10) | DEF A3-2 (`_` groups, `d` exponent); others PASS |
| Strings | `substr` 0/neg/beyond-length/negative n/fractional pos (27–34) | PASS (28,33,34 split-codepoint → DOC) |
| Strings | `strpos`, `strlen`, `ustrlen`, `length`, `trim/ltrim/rtrim`, `subinstr`, `s+"x"`, `upper+lower` (35–40,72–74,96) | PASS |
| Strings | ordering `s < "b"`, `s > "z"`, `s == "ABC"`, `s != ""`, `mi(s)` (41–43,66,77) | PASS |
| Strings | `regexm` anchors/classes/groups/`[0-9]+$`/`^a b$` (44–47,97) | PASS |
| Dates | `year month day dow doy quarter mofd yofd` on in-range, negative, fractional counts (48–55) | PASS in range |
| Dates | same beyond 31dec9999 / before 01jan0100; `dofm`, `mdy` out of Stata domain (56,57; A3_07-4) | DEF A3-3 |
| Dates | day count 2147483647 → whole collect aborts r(920) (A3_07-4z) | DEF A3-4 |
| Dates | literals `td tm tq th tw tc` incl. negative periods, 29feb2000, 0100, 9999, `tc(… 09:30)` (78–89) | PASS |
| Logic | `!x`, `x & y`, `x | y`, `cond(x,1,0)`, `cond(missing(x),1,0)`, `missing(x,y)` (58–60,67,68,76,93) | PASS |
| Typed gen | `gen byte/int/long/float/double/str3/strL` values + storage types (A3_07-1) | PASS |
| Typed gen | untyped gen double vs native float (A3_07-1u) | DOC |
| replace | byte→int and str3→str6 promotion, `replace if` (A3_07-2) | PASS / DOC (missing x under SQL mode) |
| replace | `replace … if _n == 1` refused (A3_07-2a) | DOC (loud) |
| Rows | `sort x id` missing last; `gsort -x +id` missing last (native default) (A3_02-1,2) | PASS |
| Rows | `gsort -w` + `keep in 1/5`; string sort byte order incl. ""/accents/case; `gsort -s` (3–5) | PASS |
| Rows | `_n`/`_N` in gen and keep if after sort (6) | PASS |
| Rows | `keep in 7`; `keep in 50/61` loud; `keep in 50/l` | PASS / PASS / DEF A3-9 (S2) |
| Rows | `duplicates drop` all vars with ""/missing; by-varlist first-in-order (8,9) | PASS |
| Rows | `sample 10` count = native (6 of 60); `sample 7, count`; seed reproducible (10a–c) | PASS |
| Rows | `contract g h` with missing/empty keys, `_freq`, order (11) | PASS |
| Rows | `collapse (first)(last)(firstnm)(lastnm)` over `sort g id` (12) | PASS |
| Rows | `list in 3/5`, `list if`; sort-then-drop-key order; replace of a sort key then keep in (13–15) | PASS |
| Rows | label/format survive sort+keep+collect (16) | PASS |
| collapse | mean sum sd count min max median p1 p10 p25 p33 p50 p75 p90 p99 by(g hs) with missing/empty keys, all-missing group, n=1 (A3_03-1,2,3) | PASS (tol 1e-12) |
| collapse | `(count)` on string (extension); result labels/formats/types | DOC / DEF A3-8 (S2) |
| egen | total mean sd min max count by(g hs), no-by, `count(v2*2)`, `count(1)` (A3_03-5, A3_08-5) | PASS |
| egen | untyped `egen mean` float vs double | DOC |
| merge | 1:1 numeric key with missing keys, `_merge` values/labels, using labels/formats (A3_04-1) | DEF A3-1 (common vars on `_merge==2`), rest PASS |
| merge | 1:1 on string key with "" keys (A3_04-2) | DEF A3-1 |
| merge | keep(match) keepusing(b) gen(mg); keep(1 3) (3a,3c) | PASS |
| merge | keep(master using) nogenerate (3b); keep(using) etc. (A3_08-1) | DEF A3-1 |
| merge | m:1 / 1:m with duplicates and missing keys (4a,4b) | PASS |
| merge | master not unique in 1:1 refused; common non-key var keeps master on matches; float vs double 0.1 keys (5,8,9, A3_08-3) | PASS |
| append | byte+long, float+double, str5+str10, strL+str20, missing-only cols, generate(), value-label conflict keeps master (A3_04-6a/6b) | PASS |
| append | string/numeric conflict refused (6c) | PASS |
| joinby | cartesian within key incl. missing keys (7) | PASS |
| reshape | long with suffix gap + missing cells; string j; duplicate i refused (A3_05-1,2,3) | PASS |
| reshape | wide unbalanced cells; non-unique (i,j), j with space, missing j refused; j = 2 10 11 order (4–8) | PASS (labels: A3-8 S2) |
| pivot | (sum)(mean)(count) rows(region) cols(q) = native collapse+reshape wide (9) | PASS |
| stats | summarize N/mean/sd/min/max; detail N..p99 incl. skewness/kurtosis; all-missing var (A3_06-1,2) | PASS |
| stats | tabulate one-way/two-way with/without missing, string levels (3,4) | PASS |
| stats | levelsof numeric integers, strings with quotes/spaces, `""` excluded (5a,5b,5d, A3_09) | PASS |
| stats | levelsof non-integer text format (5c, A3_07-8, A3_09-2d) | DEF A3-6 (S2) |
| stats | levelsof numeric variable named `v` → lexicographic order (A3_09-2a) | DEF A3-5 (S1) |
| stats | tabulate of variables named `v`/`n` (alias collision) order/counts (A3_09-2e,3b) | PASS |
| stats | correlate/pwcorr rho & N (6a–c); r(C) absent (6d–f) | PASS / ART (documented r(rho) only) |
| stats | misstable n_complete; count if missing(x), `s == ""`, `x > 20` both modes (7a–e) | PASS |
| stats | tabstat/codebook/distinct/histogram printed and spot-checked | PASS (no discrepancy seen) |
| sql/query | `parqit sql` NULL numeric/string; case-clashing aliases `a`/`A` (today's NAME-CASE-1) collect `a A` (A3_07-3a/3b) | PASS |
| sql/query | `parqit query WHERE x > 100` (3c) | PASS |
| keep if | ten string conditions (`s > "a"`, `s == ""`, inlist, `upper(s)=="B"`, `s < "é"` …) (A3_09-1) | PASS |
| type errors | `h == 1`, `inlist(h,1,2)`, `v + h` refused loudly (A3_08-6) | PASS (rc 198 vs native 109) |

Counts: 9 findings — S0: 3 (A3-1, A3-2, A3-3), S1: 2 (A3-4, A3-5), S2: 4 (A3-6, A3-7, A3-8, A3-9), S3: 0.
None is a regression from today's changes.
