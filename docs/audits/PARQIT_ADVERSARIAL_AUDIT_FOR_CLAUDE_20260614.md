# parqit adversarial audit for Claude

Date: 2026-06-14
Auditor: Codex (GPT-5)
Repo commit audited: `3b9e0cf4ada4d40075404e639a03425b7a16b3f1`
Mode: no source-code edits; only this Markdown report was written.

## Executive summary

The current tree is materially stronger than the 2026-06-12 audit target. The old
S0/S2 findings around `_data` view aliasing, `reshape long`, `misstable`,
`tabulate row col`, missing `chunk()`, and uncaught plugin exceptions have been
fixed and are covered by `tests/integration/t10_audit_fixes.do`.

I did not find a reproduced silent wrong-result bug in the current local run. The
full local C++ and Stata suites pass:

```text
ctest --test-dir build/dev --output-on-failure
bash tests/run_stata.sh
```

Observed: C++ `1/1` passed; Stata wrapper reported 35 PASS verdicts
(integration, verify_suite, roundtrip) in `/tmp/parqit_tests.ATaJ55`.

The remaining adversarial issues are contract/release risks rather than obvious
S0 runtime corruption. The two highest-value things for Claude to study are:

1. Decide whether extended missings must be truly lossless. The README/build
   thesis still says "lossless", but `.a`-`.z` values currently round-trip as
   plain `.`; only the label definitions survive.
2. Decide whether Parquet `NULL`-typed columns may become an all-missing Stata
   byte variable. The build brief says `LIST/STRUCT/NULL` should drop/error; the
   code and tests now encode a different decision.

## Findings table

| ID | Severity | Confidence | Area | Title | Repro? |
|---|---|---|---|---|---|
| PARQIT-C01 | S2 | certain | Type/metadata contract | Extended missing values are not lossless despite the top-level promise | existing test proves |
| PARQIT-C02 | S2/S3 | certain | Unsupported Parquet types | `NULL`-typed columns load as all-missing byte variables, contrary to the build brief | existing test proves |
| PARQIT-C03 | S3 | certain | Release surface | Version/status metadata is stale and split across release docs | code-read |
| PARQIT-C04 | S3 | possible | Portability | Parallel Stata SPI fill is enabled by default but only locally verified | code-read + tests |

Severity note: C01/C02 are S2 if the build brief remains authoritative; downgrade
only if the maintainer explicitly changes the public contract.

## Findings

### PARQIT-C01 - Extended missing values are not lossless despite the top-level promise

- **Severity:** S2 wrong behavior under the advertised lossless round-trip contract.
- **Confidence:** certain.
- **Where:** `README.md:41-44`, `README.md:291-294`, `README.md:308-311`;
  `ASSUMPTIONS.md:109-113`; `src/plugin/plugin_io.cpp:1585-1594`;
  `tests/roundtrip/t01_basic_roundtrip.do:102-104`.
- **Evidence:** README says "Lossless Stata round-trips" and "`parqit -> parqit` is
  lossless", but the limitations section admits `.a`-`.z` collapse to Parquet
  null. `ASSUMPTIONS.md` is more explicit: metadata-based restoration of extended
  missing positions is planned, and until it ships parqit -> parqit keeps plain `.`.
  The save code writes every Stata missing, including extended missings, through
  DuckDB null validity bits and only records a warning list. The roundtrip suite
  asserts this current behavior: `.a` comes back as `.`.
- **Impact:** survey-style missing categories (`.a = refused`, `.b = don't know`,
  etc.) lose their value identity after a parqit save/use round-trip. Value-label
  definitions survive, including labels attached to `.a`, but the restored cells
  no longer contain `.a`, so downstream Stata code cannot distinguish the
  categories.
- **pq comparison:** this is still much better than the pq label bug class:
  parqit preserves numeric payloads and label metadata instead of stringifying or
  blanking partially-labelled values. But it is not yet the lossless Stata
  round-trip promised by the product thesis.
- **Suggested direction:** either implement the planned metadata/RLE restoration
  for extended missing positions, or weaken the public "lossless" claim so it
  explicitly excludes extended missing value identity. If the claim remains,
  add a regression that writes `.a` and asserts readback is `.a`, not plain `.`.

### PARQIT-C02 - `NULL`-typed columns load as all-missing byte variables, contrary to the build brief

- **Severity:** S2/S3 contract drift; visible note, but wrong schema behavior if
  the brief is treated as fixed.
- **Confidence:** certain.
- **Where:** `parqit_build_prompt.md:179-182`, `parqit_build_prompt.md:241-242`;
  `src/engine/typemap.cpp:234-240`; `src/plugin/plugin_view.cpp:163-166`;
  `tests/verify_suite/v11_unsupported_types.do:1-4`, `:38-44`.
- **Evidence:** the build brief says `LIST/STRUCT/NULL` types should be
  dropped-with-message or error and "never a column of silent missings". The
  current type map special-cases `DUCKDB_TYPE_SQLNULL` as `CAST(NULL AS TINYINT)`,
  transfer `Null`, Stata type `byte`, with note "NULL-typed column: byte variable
  with every value missing". The verify test now asserts that behavior.
- **Impact:** a foreign Parquet file with a true Null column creates a Stata
  variable that is indistinguishable from a real byte variable whose observations
  all happen to be missing. That is much safer than pq's broad unsupported-type
  all-missing bug, but it still contradicts the explicit brief.
- **pq comparison:** pq's audit finding 11 was broader: `decimal/list/struct/null`
  could load as all missing under rc 0. parqit fixes decimal/list/struct and is loud,
  but the Null column remains as an all-missing column by design.
- **Suggested direction:** choose one contract. If the brief is authoritative,
  drop Null columns with a warning or error when every column would be dropped.
  If all-missing Null columns are intentional, update the build brief/README/help
  and make the note part of the public type contract.

### PARQIT-C03 - Version/status metadata is stale and split across release docs

- **Severity:** S3 release/package hazard.
- **Confidence:** certain.
- **Where:** `CMakeLists.txt:2`; `src/ado/p/parqit.ado:1`;
  `src/ado/p/parqit.sthlp:2`; `README.md:20-23`; `src/ado/p/parqit.pkg:23`;
  `CHANGELOG.md:7`, `CHANGELOG.md:116`, `CHANGELOG.md:168`.
- **Evidence:** CMake and the ado banner say `0.1.2`; the help header still says
  `0.1.0`; README status still says `v0.1.0`; `parqit.pkg` keeps
  `Distribution-Date: 20260612`; CHANGELOG has an `[Unreleased]` section at the
  top and another `[Unreleased]` section below `0.1.2`.
- **Impact:** a release/SSC package can ship a coherent binary but stale public
  docs. This matters because several important behaviors changed after 0.1.0:
  named-view fixes, `chunk()`, `relaxed`, parallel fill, `mergein`/`appendin`,
  and performance tips.
- **pq comparison:** pq's release workflow is useful prior art for per-platform
  binary renaming and SSC zip assembly; parqit's workflow is cleaner in CMake terms,
  but the package metadata still needs one release checklist that updates all
  user-visible version/date surfaces together.
- **Suggested direction:** before tagging, align README status, `.sthlp` banner,
  `parqit.pkg` distribution date, CHANGELOG sectioning, and the plugin version.
  Add a cheap release lint that compares `project(parqit VERSION ...)`, the ado
  banner, the sthlp banner, and package date.

### PARQIT-C04 - Parallel Stata SPI fill is enabled by default but only locally verified

- **Severity:** S3 portability hazard, not a reproduced local bug.
- **Confidence:** possible.
- **Where:** `src/plugin/plugin_io.cpp:823-878`, `src/plugin/plugin_io.cpp:903-958`;
  `ASSUMPTIONS.md:248-270`; `tests/verify_suite/v20_parallel_fill.do:1-10`.
- **Evidence:** reads above 50k rows use worker threads that call
  `SF_vstore`/`SF_sstore` into disjoint cells. The code has careful queue/error
  handling and V20 validates a 1.5M-row read against pyarrow on this Linux host.
  The justification is empirical: pq also calls Stata store functions from Rayon
  workers over disjoint row ranges. However, the GitHub CI cannot run Stata, so
  this default-on path is not proven on macOS/Windows or across Stata releases.
- **Impact:** if the Stata Plugin Interface's store functions are not reentrant
  on a target platform/version, the failure mode would be severe: memory
  corruption, crash, or mis-filled rows during materialisation. The local evidence
  is good; the release evidence is not yet broad.
- **pq comparison:** pq is a meaningful production precedent and was checked at
  upstream `4cc5816`, with Rayon used in `src/read.rs` row-range processing. That
  is evidence, not a formal StataCorp guarantee.
- **Suggested direction:** before release, run V20 and the full Stata suite on the
  actual release platforms/binaries. Consider documenting `PARQIT_FILL_THREADS=0`
  as the conservative fallback and defaulting to serial on any unverified platform
  until a platform smoke matrix exists.

## Important non-findings

- The old named `_data` view overwrite bug appears fixed. `_parqit_open` now writes
  a per-promotion bridge path using `PARQIT_OPENDATA_SEQ`, and the plugin owns and
  deletes bridge files on close/replace.
- The old `reshape long` duplicate-`i()` bug appears fixed. `cmd_view_reshape`
  now runs a duplicate aggregation before mutating the plan.
- The old `misstable r(n_complete)` bug appears fixed. The plugin now computes
  complete rows with a row-wise all-nonmissing predicate.
- The old `tabulate ..., row col` ignored-options bug appears fixed. `_parqit_print_tab2`
  reads `parqit_tab2_row` and `parqit_tab2_col` and prints row/column percentages.
- The old missing `save, chunk()` public option appears fixed and covered by
  `tests/integration/t10_audit_fixes.do`.
- The old uncaught `stata_call` exception boundary appears fixed with a top-level
  `try/catch` and a `selftest throw` regression.

## pq comparison notes

Audited references:

- Local pq audit package:
  `/home/mangelo/Documents/BPLIM_GitHub/pq_audit_2026-06-11/PQ_AUDIT_REPORT.md`
- Fresh upstream clone:
  `jrothbaum/stata_parquet_io @ 4cc58164723662f843c92a3f43748176a90c2e7c`

The pq audit's highest-risk classes were positional save corruption, renamed
columns loading all missing, period-date corruption, chunk/partition row loss,
hh:mm time all-null writes, uint32 overflow to missing, lossy label stringifying,
rc 0 on save failures, non-atomic `use, clear`, duplicate-name loss,
unsupported-type all-missing, internal helper clobber, invalid `in()` behavior,
and space-name failures.

The current parqit suite directly targets those hazard classes:

- `v02`, `v10`, `v16`, `v18` cover hostile names, duplicate names, injection,
  and wide manifests.
- `v03`, `v05`, `v06`, `v15`, `v19`, `v22` cover dates/times, uint32, float
  extremes, strL boundary, and DATE collect overflow.
- `v07`, `t01_roundtrip` cover label metadata and Stata metadata, with the
  extended-missing caveat in PARQIT-C01.
- `v08`, `v09`, `t10_audit_fixes` cover loud errors, atomic loads, `chunk()`,
  and old audit regressions.
- `v14`, `t04`, `t07`, `v25` cover joins/mergein/appendin and view-using paths.

Overall: parqit is no longer merely matching pq's I/O surface. For Parquet
workflows it is a superset, and the core pq corruption families are mostly
guarded by design. The remaining concerns are narrower but important because
the parqit pitch is stronger: "lossless", "metadata round-trip", and "release
package".

## Promise audit

| Claim | Source | Verdict | Evidence |
|---|---|---|---|
| Lazy verbs build a plan; `collect`/`save` materialise | README/help/build brief | Verified on local suite | full Stata suite PASS; view code compiles SQL stages |
| `parqit collect` is atomic and leaves view open | help + tests | Verified locally | `v09_atomic_clear`, `t06_named_views`, `t10_audit_fixes` PASS |
| `parqit save` writes without touching Stata memory when a view is open | README/help | Partially verified | code path `cmd_view_save` uses DuckDB `COPY`; no memory instrumentation |
| Metadata round-trip is lossless | README | False/overbroad | extended missing values collapse to plain `.`; see PARQIT-C01 |
| `LIST/STRUCT/NULL` drop/error, never all-missing | build brief | False for Null | see PARQIT-C02 |
| pq hazard classes are ported into regression tests | build brief | Mostly verified | verify suite v02-v26 maps to the pq audit classes |
| Linux release/HPC compatibility | README/workflow/CMake | Not fully verified | local dev `ldd` links dynamic libstdc++; release container not rebuilt in this audit |
| Versioned release metadata is aligned | README/sthlp/pkg/CMake/changelog | False | see PARQIT-C03 |

## Coverage map

| Area | What I checked | Depth |
|---|---|---|
| Contract baseline | Read `README.md`, `parqit_build_prompt.md`, `CLAUDE.md`, `ASSUMPTIONS.md`, help/pkg/changelog | deep |
| Existing regressions | Read old audit report and current fixes; ran full local suites | deep |
| pq comparison | Read local pq audit summary and cloned upstream `stata_parquet_io` current HEAD | partial |
| Type/metadata | Read typemap, save/read metadata paths, roundtrip and verify tests | deep for Null/extended missings; partial overall |
| Atomic/error paths | Read `copy_out_parquet`, `save_data`, `view_save`, test V08/V09 results | partial |
| Plugin/thread boundary | Read `stata_call`, parallel fill worker code, pq Rayon precedent, V20 | partial |
| Release/CI | Read CMake, workflow, package/help/changelog; did not build release zips | partial |

## Not verified

- I did not build release artifacts in the AlmaLinux 8 CI container or inspect
  actual release zips.
- I did not run Stata tests on Windows/macOS, so parallel SPI fill portability is
  not proven outside this Linux host.
- I did not run destructive full-disk, permission, or kill tests.
- I did not create new repro files because the user requested no changes except
  this report; findings above use existing tests and code evidence.
- I did not exhaust every expression translator edge case; existing unit tests
  and verify tests cover many, but not all, Stata function semantic differences.

## Test-suite gaps

- Add a release lint for version/date/doc synchronization.
- If "lossless" remains the claim, add an extended-missing restoration test that
  fails until `.a`-`.z` cell identity survives.
- Add an explicit `NULL` Parquet-column contract test matching the final decision
  (drop/error vs all-missing byte).
- Add a platform smoke matrix for `v20_parallel_fill` on release binaries.
- Add a test that asserts the Stata test runner fails on any `VERDICT(...): FAIL`
  line, even if a later PASS line exists.

## Questions for the maintainer / Claude

1. Is extended-missing cell identity required for v0.1.x, or should "lossless"
   explicitly exclude `.a`-`.z` values until the RLE metadata plan ships?
2. Should a Parquet Null column be represented as an all-missing byte variable, or
   should the build brief's drop/error rule win?
3. Is the default-on parallel fill acceptable before macOS/Windows Stata
   verification, given that CI cannot run Stata?
4. Should the next release be `0.1.3` or still `0.1.2` after the current
   `[Unreleased]` changes?
