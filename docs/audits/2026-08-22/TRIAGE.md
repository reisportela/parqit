# Audit 2026-08-22 — triage (orchestrator)

Status legend: NEW (reported) · CONFIRMED (reproduced by orchestrator) · FIX-SPEC (ready for implementer) · FIXED (implemented) · CERTIFIED (re-verified after fix)

## Orchestrator's own findings (static review of today's NAME-CASE-1/ENC-2 code)

### R1 — S1 — `rename` of an aliased column keeps the stale Stata-facing name
Repro: scratchpad/probe_rename_alias.do (section R1). Dataset with `nuemp`,`NUEMP`,`x` saved; `parqit use`; `parqit rename NUEMP_1 foo`; `parqit collect, clear` → variables are `nuemp NUEMP x` (expected `nuemp foo x`). Cause: `View::rename` / `View::rename_many` set `ViewCol.name` but leave `ViewCol.stata` ("NUEMP"), and collect/save/describe expose `stata` when set. Also affects view `save` (file would carry `NUEMP`, KV too) and `describe`.
Fix spec: in `View::rename` and `View::rename_many`, (a) clear `stata` on every renamed column (the user's new name IS the Stata-facing name; it was already ci-guarded); (b) the characteristics/notes of an aliased column live in `chars_` under the EXPOSED name (`NUEMP`, set at open/collect), not under the engine name — both rename paths currently move `chars_[oldn]` keyed by the engine name, so an aliased column's notes/chars would be orphaned and dropped on collect/save: move the entry keyed by the old `exposed()` (and, for safety, any entry keyed by the old engine name) to `newn`. Add v70 checks: rename alias → collect/save/describe show the new name AND the column's notes/chars survive; rename of a non-aliased column unaffected.
Status: CONFIRMED → FIX-SPEC

### R2 — S2 — `collect` after any verb stamps a spurious `src_name` characteristic on aliased columns
Repro: probe_rename_alias.do (section R2). `parqit use`; `parqit keep nuemp NUEMP_1`; `parqit gen z = nuemp + 1`; `parqit collect, clear` → `char NUEMP[src_name] == "NUEMP_1"` (the engine alias; should be absent — the true file name equals the Stata name). Cause: non-direct collect plans over the temp table, so `ctx.parquet_names` is empty and `write_var_records` emits `original = source_name` (= alias) while `p.stata_name` was overridden to `vc.stata`; the ado then sets `src_name` because they differ. Effects: wrong metadata; a later `parqit save` of that dataset sends `source = NUEMP_1` (only disables the direct fast path — KV `src` uses the name, so the file is right).
Fix spec: in `cmd_view_collect_prepare`, when overriding `p.stata_name = vc.stata`, also set `ctx.parquet_names[p.source_name] = vc.stata` so the var record's `original` equals the Stata name (no char); for the direct-read case the map already holds the true name. Test in v70: after verbs + collect, `char NUEMP[src_name]` is empty; sanitised names (e.g. foreign `a b`) still get their src_name.
Status: CONFIRMED → FIX-SPEC

### R3 — S2 — lazy `parqit use <varlist> using f` does not accept the exact Stata name of an aliased column while eager `use, clear` does
Observation (by code reading): eager `parqit use NUEMP using f, clear` selects by `plans[i].stata_name` (now "NUEMP"); lazy `parqit use NUEMP using f` → `keep_vars` → `expand_patterns` matches `ViewCol.name` (alias `NUEMP_1`) → "variable NUEMP not found in the view". Same asymmetry for `parqit keep/drop/order NUEMP` in a view. Not a regression from before today (the alias was also required then), but now inconsistent with the eager path and with the documented "collect/save translate the alias back".
Fix spec (safe, contained): in `View::expand_patterns`, also match a pattern against `ViewCol::exposed()` and return the column's ENGINE name (`name`) — every caller (keep/drop/order/open-varlist/collapse by/contract by/duplicates by/reshape stubs …) then keeps building SQL from engine names. Do NOT touch `col_index` (exact on engine names) nor verbs that quote the raw user token (sort/rename/gen/expressions) — those keep requiring the alias, as documented. Document: "selection varlists accept either the alias or the exact name". Test in v70.
Status: CONFIRMED (by reading) → FIX-SPEC (verify with a repro in the fix PR)

## Round 2 — from the independent verifier (VERIFY_REPORT.md): 35 CLOSED incl. every S0; the items below remain
- V2.2 — S1 — `relaxed` (union_by_name) over files with differing schemas still loads `NUEMP_1` silently and drops its metadata; help claims recovery works. Fix: implement the exact-name recovery for relaxed mode (per-file leaf names from parquet_schema, union order = first file's columns then new columns of later files) OR, if not contained, make it LOUD (note: "exact-name recovery is not available in relaxed mode; case-clashing columns carry the engine's dedup name") and correct the help. Test (v70/v74 extension: relaxed glob with clash). Status: FIX-SPEC.
- V2.3 — S2 — `float %tc` comes back `double` although the help promises the recorded type when values fit. Fix: restore FLOAT for a %tc manifest type float (eager and lazy) unless observed values cannot fit; test. Status: FIX-SPEC.
- V2.4 — S2 — unattached value labels survive a memory save but a VIEW save drops them (view_kv_fragment writes only used labels). Fix: write every value label the view carries (vallabs_), used or not; test. Status: FIX-SPEC.
- A2-15(1) — S2 — an empty Parquet column name loads as DuckDB's `C1` with no src_name; help says `v<position>`. Fix: recover empty raw names positionally (parquet_schema leaf "" → sanitised `v<pos>`, src_name ""), eager and lazy; test. Status: FIX-SPEC.
- V2.1 — S2 docs — `copysource` verifies identity + names/kinds/N/sortedby + first/last 64 rows only; a Mata edit in the middle rows is not detected (by design — the user asserts nothing changed). Fix: make the help/CHANGELOG/ASSUMPTIONS wording exact (no "never a silent copy" overclaim; state the 64-row sampling and that the copied file is the source's content with the source's sortedby). Status: FIX-SPEC (docs).
- V2.5 — S2 docs — torn-read guard (A4-3), copysource 64-row limit, reshape-wide refusal of case-clashing generated names: document in help (Materialisers/Limitations/reshape) and README. Status: FIX-SPEC (docs).
- V2.6 — S2 — foreign Hive tree whose partition key clashes only by case with a file column (`G` in the file, `g=` in the path) silently loses the file column. Fix: detect in plan_columns (hive_columns vs leaf names, ASCII-ci) and refuse loudly with a clear message (or note + alias if cheap); test. Status: FIX-SPEC.
- V2.7 — S3 — raw engine messages: missing file on `parqit use` (IO Error + SQL snippet), `partition_by()` naming every variable ("Not implemented"), a binder error under races → parqit messages. Status: FIX-SPEC (messages).

## Auditor findings (A1–A6) — to be triaged as reports arrive

### A1 (type/value fidelity) — report: A1_types_values.md (≈395 checks; no regression from today)

#### A1-1 / A1-11 — S0 — `%tc` instants beyond year ~4253 are written with sub-ms error by BOTH in-memory writers (`double us = ms * 1000.0` then llround: for |µs| ≥ 2^56 the product is not representable; ~25–38 % of year-5000/9999 instants read back 1 ms earlier; on-disk µs off even when read back right)
Fix spec: `convert_save_numeric` case WTs (plugin_io.cpp): bound |ms| (≤ 2^53 and ≤ INT64_MAX/1000), then `int64_t us = static_cast<int64_t>(llround(ms)) * 1000;` in integer arithmetic (keep the existing DT-001 ceiling guard); the lazy `compile_for_save` path is already exact (epoch_ms) — keep both identical. Tests: unit (convert at year 5000/9999 instants exact), verify: 20k random ms instants in 4253–9999 round-trip exactly (eager+lazy+staged), pyarrow shows µs == ms*1000.
Status: NEW (auditor arithmetic is right) → FIX-SPEC

#### A1-2 — S0 — same as A4-1 (fast path ignores memory order) — folded into A4-1.

#### A1-3 — S0 — a `float`/`double`/`%tc` partition key comes back as a STRING (`"2020.0"`) on eager, lazy and view-save reads
Fix spec: when the manifest (`parqit.schema`) names a partition column with a numeric/date/time type, cast it back on read: pass `hive_types={col: DOUBLE|FLOAT|BIGINT|TIMESTAMP…}` to `read_parquet` in `source_for`/`strict_schema_gate` (DuckDB option) or apply an explicit boundary CAST from the manifest type (apply_meta_type must cover partition columns); for FOREIGN trees without a manifest keep DuckDB's autocast but document. Also: if a key type cannot round-trip exactly through a Hive path value (e.g. float with fractional part formatting), refuse `partition_by` loudly for that type rather than silently changing it. Tests: float/double/%tc/%td/int/string keys × eager/lazy/view-save, type signature + cf.
Status: NEW → FIX-SPEC

#### A1-4 — S1 — a variable legally named `str` is renamed `_str` on every read (sanitiser over-reserves "str"; native `gen int str = 1` is rc 0)
Fix spec: remove "str" from `kReserved` in sanitize.cpp (keep `strL` and the `str#` family); unit test + round-trip test.
Status: NEW → FIX-SPEC

#### A1-5 — S1 — zero-row `partition_by()` save fails with a raw engine IO error rc 920
Fix spec: in the partitioned branch treat "0 rows" explicitly: write an EMPTY tree (the directory + one 0-row file carrying the full schema so a later read returns 0 obs with all variables) — mirrors `save` of an empty dataset; if that cannot be made verifiable, refuse with a parqit message ("partition_by() needs at least one observation"). Never a raw engine error. Test.
Status: NEW → FIX-SPEC

#### A1-6..A1-10 — S2 — round-trip fidelity gaps
A1-6 storage-type drift: `int/byte %td` → `long` on eager but `int` on lazy; `float/long/int %tc` → `double` — the recorded manifest type must be restored identically on eager and lazy when it can hold the data (apply the manifest type for DATE/TIMESTAMP columns too; widen only if observed values require it); A1-7 `floor(x+0.5)` bumps exact odd integers ≥ 2^52 (+1) and flags them "fractional" — use a rounding that is exact for integers beyond 2^52 (e.g. `CASE WHEN abs(x) >= 2^52 THEN x ELSE floor(x+0.5) END`, both writers and compile_for_save, and the fractional note must not fire for exact integers); A1-8 partition columns move to the end of the variable list on read-back — restore manifest order; A1-9 ns timestamps before 1970 within 1 µs below a ms boundary land 1 ms LATER than the documented floor (truncation toward zero) — use floor division for negatives; A1-10 value labels not attached to any variable are not written — serialise every value label in the dataset (`label dir` list from the ado, not only attached names) so `label define` orphans survive like native `save`; read side already restores all definitions.
Status: NEW → FIX-SPEC

### A2 (names & metadata) — report: A2_names_metadata.md (≈330 checks; ENC-2 held everywhere; NAME-CASE-1 needs the fixes below — several are regressions from today)

#### A2-1 — S0 — nested DuckDB dedup suffixes defeat the exact-name recovery (`a, a_1, A` → scan `A_1_1`; `nuemp, nuemp_1, NUEMP` → `NUEMP_1_1`): single file, silent wrong name + label/format loss (eager and lazy)
Fix spec: plugin_io.cpp `plan_columns` recovery: when leaf-count == scan-count, map POSITIONALLY and accept a scan name that equals the leaf name OR is `ascii_lower(leaf)` + one or more `_<digits>` groups (`^<leaf>(_[0-9]+)+$`, compared case-sensitively on the prefix); keep refusing any other mismatch (N2/SCH5 guard). Same rule everywhere `is_dedup_of` is used. Tests (unit for the matcher + verify with the three fixtures a, a_1, A / nuemp, nuemp_1, NUEMP / a_b, a_b_2, A_B, A_b).
Status: NEW (regression of today's recovery logic being incomplete) → FIX-SPEC

#### A2-2 — S0 — multi-file sources (glob, Hive tree, relaxed, `describe <glob>`) still load `NUEMP_1` and drop its metadata, silently (recovery gated on parquet_schema row count == ncol, never true for k>1 files)
Fix spec: read the leaf names from ONE file (`parquet_schema(<first path>)`; the strict schema gate proves all files identical); for Hive trees the partition columns are appended after the leaves — align the first n scan columns to the leaves and leave partition columns as scanned; for `relaxed` (union by name) apply the per-file recovery to the first file and document the limit if full union recovery is not contained. Tests: glob of parqit-written files with `nuemp/NUEMP` + labels (eager, lazy, describe, view save), Hive tree, relaxed.
Status: NEW → FIX-SPEC

#### A2-3 — S0 — `reshape wide` / `pivot` over an aliased stub copy `ViewCol.stata` into generated columns → a view save writes a Parquet file with DUPLICATE column names at rc 0 (pyarrow cannot read it); collect dies rc 110; pivot leaks `X_11/X_12`
Fix spec (root cause shared with R1/A2-4/A2-5/A2-9/A2-10): define ONE invariant — `ViewCol.stata` travels only with a column that keeps its engine name; every verb that creates or renames a column derives the new column with `stata` cleared (add a helper `ViewCol derived_from(const ViewCol&, name)` used by reshape_wide, pivot, reshape_long stubs/j, egen, gen, contract, collapse targets with explicit names, merge/append gen, rename/rename_many). Generated names in reshape wide/pivot must derive from the stub's EXPOSED name (`X` + j → `X1`), then pass the ci-guard against the live/generated set (refuse loudly on a clash with `x1`). Defensive: `cmd_view_save`/`view_kv_fragment` refuse to write when the exposed leaf names are not unique (never a duplicate-name file), and `cmd_view_collect_prepare` likewise refuses before creating variables. Tests: reshape wide/pivot with aliased stubs → names `X1 X2` or loud refusal; never a duplicate-name file.
Status: NEW (regression) → FIX-SPEC

#### A2-4 = R1 (rename keeps stale stata; file consequence confirmed) — FIX-SPEC above.

#### A2-5 — S1 — `collapse` default targets of aliased sources are named by the ALIAS in collect and in the saved file (`(mean) NUEMP_1` → `NUEMP_1`), while `by(NUEMP_1)` gives `NUEMP`
Fix spec: in `View::collapse`, a default target (target == source engine name) carries the source's `stata` (exposed name `NUEMP`); an explicit target name (`m = NUEMP_1`) is a new column with `stata` cleared (ci-guarded). Test.
Status: NEW (regression) → FIX-SPEC

#### A2-6 — S1 — `append …, gen(Val)` clashing only by case with a using-side column is accepted; collect then fails with a raw Binder Error
Fix spec: `View::append_with` TT-A2 check must also use `ci_clash(gen_name, c.name)` against every using side's columns (refuse before mutation, like merge gen()). Test.
Status: NEW → FIX-SPEC

#### A2-7 — S1 — a legacy note/characteristic that grows past Stata's 67,783-byte characteristic limit when transcoded is written correctly but silently LOST on read-back (also any foreign char > 67,783 bytes)
Fix spec: read side (ado `_parqit_resp_decorate`, char branch): guard `strlen(value) > 67783` → store the truncated value AND print a loud note naming the char and the original length (like the 32,000-byte value-label guard); write side: when a transcoded metadata item exceeds a Stata limit (char 67,783 / value-label text 32,000 / label 80), print a note that the file holds more than Stata can reload. Tests (40,000-byte Latin-1 note; foreign 70,000-byte char).
Status: NEW (regression via ENC-2 tail) → FIX-SPEC

#### A2-9 — S2 — `parqit.schema.sortedby` written by a view save carries engine ALIASES (a later lazy use loses the sort marker)
Fix spec: `view_kv_fragment` writes `sortedby` as EXPOSED names (map engine → exposed); the open path maps back (original_to_live already keys by original names). Test.
Status: NEW (regression) → FIX-SPEC

#### A2-10 — S2 — a column both sanitised AND case-aliased (`A B` → Stata `A_B`, alias `A_B_1`) loses its true source name on the lazy path and gets the alias stamped as `src_name`; the wrong src_name is written into a re-saved file (extends R2)
Fix spec: in `cmd_view_open` record `src_name` (keyed by the EXPOSED name) whenever `exposed() != original_name`, independent of the alias note (today the alias branch skips it); in `cmd_view_collect_prepare`, when overriding `p.stata_name = vc.stata`, set `ctx.parquet_names[p.source_name] = vc.stata` so no alias is ever stamped as src_name (R2); the true original keeps flowing through the chars channel. Test: foreign `A B` + `a_b` clash → lazy collect `char A_B[src_name] == "A B"`, re-save KV chars correct.
Status: NEW → FIX-SPEC

#### A2-11 = A1-10 (unattached value labels) · A2-12 = R3 (exact names in selection varlists) · A2-14 = A5-1 (encoding validated on view save) · A2-8 = A5-2/A5-3 (bridge loudness).

#### A2-13 — S2 — `parqit sql "SELECT * FROM read_parquet(f)"` over a case-clashing file silently loads DuckDB's deduped names (DESCRIBE cannot recover them)
Fix spec: document in help (sql/query section: a raw query exposes DuckDB's own result names; clashing source names arrive deduplicated `name_1`) and, cheaply, detect result names of the form `<other result name>(_<digits>)+` and print a `note:` naming them. 
Status: NEW → FIX-SPEC (doc + note)

#### A2-15 — S2 — contract mismatches: (1) empty Parquet column name arrives as DuckDB's `C<idx>` with no src_name — make it `v<position>` with src_name "" as the help states (recover empty raw names positionally) or fix the help; (2) help claims src_name lets "a later save restore" the original name — a save writes Stata names (original only in parqit.chars; ASSUMPTIONS #62) → fix wording; (3) CHANGELOG/ASSUMPTIONS #95(d): brought using columns clashing only by case are ALIASED and brought (fine), not refused → correct the text; (4) `parqit contract grp` with an existing `_freq` column silently projects the old `_freq` away where native stops r(110) → refuse like native (or require freq()). H15: reshape long with an aliased stub refuses loudly ("unbalanced stubs") — usability only; document that reshape stubs use the alias.
Status: NEW → FIX-SPEC (docs + small code)

### A3 (lazy verb semantics) — report: A3_verb_semantics.md (~200 checks, none a regression from today)

#### A3-1 — S0 — `merge` leaves common non-key variables MISSING on using-only rows (`_merge==2`)
Orchestrator repro: scratchpad/probe_a3a4.do → parqit k=4: c=., sc=""; native: c=444, sc="ud". Holds for 1:1/m:1/1:m, every keep()/keepusing() that keeps the column; a lazy save writes the same wrong payload. Cause: `View::merge_with` output projection emits `__m.c AS c` for common columns.
Fix spec: for a non-key master column that also exists in the using side and is kept, project `CASE WHEN __m.<mm> IS NULL THEN __u.c ELSE __m.c END AS c` (mm = master marker helper; same in the m:m/seq branch). Matched rows keep the master value even when missing (already right). Verify test (new v71 or extend v-merge family): string+numeric common vars, all keep()/keepusing() variants, both directions, master-missing-on-match stays missing, native oracle via cf.
Status: CONFIRMED → FIX-SPEC

#### A3-2 — S0 — `real()` follows DuckDB's cast grammar: `real("2019_01")`=201901, `"12_345_678"`=12345678, `"1_000.5"`=1000.5 (native .); `real("1d3")`=. (native 1000)
Fix spec: exprtrans `real()`: validate trim(s) against Stata's literal grammar `^[+-]?([0-9]+\.?[0-9]*|\.[0-9]+)([eEdD][+-]?[0-9]+)?$` (regexp_matches) and cast `translate(...,'dD','ee')` with TRY_CAST; anything else → NULL. Keep " 2 ", "+3", ".5", "5." working (they pass today). Unit tests in test_exprtrans + native oracle rows in a verify test.
Status: NEW (auditor evidence solid) → FIX-SPEC

#### A3-3 — S0 (narrow) / A3-4 — S1 — date functions outside Stata's date domain return values (native .) / day count near 2^31 aborts collect with r(920)
Fix spec: exprtrans day-argument helper: `CASE WHEN floor(x) BETWEEN -679350 AND 2936549 THEN DATE '1960-01-01' + CAST(floor(x) AS INTEGER) END` (01jan0100..31dec9999); `mdy()` guard year in [100, 9999] and month/day validity; `dofm()` guard month count in [-22320, 96479]; analogous for dofq/dofh/dofy/dofw if present; wrap in try() so nothing aborts the whole collect — out-of-range is row-local missing as the help promises. Unit + native oracle tests (year(3000000), mdy(1,1,99), dofm(96480), d=2147483647).
Status: NEW → FIX-SPEC

#### A3-5 — S1 — `parqit levelsof` of a numeric variable literally named `v` returns lexicographic order (`ORDER BY "v"` binds to the query's own `AS v` alias)
Fix spec: plugin_view.cpp levelsof: alias the rendered value with a reserved helper (`__parqit_lvl`) and ORDER BY the source expression; then AUDIT every fixed alias used in plugin-side SQL (stats/levelsof/tabulate/summarize/misstable/codebook/distinct/tabstat/correlate/histogram/count/head/list: `AS v`, `AS n`, `AS cnt`, …) for collisions with user column names — use `__parqit_*` helper aliases or subquery qualification everywhere. Test (v66/v67 family): user columns named v, n, cnt, value, total, freq with levelsof/tabulate/summarize/misstable/codebook.
Status: NEW → FIX-SPEC

#### A3-6 — S2 — levelsof/tabulate render non-integers differently from native (`0.1`/`1e-07`/`0.30000000000000004` vs `.1`/`1.00000000000e-07`/`.3`)
Fix spec: format numeric levels the way native does (%21.0g-style) using the engine's existing Stata-style number formatter (the one behind string()/strofreal, session.cpp dtoa/format_sci), after fixing A3-7; keep r(r) and values; test vs native on the A3 sample (0.1, 0.5, 1.5, 2, 1e-7, 123456789.123, 0.3).
Status: NEW → FIX-SPEC (low risk)

#### A3-7 — S2 — `string(5e-324)` renders `infe-324` (pow(10,-324) underflow in format_sci)
Fix spec: make the exponent computation underflow-safe (frexp/ldexp or snprintf("%.*e") path) for denormals; unit tests: 5e-324, 2.2250738585072014e-308, 1e-310, 0, -0, DBL_MAX, 1e308 vs Stata's %21.0g rendering.
Status: NEW → FIX-SPEC

#### A3-8 — S2 — collapse/merge/reshape result metadata differs from native (no "(mean) v" labels; source format dropped; `_merge` %8.0g + label/value label; reshape-wide labels)
Fix spec: mirror native where unambiguous: collapse targets get label "(stat) source" and native's format rule; `_merge` gets byte storage, %8.0g, label "Matching result from merge" and value label `_merge` (1 "master only (1)", 2 "using only (2)", 3 "matched (3)") unless gen() given (then name differs, labels same); reshape wide/long labels as native. Verify against native in a test. If a native rule is unclear, document the difference instead.
Status: NEW → FIX-SPEC (parity)

#### A3-9 — S2 — `keep in f/l` letter tokens refused
Fix spec: ado parses `in` ranges like Stata (`f`, `l`, negatives); for `l`/negatives resolve N via a count query (rigor over cost) then call keep_in; help: `in` syntax supports f/l. Test.
Status: NEW → FIX-SPEC

### A4 (atomicity/IO) — report: A4_atomicity_io.md (~70 experiments)

#### A4-1 — S0 — the automatic unchanged-source DIRECT save writes the SOURCE FILE instead of memory after `sort`/`gsort` and after Mata st_store/st_sstore/st_view writes (`c(changed)` stays 0); the written manifest then claims `sortedby` the rows do not have
Orchestrator repro: probe_a3a4.do → after `sort x`, g_a4.parquet rows are in source order (x 1910,1817,1724; memory x[1]=0) with KV sortedby ["x"]. Cause: `_parqit_save` gate (c(changed)==0 + nonce + size/mtime/nobs) → `cmd_save_data_direct` COPYs from the source. `c(changed)` cannot prove identity (Stata exempts sort and Mata stores by design) — no cheap automatic criterion is rigorous.
DECISION (rigor over performance, feature preserved): the source-copy path must NOT run automatically. Make it an explicit, documented opt-in option on `parqit save`: `copysource` ("copy the unchanged file loaded by the last parqit use instead of reading the dataset in memory; you assert nothing changed") and even then (a) harden the fingerprint — A4-2 — with inode + ctime (st_ctim, which utime cannot restore) + size + mtime + row count + an MD5/XXH of the Parquet footer bytes, re-checked immediately before the COPY; (b) refuse when the current `: sortedby` differs from the source manifest's sortedby or when c(changed)!=0 / N differs / k or names or types differ (loud: "use the default save"); (c) the KV sortedby written by the copy must be the SOURCE file's (true) order; (d) keep the `_dta[_parqit_fast_source_nonce]` characteristic (document it — A5-8) so the opt-in can verify the provenance. Default `parqit save` always reads memory (the general path). Tests: new verify test — sort/gsort/Mata write then save → memory content by default; `copysource` after sort → refused; `copysource` on untouched data → identical file; fingerprint tamper (same size/mtime rewrite) → refused. CHANGELOG (Changed + Added), ASSUMPTIONS entry with this reasoning, help/README/dialog (option).
Status: CONFIRMED → FIX-SPEC

#### A4-2 — S0 — fingerprint not content-sensitive (same-size rewrite with restored mtime → stale copy) — folded into A4-1.

#### A4-3 — S1 — torn read under a concurrent replace: `parqit use, clear` loaded the NEW file's rows under the OLD schema (rc 0)
Fix spec: record the file identity (dev/inode/size/mtime/ctime) of every source file at plan time; re-stat immediately before the fetch and again after it; on any change fail loudly ("<file> changed while it was being read") and leave the dataset untouched (atomic swap not committed); additionally compare the fetch's column count/names/types against the plan (cheap) and the fetched row count vs the planned count (exists); same guard for lazy collect (plan→fetch) where feasible. Test: a writer loop alternating two schemas + a reader loop (the auditor's cc/ scripts) — must never load a mixed result (reader sees rc≠0 or a consistent file).
Status: NEW → FIX-SPEC

#### A4-4 — S1 — valid destination names ≳200 chars refused ("File name too long": staging dir name = filename + ~55 chars)
Fix spec: name the staging dir/file from a short digest (e.g. `.parqit-tx-<16 hex of hash(dest)>-<pid>`) instead of the full basename; test with a 240-char basename (≤ NAME_MAX) and a 255-char one (must fail loudly, not corrupt).
Status: NEW → FIX-SPEC

#### A4-5 — S1 — `parqit save` into the open view's own DIRECTORY source is allowed (result silently doubles)
Fix spec: extend the SAVE-SELFGLOB refusal to directory sources: refuse when the (resolved) destination lies inside a directory the current view scans (or any open view? at least the current view, consistent with existing rule); message suggests saving elsewhere or collecting first. Test.
Status: NEW → FIX-SPEC

#### A4-6 — S2 — symlink dest replaced by a regular file (target stale); 0444 dest silently replaced (native `save, replace` fails on a read-only file)
Fix spec: when dest is a symlink, resolve it and replace the TARGET (stage in the target's directory, rename onto the target) — native semantics; when an existing dest is not writable by the user (access W_OK fails) refuse `replace` with a clear message (mirrors native r(603)); document both. Tests.
Status: NEW → FIX-SPEC (low risk)

#### A4-7 — S2 — foreign Hive trees with `=` inside a partition value fail with a raw DuckDB message → wrap in a parqit message naming the file/partition. A4-8 — S2/S3 — PUBLISH test hook inert on the POSIX flat path (add coverage), crash leaves lock+txn dir (message should name both for manual cleanup), 200 MB memory_limit → loud OOM (acceptable; document spill needs).
Status: NEW → FIX-SPEC (messages/tests/docs)

### A6 (performance/precision vs v0.1.27) — report: A6_performance.md
No performance regression (10 workloads, B=v0.1.27 CI build, N=today's build at audit start, BD=v0.1.27 rebuilt with the same preset as control; W1 save N 15.5/18.9 s vs B 20.1/20.2; W2 read 25.7/26.6 vs 24.9/26.6; merge/sort/collapse/view-save/2,500-col/no-clash/valid-UTF-8 all within ±6 % on medians). No precision difference (pyarrow byte-level identical files incl. parqit.* KV; datasignature/cf identical; merge/sort/collapse identical up to DuckDB parallel-sum ulps, pre-existing). Informational: A6-1 `parqit sql` +6–8 ms/call (+46 ms on a 2,500-column source) from the DESCRIBE name recovery (S3, accepted — rigor); A6-2 case-clash saves +7–10 % (footer rewrite ~5 ms; alias path) — accepted; A6-3 transcoding 50 M legacy cells +30 % vs valid twin (expected; valid path unchanged); A6-5 the implementer's in-flight builds changed `collapse (count)` targets to `long %12.0g` with label "(count) k2" (A3-8 parity; must be documented in CHANGELOG/help — check at certification).
Status: CLOSED (no action) — re-run a short W1/W2 comparison at certification after the fixes land.

### A5 (docs/contract) — report: A5_docs_contract.md (mechanical completeness clean; 106 help examples run rc 0; lint OK)

#### A5-3 — S0/S1 — `.dta` read through the adapter bridge silently collapses `.a`–`.z` to `.`, rounds fractional date/period counts (and, A5-2, transcodes legacy text) with NO note and no r()
Orchestrator repro: scratchpad/probe_a5.do — `.dta` with `e=.a`, `d=td(01jan2020)+0.5`, Latin-1 label; `parqit use using probe_a5.dta` (lazy) and `..., clear` (eager) → e[1]=., d[1]=21916, label "Região", zero `note:` lines. Cause: `_parqit_import_to_bridge` runs `quietly { use … ; parqit save "bridge", replace data }` (BRIDGE-QUIET-1 intent was only to silence import/snapshot chatter) and discards `r(ext_missing)`, `r(frac_dates)`, `r(transcoded_*)`, `r(encoding)`; `_parqit_open` forwards only ext/frac for `open _data`. Help/README claim parqit warns.
Fix spec: keep the chatter quiet but make the LOSSES loud: after the bridge save, read its r() and (a) print them through `_parqit_lossy_notes` (ext, frac, transcoded vars/cells/meta, encoding) prefixed so the user knows it came from the bridge of `<file.dta>`; (b) return them from `_parqit_import_to_bridge` and from the public commands that bridge (`parqit use using x.dta` lazy+eager, `merge/append/joinby using x.dta`, `parqit open _data`) as r(ext_missing)/r(frac_dates)/r(transcoded_*)/r(encoding) (additive). (c) Add `encoding(name)` to the bridge-creating commands where the plumbing is contained (`parqit use` and the two-table verbs' using adapters), passed to the bridge `parqit save ..., encoding()`; if not contained for a verb, document the default windows-1252 explicitly for that verb. (d) Help: Input formats section must state that a `.dta`/CSV/Excel bridge is a `parqit save` of the imported frame, so the save-side conversions apply (ext-missing collapse, fractional rounding, legacy transcoding) and are now reported; README Limitations likewise. Tests: extend v52/v60 or add v71: bridged .dta with .a / fractional / legacy label → notes present (log grep via `capture log`/`_rc`? — use r() values) and r() set, lazy and eager and merge paths.
Status: CONFIRMED → FIX-SPEC

#### A5-1 — S1 — `encoding(bogus)` on a lazy VIEW save is accepted and ignored (rc 0, file written); help says unknown names are refused before anything is written
Orchestrator repro: probe_a5.do → rc 0, file exists. Cause: `_parqit_wr_view_save_request` carries no `encoding` field; `cmd_view_save` never validates. The write dialog emits encoding() on view saves too.
Fix spec: forward `encoding` in the view-save request; `cmd_view_save` parses it with `parqit::legacy_encoding_parse` and refuses unknown names with the same message as save_data (kRcUsage) BEFORE any write; valid names are accepted with no effect (a view carries UTF-8) — help: "validated on both paths; effective for a memory save; a lazy save already holds UTF-8". Test in v32: view save with encoding(bogus) → rc 198, no file; encoding(latin1) → rc 0.
Status: CONFIRMED → FIX-SPEC

#### A5 S2 items (docs/dialog/README) — all to be addressed in the help/README/dialog pass
A5-4 read-side invalid-UTF-8 refusal (ENC1/v52) undocumented → add to String encoding; A5-5 strL promotion when transcoding widens past 2,045 (ASSUMPTIONS #94) → help; A5-6 encoding() aliases/canonical `r(encoding)` names + `r(transcoded_vars)` empty-vs-absent wording; A5-7 undocumented note families (metadata-restore guards, glob metadata mismatch, merge value-label conflict, two-table notes — see report T5 rows 13, 19–23) → document each; A5-8 `char _dta[_parqit_fast_source_nonce]` left by eager `use, clear` + the direct source-copy save path → document (and decide: keep the char; it is excluded on parqit save; Stata `save` keeps it — say so); A5-9 dialogs: add `lz4_raw` to codec lists, pivot `firstnm/lastnm/p##`; A5-10 README tables stale (`sql name()`, `appendin force`, `firstnm/lastnm`, dialogs/menu/set/path surface) → refresh; A5-11 two example-text inconsistencies; A5-12 `parqit sql` alias message printed as red `warning:` — use `note:` like `parqit use`; A5-13 `parqit use …, <unknown option>` → "filename required" (rc 100) — misleading; improve the error (low priority).
Status: NEW (from report; mechanical evidence in A5/mechanical_tables.md) → include in implementer brief (docs batch)
