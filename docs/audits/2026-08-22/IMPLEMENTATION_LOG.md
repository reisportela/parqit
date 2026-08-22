# Implementation log — parqit audit 2026-08-22 remediation

Implementer agent. Build target during work: `cmake --build build/dev --target parqit_plugin
parqit_tests -j 16`; final default `cmake --build build/dev -j 16` (refreshes ado/plus/p) after the
restriction was lifted. Unit tests: `./build/dev/parqit_tests`. Stata: `bash tests/run_stata.sh`.

Baseline performance (build/dev plugin 16:10, before any change; 5M rows × 82 vars,
`A6/w1_src.dta`, `impl/perf/perf_before.do`): save 22.2 s / 19.4 s (rep1/2); use 27.9 s / 28.9 s.

Status: all items DONE unless noted. Every fix has a pinning test; new verify tests v71–v77,
new unit tests in test_exprtrans/test_session/test_sanitize/test_typemap/test_view.

## G1 — data integrity (S0)
- **A3-1** merge common non-key var on using-only rows → `View::merge_with` projects
  `CASE WHEN __m.mm IS NULL THEN __u.c ELSE __m.c END` for a kept common column (both branches);
  string/numeric kind clash refused (rc 106). Files: `src/engine/view.cpp`. Tests: unit
  `MERGE-COMMON-1/A3-1` (test_view), verify `v71` (native cf, all keep()/keepusing(), both
  directions, m:m). Evidence: `VERDICT(V71_MERGE_COMMON_USING_ROWS): PASS`.
- **A4-1/A4-2/A1-2** copysource opt-in + hardened fingerprint. Automatic path removed; default
  save reads memory. `parqit save ..., data copysource` gate in `_parqit_save`; plugin
  `cmd_save_data_direct` re-verifies FileIdentity (abs/size/mtime/ctime/inode/footer FNV-1a) before
  and after COPY (pre_publish hook in `copy_out_parquet`), checks names/kinds/N/sortedby and
  first/last-64-row content, refuses non-reproducible datasets, writes the source's true sortedby,
  returns `r(copysource)`. Files: `src/ado/p/parqit.ado`, `src/plugin/plugin_io.{cpp,hpp}`.
  Tests: verify `v72`, integration `t11` (rewritten). ASSUMPTIONS #96. CHANGELOG Changed+Added.
  Evidence: `VERDICT(V72_COPYSOURCE_FINGERPRINT): PASS`, `VERDICT(T11_SAVE_FAST_PATH): PASS`.
- **A1-1/A1-11** %tc µs integer arithmetic → `stata_tc_ms_to_epoch_us` (typemap), used by
  `convert_save_numeric` WTs. Files: `typemap.{cpp,hpp}`, `plugin_io.cpp`. Tests: unit `TC-US-1`,
  verify `v74` E/F (pyarrow us==ms*1000, arrow+staged+lazy).
- **A3-2** real() grammar → regexp guard + `translate(dD,ee)` TRY_CAST in `exprtrans.cpp`. Tests:
  unit `REAL-GRAMMAR-1` (37 literals vs native), verify `v75`.
- **A3-3/A3-4** date-domain guards → `day_arg`/`mdy`/`dofm` in exprtrans BETWEEN guards + try().
  Tests: unit `DATE-DOMAIN-1`, verify `v75`. ASSUMPTIONS #97 (dow/doy native one-day quirk NOT
  reproduced — documented; the auditor's blanket claim was slightly off, see #97).
- **A1-3** partition-key type restore → `plan_columns` HIVE-TYPE-1 (eager) + `hive_boundary_override`
  (lazy, gated on the key arriving as text), float→FLOAT. Files: `plugin_io.cpp`, `plugin_view.cpp`.
  Test: verify `v74` (float/double/%tc/%td/%tm/int/byte/string keys × eager/lazy/view-save).
  ASSUMPTIONS #98.
- **A1-5** zero-row partition_by → `copy_out_parquet` writes an empty tree. Test: verify `v74`.
- **A2-1/A2-2** nested-dedup + multi-file name recovery → `is_dedup_of` accepts `<leaf>(_\d+)+`;
  leaf names read from ONE file; Hive partition columns tracked in `ctx->hive_columns`. Files:
  `plugin_io.cpp`. Tests: verify `v70` (extended), `v74`.
- **A2-3** reshape wide/pivot alias invariant + defensive refusal → `derive_col` clears `.stata`;
  generated names from `exposed()`; `cmd_view_save`/`cmd_view_collect_prepare` refuse duplicate
  exposed names. Files: `view.cpp`, `plugin_view.cpp`. Tests: unit `A2-3` (test_view), verify `v75`.

## G2 — S1
- **A4-3** torn-read identity check → `snapshot_source_files` before planning; `prepared_files_changed`
  re-checks before+after fetch; fetched column types compared with plan. Files: `plugin_io.{cpp,hpp}`,
  `plugin_view.cpp`. Test: verify `v73` (concurrent writer, 60 reads, 0 inconsistent).
- **A3-5** levelsof alias collision + full alias audit → `__parqit_*` helper aliases in levelsof/
  tabulate/tab2/misspatterns/tabstat/histogram/wide_j_scan. Files: `plugin_view.cpp`. Test: `v75`.
- **R1/A2-4** rename of aliased column → `View::rename`/`rename_many` use `derive_col` + `move_chars`
  (chars keyed by exposed name). Test: unit `R1/A2-4` (test_view), verify `v70`.
- **A5-1/A2-14** encoding() on view save → forwarded in the view-save request, validated in
  `cmd_view_save` before any write. Files: `parqit.ado`, `plugin_view.cpp`. Test: verify `v76`, `v32`.
- **A5-3/A5-2/A2-8** bridge losses loud + encoding() → `_parqit_import_to_bridge` reads and reports
  the bridge save's r(), `_parqit_lossy_notes` gains `source()`, encoding() plumbed to use/merge/
  joinby/append/open_data; all return the losses. Files: `parqit.ado`. Test: verify `v77`.
- **A1-4** str not reserved → removed from `kReserved` (sanitize.cpp). Test: unit `NAME-STR-1`, `v74`.
- **A4-4** long dest names → digest-keyed lock/staging siblings past NAME_MAX. Test: verify `v76`.
- **A4-5** dest inside directory source refused → SAVE-SELFDIR-1 in `cmd_view_save`. Test: `v76`.
- **A2-5** collapse default targets keep the source's exposed name; explicit targets clear .stata.
  Test: unit `A2-5/A3-8` (test_view), verify `v75`.
- **A2-6** append gen() ci-clash vs using columns refused. Test: unit `A2-6`, verify `v75`.
- **A2-7** char > 67,783 bytes truncated with a loud note on read (`_parqit_resp_decorate`). `v77`.
- **R2/A2-10** src_name on lazy path → collect sets `ctx.parquet_names[source]=vc.stata`; open
  records src_name whenever exposed()!=original independent of the alias note. Test: `v70`, `v47`.

## G3 — S2
- **A1-6** storage-type parity eager/lazy → TYPE-PARITY-1 (plugin_io range-sizes recorded
  byte/int %td and narrow/float %tc; lazy collect matches). Test: `v74` H.
- **A1-7** rounding exact ≥2^52 → `stata_round_temporal` (typemap), both writers + compile_for_save;
  the fractional note no longer fires for exact integers. Test: unit `TEMPORAL-ROUND-1`, `v74`.
- **A1-8** partition columns keep manifest order → COLORDER-1 in plan_columns + cmd_view_open/
  prepare_using. Test: `v74`.
- **A1-9** ns pre-1970 floor → `timestamp_ns_floor_us_sql` (typemap), eager+lazy. Test: unit
  `TS-NS-FLOOR-1`, verify `v74` G.
- **A1-10/A2-11** unattached value labels written → `_parqit_save` passes `label dir` names. `v74`.
- **A3-6** levelsof/tabulate rendering → `_parqit_render_num` (%9.0g/format), `_parqit_build_levels`
  (%21.0g int / macro non-int), tab/tab2 header carries kind+fmt. Test: `v75`.
- **A3-7** string() denormal → underflow-safe `format_sci` (session.cpp). Test: unit
  `STRING-DENORMAL-1`, verify `v75`.
- **A3-8** collapse/merge/reshape metadata parity → collapse target label "(stat) src"+format,
  count→long, _merge %23.0g, reshape-wide "<j> <stub>"+format. Test: `v75`. CHANGELOG Changed.
- **A3-9** keep in f/l letters + negatives → `_parqit_op_keepin` resolves N via count. Test: `v13`.
- **A4-6** symlink write-through + read-only refuse (rc 608) → `copy_out_parquet`. Test: `v76`.
- **A4-7** hive `=` message → `friendly_engine_error`. NOTE: DuckDB 1.5.3 now READS `city=a=b`
  (no error), so the wrapper is a defensive net; `v76` asserts the tree reads (documented in test).
- **A4-8** POSIX publish hook + messages → hook now honoured on the flat path; lock-refusal message
  names the txn dir too. Test: `v76`.
- **A5-12** sql alias note channel → `cmd_view_sql` emits `note:` not `warning:`. Test: `v75`.
- **A5-13** `parqit use , <badopt>` names the option. Files: `parqit.ado`. Test: `v77` (R4 path).
- **A2-9** view-save sortedby exposed names → `View::sortedby_names` maps to exposed(). Test: `v70`.
- **A2-13** sql SELECT * over clashing file → note + documented. Test: (note path) `v75` sql check.
- **A2-15** (1) empty name→v<pos> already; (2)/(3) wording fixed in help/CHANGELOG; (4) contract
  existing _freq refused (CONTRACT-FREQ-1, view.cpp). Test: `v75`.

## G4 — docs (complete)
- Help (`parqit.sthlp`): syntax lines (copysource, encoding on use/merge/append/joinby/open, keep in
  f/l); prose for copysource semantics+nonce char, bridge conversions reported, partition key types,
  str not reserved, torn-read, in f/l, levelsof/tabulate rendering, collapse/merge/reshape metadata,
  metadata-restore guards paragraph, encoding aliases/canonical r(encoding), strL promotion, read-side
  UTF-8 refusal, ≥8.99e307 note, sql alias note, symlink/read-only save, Stored results (copysource,
  bridge losses). release_lint OK.
- README: command tables refreshed (collapse firstnm/lastnm, contract freq, duplicates force, merge
  nogenerate, append generate, sql name, appendin force, save copysource, use encoding, set/path/menu
  row); Limitations legacy-text + extended-missing bridge reporting.
- Dialogs: parqit_write lz4_raw + copysource checkbox; parqit_pivot firstnm/lastnm/p10/p50/p90;
  parqit_read + parqit_combine encoding() combobox.
- CHANGELOG [Unreleased] Changed/Added/Fixed; ASSUMPTIONS #96/#97/#98.

## Not changed / documented deviations
- A4-7 hive `=`: DuckDB 1.5.3 reads it fine now; wrapper kept defensively (evidence in v76).
- A3-3/A3-4 dow/doy: native extends one day past 31dec9999; parqit uses one clean domain window
  (ASSUMPTIONS #97) — deliberate, documented, astronomical edge only.

## Evidence
- Unit tests: `./build/dev/parqit_tests` → 94 cases / 3613 assertions PASS.
- release_lint: `release-lint OK: v0.1.27 (9aug2026 / pkg 20260809)`.
- Full Stata suite: 97 VERDICT lines, ALL PASS, 0 FAIL/abort/did-not-finish (integration +
  verify_suite v02-v77 + roundtrip + concurrent x01/x02). Output: impl/full_suite.out.
- Performance (5M rows x 82 vars, seed 1234; run alone): save 22.2/19.4 s -> 16.0/16.0 s;
  use 27.9/28.9 s -> 27.2/33.9 s. No regression on the hot paths (the per-cell save loop and
  the fetch are unchanged; copysource is opt-in; the %tc conversion moved to integer arithmetic;
  the torn-read guard adds only a few stat() calls at plan/fetch).

## Round 2 (2026-08-23) — residual items of VERIFY_REPORT.md / TRIAGE "Round 2"

Implementer (round 2). Build `cmake --build build/dev -j 16`; unit `./build/dev/parqit_tests`;
Stata `bash tests/run_stata.sh`. Scratch under `local/audit_2026-08-22/impl2/` (probe.do + its log
exercise every behaviour below end to end; `full_suite.out` is the final full run).

- **V2.2 relaxed exact-name recovery (S1) — IMPLEMENTED (contained).** plan_columns now has a
  relaxed branch (`RELAXED-NAMES-1`): per-file leaf names from `parquet_schema` (batch-ordered =
  DuckDB's file order), each file's names deduped with an exact replica of the Parquet reader's
  rule (`parqit::duckdb_reader_dedup`: running per-ci-name `_<counter>`), the union predicted with
  a replica of `UnionByName::CombineUnionTypes` (`parqit::duckdb_union_by_name`: first file's
  columns, then later files' new names, ASCII-ci matched; owner = first contributor), aligned
  positionally (dedup shape / `C<index>` for an empty leaf) and mapped back to the true leaf
  names (`ctx->parquet_names`). Hazards of the engine's ci union are loud: a later file's column
  absorbed into a case-variant prints `note: relaxed: column "NUEMP" of b.parquet is unioned into
  "nuemp" …`; a true name the union would split across two columns (a: nuemp NUEMP; b: NUEMP) is
  REFUSED rc 198 on every path (`PlanContext.refusal`, honoured by cmd_view_open/prepare_using);
  a non-aligned union (replication mismatch) prints a loud "exact-name recovery is not available
  for this union … columns the engine renamed keep its names: …" note. All replicas verified
  line by line against the fetched DuckDB v1.5.3 source (multi_file_reader.cpp,
  union_by_name.cpp, parquet_reader.cpp, hive_partitioning.cpp, bind_table_function.cpp).
  Fallout fixed on the way: the lazy collect overlaid the view's metadata by POSITION while the
  direct-read planner may re-order plans by the file manifest (COLORDER-1) and leave a
  case-aliased column unmatched → one column's Stata name/metadata stamped onto another
  (reproduced on the relaxed union whose files carry manifests); the overlay now matches by
  engine name and restores the view's column order (`OVERLAY-BY-NAME-1`, plugin_view.cpp).
  Files: `src/engine/sanitize.{hpp,cpp}` (replicas), `src/plugin/plugin_io.{hpp,cpp}`
  (`Source.relaxed`, `PlanContext.refusal`, plan_columns), `src/plugin/plugin_view.cpp`.
  Tests: unit `DUCKDB-DEDUP-1`, `DUCKDB-UNION-1` (test_name_case.cpp); verify `v70` section F
  (rel: `nuemp NUEMP s extra` + label/format on eager, lazy collect, view save (pyarrow names);
  rel2 clash-in-2nd-file; cross-wiring refused eager+lazy; case merge note via log). Help
  (Column names), README, CHANGELOG, ASSUMPTIONS #99.
- **V2.3 float %tc (S2) — IMPLEMENTED.** `FLOAT-EXACT-1`: a manifest `float` in a non-FLOAT
  engine column (TIMESTAMP %tc eager, BIGINT ms lazy, INTEGER period, cast Hive key) sets
  `needs_float_exact`; the range pass adds a scan slot
  `bool_and(CAST(TRY_CAST(x AS FLOAT) AS DOUBLE) = x) FILTER (x IS NOT NULL)` (never the footer
  shortcut); `apply_meta_type` restores Float only when proven exact, Double when a value does not
  fit (a foreign manifest lie), old rule when unchecked (describe). The lazy collect hands the
  view's carried type/format to the planner (`PlanContext.meta_hint`) so the scan runs in-plan
  on the temp-table path too. Files: `typemap.{hpp,cpp}`, `plugin_io.{hpp,cpp}`,
  `plugin_view.cpp`. Tests: unit `FLOAT-EXACT-1` (test_typemap.cpp); verify `v74` H (1.8e12/
  123/-5e11 float %tc → float on eager, lazy-direct, lazy+verb, view-save; foreign not-exact
  manifest → double, values intact). Help (Dates and times) now says "when a scan proves every
  value exactly representable as a float … double otherwise".
- **V2.4 view-save value labels (S2) — IMPLEMENTED.** `view_kv_fragment` writes every definition
  in `vallabs_` (`VALLAB-ALL-1`). File: `plugin_view.cpp`. Test: `v74` C/D (view save with and
  without a verb: `orphan`+`used` in KV, `label orphan 7` restored).
- **A2-15(1) empty name (S2) — IMPLEMENTED.** The binder names an empty leaf `C<index>`;
  `recover_at` maps it back to "" (both branches), `stata_name_basis` never treats "" as a
  duplicate, the sanitiser yields `v<position>`; notes "column 2 has an empty name; loaded as
  v2" (eager) / "… it is v2 in the view" (lazy); no src_name char. Test: `v70` G.
- **V2.6 Hive key case clash (S2) — IMPLEMENTED.** `HIVE-CLASH-1` in plan_columns after the
  footer pass: keys via `parqit::duckdb_hive_keys` (Parse replica) of the first file; active for a
  directory source (hive forced) or a glob DuckDB auto-detects (AutoDetectHivePartitioningInternal
  replica: same key set in every file); ci-equal-but-not-equal key vs any leaf → refusal rc 198
  on eager/lazy/describe/glob; exactly equal key → `note:` (directory value used). Test: `v70` H;
  unit `DUCKDB-HIVE-1`.
- **V2.1 copysource docs (S2) — DONE.** Help (Materialisers) rewritten: verifies identity,
  names/kinds, N, sortedby and the first/last 64 observations; "do not compare the observations
  in between — an edit confined to the middle rows … is not detected"; copied file = source
  content with the source's own sortedby claim; no "never a silent copy" claim. CHANGELOG Changed
  bullet, README Limitations, ASSUMPTIONS #96 (c)/(e) reworded.
- **V2.5 docs (S2) — DONE.** Help Limitations: torn-read guard bullet (r(920), memory untouched,
  retry; pipeline = one query at execution time), copysource 64-row bullet, reshape wide/pivot
  ci-refusal bullet (+ the reshape paragraph), relaxed/Hive bullets; README Limitations: the same
  three bullets.
- **V2.7 messages (S3) — IMPLEMENTED.** `MSG-NOFILE-1`: the first footer probe
  (strict_schema_gate NUL check / plan_columns LIMIT 0) maps DuckDB's "No files found that match
  the pattern" to `file not found: no file matches "<p>" (nothing to read); check the path or
  pattern`, rc 601, SQL snippet stripped (`strip_sql_position`); `MSG-PART-1`: partition_by()
  naming every variable → `partition_by() must leave at least one non-partition column …` rc 198
  at all three save entries before staging; `MSG-RACE-1`: a fetch bind failure on a file source
  re-checks the identities and prints the torn-read message (or "could not be re-read as it was
  planned (modified concurrently?) …" with the engine text, no SQL snippet) instead of a raw
  Binder Error. Files: `plugin_io.cpp`, `plugin_view.cpp`. Tests: `v76` (missing file eager/lazy/
  glob/describe rc 601 + log free of "IO Error"/"LINE 1:"; partition_by all columns memory+view rc
  198, nothing written). The race message cannot be forced deterministically from a do-file
  (prepare and fetch happen inside one ado call); `v73`'s concurrent harness still passes and
  any refusal there is now worded by parqit.

### Evidence
- Unit tests: `./build/dev/parqit_tests` → `[doctest] test cases: 98 | 98 passed | 0 failed | 1 skipped`,
  `assertions: 3653 | 3653 passed | 0 failed` (4 new cases: DUCKDB-DEDUP-1, DUCKDB-UNION-1,
  DUCKDB-HIVE-1, FLOAT-EXACT-1).
- release_lint: `release-lint OK: v0.1.27 (9aug2026 / pkg 20260809); CHANGELOG top [0.1.27]`.
- Targeted: `VERDICT(V70_NAME_CASE_ROUNDTRIP): PASS`, `VERDICT(V74_TYPE_FIDELITY_PARTITION): PASS`,
  `VERDICT(V76_IO_EDGES): PASS` (extended tests).
- Full Stata suite (`bash tests/run_stata.sh`, final plugin): 97 VERDICT lines, ALL PASS, 0 FAIL /
  abort / did-not-finish — `local/audit_2026-08-22/impl2/full_suite.out`.
- Probe log of every behaviour end to end: `local/audit_2026-08-22/impl2/probe.log`.
- No version bump, no commits; nothing outside the repo touched.
