# slab vs pq vs Python canonical benchmark

- Repetitions: 3
- Numeric tolerance: 1e-08
- Canonical implementation: Python/pyarrow for use/save/describe/path; Python DuckDB for relational outputs.
- Common pq/slab commands covered: `describe`, `path`, `use`, `save`, `merge`, `append`.
- Additional workflows retained: `workflow_filter_gen`, `workflow_collapse`; pq uses native Stata after `pq use` there.
- For `use`, timing covers read into host memory only; the Parquet validation dump is written after the timer.
- For `save`, the input table is loaded before the timer; timing covers memory-to-Parquet write only.

## Precision: Common Commands

| category       | workflow   | method   |   checks |   passed |   max_abs_diff |   numeric_null_mismatches |   other_mismatches |   missing_in_canonical |   missing_in_candidate | verdict   |
|:---------------|:-----------|:---------|---------:|---------:|---------------:|--------------------------:|-------------------:|-----------------------:|-----------------------:|:----------|
| common_command | append     | pq       |        3 |        3 |              0 |                         0 |                  0 |                      0 |                      0 | PASS      |
| common_command | append     | python   |        3 |        3 |              0 |                         0 |                  0 |                      0 |                      0 | PASS      |
| common_command | append     | slab     |        3 |        3 |              0 |                         0 |                  0 |                      0 |                      0 | PASS      |
| common_command | describe   | pq       |        3 |        3 |              0 |                         0 |                  0 |                      0 |                      0 | PASS      |
| common_command | describe   | python   |        3 |        3 |              0 |                         0 |                  0 |                      0 |                      0 | PASS      |
| common_command | describe   | slab     |        3 |        3 |              0 |                         0 |                  0 |                      0 |                      0 | PASS      |
| common_command | merge      | pq       |        3 |        3 |              0 |                         0 |                  0 |                      0 |                      0 | PASS      |
| common_command | merge      | python   |        3 |        3 |              0 |                         0 |                  0 |                      0 |                      0 | PASS      |
| common_command | merge      | slab     |        3 |        3 |              0 |                         0 |                  0 |                      0 |                      0 | PASS      |
| common_command | path       | pq       |        3 |        3 |              0 |                         0 |                  0 |                      0 |                      0 | PASS      |
| common_command | path       | python   |        3 |        3 |              0 |                         0 |                  0 |                      0 |                      0 | PASS      |
| common_command | path       | slab     |        3 |        3 |              0 |                         0 |                  0 |                      0 |                      0 | PASS      |
| common_command | save       | pq       |        3 |        3 |              0 |                         0 |                  0 |                      0 |                      0 | PASS      |
| common_command | save       | python   |        3 |        3 |              0 |                         0 |                  0 |                      0 |                      0 | PASS      |
| common_command | save       | slab     |        3 |        3 |              0 |                         0 |                  0 |                      0 |                      0 | PASS      |
| common_command | use        | pq       |        3 |        3 |              0 |                         0 |                  0 |                      0 |                      0 | PASS      |
| common_command | use        | python   |        3 |        3 |              0 |                         0 |                  0 |                      0 |                      0 | PASS      |
| common_command | use        | slab     |        3 |        3 |              0 |                         0 |                  0 |                      0 |                      0 | PASS      |

## Performance: Common Commands

| category       | workflow   | method   |   runs |   failures |   mean_seconds |   sd_seconds |   min_seconds |   p50_seconds |   max_seconds |
|:---------------|:-----------|:---------|-------:|-----------:|---------------:|-------------:|--------------:|--------------:|--------------:|
| common_command | append     | pq       |      3 |          0 |          0.190 |        0.026 |         0.168 |         0.182 |         0.219 |
| common_command | append     | python   |      3 |          0 |          0.063 |        0.005 |         0.059 |         0.063 |         0.069 |
| common_command | append     | slab     |      3 |          0 |          0.054 |        0.019 |         0.042 |         0.044 |         0.075 |
| common_command | describe   | pq       |      3 |          0 |          0.089 |        0.014 |         0.079 |         0.083 |         0.105 |
| common_command | describe   | python   |      3 |          0 |          0.001 |        0.001 |         0.000 |         0.001 |         0.003 |
| common_command | describe   | slab     |      3 |          0 |          0.032 |        0.027 |         0.015 |         0.017 |         0.063 |
| common_command | merge      | pq       |      3 |          0 |          1.510 |        0.057 |         1.471 |         1.484 |         1.575 |
| common_command | merge      | python   |      3 |          0 |          0.203 |        0.025 |         0.174 |         0.216 |         0.219 |
| common_command | merge      | slab     |      3 |          0 |          0.315 |        0.021 |         0.292 |         0.319 |         0.334 |
| common_command | path       | pq       |      3 |          0 |          0.000 |        0.000 |         0.000 |         0.000 |         0.000 |
| common_command | path       | python   |      3 |          0 |          0.000 |        0.000 |         0.000 |         0.000 |         0.000 |
| common_command | path       | slab     |      3 |          0 |          0.000 |        0.001 |         0.000 |         0.000 |         0.001 |
| common_command | save       | pq       |      3 |          0 |          1.094 |        0.066 |         1.056 |         1.056 |         1.171 |
| common_command | save       | python   |      3 |          0 |          0.587 |        0.098 |         0.522 |         0.540 |         0.700 |
| common_command | save       | slab     |      3 |          0 |          1.257 |        0.135 |         1.126 |         1.249 |         1.395 |
| common_command | use        | pq       |      3 |          0 |          0.763 |        0.063 |         0.722 |         0.731 |         0.836 |
| common_command | use        | python   |      3 |          0 |          0.046 |        0.020 |         0.032 |         0.037 |         0.069 |
| common_command | use        | slab     |      3 |          0 |          0.300 |        0.041 |         0.253 |         0.322 |         0.325 |

## Precision: Additional Workflows

| category       | workflow            | method   |   checks |   passed |   max_abs_diff |   numeric_null_mismatches |   other_mismatches |   missing_in_canonical |   missing_in_candidate | verdict   |
|:---------------|:--------------------|:---------|---------:|---------:|---------------:|--------------------------:|-------------------:|-----------------------:|-----------------------:|:----------|
| extra_workflow | workflow_collapse   | pq       |        3 |        3 |       1.14e-13 |                         0 |                  0 |                      0 |                      0 | PASS      |
| extra_workflow | workflow_collapse   | python   |        3 |        3 |       1.14e-13 |                         0 |                  0 |                      0 |                      0 | PASS      |
| extra_workflow | workflow_collapse   | slab     |        3 |        3 |       1.14e-13 |                         0 |                  0 |                      0 |                      0 | PASS      |
| extra_workflow | workflow_filter_gen | pq       |        3 |        3 |       8.88e-16 |                         0 |                  0 |                      0 |                      0 | PASS      |
| extra_workflow | workflow_filter_gen | python   |        3 |        3 |       0        |                         0 |                  0 |                      0 |                      0 | PASS      |
| extra_workflow | workflow_filter_gen | slab     |        3 |        3 |       8.88e-16 |                         0 |                  0 |                      0 |                      0 | PASS      |

## Performance: Additional Workflows

| category       | workflow            | method   |   runs |   failures |   mean_seconds |   sd_seconds |   min_seconds |   p50_seconds |   max_seconds |
|:---------------|:--------------------|:---------|-------:|-----------:|---------------:|-------------:|--------------:|--------------:|--------------:|
| extra_workflow | workflow_collapse   | pq       |      3 |          0 |          1.574 |        0.095 |         1.466 |         1.617 |         1.640 |
| extra_workflow | workflow_collapse   | python   |      3 |          0 |          0.097 |        0.007 |         0.091 |         0.096 |         0.105 |
| extra_workflow | workflow_collapse   | slab     |      3 |          0 |          0.238 |        0.013 |         0.224 |         0.239 |         0.250 |
| extra_workflow | workflow_filter_gen | pq       |      3 |          0 |          1.688 |        0.114 |         1.593 |         1.656 |         1.814 |
| extra_workflow | workflow_filter_gen | python   |      3 |          0 |          0.218 |        0.023 |         0.204 |         0.206 |         0.244 |
| extra_workflow | workflow_filter_gen | slab     |      3 |          0 |          0.237 |        0.015 |         0.220 |         0.245 |         0.246 |

## Files

- Raw timings: `benchmark_raw.csv`
- Timing summary: `benchmark_summary.csv`
- Validation detail: `validation.csv`
- Validation summary: `validation_summary.csv`
- Stata log: `stata_slab_pq.log`
- Environment: `environment.json`
