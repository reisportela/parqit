# parqit benchmarks

The examples and integration tests intentionally use small artificial datasets.
They are correctness fixtures: quick to generate, easy to inspect, and suitable
for exact Stata-vs-oracle assertions.

Performance work needs a separate data family. Use
`make_synthetic_data.py` to create deterministic, medium-sized Parquet analogues
of the tour datasets:

```bash
python3 benchmarks/make_synthetic_data.py
```

Default output goes to `benchmarks/_out/synthetic_medium_data/`:

- `workers_perf.parquet`: worker-year panel, default 2,000,000 workers x 5 years.
- `firms_perf.parquet`: one row per firm, default 500,000 firms.
- `patents_perf.parquet`: several patents per firm for many-to-many workflows.
- `wide_income_perf.parquet`: wide income file, default 1,500,000 persons x 8 years.
- `messy_perf.parquet`: hostile-schema analogue for boundary read-path timing,
  default 750,000 rows.

The defaults are deliberately medium scale. They should be large enough for
repeated timings to separate real changes from scheduler/cache noise, while
remaining small enough for local iteration and artifact cleanup. Increase the
sizes with the script's CLI flags only after correctness tests pass.

To time representative feature workflows against the current development
plugin:

```bash
stata-mp -b do benchmarks/benchmark_synthetic_features.do
```

The benchmark writes raw and summary CSV files under
`benchmarks/_out/synthetic_feature_benchmark/`. It covers full `collect`,
filtered/generated `save`, grouped `collapse`, full-table `sort`, disk-side
`merge`, `joinby`, and `reshape long`. Treat the numbers as comparable only when
generated from the same plugin, same data manifest, same repetitions, and
similar host load.
