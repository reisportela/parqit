# ASSUMPTIONS.md

Decisions taken where the build brief (`parqit_build_prompt.md`) leaves latitude,
with rationale. Fixed decisions from the brief are not repeated here. Each
entry notes the conservative fallback if the assumption proves wrong.

## Engine & vendoring

1. **DuckDB pinned at 1.5.3** (latest stable release at project start,
   2026-06), vendored as the **full source tree fetched at the pinned tag**
   (SHA256-verified by CMake; offline override `-DPARQIT_DUCKDB_ARCHIVE=`).
   The brief's preferred amalgamation was tried first and rejected on
   evidence: the released 1.5.x `libduckdb-src.zip` is the bare engine —
   neither the `parquet` extension nor `core_functions` (where even
   `version()` lives since 1.5) is inside it. A source build statically
   links both via DuckDB's own default extension config, which
   `tests/unit/test_session.cpp` asserts permanently. The plugin exports
   only `stata_call`/`pginit` (version script / exported-symbols list), so
   the embedded DuckDB can never clash with another plugin's.
2. **C API only.** The plugin calls DuckDB exclusively through the stable C
   API (`duckdb.h`). The C++ API (`duckdb.hpp`) is vendored only because the
   amalgamation source requires it at compile time. Rationale: the C API is
   the documented stability surface; mixing the version-unstable C++ classes
   into plugin code buys nothing at our layer.
3. **Arrow transfer uses the modern, non-deprecated C-API pair**
   `duckdb_to_arrow_schema` + `duckdb_data_chunk_to_arrow` (verified present
   in the pinned `duckdb.h`), producing `ArrowSchema`/`ArrowArray` structs per
   the vendored `vendor/arrow/abi.h`. parqit walks those buffers directly to
   fill Stata. The deprecated `duckdb_query_arrow*` family is not used.
4. **Canonical transfer types.** All type policy lives in the C++
   manifest/typemap module: the final `SELECT` casts every result column to a
   canonical transfer set (BOOLEAN→TINYINT, TINYINT, SMALLINT, INTEGER,
   BIGINT, FLOAT, DOUBLE, DATE, TIMESTAMP(us), VARCHAR). The Arrow walker
   therefore only ever sees those formats. DECIMAL(p,s)→DOUBLE,
   UINT8→SMALLINT, UINT16→INTEGER, UINT32→BIGINT, UINT64/HUGEINT→DOUBLE
   (bound-checked, warned when > 2^53), ENUM/UUID→VARCHAR,
   TIMESTAMP_S/_MS/_NS→TIMESTAMP (ns truncates toward −∞; documented),
   TIMESTAMPTZ→UTC instant (documented), TIME→DOUBLE milliseconds since
   midnight with display format `%tcHH:MM:SS` (correct because Stata's %tc
   epoch day 0 is 1960-01-01: ms-since-midnight displays as the time of day;
   never an all-null column — charter §6.5). INTERVAL/BLOB/LIST/STRUCT/MAP/
   UNION/BIT are dropped-with-message (error if every column would be
   dropped) — never silent all-missing columns (charter §6.11). The
   genuinely typeless DuckDB `NULL` type drops too (see #43); an all-null
   *typed* column (the realistic Parquet case) loads as a faithful
   all-missing variable of its own type.
5. **"Sized by range" is computed, not guessed.** For integer-family result
   columns the plugin runs one aggregate pass (`min`, `max` per column) over
   the materialised result and picks byte/int/long/double using Stata's exact
   limits (byte −127..100, int −32,767..32,740, long −2,147,483,647..
   2,147,483,620). int32 values outside Stata's long range (e.g.
   −2,147,483,648, 2,147,483,621..2,147,483,647) therefore land in `double`
   rather than colliding with missing codes. VARCHAR columns get
   `max(octet_length)` in the same pass to size `str#` / promote to `strL`
   (>2045 bytes; Stata string sizes are bytes, not characters).
6. **Materialise-once collect.** `parqit collect` runs the pipeline once into a
   DuckDB temp table (spillable to `temp_directory`, so still out-of-core),
   reads schema + row count + ranges from it, has the ado pre-create
   variables, then streams chunks Arrow→Stata. This avoids running the user's
   query twice (once for count, once for data) and keeps the count exact.

## Stata side

7. **Stata 16.0 baseline, SPI 3.0** (`version 16.0` in the ado, matching
   xhdfe). Frames are therefore available for atomic staging.
8. **View/plan state lives in the plugin** (a per-session singleton holding
   the DuckDB instance, the source registration, the op list and the column
   manifest). The ado keeps only cosmetics. Consequence (documented): `discard`
   or `program drop _all` unloads the plugin and resets any un-materialised
   view; data on disk is never affected.
9. **ado→plugin protocol is a JSON request file** (path passed as the single
   `plugin call` argument, hex-encoded) in which **every user-originated
   string value is hex-encoded UTF-8**, so no quoting/escaping bug class can
   exist in the ado-side writer (writer is a small Mata helper; parser is
   nlohmann/json in the plugin). Plugin→ado responses go through
   `SF_macro_save` locals, arbitrary text again hex-encoded; the ado decodes
   with the same Mata helper. Big payloads (schemas for 2,500+ vars) fit
   comfortably in Stata-MP macros (≈4 MB cap); if a response ever exceeds a
   safe threshold the plugin switches that field to a response tempfile.
10. **Atomic collect** stages into a tempframe, applies *all* metadata there,
    `save`s to a tempfile and `use`s it in the user's frame — the in-memory
    dataset is destroyed only after the staged result is a complete, valid
    .dta (charter §6.9). After the swap, parqit clears `S_FN`/`S_FNDATE` so
    `c(filename)` does not point at a vanishing tempfile (verified against
    this Stata; if a future Stata decouples `c(filename)` from `S_FN`, the
    fallback is import-like semantics: empty filename, `c(changed)`
    documented).
11. **`plugin call` always passes an explicit varlist** and the request
    carries the same names in order; the plugin cross-checks `SF_nvars()`
    and per-position string-ness (`SF_var_is_string`) against the manifest
    before touching any data (charter §6.1 made structural).

## Type & metadata details

12. **Strings:** parquet NULL and `""` both become `""` in Stata (Stata has
    no string missing); on write `""` is written as `""`, never NULL.
    Documented asymmetry. Binary strLs (`SF_var_is_binary`) are refused
    loudly in v1 (no BLOB path yet); text strLs round-trip.
12a. **strL writes cannot cross the SPI** (empirical: SPI 3.0 has
    `SF_strldata` for reading only; `SF_sstore` silently truncates strL
    targets). parqit therefore streams strL cells from the plugin into a
    binary sidecar file (fixed 32-byte header + raw bytes per cell) which
    Mata pours into the staged dataset via `st_sstore` — Mata strings have
    no SPI length limit. Covered by the strL leg of
    `tests/roundtrip/t01_basic_roundtrip.do`.
12b. **Saved Stata types round-trip.** The observed-range pass picks the
    smallest exact type for foreign files, but when `parqit.schema` records
    the original type, that type wins (widened only if third-party edits
    put values beyond its range): a `long` saved through int32 comes back
    `long`, a `str8` keeps width 8, a short strL stays strL (§4 byte-exact
    round-trip promise).
13. **Extended missings `.a`–`.z`** map to a single Parquet NULL on write,
    with the loss reported (warning listing affected variables). Per the
    build brief (§4: extended missings "survive only via this metadata"),
    their *label definitions* round-trip (in the `parqit.*` value-label blob)
    but their per-cell identity does not — parqit→parqit restores plain `.`.
    This is the specified v1 contract, documented in help + README
    "Limitations" — not a temporary gap. Positional restoration via a
    `parqit.*` RLE map remains a possible future enhancement, not a committed
    deliverable; it would need a new metadata key kept backward-compatible
    with files already written, and the read/write hot paths plus the
    `t01_basic_roundtrip` assertion would change with it.
14. **`%tC` (leap-second) and `%tb` (business calendar) variables** are
    stored as INTEGER counts with their format recorded in `parqit.*` metadata
    — same policy as `%tm/%tq/%th/%ty/%tw` (charter §6.3): semantics survive
    parqit→parqit, and no third-party reader ever sees mis-scaled calendar dates.
15. **Stata value labels, variable labels, notes, display formats,
    characteristics and the original (pre-sanitisation) column names** are
    serialised as JSON under file-level parquet KV metadata keys
    (`parqit.schema`, `parqit.vallabs`, `parqit.notes`, `parqit.chars`,
    `parqit.version`). Parquet has no widely-readable per-column KV channel, so
    file-level is the interoperable choice.
16. **Timestamp precision:** Stata `%tc` is integer milliseconds;
    TIMESTAMP(us) values are floor-divided to ms (exact when the source is
    ms-resolution; sub-ms truncates toward −∞ deterministically; documented).

## Build & test

17. **Unit-test framework: doctest 2.4.12** (single vendored header, tests
    target only, never shipped in release artifacts).
18. **CI builds and runs C++ unit tests on all three OSes; Stata integration
    and verify suites run on licensed machines** (StataNow MP on this Linux
    box; macOS locally). CI cannot run Stata (no license in runners) — same
    constraint and convention as pq.
19. **Linux release binary** is built in an AlmaLinux 8 container
    (glibc 2.28) with `-static-libstdc++ -static-libgcc`, so one `.so` runs
    on EL8/EL9 HPC clusters and modern distros alike. macOS deployment
    targets: 11.0 (both architectures).
20. **`parqit save` of the in-memory dataset** (no open view) bridges
    Stata→DuckDB through a temp table filled via `SF_vdata`/the appender —
    the brief's sanctioned v1 bridge (its temp-Parquet variant, minus one
    disk round-trip; the temp table spills via `temp_directory` if needed).
    The Arrow-scan ingestion path remains the documented later optimisation.

20a. **Large-read benchmark native `use` leg.** Stata's native `use` command
     reads `.dta`, not Parquet, and this Stata installation has no native
     `import parquet` subcommand. The benchmark harness therefore times
     native `use` on `main_95_21_ready.dta`, matched to
     `main_95_21_ready.parquet` by observation and variable count, generating
     a scratch `.dta` from that Parquet only when no matching candidate is
     available. The conversion time is reported but excluded from the read
     benchmark.

20b. **Synthetic performance data scale.** Feature/precision fixtures remain
     tiny (`examples/make_data.py` and self-contained test do-files). The
     synthetic performance family generated by
     `benchmarks/make_synthetic_data.py` defaults to a medium scale
     (10M worker-year rows, 500k firms, about 1M patent rows, 1.5M
     wide-income rows that expand to 12M rows in `reshape long`, and 750k
     hostile-schema rows), compressed with Parquet
     ZSTD and 65,536-row groups. This is the current compromise between
     timing signal and local iteration cost; performance claims still require
     repeated runs under comparable host load.

20c. **Unchanged-source save fast path.** The in-memory `parqit save …, data`
     bridge remains the fully general writer. A narrower fast path is allowed
     only when the current dataset is still the unchanged result of
     `parqit use …, clear` from one regular Parquet file: the ado marks that
     dataset with an internal nonce, the plugin records the source file's
     absolute path/size/mtime, and `parqit save` rechecks both `c(changed)==0`
     and the file fingerprint before using DuckDB `COPY` directly from the
     source. The fast path is disabled for source-name sanitisation/duplicates
     and for `%tc`/unknown temporal formats; `%td` and period-count formats are
     safe because the direct SQL writes the same DATE/INTEGER physical types as
     the general Stata-memory writer. The internal nonce characteristic is
     omitted from `parqit.chars`.

## M2–M5 decisions

21. **Named views; collect does not consume.** Several lazy views can be
    open at once (`name()` on `parqit use`/`parqit sql`/`parqit open _data`;
    `parqit view <name>` switches, `parqit view <name>: <cmd>` runs one-offs,
    `parqit views` lists, `parqit close [name|_all]` closes); verbs hit the
    current view. `parqit collect` keeps the view alive (dbplyr semantics;
    re-collecting re-executes). The original collect-consumes rule existed
    to stop `parqit save` silently writing a stale pipeline after a collect;
    that hazard is now handled by explicitness instead: with a view open,
    `parqit save` materialises the *current view* and says so by name, and
    the `data` option forces an export of the in-memory dataset. A plain
    `parqit use <file>, clear` read never touches any view.
22. **`merge m:m`** implements Stata's sequential pairing via a per-key
    spine (i = 1..max(n_m, n_u)) with clamped row lookups — exactly
    Stata's documented result, including repeated last rows.
23. **`keep in #`** keeps exactly observation #, like native Stata; ranges
    are validated structurally at the verb and against real counts at
    materialisation. Negative/inverted forms are rejected on a lazy view.
24. **Sampling** uses DuckDB reservoir sampling (`count` = rows, default =
    percent), reproducible with `seed()`; without a seed it is
    nondeterministic, like Stata without `set seed`.
25. **`parqit summarize`** returns `r()` of the last variable summarised
    (Stata convention); both summaries run as single pushdown aggregates.
26. **Dates inside pipelines are Stata numbers** (day counts, millisecond
    counts) — converted only at the Parquet boundary, so date arithmetic
    and `td()`-style literals translate verbatim. Timestamp µs values are
    floored to ms with exact integer arithmetic.
27. **`duplicates drop <varlist>`** requires a declared sort: "first
    occurrence" must be well-defined on a parallel engine (determinism by
    design; plain `duplicates drop` needs no order).
28. **`parqit sql`** opens a view over the query result with the same
    boundary casts as file sources; `parqit query` appends a verbatim
    fragment and validates it compiles immediately — a broken fragment
    closes the view loudly rather than leaving it half-working.
29. **`chunk(#)` = Parquet row-group size.** The brief lists `chunk()` on
    `parqit save` without defining it; the natural engine meaning is rows
    per row group (`ROW_GROUP_SIZE` in DuckDB's COPY). DuckDB rounds it
    to multiples of its 2048-row vectors — documented in the help; values
    ≤ 0 are rejected loudly.
30. **`reshape long` validates `i()` uniqueness eagerly** (one aggregation
    pass at plan time), mirroring what `reshape wide` already did for
    `(i,j)`. Laziness loses one pass; silently fabricating long data from
    duplicate panel ids (audit PARQIT-02) would be a charter violation.
31. **`open _data` bridge files are per-promotion and view-owned.** A
    unique snapshot per promotion is the only design under which several
    promoted views can coexist (audit PARQIT-01); the plugin deletes the
    file when its view is closed or replaced, so promotions cannot
    accumulate in the temp dir within a session.
32. **NaN is the silent float NA; ±Inf is a loud missing.** Many parquet
    writers encode NA as NaN, so NaN→`.` without a note (warning would be
    constant noise); Inf is a *value* Stata cannot hold, so the load
    prints a per-column count when it collapses to missing.
33. **float32 columns widen to double by observed range.** Finite float32
    values in ±(1.70e38, 3.40e38] exceed Stata's float ceiling; the
    range pass (FILTER isfinite) promotes such columns to double with a
    note — never a silent missing.
34. **Embedded NUL bytes in str# values truncate loudly.** The SPI is
    C-string; truncation at the first NUL is unavoidable for str#, so the
    load reports a per-column count of truncated cells.
35. **String writes canonicalise NULL≡"" to ""** — the distinction does
    not exist inside Stata, so the writer emits "" (never NULL) for
    string cells; third-party readers see empty strings.
36. **Column sizing trusts Parquet row-group statistics when exact.** On
    read, integer (`byte/int/long`) and float-vs-double sizing is taken
    from the per-row-group `stats_min_value`/`stats_max_value` in the
    Parquet footer instead of a full data scan, but **only** where the
    answer is provably exact: integer columns reaching this path are
    ≤32-bit (64-bit ints are excluded and still scanned), so their min/max
    is exact in a double; a float column is trusted only when metadata
    proves both bounds fall inside Stata's float range, else it falls back
    to the exact `FILTER(isfinite)` scan. Metadata is used only when every
    row group carries a non-null min and max; files with duplicate column
    names disable the metadata path (merged `path_in_schema` groups would
    be ambiguous). String byte-length (`str#` vs `strL`) is never in
    Parquet stats, so strings always scan. This removes the second full
    pass over the file in the common all-numeric case without changing any
    chosen storage type (verified against the prior scan-based result and
    the v06/v15/v18 verify tests). Conservative fallback if a writer emits
    wrong stats: the only risk is over-/under-sizing, caught by the
    round-trip oracle tests; revert to an unconditional scan if it occurs.
37. **Reads of ≥50k rows fill Stata in parallel (producer/consumer
    pipeline).** The Parquet→Stata materialise (`parqit use …, clear`,
    `parqit collect`) writes every result cell through the per-cell SPI
    store, which dominates the read. The brief mandates studying the prior
    art `stata_parquet_io` (pq) for mechanics: pq calls the *identical*
    `SF_vstore`/`SF_sstore` from many worker threads over disjoint row
    ranges in production — establishing that the store is reentrant for
    **distinct** cells. parqit adopts this as a pipeline: the calling thread
    is the producer (DuckDB fetch + Arrow convert, necessarily
    single-threaded — `duckdb_data_chunk_to_arrow` dereferences the shared
    client context), and up to `min(cores, 8)` worker threads each fill
    whole chunks. Disjoint chunks → disjoint observations → no two threads
    touch the same cell; `fill_column` is reused unchanged, so every
    type/missing/Inf/NUL rule is byte-identical to the serial path (only
    the scheduling differs). Shared state is race-free by construction: the
    strL sidecar FILE is written under a mutex (records carry
    position-encoded headers, so order is irrelevant), the Inf/NUL tallies
    are per-worker vectors reduced after the join, and the queue / abort
    flag / first-error string are guarded by the queue mutex. No C++
    exception may cross a thread boundary (charter §6.8): the worker bodies
    and the producer loop are wrapped so a throw (e.g. `std::bad_alloc`)
    becomes the same loud nonzero-rc abort a soft failure uses, and the
    workers are always joined before return — preserving
    validate-then-mutate atomicity (V09). Reads below 50k rows, and
    `PARQIT_FILL_THREADS=0|1`, keep the unchanged serial path;
    `PARQIT_FILL_THREADS=n` overrides the worker count (≤1024) for atypical
    very wide / string-heavy reads. On the 47.6M×8 reference file this cut
    `parqit use` ≈2.7s→≈1.5s with identical values (independent pyarrow
    oracle at 1.5M rows — verify test **V20_PARALLEL_FILL** — and a
    serial-vs-parallel checksum at 47.6M); the producer's single-threaded
    scan-drain is the remaining floor. Conservative fallback:
    `PARQIT_FILL_THREADS=1` restores exact serial behaviour if a platform's
    store ever proves non-reentrant.

38. **A pure full-file passthrough `collect` sizes columns from Parquet
    statistics, exactly like `parqit use`.** `parqit use FILE` + `parqit collect`
    builds a lazy view then materialises it. When that view is an
    untouched full-file read (`direct_read`: no stage, sort, filter, range,
    limit or projection — guaranteed by `n_stages()==0`), its columns are
    byte-for-byte the columns a direct `parqit use FILE, clear` would read, so
    its sizing may use the same F2 row-group-statistics path (#36) instead
    of a redundant second full scan. The view now carries the backing
    Parquet paths (`View::set_source_paths`, set only by `cmd_view_open`
    over files — empty for SQL/bridge sources), and `cmd_view_collect_prepare`
    feeds them to `plan_columns` on the `direct_read` branch. Precision is
    unchanged by construction: `plan_columns` still falls back to a real
    scan for any column the footer cannot size exactly (strings always;
    >2^53 ints; floats whose footer bound exceeds Stata's float range;
    date/timestamp stats that don't cast to a number; duplicate-named or
    stats-less files), so the metadata-sized plan is identical to the
    scan-sized one. Verified byte-identical (storage type, format and value
    signature) against the direct path across the type spectrum — verify
    test **V21_COLLECT_PASSTHROUGH_SIZING** (all-numeric, int/double/string/
    DATE, uint32/decimal/dup-name, and a multi-file glob). On the
    all-numeric 47.6M×8 reference file this closes the `use`→`collect` gap
    (≈+0.24s → ≈+0.007s, same-session min-of-6); string-heavy files were
    already scan-bound and are unchanged (the residual scan only narrows,
    never widens, so no read can regress). The materialise-then-size path
    (any view with stages/sort/filter) is unaffected — it has no Parquet
    footer to consult and still sizes from its temp table.

39. **A bare Parquet DATE column collects as Stata `long`, matching
    `parqit use`.** The read planner maps a Parquet `DATE` to `long`
    unconditionally (a date can span beyond `int`; `typemap` rule). On the
    `collect` path the column reaches the planner already cast to an integer
    day-count, so range refinement could shrink it to `int`/`byte` and
    overflow for dates past ~2049 (>32740 days from 1960). The collect
    metadata overlay now restores the date-aware floor: a column whose
    format is `%td` **and** which carries no recorded Stata `meta_type`
    (parqit-written files carry one and are governed by it) is stored `long`.
    This is a pre-existing `collect`-vs-`use` discrepancy fixed here, not a
    consequence of #38 (date footer stats never cast to a number, so #38's
    metadata path never touches a date column). Verify test
    **V22_COLLECT_DATE_NO_OVERFLOW** loads dates spanning 1900–2099 and
    checks the exact day-count against an independent oracle on both paths;
    period counts (`%tm`/`%tq`/…, stored as integers) and datetimes
    (`%tc`, stored as doubles) already agreed between the paths and are
    untouched (V03_PERIOD_DATES, V05_HHMM still pass).

40. **`parqit use … , relaxed` unions a mixed-schema file set by column name.**
    A glob/Hive set whose files do not share one schema is, by default, a loud
    error (`read_parquet` over `['…']` reports the mismatch — never a silent
    column drop). `relaxed` opts into DuckDB `read_parquet(…, union_by_name =
    true)`: the view's columns are the union across files, and a column absent
    from a given file reads as Stata missing for that file's rows — the same
    contract as pq's `relaxed` and as `parqit append` (which already unions by
    name). The flag rides through both `parqit use` paths (the lazy `view_open`
    and the direct `use_prepare`) via `source_for(files, relaxed)`. Precision
    is unaffected: the F2 metadata-sizing fast path (#38, #36) still holds
    because a column carried by only some files has per-row-group stats in
    fewer groups than the total, so `count(stats) < count(*)` and it falls back
    to an exact scan. Default off keeps the strict single-schema behaviour.
    Recorded for the pq→parqit Parquet feature-parity audit (see
    `PARITY_parqit_vs_pq_claude.md`); verify test **V23_RELAXED_UNION_BY_NAME**
    (loud without, exact union with, homogeneous glob unaffected).

41. **Non-Parquet inputs: CSV scans out-of-core; .dta/.xls/.xlsx bridge.**
    A `parqit use` source and a `merge`/`joinby`/`append` `using` side are
    dispatched by file extension (ado helper `_parqit_resolve_source`):
    - `.parquet`/dir/glob → `read_parquet` (as before);
    - `.csv`/`.tsv`/`.txt`/`.tab` → DuckDB `read_csv_auto`, scanned out-of-core
      like Parquet (the engine carries no Parquet footer for CSV, so the
      metadata paths — dup-name recovery, parqit.* labels, F2 stats sizing — are
      skipped and columns size from the scan). The request carries `csv:true`
      (a JSON boolean like `relaxed`/`owned`, NOT a hex `_parqit_jtext` value —
      the plugin reads `req.value("csv", false)`);
    - `.dta`/`.xls`/`.xlsx` → not engine-scannable, so the ado imports the file
      into a throwaway frame (`use` / `import excel` / `import delimited`) — the
      caller's working dataset is untouched — and `parqit save … , data` snapshots
      it to a Parquet *bridge* in `c(tmpdir)` the engine then scans. The bridge
      carries the source's labels/formats. The choice is deliberate: a bridge is
      right for a *small* side (a lookup `.dta`, an `.xlsx`); a *large* `.dta`
      master gains nothing (it would enter Stata anyway) — prefer `use` + `parqit
      open _data`. Lifetime: a `parqit use <dta>` lazy view *owns* its bridge (the
      plugin erases it on close/replace via the `owned` flag); a `parqit use
      <dta>, clear` bridge is consumed into memory and erased immediately; a
      `using`-side bridge is registered in `$PARQIT_IMPORT_BRIDGES` and swept up
      at `parqit close _all`. SAS/SPSS stay out of scope (parqit links no reader and
      the brief excludes them). Verify test **V24_MULTIFORMAT_SOURCES** (CSV/
      .dta/.xlsx as source; the lazy-master + merge(.dta) + collect workflow
      keeping the master out of memory; joinby with a CSV using side).

42. **The in-memory → DuckDB transfer is single-threaded by necessity.**
    `parqit save … , data` / `parqit open _data` move Stata's in-memory columns into
    DuckDB (a temp table, then COPY to Parquet). The write fills DuckDB data
    chunks in 2048-row column batches and appends them whole (not one
    `duckdb_append_*` per cell): ~8.8 s → ~7.4 s on 10M×13, conversions
    byte-identical. The residual cost is the per-cell `SF_vdata`/`SF_sdata`
    reads (~5.5 s/10M). These **cannot be parallelised**: calling the SPI read
    functions from `std::thread` workers corrupts the heap (double-free crash) —
    the SPI *store* (`SF_vstore`/`SF_sstore`) is reentrant for distinct cells
    (the basis of the parallel fill, #37) but the *read* side is not, confirmed
    empirically. So the read stays on the calling thread. **A Mata bulk-extract
    bridge was tried and does not help** — two dead ends, both reverted:
    (a) Mata `st_data()` copies 8×10M numeric columns in ~0.2 s, but the only
    channel to the plugin is a file, and a raw little-endian dump
    (`fbufput "%8z"`) round-trips ~0.7 GB (10M) to disk — the write+read costs as
    much as the per-cell reads it replaces (10M×13 measured ~8.1 s, *slower* than
    the 7.4 s bulk path), and on the 47.6M×8 reference the 3 GB Mata matrix + 3 GB
    file errors (`r(3300)`); (b) Mata string serialisation (`invtokens`) is ~13 s
    for 4×10M, worse than `SF_sdata`. The 7.4 s bulk write is therefore the
    practical floor for the bridge. **For an in-memory ⋈/+ disk join the fast
    route is `parqit mergein`/`parqit appendin`** (a native `merge`/`append` reading
    only the needed columns of the disk side — the in-memory data never
    round-trips); the `parqit open _data` bridge is for *big ⋈ big*, where DuckDB's
    hash join outweighs the ~7.4 s transfer.

43. **The brief's `NULL`-type → drop rule binds the *typeless* DuckDB `NULL`
    type, not an all-null *typed* column.** Adversarial audit PARQIT-C02 read
    the type map's old `DUCKDB_TYPE_SQLNULL` → all-missing `byte` case as a
    brief violation (§4/§6.11 group `LIST/STRUCT/NULL` for drop-with-message).
    The type map now drops a genuinely typeless `DUCKDB_TYPE_SQLNULL` column
    exactly like `LIST`/`STRUCT` (verified by the C++ unit test
    `test_typemap` and mirrored in the lazy-view planner). Empirically,
    however, that case is *unreachable from the read path*: a Parquet "null"
    column carries a physical type (pyarrow's `null` is written as an
    all-null `int32`), so DuckDB's `read_parquet` reports it as `INTEGER`,
    not `SQLNULL` — and even a bare `SELECT NULL` literal resolves to
    `INTEGER` in DuckDB. Such all-null *typed* columns therefore load as a
    faithful all-missing variable sized to their own type (an all-null
    integer → all-missing `byte`), which is correct and is **not** the pq
    finding-11 hazard (that was real *data/structure* — decimal, list,
    struct — silently blanked). `v11_unsupported_types` asserts this faithful
    all-missing behaviour for an all-null column and the loud drop-all error
    for a file whose every column is genuinely unrepresentable (all `list`).
    Net: the code matches the brief letter for the typeless case while the
    realistic Parquet case stays a faithful, loss-free all-missing column.

44. **Expression-translator Stata-fidelity fixes (2026-06-14 cross-audit,
    tightened 2026-06-16).** Verified against real Stata 19.5:
    (a) `string()`/`strofreal()` emits Stata's `%9.0g`, not a raw SQL `CAST`.
    The internal DuckDB scalar follows Stata's width-constrained decimal vs
    scientific switch, including exponent-width edges such as `1e100` →
    `1.0e+100`, the small-magnitude decimal band such as `.00009999999` →
    `.0001`, and the scientific cutoff such as `.000009999999` → `1.00e-05`.
    (b) `substr()`/`strpos()` are BYTE-indexed like Stata (`usubstr`/`ustrpos`
    stay character-indexed). A byte slice that splits a multibyte UTF-8
    sequence cannot be carried as a DuckDB/Arrow VARCHAR, so `substr()` maps
    that invalid fragment to U+FFFD instead of returning `""` or aborting; valid
    byte slices are exact. (c) `^` is left-associative; `mod(x, y≤0)` is missing; `inrange()` treats a
    missing bound as ±∞ and a missing `x` as out of range; logical `&`/`|`/`!`
    and bare `if x` treat a missing value as true (nonzero); `==`/`!=` are total
    (0/1) under `statamissing`. (d) `real('inf')`/`real('nan')` → missing.

45. **Out-of-core join keys are normalised to Stata's missing equivalence.**
    `merge`/`joinby` compare keys with `IS NOT DISTINCT FROM` after mapping a
    string `""` → NULL and a floating NaN → NULL, so a missing key matches a
    missing key regardless of how each side encodes it (pandas/pyarrow write a
    missing float as NaN; DuckDB/parqit write NULL). Integer keys are untouched
    (the CASE returns the original value, so no type/precision change). Without
    this, an out-of-core join could give a different `_merge`/match set than
    native Stata, `parqit mergein`, or `parqit collect` of the same data.

46. **Number↔SQL text is locale-independent** (`std::to_chars`/`std::from_chars`,
    `dtoa`/`atod` in `session.cpp`). `std::to_string`/`printf("%g")`/`strtod`
    honour `LC_NUMERIC`, so under a comma-decimal OS locale they would emit/parse
    `"3,14"` and break generated SQL (collapse percentiles/median, `sample
    <share>`, the histogram, skewness/kurtosis). parqit now always uses '.' and the
    shortest round-trippable form. Stata itself keeps `LC_NUMERIC=C`, so this was
    latent, but it is now correct on any process locale.

47. **Known low-risk items left as-is (documented, not silent).** A few audit
    items are correct today and were deliberately not changed to avoid a
    per-cell cost or a riskier rewrite, with the rationale recorded here:
    (a) `SF_vstore`/`SF_sstore` return codes are not checked per cell on the fill
    path — the manifest's `SF_in`/`SF_nvars`/per-position checks make an
    out-of-range store impossible, and a per-cell branch would tax the hottest
    loop. (b) The Arrow string walker assumes DuckDB's default regular int32
    offsets (correct for the pinned DuckDB 1.5.3; a single chunk would also need
    > 2 GB of string bytes to overflow). (c) Partitioned `parqit save` writes
    directly to the final tree (not via a temp-then-rename) — a mid-write failure
    is loud (nonzero rc) but can leave a partial tree the user must remove before
    retrying; single-file save is fully atomic. (d) The lazy-view `parqit save`
    path performs the same extended-missing / fractional-date conversions as the
    in-memory path but does not re-emit their warning notes. (e) A characteristic
    on a foreign column whose name was sanitised is dropped on a view re-save
    (the char target is not remapped through the sanitiser). These are tracked
    for a future pass; none silently corrupts data.

48. **In-memory `parqit save …, data` assembles each column once as an Arrow
    array and COPYs from a registered Arrow scan.** Measurement (2026-06-15)
    localised parqit's only remaining save deficit vs `pq` to the *write
    assembly*, not the SPI reads: numeric saves already matched/beat `pq`, but
    routing columns through a DuckDB temp table (appender → table storage →
    `COPY` re-scan) cost ~2× on the assembly, dominated by strings. The default
    writer now fills per-column buffers (numeric typed buffers + a validity
    bitmap; strings as Arrow utf8 offsets+bytes) via the same
    `convert_save_numeric` the staged path uses — so it is **byte-identical**
    (verified by an independent pyarrow oracle over %td/%tm/%tq/%tc/strings/
    labels/sysmiss/extended-missing/fractional-date, and by the full verify
    suite run under both paths) — then registers them with
    `duckdb_arrow_array_scan` and COPYs straight to Parquet. Result on 10M rows:
    mixed 13-col 6.6s→4.9s (now *faster* than `pq`), numeric 2.7s→1.7s,
    string-only 4.5s→3.2s; every case is faster than the old path (no
    regression). **`duckdb_arrow_array_scan` is marked deprecated in DuckDB**
    but is present and correct in the pinned 1.5.x; its behaviour is pinned by
    the always-on engine-capability test `tests/unit/test_arrow_copy_bench.cpp`
    (so a DuckDB upgrade that drops/changes it fails the build, never a user),
    and `PARQIT_SAVE_NOARROW=1` selects the staged temp-table fallback (kept,
    byte-identical) at run time. Full-range only — `save_data` never carries
    if/in. String columns whose on-disk payload would exceed 2 GiB (int32 Arrow
    offsets) error loudly rather than overflow.
49. **`parqit save` requires valid UTF-8 in string cells; invalid bytes are a
    loud per-cell error, never a silent corruption.** Arrow/DuckDB/Parquet
    VARCHAR must be valid UTF-8, but a Stata `str#`/`strL` can hold arbitrary
    bytes (Latin-1/legacy text from imports or `char()`; the binary-strL case is
    already rejected separately). Writing such bytes verbatim into a
    UTF-8-typed column produced a file no reader — parqit included — could decode
    on the Arrow path, and a silently nulled cell on the staged path (both
    `rc 0`, no warning; an adversarial-audit finding, 2026-06-16). Both writers
    now validate each cell with `parqit_is_valid_utf8` (strict well-formed UTF-8:
    rejects overlong forms, surrogates, code points > U+10FFFF — the same
    boundary as the engine's `utf8_lossy` walker) and fail with `kRcUsage` at the
    offending `var[obs]`, directing the user to `unicode translate`. Chosen over
    lossy U+FFFD sanitisation because the latter would *destroy* recoverable text
    (`é`→`�`) whereas the loud error routes the user to a correct transcoding;
    it also keeps the two write paths consistent and the metadata path (labels,
    serialised separately) is unaffected. Conservative fallback if too strict for
    some workflow: switch to lossy-with-warning, reusing `utf8_lossy` and the
    existing `_parqit_lossy_notes` plumbing. Verify test `v32_invalid_utf8_save`.

50. **Residual-hazard fixes from the 2026-06-23 multi-agent adversarial audit.**
    Decisions taken where the brief was silent or where Stata fidelity was the
    deciding factor (all locked by `v33_audit_fixes_20260623` against native
    Stata / pyarrow oracles):
    - **`gen <byte|int|long|float>` coerces the value like native Stata
      (EXPR-1).** Verified against Stata 19.5: integer targets *truncate toward
      zero* (`3.9`→`3`, `-2.5`→`-2`, not round-half) and an out-of-range value is
      *system missing* (`gen byte = 200`→`.`, `=101`→`.`; byte data range is
      −127..100). `View::gen` wraps the value in
      `CASE WHEN trunc(v) ∉ [min,max] THEN NULL ELSE CAST(trunc(v) AS <int>) END`
      (and `CAST(v AS FLOAT)` for float), which also sizes the collected column
      to the requested type instead of widening to double. Applied to `gen`
      only (the documented storage-request entry point), not `replace` (which
      keeps the column's existing type and re-sizes at collect). Period/date
      formats are never attached by `gen`, so the coercion never re-truncates a
      day/period count.
    - **Default SQL missing-comparison semantics are unchanged (EXPR-2/EXPR-3).**
      The brief fixes "default to SQL semantics; `statamissing on` emulates
      Stata". So `keep if x > c` and `gen y = x > c` keep their SQL-NULL outcome
      for missing `x` by default; only the *help text* was corrected (it had
      claimed the SQL default "coincides with Stata" — true for `<`,`<=`,`==`,
      false for `>`,`>=`,`!=`). Changing the default was rejected as a silent
      public-semantics change (AGENTS.md non-regression rule); `statamissing on`
      already reproduces Stata in both filters and assignments.
    - **Internal literal reads are glob-escaped (GLOB-1).** Only parqit's own
      self-reads of a known-literal path (the save verify, the unchanged-source
      fast-path source re-read) are escaped; user-facing `parqit use` keeps glob
      semantics, so `parqit use "y*.parquet"` still expands as before.
    - **Atomic replace via rename-aside (ATOM-PART-1 / IO-2).** Both the
      partitioned-tree replace and the Windows flat-file replace move the old
      target aside and delete it only after the new one is in place (restoring on
      failure), so a crash never leaves neither. POSIX flat-file replace is still
      a single atomic `rename` (#47c superseded for the partitioned case).
    - **`collapse (first)/(last)` and `merge m:m` master pairing fall back to a
      total order over all columns when no/partial sort is present
      (COLLAPSE-3 / TT-A1)** — reproducible for fixed inputs, at a small extra
      ORDER BY cost only on those paths. Reproducing a *specific* native-Stata
      physical order still needs an explicit `parqit sort` (documented).
    - **Weights are rejected, not implemented (COLLAPSE-WEIGHTS).** `collapse`
      with `[fweight=…]`/`[aweight=…]`/… is a clear "not supported" error rather
      than a mis-parse; implementing weighted aggregates is left for a later
      feature pass (no precision loss — the path never produced a result).
    - **Deferred (loud/safe today, low reward vs risk):** lazy `use`→`collect`
      does not yet record a sanitised foreign column's original name in
      `char[src_name]` the way the eager `use, clear` path does (INJID-2 — the
      data and types load correctly; only the recovery characteristic is absent
      on the lazy path); `summarize, detail` still scans once per variable
      (PERF-DETAIL-KSCAN — a CTE rewrite risks changing a returned scalar, so it
      is gated on a measured A/B with full re-test). Both are tracked for a
      follow-up.

51. **Residual-hazard fixes from the 2026-06-23 third audit round (post-Codex).**
    Decisions where Stata fidelity or cross-tool consistency was the deciding
    factor (all locked by `v35_audit_fixes_20260623b`):
    - **`gen str#` truncates to the declared byte width (STR-GENWIDTH-1),** the
      string analog of #50's numeric `gen byte/int/long` coercion, via the
      byte-indexed `parqit_substr_bytes`. The common case (ASCII, or a multibyte
      char not split at the boundary) is byte-exact with native Stata. A codepoint
      split exactly at the str# byte boundary yields U+FFFD (and the column may be
      one codepoint wider) rather than Stata's raw partial byte, because the engine
      keeps valid UTF-8 — consistent with parqit's `substr()` (#44) and the save
      UTF-8 requirement (#49). Applied to `gen` only (the documented storage-
      request entry point), not `replace`.
    - **Grouping/join keys fold ""/NaN to Stata-missing everywhere (GROUPKEY-1,
      TT-MM-MISSING-1).** The `merge`/`joinby` join already normalized keys
      (#45); the within-key windows + spine of `merge m:m`, and the GROUP
      BY/PARTITION BY of `collapse`/`contract`/`duplicates drop`/`egen , by()`,
      now use the same idiom (string `nullif(k,'')`, numeric
      `CASE WHEN isnan(CAST(k AS DOUBLE)) THEN NULL`). This matters only for
      FOREIGN files that mix missing encodings in one key column (a NULL and a
      NaN, or a "" and a NULL); parqit-written files are single-encoding (#34) so
      behaviour is unchanged, and the per-row scalar cost is the same one already
      paid on the merge path. Reshape i()/j() grouping was left as-is this round
      (it was just restructured for leading-zero suffixes; lower incremental risk
      to defer).
    - **`parqit save` refuses a partitioned `replace` whose destination contains
      (or is contained by) the open view's glob/directory source (SAVE-SELFGLOB-1)**
      — the IO-1 guard previously skipped glob sources and could delete the source
      tree. Internal literal self-reads are glob-escaped (#50 GLOB-1); the
      user-facing `parqit use` keeps glob semantics.
    - **`parqit set threads` parses strictly** (whole-token digits, 1..INT32),
      turning a silent truncation / raw DuckDB INTERNAL assertion into a clear
      error (SET-THREADS-1/2). **`parqit set tempdir` warns (does not block) on a
      non-existent directory** (SET-TEMPDIR-1) — the user may create it before the
      first spill, so erroring was rejected as too strict.
    - **Metadata restore never fails the load:** a foreign `parqit.dtalabel`
      over Stata's 80-char limit is truncated best-effort rather than aborting
      `use`/`collect` with r(133) (DTALABEL-LEN-1) — consistent with the
      best-effort metadata-restore posture.
    - **`strpos(s,"")` -> 0** (Stata), not DuckDB's 1 (STRPOS-EMPTY-1);
      **`length()` on a numeric is a clear error naming `length()`**
      (LENGTH-NUMERIC-1) — numeric (format-aware) `length()` is not implemented in
      the translator (no per-variable format there); use `parqit sql`.
    - **Performance:** two-way `parqit tabulate` derives its distinct-column count
      from the already-materialised, cell-bounded GROUP BY result instead of a
      separate `count(DISTINCT)` scan (PERF-TAB2-PRECOUNT-1) — one pass not two,
      output unchanged. This offsets the per-row group-key normalisation above.
    - **Follow-up (v0.1.10):** the no-by `collapse` over zero rows no longer
      fabricates a row — it emits zero observations via `HAVING count(*) > 0`
      (COLLAPSE-EMPTY-1), at zero added cost and consistent with the `by()` case
      (native Stata r(2000) is an error; zero rows is the non-corrupting analog).
    - **RESAVE-STALE-SRCNAME-1 evaluated and intentionally NOT applied.** Dropping
      `[src_name]` on save (to avoid a provenance char that can go stale after an
      explicit rename) would lose the original (foreign) column-name recovery on a
      parqit->parqit round trip — a precision/feature loss the maintainer's
      constraints forbid. The characteristic is kept; the staleness is a niche,
      rename-only cosmetic and not worth the trade-off.
    - **Still deferred (loud/safe today):** strL save return codes stay unchecked
      (#47a class); the sorted-array percentile list is rebuilt per percentile
      (PERF-PCTILE-REBUILD-1) and `summarize, detail` scans once per variable
      (PERF-DETAIL-KSCAN) — both gated on a measured A/B since a CTE rewrite could
      change a returned scalar; `reshape long`/`wide` i()/j() grouping does not yet
      fold ''/NaN keys (GROUPKEY-1 was applied to collapse/contract/duplicates/egen
      but reshape was just restructured for leading-zero suffixes, so its key
      folding is left for a focused follow-up).

52. **Residual-hazard fixes from the 2026-06-24 fourth adversarial audit round
    (v0.1.11).** Every claim was checked against a native Stata oracle before any
    change — the audit ran statically and its runtime predictions were unverified.
    - **Lazy boundary normalisation (PQ-AUD-001/002).** `boundary_for()` now maps
      a foreign FLOAT/DOUBLE `NaN`/`±Inf`/`|x| ≥ SV_missval` to NULL and folds a
      VARCHAR/ENUM/UUID `NULL` to `""`, the same guards the eager fill and direct
      save already used. Lazy views therefore agree with the eager `use, clear`
      path on missingness, order, stats, dedup and saved payloads. A column's
      values are now computed expressions rather than raw Parquet columns, so a
      `MISS-1` provenance flag (`ViewCol.normalized`, set only at the boundary,
      dropped by any recomputing verb) lets `missing()` and lazy `save` skip the
      redundant guard on already-clean columns — keeping the common path at
      baseline while a gen/replace/aggregate result (which *can* hold a generated
      special) still gets the full finite check. `duplicates drop` with no varlist
      (PQ-AUD-006) is fixed for free by this normalisation: `SELECT DISTINCT` over
      the now-folded columns collapses `NULL`-vs-`""` and `NaN`-vs-`NULL` exactly
      like native Stata, so no `row_number()` rewrite (and no perf regression) was
      needed.
    - **`egen` storage = value semantics (PQ-AUD-004)** and **`gen` type-family
      checking (PQ-AUD-005).** `egen` with an explicit narrow numeric type now runs
      `coerce_storage()` (out-of-range → missing, native-verified) and rejects a
      string storage type; `gen` rejects a storage type whose family disagrees with
      the expression (native r(109)). Both were metadata-only before.
    - **Date/time literal validation (PQ-AUD-007).** `parse_dmy()` validates month
      length and leap years and `parse_hms()` bounds the second at `< 60`, so
      `td(31feb2020)`, `td(29feb2019)`, `tc(... 00:00:60)` fail loudly (native
      r(198)) instead of rolling forward. A `tc()`/`tC()` 60th second is rejected
      even though native `%tC` accepts a *true* leap-second instant: parqit stores
      `%tC` as the same count as `%tc` (no leap-second table — item #14), so a `:60`
      here could only be silently mis-converted, and a loud error is the safe match.
    - **PQ-AUD-003 evaluated and intentionally NOT applied (false positive).** The
      audit wanted lazy `replace` to coerce into the *existing* narrow storage type
      (byte `replace b = 200` → `.`, str3 `replace s = "abcdef"` → `"abc"`). Native
      Stata `replace` does the opposite — it **auto-promotes** the storage type to
      fit the value (byte→int keeping 200, str3→str6 keeping `"abcdef"`, int→long,
      long→double; verified on Stata 16+). parqit already reproduces that promotion
      via the collect-time `apply_meta_type()` range-widening, so adopting the
      audit's "fix" would have *introduced* a value/precision regression. A
      regression guard in `v37_audit_fixes_20260623d` pins the promotion behaviour
      so it cannot be "corrected" away later.

53. **Two-directional data-integrity audit (2026-06-24).** A 9-dimension source
    audit plus an empirical pyarrow/duckdb round-trip campaign confirmed parqit is
    exactly faithful both ways (foreign Parquet → Stata, and Stata → Parquet),
    within the documented type contract, and that the only value losses are Stata's
    own limits (no int64 type → >2^53 rounds to double; one Parquet missing concept
    → extended `.a`–`.z` collapse to `.`), each announced with a loud `note:`.
    - **DT-001 fixed:** the `%tc` save range guard rejected only `ms > 9.22…e15`,
      but that ms literal rounds up one ulp to the double `9223372036854776.0`,
      so a `%tc` value at the int64-microsecond ceiling (a year ~294,247 date)
      passed the guard and `llround(ms·1000)` reached `2^63` (UB → `INT64_MIN`),
      written with `rc 0`. Both save paths (`plugin_io.cpp` fill and staged) now
      bound the microsecond product directly against ±`2^63` (`0x1p63`), which is
      exactly representable and also excludes the `INT64_MIN` sentinel. The
      sibling `%tC` guard already used a clean `2^53` power-of-two literal and was
      not affected. Pinned by `v38_xtool_fidelity`; real dates are unaffected.
