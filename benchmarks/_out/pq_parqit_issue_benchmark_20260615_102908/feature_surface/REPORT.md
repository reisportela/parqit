# pq/slab full feature-surface timing supplement

- Repetitions: 3
- Data: `/home/mangelo/Documents/GitHub/slab/benchmarks/_out/pq_slab_issue_benchmark_20260615_102908/feature_surface/data`
- Unit: representative end-to-end workflow for each feature. For lazy slab verbs, time includes the materializer that executes the plan.
- Python is the canonical implementation: pyarrow for file I/O and DuckDB SQL for relational/statistical workflows.
- Administrative commands without a data-processing equivalent are excluded; `path`, `show`, `explain`, and `glimpse` are included because they have direct metadata/introspection analogues.

## Common pq/slab features: p50 seconds

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

## slab-only features: p50 seconds

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

## Files

- `feature_raw.csv`
- `feature_summary_long.csv`
- `feature_p50_wide.csv`
- `feature_surface_stata.log`
