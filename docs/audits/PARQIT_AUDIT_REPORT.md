# parqit audit report

Date: 2026-06-12
Auditor: Codex (GPT-5)
Repo commit audited: c4c9834bd93fe24ab2b2c0e83ceb41161bb7ab38
Time spent: one interactive audit session, approximately 2 hours

## Executive summary

The existing C++ and Stata suites pass on the checked tree, but the audit found multiple public-contract failures.
Two reproduced cases silently return the wrong dataset/result instead of failing: named `_data` views can read a later dataset, and `reshape long` accepts duplicate `i()` rows that native Stata rejects.
One reproduced returned scalar is wrong: `parqit misstable` returns a boolean-like value in `r(n_complete)`, not the number of complete observations.
One documented option path is accepted but ignored: two-way `tabulate, row col` does not print row/column percentages.
The public `chunk()` option for `parqit save` is documented in the README and build brief but absent from the parser/implementation.
I would not tag this as release-ready for research workflows until the S0 findings are fixed and covered by regression tests.

Top recommended actions:
1. Fix PARQIT-01 before release: `parqit open _data, name()` must snapshot or pin a unique backing file per named view.
2. Fix PARQIT-02 before release: `reshape long` must enforce Stata's `i()` uniqueness contract before changing the plan.
3. Fix PARQIT-03 and add exact stored-result tests for exploration commands.
4. Either implement or remove the public `chunk()` surface (PARQIT-05) consistently across README, help, and build brief.
5. Add regression tests from `audit_repro/` plus output assertions for `tabulate, row col` and plugin exception boundaries.

## Findings table

| ID | Severity | Confidence | Area | Title | Repro? |
|---|---|---|---|---|---|
| PARQIT-01 | S0 | certain | E, D | Named `_data` views share one overwritten bridge file | yes |
| PARQIT-02 | S0 | certain | G, K | `reshape long` accepts duplicate `i()` rows that Stata rejects | yes |
| PARQIT-03 | S2 | certain | H, K | `misstable` returns wrong `r(n_complete)` | yes |
| PARQIT-04 | S2 | likely | H, K | `tabulate ..., row col` options are accepted but ignored | no |
| PARQIT-05 | S3 | certain | I, K | Public `parqit save, chunk()` option is documented but not implemented | no |
| PARQIT-06 | S3 | likely | J | `stata_call` has no catch-all exception boundary | no |
| PARQIT-07 | S4 | certain | K | `collect` view-lifetime docs disagree | no |

## Findings

### PARQIT-01 - Named `_data` views share one overwritten bridge file

- **Severity:** S0 silent wrong results/data corruption
- **Confidence:** certain (reproduced)
- **Where:** `src/ado/p/parqit.ado:1094-1098`; view plans later scan the stored file path lazily.
- **Evidence:** `_parqit_open` uses `local bridge "`c(tmpdir)'/_parqit_opendata_`c(pid)'.parquet"`, erases it, saves the current in-memory data to that same path, then opens it under the requested name. A later `parqit open _data, name(second)` in the same Stata process overwrites the backing file for `name(first)`.
- **Repro:** `audit_repro/repro_open_data_named_view_overwrite.do`
- **Repro transcript:** the script opens `x=1` as `first`, opens `x=2` as `second`, switches back to `first`, then `parqit collect` returns `x=2`: `VERDICT(REPRO_OPEN_DATA_OVERWRITE): FAIL - first view read the second dataset`.
- **Impact:** any user keeping multiple named views promoted from Stata memory can silently analyze the most recently promoted dataset while believing they are analyzing an older named view.
- **Suggested direction:** give every `_data` promotion a unique immutable temp Parquet path and make the view own its lifetime, or eagerly copy/snapshot the file into a registry-managed location. Add a regression that collects an older named `_data` view after opening a newer one.

### PARQIT-02 - `reshape long` accepts duplicate `i()` rows that Stata rejects

- **Severity:** S0 silent wrong results/data corruption
- **Confidence:** certain (reproduced)
- **Where:** `src/ado/p/parqit.ado:1320-1352`; `src/plugin/plugin_view.cpp:1186-1193`; `src/engine/view.cpp:951-1048`.
- **Evidence:** the ado layer passes `i()` and `j()` through. The plugin directly calls `g_view_ref().reshape_long(...)`. The engine validates stubs, suffix balance, and type consistency, then emits a `UNION ALL`, but there is no `i()` uniqueness check before accepting the reshape. Native Stata aborts with duplicate `i()` rows.
- **Repro:** `audit_repro/repro_reshape_long_duplicate_i.do`
- **Repro transcript:** native `reshape long inc, i(id) j(year)` returns nonzero for duplicated `id`; parqit accepts the same wide data and collects 6 rows: `VERDICT(REPRO_RESHAPE_LONG_DUPLICATE_I): FAIL - parqit accepted duplicate i() that native Stata rejects`.
- **Impact:** duplicate panel identifiers can be converted into apparently valid long data instead of stopping at the same contract violation Stata reports. This is a silent data-shape error.
- **Suggested direction:** before mutating the view, run a uniqueness check equivalent to Stata's `i()` contract against the current pipeline. Return a loud nonzero error on duplicates, and test both duplicate and unique cases.

### PARQIT-03 - `misstable` returns wrong `r(n_complete)`

- **Severity:** S2 wrong behaviour, workaroundable only if users recompute manually
- **Confidence:** certain (reproduced)
- **Where:** `src/plugin/plugin_view.cpp:1442-1480`; `src/ado/p/parqit.ado:1492-1494`; `src/ado/p/parqit.ado:2371-2395`; public stored result in `src/ado/p/parqit.sthlp:208-209`.
- **Evidence:** the plugin returns total rows and per-variable missing counts. `_parqit_print_misstable` increments `total_missing_vars` when a variable has any missing values, then saves `parqit_miss_complete` as `1` if no variable has missing values, else `0`. `_parqit_misstable` returns that as `r(n_complete)`. That is not a complete-observation count.
- **Repro:** `audit_repro/repro_misstable_n_complete.do`
- **Repro transcript:** with rows `(1,10)`, `(.,20)`, `(3,.)`, native complete observations are 1; parqit returns 0: `VERDICT(REPRO_MISSTABLE_N_COMPLETE): FAIL - r(n_complete) is not complete-observation count`.
- **Impact:** scripts using `r(n_complete)` for sample accounting, QA checks, or exclusion reporting get the wrong scalar whenever any variable has missing data.
- **Suggested direction:** either compute complete observations with a row-wise all-nonmissing predicate over the selected variables, or rename/remove this stored result. Add a test with at least one complete and multiple incomplete rows.

### PARQIT-04 - `tabulate ..., row col` options are accepted but ignored

- **Severity:** S2 wrong behaviour, visible but easy to miss in logs
- **Confidence:** likely (clear code-read)
- **Where:** `src/ado/p/parqit.ado:1438-1465`; `src/ado/p/parqit.ado:2439-2492`; public syntax in `src/ado/p/parqit.sthlp:69`.
- **Evidence:** `_parqit_tabulate` parses `ROW COL` and sets `_sq_row` / `_sq_col`, but those locals are only set after the plugin call and `_parqit_print_tab2` never reads them. `_parqit_print_tab2` prints only counts and totals.
- **Repro:** not added; the existing `tests/integration/t09_explore2.do` calls `parqit tabulate g year, row col` but does not assert that percentages are present.
- **Impact:** users requesting row or column percentages see a valid-looking two-way table with only counts. A log scan may not reveal that the requested percentage panels were omitted.
- **Suggested direction:** either implement Stata-like row/column percentage output and stored results, or reject `row`/`col` with a nonzero error until implemented. Add log-content assertions.

### PARQIT-05 - Public `parqit save, chunk()` option is documented but not implemented

- **Severity:** S3 robustness/public-surface hazard
- **Confidence:** certain
- **Where:** `parqit_build_prompt.md:150-151`; `README.md:187-188`; parser at `src/ado/p/parqit.ado:1007-1010`.
- **Evidence:** the build brief and README advertise `parqit save <dest> [, replace partition_by() compression() chunk()]`. `_parqit_save` parses `replace`, `data`, `compression()`, `compression_level()`, and `partition_by()`, but no `chunk()` option. `rg chunk` found no save-option implementation; only Arrow/DuckDB internal chunk handling and docs references.
- **Repro:** not necessary; this is a direct parser/contract mismatch.
- **Impact:** users following the public command surface cannot use the advertised option. Worse, release tests can pass while a documented core materialiser option is absent.
- **Suggested direction:** decide whether `chunk()` is part of v0.1. If yes, implement and test it with `partition_by`. If no, remove it from `README.md`, `parqit_build_prompt.md`, future help, and assumptions.

### PARQIT-06 - `stata_call` has no catch-all exception boundary

- **Severity:** S3 robustness/portability hazard
- **Confidence:** likely (code-read, not reproduced)
- **Where:** `src/plugin/parqit_plugin.cpp:133-172`.
- **Evidence:** `PARQIT_EXPORT ST_retcode stata_call(int argc, char *argv[])` constructs `std::vector<std::string>` and dispatches directly to many C++ handlers without `try/catch`. Most handlers return `ST_retcode` on expected errors, but `std::bad_alloc`, filesystem exceptions, JSON/library exceptions, or uncaught DuckDB wrapper exceptions can still cross an `extern "C"` Stata plugin boundary.
- **Repro:** none; I did not inject allocator/filesystem failures.
- **Impact:** rare unexpected exceptions can terminate Stata instead of returning a loud parqit error. This is especially relevant on HPC nodes, temporary-directory failures, and malformed external inputs.
- **Suggested direction:** wrap the entire dispatcher in `try { ... } catch (const std::exception&) { ... } catch (...) { ... }`, print a bounded message through `SF_error`, and return a real nonzero `ST_retcode`. Add a test-only command or fault-injection path to prove the boundary.

### PARQIT-07 - `collect` view-lifetime docs disagree

- **Severity:** S4 docs/public-surface inconsistency
- **Confidence:** certain
- **Where:** `README.md:187`; `src/ado/p/parqit.sthlp:131-134`.
- **Evidence:** README says `parqit collect [, clear]` "Consumes the view." The help file says the view "stays open" and collecting again re-executes. The audit prompt and current tests assume non-consuming exploration semantics.
- **Repro:** not needed.
- **Impact:** users and tests can disagree about whether a collect should close the view. This is documentation-level today, but it affects public command semantics.
- **Suggested direction:** choose one contract and align README, help, tests, and changelog. Based on current help/tests, the likely intended wording is "does not consume the view."

## Promise audit

| Claim | Source | Verdict | Evidence pointer |
|---|---|---|---|
| Only final materialisers bring data into Stata or write Parquet; lazy verbs build a plan. | `src/ado/p/parqit.sthlp:99-107`, build brief | Partially verified | Existing suites pass; code structure is lazy, but PARQIT-01 shows `_data` named views can point at a mutable bridge file. |
| Several named views can be open at once and holding many costs nothing. | `src/ado/p/parqit.sthlp:119-128` | False for `_data` views | PARQIT-01. File-backed views were not found broken in this audit. |
| `parqit collect` atomically replaces data only after load is valid. | `src/ado/p/parqit.sthlp:131-133` | Partially verified | `tests/verify_suite/v09_atomic_clear.do` passed; destructive mid-collect failure was not tested. |
| `parqit save` writes Parquet without touching Stata memory. | `README.md:187-188`, `src/ado/p/parqit.sthlp:135-137` | Partially verified | Existing tests passed; I did not instrument memory or prove every path, and `_parqit_save, data` intentionally exports memory. |
| `parqit save, chunk()` is part of the materialiser surface. | `parqit_build_prompt.md:150-151`, `README.md:187-188` | False | PARQIT-05. |
| `reshape long|wide` provides Stata-compatible reshape behaviour. | `README.md:173`, `src/ado/p/parqit.sthlp:54` | False for `long` duplicate `i()` | PARQIT-02. |
| `parqit misstable` reports missing counts/share and returns useful stored results including `r(n_complete)`. | `src/ado/p/parqit.sthlp:208-209`; stored result implied by ado | False for `r(n_complete)` | PARQIT-03. Printed per-variable missing counts matched the small case. |
| `parqit tabulate a b, row col` supports row/column options. | `src/ado/p/parqit.sthlp:69` | False | PARQIT-04. |
| `merge` validates uniqueness contracts and `m:m` follows Stata sequential pairing. | `src/ado/p/parqit.sthlp:159-165` | Partially verified | Existing two-table tests passed; I did not build adversarial duplicate-order cases for `m:m`. |
| Metadata survives via `parqit.*` Parquet key-value metadata. | `src/ado/p/parqit.sthlp:108-112` | Partially verified | `tests/roundtrip/t01_basic_roundtrip.do` and `tests/verify_suite/v07_label_fidelity.do` passed. I did not exhaust notes/characteristics/value-label edge cases. |
| Date/time and numeric range contracts are preserved. | build brief/type contract; `src/ado/p/parqit.sthlp` type section | Partially verified | `v03_period_dates`, `v05_hhmm_datetime`, `v06_uint32_overflow`, and `v13_in_ranges` passed; I did not generate all pyarrow adversarial payloads requested in B. |
| Unsupported types fail loudly. | build brief/type contract | Verified on covered fixtures | `tests/verify_suite/v11_unsupported_types.do` passed; not exhaustive over LIST/STRUCT/DECIMAL combinations. |
| `parqit collect` consumes the view. | `README.md:187` | Conflicting docs | PARQIT-07. |

## Coverage map

| Area | What I actually did | Depth |
|---|---|---|
| A. Protocol and injection | Read request/dispatch/sanitizer-related code, existing unit tests for hex/request/sanitize; did not write new SQL-injection payloads. | partial |
| B. Type map and numeric boundaries | Read type-map and IO code; ran existing C++ tests and Stata verify suite including dates, uint32, unsupported types, ranges. | partial |
| C. Missing-value semantics | Read expression/sort/stat paths; wrote and ran a reduced `gsort -x` missing-order repro that passed; found `misstable` stored-result bug. | partial |
| D. Atomicity and errors | Ran `v08_save_errors_loud` and `v09_atomic_clear`; inspected plugin-call `_rc` checks in touched paths. No full-disk or kill tests. | partial |
| E. Plugin global state and resources | Read named-view and `_data` bridge code; reproduced stale/overwritten backing file bug. | deep for `_data`, partial overall |
| F. Locale | Considered from code paths; did not run `LC_ALL=pt_PT.UTF-8` or `set dp comma` adversarial tests. | skim |
| G. Expression translator | Read expression/view code enough to assess reshape and sort; did not exhaust operator/function differences. | partial |
| H. Tests themselves | Ran `ctest --test-dir build/dev --output-on-failure` and `bash tests/run_stata.sh`; inspected test file inventory and gaps. | partial |
| I. Build/CI/release | Read CMake/workflow/build docs; ran `ldd build/dev/parqit.plugin`, which showed dynamic `libstdc++.so.6` in the dev build. Did not rebuild release in CI container. | partial |
| J. C++ safety boundary | Read exported plugin dispatcher and entry boundary; did not fault-inject exceptions or strL sidecar corruption. | partial |
| K. Promise audit | Checked README, build brief, help, assumptions, and selected behaviour. Not exhaustive claim extraction. | partial |

Test commands run:

```bash
ctest --test-dir build/dev --output-on-failure
bash tests/run_stata.sh
```

Observed result: the C++ unit target passed; the Stata wrapper reported PASS for the integration, verify-suite, and roundtrip scripts present in `tests/`.

Additional repro scripts written:

```text
audit_repro/repro_open_data_named_view_overwrite.do
audit_repro/repro_reshape_long_duplicate_i.do
audit_repro/repro_misstable_n_complete.do
audit_repro/repro_gsort_missing_order.do
```

The first three intentionally fail on the audited tree. The `gsort` script passed in the reduced ordinary-missing case and is kept as nonfinding evidence.

## Not verified

- I did not run full-disk, kill-9, interrupted-save, or temp-directory permission failure tests.
- I did not run locale adversarial tests with both process `LC_ALL=pt_PT.UTF-8` and Stata `set dp comma`.
- I did not generate pyarrow adversarial Parquet files for every type boundary: uint64 above 2^53, DECIMAL scale, NaN/Inf, negative pre-1960 floor division, LIST/STRUCT variants, or invalid UTF-8.
- I did not rebuild release artifacts in the AlmaLinux 8 CI container, inspect release zips, or prove old-glibc/static-libstdc++ properties. The local dev plugin dynamically links `libstdc++.so.6`.
- I did not prove all raw SQL/query escape-hatch isolation properties after later non-raw stages.
- I did not test very wide datasets near Stata macro limits or 2500+ variable manifests.
- I did not audit strL sidecar truncation/corruption handling beyond existing suite coverage.
- I did not exhaust every public promise in README/help; the table above focuses on high-risk and checked claims.

## Test-suite gaps

- No regression for multiple named `parqit open _data, name()` views preserving independent backing data.
- No regression that `reshape long` rejects duplicate `i()` combinations like native Stata.
- No stored-result assertion for `parqit misstable r(n_complete)` as complete-observation count.
- `tabulate ..., row col` is called in the existing suite but the output is not asserted for row/column percentages.
- The verify-suite numbering lacks `v01` and `v04` equivalents from the comparable pq audit pattern; chunk/partition behaviour is especially uncovered because `chunk()` is not implemented.
- No automated promise audit that public syntax in README/help matches ado parser syntax.
- Limited negative tests for locale-dependent numeric formatting and parsing.
- No fault-injection test for an uncaught C++ exception crossing `stata_call`.
- `parqit set threads|memory_limit|tempdir` has shallow coverage relative to its resource-management promises.
- Two-table `m:m` sequential-pairing equivalence is not stress-tested with adversarial duplicate ordering.

## Questions for the maintainer

1. Should `parqit open _data, name(view)` be a snapshot of the in-memory dataset at call time? The docs imply yes, and the current shared temp file breaks that model.
2. Is `r(n_complete)` intended to mean the number of observations complete across the selected variables? If not, the name and help need to say what it means.
3. Is `chunk()` in scope for v0.1, or is it a stale build-brief/README promise?
4. Should `collect` consume the view or leave it open? README and help currently disagree.
5. For `tabulate, row col`, should parqit match Stata's displayed percentage panels exactly, or should unsupported panels be rejected until exact output parity is implemented?
