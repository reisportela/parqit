# Changelog

All notable changes to `parqit` are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/) and the project adheres to
semantic versioning once `v0.1.0` is tagged.

## [Unreleased]

## [0.1.14] — 2026-07-02

Storage-type fidelity fix, reported from production use (a `byte` variable
came back `int` after a save/use round-trip), plus the findings of a full
adversarial audit (three code auditors + a native-Stata adjudication pass +
three empirical Stata batteries against native oracles). Locked by
`v41_type_fidelity`, `v42_expr_semantics`, `v43_io_hardening` and new unit
tests; every disputed semantic was adjudicated against a live native Stata
before being called a bug (two "confirmed" audit findings — `mod(x,0)` and
`!x^y` precedence — turned out to be native behavior parqit already matched,
and were pinned as correct instead of "fixed").

### Fixed
- **Storage types round-trip exactly; a display format never widens them
  (TYPE-1).** `refine_plan`'s period-format guard ("period/date formats keep
  their integer storage wide enough") widened a range-sized `byte` to `int`
  whenever the column carried *any* display format, not just a `%t*` class.
  Files written by parqit always record the format in `parqit.schema`, so
  every parqit-written `byte` column — plain, labelled, formatted or
  all-missing — loaded back as `int` on the eager-use, lazy-collect and
  view-save paths alike, and `apply_meta_type` could not narrow it back
  (the saved type is a floor, not a ceiling, by design). Foreign files
  masked the bug: with no recorded format, pure range refinement sized
  them correctly. The guard now fires only for genuine `%t*` format
  classes; on-disk physical types were always correct (`int8`), so no
  file written by an affected version needs rewriting — re-reading with
  0.1.14 restores the original storage type.
- **Generated infinities are missing everywhere (INF-1).** `exp(800)`,
  `1e300*1e300`, `8e307+8e307` and the literal `1e309` produced IEEE `+Inf`
  inside the pipeline (native Stata: `.`), which passed `< .` filters
  (inverting them), poisoned `collapse`/`summarize` aggregates into `.`,
  and disagreed with `missing()` on the same value. All arithmetic
  producers (`+ - * /` and `^`, `exp()`) now route through a
  `parqit_finite` scalar (missing when non-finite or ≥ Stata's missing
  threshold), and out-of-range literals translate to missing. Consequence:
  expressions compute in double precision like Stata's evaluator, so an
  *untyped* `parqit gen` over integer columns now stores `double` (type a
  `gen` to control storage; values are unchanged).
- **Date functions floor and never abort (DATE-2).** `year/month/day/
  quarter/dow/doy/mofd/yofd/mdy/dofm` used a bare `CAST(x AS INTEGER)`:
  a fractional day count *rounded* (`day(21915.9)` was off by one; native
  floors — `day(-0.5)` = 31) and an out-of-range argument (`year(3e9)`)
  aborted the whole query instead of being row-local missing. Now
  `floor()` + `try()`, the DATE-1 treatment, on the entire family — plus
  loud type errors for string arguments (native r(109)).
- **`subinstr()` no longer wipes values on a NULL needle/replacement
  (SUBINSTR-NULL-1).** Only the first argument was coalesced; a NULL
  second/third argument (reachable mid-pipeline after an unmatched merge)
  made DuckDB `replace()` return NULL — the whole value silently
  destroyed. All three string arguments are coalesced now; the empty-needle
  identity matches native (`subinstr("abc","","X",.)` = `"abc"`).
- **Loud rejections where native Stata rejects.** `||`/`&&` (r(198)
  natively; previously accepted silently), uppercase extended-missing
  literals (`x == .A`), malformed numbers (`1.2.3`), string arguments to
  `mod/round/ln/log10/sqrt/exp` and the date family (r(109) natively),
  and a `cond()` 4th branch whose type differs from the 2nd/3rd.
- **Filename wildcards are `*` and `?` only; `[` is always literal
  (GLOB-2).** With only `data1.parquet` on disk, `parqit use using
  "data[1].parquet"` silently read `data1.parquet` (rc 0, wrong data,
  message claiming the bracket name) — bracket names are common (browser
  download copies), bracket glob classes in a Stata filename are not.
  A path that exists literally is now always that file (fully escaped);
  a non-existing path acts as a pattern with `*`/`?` live and `[`
  literal, so a missing bracket-named file is a loud file-not-found.
- **`_se` joins the reserved-name set (SAN-SE-1).** A parquet column named
  `_se` loaded under that name — which Stata itself refuses — and a later
  `summarize _se` silently matched the system variable (empty no-op). It
  sanitises to `__se` like `_b`/`_pred` already did.
- **Per-process spill directory (SPILL-1).** The DuckDB spill default was
  `c(tmpdir)/_parqit_spill`, shared by every Stata instance on the machine
  (and every user on an HPC login node); concurrent sessions could
  interfere. Now `_parqit_spill_<pid>`.
- **`reshape wide` with missing `j` is a loud error (RESHAPE-MISSJ-1).**
  Rows whose `j` was missing (numeric `.`/NaN or string `""`) were
  silently excluded from the pivot — their stub values destroyed with
  rc 0; native Stata errors r(498) "variable j contains missing values".
- **Lazy `save` after merge/append never writes SQL NULL strings
  (MERGE-NULLSTR-1).** The MISS-1 `normalized` flag survived two-table
  verbs, but FULL/LEFT JOIN and UNION-BY-NAME introduce NULLs into carried
  string columns of unmatched/absent rows — the save guard was skipped and
  the file carried string NULLs (third-party readers saw `None` where the
  eager path writes `""`). Two-table verbs now clear the flag on string
  columns.
- **Dropping a sort key no longer kills the pipeline (SORT-DROP-1).**
  `parqit sort y` … `parqit drop y` … `collect` died with a raw DuckDB
  Binder Error; native keeps the physical order and clears `sortedby`.
  The drop stage now bakes the ordering in and truncates the recorded
  sort keys at the first dropped column.
- **`duplicates drop` (no varlist) sees `""` ≡ NULL (DUP-NORM-1).** The
  no-varlist form used a raw `SELECT DISTINCT`, so a `("", NULL)` string
  pair (reachable after merge/append) survived as two rows while the
  varlist form deduped it. Both forms now normalize keys identically.
- **`parqit list ... in f/l` out of range errors like native** instead of
  printing nothing with rc 0; **`parqit head` requires a positive count**
  (a negative reached the plugin as "no limit" and materialised the whole
  view); **`parqit appendin, keep()` validates against the using file**
  (native semantics; it also projects the read to the kept columns);
  **a failed `parqit append` no longer half-merges value-label metadata**
  (validate-then-mutate); **the view-prefix restore reports failure**
  instead of silently leaving the wrong view current; **the collect/use
  frame swap is `nobreak`** (a Break in the swap window could lose both
  the old and new data); **bridge cleanup survives paths with spaces**;
  **response parsing drops the last `fget` 32 KB line cap** (META-1
  pattern everywhere); **hostile oversized value-label text truncates
  with a note** instead of aborting the load; **the direct Arrow save
  path refuses a restricted observation range loudly** (latent
  mis-indexing guard, SAVE-RANGE-1).

### Fixed (found by executing the help-file examples)
- **The save self-overwrite guard matches the source glob, not its base
  directory, on absolute paths (SAVE-SELFGLOB-2).** With a view open over
  `qp_*.parquet`, saving to any {it:existing} destination in the same
  folder was refused (false positive — the documented
  filter-then-save-next-to-the-source workflow failed on its second run,
  once the partitioned tree existed), while a {it:relative} destination
  that did not exist yet was never made absolute, so overwriting the
  view's own source escaped the guard entirely on first contact. The
  destination is now matched against the decoded source pattern (parqit's
  glob dialect: `*`/`?` per segment, `**` deep, `[` literal), directory
  destinations are refused when the pattern's base lies inside them, and
  writing a NEW file the open view's glob would re-read is refused too.
  Locked by `v45_save_selfoverlap`; every help example now runs twice
  end-to-end in validation.

### Performance (same release; no precision change, verified by native oracles)
Benchmarked on the synthetic 10M×13 workers panel (min-of-3, quiet machine)
against the pre-audit build; the full correctness suite plus a new
native-oracle equivalence test gate every number.
- **`summarize, detail` is ~6.4x faster (PERF-DET-1): 4.38s → 0.68s for four
  10M-row variables.** Was one scan per variable with a per-variable mean
  subquery and NINE single-threaded `list_sort(list(x))` materialisations;
  now one count/mean/min/max scan for all variables, one central-moments
  scan with each mean injected as an exact `dtoa` literal (identical
  two-pass math), and per-variable order statistics via a CTAS through
  DuckDB's PARALLEL sort operator plus O(1) rowid point-picks. The Stata
  percentile rule (np = n·p/100: integral → mean of neighbours, else ceil)
  is applied in exact integer arithmetic; values are the same data elements
  the sorted-list form produced. A multi-stage pipeline is materialised
  once so k variables do not re-run it k times. Locked by
  `v44_summarize_detail_native` (native `summarize, detail` r() equality
  across even/odd counts, n=1..7, constants, missings, int64/byte, and
  all-missing columns). `quantile_disc` was measured and rejected: its
  finalize is effectively single-threaded (1.1s/variable).
- **`duplicates drop` (no varlist) uses a parallel hash GROUP BY +
  `any_value` (PERF-DUP-1)** instead of the `row_number()` window the
  DUP-NORM-1 fix had introduced — same normalized-key semantics, ~40%
  faster on a 10M dedupe, back at the old `SELECT DISTINCT` speed
  (1.15s vs 1.09s baseline) while keeping `''`≡NULL correctness.
- **strL sidecar reads switch to 8 MiB gulps (PERF-STRL-1)** — two
  `fread()` syscalls per cell become ~70 per file; local timing is
  unchanged (the floor is Stata's own per-cell strL store, measured
  0.5s/200k cells) but syscall-latency-bound filesystems (NFS/Lustre —
  the HPC target) no longer pay per-record round-trips. Record format
  unchanged; pinned by the existing v19 strL boundary test.
- Whole-surface check: use/collect/save/collapse/sort/merge/joinby/
  reshape/append remain at their engine floor (within noise of the
  pre-audit build); the INF-1 correctness guards cost ~4.7% on an
  expression-dense pipeline (5 generated columns over 2M rows) — the
  price of overflow values no longer silently inverting filters.

### Pinned as correct (native adjudication, not bugs)
- `mod(7,0)` is missing natively (the manual's `mod(x,0)=x` is stale);
  parqit already matched.
- `!5^0` is 0 natively — `^` binds tighter than `!` in practice; parqit's
  parser already matched. Documented in the translator.

## [0.1.13] — 2026-06-24

`parqit set tempdir` portability fix, surfaced by the external test pack once its
`set tempdir` test was made to pass native forward-slash paths. Locked by
`v40_set_tempdir_paths`; verified live against native Stata.

### Fixed
- **`parqit set tempdir` now accepts absolute and space-containing paths
  (SET-TEMPDIR-2).** The value arrives quoted (mandatory for a path with spaces,
  and usual for any path), and `_parqit_set` kept those surrounding quotes in the
  parsed value. A later regular-quoted reference (`"`value'"`) then expanded to
  `""/abs/path""`, which Stata parses as an *arithmetic expression* — a leading
  `/` divides by the first path component — so `parqit set tempdir "/scratch/me"`
  aborted with ``<component> not found``, `rc 111`. This broke the spill-to-scratch
  directory on every Unix absolute path (the HPC case the feature exists for);
  Windows escaped it only because its paths start with a drive letter (`C:`). The
  setter now strips one surrounding pair of double quotes so the value is the
  literal path, which `direxists`, the hex wire, and the engine all then receive
  intact. Scalar settings (`statamissing`, `threads`, `memory_limit`) and the
  loud unknown-name error are unaffected.

## [0.1.12] — 2026-06-24

Fifth adversarial-audit round (external test-pack, `verifications/parqit_audit_package`).
The single live-reproduced finding was confirmed against native Stata before the
fix, and the fix's Mata primitives were verified empirically (`st_global` on an
absent variable aborts rc 3300; `st_varindex` aborts rc 3500 and is unusable as a
guard; `_st_varindex` returns `.` for an absent name and for `"_dta"` without
aborting or abbreviating). Locked by `v39_char_projection`; 49/49 Stata suites and
the C++ suite green, no prior suite regressed.

### Fixed
- **Restoring a characteristic or note of a projected-away variable no longer
  aborts materialisation (PARQIT-CHAR-01).** Reading a column subset of a Parquet
  file that parqit wrote with a variable characteristic or note —
  `parqit use <varlist> using f, clear`, or a `contract`/`collapse`/`keep`/`drop`/
  `reshape` that removes a char- or note-bearing column before `collect` — emitted
  the char/note record for the now-absent variable, and the ado's `st_global(tgt +
  "[" + cname + "]", …)` aborted with `rc 3300 argument out of range`
  (`_parqit_resp_decorate(): function returned error`). The materialisation failed
  loudly and left memory intact (no stale/missing data), but the reach was broad:
  a plain column-subset `use` of any parqit-written file carrying metadata could
  trip it. Fixed at both layers, defence in depth: the plugin emitter now filters
  char/note records to `_dta` and live result columns, mirroring the `save` path's
  live-column filter; and the ado's `char` branch applies a characteristic only to
  `_dta` or a variable that survives in the staged result (existence-gated with the
  non-aborting `_st_varindex`, which also protects already-written and foreign
  files without a rebuild). `rename` still carries the characteristic to the new
  name (META-2), and notes/chars still round-trip on a full read — both are pinned
  as non-regressions by the new test. Distinct from META-1 (rc 3300 from a
  truncated long char line) and META-2 (chars following a rename); neither covered
  a char on a variable removed by projection.

## [0.1.11] — 2026-06-24

Fourth adversarial-audit round plus a dedicated two-directional data-integrity
audit. Every fix below was checked against an independent oracle (native Stata,
or pyarrow/duckdb for the foreign side) — the static audits' predictions were
unverified, and one (PQ-AUD-003) turned out to be wrong, see Notes. Locked by
`v37_audit_fixes_20260623d` and `v38_xtool_fidelity` plus new C++ unit tests
(`DATE-LIT-1`, `MISS-1`); 48/48 Stata suites and 53/826 C++ assertions green, no
prior suite regressed.

### Fixed
- **Lazy float/double columns normalize IEEE specials to Stata missing at the
  boundary (PQ-AUD-001).** A foreign Parquet `NaN`, `±Inf`, or a magnitude that
  collides with Stata's missing sentinel (`|x| ≥ SV_missval = 2^1023`) now
  becomes `.` the moment a lazy view scans it — the same guard
  the eager `use, clear` fill and the direct in-memory save already applied. So
  `missing(x)`, `keep if missing(x)`, sorts, stats, `duplicates`, and lazy
  `save` over hostile Parquet now match the eager path instead of leaking a
  value Stata cannot represent. A special generated *after* the boundary
  (`gen z = exp(10000)` → `+Inf`) is caught by `missing()`/`mi()` and by lazy
  `save` too, matching native Stata's overflow-to-missing.
- **Lazy string columns fold a Parquet NULL to the empty string `""`
  (PQ-AUD-002).** Stata has no string NULL; `""` is the string missing value.
  Lazy views now coalesce `NULL` → `""` at the boundary, so a `NULL` and an
  empty string tie for `sort`/`_n`/`keep in`/`duplicates` and lazy `save` never
  writes a string NULL — again matching the eager fill.
- **`duplicates drop` with no varlist now folds missing encodings
  (PQ-AUD-006).** With the 001/002 boundary normalization in place, the
  no-varlist `SELECT DISTINCT` collapses `NULL`-vs-`""` and `NaN`-vs-`NULL`
  exactly like native Stata — fixed at the root with zero extra query cost
  (the raw `DISTINCT` is kept; no `row_number()` partition was needed).
- **`egen` enforces an explicit storage type as value semantics, not just
  metadata (PQ-AUD-004).** `egen byte t = total(x)` whose sum is out of byte
  range now stores `.` (native), not 200; a narrow numeric type truncates toward
  zero / sends out-of-range to missing via the same `coerce_storage()` `generate`
  uses, and a string storage type on a numeric `egen` is a loud type error.
- **`generate` rejects a storage type whose family disagrees with the
  expression (PQ-AUD-005).** `gen str3 s = 123` and `gen byte b = "abc"` are now
  loud type mismatches (native r(109)) instead of silently producing a column
  whose metadata contradicts its values.
- **`td()`/`tc()`/`tC()` reject impossible calendar dates and a 60th second
  (PQ-AUD-007).** `td(31feb2020)`, `td(29feb2019)`, `td(31apr2020)`,
  `tc(... 00:00:60)`, `tc(... 24:00:00)` now fail loudly (native r(198)) instead
  of rolling forward to a wrong date via day-count arithmetic; `parse_dmy()`
  gained month-length + leap-year validation and `parse_hms()` now bounds the
  second at `< 60`. (Valid leap days like `29feb2020` still parse.)
- **`save` of an out-of-range `%tc` datetime now errors loudly at the exact
  int64-microsecond ceiling (DT-001).** The guard literal `9.22…e15` ms rounded
  *up* one ulp to the double `9223372036854776.0`, leaving a one-ulp hole where
  a `%tc` value (a year ~294,247 date) passed the check and `llround(ms·1000)`
  hit `2^63` (undefined → `INT64_MIN`) and was written to Parquet with `rc 0` —
  exactly the "no INT64_MIN on disk / no silent out-of-range" promise the
  contract makes. Both the fill and the staged/direct save paths now bound the
  microsecond product directly against ±`2^63` (`0x1p63`, exactly representable),
  also keeping the result off the `INT64_MIN` sentinel. Found by the
  data-integrity audit; pinned in `v38_xtool_fidelity`. Real dates are
  unaffected (the trigger is unreachable by any genuine date).

### Performance
- The 001/002 boundary normalization makes float/double and string columns
  computed expressions, so operations that previously read a raw Parquet column
  pay a small per-row guard. A `MISS-1` provenance flag (`ViewCol.normalized`)
  removes the redundant double-guarding: a boundary column carried through
  unchanged skips the guard in `missing()` and in lazy `save`, and only a
  recomputed column (gen/replace/aggregate) pays. On an in-memory 4M-row probe,
  collect/filter/sort/collapse stayed at baseline; `save` and
  `count if missing()` over a pure float column carry the irreducible cost of
  the boundary `CASE` (largest measured ~+25–60 ms/4M rows in memory, which
  shrinks against I/O out-of-core). Correctness is the binding constraint here.

### Notes
- **PQ-AUD-003 was evaluated and intentionally NOT applied — it is a false
  positive.** The audit claimed lazy `replace` should coerce into the existing
  narrow storage type (so `replace b = 200` on a `byte` → `.`, `replace s =
  "abcdef"` on a `str3` → `"abc"`). Native Stata does the opposite: `replace`
  **auto-promotes** the storage type to fit the value (`byte`→`int` keeping 200,
  `str3`→`str6` keeping `"abcdef"`, verified on Stata 16+). parqit already
  matches that promotion (the collect-time `apply_meta_type()` widening), so
  applying the audit's fix would have *introduced* a precision/value regression.
  A regression guard in `v37` pins the correct promotion behaviour.
- **Two-directional data-integrity audit — verdict: faithful.** A 9-dimension
  source audit (type map, value fill, save, missing values, strings, date/time,
  numeric precision, metadata, structure) plus an empirical round-trip campaign
  with independent pyarrow/duckdb oracles found exactly one defect (DT-001,
  fixed above) and no S1–S3 issues. Confirmed exact in BOTH directions: every
  foreign (DuckDB/Arrow/pyarrow) integer/float/decimal/bool/date/timestamp/
  string/NULL lands in Stata exactly (within the documented type contract), and
  Stata byte/int/long/float/double/str#/strL/dates/missing/labels/notes/formats/
  characteristics save to Parquet and round-trip exactly. The only losses are
  Stata's *own* limits — no native int64 (values beyond 2^53 round to the
  nearest double) and a single Parquet missing concept (extended missing `.a`–
  `.z` collapse to `.`, labels kept) — and **every such loss is announced with a
  loud `note:`**, never silent. Invalid UTF-8 on write is refused (`rc≠0`); an
  embedded NUL round-trips faithfully to Stata's own NUL-terminated view of a
  `str#`. Locked by `v38_xtool_fidelity`.

### Documentation
- Help (`parqit.sthlp`) and `README.md` now carry an explicit **Acknowledgements**
  section crediting **`pq` by Jon Rothbaum** as the work from which the `parqit`
  solution was designed, and thanking the **BPLIM team at Banco de Portugal**
  (https://bplim.bportugal.pt/), whose interaction greatly benefited development.
  The help's date-literal list now also documents `tC()` and the loud rejection
  of impossible dates.
- **`README.md` now teaches a one-line `net install` directly from GitHub** —
  `net install parqit, from("https://github.com/reisportela/parqit/releases/download/v0.1.11") replace`
  — alongside the offline download-then-install route. The release workflow
  (`.github/workflows/build.yml`) now also publishes the loose package files
  (`parqit.pkg`, `stata.toc`, `.ado`/`.sthlp`, per-OS `*.plugin`) as release
  assets so that URL resolves; the `net install` mechanism was verified locally
  against the package layout (`net install` → `version` → `selftest` all rc 0).

## [0.1.10] — 2026-06-23

Follow-up to the third audit round, applying the deferred items that are clean
and constraint-safe. Locked by `v36_audit_fixes_20260623c`; 46/46 Stata suites
and 50/784 C++ assertions green.

### Fixed
- **A no-by `parqit collapse` over zero input rows yields zero observations,
  not one fabricated row (COLLAPSE-EMPTY-1).** `keep if <impossible>` then
  `collapse (mean) x` no longer collects a single `mean .`, `sum 0`, `count 0`
  row (native Stata stops with r(2000)); it now emits zero rows via
  `HAVING count(*) > 0`, consistent with the `by()` case. Zero added cost.

### Notes
- **RESAVE-STALE-SRCNAME-1 was evaluated and intentionally NOT applied.**
  Dropping a column's `[src_name]` characteristic on save would have lost the
  ability to recover the original (foreign) column name after a parqit→parqit
  round trip — a precision/feature loss. The "staleness" it targeted only
  arises after an explicit rename, a niche case not worth that trade-off; the
  characteristic is kept. The other deferred items (PERF-DETAIL-KSCAN /
  PERF-PCTILE-REBUILD, strL save return codes, reshape i()/j() key folding)
  remain deferred for the reasons in ASSUMPTIONS #51.

## [0.1.9] — 2026-06-23

Third adversarial-audit round (multi-agent), run over the v0.1.8 + Codex tree.
Every fix is locked by an independent oracle (native Stata twin or pyarrow) in
`v35_audit_fixes_20260623b` (and the Codex reshape fix in `v34`); the prior 44
suites and the C++ unit suite stay green (45/45 Stata, 50/784 C++).

### Fixed
- **`reshape long` handles numeric suffixes with leading zeros like native
  Stata.** A column such as `inc01` now contributes the `j=1` level but is
  carried as an ordinary column; the long value comes from the canonical
  `inc1` column when present, otherwise the long stub is missing. This prevents
  silent row fabrication when both `inc1` and `inc01` exist. (Codex round; test
  `v34_reshape_leading_zero_suffix`.)
- **`gen str#` truncates the value to the declared byte width (STR-GENWIDTH-1).**
  `parqit gen str3 x = "hello"` now stores `"hel"` (str3) like native Stata,
  the string analog of the v0.1.8 numeric `gen byte/int/long` coercion. A
  codepoint split exactly at the byte boundary yields U+FFFD (parqit keeps valid
  UTF-8, as `substr()` already does); the common case is byte-exact.
- **`merge m:m` folds NULL/NaN/`""` keys into one missing group (TT-MM-MISSING-1).**
  The within-key windows and the spine partitioned on the RAW key while the join
  normalized it, so mixed missing encodings across master/using (a master NULL
  key, a using NaN key) over-matched. The keys are now normalized in place so the
  windows, spine and join all see one missing group — consistent with the other
  merge kinds, which were already correct.
- **`collapse`/`contract`/`duplicates drop`/`egen … , by()` fold `""`/NaN to
  missing in the group key (GROUPKEY-1).** Like `merge`/`joinby` (and native
  Stata, where `""` is the missing string and a NaN/NULL float is `.`), an empty
  string now groups with NULL and a NaN with NULL, instead of splitting
  Stata-identical keys into separate groups (and `duplicates drop` missing real
  dups). Matters for foreign files that mix encodings; parqit-written files were
  already single-encoding.
- **Partitioned `parqit save … , replace` over the open view's glob/directory
  source is refused (SAVE-SELFGLOB-1).** The IO-1 self-overwrite guard skipped
  glob sources, so `save dir, replace partition_by()` over a `use "dir/"` view
  renamed-aside and deleted the source tree. The guard now also checks
  directory containment for glob/dir sources.
- **`parqit set threads` validates strictly (SET-THREADS-1/2).** A non-integer
  (`4.5`→silently 4), trailing garbage (`4 8`→4), or out-of-range value (a raw
  DuckDB INTERNAL cast-overflow assertion + stack trace) are now a clear
  `value must be a positive integer (1..2147483647)` error.
- **A foreign `parqit.dtalabel` longer than 80 chars no longer aborts the load
  (DTALABEL-LEN-1).** The dataset label is applied best-effort and truncated to
  Stata's 80-char limit instead of failing the whole `use`/`collect` with r(133).
- **`strpos(s, "")` returns 0 like Stata (STRPOS-EMPTY-1)** (DuckDB returns 1 for
  an empty needle); **`length()` on a numeric is a clear error naming `length()`**
  rather than the misleading "strlen() needs a string" (LENGTH-NUMERIC-1).
- **`parqit open _data` no longer orphans its temp bridge** if the subsequent
  view-open fails (OPENDATA-BRIDGE-ORPHAN-1): the bridge is tracked for the
  `close _all` sweep and erased on the failure path.

### Changed
- **Two-way `parqit tabulate` drops a redundant pre-scan (PERF-TAB2-PRECOUNT-1).**
  The distinct-column count is now derived from the already cell-bounded GROUP BY
  result instead of a separate `count(DISTINCT)` full scan — one pass over the
  data instead of two. Output unchanged.
- **`parqit set tempdir` warns when the directory does not exist** rather than
  deferring a confusing failure to spill time (it still accepts the path; the
  user may create it before the first out-of-core query).
- **`release_lint.sh` now rejects duplicate `###` headings in every
  `CHANGELOG.md` section**, not only in `[Unreleased]`; the duplicated
  `0.1.3` `### Added` section was consolidated. (Codex round.)

## [0.1.8] — 2026-06-23

Residual-hazard round from a multi-agent adversarial audit. Every fix is locked
by an independent oracle (native Stata twin or pyarrow) in the new verify test
`v33_audit_fixes_20260623`; the prior 42 suites and the C++ unit suite stay
green.

### Fixed
- **`parqit gen <byte|int|long|float>` honours the storage type like native
  Stata (EXPR-1).** A narrow integer target now truncates toward zero and maps
  an out-of-range value to system missing (`gen byte b = 3.9` → `3`,
  `gen byte b = 200` → `.`, `gen byte b = -2.5` → `-2`), and the collected
  column is sized to the requested type (byte/int/long) rather than silently
  widening to double; `gen float` rounds to float32. Previously the requested
  type was discarded and the raw double stored. The default (no-type) path is
  unchanged.
- **`parqit save` verify/fast-path reads escape glob metacharacters (GLOB-1).**
  The internal self-read that confirms a written file (and the unchanged-source
  fast path that re-reads the source) treated a literal path like
  `panel[2020].parquet` as a glob, so a sibling matching the bracket class could
  be scanned in place of the real file — a spurious verify failure, or silently
  the wrong rows. Internal literal reads are now glob-escaped; user-facing
  `parqit use` keeps its glob semantics.
- **Partitioned `parqit save … , replace` is crash-safe (ATOM-PART-1).** The old
  partition tree is now renamed aside and removed only after the new tree is
  swapped in (restored on failure), instead of being deleted before the rename —
  closing a window in which a crash left neither tree. The Windows flat-file
  replace uses the same rename-aside (IO-2).
- **`collapse (first)/(last)` without a prior sort is deterministic
  (COLLAPSE-3).** It now falls back to a reproducible total order over all
  columns instead of an engine-defined `row_number() OVER ()` that varied with
  parallelism / row-group layout.
- **`merge m:m` master-side pairing is reproducible (TT-A1).** The within-key
  row index now orders by all master columns (mirroring the using side) when the
  view is unsorted or sorted only on the key, so the pairing no longer depends on
  scan order.
- **`parqit append … , generate(name)` colliding with a using-file column is a
  clear error (TT-A2)** instead of a cryptic DuckDB "duplicate name in UNION BY
  NAME" binder error.
- **`parqit collapse` with a weight expression is a clear, loud error
  (COLLAPSE-WEIGHTS).** `collapse (mean) x [fweight=n]` no longer mis-parses into
  "variable n] not found"; weights are reported as unsupported.
- **`reshape wide` with a numeric `j()` orders generated columns numerically**
  (`inc2` before `inc10`, RESHAPE-WIDE-COLORDER), matching native Stata, instead
  of by the lexicographic string cast.
- **`parqit tabulate` of an integer-valued float/double prints `11`, not `11.0`
  (TAB-FLOAT-1)**, reusing the same integer-trimming as `levelsof`; the two-way
  table also orders numeric axes numerically (TAB2-ORDER-1).
- **`DECIMAL` columns whose integer part can exceed 2^53 now warn on read
  (DEC-1)** with the same ">2^53 rounded to nearest double" note that BIGINT /
  HUGEINT already emit, so the precision loss is no longer silent.
- **`TIME` nanosecond columns flag the sub-millisecond truncation (TS-NS-1)** the
  way nanosecond `TIMESTAMP` already did.

### Added
- **Chained relational comparisons (EXPR-4).** `1 < x < 3000` now parses
  left-associatively as `(1 < x) < 3000` (a 0/1 result) instead of erroring. A
  column reference named like the internal `_n`/`_N` sentinel is now a loud
  error rather than malformed SQL (INJID-1).

### Changed
- **`parqit codebook` scans the file once, not once per variable
  (PERF-CODEBOOK-KSCAN).** Like `summarize`/`distinct`, it now issues a single
  combined aggregate; `parqit codebook` over `k` variables is ~`k`× less I/O on
  wide or cold/network files. Returned values are unchanged.

### Documentation
- **Missing-value semantics for `keep if`/`gen` are stated precisely** in the
  help: the SQL default coincides with Stata for `<`, `<=`, `==` but differs for
  `>`, `>=`, `!=` against missings (and `gen y = x > c` yields missing where `x`
  is missing); `parqit set statamissing on` reproduces Stata in both filters and
  assignments. This corrects the previous over-broad "coincides with Stata"
  claim; the SQL default is the charter-mandated behaviour and is unchanged.
- A foreign column whose name was sanitised is recorded via `char[src_name]` on
  the eager `parqit use … , clear` path but not yet on the lazy `use` → `collect`
  path (INJID-2, deferred); `summarize, detail` still scans once per variable
  (PERF-DETAIL-KSCAN, deferred). See ASSUMPTIONS.

## [0.1.7] — 2026-06-16

### Fixed
- **`parqit save` refuses strings with invalid UTF-8 instead of corrupting the
  file.** A Stata string carrying raw Latin-1/legacy bytes (e.g. `é` = `0xE9`,
  common in imported or admin data) used to be written verbatim into a
  UTF-8-typed Parquet column: the Arrow-scan writer produced a file no reader —
  parqit included — could decode (`parqit collect` → "not valid UTF8"), and the
  staged (`PARQIT_SAVE_NOARROW`) writer silently nulled the cell, with `rc 0` and
  no warning in both cases. Both writers now validate UTF-8 per cell and fail
  **loudly** at the offending `var[obs]` (pointing to `unicode translate`),
  never `rc 0` with a broken/stale file, and a failed save never clobbers a
  pre-existing good file. Valid UTF-8 (ASCII/accented/emoji/strL) is unaffected.
  This restores the "byte-identical under both paths" guarantee for v0.1.6.
  Verify test `v32_invalid_utf8_save`; see ASSUMPTIONS #49.
- **`parqit query` validates on a copy of the current view.** An invalid raw SQL
  fragment now returns a real error while leaving the previous lazy pipeline
  open and unchanged; it no longer discards the view during failed validation.
- **`string()`/`strofreal()` follow Stata's default `%9.0g` more closely.**
  Boundary cases that used to drift from Stata (`1e100`, `-1e100`,
  `.00009999999`, `.000123456`, and nearby scientific/decimal cutoffs) now go
  through an internal DuckDB scalar checked against native Stata output.
- **`substr()` is byte-indexed without blanking invalid UTF-8 slices.** Valid
  byte slices are exact Stata-style slices. If the requested bytes split a
  UTF-8 codepoint, parqit returns U+FFFD because DuckDB/Arrow VARCHAR values must
  remain valid UTF-8.

## [0.1.6] — 2026-06-16

### Changed
- **Much faster general `parqit save …, data` (modified in-memory data).** The
  writer now assembles each column once as an Arrow array and COPYs straight
  from a registered Arrow scan, instead of staging every value through a DuckDB
  temp table. On 10M rows this moves the mixed 13-column save from ~6.6s to
  ~4.9s (now faster than `pq`'s ~5.0s), numeric-only from ~2.7s to ~1.7s, and
  string-only from ~4.5s to ~3.2s — every case faster than before, none slower.
  Output is byte-identical (same conversions, verified by an independent pyarrow
  oracle and the full verify suite under both paths). The previous temp-table
  writer remains as a fallback, selectable with `PARQIT_SAVE_NOARROW=1`. Uses
  DuckDB's `duckdb_arrow_array_scan` (deprecated upstream but present in the
  pinned 1.5.x; pinned by an engine-capability test). See ASSUMPTIONS #48.
- **Faster `parqit save …, data` after an unchanged `parqit use …, clear`.**
  When the in-memory dataset is still the exact, unmodified result of loading a
  single regular Parquet file, `parqit save …, data` now writes by running DuckDB
  directly from that source file with the current Stata manifest and `parqit.*`
  metadata, instead of reading every cell back through the Stata Plugin
  Interface. The fast path is intentionally narrow: it is disabled after data
  changes, if the source file fingerprint changed, for sanitised/pathological
  source names, and for unsupported temporal formats (`%tc` stays on the general
  path). The old writer remains the fallback. On the 10M×13 synthetic benchmark,
  the `SAVE` leg moved from **6.852s** to **1.079s** while the pq leg in the same
  rerun was **5.342s**; `USE`, `DESCRIBE`, `MERGE`, merge→save, and `APPEND`
  remained in the same timing band, with value equality still reported by the
  benchmark.

## [0.1.5] — 2026-06-14

### Fixed
- **The view-save paths now emit the same lossy-conversion notes as the
  in-memory save** (ATOM-2, from `PARQIT_ADVERSARIAL_AUDIT_FOR_CODEX_20260614.md`).
  A `parqit save` through a lazy view, and `parqit open _data`, performed the same
  lossy conversions the in-memory `save_data` path warns about (extended
  missings `.a`–`.z` → null, non-integer date/period values rounded) but printed
  **no note** — so a user could lose `.a`–`.z` identity or fractional-date
  precision silently through the product's headline workflow. Now:
  - **`parqit open _data`** surfaces the bridge snapshot's extended-missing and
    fractional-date notes that `qui` used to swallow (the loss happens when the
    in-memory dataset is written to the promotion bridge).
  - **`parqit save` through a view** detects a non-integer `%td`/`%tc`/`%tC`/period
    value that `compile_for_save`'s `round()` would change (e.g. a date column
    made fractional by `parqit replace`) and emits the same "rounded to the nearest
    unit" note. It does **not** invent an extended-missing note — a view over a
    Parquet source carries no Stata extended missings (those pass through
    verbatim) — and it does **not** double-warn for a loss already reported at
    `parqit open _data`.
  - The frac detection adds **no work to the common path**: a save with no date
    columns does nothing, and a date-bearing save runs only a metadata-only
    `LIMIT 0` describe — the one aggregate scan fires solely when a date column
    is genuinely floating-typed (A/B benchmark on a 5M-row date file: within
    run-to-run noise). The on-disk payload is byte-identical to the in-memory
    save and unchanged from before.
  - Locked in by `tests/verify_suite/v29_atom2_view_notes.do` (open-`_data` notes,
    `view == memory` payload, view-path frac note with no spurious ext note, and
    a clean integer-date save staying silent), each on-disk check via an
    independent pyarrow oracle.
- **Characteristics and notes survive a renamed column** (META-2): `parqit rename`
  now follows a column's characteristics (and notes, which are characteristics)
  to the new name, so they are no longer dropped when a renamed foreign column is
  saved through a view. Value labels already rode on the column and were unaffected.
- **`parqit save … , replace partition_by(...)` is re-runnable** (IO-3): a
  partitioned save with `replace` now removes an existing partition-*tree*
  directory (only after the new tree is staged and verified, never losing data
  on failure) and then renames atomically; a target that exists as a plain file,
  or `replace` omitted, is still refused loudly. (ATOM-1's atomicity was already
  in place — the partitioned branch stages in `dest.parqit_tmp`, verifies, then
  atomically renames.) Locked in by `tests/verify_suite/v30_residual_fixes_20260614.do`
  (re-run yields no stale/duplicated rows; pyarrow oracle).
- **An aborted collect no longer orphans its spill temp table** (ATOM-3):
  `set_prepared_read` drops a prior un-fetched `_parqit_collect_*` temp table before
  preparing the next, so a collect that aborts between prepare and fetch cannot
  accumulate spill tables for the session's life. The happy path (the fetch drops
  its own table) is unaffected (`DROP … IF EXISTS` is idempotent).

### Known limitations
- **All-null typed column** (META-3, assessed, not changed): an entirely-null
  *typed* Parquet column loads as an all-missing `byte` with no note. The values
  are correct (all missing); only the type metadata is downgraded. A faithful
  note needs the row count (to avoid spurious notes on 0-row files), which is
  computed after the sizing block, so a correct fix would thread state through the
  performance-critical F2 footer-sizing path for an informational note on a rare
  case — deferred as poor cost/benefit under the no-regression mandate.

## [0.1.4] — 2026-06-14

### Fixed
Second-round fixes from the 2026-06-14 multi-agent adversarial audit (see
`PARQIT_ADVERSARIAL_AUDIT_2026-06-14_f386b5b.md`). All are Stata-faithful, verified
against real Stata 19.5 + an independent pyarrow oracle, and locked in by
`tests/verify_suite/v28_audit_fixes_20260614.do` plus new C++ unit assertions;
the full suite (C++ + 36 Stata verdicts) stays green.

- **`gen`/`replace` no longer leak `inf`/`nan` to disk** (NUM-1): `x/0` and
  non-real powers like `(-8)^0.5` are missing in Stata. The translator now guards
  `/` and `^` (`CASE WHEN isfinite(...) THEN ... ELSE NULL END`), so `parqit collect`
  and `parqit save` agree and a third party never reads `inf`/`nan` from a parqit file.
- **`round()` ties round toward +∞, not away from zero** (NUM-2): `round(x)` is
  `floor(x+0.5)` and `round(x,u)` is `floor(x/u+0.5)*u`, so `round(-2.5)==-2` and
  `round(-0.5)==0`, matching Stata (DuckDB's `round()` rounded ties away from zero).
- **`upper()`/`lower()` are ASCII-only; `ustrupper()`/`ustrlower()` are Unicode**
  (STR-1): `upper("café")=="CAFé"` again (a byte-safe ASCII fold via `translate`),
  while the `ustr*` family keeps DuckDB's Unicode case mapping.
- **An invalid `mdy()`/`dofm()` is row-local missing, never a query abort**
  (DATE-1): `make_date` is wrapped in `try()`, so one bad date triple no longer
  kills the whole `collect`/`save`.
- **`merge` uniqueness guard folds `""`/`NaN` to Stata-missing like the join**
  (MERGE-1): `check_unique` now applies the same `nullif('')` / `NaN→NULL`
  normalisation the join uses, so a third-party (pandas/pyarrow) key that is `""`
  beside `NULL`, or `NaN` beside `NULL`, can no longer pass an `m:1`/`1:1` check
  and then over-match cartesian-style.
- **`merge, keep()` is a set** (MERGE-2): the mask is built from idempotent
  flags, so `keep(master master)` returns master rows (it had summed bits and
  flipped to using-only).
- **`reshape long` errors on a bare column named like a stub** (RESHAPE-5):
  a kept column colliding with a stub or with `j` now stops loudly (Stata rc 110)
  instead of emitting two same-named columns and losing data.
- **In-memory save of an out-of-range `%tc` errors loudly** (TYPE-SAVE-1): the
  `%tc` writer gained the same range guard the `%td`/`%tC`/period writers have, so
  it no longer narrows to `int64`-MIN garbage with rc 0.
- **`parqit save` onto the open view's own source file is refused** (IO-1): an exact
  canonical-path match is rejected (write elsewhere or `parqit collect` first),
  preventing a lazy view from silently truncating the data it still rereads.
- **Long value labels / notes / characteristics survive the read** (META-1): the
  metadata response is now read whole with `fread` and split on the newline,
  instead of `fget`, whose 32768-byte line cap truncated a long hex field (or
  aborted the load with `r(3300)`). A 30000-byte value label and characteristic
  now round-trip exactly. The (binary, `\n`-only) writer is unchanged.
- **Arrow string layout is guarded** (STR-2): the VARCHAR walker now asserts the
  expected 3-buffer `utf8` layout, so a future engine change to large-utf8 /
  string-view fails loudly instead of reading garbage offsets. No effect on the
  pinned DuckDB 1.5.3.

### Added
- **`parqit rename (oldlist) (newlist)`** (RENAME-1): the parenthesised group form
  documented in the README/brief is now accepted alongside `rename old new`,
  renamed pairwise with equal-length validation.
- **`egen name = fcn(expr)` accepts internal commas** (EGEN-1): the option split
  is parenthesis-aware, so `egen m = mean(cond(x>0, y, .)), by(g)` works.
- **Generated `_merge` carries Stata's value labels** (TT-3): `1 "Master only (1)"`,
  `2 "Using only (2)"`, `3 "Matched (3)"`, so `tabulate`/`list` read like native
  `merge`. Numeric `_merge` values are unchanged.

### Performance
- **`gen`/`keep if` with `_n` stay streaming** (PERF-1): the row-context subquery
  now emits `row_number()` only for `_n` and the blocking `count(*) OVER ()` only
  for `_N`. `_n`-only idioms (`gen seq=_n`, `keep if _n<=K`) become a pure
  `STREAMING_WINDOW` instead of buffering the whole input — ~1.85× faster on a
  20M-row probe (0.805s → 0.435s) and out-of-core safe. `_N` and `_n`+`_N` plans
  are unchanged; no result changes.
- **Lazy open no longer forces a row count for metadata probes** (PERF-3):
  `plan_columns` skips `count(*)` when only schema/metadata is needed, so a
  CSV/non-Parquet `parqit use`/using-side open stops doing a full scan (Parquet was
  already footer-cheap and is unaffected).

### Changed
- **Benchmarks carry no private paths** (REL-2b): the seven `benchmarks/*.do`
  files now take the repo root and reference dataset from args → `PARQIT_REPO` /
  `PARQIT_BENCH_REF` env vars → portable defaults, instead of a hard-coded personal
  path. `tests/release_lint.sh` gained a guard that fails on any `/home/…` or
  `/Users/…` absolute path in tracked source/test/benchmark files (historical
  `.md` audit reports exempt).

### Known limitations
- **`string()` small-magnitude band** (STR-2, resolved in `[Unreleased]`;
  assessed, not changed in this 0.1.4 pass): Stata's
  default `%9.0g` is a width-9 *general* format, approximated by `%.7g`; values in
  `[1e-5,1e-4)` print in scientific where Stata prints decimal. A faithful fix
  means reimplementing width-9 formatting across the whole range, which would risk
  regressing the many values that currently match, so it stays a documented
  approximation (ASSUMPTIONS #44) for this no-regression pass.

### Fixed (earlier 2026-06-14 round)
Correctness fixes from the 2026-06-14 adversarial cross-audit (see
`PARQIT_ADVERSARIAL_AUDIT_FOR_CODEX_20260614.md`). All are Stata-faithful and
verified against real Stata 19.5 + an independent oracle; the C++ translator
fixes add 94 executed assertions and a new `v27_audit_fixes` verify test.

- **`merge`/`joinby` now match missing/empty/NaN keys across tool encodings**
  (TT-1). Join keys are normalised to Stata's missing equivalence inside the
  comparison (`""` ≡ NULL, NaN ≡ NULL), so an out-of-core join of a third-party
  (e.g. pandas/pyarrow, which encodes a missing float as NaN) file matches the
  same rows as native Stata, `parqit mergein`, and `parqit collect`. Integer keys
  keep their exact type/value.
- **`reshape long` no longer absorbs a variable that merely shares a stub
  prefix** (RESHAPE-1): with a numeric `j`, only numerically-suffixed columns
  are `xij` members, so `income` is carried (not read as `inc`+`ome`), matching
  Stata. An `i()` variable named like a stub prefix is never consumed (RESHAPE-2).
  `reshape wide` rejects j-values that would form illegal Stata names (RESHAPE-3);
  stub metadata is taken from the lowest j-value, not the lexicographically
  smallest suffix (RESHAPE-4).
- **`collapse (count)`/`firstnm`/`lastnm` on string variables treat `""` as
  missing** (COLLAPSE-1/PARITY-1) — `count` returns the nonmissing count and the
  `*nm` selectors skip empty strings, matching Stata.
- **Expression translator** (XLAT-1…9, PARITY-6), all vs real Stata:
  `string()`/`strofreal()` now emit Stata's `%9.0g` text (not raw `CAST`);
  `substr()`/`strpos()` are byte-based (Stata semantics; `usubstr`/`ustrpos`
  stay character-based); `^` is left-associative; `cond()`, `&`/`|`/`!`,
  `mod(x,y≤0)`, `inrange()` with a missing bound, and `==`/`!=` in value
  context under `statamissing` all match Stata's missing handling; `real()`
  maps `'inf'`/`'nan'` to missing; an internal `_n`/`_N` marker can no longer
  be smuggled through a string literal.
- **A finite Parquet `DOUBLE` outside Stata's storable range** (|x| ≥ 8.99e+307)
  now loads as missing **with a loud note**, never a silent wrong value (TYPE-1).
- **The save path bound-checks period/date values** before narrowing to the
  on-disk integer width and errors loudly on overflow, instead of a silent wrap
  (TYPE-2).
- **Partitioned `parqit save` now stages the partition tree before publishing it**
  (ATOM-1). A failed or unverifiable write removes the temporary tree instead of
  leaving a partial final dataset that blocks retry.
- **A UUID column no longer crashes the eager `parqit use`** path — strings are
  sized over their casted projection, so `use` and `use`+`collect` agree (STR-1).
- **An all-null column loads with a note** instead of a silent all-missing
  variable (META-3).
- **Foreign `parqit.*` metadata is hardened**: a value-label value travels
  hex-encoded so a stray `|`/newline cannot corrupt the protocol (INJ-1); a char
  target/name or value-label name that is not a legal Stata identifier is
  skipped with a note instead of aborting the whole load (META-1); non-integer
  value-label keys are skipped (META-4) while `.`/`.a`–`.z` keys still apply.
- **Numeric ↔ SQL text is locale-independent** (LOC-1/2): `collapse`
  percentiles/median, `sample <share>`, the histogram and skewness/kurtosis
  literals use `std::to_chars`/`std::from_chars`, so a comma-decimal OS locale
  can no longer break or mis-round them. A rejected `memory_limit`/`tempdir`
  value is no longer cached for re-apply (LOC-3).
- **`sort`/`gsort` emit explicit `NULLS LAST`** so missing-last ordering is
  Stata-correct independent of DuckDB's default (SORT-1). The strL sidecar
  header widens the observation field to span Stata-MP's full row capacity
  (THREAD-1). Per-cell SPI store failures during `parqit use`/`collect` now abort
  loudly instead of leaving a staged cell at its default value (THREAD-3). m:m
  merge pairs the using side deterministically (TT-2).
- **`parqit describe` also returns `r(n_columns)`** as a pq-compatible alias of
  `r(n_cols)` (PARITY-5).

### Changed (earlier 2026-06-14 round)
- README qualifies the "lossless" round-trip claim (extended-missing *cell
  identity* `.a`–`.z` is, by the v1 contract, not preserved — labels are);
  `ASSUMPTIONS.md` records the new translator/merge/locale decisions and the
  remaining latent items. The release workflow now installs the static
  libstdc++ archive and fails the build if the shipped Linux plugin links
  libstdc++/libgcc_s dynamically (REL-1), and refuses to publish a release whose
  git tag disagrees with the project version (REL-3). `verify_suite/v21`–`v26`
  use the runner-passed paths (no hardcoded machine paths / private data) (REL-2).

## [0.1.3] — 2026-06-14

### Added
- **Inline performance tips.** When parqit spots a large operation that has a
  faster route, it prints one truthful, English, one-line tip pointing to it —
  e.g. a big `parqit mergein` suggests doing the join out of core in DuckDB and
  collecting only the result; a large `parqit open _data` suggests
  `parqit mergein`/`appendin` for small-lookup joins. Tips fire only above ~1M
  rows (normal use stays quiet); `global PARQIT_NOTIPS 1` mutes them. A new help
  section ({help parqit##perf}) discusses the in-memory ⋈ disk trade-off with
  worked examples. Verify test `v26_perf_tips`.
- **`parqit mergein` / `parqit appendin`** — join the *in-memory* Stata dataset with
  a disk file (Parquet/CSV/.dta/.xlsx) **fast**, when the disk side is the
  smaller (lookup) one. Instead of round-tripping the in-memory data through the
  DuckDB bridge (`parqit open _data`, ~8.8 s for 10M×13), the in-memory master
  stays put: parqit reads only the needed columns of the disk side (projection
  pushdown) into a throwaway frame, then a **native** `merge`/`append` runs.
  Syntax mirrors native merge: `parqit mergein m:1 key using lookup.parquet,
  keepusing(rate) nogenerate`. On 10M ⋈ 500k this is **≈3.4 s vs ≈9.6 s** for the
  bridge path — result byte-identical (verify test `v25_mergein_appendin`,
  checked `cf _all` against a native merge). A failed disk read leaves the
  in-memory data intact. For *big-on-big* prefer the out-of-core `parqit use …;
  parqit merge` path. See `ASSUMPTIONS.md` #42.

- **Read non-Parquet inputs.** `parqit use` and the `using` side of
  `parqit merge`/`joinby`/`append` now accept, by file extension:
  - **delimited text** (`.csv`/`.tsv`/`.txt`) — scanned *out of core* with
    DuckDB `read_csv_auto` (schema/delimiter auto-detected), exactly like
    Parquet: the file may exceed memory and only the needed columns/rows are
    touched;
  - **Stata `.dta`** and **Excel `.xls`/`.xlsx`** — not engine-scannable, so
    parqit imports them into a throwaway frame (the working dataset is untouched)
    and snapshots them to a small Parquet *bridge* the engine scans, carrying
    their variable/value labels and formats. Best for a small side (a lookup
    `.dta`, an `.xlsx`); for a large `.dta` master prefer `use` + `parqit open
    _data`. Bridges are swept up at `parqit close _all`.

  This makes the lazy-master idiom span formats: keep a large Parquet/CSV master
  out of Stata, `parqit merge … using lookup.dta`, and `parqit collect` only the
  result. SAS/SPSS remain out of scope. Verify test `v24_multiformat_sources`.
  See `ASSUMPTIONS.md` #41.
- **`parqit use … , relaxed`** — read a glob/file-set whose files have
  *different* schemas by union of column names (columns absent from a file
  arrive missing), mirroring pq's `relaxed`. Without it, a schema mismatch
  across the matched files stays a loud error (never a silent drop). Compiles
  to DuckDB `read_parquet(…, union_by_name = true)`; the F2 metadata-sizing
  fast path still holds (a column present in only some files falls back to a
  scan). Verify test `v23_relaxed_union_by_name`. See `ASSUMPTIONS.md` #40.
- `PARQIT_FILL_THREADS` environment override for the fill worker count:
  `0`/`1` forces the serial path; `n>1` forces that many workers
  (default `min(cores, 8)`). An escape hatch and a tuning knob for
  atypical very wide / string-heavy reads. Read from the OS environment
  (`getenv`) — set it in the shell before launching Stata, not as a Stata
  `global` — and now documented in the help Performance tips and the README.
- Verify test `v20_parallel_fill` — a 1.5M-row parallel read checked
  cell-for-cell (positions, aggregates, Inf/null tallies) against an
  independent pyarrow oracle.
- Verify test `v21_collect_passthrough_sizing` — `parqit use` vs
  `parqit use`+`parqit collect` must be byte-identical (storage type, format,
  value signature) across all-numeric, int/double/string/DATE,
  uint32/decimal/dup-name files and a multi-file glob.
- Verify test `v22_collect_date_no_overflow` — a Parquet DATE column
  spanning 1900–2099 collects to `long` with the exact day-count on both
  paths (independent oracle), guarding the date-aware storage floor.

### Changed
- **Faster `parqit save … , data` and `parqit open _data`** (the in-memory →
  Parquet path). The Stata→DuckDB transfer now fills DuckDB data chunks in
  column-vector batches (2048 rows) and appends them whole, instead of one
  `duckdb_append_*` call per cell. On a 10M×13 in-memory dataset the write
  drops **≈8.8 s → ≈7.4 s**; every type / date / period / missing conversion is
  byte-identical (the roundtrip suite and V03/V05/V06/V15/V19 verify it). The
  remaining cost is the per-cell `SF_vdata`/`SF_sdata` read out of Stata memory
  (~5.5 s/10M), which — unlike the `SF_vstore` *writes* the parallel fill uses —
  is **not safe to call from worker threads**, so it stays single-threaded; a
  future bulk path (Mata `st_data`, which extracts the same columns in ~0.2 s)
  can lift it. See `ASSUMPTIONS.md` #42.
- **`parqit use FILE` + `parqit collect` no longer re-scans to size columns**
  when the view is an untouched full-file passthrough. The collect path
  now reuses the same Parquet row-group-statistics sizing the direct
  `parqit use …, clear` path uses (the F2 metadata path), instead of a
  redundant second full scan. On the all-numeric 47.6M×8 reference file
  this closes the gap to the direct read (`use`→`collect` ≈+0.24 s →
  ≈+0.007 s, same-session min-of-6); string-heavy files were already
  scan-bound and are unchanged — the residual sizing scan only ever
  narrows, so no read regresses. Output is byte-identical to the direct
  path (storage type, format, values). See `ASSUMPTIONS.md` #38.
- **Parquet → Stata reads now fill in parallel** (`parqit use …, clear`,
  `parqit collect`). The materialiser runs a producer/consumer pipeline: the
  calling thread fetches + Arrow-converts DuckDB chunks while a pool of
  worker threads stores whole chunks into the staged frame through the
  per-cell SPI, overlapping the engine scan with the fill. On the 47.6M×8
  reference file `parqit use` drops **≈2.9 s → ≈1.5 s** (same session, ~49%).
  No feature or precision change — `fill_column` is reused byte-for-byte,
  so every type/missing/Inf/NUL rule is identical to before; only the
  scheduling differs, and reads below 50k rows keep the serial path. The
  technique follows the prior art `stata_parquet_io`, which stores from
  many threads over disjoint rows (the SPI store is reentrant for distinct
  cells). See `ASSUMPTIONS.md` #37.

### Fixed
- **A Parquet DATE column could collect as Stata `int` and overflow** for
  dates past ~2049. `parqit use` already stored such columns as `long`; the
  `parqit collect` path range-refined the cast day-count down to `int`. The
  collect overlay now restores the date-aware `long` floor for `%td`
  columns with no recorded Stata type, matching `parqit use` exactly
  (parqit-written files were already governed by their stored type). Found
  while verifying the passthrough-sizing change above. See
  `ASSUMPTIONS.md` #39.

## [0.1.2] — 2026-06-12

Broad-spectrum adversarial testing round (joins on missing keys, IEEE
specials, hostile names/payloads, locale, 2500-var width, strL
boundaries) — new suites `v14`–`v19` in `tests/verify_suite/`.

### Fixed
- **float32 values beyond Stata's float range** (±1.70e38 < |v| ≤ ±3.40e38)
  silently became missing: the observed-range pass now widens such
  columns to `double` with a note (finite-only range via `isfinite`).
- **±Inf → missing was silent**: the load now prints a per-column count
  ("Stata has no infinity"). NaN stays a silent NA by convention — it is
  how parquet writers encode float NA.
- **Embedded NUL bytes truncated str# values silently** (the SPI is
  C-string): truncation is now counted and reported per column.

### Added
- `examples/parqit_tour.do` finds the repo-local `ado/plus/p` tree by
  itself when parqit is not yet on the adopath (no arguments needed).

## [0.1.1] — 2026-06-12

Fixes for the findings of an independent adversarial audit
(`PARQIT_AUDIT_REPORT.md`); regression suite in
`tests/integration/t10_audit_fixes.do`.

### Fixed
- **PARQIT-01 (S0):** `parqit open _data` wrote every promotion to one shared
  bridge file, so a later `open _data, name()` silently rebound earlier
  named views to the newest dataset. Each promotion now snapshots to a
  unique file owned by its view; the plugin erases it when the view is
  closed or replaced.
- **PARQIT-02 (S0):** `parqit reshape long` accepted duplicate `i()` rows that
  native Stata rejects, fabricating long data. It now enforces Stata's
  uniqueness contract eagerly (same pattern `wide` already used for
  `(i,j)`).
- **PARQIT-03 (S2):** `parqit misstable` returned a 0/1 flag in
  `r(n_complete)`; it now returns the true complete-observation count
  (row-wise non-missing across the selected variables) and prints it.
- **PARQIT-04 (S2):** `parqit tabulate a b, row col` accepted but ignored the
  options; row/column percentage panels are now printed.
- **PARQIT-06 (S3):** `stata_call` gained a catch-all exception boundary —
  no C++ exception can cross the `extern "C"` SPI boundary and kill Stata;
  `selftest throw` fault-injects to prove it.
- **PARQIT-07 (S4):** README said `collect` consumes the view; aligned with
  the actual (and documented-in-help) semantics: the view stays open.

### Added
- **`parqit save …, chunk(#)`** (PARQIT-05): the documented option now exists —
  target rows per Parquet row group via the engine's `ROW_GROUP_SIZE`
  (rounded by DuckDB to 2048-row vector multiples).

## [0.1.0] — 2026-06-12

First working release: the complete public surface of the design README,
green on the full correctness suite (553 C++ assertions executed against
the embedded engine; 17 Stata batch suites on StataNow 19.5 MP with
pyarrow/duckdb as independent oracles, covering every applicable invariant
of the pq 3.0 audit charter).

### Added
- **Self-verifying tour** (`examples/parqit_tour.do` +
  `examples/make_data.py`): eleven sections exercising every feature over
  small artificial datasets (incl. a hostile-schema file), with native
  Stata as the oracle for each lazy result; runs in installed or
  development mode. `parqit egen` now accepts a storage-type prefix
  (`parqit egen double x = …`), carried as the collect-time type like gen.
- **Extended exploration kit** (all push-down; views never mutated,
  memory never touched): `parqit count if <exp>` (filtered count without
  touching the pipeline); `parqit list [varlist] [if] [in f/l]`
  (non-mutating preview with Stata's slice-then-filter semantics);
  `parqit ds` and `parqit lookfor` (names/labels); `parqit codebook`
  (kind/obs/missing/distinct/min/max/label per variable, one scan);
  `parqit distinct [, joint]`; `parqit duplicates report|list`;
  `parqit misstable patterns`; `parqit tabulate` gains native-style missing
  exclusion plus `missing`, `row`, `col`; `parqit tabstat varlist,
  statistics(…) by(…)` (n mean sd var sum min max range median p##);
  `parqit correlate` (listwise) and `parqit pwcorr [, obs sig]` (pairwise,
  exact t-based p-values); `parqit histogram [, bins() nodraw]` — bins
  computed by the engine, only the bin table reaches Stata, drawn with
  twoway bar.
- **Exploration kit on lazy views** (all push-down; nothing materialised,
  memory untouched): `parqit summarize, detail` (variance, skewness,
  kurtosis and p1–p99 with Stata's exact moment and percentile
  definitions — verified against native `summarize, detail` r() to
  1e-10), two-way `parqit tabulate a b` (cross-tab with totals, ≤30
  columns), `parqit misstable [varlist]` (missing count/share per variable;
  strings count ""), and `parqit levelsof var [, limit()]`
  (`r(levels)` with levelsof-style quoting).
- **Views as using sides**: `parqit merge`/`joinby`/`append` accept
  `view:<name>` wherever a using file is expected — the other view's
  pipeline embeds as a subquery, so filtered-view-to-filtered-view joins
  run as one out-of-core query with nothing materialised. Uniqueness
  contracts and pending-range validation apply to view sources; a view
  can be merged with itself; `append` mixes files and views freely.
- **Named views** (frames-like vocabulary): `parqit use using <files>,
  name(qp)` opens/replaces a named lazy view and makes it current (also
  `name()` on `parqit sql` and `parqit open _data`); `parqit view <name>`
  switches, `parqit view <name>: <command>` runs a one-off against another
  view and restores the current one, `parqit views` lists open views,
  `parqit close [<name>|_all]` closes. Verbs always operate on the current
  view; plain `parqit use <file>, clear` reads never touch views.
- M4 power features: `parqit reshape long|wide` (long keeps missing cells
  like Stata via per-j UNION; wide enforces Stata's contracts — (i,j)
  uniqueness, no stray variables — and scans j values up front);
  `parqit sql "…"` opens a view over any DuckDB query (boundary-cast like a
  file source; `, clear` collects immediately); `parqit query "…"` injects a
  raw fragment (e.g. QUALIFY) with fail-fast validation; `parqit summarize`
  and `parqit tabulate` pushdown summaries with `r()` results; `parqit path`;
  the full SMCL help file (`parqit.sthlp`).
- M3 two-table verbs, all lazy with the using side on disk: `parqit merge
  1:1|m:1|1:m|m:m <keys> using <file>` with a Stata-compatible `_merge`
  (missing keys match missing keys via IS NOT DISTINCT FROM), uniqueness
  contracts validated loudly up front, `keep()`, `keepusing()`, `gen()`,
  `nogenerate`, master-wins column collisions (warned), value-label
  definitions carried across; m:m implements Stata's sequential pairing
  exactly (spine + clamped row lookups). `parqit append using <files>`
  (several files, UNION ALL BY NAME, missing columns null, loud
  string/numeric conflicts, `generate()` source marker). `parqit joinby
  <keys> using <file>` (within-key cartesian).
- M2 lazy view + single-table verbs: `parqit use using <files>` now opens a
  lazy view (nothing read; `, clear` still materialises immediately);
  verbs `keep`/`drop` (varlists with wildcards, `if` expressions, `in`
  ranges), `gen` (with storage-type request and `if` qualifier),
  `replace`, `rename`, `order`, `sort`/`gsort`, `collapse` (mean sum sd
  count min max median pNN first last firstnm lastnm — percentiles follow
  Stata's exact rule), `contract`, `duplicates drop`, `sample`
  (reservoir, `count`, `seed()`), `egen` (total mean sd min max count,
  `by()`); materialisers `collect` (one spillable execution, atomic
  swap-in), `save` (pipeline → Parquet, Stata memory untouched), `count`,
  `head`/`list`, `show` (dbplyr-style CTE pipeline), `explain`,
  `describe` (view form); `parqit open _data`; `parqit close`; `parqit set
  statamissing|threads|memory_limit|tempdir`.
- Stata-expression → SQL translator (C++, exhaustively unit-tested by
  executing translated SQL against the engine): operators with Stata
  precedence, missing-literal rewrites (`x < .` et al.), `statamissing`
  mode emulating "missing sorts high", strings-as-"" semantics, 40+
  functions incl. byte-correct strlen, Stata-exact mod/round/cond/
  inrange/inlist, and td()/tm()/tq()/th()/tw()/ty()/tc() literals; _n/_N
  as windows over the declared sort. Dates are day counts and datetimes
  millisecond counts inside pipelines (Stata semantics), converted only
  at the Parquet boundary.
- M1 plain I/O: `parqit use` (whole file, glob or hive directory; optional
  varlist = named columns in named order), `parqit save` (in-memory dataset →
  Parquet with `replace`, `compression()`, `compression_level()`,
  `partition_by()`), `parqit describe` (schema, rows, row groups, files,
  `r()` scalars). Full §4 type map with observed-range integer/string
  sizing; `%td`→DATE, `%tc`→TIMESTAMP, `%tm/%tq/%th/%tw/%ty/%tb` stay
  INTEGER and `%tC` BIGINT period counts; DECIMAL→double; UINT32 via
  BIGINT (never overflow-nulls); TIME→ms-since-midnight; LIST/STRUCT et
  al. dropped loudly. Stata metadata (variable/value labels incl. extended
  missings, notes, chars, formats, data label, original column names)
  round-trips via `parqit.*` Parquet KV metadata. Atomic loads (tempframe →
  validated swap), loud rc on every failure path, written files verified
  by a fresh scan before success is reported.
- Verify suite (ported from the pq 3.0 audit, with pyarrow oracles):
  renamed/awkward column names carry data, period dates never mis-scaled,
  uint32/uint64 overflow, save errors loud, atomic clear, duplicate
  columns disambiguated, unsupported types loud; plus full-type roundtrip
  and shape/option integration tests.
- M0 skeleton: repository layout, CMake build (DuckDB 1.5.3 built from
  source at a SHA256-pinned tag with parquet + core_functions statically
  linked, Stata Plugin Interface 3.0, Arrow C Data Interface header,
  nlohmann/json), cross-platform CI, `parqit` ado dispatcher, and a plugin
  that loads in Stata and reports engine/plugin versions (`parqit version`,
  `parqit selftest`).

### Changed
- **`parqit collect` no longer consumes the view** (dbplyr semantics): the
  pipeline stays open for further exploration or re-collection. The
  silent-stale-save hazard that motivated consumption is now handled by
  explicitness: with a view open, `parqit save` materialises the current
  view and names it in its output, and the new `data` option exports the
  in-memory dataset instead.

### Fixed
- Plugin resolution from an installed PLUS directory: `findfile` returns
  sysdir paths like `~/ado/plus/p/parqit.plugin`, which Stata's plugin
  loader hands raw to dlopen (no tilde expansion → rc 601). The ado now
  expands a leading `~/` via `$HOME` before loading. Found by the
  first clean-environment install check; every test had masked it by
  using an absolute `$PARQIT_PLUGIN_PATH`.
