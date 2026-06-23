# pq 3.0.7 vs slab issue and benchmark audit

## Executive summary

- `pq` was refreshed with `ssc install pq, replace`; SSC reported all files already up to date. Installed `pq` is 3.0.7.
- The old 14-issue pq repro suite now shows `1/14` fixed and `13/14` still confirmed in `pq`: only issue 01 no longer reproduces; issues 02-14 still reproduce.
- `slab` current dev plugin passed the mapped issue coverage for `14/14` issues, and the full Stata suite emitted 39 PASS verdicts.
- Common-command precision passed `18/18` checks; full benchmark precision passed `24/24` checks. Common-command max absolute diff was `0`; overall max absolute diff was `1.14e-13`.
- Performance numbers below are from the completed 3-repetition reduced benchmark: workers 1.5M rows, firms 500k rows, patents 100k rows. A 10M-row run was attempted first but was externally terminated during the second Stata repetition, so it is preserved as partial evidence and not used for the summary.
- Issue-suite runtime spans from log mtimes: pq repro suite about `18.1s`; slab Stata suite about `50.8s`.

## Issue matrix

|   issue | title                                                   | pq_verdict     | pq_resolved   | slab_verdict   | slab_resolved   | slab_evidence                                  | note                                                                                                                                       |
|--------:|:--------------------------------------------------------|:---------------|:--------------|:---------------|:----------------|:-----------------------------------------------|:-------------------------------------------------------------------------------------------------------------------------------------------|
|       1 | save varlist/options positional corruption family       | NOT REPRODUCED | yes           | PASS           | yes             | T02_USE_OPTIONS; T03_VERBS; V30_RESIDUAL_FIXES | pq 3.0.7 fixes the audited varlist family; slab passed named projection/order and save/partition replacement checks on supported surface.  |
|       2 | renamed/import-sanitized columns load all missing       | CONFIRMED      | no            | PASS           | yes             | V02_RENAMED                                    | slab loads reserved/digit/space/long/unicode names with original values preserved.                                                         |
|       3 | tm/tq/th/ty/tw period values written as daily dates     | CONFIRMED      | no            | PASS           | yes             | V03_PERIOD_DATES; v22_collect_date_no_overflow | slab preserves period counts and display formats without daily-date reinterpretation.                                                      |
|       4 | chunk plus partition_by data loss / replace semantics   | CONFIRMED      | no            | PASS           | yes             | T10_AUDIT_FIXES; V30_RESIDUAL_FIXES            | slab chunk row groups and partition replacement tests passed; partition replace is re-runnable and stale rows are refused without replace. |
|       5 | HH:MM display format writes all-null time column        | CONFIRMED      | no            | PASS           | yes             | V05_HHMM                                       | slab does not classify %tcHH:MM:SS as time-of-day and preserves values.                                                                    |
|       6 | uint32 values above int32 max become missing            | CONFIRMED      | no            | PASS           | yes             | V06_UINT32                                     | slab preserves unsigned/wide integer values instead of silent missing.                                                                     |
|       7 | label option blanks unlabeled numeric values            | CONFIRMED      | no            | PASS           | yes             | V07_LABELS; T01_ROUNDTRIP                      | slab keeps numeric values on disk and round-trips labels separately.                                                                       |
|       8 | failed save reports rc=0 / errors swallowed             | CONFIRMED      | no            | PASS           | yes             | V08_SAVE_ERRORS                                | slab save failures return nonzero rc, including missing dirs/codecs/partition conflicts.                                                   |
|       9 | use, clear destroys in-memory data before validation    | CONFIRMED      | no            | PASS           | yes             | V09_ATOMIC_CLEAR                               | slab failed loads preserve the current in-memory dataset.                                                                                  |
|      10 | duplicate parquet column names silently drop one column | CONFIRMED      | no            | PASS           | yes             | V10_DUP_COLUMNS                                | slab disambiguates duplicate names and retains both columns.                                                                               |
|      11 | unsupported parquet types load as all-missing rc=0      | CONFIRMED      | no            | PASS           | yes             | V11_UNSUPPORTED                                | slab handles decimal values and loudly drops/refuses unsupported list/struct instead of all-missing success.                               |
|      12 | user column _pq_strl_key clobbered by strL helper       | CONFIRMED      | no            | PASS           | yes             | V12_INTERNAL_NAMES                             | slab preserves hostile helper-named columns.                                                                                               |
|      13 | invalid in() ranges silently return empty/full data     | CONFIRMED      | no            | PASS           | yes             | V13_IN_RANGES                                  | slab validates invalid ranges and does not silently return empty/all rows.                                                                 |
|      14 | space in parquet column name makes file unreadable      | CONFIRMED      | no            | PASS           | yes             | V02_RENAMED; V16_INJECTION_HOSTILE             | slab treats whitespace/hostile names as data and sanitizes safely.                                                                         |

## Performance p50 seconds: common commands

| workflow   |    pq |   python |   slab |
|:-----------|------:|---------:|-------:|
| append     | 0.182 |    0.063 |  0.044 |
| describe   | 0.083 |    0.001 |  0.017 |
| merge      | 1.484 |    0.216 |  0.319 |
| path       | 0     |    0     |  0     |
| save       | 1.056 |    0.54  |  1.249 |
| use        | 0.731 |    0.037 |  0.322 |

## Performance p50 seconds: additional workflows

| workflow            |    pq |   python |   slab |
|:--------------------|------:|---------:|-------:|
| workflow_collapse   | 1.617 |    0.096 |  0.239 |
| workflow_filter_gen | 1.656 |    0.206 |  0.245 |

## Precision summary

| category       | workflow            | method   |   checks |   passed |   max_abs_diff |   numeric_null_mismatches |   other_mismatches | verdict   |
|:---------------|:--------------------|:---------|---------:|---------:|---------------:|--------------------------:|-------------------:|:----------|
| common_command | append              | pq       |        3 |        3 |       0        |                         0 |                  0 | PASS      |
| common_command | append              | python   |        3 |        3 |       0        |                         0 |                  0 | PASS      |
| common_command | append              | slab     |        3 |        3 |       0        |                         0 |                  0 | PASS      |
| common_command | describe            | pq       |        3 |        3 |       0        |                         0 |                  0 | PASS      |
| common_command | describe            | python   |        3 |        3 |       0        |                         0 |                  0 | PASS      |
| common_command | describe            | slab     |        3 |        3 |       0        |                         0 |                  0 | PASS      |
| common_command | merge               | pq       |        3 |        3 |       0        |                         0 |                  0 | PASS      |
| common_command | merge               | python   |        3 |        3 |       0        |                         0 |                  0 | PASS      |
| common_command | merge               | slab     |        3 |        3 |       0        |                         0 |                  0 | PASS      |
| common_command | path                | pq       |        3 |        3 |       0        |                         0 |                  0 | PASS      |
| common_command | path                | python   |        3 |        3 |       0        |                         0 |                  0 | PASS      |
| common_command | path                | slab     |        3 |        3 |       0        |                         0 |                  0 | PASS      |
| common_command | save                | pq       |        3 |        3 |       0        |                         0 |                  0 | PASS      |
| common_command | save                | python   |        3 |        3 |       0        |                         0 |                  0 | PASS      |
| common_command | save                | slab     |        3 |        3 |       0        |                         0 |                  0 | PASS      |
| common_command | use                 | pq       |        3 |        3 |       0        |                         0 |                  0 | PASS      |
| common_command | use                 | python   |        3 |        3 |       0        |                         0 |                  0 | PASS      |
| common_command | use                 | slab     |        3 |        3 |       0        |                         0 |                  0 | PASS      |
| extra_workflow | workflow_collapse   | pq       |        3 |        3 |       1.14e-13 |                         0 |                  0 | PASS      |
| extra_workflow | workflow_collapse   | python   |        3 |        3 |       1.14e-13 |                         0 |                  0 | PASS      |
| extra_workflow | workflow_collapse   | slab     |        3 |        3 |       1.14e-13 |                         0 |                  0 | PASS      |
| extra_workflow | workflow_filter_gen | pq       |        3 |        3 |       8.88e-16 |                         0 |                  0 | PASS      |
| extra_workflow | workflow_filter_gen | python   |        3 |        3 |       0        |                         0 |                  0 | PASS      |
| extra_workflow | workflow_filter_gen | slab     |        3 |        3 |       8.88e-16 |                         0 |                  0 | PASS      |

## Full feature-surface timings

- Supplementary run: `480` timing rows, `3` repetitions, `0` failures.
- Data size: 200k worker rows, 500k firm rows, 50k patent rows; this is a breadth benchmark, not the 1.5M-row performance benchmark above.
- Unit: representative end-to-end workflow per feature. For lazy `slab` verbs, timing includes the materializer (`save`, `collect`, or pushed-down stats) that executes the plan.
- Python is the canonical timing: pyarrow for file I/O and DuckDB SQL for relational/statistical workflows.
- Scope: Parquet/common `pq` command families plus implemented `slab` data-processing and introspection commands. Format-specific `pq` SAS/SPSS shortcuts are not timed here because this repo has no SAS/SPSS canonical fixtures and they have no `slab` equivalent surface.

### Common pq/slab features: p50 seconds

| feature               | description                              |   python |    pq |   slab |
|:----------------------|:-----------------------------------------|---------:|------:|-------:|
| append                | append/union-by-name one Parquet file    |    0.041 | 0.1   |  0.043 |
| append_filter         | append filtered rows                     |    0.039 | 0.08  |  0.041 |
| describe              | file schema/row metadata                 |    0.001 | 0.026 |  0.023 |
| describe_detailed     | detailed schema metadata                 |    0.001 | 0.025 |  0.023 |
| merge                 | many-to-one join                         |    0.21  | 0.572 |  0.264 |
| merge_keepusing       | join with projected using columns        |    0.118 | 0.452 |  0.247 |
| path                  | resolve an absolute path                 |    0     | 0     |  0.001 |
| save                  | write in-memory data to Parquet          |    0.165 | 0.171 |  0.308 |
| save_chunk            | write with explicit row-group/chunk size |    0.173 | 0.957 |  0.228 |
| save_compression_zstd | write with explicit zstd compression     |    0.157 | 0.2   |  0.274 |
| save_filter           | write filtered rows                      |    0.072 | 0.185 |  0.04  |
| save_partition_by     | write a partitioned Parquet dataset      |    0.091 | 0.255 |  0.261 |
| save_varlist          | write selected columns                   |    0.077 | 0.05  |  0.07  |
| use                   | read Parquet into host memory            |    0.009 | 0.161 |  0.07  |
| use_drop              | read all except selected columns         |    0.074 | 0.076 |  0.149 |
| use_filter            | read rows satisfying a predicate         |    0.037 | 0.035 |  0.07  |
| use_random_n          | read/sample a fixed row count            |    0.032 | 0.041 |  0.049 |
| use_range             | read a row range                         |    0.016 | 0.029 |  0.034 |
| use_sort              | read sorted output                       |    0.103 | 0.171 |  0.171 |
| use_varlist           | read selected columns                    |    0.006 | 0.017 |  0.021 |

### slab-only features: p50 seconds

| feature            | description                                |   python |   slab |
|:-------------------|:-------------------------------------------|---------:|-------:|
| appendin           | native append with disk data read by slab  |    0.122 |  0.163 |
| codebook           | compact variable diagnostics               |    0.023 |  0.023 |
| collapse           | grouped aggregates                         |    0.075 |  0.102 |
| collect            | materialize a lazy view into Stata memory  |    0.085 |  0.095 |
| contract           | grouped frequencies                        |    0.009 |  0.029 |
| correlate          | correlation matrix                         |    0.012 |  0.029 |
| count              | pushed-down row count                      |    0.002 |  0.008 |
| count_if           | pushed-down conditional count              |    0.004 |  0.005 |
| distinct           | distinct count                             |    0.005 |  0.013 |
| drop_if            | lazy anti-filter                           |    0.097 |  0.091 |
| drop_vars          | lazy drop by varlist                       |    0.08  |  0.089 |
| ds                 | varlist discovery                          |    0.001 |  0.006 |
| duplicates_drop    | deduplicate rows                           |    0.063 |  0.082 |
| duplicates_report  | duplicate report                           |    0.015 |  0.023 |
| egen_by            | group/window egen                          |    0.15  |  0.11  |
| explain            | show DuckDB plan                           |    0.003 |  0.015 |
| gen                | computed column                            |    0.109 |  0.16  |
| glimpse            | describe alias                             |    0.001 |  0.012 |
| gsort              | descending/compound sort                   |    0.166 |  0.172 |
| head               | preview first rows                         |    0.006 |  0.029 |
| histogram_nodraw   | histogram bin computation                  |    0.163 |  0.027 |
| joinby             | many-to-many join                          |    0.131 |  0.241 |
| keep_if            | lazy row filter                            |    0.071 |  0.067 |
| keep_in            | validated row range                        |    0.029 |  0.037 |
| keep_vars          | lazy projection by varlist                 |    0.025 |  0.042 |
| levelsof           | distinct levels as r(levels)               |    0.005 |  0.011 |
| list               | preview selected rows/columns              |    0.007 |  0.022 |
| lookfor            | search variable names/labels               |    0.001 |  0.006 |
| mergein            | native merge with disk lookup read by slab |    0.112 |  0.165 |
| misstable          | missing-value summary                      |    0.006 |  0.017 |
| misstable_patterns | missing-value pattern table                |    0.016 |  0.023 |
| open_data          | promote current Stata data to a lazy view  |    0.153 |  0.021 |
| order              | column order                               |    0.078 |  0.162 |
| pwcorr             | pairwise correlation                       |    0.017 |  0.027 |
| query_qualify      | raw SQL fragment in the pipeline           |    0.051 |  0.027 |
| rename             | column rename                              |    0.089 |  0.139 |
| replace            | conditional replacement                    |    0.102 |  0.143 |
| reshape_long       | wide-to-long reshape                       |    0.052 |  0.085 |
| reshape_wide       | long-to-wide reshape                       |    0.065 |  0.076 |
| sample_count       | deterministic count sample                 |    0.036 |  0.059 |
| show               | show generated SQL                         |    0     |  0.01  |
| sort               | ascending sort                             |    0.156 |  0.164 |
| sql_clear          | raw SQL materialized to memory             |    0.01  |  0.021 |
| sql_save           | raw SQL view saved to Parquet              |    0.026 |  0.018 |
| summarize          | pushed-down summary statistics             |    0.014 |  0.019 |
| summarize_detail   | pushed-down detailed summary               |    0.095 |  0.092 |
| tabstat            | table of summary stats                     |    0.01  |  0.02  |
| tabulate_oneway    | one-way frequency table                    |    0.005 |  0.01  |
| tabulate_twoway    | two-way frequency table                    |    0.014 |  0.027 |
| use_lazy           | open a lazy view without reading rows      |    0.002 |  0.004 |

Supplementary files:

- `feature_surface/REPORT.md`
- `feature_surface/feature_raw.csv`
- `feature_surface/feature_summary_long.csv`
- `feature_surface/feature_p50_wide.csv`
- `feature_surface/feature_failures_wide.csv`
- `feature_surface/feature_surface_stata.log`

## Provenance

- Benchmark canonical: Python/pyarrow for `use`/`save`/`describe`/`path`; Python DuckDB for relational outputs.
- Benchmark repetitions: `3`; DuckDB `1.4.4`; pyarrow `24.0.0`.
- pq update provenance: `provenance/hashes_and_context.txt`; the full local Stata batch log is intentionally not committed because the Stata startup header contains license-identifying text.
- pq issue logs: `pq_verify_suite/` and `pq_verify_suite/VERDICTS_SUMMARY.txt`.
- slab suite log: `slab_run_stata.log`; copied detailed logs in `slab_stata_logs/`.
- Benchmark report: `common_benchmark_small/REPORT.md`; raw files in `common_benchmark_small/benchmark_raw.csv`, `validation.csv`, `benchmark_summary.csv`, `validation_summary.csv`.
- Partial large benchmark: `common_benchmark/` was terminated before complete validation and is not used in the headline tables.
- Hash/provenance file: `provenance/hashes_and_context.txt`.

## Host context

```
Linux athena 5.14.0-611.5.1.el9_7.x86_64 #1 SMP PREEMPT_DYNAMIC Tue Nov 11 08:09:09 EST 2025 x86_64 x86_64 x86_64 GNU/Linux
 10:38:39 up 187 days, 22:36, 20 users,  load average: 10,66, 15,87, 15,87
```
