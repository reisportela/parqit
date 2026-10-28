# Native DuckDB settings: 0.8.0 implementation and validation

Scope authorised on 28 October 2026: implement a generic native setting
interface, test and review it, update both helps, bump, commit, push and
release. Work targets DuckDB 2.0.0 only. The optional 1.5.6 engine path and
global Stata installation are outside this work.

## Public interface

```
parqit set duckdb
parqit set duckdb name
parqit set duckdb name value
parqit set duckdb reset name
```

The embedded engine supplies registered names, aliases, input types, scopes
and value validation. Listing prints metadata, rather than dumping all
configuration values. Values are single literals; quoted empty strings and
SQL NULL remain distinct. Setting names are checked as identifiers and
values use DuckDB's native typed setting executor. Engine errors are surfaced without silently replacing
the database or clearing views. `temp_directory` also aliases the existing
`tempdir` control. The native interface is independent of `fill_threads` and
other transfer controls.

The eight pre-existing parqit controls retain their defaults and validation.
The native route honours DuckDB's semantics, including its native thread
range; it does not silently apply the legacy CPU clamp. Native RESET means
the engine default, which can differ from parqit's automatic policy.

## State and integration

- One live engine remains attached to the Stata session. Configuration
  changes do not recreate it. Read-only, startup and state restrictions
  remain DuckDB restrictions.
- Default mutation scopes come from DuckDB's SET resolver; the catalog's
  effective-value origin is not mistaken for the mutation scope.
- Original input literals or RESET actions are retained, rather than
  replaying rounded display values. Connection-local overrides are applied
  when auxiliary query connections are created. Catalog metadata is cached
  for recovery, with refresh on listing or discovery of an unknown name.
- An explicit native asynchronous-thread choice survives a legacy CPU
  change. Explicit native streaming budgets suppress automatic per-fetch
  sizing; a legacy `stream_buffer_mb auto|0|n` hands control back.
- User-disabled optimizers compose with the package's required plan guards.
  A direct native choice marks the guard state dirty so it cannot bypass a
  guard whose booleans were already cached.
- Replay state is allocated before native mutation. If SET succeeds but
  subsequent inspection fails, the message says it was accepted and the
  override remains recorded. RESET can use cached metadata to recover from
  a setting that restricts SQL introspection. Native get/SET/RESET callbacks
  avoid SQL parsing, allowing an expression-depth setting to be reset even
  while ordinary SQL parsing is restricted.
- The native Arrow profile is inspected directly through the C API. Regular
  utf8 keeps its existing transfer loop; large-utf8 and string-view results
  select checked alternative walkers from the result's own schema. The
  strL sidecar, numeric conversion and metadata contracts are retained.

GLOBAL means the DuckDB instance. LOCAL in the catalog means a connection;
it is not an operating-system or permanent Stata setting. Advanced options
can change intended semantics: `preserve_insertion_order=false` allows row
reordering without a final ORDER BY and can alter positional results. It is
never enabled automatically. A selected configuration need not be faster.

## Native capability assessment

The published 0.7.0 CI Linux plugin registered 64 of the maintainer's 65
requested setting names, including aliases. `pandas_analyze_sample` was not
registered. Twenty-nine controlled same-instance attempts gave the expected
results, including read-only and irreversible-allocation refusals. A bounded
synthetic run observed 14,352,384 spill-file bytes and reproduced the native
refusal to change an already used temporary directory; the original table
remained readable.

The pinned engine accepted but ignored COPY's deprecated `CHUNK_SIZE`.
With 10,000 synthetic rows, `ROW_GROUP_SIZE 2048` produced five groups,
whereas default COPY and `CHUNK_SIZE 2048` produced one. Independent PyArrow
metadata inspection and full table equality confirmed identical schema,
values and order. Use the existing parqit `chunk()` control, which emits
`ROW_GROUP_SIZE`; COPY/read_parquet/ATTACH parameters are not SET names.

Assessment evidence is in
`audit_repro/duckdb_settings_assessment_20261028/assessment_receipt.json`.

## Arrow dependency check

No Arrow C++ or PyArrow library is linked into the plugin. The vendored
`vendor/arrow/abi.h` was byte-identical to Apache Arrow's current upstream
header when checked on 28 October 2026. `ArrowSchema`, `ArrowArray` and
`ArrowArrayStream` therefore match the current definitions; an Arrow package
version is distinct from this stable ABI and the output-format settings.
The receipt and fetched reference text are in
`audit_repro/release_0_8_0_20261028/arrow_abi_receipt.json`.

## Validation status

The final local C++ suite passed 329 cases and 720,327 assertions. All six
CTest targets passed across the full run and a focused plugin-runtime rerun:
the first runtime check used a stale probe identity, so the probe was rebuilt
before its passing rerun. One optional Arrow microbenchmark was skipped.

V198 passed live native changes, aliases, empty/NULL values, rejected values,
startup/read-only restrictions, policy precedence, optimizer choices and
expression-depth recovery without clearing the unrelated dataset or view.
V199 passed all sixteen direct/transformed transfers across four Arrow
profiles and serial/parallel fill against an independent Stata dataset:
numeric types, missing values, dates, clocks, short/long/Unicode strings,
labels, formats, characteristics and notes. C++ tests also asserted actual
`u`, `U` and `vu` output schemas and exact byte-budget replay.

Private `net install`/`net get` passed package identities and an end-to-end
native settings / weighted collapse / Arrow transfer probe. Both final helps
rendered through Stata's SMCL translator. Actual GUI Submit for native
set/get/reset passed on the privately installed package, preserving the
selected view and unrelated dataset; the compact window was inspected.

The first full staged Stata run produced 259 PASS and five failures under
the deliberately limited two-CPU allocation. Each failure was reproduced
on unchanged 0.7.0: fixed expectations of 3, 4 or 9 workers contradicted the
existing CPU clamp and AUTO reader policy. The five tests now derive these
expectations from the allocation; all five reruns passed. Their payload,
metadata, native/reference, refusal and fault-injection assertions remain
intact. The existing C++ route-boundary tests retain their high-thread
planning cases. A fresh full Stata run on the exact tag-CI Linux package
remains a publication gate rather than being inferred from these reruns.

## Performance assessment

The server was busy with unrelated work, as the maintainer explicitly noted.
No timing result here certifies a universal speed-up or absence of every
performance regression. Subsequent functional/GUI tests used fresh child
processes with a two-CPU affinity; existing processes were not modified.

The matched local 0.7.0/0.8.0 comparison used the same compiler and pinned
DuckDB 2.0 archive, four query threads and a 1 GB memory limit. Eleven
scenarios cover grouped, filtered, derived and global aggregates at 600,000
and 6,000,000 rows, a full numeric collection, sorted slicing and a 200,003-row
text collection. Three interleaved process pairs provide six measured
observations per version/scenario after warm-up. Emitted SQL, column types
and numeric payload bits/text values matched exactly; ordered collections
were compared in their actual order.

The small text case had a slower median (0.0530 to 0.0725 seconds), with broad
overlap and an after-version minimum faster than before. A bounded follow-up
used 1,000,003 text rows, four interleaved pairs and three measured repetitions
per process. Median collection seconds were:

| Fill workers | 0.7.0 | 0.8.0 | Observations per version |
|---|---:|---:|---:|
| auto | 0.1915 | 0.1795 | 12 |
| 1 | 0.2370 | 0.2040 | 12 |
| 4 | 0.2005 | 0.1790 | 12 |

Schema, complete values and row order matched independently in every run.
The small-case slowdown did not reproduce at the larger size or controlled
fill counts. Some small global-aggregate medians also moved upwards by a few
milliseconds within overlapping ranges. The evidence shows no consistent
regression in this finite campaign; it is diagnostic under shared load.
Further performance loops were stopped rather than treating noise as a
reason to change precision, algorithms or defaults. Individual samples,
resource/load context and limits are retained in the performance receipts.
One-minute load averages were about 36.6–39.9 during the first comparison.
Whole-process maximum RSS ranges were 464,284–542,572 KiB before and
489,820–496,624 KiB after; these finite shared-host observations are not a
universal memory bound.

## Custody and publication

The base commit is `13ad99a26ae070f41214484784956ad3fa91d771` (0.7.0).
Pre-change copies/hashes and the matched local 0.7.0 binary/ado are retained
under `audit_repro/release_0_8_0_20261028/`. Pre-existing `.gitignore` and
`docs/audits/README.md` changes and unrelated untracked files are preserved.
Only the explicit product/documentation/test allowlist may be committed.
Local diagnostics are not substituted for CI-built release assets.
