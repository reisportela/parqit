# A2 — variable NAMES and METADATA fidelity (ENC-2 + NAME-CASE-1) — adversarial audit 2026-08-22

Auditor dimension A2. Tree: repo-local install `ado/plus/p` (plugin built 2026-08-22 16:10), Stata MP 19.5 batch,
pyarrow 24.0.0, duckdb CLI v1.5.0-dev (oracle only). All experiments live in
`local/audit_2026-08-22/A2/` (`a2_01` … `a2_08`, `probe_*.do`, fixtures from `mk_foreign.py`); every
check prints `PASS/FAIL <id>` and each log ends with a SUMMARY line. Checks that FAIL only because of
my own expectation/test errors were corrected and re-run; the residual FAILs map 1:1 to the findings
below. Nothing outside `A2/` was written; no tracked file was modified.

Totals: 8 do-files, ~330 individual checks, ~70 distinct hypotheses. Findings: **4 × S0, 3 × S1,
8 × S2, 0 × S3**. Three orchestrator items (R1/R2/R3) were re-confirmed independently and are folded
in (A2-4, A2-10, A2-12) with new consequences.

---

## Findings (ranked by data-integrity impact)

### A2-1 — S0 — DuckDB's *nested* dedup suffix defeats the exact-name recovery: single file, silent rename + metadata loss
- **Repro:** `A2/probe_nested_dedup.do` (fixtures `nest_n1.parquet` = columns `a, a_1, A`; `nest_n4` = `nuemp, nuemp_1, NUEMP`; `nest_n5` = `a_b, a_b_2, A_B, A_b`, each with a `parqit.schema` carrying labels). Also `a2_08_direct_path.do` check 8.3 with a file **parqit itself wrote** (`direct_san.parquet` / `out_sanitize.parquet`, names `a_b a_b_2 _1x _1x_2 A_B A_b`).
- **Observed:** eager `parqit use … , clear` and lazy `use`+`collect` load `a a_1 A_1_1` (label of `A` lost, no `src_name`, no note), `nuemp nuemp_1 NUEMP_1_1`, `a_b a_b_2 A_B A_b_2_1`. The DuckDB **parquet reader** dedups `A` → `A_1` (taken) → `A_1_1` (nested), whereas the binder dedup used by the unit tests yields `A_3`. `plan_columns`' positional recovery `is_dedup_of(scan, leaf)` accepts only `<leaf>_<digits>`, so `A_1_1`/`NUEMP_1_1`/`A_b_2_1` are not mapped back, the sanitiser sees a "new" legal name, and the KV manifest (`src = "A"`) no longer binds → label/format silently dropped.
- **Expected:** names `a a_1 A` (…`nuemp nuemp_1 NUEMP`, `a_b a_b_2 A_B A_b`) with their labels, as v70 promises.
- **Regression from today?** No (before today every clash became `NUEMP_1`); today's fix is incomplete.
- **Fix location:** `src/plugin/plugin_io.cpp` `plan_columns` (`is_dedup_of`): accept `<leaf>(_<digits>)+`, or map positionally whenever leaf-count == scan-count and `ascii_lower(scan)` starts with `ascii_lower(leaf) + "_"`; the same lambda is reused by `cmd_view_open`/`prepare_using` via `meta_ctx.parquet_names`. Add a unit test that feeds the *reader's* dedup shapes; extend v70 with `[a, a_1, A]`.

### A2-2 — S0 — Multi-file sources (glob, Hive tree, `relaxed`, `describe <glob>`) still load `NUEMP_1` and drop its metadata, silently
- **Repro:** `a2_01_names_foreign.do` 1i.1–1i.9 (`glob/g*.parquet` written by pyarrow; `pglob/p*.parquet` written by **parqit** with `label var NUEMP` + `format`), `a2_05b_footer_rest.do` 5c.1–5c.4 (Hive tree built by DuckDB from a parqit file; `relaxed` glob).
- **Observed:** `parqit use using "glob/g*.parquet", clear` → `nuemp NUEMP_1 s` (no note); lazy collect same; `parqit describe "pglob/p*.parquet"` → `nuemp NUEMP_1`; on the parqit-written glob the label `Upper label` and `%12.0f` of `NUEMP` are **lost**. Single-file reads of the same files are exact.
- **Cause:** `plan_columns` only recovers true names when `SELECT name FROM parquet_schema(paths) WHERE num_children=0` returns exactly `ncol` rows; with k files it returns k·ncol rows, so the recovery is skipped.
- **Expected:** `nuemp NUEMP s` + metadata, like the single-file case (this is the `qp_*.parquet` panel workflow the live finding came from).
- **Regression?** No — but the NAME-CASE-1 contract in README/help/CHANGELOG is stated unconditionally.
- **Fix location:** `plugin_io.cpp` `plan_columns`: read leaf names from one file (`parquet_schema(first path)`; the strict schema gate already proves all files identical; relaxed mode needs a union-by-name with the same per-file recovery) instead of gating on the row count.

### A2-3 — S0 — `reshape wide` / `pivot` over an aliased stub: the view save writes a Parquet file with DUPLICATE column names (rc 0); collect dies rc 110; pivot leaks the alias into generated names
- **Repro:** `A2/probe_wide_save.do`, `a2_02_alias_verbs.do` 2i.3–2i.6 (`long_xX.parquet`: `id j x X`; `parqit reshape wide x X_1, i(id) j(j)`; `parqit pivot (mean) X_1, rows(id) col(j)`).
- **Observed:** reshape wide accepted; `parqit describe` shows `X_11`, `X_12` **both** tagged "(Stata name X …)"; `parqit save out_wide_alias.parquet, replace` returns rc 0 and pyarrow reports names `['id','x1','X','x2','X']` — `pq.read_table` fails "Can't unify schema with duplicate field names"; `parqit use` of that file loads `X` and `X_1`; `parqit collect` instead stops "variable X already defined" r(110). `pivot` + collect/save yields `X_11 X_12` (alias embedded in the generated names; native would be `X1 X2`).
- **Cause:** `View::reshape_wide` copies the stub `ViewCol` (`nc = sc`) including `.stata`, then renames only `.name`; `view_kv_fragment`/`cmd_view_save` build `leaf_names` from `exposed()` without a uniqueness check before `parquet_rename_leaf_columns`.
- **Expected:** generated names `X1 X2` (from the Stata name), unique; or a loud refusal. Never a duplicate-name file at rc 0.
- **Regression?** Yes (new `.stata`/footer-rename code).
- **Fix location:** `src/engine/view.cpp` `reshape_wide` (clear `.stata`, derive the generated name from `exposed()`), `cmd_view_pivot`; defensive: `plugin_view.cpp` `cmd_view_save` refuse when `leaf_names` are not unique (also guards `parquet_rename_leaf_columns`).

### A2-4 — S0 — `rename` of an aliased column keeps the stale Stata-facing name in collect, describe AND the written file (= orchestrator R1, confirmed; file consequence is new)
- **Repro:** `a2_02_alias_verbs.do` 2f.1–2f.5; `a2_06_misc.do` 6b.1/6g.1 (`base.parquet`; `parqit rename NUEMP_1 foo`; `parqit save out_ren.parquet`).
- **Observed:** collect → `… NUEMP …` (no `foo`); view save → pyarrow names contain `NUEMP`, not `foo`; group form `(nuemp NUEMP_1) (lo hi)` → `lo NUEMP`; notes do not follow. All rc 0.
- **Regression?** Yes. **Fix:** `View::rename`/`rename_many` clear `.stata` and move `chars_[exposed()]`.

### A2-5 — S1 — `collapse` default targets of aliased sources are named by the ALIAS in collect and in the saved file
- **Repro:** `a2_02` 2g.4; `a2_06` 6b.2/6g.2 (`parqit collapse (mean) NUEMP_1 (first) S_1, by(grp)` → collect `grp NUEMP_1 S_1`; view save file names `['grp','NUEMP_1','S_1']`).
- **Expected:** `NUEMP S` (the help: "collect and save translate back"); `collapse by(NUEMP_1)` does give `NUEMP` (2g.6 PASS), so the two branches disagree.
- **Regression?** Yes. **Fix:** `View::collapse` — default target = source's `exposed()` (ci-guarded) or carry `.stata` when target == source name.

### A2-6 — S1 — `append …, gen(Val)` clashing only by case with a using-side column is accepted; the view is mutated and `collect` then fails with a raw DuckDB Binder Error
- **Repro:** `a2_07_clash_refusals.do` 7.13/7.14, `a2_08` 8.6 (`plain.parquet` master; `u2.parquet` has `val`; `parqit append using u2.parquet, gen(Val)` rc 0; `parqit collect` → "Binder Error: UNION (ALL) BY NAME … the name "Val" occurs multiple times").
- **Expected:** refused before mutation, like `merge gen(VAL)` (7.11 PASS). CHANGELOG says append gen is covered.
- **Fix:** `View::append_with` — `ci_clash(gen_name, c.name)` against every using side (TT-A2 only checks exact equality).

### A2-7 — S1 — ENC-2: a legacy note/characteristic that grows past Stata's 67,783-byte characteristic limit when transcoded is written correctly but silently LOST on read-back
- **Repro:** `a2_04_encoding.do` 4g.1–4g.5 (40,000-byte Latin-1 `é` note → file holds the 80,000-byte UTF-8 note (pyarrow 4h.6 PASS) → `parqit use`/`collect` → `char _dta[note1]` empty, rc 0, no message). Same for a foreign char of 70,000 bytes (`a2_03` 3e.9). Native probe: Mata `st_global` stores nothing for ≥ 67,784 bytes (rc 0).
- **Expected:** truncate with a note, like the existing 32,000-byte value-label and 80-char label guards.
- **Regression?** Yes (the transcoding path creates the case; before, such data was refused loudly). **Fix:** `parqit.ado` `_parqit_resp_decorate` (char branch): guard `strlen(value) > 67783` → truncate + note; optionally note on the write side.

### A2-8 — S2 — `.dta`/`.xlsx` bridge (`parqit use using x.dta`, two-table `using x.dta`) and `parqit open _data` transcode legacy text SILENTLY and with no `encoding()` choice
- **Repro:** `a2_04` 4f.1–4f.6 (`enc_legacy.dta` with `caf\xE9`, legacy label and note; logs `enc_bridge.log`/`enc_open.log` contain no "transcoded" note). Data arrives correctly transcoded with cp1252.
- **Cause:** `_parqit_import_to_bridge` and `_parqit_open` run `parqit save …, data` under `quietly`; `_parqit_open` re-surfaces ext/frac notes but not `r(transcoded_*)`; no `encoding()` option is plumbed.
- **Expected:** the same loud note as a direct save, and `encoding()` on `use`/`open _data`/two-table verbs (a MacRoman `.dta` cannot be read correctly today).

### A2-9 — S2 — `parqit.schema.sortedby` written by a view save carries engine ALIASES; a later lazy `use` loses the sort marker
- **Repro:** `a2_02` 2d.1–2d.4, 2d.8, 2k.7 (`parqit sort S_1 NUEMP_1` → save → KV `sortedby = ["S_1","NUEMP_1"]`; eager reopen shows `S NUEMP` by coincidence (scan names happen to equal the aliases), lazy reopen + collect → sortedby empty; plain view save of `base.parquet` writes `["NUEMP_1","nuemp"]`).
- **Fix:** `View::sortedby_names()` (view.cpp) map quoted engine names → `exposed()`; `cmd_view_open` then restores via `original_to_live`.

### A2-10 — S2 — A column that is both sanitised AND case-aliased loses its true source name on the lazy path and gets the alias stamped as `src_name` (extends orchestrator R2); the wrong `src_name` is then written into the re-saved file
- **Repro:** `a2_01` 1d.6b/1d.8 (`f_sanitize_clash.parquet`: `A B` → Stata `A_B`, alias `A_B_1`; lazy collect → `char A_B[src_name] = "A_B_1"` instead of `"A B"`; pyarrow of `out_sanitize.parquet`: `parqit.chars.A_B.src_name = "A_B_1"`); `a2_02` 2b.5 (plain `keep` + collect → `NUEMP[src_name] = "NUEMP_1"`). Eager path is right (1d.3 PASS).
- **Fix:** `cmd_view_open` (record `src_name` for a renamed aliased column too); `cmd_view_collect_prepare` set `ctx.parquet_names[p.source_name] = vc.stata` when overriding the Stata name.

### A2-11 — S2 — Value-label definitions not attached to a saved variable are silently dropped
- **Repro:** `a2_03_metadata.do` 3a.8/3b.8/3c.8/3f.2 (`label define vl_orphan 7 "orphan"` → not in `parqit.vallabs`, not restored). Native `save` keeps unattached labels; README "Lossless metadata round-trips … value labels" and the help do not state the restriction.
- **Fix:** `_parqit_wr_save_request` (send `label dir`) or document the contract.

### A2-12 — S2 — Lazy `parqit use <varlist> using f`, `keep/drop/order`, `list` do not accept the exact Stata name of an aliased column (eager `use <varlist>` does) (= orchestrator R3, confirmed)
- **Repro:** `a2_02` 2l.1–2l.4, 2j.5 (`parqit use NUEMP nuemp using base.parquet` lazy → rc 198; eager → OK).

### A2-13 — S2 — `parqit sql "SELECT * FROM read_parquet(f)"` on a case-clashing file silently loads DuckDB's deduped names
- **Repro:** `a2_06` 6e.3 (`f_threeway.parquet` → view/collect names `x X_1 x_1_1 X_1_2`, no note; `DESCRIBE` cannot recover them). `parqit sql "SELECT 1 AS x, 2 AS X, …"` is exact (6e.2 PASS).
- **Fix/Doc:** document that `sql` over a scan is DuckDB's naming, or detect `<name>_<k>` patterns and warn.

### A2-14 — S2 — `encoding()` on a view save is ignored even when invalid (`encoding(bogus)` → rc 0)
- **Repro:** `a2_04` 4e.2. Help: "Any other name is refused before anything is written." **Fix:** validate in `_parqit_save` before the view branch (or pass through `view_save`).

### A2-15 — S2 — Documentation/contract mismatches found while testing
1. Help "empty names become `v<position>`": an empty Parquet column name arrives as DuckDB's scan name `C<idx>` (`C6` in `a2_01` 1f.6) and gets no `src_name`.
2. Help "The original name is retained in `char var[src_name]` … so a later save can restore it": a save writes the **Stata** names (`a_b`, not `a b`); the original survives only in `parqit.chars` (`a2_01` 1d.7/1d.8; ASSUMPTIONS #62 says so — the help sentence is misleading).
3. CHANGELOG/ASSUMPTIONS #95(d): "merge/joinby … brought using columns" clashing by case are "refused" — actually a using `X` beside master `x` is aliased and **brought** (which is native-like and fine), `a2_07` 7.15–7.17.
4. `parqit contract grp` with an existing `_freq` column silently projects the old `_freq` away where native stops r(110) (`a2_01` 1g.5) — semantics only, counts right.

---

## What held (selected PASS evidence, by hypothesis)

| # | Hypothesis | Result | Where |
|---|---|---|---|
| H1 | Three-way clash `x X x_1 X_1` (foreign): eager/lazy names+values exact; alias `X_2`; re-save pyarrow-exact | PASS | a2_01 1a.* |
| H2 | Exact duplicate + case clash (`dup dup DUP s S`): names `dup dup_1 DUP s S`, src_name, values, re-save | PASS | a2_01 1b.* |
| H3 | Unicode names `año/AÑO/variável/VARIÁVEL` not folded (DuckDB is ASCII-only), exact everywhere | PASS | a2_01 1c.* |
| H4 | Sanitiser collisions `a b`/`a_b`/`1x`/`_1x`: deterministic `_2` suffixes, src_name (eager) | PASS | a2_01 1d.1–1d.5 |
| H4b | …same on the lazy path for the sanitised+aliased column | **FAIL** → A2-10 | 1d.6b, 1d.8 |
| H5 | Reserved words (`if _n in byte str5 strL _all`) prefixed; DuckDB keywords (`select from order group table`) kept; gen/keep/sort/collect/re-save | PASS | a2_01 1e.* |
| H6 | Hostile chars in names (`"` `` ` `` `$` `\|` newline tab `'`): sanitised, src_name exact incl. newline/tab, collect, re-save KV | PASS (empty name → `C6`, see A2-15.1) | a2_01 1f.* |
| H7 | Helper-pattern names (`__parqit_rn_1 _merge _freq __parqit_s0 __parqit_s1`): collapse (first/mean/count), gen `_n`, collect, re-save | PASS | a2_01 1g.* |
| H8 | 32-char names differing by case → alias 34 chars is addressable in keep/gen; 33-char names truncate+suffix; exact on collect/re-save | PASS | a2_01 1h.* |
| H9 | Multi-file glob / Hive / relaxed / describe glob keep exact names + metadata | **FAIL** → A2-2 | a2_01 1i.*, a2_05b 5c.* |
| H10 | Nested dedup shapes (`a a_1 A`, `nuemp nuemp_1 NUEMP`) recovered | **FAIL** → A2-1 | probe_nested_dedup, a2_08 8.3 |
| H11 | Alias addressing in keep/drop/order/sort/gsort/gen/replace/egen/contract/duplicates/query/count/list/summarize/tabulate/levelsof/ds/lookfor/codebook/misstable; exact names + label/format/vallab/notes/chars/sortedby on collect | PASS | a2_02 2b–2e, 2g.1–2, 2g.5–10, 2j.* |
| H12 | rename of aliased column (single + group form) | **FAIL** → A2-4 | a2_02 2f.*, a2_06 6b.1 |
| H13 | collapse default target / by() of aliased column | by() PASS; target **FAIL** → A2-5 | a2_02 2g.4/2g.6 |
| H14 | merge 1:1 on `id` with using carrying `nuemp/NUEMP`; merge on the alias key; merge on `nuemp` when using has only `NUEMP` refused; keepusing; append stacks `NUEMP`→`NUEMP`; joinby | PASS | a2_02 2h.* |
| H15 | reshape long with aliased stub | loud refusal "variable V1 is missing (unbalanced stubs)" — usability gap, not integrity | a2_02 2i.2 |
| H16 | reshape wide / pivot with aliased stub | **FAIL** → A2-3 | a2_02 2i.3–2i.6, probe_wide_save |
| H17 | View save: names exact in FILE (pyarrow), `parqit.schema` name/src = file names, chars keyed by Stata names, value labels, values; eager reopen restores everything incl. sortedby `NUEMP nuemp` | PASS | a2_02 2k.*, 2d.7 |
| H18 | KV sortedby of a view save names the FILE columns | **FAIL** → A2-9 | a2_02 2d.8, 2k.7, 2d.4 |
| H19 | open _data with clashing names: alias, collect exact + labels | PASS | a2_02 2l.5–2l.6 |
| H20 | Lazy `use <exact names>` | **FAIL** → A2-12 | a2_02 2l.3 |
| H21 | Value labels: texts with `"` `` ` `` `'` `$` `\|` `%` `{}` `\` and embedded newline; keys −1/0/2147483620/.a/.z; 32,000-byte text; shared label on two vars; all through save→use, lazy collect, view save→use; KV inspected | PASS | a2_03 3a–3c .1–.7, 3f.* |
| H22 | Unattached value label survives | **FAIL** → A2-11 | a2_03 .8, 3f.2 |
| H23 | Variable labels: 80 ASCII chars, 40 `é` (80 bytes), special chars; data label 80 chars; 80-`é` labels (160 bytes) after transcoding | PASS | a2_03 .9–.11/.17, a2_04 4g.2/4g.3 |
| H24 | Notes: 20 notes on a var (note0=20), 60,000-char `_dta` note, special chars; chars `_dta[a_b]`, `id[__x]` with dq+newline, 60,000-char value | PASS | a2_03 .12–.16 |
| H25 | Display formats `%td %tc %tC %tm %tq %th %ty %tw %9.2f %12.0gc %21x %-20.15g %-10s %~10s %10.0fc %tdCCYY-NN-DD %tcHH:MM:SS` round-trip (values unchanged) | PASS (`%tb` untested — needs a calendar file) | a2_03 .18–.20 |
| H26 | sortedby restoration; prefix rule when a lazy keep/drop or eager subset removes a key — matches native | PASS | a2_03 3d.* |
| H27 | Foreign hostile KV: 120-char label → 80, invalid label name skipped, 40,000-char text → 32,000 with note, non-integer keys skipped, `.a` key ok, invalid char name skipped, 100-char data label → 80, sortedby applied | PASS (loud) | a2_03 3e.* |
| H28 | Foreign char > 67,783 bytes | **FAIL** (silent) → A2-7 | a2_03 3e.9 |
| H29 | ENC-2: mixed valid+invalid cell → whole-string transcode (documented "item by item" = unicode translate behaviour, mojibake `cafÃ© é`); pure legacy cell; legacy value-label text AND NAME (`l\xE9x` → `léx` attached), notes on `_dta`/var, char, data label, var label; `str20` width kept when it fits | PASS | a2_04 4a.* |
| H30 | encoding() aliases: windows-1252/cp1252/cp-1252/windows1252/latin-1/ISO-8859-1/iso8859-1/iso88591/latin9/iso-8859-15/iso885915/macroman/mac-roman/mac_roman/macintosh/" latin1 " accepted; utf8/utf-8/latin2/cp850/ascii refused rc 198 | PASS | a2_04 4b.* |
| H31 | Transcoding widens `str2045` → `strL`, 4,090 bytes, round-trips (eager + lazy), pyarrow | PASS | a2_04 4c.*, 4h.4 |
| H32 | Valid UTF-8 byte-exact: U+FFFD, C1 (U+0080/U+009F), 4-byte emoji, U+FFFE/U+FFFF, U+10FFFF, BMP; overlong/surrogate/F5 treated as legacy | PASS | a2_04 4d.*, 4h.5 |
| H33 | encoding() on view save | accepted+ignored (documented); `bogus` **not refused** → A2-14 | a2_04 4e.* |
| H34 | `.dta` bridge / open _data / merge-using-.dta transcode | transcode PASS; **silent** → A2-8 | a2_04 4f.* |
| H35 | Transcoding growth past Stata limits (labels 160 bytes fine; note 80,000 bytes) | note **FAIL** → A2-7 | a2_04 4g.* |
| H36 | 2,497 vars × 10,000 rows with clashes (`v1..v1245`/`V1..V1245`, `name/NAME`, `nuemp/NUEMP/NuEmp`), zstd level 9, chunk(2048) → 5 row groups, footer 1,087,006 bytes: both writers; eager names in order, values/labels/notes identical; lazy alias keep+collect; pyarrow names unique/exact + values; duckdb CLI `parquet_schema` exact + reads; codec honoured | PASS | a2_05 5a.*, a2_05b 5d.* |
| H37 | partition_by() with clashes refused, nothing written, no `.parqit_*` leftovers; `replace` over an existing clashing file swaps cleanly; no-replace refusal rc 602 | PASS | a2_05b 5b.* |
| H38 | Fresh Stata session reads the footer-renamed files (threeway, big_a, base) exact incl. labels/sortedby | PASS | a2_06 6a.* |
| H39 | Unchanged-source (direct/fast) save path with clashing or sanitised names → general path; exact | PASS | a2_08 8.1–8.5 |
| H40 | `parqit sql "SELECT 1 AS x, 2 AS X, 3 AS x_1, 4 AS X_1"` exact | PASS; `SELECT *` over a clashing file **not** → A2-13 | a2_06 6e.* |
| H41 | mergein/appendin with a clashing disk side: exact names, right values | PASS | a2_06 6d.* |
| H42 | Loud refusals: collapse targets `a/A`, target vs by, contract freq(GRP)/freq(X), egen X vs x, gen ID vs id, rename x→Y, reshape wide j `A/a`, merge gen(VAL)/gen(MERGE); swap rename `(x y)(Y X)` allowed; view intact after refusals | PASS | a2_07 7.1–7.12 |
| H43 | append gen(Val) vs using val | **FAIL** → A2-6 | a2_07 7.13, a2_08 8.6 |
| H44 | No `ARROW:schema` KV in parqit files (pyarrow would otherwise trust it over footer names) | PASS | a2_01 1x.1 |
| H45 | Labels land on the right case-variant after a save+use | PASS | a2_06 6f.1 |
| H46 | Value-label text 16,001 `é` (32,002 bytes): native `label define` itself truncates to 32,000 bytes; parqit round-trips what Stata holds | PASS (no defect) | a2_06 6c.* |

Not tested (out of reach here): `%tb` business-calendar formats (needs a calendar file); a partition_by tree containing a clash is refused by design so no tree was exercised; injecting a footer-rename failure (no fault hook exists; the staged-file path was verified clean on success and on the partition refusal).

## Notes for the fixer
- A2-1/A2-2 are the two that bite real panels (`qp_*.parquet` globs; any file whose clash candidate `_1` is already a literal name). Both are in `plan_columns`' recovery block and its callers — one fix covers eager `use`, lazy `use`, `prepare_using`, `describe`.
- A2-3/A2-4/A2-5/A2-9/A2-10 share a root cause: `ViewCol.stata` is carried or created inconsistently across verbs (`rename` keeps it, `reshape_wide` copies it, `collapse` targets drop it, `sortedby_names`/`collect` ignore it). A single invariant helper (`exposed()` for every user-facing name; `.stata` cleared on any rename/regeneration) plus a `leaf_names` uniqueness assertion before the footer rename closes all five.
- A2-7/A2-8 are the ENC-2 tail: the read side needs the 67,783-byte char guard; the bridge paths need the note and an `encoding()` hand-off.
