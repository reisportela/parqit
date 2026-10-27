# gcollapse analytical and data-option parity

Date: 27 October 2026. Scope authorised by the maintainer: analytical and
data-option parity, rather than compatibility with gtools-specific execution
switches. This is an unreleased local change on top of the unique/nunique
work. DuckDB remains 2.0.0; transformations remain lazy view operations.

## Implemented surface

| Area | parqit collapse |
|---|---|
| Existing native statistics | mean, sum/rawsum, sd, standard errors, median, arbitrary percentiles, iqr, count, percent, extrema and first/last variants |
| Additional summaries | geomean, nmissing, nansum/rawnansum, variance, cv, skewness, kurtosis and range |
| Distinct counts | nunique and the parqit alias unique, numeric or string |
| Selections | select# and select-#; rawselect# and rawselect-#; positive one-based ranks, missing when unavailable |
| Gini | default negative-to-zero, dropneg and keepneg variants |
| Weights | aweight, fweight, iweight and pweight; expressions over the view; family/statistic restrictions checked |
| Sample | if, in including f/l and negative bounds, cw; common exclusion of zero/missing weights |
| Per-target weighting | rawstat(), including target patterns/ranges |
| Group information | freq() counts admitted rows, including rows with missing statistic sources |
| Merge | merge, replace and noinit, including partial samples and original row/column order |
| Naming/group order | source patterns/ranges/abbreviations; wildparse captures (*, ? and digit #); signed by keys; ds/nods; unsorted |
| Storage | double and deferred, proved integer packing for sumcheck |
| Labels | labelformat tokens, source-label substrings and rclass labelprogram callbacks |
| Native menu | statistic list plus a compact Collapse options tab with weights, sample, merge, naming, storage and labels |

Unweighted pivot shares the additional statistics. Its separate syntax does
not gain collapse's weight/data options.

The native weight restrictions are retained, including when rawstat is
specified: pweights are refused for sd/variance/cv and all three standard
errors; iweights for standard errors; aweights for binomial and Poisson
standard errors. Frequency weights must be nonnegative integers; other
weights must be nonnegative. Source missingness is treated separately for
each target unless cw is requested.

Analytic counts and missing counts count rows; other weighted counts use
weight sums. Analytic sums are n times the weighted mean; other weighted
sums are sum(wx). Percent uses the share of nonmissing weight for every
family. Raw targets retain the common admission rules but ignore the weight
in their statistic. Weighted select uses absolute cumulative weight; rawselect
uses the unweighted rank after admission.

With merge, new targets are missing outside the admitted sample; existing
targets require replace. noinit preserves existing outside values. A shared
materialised CTE prevents a volatile prefix from being independently
re-evaluated for original rows and the grouped branch. Alias identities,
case collisions and output-name conflicts are checked before committing.
Expressions bind to the original view schema, not to temporary columns.
Label callbacks run in an empty temporary frame so they cannot accidentally
clear or change the unrelated dataset in memory.

## Numerical and storage decisions

Ordinary unweighted calls delegate to the previous collapse compiler,
retaining the compact exact sum/mean/count path. New weighted states use
fixed-size dyadic accumulators: products are accumulated before rounding,
integer/decimal coefficients retain their original bits, and central moments
are formed exactly. Frequency-weight binomial and Poisson errors avoid a
rounded mean losing a small tail or a rounded total crossing a half tie.

Weighted selections compare cumulative mass against the integer rank exactly.
Percentiles use the gtools convention of normalising to the nonmissing row
count and a 1e-6 positional tolerance. Scaled mass representations keep
percentages and cumulative proportions finite when intermediate totals exceed
the Stata/double range, and avoid premature underflow of a tiny percentage.

Gini uses the sorted pairwise definition with bounded exact states. A state
tracks prefix weight, prefix weighted value and the ordered half of the
pairwise weighted absolute differences. Sorted blocks combine only when
their value ranges do not overlap; an invariant failure is an error. The
final ratio is formed from exact integers, including signed/decimal inputs.
Sorting uses the window operator rather than collecting a whole group into
an in-memory list.

Exact frequency counts use DECIMAL(38,0) for Parquet compatibility: DuckDB
COPY converts a bare HUGEINT to double. The usual integer precision policy
applies at collect, including refusal or explicit int64(string)/int64(round)
for nonrepresentable integers. double storage intent does not silently cast
these counts before the precision check. Counts beyond the exact SQL integer
range fail loudly. sumcheck proves range/integrality from the materialised
result; it never narrows speculatively. Direct save retains the SQL type and
records packing intent for a subsequent lazy collect.

## Deliberate differences

- parqit keeps its existing double-precision policy, source-name default
  variable labels, boundary missing-category folding and empty-view policy.
  An explicit labelformat can use source labels. Numeric extended missings
  are already one missing category in the view; string NULL and empty are
  the same missing string. Both still count as one level under nunique.
- Collapse changes a lazy plan. It does not scan it merely to populate
  gcollapse's eager r(N), r(J), minJ and maxJ. freq() and explicit view reports
  provide the corresponding data. Native fast is not a recovery mode for
  a lazy transformation.
- The default by-range interpretation remains Stata's; nods explicitly
  selects concatenated +/- directions. Plain +/- prefixes are accepted.
- Gtools hash selection, collision fallback, I/O-versus-memory allocation,
  recast, diagnostic switches and strL plugin workarounds are outside the
  authorised parity scope. DuckDB execution settings use parqit set; general
  lossless storage compression remains available at collect, compress.
- Time-series source expressions should be generated before collapse.

The installed gtools reference is gcollapse 1.3.1 / internal ado 1.10.1.
Its unweighted rawselect showed a one-position discrepancy. With fractional
weights, nine selection cells in the differential fixture disagreed with
exact cumulative-mass selection. These cases are checked against explicit
ordering and Decimal/Fraction arithmetic, not reproduced as numerical bugs.
The installed gtools also returned missing for a one-row frequency/importance
weighted sd where native Stata returned zero. Native/independent arithmetic
is the reference in that case. No claim of bitwise identity with gtools is
made: floating rounding, storage and documented boundary policies differ.

## Validation and custody

Evidence is in `audit_repro/gcollapse_parity_20261027/`. `before.json` and
`originals/` preserve the previous files, including unique/nunique and the
unrelated dirty-worktree boundaries. The existing V33 weight-refusal check
is updated to require the now-supported native weighted mean.

The differential fixture compares five weight settings and 2,754 cells.
Ordinary numerical comparisons use the recorded tolerance; selections use
exact independent cumulative arithmetic, with the nine reference differences
reported separately. V196 covers data options, labels, storage, merge and
saved-payload oracles. V197 adds Decimal-120/Fraction checks, wide integer
and decimal moments/Gini, subnormal values, cancellation, tiny binomial tails,
large exact counts and extreme weight shares. C++ unit cases exercise these
compilers and arithmetic states. A real GUI Submit checks the chosen view,
weights/range/options, result values and preservation of the previous current
view and dataset. Old-plugin refusal is tested separately.

Final regression totals, artefact identities, GUI/help checks, performance
evidence and any execution limits are recorded in `receipt.json`.
This change does not publish a release or install into the global ado tree.

### Completed checks and practical limits

- Full functional sweep: 261 PASS verdicts. V124 stopped because Stata's
  default Python could not import duckdb; its unchanged numerical checks
  passed in the existing Miniconda environment, selected only for that batch
  process. Combined coverage is 262 PASS verdicts. The original runner's
  exit 1 and its interrupted log are retained.
- The final build, after the narrow decimal-domain correction and the
  ordinary-request fast dispatch, passes CTest 6/6 (322 unit cases and
  720,167 assertions; the existing optional Arrow microbenchmark is skipped).
  The six affected Stata collapse files, V33, V124 and the five-setting
  differential comparison pass on this final build. These final checks
  supplement the full sweep; they are not described as a new all-suite run.
- The decimal-domain negative control fails before the correction and
  passes afterwards: a DECIMAL(38,38) just below one is not a binary input,
  even if converting it to double would yield one. Label callbacks are
  explicitly tested with a callback that clears its empty frame.
- Gini at two million synthetic rows/100 groups, one thread and a 64 MB
  memory limit passes an exact independent integer/Fraction oracle for all
  100 results. Peak observed spill is 39,845,888 bytes. This is a bounded
  spill test, not an arbitrary-scale or process-RAM ceiling guarantee.
- Native GUI Submit, source-view/current-view restoration, help rendering,
  dialog/release lint and old-plugin refusal pass.

The original CI-release timing comparison was confounded by build/compiler
differences. A diagnostic baseline was therefore reconstructed from the
pre-task source units with the same GCC 12 compiler, unchanged dependencies
and an ABI size/offset check. Its identity is explicitly diagnostic, not a
release asset. The final ABBA comparison uses 600,000 synthetic rows, four
threads and eight measured samples per version/scenario after warm-up.
Compiled SQL and saved payload bytes match exactly in every scenario.

| Scenario | Before collect median | After collect median | Before/after planning median |
|---|---:|---:|---:|
| Grouped mean/sum/count | 0.081 s | 0.082 s | 0.013 / 0.0155 s |
| Filtered grouped aggregate | 0.0455 s | 0.051 s | 0.0125 / 0.0155 s |
| Derived-column aggregate | 0.091 s | 0.080 s | 0.0135 / 0.015 s |
| Global aggregate | 0.0285 s | 0.018 s | 0.0135 / 0.0115 s |

The execution distributions overlap and earlier runs changed the direction
of several differences. These short timings do not establish a persistent
regression or a general speed-up. Planning overhead is small in absolute
terms but remains visible in some medians; no universal performance
non-regression certification is claimed. Raw samples, ranges and build
identities are preserved in `bench_matched_fast/receipt.json`.

The existing Python duckdb binding emits nanobind shutdown warnings
(28 instances, one keep-alive record and six types) after V124's PASS and
process exit 0. That external oracle-environment warning is retained and
has not been remediated. New platform CI and an arbitrary-scale performance
campaign have not been run; no publication or global installation occurred.

Primary references: [gcollapse documentation](https://gtools.readthedocs.io/en/latest/usage/gcollapse/),
[gtools source](https://github.com/mcaceresb/stata-gtools),
[native collapse manual](https://www.stata.com/manuals/dcollapse.pdf).
The consulted gtools source revision and original C files are retained under
`reference/`; downloaded reference code was not executed.
