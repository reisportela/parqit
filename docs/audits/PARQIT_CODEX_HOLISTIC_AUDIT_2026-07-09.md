# parqit holistic adversarial audit — 2026-07-09

## Executive summary

- **Overall verdict:** v0.1.19 is substantially reliable, but the audit found four S0 silent-correctness defects and one still-open S0 semantic limitation.
- **Baseline audited:** `9b62222b6fa8c335f8f2163c7fd52161afcb8d20`; work branch: `codex-holistic-audit-2026-07-09`.
- **TEMPORAL-ROUND-1 — fixed:** all save paths now apply native Stata's exact-half temporal rounding; pinned by `v54` and pyarrow.
- **RESHAPE-MISSKEY-1 — fixed:** reshape now treats SQL NULL, empty string and NaN as the same Stata missing key; pinned by `v55`.
- **REGEXM-NULL-1 — fixed:** a NULL regex pattern now behaves as Stata's empty string; pinned by a C++ execution test and `v56`.
- **STATS-MISSKEY-1 — fixed:** tabulate, duplicates, codebook, distinct and tabstat now share native missing-key semantics; pinned by `v57`.
- **MM-ORDER-1 — still open:** lazy `merge m:m` cannot reproduce native physical within-key order; the limitation is now explicit and `joinby` is recommended.
- **HARNESS-NOMATCH-1 / HARNESS-ABORT-1 / TEST-TMP-OWNERSHIP-1 — fixed:** empty/stale-green runs and cross-run scratch collisions are rejected.
- **Performance:** no optimization landed; measured gaps were structural, correctness costs, inside noise, or unsafe to change in this pass.
- **Docs:** stale syntax, version examples, absolute timing claims, type precision caveats, the `m:m` contract and the executable tour were corrected.
- **Final gate:** release lint and build passed; CTest 3/3; doctest 63 cases and 973 assertions; Stata 69 files and 70/70 PASS verdicts.

## Scope, baseline and evidence discipline

The audit followed `parqit_build_prompt.md` as the fixed contract and read the
current code rather than accepting prior reports as evidence. The current README,
help, `CLAUDE.md`, all 73 assumptions after this pass, the prior holistic,
performance and adversarial reports, the external `pq` charter, and the relevant
`xhdfe` house-style surfaces were reviewed. Previous `v27`–`v53` findings were
treated as regression targets.

Every confirmed reliability defect below has a runnable record under
`audit_repro/`. The fixed data-path findings were observed failing on the baseline
before the implementation was changed, then pinned with an independent native
Stata or pyarrow oracle. The intentionally red `MM-ORDER-1` reproducer records the
remaining limitation.

The first three logical local commits are
`164cce1b90e86f0f1f9e62e4b23245240cffe112` (fidelity fixes and pins) and
`1f68846f98f0b5939c632e843305d2cccbc7076d` (runner/parallel-fill hardening),
followed by `aad830ea766c32d56a2cb2a5e9e570a2ba0183dc` (documentation, tour and
initial report). The final test-scratch-hardening commit also updates this report.
No remote state was touched.

## Reliability findings

| ID | Severity | Confidence | Area | Title | Repro? | Status |
|---|---:|---|---|---|---|---|
| TEMPORAL-ROUND-1 | S0 | certain | A/B, save boundary | Fractional temporal payload depended on save path | yes | Fixed |
| RESHAPE-MISSKEY-1 | S0 | certain | C/D, reshape | One Stata missing key became two reshape identities | yes | Fixed |
| REGEXM-NULL-1 | S0 | certain | D/F, expressions | `regexm()` propagated a NULL pattern that Stata sees as `""` | yes | Fixed |
| STATS-MISSKEY-1 | S0 | certain | D, statistics | Exploration commands split or counted equivalent missing keys | yes, two focused repros | Fixed |
| MM-ORDER-1 | S0 | certain | C/D, two-table verbs | Lazy `merge m:m` can pair different payload rows than native Stata | yes | Reported only; docs fixed |
| HARNESS-NOMATCH-1 | S3 | certain | K, test integrity | A misspelled filter returned a green empty test run | yes | Fixed |
| HARNESS-ABORT-1 | S3 | certain | K, test integrity | An early PASS masked a later uncaptured Stata abort | yes | Fixed |
| TEST-TMP-OWNERSHIP-1 | S3 | certain | K, test integrity | Concurrent/repeated test runs shared or leaked scratch state | yes, two focused repros | Fixed |

### TEMPORAL-ROUND-1

- **Severity / confidence:** S0 / certain.
- **Where:** `src/plugin/plugin_io.cpp:2035-2108` and
  `src/plugin/plugin_view.cpp:254-283`.
- **Evidence:** the two physical writers used C/C++ `nearbyint()` for several
  temporal classes, which normally resolves exact halves to even; `%tc` preserved
  fractional milliseconds. Lazy `compile_for_save` used DuckDB `round()`, whose
  negative-half result differs from native Stata. Native Stata verified
  `round(100.5)==101` and `round(-100.5)==-100`. A pyarrow read showed different
  on-disk dates/timestamps/counts for the same logical values depending on whether
  the source was an in-memory dataset or a lazy view.
- **Repro:** `audit_repro/repro_fractional_temporal_save_divergence.do`.
- **Impact:** a researcher saving a fractional `%td`, `%tc`, `%tC` or period value
  could get a different Parquet payload solely because the same pipeline took a
  different materialisation path. This is silent value corruption at exact ties,
  plus sub-millisecond path divergence for `%tc`.
- **Resolution — Fixed:** `stata_round_unit()` explicitly implements
  `floor(x + 0.5)`. The Arrow and staged writers share
  `convert_save_numeric()`, `%tc` is rounded before microsecond encoding, and lazy
  SQL emits the same rule. `tests/verify_suite/v54_temporal_save_path_parity.do`
  independently reads both files with pyarrow and checks exact expected payloads.
  The standalone repro changed from FAIL to PASS.

### RESHAPE-MISSKEY-1

- **Severity / confidence:** S0 / certain.
- **Where:** `src/plugin/plugin_view.cpp:135-149`,
  `src/plugin/plugin_view.cpp:1697-1704`,
  `src/plugin/plugin_view.cpp:1773-1786`, and
  `src/engine/view.cpp:1523-1547`.
- **Evidence:** after `append`, the same Stata string missing could be physically
  `''` in one input and SQL NULL in the other. The reshape uniqueness checks and
  wide grouping used raw keys, so long reshape failed to detect duplicate Stata
  `i()` values and wide reshape emitted separate rows. Native Stata materialising
  those rows first sees both keys as `""`.
- **Repro:** `audit_repro/repro_reshape_missing_key_normalization.do`.
- **Impact:** long reshape could fabricate extra long observations; wide reshape
  could split one entity across rows. Both are silent panel-identity errors.
- **Resolution — Fixed:** `norm_view_key()` and the existing engine
  `norm_group_key()` now canonicalize string empty/NULL and numeric NaN/NULL in
  both reshape validation and the emitted wide plan.
  `tests/verify_suite/v55_reshape_missing_keys.do` uses pyarrow-created mixed
  schemas and a native Stata oracle. It changed from FAIL to PASS.

### REGEXM-NULL-1

- **Severity / confidence:** S0 / certain.
- **Where:** `src/engine/exprtrans.cpp:1133-1140`.
- **Evidence:** the translator coalesced the regex subject but not the pattern.
  A pattern column absent from an appended file is SQL NULL inside the plan but
  becomes the Stata string missing `""` on collection. Native
  `regexm("xyz", "")` is 1; the lazy expression returned NULL/missing.
- **Repro:** `audit_repro/repro_regexm_null_pattern.do`.
- **Impact:** a derived indicator silently became missing instead of 1 whenever a
  two-table verb introduced the pattern as a missing string.
- **Resolution — Fixed:** both operands are now coalesced to `''` before
  `regexp_matches`. The engine execution case in
  `tests/unit/test_exprtrans.cpp` and native-oracle integration test
  `tests/verify_suite/v56_regexm_null_pattern.do` pin the behavior. The repro
  changed from FAIL to PASS.

### STATS-MISSKEY-1

- **Severity / confidence:** S0 / certain.
- **Where:** `src/plugin/plugin_view.cpp:2445-2465`,
  `src/plugin/plugin_view.cpp:2519-2533`,
  `src/plugin/plugin_view.cpp:2589-2650`,
  `src/plugin/plugin_view.cpp:2675-2717`, and
  `src/plugin/plugin_view.cpp:2811-2860`.
- **Evidence:** one- and two-way tabulate, duplicate partitions, codebook,
  distinct, and tabstat by-groups used raw keys. Mixed `''`/NULL inputs therefore
  produced two missing cells or duplicate groups. `codebook` counted the empty
  string as a distinct value and `tabstat, by()` printed a missing group. Fresh
  native Stata probes established that codebook excludes string missing from its
  unique count and tabstat omits a missing by-group.
- **Repros:** `audit_repro/repro_duplicates_missing_key_normalization.do` and
  `audit_repro/repro_stats_missing_semantics.do`.
- **Impact:** data-quality diagnostics and grouped results could silently report
  the wrong number of categories, uniques, duplicate groups, or by-groups after a
  merge/append/reshape.
- **Resolution — Fixed:** all named commands use the same canonical Stata group
  key. Joint distinct counts now exclude incomplete tuples, codebook min/max and
  distinct exclude canonical missing, and tabstat filters the missing by-group.
  `tests/verify_suite/v57_stats_missing_keys.do` pins display-only response records
  as well as returned scalars. Both repros changed from FAIL to PASS.

### MM-ORDER-1

- **Severity / confidence:** S0 / certain.
- **Where:** `src/engine/view.cpp:992-1016`; corrected public contract at
  `README.md` Limitations and `src/ado/p/parqit.sthlp` Two-table verbs/Limitations.
- **Evidence:** native Stata pairs `m:m` rows in the existing physical within-key
  order. The lazy engine has no stable physical row identity and deliberately
  orders ties by user sort plus all values (`src/engine/view.cpp:996-1014`). With
  master payload order `20,10` and using order `300,100,200`, `cf` shows different
  paired payloads even though both implementations use the clamped sequential
  reuse rule.
- **Repro:** `audit_repro/repro_merge_mm_physical_order.do` intentionally ends
  `FAIL - deterministic fallback differs from native physical-order pairing`.
- **Impact:** an already-dangerous `merge m:m` can silently attach a different
  using payload to a master row than native Stata on the same unsorted inputs.
- **Resolution — Reported only; docs fixed:** reproducing physical order requires
  a row-identity/provenance contract across lazy plan stages, not a safe local
  patch. The source comment, README, help and assumption 72 now state the real
  deterministic-value-order contract and recommend `joinby`. A maintainer decision
  is needed before either refusing `m:m` or designing persistent physical row IDs.

### HARNESS-NOMATCH-1

- **Severity / confidence:** S3 / certain.
- **Where:** `tests/run_stata.sh:29-54`.
- **Evidence:** `tests/run_stata.sh __CODEX_FILTER_THAT_MATCHES_NO_TEST__`
  selected no files, printed an empty summary and exited 0.
- **Repro:** `audit_repro/repro_runner_no_match.sh`.
- **Impact:** a typo in a targeted gate could be reported as successful while no
  test ran, weakening every audit claim based on the runner.
- **Resolution — Fixed:** the runner counts selected files and exits 2 with a
  clear diagnostic on zero matches. `tests/test_run_stata_no_match.sh` is a CTest
  regression. The repro now passes because it observes rc 2.

### HARNESS-ABORT-1

- **Severity / confidence:** S3 / certain.
- **Where:** `tests/run_stata.sh:56-80`.
- **Evidence:** a synthetic Stata log containing `VERDICT(FAKE): PASS` followed by
  uncaptured terminal `r(9);` was accepted. Batch Stata's process exit status did
  not expose the failure.
- **Repro:** `audit_repro/repro_runner_pass_then_abort.sh`.
- **Impact:** a test could print an early PASS, abort during cleanup or later
  assertions, and leave CI green.
- **Resolution — Fixed:** the runner compares the last terminal `r(#);` line with
  the last verdict and rejects an abort that follows it. Expected captured errors
  remain valid because a later final verdict supersedes them. The same CTest shell
  pins both harness defects.

### TEST-TMP-OWNERSHIP-1

- **Severity / confidence:** S3 / certain.
- **Where:** `tests/unit/test_tmp.hpp`, `tests/unit/test_arrow_copy_bench.cpp`,
  `tests/unit/test_session.cpp`, `tests/unit/test_request.cpp`,
  `tests/run_stata.sh`, `tests/integration/t02_use_options.do`, and the
  `unit_concurrent` registration in `CMakeLists.txt`.
- **Evidence:** running CTest and `./build/dev/parqit_tests` concurrently made the
  Arrow capability test fail at its COPY. An eight-process focused run then
  failed seven processes: each used the same `/tmp/parqit_arrowcap.parquet`.
  Session/request tests had the same fixed-name hazard even when identical payloads
  made their races less visible. A subsequent full Stata rerun exposed the sibling
  problem: passing `t02` left its `tempfile`-derived directory behind, and later
  OS temp-prefix reuse aborted at `mkdir` with r(693).
- **Repros:** `audit_repro/repro_unit_temp_collision.sh` delegates to the focused
  eight-process stress gate in `tests/test_unit_concurrent.sh`;
  `audit_repro/repro_t02_fixture_leak.sh` runs passing `t02` under a dedicated
  TMPDIR and detects any persistent directory.
- **Impact:** parallel CTest jobs or simultaneous local agents could report a
  product regression or read another process's oracle; a repeated Stata suite
  could abort before testing anything. This is a reproducibility and CI-integrity
  failure, not a plugin data-path bug.
- **Resolution — Fixed:** all writable unit scratch paths now use the platform
  temp directory plus PID and clean their own artifacts. The Stata runner creates
  and removes a private TMPDIR per selected test; `t02` also owns and removes its
  working-directory fixture. CTest registers the eight-process gate and its runner
  contract verifies temp-root disposal. The unit repro changed from seven
  failures/FAIL to PASS, the t02 leak repro changed from FAIL to PASS, consecutive
  targeted t02 runs pass, and `ctest --preset dev -j 3` passes 3/3 while `unit`
  and `unit_concurrent` run together.

## Promise audit

“Corrected” means the claim was false or overbroad at the baseline and no longer
survives in that form.

| Claim | Source | Verdict | Evidence |
|---|---|---|---|
| Verbs form a lazy plan and a materializer executes one DuckDB query | README Architecture; help Data paths | VERIFIED | `parqit show`/`explain`, `T03`, `T05`, `T13`, and `View::compile()` inspection |
| `collect, clear` replaces memory atomically and leaves the view open | README command table; help Data paths | VERIFIED | staged-frame validate-then-swap in `_parqit_load_core`; `v09`, `v43`, `T06` |
| Lazy `save` writes Parquet without loading the result into Stata memory | README thesis/materializers; help Data paths | VERIFIED | `view_save` calls DuckDB COPY; `T03`, `T11`; the documented `data` option is the explicit in-memory path |
| `save` itself is atomic | help Saving | VERIFIED on ordinary failures | temp-target/verification/rename code read; `v08`, `v30`, `v43`, `v45`; kill-9/full-disk endpoint not exercised |
| Stata metadata round-trips losslessly | README Metadata; help Metadata | VERIFIED WITH DOCUMENTED LIMITS | labels, formats, notes and characteristics: `v07`, `v29`, `v39`, `v47`, `v51`; extended-missing categories collapse by documented format limit |
| Numeric types never silently overflow or demote | README Type mapping; help Types | VERIFIED WITH DOCUMENTED PRECISION NOTES | `v06`, `v11`, `v15`, `v21`, `v38`, `v41`, `v49`, `v51`; DECIMAL→binary64 and UINT64>2^53 are now stated as rounded-with-note |
| Date, datetime and period scaling is path-independent | README/help Type mapping | FALSE at baseline; CORRECTED | TEMPORAL-ROUND-1; pyarrow oracle `v54`; prior date suites `v03`, `v05`, `v22`, `v38` remain green |
| `merge m:m` reproduces native sequential pairing exactly | baseline README/help | FALSE; CORRECTED | MM-ORDER-1 fresh native repro; docs now distinguish the rule from physical pairing |
| Missing keys are equivalent across joins, groups, reshape and statistics | help Missing values/statistics | FALSE at baseline; CORRECTED | `v14`, `v27`, `v35` covered older paths; RESHAPE-MISSKEY-1 and STATS-MISSKEY-1 closed uncovered paths with `v55`/`v57` |
| SQL mode vs `statamissing on` follows the documented comparison contract | README Limitations; help Missing values | VERIFIED | translator read; `v17`, `v28`, `v42`, locale run |
| Parallel and serial fills are byte-identical | README Tuning; help Performance | VERIFIED | `v20` now checks every cell at 1.5M rows with `PARQIT_FILL_THREADS=24` |
| `describe` is “instant on any size” | baseline help First contact | OVERBROAD; CORRECTED | it reads footer metadata without value scans, but an unbounded wall-time promise is unsupportable; wording now states the actual I/O contract |
| `glimpse` requires a filename | baseline help Syntax | FALSE; CORRECTED | `_parqit_glimpse` delegates to optional-target `_parqit_describe`; brackets now match the parser |
| The repository ships both the tour and `parqit_clean_demo.do` | baseline help Examples | FALSE; CORRECTED | only `examples/parqit_tour.do` exists; the nonexistent program was removed from help |
| The README's exact `mergein` 3.4s-vs-9.6s comparison is current | baseline README | UNTRACEABLE/STALE; CORRECTED | no matching current artifact or fixed environment; replaced with the architectural guidance and fresh results below |
| The install example points to the current v0.1.19 tag | baseline README Installation | FALSE; CORRECTED | stale v0.1.13 changed to v0.1.19 |
| The complete command-line workflow has executable documentation | README Tour; help Examples | VERIFIED after update | `T13` runs the tour; `VERDICT(PARQIT_TOUR): PASS`; new sections cover `mergein`, `appendin`, compression level and all four settings |

## Performance assessment

### Method and inputs

All numbers below are fresh wall-clock minima of three runs; seconds are rounded
to three decimals. The broad harness used a 200,000-row workers subset and emitted
160 feature/method rows with zero failures. The load harness used the deterministic
10,000,000-row × 13-column workers file, a 500,000-row firms file, the patents
file, and the wide-income file under `benchmarks/_out/synthetic_medium_data`.

Two comparison types are kept distinct:

- **CLI floor:** the same core relational SELECT/COPY was run by the DuckDB CLI
  after one warm-up, without parqit's validation, metadata and Stata bridge work.
- **Py/DDB lower bound:** the broad harness's canonical pyarrow file I/O or
  embedded DuckDB SQL. It is useful for breadth but is not called an exact CLI
  floor for host-native commands (`mergein`, `appendin`) or display work.

Commands actually used (with `REPO=$PWD`) were:

```bash
python3 benchmarks/benchmark_feature_surface.py \
  --repo . --plugin build/dev/parqit.plugin \
  --source-datadir benchmarks/_out/synthetic_medium_data \
  --outdir /tmp/parqit_codex_audit_perf_surface --reps 3 \
  --stata-bin stata-mp

stata-mp -b do benchmarks/benchmark_synthetic_features.do \
  "$REPO" "$REPO/build/dev/parqit.plugin" \
  "$REPO/benchmarks/_out/synthetic_medium_data" \
  /tmp/parqit_codex_audit_perf_large 3

stata-mp -b do /tmp/parqit_codex_audit_perf_probe.do \
  "$REPO" "$REPO/build/dev/parqit.plugin" \
  "$REPO/benchmarks/_out/synthetic_medium_data" \
  /tmp/parqit_codex_audit_perf_probe_out

stata-mp -b do /tmp/parqit_codex_audit_strl_perf.do \
  "$REPO" "$REPO/build/dev/parqit.plugin" \
  /tmp/parqit_codex_strl_100k.parquet
```

The CLI floor invocation used `PRAGMA threads=16`, `.timer on`, one untimed
warm-up and three numbered destinations. Its measured SELECT/COPY bodies were:

```sql
-- filter/gen
COPY (SELECT *, ln(wage) AS lwage FROM read_parquet(W)
      WHERE year >= 2020 AND wage > 0) TO OUT (FORMAT PARQUET, COMPRESSION SNAPPY);
-- collapse
COPY (SELECT firm_id, year, avg(wage) AS wage,
             stddev_samp(wage) AS sd_wage, count(wage) AS n
      FROM read_parquet(W) GROUP BY firm_id, year)
      TO OUT (FORMAT PARQUET, COMPRESSION SNAPPY);
-- sort
COPY (SELECT * FROM read_parquet(W)
      ORDER BY firm_id NULLS LAST, year NULLS LAST, wage NULLS LAST)
      TO OUT (FORMAT PARQUET, COMPRESSION SNAPPY);
-- merge/joinby
COPY (SELECT projected_columns FROM read_parquet(W) w
      INNER JOIN read_parquet(F_OR_P) u ON w.firm_id=u.firm_id
      WHERE w.year=2022) TO OUT (FORMAT PARQUET, COMPRESSION SNAPPY);
-- reshape long
COPY (SELECT pid, grp, parsed_year, inc FROM
      (UNPIVOT read_parquet(WIDE) ON inc2018,inc2019,inc2020,inc2021,inc2022
       INTO NAME source_name VALUE inc))
      TO OUT (FORMAT PARQUET, COMPRESSION SNAPPY);
```

`W`, `F_OR_P`, `WIDE` and `OUT` were replaced with the quoted benchmark input
and numbered `/tmp` destination paths. For merge and reshape, the floor preserves
the output relation but intentionally omits parqit's uniqueness/value-domain
prechecks; that omitted correctness work is part of the measured bridge gap.

### Headroom table

| Operation / shape | parqit now | Floor or lower bound | Gap | Verdict |
|---|---:|---:|---:|---|
| `use`/`collect`, 10M×13 into Stata | 2.926 | 1.951 Arrow fetch | 0.975 | Structural SPI allocation/fill and decoration; no bulk Stata column API found |
| `save`, 200k in-memory | 0.152 | 0.107 PyArrow LB | 0.045 | Small writer/metadata overhead; no safe win outside noise |
| `keep if` / `drop if`, 200k + save | 0.050 / 0.097 | 0.046 / 0.068 Py/DDB LB | 0.004 / 0.029 | At/near execution floor |
| `drop` variables, 200k + save | 0.184 | 0.077 Py/DDB LB | 0.107 | Mostly host/materializer fixed cost on a small job |
| `gen` / `egen, by()` / `replace`, 200k + save | 0.185 / 0.157 / 0.219 | 0.079 / 0.132 / 0.063 Py/DDB LB | 0.106 / 0.025 / 0.156 | Small-job plan/materializer overhead; no isolated safe code target |
| filter + `gen` + `save`, 10M×13 | 1.314 | 1.264 CLI | 0.050 | At engine floor within run noise |
| `collapse` + save, 10M | 0.474 | 0.354 CLI | 0.120 | Small validation/metadata wrapper; engine dominates |
| `contract` + save, 10M | 0.037 | 0.065 CLI | ≤ noise | No detectable bridge headroom |
| `duplicates drop` + save, 10M | 1.131 | 0.832 CLI | 0.299 | Canonicalization plus verified write; not removed for speed |
| `sort` + save, 10M×13 | 1.455 | 1.477 CLI | ≤ noise | At engine floor |
| `gsort`, 200k + save | 0.196 | 0.112 Py/DDB LB | 0.084 | Small-job fixed cost; no load regression observed |
| `merge m:1` + save, 10M/500k | 0.832 | 0.491 CLI | 0.341 | Required uniqueness and key-normalization checks explain real gap |
| `joinby` + save, 10M reference | 1.029 | 0.676 CLI | 0.353 | Required schema/key work; no safe bypass |
| `append`, 200k | 0.034 | 0.020 Py/DDB LB | 0.014 | At floor for practical purposes |
| `mergein` / `appendin`, 200k host path | 0.213 / 0.125 | 0.084 / 0.068 canonical LB | 0.129 / 0.057 | No exact DuckDB floor exists for native Stata merge/append; current projection path is correct |
| `reshape long` + save, large wide file | 0.798 | 0.763 CLI | 0.035 | At engine floor |
| `reshape wide` + save, 10M | 0.529 | 0.238 CLI core | 0.291 | Eager `(i,j)` uniqueness and j-domain checks are mandatory |
| `pivot` + save, 10M | 0.120 | 0.094 CLI core | 0.026 | At/near engine floor |
| `summarize`, 10M, four variables | 0.069 | 0.079 CLI | ≤ noise | At floor |
| `summarize, detail` | 0.047 (200k); 0.372 (10M probe) | 0.060 Py/DDB (200k); exact 10M CLI not measured | ≤0 at 200k; n/v at 10M | Existing exact-order-statistic implementation retained; no regression |
| one-/two-way `tabulate`, 200k | 0.012 / 0.015 | 0.003 / 0.006 Py/DDB LB | 0.009 / 0.009 | Fixed call/format cost only |
| `tabstat` exact grouped p50, 10M | 1.167 | 0.374 CLI aggregate LB | 0.793 | Lower bound omits Stata's exact-rank reconstruction; optimization is delicate and deferred |
| `codebook`, 200k | 0.020 | 0.011 Py/DDB LB | 0.009 | One combined scan already; at floor |
| `correlate` / `pwcorr` | 0.089 (10M) / 0.023 (200k) | 0.050 CLI / 0.010 Py/DDB LB | 0.039 / 0.013 | Small response/format overhead |
| `histogram, nodraw`, 200k | 0.027 | 0.202 canonical | ≤0 | Canonical implementation is not an engine-floor comparison; no issue |
| `misstable`, 10M | 0.083 | 0.052 CLI | 0.031 | At/near floor |
| strL read, 100k × 2,900 bytes (290.8 MB payload) | 0.841 | 0.033 warm pyarrow | 0.808 | Structural sidecar parsing plus Stata strL stores; exact payload assertions pass |

### Landed performance changes

None. No candidate satisfied all four requirements (measured, outside noise, safe
by construction, and independently gated) without weakening fidelity or an error
path.

### Measured and rejected or deferred

- **Grouped exact percentiles:** the aggregate lower bound is 0.793s below
  `tabstat` on the 10M case, but does not itself reproduce Stata's exact-rank
  reconstruction. Parallelizing the real algorithm while preserving the
  percentile rule, group cap and bounded error path is a separate correctness
  project.
- **Reshape wide:** removing the prechecks would recover up to 0.291s but would
  reintroduce silent duplicate-cell fabrication. Rejected.
- **Merge:** bypassing uniqueness/key canonicalization could recover up to 0.341s
  but would weaken Stata merge semantics. Rejected.
- **Duplicates:** the 0.299s gap includes canonical Stata missing equivalence and
  a verified Parquet write. No safe isolated win was found.
- **strL:** the 0.808s difference is dominated by the required Stata strL boundary;
  no supported bulk SPI primitive was found. A speculative sidecar rewrite was
  rejected.
- **Sub-0.2s breadth gaps:** startup, response formatting and materializer costs
  could not be isolated from noise into a safe change. No micro-optimization was
  landed.

## Documentation and examples

### `src/ado/p/parqit.sthlp`

- Replaced the absolute `merge m:m` equivalence claim with the actual sequential
  reuse plus deterministic-value-order limitation in syntax discussion, examples
  and Limitations.
- Documented canonical missing grouping for tabulate/duplicates/codebook/distinct
  and the omission of missing `tabstat, by()` groups.
- Added truthful DECIMAL and UINT64 binary64 precision notes and the common native
  rounding rule for fractional temporal saves.
- Corrected `glimpse [filename]`, removed the nonexistent
  `parqit_clean_demo.do`, and replaced “instant on any size” with the footer-only
  I/O contract.
- Verified `help parqit` renders in batch Stata without error.

### README and developer guidance

- Updated the install-tag example from v0.1.13 to v0.1.19 and `CLAUDE.md` from
  v0.1.14 to v0.1.19.
- Removed the stale exact `mergein` timing claim, corrected `save`/`list`/`glimpse`
  syntax, clarified that strL sizing is in UTF-8 bytes, and documented DECIMAL,
  UINT64 and `m:m` limits.
- Kept the version at v0.1.19; no public command was removed or renamed.

### Executable examples

- `examples/parqit_tour.do` now demonstrates and asserts `mergein`, `appendin`,
  `compression_level()`, and all four `parqit set` settings.
- `tests/integration/t13_tour.do` makes the public tour a permanent integration
  test and exercises the batch/GUI contract for `menu` plus the defensive
  `_dlgvars` entry.
- Final lines include `VERDICT(PARQIT_TOUR): PASS` and
  `VERDICT(T13_AUXILIARY_COMMANDS): PASS`.
- The user-owned untracked `examples/parqit_dlg.do` contains local absolute paths.
  It was deliberately left unmodified and untracked rather than committing a
  machine-specific example. `scratch_inj/` and the supplied audit brief were also
  preserved.

## Command-surface coverage map

The dispatcher contains 54 tokens including internal `_dlgvars` (the brief says
55, but the supplied list and `local cmds` each count 54). Every token now has at
least a live command-contract execution; successful GUI mutation remains the one
scenario unavailable in batch.

| Surface cluster | Commands | Primary live coverage |
|---|---|---|
| Plugin/package | `version selftest path` | `m0_smoke`, `t02`, tour/T13 |
| Input/output | `use save describe glimpse open close collect` | `t02`, `t03`, `t06`, `t11`, tour/T13, `v08`, `v09`, `v43`–`v53` |
| Projection/expressions | `keep drop gen egen replace rename order` | `t03`, tour/T13, `v02`, `v13`, `v20`, `v27`, `v28`, `v31`, `v33`, `v42` |
| Ordering/reduction | `sort gsort collapse contract duplicates sample` | `t03`, `t08`, `t09`, tour/T13, `v20`, `v27`, `v35`, `v36`, `v57` |
| Preview/introspection | `count head list show explain ds lookfor` | `t02`, `t05`, `t08`, `t09`, tour/T13, `v43` |
| Session/views | `set view views` | `t06`, `t07`, tour/T13, `v17`, `v40` |
| Two-table | `merge append joinby mergein appendin` | `t04`, `t07`, tour/T13, `v12`, `v14`, `v25`, `v27`, `v35`, `v48`, `v55`–`v57` |
| Shape | `reshape pivot` | `t05`, `t12`, tour/T13, `v27`, `v34`, `v43`, `v55` |
| Raw SQL | `sql query` | `t05`, tour/T13, `v16`, `v31` |
| Statistics | `summarize tabulate misstable levelsof codebook distinct tabstat correlate pwcorr histogram` | `t05`, `t08`, `t09`, tour/T13, `v17`, `v28`, `v42`, `v44`, `v57` |
| GUI glue | `menu _dlgvars` | T13 validates defensive/batch contracts; actual dialog-list population needs GUI Stata |

## A–K audit coverage

| Area | Depth | Work actually performed |
|---|---|---|
| A. Precision/type map | Deep | Read `typemap.cpp` and both fill/save paths; attacked exact Stata ranges, float specials, uint/decimal notes and forged/partial footer stats; reran `v06`, `v11`, `v15`, `v21`, `v38`, `v41`, `v49`, `v51` through the full gate |
| B. Dates/periods | Deep | Traced both epochs and every temporal format; attacked negative half ties and fractional `%tc`; wrote TEMPORAL-ROUND-1 repro and `v54`; retained `v03`, `v05`, `v22`, `v38` |
| C. Column identity | Deep | Read ViewCol/source-name flow and parallel worker offsets; checked rename/collision, reshape/pivot/collapse; expanded `v20` to every-cell assertions with 24 workers; no content swap found |
| D. Missing semantics | Deep | Audited translator, joins, grouping, sort/window and `_merge`; found/fixed reshape and statistics gaps; reproduced the open `m:m` physical-order problem |
| E. Strings | Deep | Audited UTF-8 bytes/chars, 2045/2046 boundary, NUL/invalid UTF-8, hostile names and strL sidecar; exercised `v16`, `v19`, `v32`, `v50`, `v52` and a 290.8 MB strL payload |
| F. Expression translator | Deep | Read parser/function mappings and finite guard; reviewed precedence, Stata round/mod/division/date semantics and unsupported-function errors; found/fixed REGEXM-NULL-1; full unit and `v28`, `v31`, `v33`, `v42` green |
| G. Locale | Deep | Audited locale-independent numeric conversions and ran `v17` with `LC_ALL=pt_PT.UTF-8` plus Stata `set dp comma`; PASS |
| H. Atomicity/errors | Deep, destructive endpoints partial | Audited every ado plugin call/rc check, frame swap, save staging/rename and stale response paths; attacked missing files, schema mismatch, range errors and >2^31 ceiling through existing pins; full disk and kill-9 not run |
| I. C++ boundary safety | Deep static + live boundary tests | Read extern-C catch-all, worker exception capture and 1-based SPI indices; exercised 2,500 variables, parallel fill and >2^31 refusal (`v18`, `v20`, `v53`); no sanitizer run |
| J. Protocol/injection | Deep | Traced user strings through UTF-8 hex, sanitizer and SQL quoting; hostile quotes/pipes/newlines/NUL via `v16` and `v50`; raw `sql/query` remains explicitly raw |
| K. Test integrity | Deep | Audited runner, every verdict, scratch ownership and independent-oracle direction; fixed two Stata-runner defects plus cross-process/unit and repeated-Stata scratch ownership; built the 54-token command coverage map; full suite produced 70/70 PASS verdicts |

## Not verified

- **Power-loss/kill-9 and actual full-disk save:** static staging/rename logic and
  ordinary failure tests passed, but reproducing these endpoints would require a
  disposable mounted filesystem or quota plus process fault injection. No tracked
  fixture was put at risk.
- **Windows and macOS runtime:** CMake and portability surfaces were read, but this
  run built and executed only Linux x86-64. CI or machines for both platforms are
  required.
- **Successful GUI menu/dialog behavior:** batch Stata correctly returns rc 199
  for `menu`; `_dlgvars` is defensively callable. Verifying menu placement,
  repopulation and button actions requires GUI automation or manual Stata GUI.
- **Injected C++ worker exception under Stata:** catch/propagation was read and
  ordinary failures exercised, but no test-only throw hook was added.
- **Extreme footer cardinality:** `describe` was fast and footer-only on the 10M
  file with 153 row groups, but “any size” was intentionally removed rather than
  pretending to test millions of files/row groups.
- **Exact CLI floor for every sub-20ms helper/display command:** the 160-row broad
  canonical harness covers them; exact same-SQL CLI floors were reserved for the
  materializing/load-heavy classes where a wall-clock decomposition is meaningful.

## Test-suite gaps

- No automated successful-GUI test for `menu`, the ten dialogs, or live
  `_dlgvars` LIST mutation. T13 covers only the safe batch/defensive branches.
- No crash-consistency test using kill-9 or an actual exhausted filesystem.
- No Windows/macOS execution matrix in this local gate.
- `merge m:m` physical-order parity has an intentionally failing reproducer, not
  a passing promise test, because the contract remains unresolved.
- No test-only injection of an exception from a parallel fill worker.
- Performance tests record evidence but impose no CI timing threshold; this avoids
  noisy false failures but means regressions require periodic benchmark review.

There is no completely unexercised public data subcommand after T13. The remaining
gaps are environments/failure modes, not missing dispatcher tokens.

## Questions for the maintainer

1. Should lazy `merge m:m` be refused with a loud diagnostic, retained with the
   now-explicit deterministic-order limitation, or redesigned around persistent
   source row IDs? This is the only still-open S0.
2. Is grouped exact-percentile performance important enough to justify a separate
   correctness-first design and benchmark pass? The measured `tabstat` headroom is
   0.793s on the 10M reference, but the implementation is delicate.
3. Should release CI gain a GUI/manual checklist and native Windows/macOS runners,
   or is the current Linux plus static portability gate the intended v0.1.x scope?

## Verification gate

The final working tree passed:

```text
bash tests/release_lint.sh
  release-lint OK: v0.1.19 (03jul2026 / pkg 20260703)

cmake --preset dev
cmake --build build/dev -j
  PASS

ctest --preset dev -j 3 --output-on-failure
  3/3 tests passed (unit + runner contract + eight-process unit concurrency)

./build/dev/parqit_tests
  63/63 test cases passed; 973/973 assertions passed; 1 skipped

STATA=stata-mp BUILD_DIR="$REPO/build/dev" bash tests/run_stata.sh
  69 test files; 70/70 VERDICT lines PASS; 0 FAIL; 69 logs present

STATA=stata-mp BUILD_DIR="$REPO/build/dev" bash tests/run_stata.sh v54
STATA=stata-mp BUILD_DIR="$REPO/build/dev" bash tests/run_stata.sh v55
STATA=stata-mp BUILD_DIR="$REPO/build/dev" bash tests/run_stata.sh v56
STATA=stata-mp BUILD_DIR="$REPO/build/dev" bash tests/run_stata.sh v57
STATA=stata-mp BUILD_DIR="$REPO/build/dev" bash tests/run_stata.sh v20
  each targeted family PASS
```

The audit made no version bump and did not push, tag, alter the global ado tree,
or modify the user's untracked local files.
