# Implementer brief — parqit audit 2026-08-22 remediation

You implement the corrections specified in `local/audit_2026-08-22/TRIAGE.md` (read it fully first; per-finding
repros and evidence live in `local/audit_2026-08-22/A1_types_values.md`, `A3_verb_semantics.md`,
`A4_atomicity_io.md`, `A5_docs_contract.md` and their `A*/` directories). The orchestrator (another agent) reviews
and certifies your work; report faithfully, never claim what you did not verify.

## Ground rules (non-negotiable)
1. Read `CLAUDE.md` first and follow it: correctness first; loud errors (nonzero rc + message, never rc 0 with
   wrong/partial output); atomic validate-then-mutate; every decision that the brief/README do not already fix goes
   into `ASSUMPTIONS.md` (new numbered items after #95); every user-visible change gets a `CHANGELOG.md` bullet
   under `## [Unreleased]` (Fixed / Changed / Added — no duplicate `###` headings inside the section); public
   surface changes (new option, new r() result, new note) must be reflected in `src/ado/p/parqit.sthlp`,
   `README.md` and — for options — the matching dialog `src/ado/p/parqit_*.dlg`; `bash tests/release_lint.sh`
   must pass at the end. Do NOT bump the version, do NOT commit, do NOT touch `~/ado` or anything under
   `/home/mangelo` outside this repo, do NOT edit files under `local/` except your own log (below).
2. Rigor over performance: when a rigorous fix costs time, take the rigorous fix. Never remove a feature or weaken
   an error path: A4-1 converts the automatic fast path into an explicit opt-in option (the capability stays).
3. Today's earlier changes (ENC-2 legacy-text transcoding, NAME-CASE-1 case-distinct names — see CHANGELOG
   [Unreleased], ASSUMPTIONS #94/#95, tests v32/v70, unit tests test_legacy_encoding/test_name_case) must keep
   working; do not undo them.
4. BUILD ONLY THESE TARGETS: `cmake --build build/dev --target parqit_plugin parqit_tests -j 16`. Do NOT run the
   default target (it copies the plugin into `ado/plus/p`, which other running Stata processes are using). The
   Stata test runner uses `build/dev/parqit.plugin` + `src/ado/p` directly, so this is enough.
5. Tests: unit tests (`./build/dev/parqit_tests`), targeted Stata tests (`bash tests/run_stata.sh <name-fragment>`),
   and before you report: the FULL Stata suite (`bash tests/run_stata.sh` — every VERDICT must be PASS) and
   `bash tests/release_lint.sh`. Every fix needs a pinning test: a doctest unit test when the logic is engine-side
   (exprtrans/typemap/sanitize/session/footer) and/or a `tests/verify_suite/v7N_*.do` test (self-contained, generates
   its own data, INDEPENDENT oracle — native Stata on the same data, pyarrow, duckdb CLI — and a final
   `VERDICT(...): PASS/FAIL` line; `python:` blocks only at top level, never inside loops; use `capture` + explicit
   PASS/FAIL lines where one failure must not hide the rest). Add new verify tests as v71, v72, … (v70 exists).
   To re-run an auditor's repro do-file against YOUR build, copy it into your scratch dir and replace its header
   with `adopath ++ "<repo>/src/ado/p"` + `global PARQIT_PLUGIN_PATH "<repo>/build/dev/parqit.plugin"` (the
   auditors' files point at the old install tree).
6. Keep a running log at `local/audit_2026-08-22/IMPLEMENTATION_LOG.md`: per item — status (done / partially /
   not done + why), files touched, tests added, the exact evidence lines (VERDICT/ctest output), open questions.
   Scratch files go under `local/audit_2026-08-22/impl/`.
7. Work in priority order; a later group must never break an earlier one. If an item turns out to be wrongly
   specified (the auditor's oracle was wrong), say so in the log with your evidence instead of forcing the change.

## Priority groups (IDs refer to TRIAGE.md — read the fix specs there)
- G1 — data integrity (S0): A3-1 merge using-only rows; A4-1/A4-2 (+A1-2) automatic source-copy save → opt-in
  `copysource` with hardened fingerprint (inode+ctime+size+mtime+rowcount+footer hash re-checked before COPY),
  sortedby/c(changed)/N/k/names/types guards, true sortedby in KV; A1-1/A1-11 `%tc` µs integer arithmetic;
  A3-2 `real()` grammar; A3-3/A3-4 date-domain guards (row-local missing, never abort); A1-3 partition-key type
  restore (and refuse non-round-trippable key types).
- G2 — S1: A4-3 torn-read identity check (plan→fetch re-stat + schema/count compare, loud failure, dataset
  untouched); A3-5 levelsof alias collision + audit of every fixed alias in plugin-side SQL; R1 rename of an
  aliased column (clear `stata`, move chars keyed by the old exposed name); A5-1 `encoding()` validated on the
  view-save path; A5-3/A5-2 bridge losses made loud (notes + r() from `_parqit_import_to_bridge` and the public
  commands that bridge; `encoding()` pass-through for `parqit use`/two-table adapters where contained, else
  documented default); A1-4 `str` not reserved; A1-5 zero-row partition_by; A4-4 long destination names (short
  digest staging names); A4-5 dest inside the view's directory source refused.
- G3 — S2 code: R2 spurious src_name after verbs; R3 selection varlists accept exact Stata names of aliased
  columns (expand_patterns → engine names); A1-6 manifest storage type restored identically eager/lazy
  (int %td, long %tc…); A1-7 rounding exact for integers ≥ 2^52; A1-8 partition columns keep manifest order;
  A1-9 ns pre-1970 floor; A1-10 unattached value labels written; A3-6 levelsof/tabulate number rendering
  (%21.0g-style via the existing Stata formatter); A3-7 string() denormal; A3-8 collapse/merge/reshape metadata
  parity (labels/formats/_merge value label) where native rules are clear; A3-9 `keep in f/l` letters;
  A4-6 symlink/read-only destinations; A4-7 hive `=` message; A4-8 messages + POSIX test-hook coverage;
  A5-12 `sql` alias note channel; A5-13 `parqit use , <badopt>` message.
- G4 — docs (must be complete): A5-4…A5-11 plus every new option/note/r() from G1–G3; README command tables
  refreshed (A5-10); dialogs (A5-9: `lz4_raw`, pivot `firstnm/lastnm/p##`, and the new `copysource`/`encoding()`
  where applicable); help prose for: copysource semantics and caveats, bridge conversions reported, partition key
  types, str not reserved, torn-read protection, `in f/l`, levelsof rendering, `_parqit_fast_source_nonce` char.

## Definition of done (report back with evidence)
- Unit tests: all pass. Full Stata suite: all VERDICT PASS (paste the summary). release_lint: OK.
- Each TRIAGE item: status + test name that pins it + the auditor repro re-run result against your build.
- Performance sanity: the general `parqit save` path and `parqit use` must not slow down measurably (time the
  5M-row synthetic save/use before and after — `local/audit_2026-08-22/A6/w1.do`-style — and report the numbers).
- Final message: ≤ 25 lines — what was fixed, what was not (and why), files touched, test evidence, open decisions
  for the maintainer.
