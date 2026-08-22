# Certification — parqit adversarial audit and remediation, 2026-08-22

Orchestrator/certifier: Claude (Fable 5). Scope requested by the maintainer: after the ENC-2/NAME-CASE-1 fixes
of the same day, run a thorough, holistic, adversarial audit with data integrity as the non-negotiable
criterion; delegate the implementation of corrections; certify no regression in precision, features or
performance (rigor over performance); ensure the help reflects every feature/option.

## 1. Audit (six parallel auditors, ≈1,300 targeted experiments, independent oracles: native Stata, pyarrow, duckdb CLI)
| Dim | Report | Checks | Findings | Top items |
|---|---|---|---|---|
| A1 types/values | A1_types_values.md | ≈395 | 3 S0 · 2 S1 · 6 S2 | %tc µs precision ≥ year 4253; fast-path row order; partition keys float/double/%tc → string; `str` reserved; 0-row partition_by |
| A2 names/metadata | A2_names_metadata.md | ≈330 | 4 S0 · 3 S1 · 8 S2 | nested/multi-file name recovery; reshape wide/pivot alias invariant (duplicate-name file); rename/collapse of aliased columns; append gen clash; char > 67,783 bytes |
| A3 verb semantics | A3_verb_semantics.md | ≈200 | 3 S0 · 2 S1 · 4 S2 | merge using-only rows lose common vars; real() grammar; date domain; levelsof alias `v`; string() denormal |
| A4 atomicity/IO | A4_atomicity_io.md | ≈70 | 2 S0 · 3 S1 · 3 S2/3 | automatic source-copy save after sort/gsort/Mata (and fingerprint not content-sensitive); torn read; long names; dest inside dir source |
| A5 docs/contract | A5_docs_contract.md | mech. tables + 13 | 1 S0/S1 · 2 S1 · 9 S2 | bridge of .dta silent on losses; encoding() unvalidated on view save; option/README/dialog gaps |
| A6 performance | A6_performance.md | 10 workloads × 3–5 reps | none | no regression vs v0.1.27; byte-identical outputs |
Orchestrator's own static review added R1 (rename of aliased column keeps stale Stata name + orphaned chars), R2 (spurious src_name after verbs), R3 (selection varlists vs exact names). Full triage with fix specs: TRIAGE.md.

## 2. Remediation (implementer agent; log IMPLEMENTATION_LOG.md)
All 38 triaged items implemented in priority order G1 (S0) → G2 (S1) → G3 (S2) → G4 (docs). Key decisions recorded
in ASSUMPTIONS #96 (source-copy save is an explicit opt-in `copysource` with a content-sensitive fingerprint —
rigor over performance, feature preserved), #97 (date-function domain window), #98 (partition-key type restore).
CHANGELOG `[Unreleased]`: Changed 5 · Added 4 · Fixed 15. Help/README/dialogs updated (A5 mechanical gaps closed;
new options `copysource`, `encoding()` on use/merge/append/joinby/open, `keep in f/l`, bridge r() results).
New tests: verify v71–v77, unit cases in test_exprtrans/test_session/test_sanitize/test_typemap/test_view; t11 rewritten.
Not forced (documented): DuckDB 1.5.3 already reads Hive values containing `=` (A4-7 wrapper kept defensively);
native `dow/doy` extend one day past 31dec9999 (A3-3; single clean window, ASSUMPTIONS #97).

## 3. Certification evidence (orchestrator, independent of the implementer)
- Build: clean (no warnings); `./build/dev/parqit_tests` 94 cases / 3,613 assertions PASS; `bash tests/release_lint.sh` OK.
- Full Stata suite (`bash tests/run_stata.sh`, run 3): **97 VERDICT PASS / 0 FAIL** (incl. v71–v77; log scratchpad/full_suite3.log).
- Orchestrator repros re-run on the final install tree: A3-1 (merge k=4 → c=444 sc="ud" = native), A4-1 (save after sort writes memory order, true sortedby), R1 (`rename NUEMP_1 foo` → foo), R2 (no spurious src_name), A5-1 (`encoding(bogus)` on view save → rc 198, nothing written), A5-2/3 (bridge notes printed: transcoding, ext-missing, fractional).
- Performance (A6 harness, W1 save / W2 eager read, 5M × 82, 7 alternating repetitions under machine load 6–12):
  W1 B min 15.14 / med 18.59 vs N min 15.58 / med 21.32 (min ratio 1.03; paired median +1.5 s, dominated by two
  load-bound repetitions); W2 B min 26.03 / med 26.29 vs N min 26.43 / med 27.20 (min ratio 1.015, med 1.035,
  paired median +0.7 s). No regression beyond load noise at the 5 % level; same picture as A6's pre-remediation
  campaign. Outputs byte-identical across installs (A6) and `cf` exact.
- Independent adversarial verification of the fixes: VERIFY_REPORT.md (see §4).

## 4. Adversarial verification (independent verifier agent) — VERIFY_REPORT.md
Re-ran every auditor repro on the final build plus 13 new adversarial do-files (~600 checks) and 3 concurrent
writer/reader harnesses. **35 items CLOSED — every S0 of the triage** (A3-1 64 native-`cf` merge variants;
copysource refuses cp -p/rsync/in-place/new-inode tampers and sort/gsort/end-row Mata edits while the default
save writes memory after every c(changed)-exempt mutation; 20k %tc instants exact on both writers + lazy; 53
real() literals = native; date edges = native; 14 partition-key types × eager/lazy/view-save exact; 4-level nested
dedup + glob/Hive recovery; reshape wide/pivot/rename/collapse aliases; 60 eager reads under 44 concurrent replaces →
0 torn, 16 loud; bridge losses loud + encoding honoured on all 6 commands). Regression guard v32/v70–v77 PASS; help
mechanically complete; lint OK.
Residuals (round 2, TRIAGE "Round 2"): V2.2 S1 `relaxed` union still silent on clashing names (help overclaims);
V2.3 float %tc → double; V2.4 unattached value labels dropped by a VIEW save; A2-15(1) empty name → `C1`;
V2.1/V2.5 docs (copysource 64-row sampling wording, torn-read guard, reshape-wide refusal); V2.6 foreign Hive key
clashing by case silently drops the file column; V2.7 three raw engine messages. A round-2 implementer was launched
for these; round-2 results are appended below when certified.

## 4b. Round 2 (residuals) — implemented and certified
Round-2 implementer closed all 8 residuals: V2.2 relaxed-mode exact-name recovery (exact replicas of DuckDB's
reader dedup / union_by_name / Hive-key rules, unit-pinned `DUCKDB-DEDUP-1/UNION-1/HIVE-1`; a union that would split
a name is refused rc 198; never silent), V2.3 float %tc restored when a scan proves float32-exactness (`FLOAT-EXACT-1`),
V2.4 view save writes every value label, A2-15(1) empty names → `v<position>` with a note, V2.6 Hive key clashing by
case with a file column refused (`HIVE-CLASH-1`), V2.1/V2.5 help/README/CHANGELOG/ASSUMPTIONS #96 wording exact +
new #99, V2.7 parqit messages for missing files / all-column partition_by / fetch bind failures.
Orchestrator re-certification: rebuild clean; `parqit_tests` 98 cases / 3,653 assertions PASS; `release_lint` OK;
install tree == src (ado/help/dialogs/pkg byte-identical); targeted v70/v74/v76 PASS; the verifier's round-2 repro
files (N7_names, N3_tc, N11_misc) re-run on the final build: only the two N11 checks with wrong expectations
differ — `count if v > 1` is 3 natively too, and the three `string()` rows are |x| ≥ 8.99e307 (beyond Stata's
maxdouble; documented INF-1). Full Stata suite run 4: see §3 addendum below.

### §3 addendum — final build (after round 2)
Full Stata suite run 4 on the final build: **97 VERDICT PASS / 0 FAIL** (scratchpad/full_suite4.log). Unit tests
98 cases / 3,653 assertions PASS. release_lint OK. Certified for release as v0.1.28 (2026-08-23).

## 5. Residual / decisions left to the maintainer
- Version/date bump before release (banners still `0.1.27 9aug2026`); nothing committed.
- `_dta[_parqit_fast_source_nonce]` characteristic is kept (documented) so `copysource` can verify provenance; Stata's
  own `save` writes it into a .dta.
- Eager read is ~2–3 % slower than v0.1.27 on a 5M × 82 read under load (torn-read identity checks + name recovery);
  acceptable under rigor-over-performance; can be profiled on request.
- Dialog changes (copysource checkbox, encoding comboboxes, lz4_raw, pivot stats) are not exercised by the batch suite —
  a quick GUI check is advisable.
