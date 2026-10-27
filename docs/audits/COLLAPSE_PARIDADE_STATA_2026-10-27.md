# collapse: distinct counts and comparison with native Stata

Date: 27 October 2026. Baseline: public parqit 0.6.1, DuckDB 2.0.0;
reference: local StataNow MP 19.5 and the official Stata collapse manual.
All fixtures are synthetic. The new feature is an unreleased local change.

## Finding and implementation

Neither baseline `parqit collapse` nor native `collapse` accepted `(unique)`
or `(nunique)` (both returned 198). The distinct-count statistic is an
extension: gtools calls its version `nunique`.

The candidate adds `(unique)` and its alias `(nunique)` to the shared
collapse/pivot compiler and menu. For example:

```stata
parqit collapse (unique) firms=firmid, by(year)
```

Each source has its own exact count, including one missing-value category,
as in the existing `parqit unique` command. An all-missing group returns 1;
an empty sample has no output rows. Numeric missings share one category,
and SQL NULL and the empty string share the missing-string category. A
space is a string value. Extended Stata missings have already been folded
by the documented boundary policy. To exclude missing identifiers, add
`if !missing(firmid)`; groups with no remaining observations disappear.
Several sources count individually, not as a joint tuple; an `egen group()`
identifier can represent a tuple when that is required.

The exact SQL aggregate retains the key's type. It does not narrow integers
to double or estimate cardinality. Counts, labels and formats follow the
existing count policy, with a numeric format for string sources. The new
statistic does not enter the compact scaled sum/mean/count optimisation.
The regression includes adjacent integer keys 2^53 and 2^53+1, null/empty
strings, long UTF-8 strings, signed zero and nonfinite inputs.

## What remains different from native collapse

The 17 named native statistics, plus every integer percentile p1 through
p99, are supported with their full names. No named native statistic was
missing. The following syntax differences were reproduced with both
commands on the same synthetic source:

| Capability | Native result | parqit result | Practical implication |
|---|---:|---:|---|
| Four weight families (mean) | accepted | 198 | Main remaining analytical gap; there is no general weighted workaround. `rawsum` equals `sum` without weights. |
| `cw` | accepted | 198 | Use an explicit complete-case `if !missing(x,y)` over **all** sources. |
| `in 2/5` | accepted | 198 | Use `parqit keep in 2/5` before aggregation, with the intended order declared. |
| Time-series source `m=L.x`, after tsset | accepted | 198 | Generate the lag first, then aggregate the generated variable. |
| Source wildcard `x*` or range `x-y` | accepted | 198 | Enumerate source names. Patterns in `by()` are a separate supported facility. |
| `sem`, `seb`, `sep` | accepted | 198 | Use `semean`, `sebinomial`, `sepoisson`. |
| `fast` | accepted | 198 | Native recovery behaviour on Break concerns the dataset in memory; parqit changes a lazy plan. |

Weights and `cw` are the highest-value future additions for parity.
Supporting the source varlist grammar, direct `in`, time-series source
operators and the three abbreviations would improve convenience. This
change implements the requested distinct counts; it does not silently add
or reinterpret the other options.

Existing differences to retain when interpreting results:

- Native collapse replaces the dataset immediately; parqit modifies a
  lazy view and executes it on save/collect. A failed candidate operation
  must leave the original plan intact.
- Native first/last use the current observation order. Declare `parqit sort`
  for the intended order; without it, parqit uses a deterministic total
  order over all columns. Empty native samples return 2000; the lazy
  collapse produces zero rows.
- parqit retains double precision for aggregate results where native
  sometimes stores float, and its exact-value arithmetic can differ in the
  last bits. The comparison below reports those differences explicitly.
- String first/last/firstnm/lastnm are supported by native Stata too, despite
  the manual's general numeric-source wording. String count and fractional
  percentiles such as p12.5 are existing parqit extensions (native returned
  109 and 198 respectively).

The menu previously omitted rawsum, percent, semean, sebinomial, sepoisson
and iqr, although the CLI supported them. They now appear alongside
unique/nunique; arbitrary percentile names remain available through the
additional-specs input. Both helps and the README describe the new feature
and the remaining native syntax gaps.

## Evidence and limits

Completed validation: CTest 6/6, with 317 unit cases and 720,111 assertions
passing (the existing opt-in Arrow microbenchmark remains skipped); the
targeted distinct-count case has 41 assertions. Native GUI Submit, dialog
and release lint, and native rendering of both helps passed.

The full Stata sweep recorded 259 PASS verdicts and one environment abort:
V124 stopped at `import duckdb` because Stata's default `/usr/bin/python3.12`
could not find that module. The unchanged V124 passed when rerun with the
already-installed `/home/mangelo/miniconda3/bin/python3`, selected for that
batch process only. Combined coverage is 260 PASS verdicts with no unresolved
case; the initial full runner's exit 1 is retained as evidence. No packages
or permanent Python settings were changed.
The rerun exited 0 after its PASS verdict but the existing Python DuckDB
binding emitted nanobind shutdown warnings (28 leaked instances, one
keep-alive record and six types). This oracle-environment warning remains
unresolved; it did not interrupt the numerical assertions.

Evidence is under `audit_repro/collapse_unique_20261027/`:

- `baseline.do` / `baseline.log`: native versus released-plugin acceptance
  matrix, including all 99 integer percentiles; terminal PASS confirms that
  the probe finished, not that unsupported features are implemented.
- `unit_red_confirmed.log`: the new unit test fails on the previous compiler
  because the distinct statistic is absent. `unit_green.log`: 41 assertions
  pass after implementation.
- `native_parity.do` / `native_parity.log`: all native statistics on a double
  fixture, with `if`, missing groups and explicit first/last ordering. The
  discrete statistics and all 99 percentiles compare exactly with native
  values; first/last string results also compare exactly. Six group/statistic
  cells in sd, semean and sepoisson differ by one binary64 ulp. The separate
  Decimal-100 oracle bounds mean/sd/semean/sepoisson error to two ulps of
  the exact real result; observed maximum error is one ulp. This is the
  existing documented precision policy, not bitwise native identity.
- `tests/verify_suite/v195_collapse_unique.do`: independent native
  tag-and-sum counts, Python sets and PyArrow reads of saved results, both
  aliases, filters, pivot, metadata, explicit view targeting, empty samples
  and failure atomicity. Terminal PASS is required.
- `ctest.log`, `stata_full.log`, `gui_smoke_host2.log`, `render_help.log`:
  complete local regression results, actual GUI Submit and native help
  rendering are recorded separately. Final totals and artifact hashes are
  recorded in `receipt.json`.
- `v124_with_python.do` / `v124_with_python.log`: the process-local Python
  selection and successful rerun of the full sweep's sole interrupted case.

The work preserves the pre-existing changes to `.gitignore` and
`docs/audits/README.md`. Original copies and SHA-256 hashes of the nine
edited existing files are retained in `originals/` and `originals.json`.
The two additional product files are this report and V195. No version bump,
publication or global ado installation is part of this change. This local
validation does not certify new platform builds or arbitrary-scale exact
distinct aggregation performance.

Primary references:

- [Stata collapse manual](https://www.stata.com/manuals/dcollapse.pdf)
- [gtools gcollapse documentation](https://gtools.readthedocs.io/en/latest/usage/gcollapse/)
