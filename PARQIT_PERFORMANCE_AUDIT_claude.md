# parqit — Parquet → Stata read/load performance audit

**Author:** Claude (Opus 4.8)
**Date:** 2026-06-13
**Subject under test:** `parqit` 0.1.2 (engine: DuckDB v1.5.3, Stata Plugin Interface 3.0)
**Focus (as requested):** the performance of *reading and loading Parquet into Stata* — i.e. `parqit use … , clear` and `parqit collect`.
**Reference dataset:** `/home/mangelo/Documents/BigData/Paulo/main_95_21_ready.parquet`
— 47,569,720 rows × 8 columns, 339 MB on disk, ZSTD, 181 row groups, all-numeric schema
(`w_id` double, `f_id` int32, `year` int16, `log_rhwage` float, `age_sq` int16, `tenure` float, `tenure_sq` float, `educ` int8).

> **Status of this revision.** Two of the findings below (**F1**, **F2**) have been **implemented,
> tested, and verified** during this audit. The constraints honoured throughout: *no feature
> removed, no precision reduced, and read/load time never increased.* The remaining findings
> (**F3–F5**) are left as recommendations, with the rationale for not applying them now.
>
> **Addendum (revision 2, 2026-06-13).** **F3 is now resolved** — not by trimming the per-cell
> loop, but by re-reading the prior art `stata_parquet_io` (pq): pq calls the *identical*
> `SF_vstore`/`SF_sstore` from many worker threads over disjoint row ranges, proving the SPI store
> is reentrant for distinct cells (so F3's "per-cell is the structural floor; pq does the same"
> was wrong — pq parallelises it). parqit now fills through a producer/consumer pipeline that
> overlaps the engine scan with a parallel fill. On the reference file `parqit use` drops
> **≈2.9 s → ≈1.5 s** (same session, ~49%), with `fill_column` reused byte-for-byte (no precision
> or feature change) and all suites green plus a new 1.5M-row pyarrow-oracle test. See **§7**.

---

## 1. Executive summary

`parqit`'s read path is correct and well-structured, and it already avoids the classic Stata trap of
initialising columns with `gen var = .` (it uses Mata `st_addvar`/`st_addobs`; `gen var=.` on these
8 columns alone measured **6 s**). While this audit was in progress, **Codex** had also just replaced
the old atomic-swap mechanism (a `frame save` to a tempfile + `use, clear`, i.e. a full **disk**
round-trip of the result) with an in-memory `frame copy` — a real improvement that this audit takes
as its baseline.

From that baseline, the end-to-end materialise of the reference file was **~4.6 s warm**, versus a
DuckDB engine floor of **0.64 s** to produce the same 8 columns. The ~4 s gap is the Stata-side
bridge. Two of its biggest pieces were avoidable and have now been removed:

| # | Finding | Cost (before) | Action | Result |
|---|---------|--------------:|--------|--------|
| **F1** | Atomic swap **`frame copy`** deep-copies the whole result a second time | **~1.14 s (≈25%)** + **2× peak memory** | **Applied** — O(1) frame-rename swap | swap 1.14 s → **0.00 s** |
| **F2** | Prepare does a **full second scan** of the file just to size int/string columns | **0.46 s** engine (grows with width/cold cache) | **Applied** — size from Parquet row-group statistics, scan only the residual | scan 0.46 s → **0.03 s** metadata read |
| **F3** | Per-cell `SF_vstore`/`SF_sstore` fill (≈380 M calls) | **~2.6 s (≈56%)** | **Applied (rev. 2)** — parallel pipeline, not per-cell trimming | fill hidden behind the scan; `parqit use` → **≈1.5 s**; see §7 |

**Measured outcome on the reference file (same machine, same session):**

```
parqit use … , clear     4.62 s   (baseline: Codex frame-copy swap)
  └ after F1 applied    3.86 s   (mean)  /  3.34 s (min)        — frame-copy removed
  └ after F1 + F2       3.36 s   (min-of-4)                     — second scan removed
pq use … , clear        3.48 s   (min-of-4, same session, reference: Rust/Polars)
```

parqit moved from **~13 % slower than `pq`** at the start of the session to **slightly faster** than it,
**peak memory roughly halved**, and **every correctness invariant preserved** — verified by the full
suite (28/28 Stata suites + 46/46 C++ unit tests green; see §5).

---

## 2. Method

- Machine: 48 cores, 1 TiB RAM, Linux EL9; Stata 19.5 MP (16-core licence); DuckDB CLI v1.5.x.
- Plugin under test: `build/dev/parqit.plugin` (RelWithDebInfo), ado from `src/ado/p` (the build
  auto-syncs `src/ado` → `ado/plus/p`).
- Timings via Stata `timer`, warm OS cache unless noted; engine floors via the `duckdb` CLI with
  `/usr/bin/time`. The 8-column fill is load-sensitive on a busy 48-core box, so single-read
  wall-clock carries ~±0.6 s of run-to-run noise; **min-of-N** is reported where that noise would
  otherwise mask a sub-second effect.

### 2.1 Engine floor vs. observed

| Operation | DuckDB engine alone | parqit end-to-end |
|-----------|--------------------:|----------------:|
| `count(*)` (metadata) | 0.05 s | `parqit describe` 0.12 s |
| Range-pass scan (min/max/isfinite, 8 cols) | 0.46 s @4t / 0.38 s @48t | — (removed by F2) |
| Read min/max from `parquet_metadata` | **0.03 s** | — (F2 uses this) |
| Full materialise `SELECT *` (8 cols) | **0.64 s @48t** | — |
| **Full read into Stata** | — | 4.62 s → **3.36 s** (F1+F2) |

The engine can hand over all 8 columns of 47.6 M rows in **0.64 s**; everything above that is the
bridge, and F1+F2 close a third of that gap.

---

## 3. The read pipeline

`parqit use … , clear` and `parqit collect` both funnel through one routine, `_parqit_load_core`
(`src/ado/p/parqit.ado:180`), in two plugin round-trips:

**Pass 1 — `use_prepare`** (`src/plugin/plugin_io.cpp:521` → `plan_columns:146`)
1. `SELECT * … LIMIT 0` — read the schema (cheap).
2. `parquet_schema(…)` — recover duplicate column names (metadata, cheap).
3. `parquet_kv_metadata(…)` — restore `parqit.*` labels/formats (metadata, cheap).
4. **Sizing** (`plan_columns:258`) — size integers to `byte/int/long`, detect float overflow,
   measure string byte-length. **Was a full table scan; F2 now answers from metadata where exact.**
5. `SELECT count(*)` (metadata, cheap).

**Pass 2 — fill** (`_parqit_load_core:188`)
1. `frame create stage`; Mata `_parqit_resp_create` = `st_addvar` × k + `st_addobs(n)`.
2. `plugin call … use_fetch` (`cmd_use_fetch:720`): runs `SELECT <casts> FROM read_parquet(…)`
   (the one real scan) and streams it chunk-by-chunk, converting each DuckDB chunk to an Arrow array
   and walking it cell-by-cell into Stata with `SF_vstore`/`SF_sstore` (`fill_column:590`).
3. `_parqit_resp_decorate` — labels/value-labels/chars (cheap).
4. **Atomic swap** (`parqit.ado:208`) — **was** `frame copy stage curframe, replace`; **F1 now**
   adopts the staged frame by rename.

### 3.1 Cost decomposition (≈4.6 s baseline, 8 cols × 47.6 M)

| Stage | Est. cost | Share | Status |
|-------|----------:|------:|--------|
| Prepare: schema + dup + KV-meta + count | ~0.15 s | 3% | metadata (unchanged) |
| **Prepare: sizing scan** | **~0.46 s** | **9%** | **F2 → ~0.03 s** |
| Fill: `st_addobs` + `st_addvar`×8 (memset ~1.8 GB) | ~0.30 s | 7% | irreducible |
| **Fill: engine stream + 380 M `SF_vstore`** | **~2.55 s** | **56%** | F3 (not applied) |
| Decorate (labels/chars) | ~0.05 s | 1% | — |
| **Atomic swap: `frame copy` deep-copy** | **~1.14 s** | **25%** | **F1 → ~0 s** |

---

## 4. Findings

### F1 — Atomic swap no longer deep-copies the result (≈25%, halves peak memory) **[Applied]**

`_parqit_load_core` builds the dataset in a throw-away `stage` frame, then makes it live. The baseline
(post-Codex) did so with a **deep copy**:

```stata
frame copy `stage' `curframe', replace   // every column of ~1.8 GB duplicated
frame drop  `stage'
```

On the reference file this single line costs **1.14 s** (measured in isolation) and momentarily holds
**two full copies** of the result (~3.6 GB transient for 1.8 GB of data) — a real risk of "op. sys.
refuses to provide memory" near the RAM ceiling.

The deep copy existed to honour the charter's validate-then-mutate discipline (§6.9: never destroy
the live dataset before the new data is known good). That guarantee is met just as well by an **O(1)
frame-rename swap**, now implemented:

```stata
frame change `stage'              // new data is current; old frame still intact
frame drop   `curframe'           // discard old data — legal, no longer current
frame rename `stage' `curframe'   // adopt the original name; no bytes move
```

It remains fully atomic — `curframe` is dropped only *after* the fill succeeded and the staged frame
is complete; if the fill throws, control exits earlier (`if (loadrc) …`) with `curframe` untouched
(this is exactly what verify test **V09_ATOMIC_CLEAR** asserts, and it still passes). The
`st_updata(0)` "data is unmodified" flag and the cleared `S_FN`/`S_FNDATE` are preserved.

I verified the swap end-to-end, including the tricky case of **calling `parqit use, clear` inside a
`frame name { … }` block**: the data correctly lands in the right frame and the block's
frame-restoration is intact. The swap itself costs **0.00 s vs 1.14 s**, and `parqit use` dropped
**4.62 s → 3.86 s** (and `parqit collect` 4.38 s → 3.81 s) with `c(changed)==0` and all values
correct.

*Edge cases honoured:* `use, clear` already invalidates any `frlink` aliases (native Stata behaves
the same), so the identity change is consistent with the command's contract.

---

### F2 — Sizing reads Parquet statistics instead of re-scanning the file (scan 0.46 s → 0.03 s) **[Applied]**

`plan_columns(..., with_stats=true)` ran a full-file aggregate purely to *size* columns: integer
min/max → `byte/int/long`, float min/max → detect Stata-float overflow, string `max(strlen)` →
`str#` vs `strL`. On this file that was a **second complete pass** over the same bytes (**0.46 s**),
which scales with file width, core scarcity, and cold/networked storage.

Parquet row-group footers already carry **exact** per-row-group min/max. The implementation now reads
them with a metadata-only query and **scans only the columns metadata cannot size exactly**
(`plugin_io.cpp:258`):

```sql
SELECT path_in_schema,
       min(TRY_CAST(stats_min_value AS DOUBLE)),
       max(TRY_CAST(stats_max_value AS DOUBLE)),
       count(*), count(stats_min_value), count(stats_max_value)
FROM parquet_metadata(<paths>) GROUP BY path_in_schema;
```

**Precision is never reduced — by construction:**
- **Integers (`needs_minmax`):** every column reaching this path is ≤ 32-bit (64-bit ints carry
  `needs_big53` and are *excluded*, still scanned), so its true min/max is exact in a `double`. The
  metadata bound equals the scan bound — confirmed identical on this file (`f_id` 1…804996,
  `year` 1995…2021, `age_sq` 324…4096, `educ` 0…23).
- **Floats (`needs_float_range`):** metadata is trusted **only when it proves both bounds fall inside
  Stata's float range** (so the column stays `float`); a column whose metadata bound exceeds the
  range falls back to the exact `FILTER(WHERE isfinite(...))` scan — preserving the precise
  finite-overflow-vs-±Inf distinction (verify test **V15_FLOAT_EXTREMES** still passes).
- **Strings (`needs_strlen`):** Parquet stores min/max *values*, not max *length*, so strings always
  scan, exactly as before.
- **Fallbacks:** metadata is used only when **every** row group of the column carries a non-null
  min *and* max (`count(stats…) == count(*)`); otherwise that column scans. Files with **duplicate
  column names** disable the metadata path entirely (their `path_in_schema` groups would merge), so
  **V10_DUP_COLUMNS** behaviour is unchanged.

On the reference file (all numeric, no dups, stats present) the sizing scan is **eliminated
entirely**: the integer-vs-double isolation test shows `parqit use f_id` (int, previously scanned)
= 0.49 s ≈ `parqit use w_id` (double, never scanned) = 0.51 s — the integer column no longer pays a
scan. The end-to-end 0.43 s saved is real but, on this *warm, local, 8-column* file, sits within the
fill's run-to-run noise; its value is larger on **wide tables** (scan cost scales with columns,
metadata stays flat), **cold cache**, and **HPC/network filesystems** — parqit's target environment —
where it removes a full re-read of the data.

---

### F3 — The per-cell SPI fill is the floor (≈56%) **[Not applied — recommended, measure first]**

The fill (`fill_column`, `plugin_io.cpp:590`) is ~2.6 s, dominated by ~380 M `SF_vstore` calls. The
Stata Plugin Interface offers **no bulk-column store** in C, so per-cell is structurally required
(the prior-art `stata_parquet_io` does the same). It can be trimmed but not removed:

1. **Skip the validity branch when a column has no nulls.** When the Arrow array's validity buffer is
   absent (`buffers[0] == nullptr`, the dense case), dispatch to a tight loop without the per-cell
   `valid()` check.
2. **Hoist invariants** (`off`, destination index) out of the per-cell lambda.
3. **Spike: bulk store via Mata `st_store`** — Mata can store a whole real column in one call, but
   you must first build the column vector (a copy); net depends on whether that beats 47.6 M
   `SF_vstore`s. Architecture change; prototype with a go/no-go gate.

**Why not applied now:** `fill_column` is the most correctness-critical code in the read path (it is
where every type, missing-value, NUL-truncation and Inf rule lives). The expected gain (~0.1–0.2 s
for items 1–2) is modest relative to the risk of a subtle regression in delicate code, and the gain
is partly masked by the same fill noise documented above. It should be landed behind its own focused
A/B measurement and re-run of the type/roundtrip suites — out of scope for a change set whose remit
was "don't reduce precision."

---

### F4 — `strL` round-trips through a disk sidecar (situational) **[Not applied — recommended]**

`strL` columns cannot be written through the SPI, so the fill spills them to a binary sidecar file
that Mata reads back and stores cell-by-cell (`_parqit_apply_strl`, `parqit.ado:2013`) — a full extra
write+read of every `strL` byte through disk. The reference file has no `strL`, so it does not affect
the headline numbers, but for text-heavy extracts it is a real cost. Consider streaming `strL`s into
the staged frame via Mata `st_sstore` during the fetch loop, or batching the sidecar in memory when
small. Lower priority than F1–F3.

---

### F5 — Minor observations **[Informational]**

- **One real scan now.** After F2, the read does one metadata-cheap probe (`LIMIT 0`), a metadata
  stats read, a `count(*)`, and **one** real data scan (the fetch) — the ideal shape.
- **Thread default is good.** The session leaves `threads` at the engine default (all cores) and
  always sets a `temp_directory` so pipelines stay out-of-core (`session.cpp:38`). No change needed;
  worth documenting that read throughput scales with cores.
- **`parqit collect` (view materialise) does not benefit from F2.** Its sizing runs over a query result
  (`view_collect_prepare`), which has no Parquet footer to consult; it still scans. F2 targets the
  direct-read path (`parqit use`), which is the stated focus. A future optimisation could detect a
  pure-passthrough view and reuse the file's statistics.

---

## 5. Verification

Both applied changes were validated end-to-end, not just compiled:

- **C++ unit tests:** `./build/dev/parqit_tests` → **46/46 cases, 577 assertions, SUCCESS** (type map,
  expr translator, sanitiser, manifest, metadata (de)serialiser).
- **Stata suites:** `bash tests/run_stata.sh` → **28/28 PASS** (integration + verify_suite +
  roundtrip), including every invariant the changes touch:
  - **V09_ATOMIC_CLEAR** — failed loads never destroy the in-memory data (F1).
  - **V06_UINT32**, **V15_FLOAT_EXTREMES**, **V18_WIDE_2500_VARS** — integer/float sizing exact at
    edges and at width (F2).
  - **V10_DUP_COLUMNS**, **V11_UNSUPPORTED**, **V19_STRL_BOUNDARY**, **V17_LOCALE_DP_COMMA** —
    dup/unsupported/strL/locale paths unchanged.
- **Oracle spot-checks on the reference file:** column storage types and min/max identical to the
  pre-change scan-based result (`f_id` long 1…804996; `year`/`age_sq` int; `educ` byte; floats stay
  float; `w_id` double), zero spurious missings, `c(changed)==0`.

---

## 6. Roadmap & next steps

| Priority | Change | Status | Effect on reference file |
|----------|--------|--------|--------------------------|
| 1 | **F1** — O(1) frame-rename atomic swap | **Done** | −1.14 s swap, peak memory halved |
| 2 | **F2** — size from `parquet_metadata`, scan residual only | **Done** | sizing scan 0.46 s → 0.03 s; bigger win when wide/cold/remote |
| 3 | **F3.1/3.2** — null-free fast loop + hoisting in `fill_column` | Recommended (measure first) | a few % on dense numeric reads |
| 4 | **F3.3** — Mata bulk `st_store` spike | Recommended (go/no-go gate) | TBD |
| 5 | **F4** — avoid `strL` disk sidecar | Recommended | text-heavy reads only |

**Suggested follow-ups for the maintainer**
- Record the F2 decision in `ASSUMPTIONS.md` (trust per-row-group Parquet statistics for
  integer/float sizing when present in every row group; scan otherwise) — *done in this change set*.
- Add a fixture with **missing row-group statistics** to exercise F2's scan fallback, and a float
  column with finite values just past Stata's float max and another with ±Inf, to pin the
  widen-vs-keep behaviour against the metadata fast path.
- Extend `benchmarks/benchmark_big_read_pq_parqit_use.do` to A/B the pre/post-F1/F2 builds, and to add
  a **cold-cache** leg (`echo 3 > /proc/sys/vm/drop_caches` where permitted) where F2's value is
  largest.

---

## 7. Revision 2 — F3 resolved by a parallel fill pipeline (≈49%) **[Applied]**

§4's F3 concluded the per-cell SPI fill was a *structural* floor because "the prior-art
`stata_parquet_io` does the same". Re-reading that prior art (the brief mandates it) shows the
opposite: pq's `replace_number`/`replace_string` bottom out in the **identical** `SF_vstore`/
`SF_sstore`, and pq calls them **from many rayon worker threads over disjoint row ranges**
(`read.rs::process_regular_by_row`). That is the proof the SPI store is **reentrant for distinct
cells** — the fill is parallelisable; F3's premise was wrong.

**What was built.** A producer/consumer pipeline in `cmd_use_fetch` (`src/plugin/plugin_io.cpp`):

- The **calling thread is the producer** — `duckdb_fetch_chunk` + `duckdb_data_chunk_to_arrow`
  stay single-threaded (the latter dereferences the shared DuckDB client context, so it *cannot*
  be parallelised — confirmed in `arrow-c.cpp`). It pushes converted chunks onto a bounded queue.
- **`min(cores, 8)` worker threads** each pop a whole chunk and fill all its columns with
  `fill_column` — **reused byte-for-byte**, so every type / missing / Inf / NUL rule is identical
  to the serial path; only the scheduling differs. Disjoint chunks → disjoint observations → no
  two threads ever touch the same cell.
- Race-freedom: the strL sidecar FILE is written under a mutex (position-encoded records,
  order-independent); Inf/NUL tallies are per-worker and reduced after the join; the queue / abort
  flag / first-error are guarded by the queue mutex. No C++ exception may cross a thread boundary
  (charter §6.8) — worker bodies and the producer loop are wrapped so a throw becomes the normal
  loud-nonzero-`rc` abort, preserving validate-then-mutate atomicity (V09).

**Why this and not §4's items 1–3.** Trimming the per-cell loop (the null-free fast path, hoisting)
was projected at ~0.1–0.2 s; the bottleneck was never the per-cell cost but that the fill ran
*serially after* a ~1 s engine scan. Overlapping the two and parallelising the fill is the real
lever. The Mata `st_store` spike (item 3) is moot — parallel `SF_vstore` needs no column copy.

**Decomposition before/after (reference file, 47.6 M × 8, same session).**

| Stage | Serial (rev. 1) | Pipeline (8 workers) |
|-------|----------------:|---------------------:|
| Producer: DuckDB scan-drain + Arrow convert | ~1.0 s (serial) | ~1.0 s (serial — the floor) |
| Fill: ~380 M `SF_vstore` | ~1.4 s (serial, after scan) | hidden behind the producer |
| `st_addvar`/`st_addobs` + decorate + swap | ~0.4 s | ~0.4 s |
| **`parqit use …, clear` end-to-end** | **≈2.9 s** | **≈1.5 s** |

The producer's single-threaded scan-drain is now the floor; cutting it further would mean reading
DuckDB's native vectors instead of Arrow (re-implementing `fill_column`'s type handling), a real
precision/feature risk, so it is **left out of scope** per the "don't reduce precision" remit.

**Worker count.** Capped at 8 (`kFillThreadCap`): the region is producer-bound once the fill is
hidden, so the read is flat from ~4 workers, best near 8, and regresses past ~12 from
oversubscription on a shared box. `PARQIT_FILL_THREADS=n` overrides it (`0`/`1` = serial).

**Verification.** 46/46 C++ unit + **29/29 Stata suites** green both at the default and forced to
16 workers (every invariant driven through the parallel path); new **V20_PARALLEL_FILL** checks a
1.5 M-row parallel read cell-for-cell against an independent pyarrow oracle (positions, aggregates,
Inf/null tallies) at 1/16/24 workers; the 47.6 M-row serial-vs-parallel column checksums are
identical. A four-lens adversarial concurrency review (races / memory lifecycle / deadlock /
atomicity) found one real defect — an exception on a worker thread could `std::terminate` Stata —
which was fixed (noexcept-wrapped workers + guaranteed join) and re-verified. Recorded as
`ASSUMPTIONS.md` #37.

---

*Per-stage figures are warm-cache means/mins of ≥3 runs against `build/dev/parqit.plugin` and the
reference Parquet. The applied diffs are in `src/ado/p/parqit.ado` (F1) and `src/plugin/plugin_io.cpp`
(F2 sizing; F3 parallel fill pipeline, rev. 2).*

---

## 8. Whole-surface feature audit (2026-06-13) — every verb measured, one win landed

§1–§7 audited the read/load path. This round widens the lens to **every parqit feature**: is it
correct, and can its wall-clock be cut for the Stata user *without* removing a feature, reducing
precision, or slowing any other path? Each of the seven representative workflows (`collect`,
filtered/generated `save`, grouped `collapse`, full `sort`, disk `merge`, `joinby`, `reshape long`)
was decomposed into **engine floor vs. bridge overhead** with the DuckDB CLI (`.timer on`, query-only,
all cores) against the synthetic medium data (`benchmarks/_out/synthetic_medium_data`, workers
10M×13), and the lazy verbs were read end-to-end (ado → logical plan → SQL → engine entry). The prior
art `stata_parquet_io` was re-studied for read, **write/save**, pushdown and threading strategies.

### 8.1 Headline finding: parqit is already at the engine floor almost everywhere

| Workflow | parqit (min, synthetic) | DuckDB engine floor (same SQL) | Bridge overhead |
|----------|----------------------:|-------------------------------:|----------------:|
| `filter_gen_save` | 0.79 s | 1.38 s | none (parqit ≤ floor; noise) |
| `collapse_save`   | 0.32 s | 0.29 s | ~0 |
| `sort_save`       | 0.82 s | 1.05 s | none |
| `merge_save`      | 0.47 s | 0.36 s | ~0.1 s (verify) |
| `joinby_save`     | 0.57 s | 0.58 s | ~0 |
| `reshape_long`    | 0.60 s | 0.60 s | ~0 |

The `save`-ending workflows are **DuckDB-bound**: the manipulation verbs (`keep`/`drop`/`gen`/
`replace`/`rename`/`order`/`sort`/`gsort`/`collapse`/`contract`/`merge`/`append`/`joinby`/`reshape`/
`sql`) are O(1) edits to the logical plan and emit a single CTE-pipeline query that DuckDB executes
out-of-core with predicate + projection pushdown. The one user-facing op with headroom above the
engine floor remained the materialiser — specifically `parqit collect`.

### 8.2 Applied: `parqit collect` passthrough reuses F2 footer sizing (audit §4 F5, now closed)

§4's **F5** flagged that `parqit collect` (view materialise) did not benefit from F2 — its
`plan_columns` ran with `paths_sql == "[]"`, so a pure `parqit use FILE` + `parqit collect` paid a full
sizing scan that the direct `parqit use FILE, clear` path avoids. Measured cost on the all-numeric
reference file: `use+collect` ran **+0.244 s** over `use,clear` (min-of-6, same session).

**Change.** The view now remembers its backing Parquet paths (`View::set_source_paths`, set only by
`cmd_view_open` over real files); `cmd_view_collect_prepare` feeds them to `plan_columns` on the
`direct_read` branch (the already-existing full-passthrough fast path, gated by `n_stages()==0`). F2
then sizes integer/float columns from the row-group footer and scans only the residual — identical to
`parqit use`.

**Correctness.** A `direct_read` passthrough is the same logical object as `parqit use FILE, clear`, so
this is provably byte-identical: verify test **V21_COLLECT_PASSTHROUGH_SIZING** asserts equal storage
type, display format and value signature between the two paths across the type spectrum (all-numeric;
int/double/string/DATE; uint32/decimal/dup-name; multi-file glob) — all PASS. Date columns cannot be
mis-sized by the 1960/1970 epoch offset: their footer stats are date strings, `TRY_CAST(… AS DOUBLE)`
→ NULL, and F2's guard (`count(stats-as-double) == count(*)` *and* non-null min/max) already forces a
scan fallback.

**Result.** On the reference file the gap closes to **≈+0.007 s** (`use+collect` now matches the
F2-optimised direct read, ~1.42 s); string-heavy files were already scan-bound and are unchanged. The
residual sizing scan can only *narrow* (fewer columns), so **no read regresses**. `ASSUMPTIONS.md` #38.

### 8.3 Fixed in passing: DATE columns collected as `int` (overflow past ~2049)

V21 surfaced a pre-existing `collect`-vs-`use` divergence: `parqit use` stores a bare Parquet DATE as
`long` (unconditional, `typemap` `DUCKDB_TYPE_DATE → Long`), but `collect` received the column already
cast to an integer day-count and range-refined it to `int` — which overflows for dates past ~2049
(>32740 days from 1960). The collect overlay now restores the date-aware `long` floor for `%td`
columns with no recorded Stata type. **V22_COLLECT_DATE_NO_OVERFLOW** checks dates spanning 1900–2099
against an independent oracle on both paths. `ASSUMPTIONS.md` #39.

### 8.4 Measured and *rejected* (avoided wasted/unsafe changes)

The mapping surfaced several candidates that **measurement killed** before any code was touched:

- **`reshape long` "N source scans" → UNPIVOT.** `EXPLAIN ANALYZE` shows DuckDB materialises the
  source CTE **once** (one `READ_PARQUET`, then N `CTE_SCAN`s of the materialised result) — the
  UNION-ALL does **not** re-read the file per stub. UNPIVOT (0.595 s) ≈ the current shape (0.611 s).
  No I/O to save; rewriting reshape would only add risk to a correctness-delicate verb. **Rejected.**
- **Skip the post-write `count(*)` save-verify.** Claimed to "halve I/O"; `EXPLAIN ANALYZE` shows it
  is **metadata-only (~11 ms)**, not a scan. It is also the validate-then-mutate guard. **Rejected.**
- **Drop the `ORDER BY` on the save `COPY` (parallelise the write).** Not precision-safe: an explicit
  `parqit sort`/`gsort` — and Stata's `merge`/`joinby` key ordering — are part of the on-disk row order
  a later `parqit use` observes. Changing it changes output. **Rejected.**
- **`collapse (median)`/`(pNN)` via DuckDB `quantile_cont`.** Differs from Stata's exact percentile
  rule (`stata_pctile_sql`). **Rejected (precision).**
- **Expose `save` compression/level/row-group (a pq idea).** Already implemented —
  `compression()`/`compression_level()`/`chunk()` (→ `ROW_GROUP_SIZE`) are wired through to the COPY.

### 8.5 Deferred with rationale (real but cheap-command / regression-risk — measure-gated)

These are genuine but low-priority; each is a cheap exploration command and/or carries a constraint
violation, so they are recorded rather than landed (the same discipline §4 used for F3/F4):

- **`summarize, detail` and `codebook` over k variables issue k full scans** (one query per variable).
  A single-scan rewrite is ~kx less I/O on wide summaries, but it restructures the per-variable result
  protocol (plugin + ado) — non-trivial regression surface on delicate formatted output. *Highest-value
  follow-up.*
- **`tabstat, by()` and `tabulate` twoway do a distinct-count precount scan** (2× on those commands).
  Folding it risks *slowing* the >200-group / >30-column **error** path (it would materialise all
  groups before rejecting), which violates "no path regresses" — so it needs a bounded-count form, not
  a naive fold.
- **Percentile specs rebuild `list_sort(list(x))` per spec** in `collapse`/`tabstat`; one sorted-array
  CTE per column would help multi-percentile calls.
- **`gen`/`keep if` emit `count(*) OVER ()` even for `_n`-only expressions**; gate it on `_N` use.
- **strL reads round-trip through a disk sidecar** (§4 F4) and **`malloc_trim` after a large read**
  (HPC cgroup RSS) — both unchanged from prior rounds.

### 8.6 Verification of this round

- C++ unit: **46/46 cases, 577 assertions, SUCCESS**.
- Stata suites: **31/31 PASS** (integration + verify_suite incl. new V21/V22 + roundtrip), every
  pre-existing invariant intact (V03_PERIOD_DATES, V05_HHMM, V06_UINT32, V09_ATOMIC_CLEAR,
  V15_FLOAT_EXTREMES, V20_PARALLEL_FILL, …).
- Perf: reference-file `use→collect` gap **0.244 s → 0.007 s** (min-of-6, same session); all seven
  synthetic workflows **0 failures**, no regression beyond box noise.

*Applied diffs this round: `src/engine/view.hpp` + `src/plugin/plugin_view.cpp` (collect passthrough
sizing #38; date-aware collect floor #39). New tests `tests/verify_suite/v21_collect_passthrough_sizing.do`,
`v22_collect_date_no_overflow.do`, fixture `tests/fixtures/far_dates.parquet`, probe
`benchmarks/probe_collect_vs_use.do`.*

---

## 9. Round 3 (2026-07-02, v0.1.14) — summarize-detail 6.4x; regressions from the correctness round measured and repaired

Method as §2, synthetic 10M×13 workers panel, min-of-3 on a quiet machine,
baseline = pre-audit HEAD build in a separate worktree (same DuckDB tarball).
Every change gated by the full suite plus a new native-oracle test.

| Workflow (10M rows) | HEAD | v0.1.14 | Δ |
|---|---:|---:|---:|
| `use, clear` | 1.428 | 1.424 | — |
| `use` + `collect` | 1.410 | 1.489 | noise |
| filter+gen+save | 0.079 | 0.074 | — |
| expr-heavy collect (5 gens, 2M) | 4.452 | 4.693 | **+4.7% (INF-1 guards — correctness price)** |
| collapse 6 stats by firm | 0.378 | 0.363 | — |
| gsort+save | 0.678 | 0.709 | noise |
| merge m:1+save | 1.164 | 1.087 | — |
| reshape long+save | 0.613 | 0.591 | — |
| duplicates drop (no varlist) | 1.088 | 1.152 | +6% (was +38% with the DUP-NORM-1 window; repaired via hash GROUP BY + any_value) |
| summarize (13 vars) | 0.147 | 0.148 | — |
| **summarize, detail (4 vars)** | **4.375** | **0.684** | **−84% (6.4x)** |
| codebook (13 vars) | 0.254 | 0.250 | — |
| tabstat by() | 1.204 | 1.223 | — |

**PERF-DET-1** (the §8.5 "highest-value follow-up", now landed): detail was one
query per variable — mean subquery + nine `list_sort(list(x))` (single-threaded)
per query. Now: pass 1 count/mean/min/max for all vars (one scan); pass 2
central moments with the pass-1 mean as an exact dtoa literal (identical
two-pass math); per-var order statistics via `CREATE TEMP TABLE … ORDER BY`
(DuckDB's parallel sort: 0.17s for 10M doubles vs 1.1-1.4s for
quantile_disc/list_sort, both effectively single-threaded in finalize) plus
O(1) rowid point-picks; Stata's percentile rule applied in exact integer
arithmetic. Multi-stage pipelines materialise once, so k variables cost one
pipeline run. `quantile_disc` was prototyped first and **rejected on
measurement** (index semantics verified against quantile_sort_tree.hpp, but
1.1s/var finalize made 4 vars *slower* than baseline). Locked by
**V44_SUMMARIZE_DETAIL_NATIVE**: r()-equality with native `summarize, detail`
over even/odd counts, integral/non-integral n·p/100, n=1..7, constants,
missings, int64/byte, all-missing.

**PERF-DUP-1**: DUP-NORM-1's correctness fix (normalized-key dedupe) had used a
`row_number()` window (+38%); a parallel hash `GROUP BY norm(key…)` +
`any_value` keeps the semantics (rows within a group are identical up to
missing encoding) at DISTINCT-like speed.

**PERF-STRL-1**: the strL sidecar reader parses 8 MiB gulps instead of two
`fread()` per cell. Local wall-clock unchanged — measured floor is Stata's own
strL store (`st_sstore` alone: 0.50s/200k×2.9KB cells; parqit end-to-end 2.0s
vs 0.44s for the same payload as str2000) — but ~70 syscalls replace 400k,
which matters on syscall-latency-bound HPC filesystems. Measured and left:
further strL work hits the st_sstore floor.

**Measured and rejected this round:** `quantile_disc` for detail (above);
UNION-ALL per-variable parallelism (branch finalizes serialize: 2.65→2.16s);
folding the tabstat by() precount (grouped percentiles dominate its 1.2s, and
the fold would slow the >200-group error path — same verdict as §8.5).

---

## 10. Headroom assessment (2026-07-02) — what is still achievable, and at what cost

Requested explicitly: an assessment of the gains that remain on the table.
Grounded in this session's measurements (10M×13 synthetic panel, 48-core EL9).

### 10.1 At the floor — no meaningful headroom without platform changes

| Path | Now | Floor | Why the gap is structural |
|---|---:|---:|---|
| `use, clear` / `collect` (10M×13) | 1.42 s | ~0.6 s engine | The gap is the SPI per-cell store. It is already parallel and **does not scale further**: `PARQIT_FILL_THREADS` 4/8/16/24 → 1.56/1.63/1.73/1.69 s (min-of-4). Memory-bandwidth/SPI bound; only a bulk Stata-memory API (does not exist) would move it. |
| filter/collapse/sort/merge/joinby/reshape → `save` | 0.07–1.1 s | ≈ same | Measured at the DuckDB floor (§8.1, re-confirmed this round). The verbs are plan edits; the engine is the cost. |
| strL reads | 2.0 s / 580 MB | ~0.5 s | Floor is Stata's own `st_sstore` (0.50 s / 200k×2.9KB cells, measured bare). Sidecar+parse ≈ 0.7 s could shrink ~0.3–0.5 s by overlapping the sidecar write with the scan — medium risk on a delicate path, low priority. Mata is the only strL writer; the disk round-trip cannot be eliminated. |
| `summarize` (plain), `codebook`, `misstable`, `distinct` | 0.14–0.28 s | ≈ same | Already single-scan for all variables. |
| `summarize, detail` | 0.68 s | ~0.5 s | Just landed at 6.4×; the residual is two scans + k parallel sorts, near-irreducible for exact percentiles. |

### 10.2 Real remaining opportunities, ranked

1. **Grouped exact percentiles with few, large groups** — `tabstat, by()` (1.2 s
   for 3 stats × 3 vars × 4 groups) and `collapse (median/pNN)` on low-cardinality
   by(): the per-group `list_sort` is single-threaded. A single parallel sort of
   (by, x) + per-group rank picks (the §9 detail technique generalised with a
   group-count join) could take that shape to ~0.4–0.6 s (**2–3×**). Many-small-
   groups shapes (the common collapse) are already fine (0.36 s / 500k groups).
   Complexity medium-high; the exact Stata rule per group is delicate. Do it if
   users hit the few-large-groups shape in practice.
2. **INF-1 guard flattening in the translator** — nested `parqit_finite` calls in
   an arithmetic chain collapse to the outermost guard with identical missing
   semantics (an inner overflow propagates as NULL/Inf either way). Recovers most
   of the measured +4.7% on expression-dense pipelines (~0.15–0.2 s / 2M×5 gens).
   Medium risk: a translator refactor over freshly audited code; needs chain-shape
   unit tests. Worth it in a maintenance release, not urgent.
3. **Bounded group-count probes** — `tabulate` (10k cap) and `tabstat by()` (200
   cap) materialise all groups before rejecting. A LIMIT-bounded pre-probe would
   cap the *error* path only; the success path is unchanged. Tiny win, tiny risk.
4. **Not worth doing** (measured/analysed and rejected): more fill threads (no
   scaling — above); UNION-ALL per-variable stats parallelism (finalizes
   serialize); `quantile_disc` (single-threaded finalize); narrowing integral
   doubles on save (data-dependent type flapping vs v41's fidelity contract);
   dropping the save ORDER BY or the post-write verify (§8.4, unchanged).

### 10.3 Bottom line

The manipulation layer is at the engine floor and the read path is at the SPI
floor; both are structural. What remains is: grouped-percentile parallelisation
(2–3× on one specific shape), guard flattening (~5% on expression-heavy
pipelines), and micro-bounds on two error paths. parqit's headline costs are now
where they belong — in DuckDB and in Stata's own storage interfaces.
