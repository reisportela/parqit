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
    the v06/v15/v18 verify tests). A writer that emits WRONG statistics
    (spec-violating; also misleads DuckDB's own predicate pushdown) can
    under-size the Stata type — and under-sizing is NOT benign: SF_vstore
    silently maps the out-of-range value to missing (this was mis-assessed as
    "caught by the round-trip oracle tests" — those all use honest stats, so
    they never exercised it). The fill now bounds every value against its
    planned type's window and refuses the load loudly on any overflow (v49,
    NUM1/IO1 [[63]]), so a lying-stats file fails cleanly instead of silently
    corrupting; the metadata fast path itself is unchanged.
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
    **Current status (2026-08-08):** this paragraph is historical. The fill path
    checks every `SF_vstore`/`SF_sstore` return; #77 makes flat and partitioned
    publication transactional; `v29` pins lazy conversion notes; and #62 carries
    sanitised-name provenance through lazy collect/save; #87 closes the Arrow
    offset ceiling. None of the items listed in this historical paragraph
    remains a current constraint.

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
    if/in. If a string column outgrows regular Arrow's signed-int32 offsets, the
    assembler stops before narrowing an offset and automatically re-runs the
    byte-identical chunked staged writer (#87).
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
    **Superseded on 2026-08-22 by #94:** the save path now transcodes legacy
    8-bit text (cells and metadata) instead of refusing it.

50. **Residual-hazard fixes from the 2026-06-23 multi-agent adversarial audit.**
    Decisions taken where the brief was silent or where Stata fidelity was the
    deciding factor (all locked by `v33_audit_fixes_20260623` against native
    Stata / pyarrow oracles):
    - **`gen <byte|int|long|float>` coerces the value like native Stata
      (EXPR-1).** Verified against Stata 19.5: integer targets *truncate toward
      zero* (`3.9`→`3`, `-2.5`→`-2`, not round-half) and an out-of-range value is
      *system missing* (`gen byte = 200`→`.`, `=101`→`.`; byte data range is
      −127..100). `View::gen` wraps the value in
      `CASE WHEN trunc(v) ∉ [min,max] THEN NULL ELSE CAST(trunc(v) AS <int>) END`.
      Float targets similarly map finite values outside ±1.70e38 to NULL before
      `CAST(v AS FLOAT)`. This also sizes the collected column to the requested
      type instead of widening to double. Applied to `gen`
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
    - **Historical deferrals:** lazy original-name provenance (INJID-2) was
      deferred here and later closed by #62. The PERF-DETAIL-KSCAN concern was
      measured and closed without a rewrite in #51: the current per-variable
      parallel sort beat the proposed combined aggregate.

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
    - **Historical correction (reverified 2026-08-08):** the earlier claim
      **`strpos(s,"")` -> 0** was false for a non-empty haystack. Live
      StataNow 19.5 returns 1 when `s != ""` and 0 only when `s == ""`;
      DuckDB's unconditional 1 therefore also needed a guarded translation
      (STRPOS-EMPTY-2).
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
    - **Performance deferrals closed without a rewrite (2026-08-08):** the pinned
      DuckDB 1.5.3 physical plan common-subexpression-eliminates repeated
      `list_sort(list(x))` percentile expressions into one aggregate and one sort,
      so PERF-PCTILE-REBUILD-1 was not real. On a generated 5M x 4 Parquet input
      at eight threads, three warm runs of the current complete
      `summarize, detail` took 1.044/1.024/1.028 s; `quantile_disc` took
      1.804/1.888/1.842 s for the order statistics alone. PERF-DETAIL-KSCAN is
      therefore the faster measured design, not actionable debt. The strL
      return-code gap is closed by #86; the reshape missing-key deferral by #69.

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

54. **Expressions compute in double; untyped `gen` results store `double`
    (2026-07-02, INF-1).** Stata's expression evaluator is all-double, and the
    audit showed DuckDB-typed arithmetic diverging twice: `INT32+INT32` near
    2^31 aborts the whole query (Stata: 4e9), and double overflow produced a
    live `+Inf` that passed `< .` filters and poisoned aggregates (Stata: `.`).
    All arithmetic producers now cast operands to DOUBLE and route the result
    through the `parqit_finite` scalar. Consequence: an *untyped*
    `parqit gen z = a + 1` over integer columns collects/saves as `double`
    (previously the narrowest integer type; native Stata's own untyped `gen`
    default is `float` — no engine matches native storage here, values are
    identical). Users who want narrow storage type their `gen`, as in native
    Stata. Conservative fallback: a typed `gen` (`parqit gen byte z = …`)
    still coerces exactly as before.
55. **`||`/`&&` now rejected (2026-07-02).** Native Stata expressions reject
    them (r(198)); parqit had silently accepted them as `|`/`&`. Accepting a
    private dialect invites scripts that break under native Stata, against
    the "Stata's vocabulary" thesis. Any existing parqit script using them
    gets a loud, anchored error naming the fix.
56. **`regexm()` stays on RE2, documented (2026-07-02).** Stata's own regex
    engine treats `\d`/`\w`/`{n,m}`/non-greedy as literals; RE2 honours them.
    Reimplementing Stata's engine is out of scope for v1; the dialect
    difference is documented in the help (patterns restricted to POSIX
    classes and `* + ? . [] ^ $` behave identically). Conservative fallback
    if this bites users: translate-time rejection of patterns containing
    backslash escapes.
57. **`reshape wide` with both `stub1` and `stub01` present keeps parqit's
    current pairing (2026-07-02).** Native Stata accepts that layout (rc 0,
    verified live); the v34 leading-zero fix pins parqit's choice (`stub1`
    is the j=1 payload, `stub01` carried as data). Not observed to diverge
    on payload; revisit only with a concrete native counterexample.
58. **Glob wildcards restricted to `*` and `?` (2026-07-02, GLOB-2).** DuckDB
    globs also honour `[...]` classes, but a bracket is far more likely to be
    a literal byte of a Stata user's filename (`data[1].parquet` download
    copies) than an intentional character class — and an unescaped class
    silently read a *different* file with rc 0, the worst failure mode in the
    charter. `[` is now always literal; `*`/`?` remain live in non-existing
    paths. Conservative fallback if a user genuinely needs classes:
    `parqit sql` with read_parquet and a raw pattern.
59. **`summarize, detail` order statistics via session-scoped scratch tables
    (2026-07-02, PERF-DET-1).** The per-variable sorted projection lives in
    `__parqit_sumdet_src`/`__parqit_sumdet_srt` TEMP tables (dropped before
    create and after use). The name is fixed, not `fresh_helper`-generated:
    the tables live in parqit's embedded session catalog, which only
    `parqit sql` could also touch; a user table with that exact name would be
    dropped. Documented trade-off — the `__parqit_` prefix is reserved across
    the project (helpers, spill dirs), and `parqit sql` users are already
    warned off the prefix by convention. rowid-on-CTAS = insertion order =
    ORDER BY order relies on DuckDB's default preserve_insertion_order, which
    parqit never changes (same dependency as `keep in`).
60. **`parqit pivot` is defined as `collapse` + `reshape wide`, atomically
    (2026-07-02).** The brief has no pivot-table verb; Excel's semantics
    (rows × columns × aggregated values) decompose exactly into two verbs
    the suite already trusts, so pivot compiles to those two stages rather
    than a third aggregation path (`parqit show` shows both — honest and
    pedagogical). Consequences accepted as design: the default statistic is
    `mean` (collapse's default, not Excel's `sum` — the dialog always writes
    the statistic explicitly, defaulting to sum there, so clicks match
    Excel); a missing `cols()` value errors loudly like native
    `reshape wide` r(498) (Excel's "(blank)" column would silently invent a
    column name — a `missing` option can be added additively later); column
    names are `tgt`+`value` under reshape's valid-Stata-name contract
    (negative or decimal cols() values error rather than sanitise
    silently); the 2000-distinct-values cap and numeric-vs-string j
    ordering are shared with reshape via one helper (`wide_j_scan`), so the
    v34 pinning covers both verbs. The plugin snapshots the View (a value
    type) and restores it on any failure: a refused spread can never leave
    the collapse stage half-applied.
61. **Strict-mode glob schema gate refines physical differences via resolved
    schemas (2026-07-03, SCH1/SCH2).** The `.sthlp` has always promised that
    without `relaxed` a schema mismatch across matched files is a loud error;
    the implementation inherited DuckDB's first-file-schema-wins cast instead
    (silent down-cast of a widened column; silent drop of an extra one). The
    gate fingerprints leaf schemas from `parquet_schema` (footer-only, one
    query, order- and case-insensitive) and, only when fingerprints differ,
    DESCRIBEs one representative file per fingerprint: files that resolve to
    the same DuckDB schema (INT96 vs TIMESTAMP_US legacy mixes, converted- vs
    logical-type annotation styles) proceed; a real resolved difference
    refuses with the column, both types and both files named. Case-only name
    differences continue to merge (DuckDB matches parquet names
    case-insensitively; erroring would newly break previously-working reads —
    documented as SCH6, LOW). Cost: nothing for a single literal file; one
    footer query per multi-file read; k tiny DESCRIBEs only in the rare
    mixed-fingerprint case. Fallback if a legitimate mixed layout must load:
    `relaxed` (unchanged, and its widening was verified correct).
62. **Lazy original-name provenance travels via view chars, not a ViewCol
    field (2026-07-03, F8).** The Codex audit proposed adding
    `ViewCol.origin` and writing `parqit.schema src = origin`; rejected
    because `src` must equal the *written file's* physical column name for
    the reload to bind its metadata (the physical columns of a view save are
    the sanitised names), so an origin-valued `src` would orphan every
    label/format on reload. Instead `view_open` records
    `chars[name]["src_name"] = original` — the same characteristic the eager
    loader sets — and the existing chars plumbing (collect decoration,
    view-save `parqit.chars`, META-2 re-keying on rename) carries it
    everywhere. One asymmetry accepted: a view save writes the *sanitised*
    name as the physical parquet column (unchanged behaviour); the original
    is recoverable from `parqit.chars`, not from the column name itself.
63. **The numeric fill bounds every value against its planned Stata type
    (2026-07-03, NUM1/IO1 + T2).** `fill_column` computes the storable window
    of the planned byte/int/long/float type and counts any value outside it;
    a nonzero count refuses the whole load with a loud rc (the staged swap
    keeps memory intact). This is the mirror of the pre-existing float
    inf/sentinel guard, extended to integers and dates. It fires only when a
    plan is wrong: honest integer/float stats always size a type that fits
    (never triggers), so the live triggers are (a) a spec-violating file
    whose footer stats understate the data — the same file also defeats
    DuckDB's predicate pushdown, so there is no lazy-filter escape, only a
    stats rewrite — and (b) a DATE beyond Stata's %td long window (dates never
    range-refine). %tc/%tw/… periods and timestamps store as double and are
    not windowed (no false positives). Cost: one comparison per stored cell.
64. **A NUL byte in a parquet column name is refused, not carried
    (2026-07-03, NM1).** The SPI's column-name path is C-string throughout
    (`duckdb_column_name`, `duckdb_value_varchar` over `parquet_schema`), so
    `"col\0hidden"` truncated to `"col"`, collided with a real sibling, and
    the fetch SELECT bound one physical column twice — silent data loss +
    duplication at rc 0. The name cannot survive downstream, so the source
    gate refuses it on every surface (relaxed included), naming the column
    with the NUL rendered `<NUL>`. DuckDB VARCHARs are length-counted, so the
    footer probe (`contains(name, chr(0))`) sees it even though the C API
    would not. Documented non-goal: parqit will not invent a surrogate name
    for a NUL-bearing column (that is the eager path's src_name role, and the
    collision makes even that ambiguous).
65. **Foreign display formats are applied through a capture (2026-07-03,
    META-A).** Every restored metadatum has a warn-and-skip guard except the
    display format, which went straight to st_varformat (abort rc 3300 on a
    format Stata rejects). It now goes through `_stata("format ...", 1)` — a
    non-aborting capture that returns the rc — after the format string is
    screened for command metacharacters (backtick, quote, dollar, control
    bytes), which a legitimate format never contains and which also blocks
    injection through the apply. An unscreenable/rejected format is skipped
    with a note; the load and all other metadata survive. Non-goal: parqit
    does not attempt to repair a bad format, only to not die on it.
66. **Response parsing and value-label restore are O(n) (2026-07-03,
    META-D).** `_parqit_resp_lines` uses select(); `_parqit_resp_decorate`
    preallocates the value-label vectors to the line-count bound and
    index-assigns. The prior `x = x \ row` per line/entry was O(n^2) and hung
    on a large-but-legitimate label. The upper-bound preallocation
    (line count) is trimmed to the accepted-entry count before st_vlmodify.
67. **Precision notes and the sub-ms note ride SF_error at fetch, not the ado
    printf (2026-07-03, NUM2 + T1).** The per-column ColumnPlan.note and a new
    data-driven sub-millisecond-truncation counter are emitted by
    cmd_use_fetch via SF_error, alongside the inf/NUL/range notes, so they
    survive `quietly` (the ado's warn printf did not). write_var_records no
    longer emits the per-column note as a 'warn' response record; general
    structural warnings (ctx.warnings) keep the record/printf path. The sub-ms
    note is data-driven (counts real truncations) and gated by
    ColumnPlan.note_subms so it fires only for us-resolution TIMESTAMP/TIME
    that lack a static NS/precision note — never a blanket note on a ms-exact
    microsecond column, and never a double note on the NS path.
68. **Fractional temporal saves use native Stata's integer-unit rule on every
    path (2026-07-09, TEMPORAL-ROUND-1).** A Stata date, datetime or period is
    an integer count even when stored in a double. If a fractional value reaches
    `parqit save`, both the Arrow writer, its staged fallback and lazy
    `compile_for_save` apply `floor(x + 0.5)` — native `round(x)`, including
    exact negative half ties toward +infinity — and issue the existing
    fractional-value note. `%tc` is rounded to an integer millisecond before it
    is encoded as a microsecond timestamp; it no longer preserves a fractional
    millisecond on one path while the other path rounds it.
69. **Every user-visible group key applies Stata missing equivalence after a
    two-table verb (2026-07-09, RESHAPE-MISSKEY-1 / STATS-MISSKEY-1).** Append,
    merge and reshape can introduce SQL NULL beside an empty string (or NaN)
    that represents the same Stata missing value. Reshape uniqueness/grouping,
    tabulate, duplicates report/list and tabstat now use the same
    `nullif(k,'')` / NaN-to-NULL key as collapse, contract, egen and joins.
    `codebook`/`distinct` exclude that canonical missing from unique counts;
    `tabstat, by()` excludes a missing by-group, as native Stata does. This
    closes the reshape deferral recorded in #51.
70. **All string operands of translated functions obey NULL == empty-string
    semantics (2026-07-09, REGEXM-NULL-1).** In particular, `regexm(subject,
    pattern)` coalesces both operands. A missing column introduced by append is
    the Stata string `""`; an empty regular expression matches, rather than
    returning SQL NULL because only the subject was normalised.
71. **A filtered Stata test run that selects zero tests is an error, and PASS
    is not final if Stata aborts afterwards (2026-07-09,
    HARNESS-NOMATCH-1 / HARNESS-ABORT-1).** `tests/run_stata.sh <fragment>`
    returns rc 2 with a clear message if no test basename matches. For every
    selected log it also rejects an uncaptured terminal `r(#);` occurring after
    the last verdict; captured negative-path errors remain valid because the
    test continues to a later verdict. An empty summary or stale early PASS can
    never be mistaken for a green gate; a CTest shell case pins both attacks.
72. **`merge m:m` preserves the clamped sequential reuse rule, not native
    physical within-key order (2026-07-09, MM-ORDER-1; supersedes the exactness
    wording in #22/#51).** A lazy plan does not retain a stable physical row id
    for both input relations. parqit therefore applies a deterministic total
    value order before the sequential spine. Row counts and repeated-last-row
    mechanics match Stata, but paired non-key payloads can differ from a native
    `merge m:m` on unsorted inputs. The help/README state this limitation and
    recommend `joinby`. Preserving native physical order would require a new
    source-row-identity contract across every plan stage; that architectural,
    correctness-sensitive change is deferred rather than guessed.
73. **Every test scratch artifact is run-owned (2026-07-09,
    TEST-TMP-OWNERSHIP-1).** Independent CTest jobs, local agents, repeated Stata
    suites, or explicit stress runs may execute concurrently and may reuse an OS
    process/temp-name prefix. Fixed unit filenames under `/tmp`/`%TEMP%` let one
    process truncate another's oracle; a directory made beside Stata `tempfile`
    is not auto-removed and can break a later run. Writable C++ unit paths use the
    platform temp directory plus process id. The Stata runner supplies and removes
    a private TMPDIR for every selected test, while `t02` also cleans its directory
    fixture for safe direct runs. Concurrent-unit and direct-fixture repros plus
    the runner CTest shell case pin these invariants. Literal temp paths embedded
    only as request payload examples remain literal by design.
74. **Persistent adapter and `open _data` bridges are atomically reserved and
    registry-owned (2026-07-14, BRIDGE-XPROC-1 / BRIDGE-LIFETIME-1;
    supersedes the naming and global-sweep details in #31/#41).** StataNow may
    expose empty `c(pid)` and `c(processid)`, so the plugin creates a private
    directory using the real OS PID, an operation counter and 128 random bits;
    atomic directory creation is the final collision arbiter. A path starts
    pending, can be claimed only if it is an input of the successful operation,
    and is reference-counted across every view whose compiled plan depends on
    it. Failed operations discard their own pending paths, replacement/close
    releases the old plan, and `close _all` sweeps only paths proven by the
    plugin registry to be package-owned. Unknown user paths are never deletion
    candidates. The `x01_bridge_xproc` licensed-Stata gate pins the cross-process
    contract with one shared temp directory containing spaces and Unicode.
75. **Public lazy `merge m:m` is refused before side effects (2026-07-14,
    MM-ORDER-1; supersedes #22/#51/#72 for the public lazy command).** Native
    sequential pairing depends on each input's physical within-key order, which
    the lazy plan does not preserve. The stable rc is 198, and refusal occurs
    before resolving/importing the using side or mutating the current view.
    `parqit joinby` is the Cartesian alternative; `parqit mergein m:m` remains
    the deliberate native-order escape hatch and is tested against Stata.
76. **The release upload source is the CMake-maintained distribution surface
    (2026-07-14, DIST-STRIP-1).** CI collects `ado/plus/p/parqit.plugin`, not the
    raw build-tree target. The exact collected file is then checked per platform:
    Linux is ELF64, stripped of ordinary symbol/debug sections, exports the two
    Stata entry points and has no runtime `libstdc++`/`libgcc_s`; macOS verifies
    Mach-O plus exports after `strip -x`; Windows compiles the embedded DuckDB
    with DLL export annotations disabled, applies the two-entry module-definition
    file, and verifies PE/COFF plus the exact export set. These are packaging
    checks, not claims of cross-platform Stata runtime coverage.
77. **Every final output transaction proves ownership before cleanup
    (2026-07-14, REL-001 / OUTPUT-XPROC-1).** A save atomically reserves the
    sibling directory `<dest>.parqit_lock` and creates a same-filesystem staging
    directory from the real PID, a process counter and 128 random bits. It may
    recursively clean only that random directory; a pre-existing lock or any
    historical `.parqit_tmp`/`.parqit_old` object blocks or survives the save.
    A crash can intentionally leave a stale lock that requires explicit human
    removal: fail-closed is preferable to guessing ownership and deleting a
    live writer's state. `x02_output_xproc` pins exclusive publication with two
    real Stata processes and equal-sized competing payloads.
78. **Exact foreign numerics remain exact until the Stata boundary
    (2026-07-14, DATA-002 / DATA-003 / TYPE-007).** `UBIGINT`, 128-bit integer
    and DECIMAL keys stay in their physical DuckDB type through lazy verbs and
    Parquet-to-Parquet save. Collect is the only operation that must enter
    Stata's binary64 universe and retains the existing precision note. Numeric
    expression tokens are first canonicalized through one locale-independent
    binary64 parse, so `2^53+1` cannot acquire a precision that Stata never had;
    explicit and untyped-double results are physically `DOUBLE` on every save
    path.
79. **A declared `parqit.*` metadata channel is restored as one validated unit
    (2026-07-14, META-010/011/012/013).** Every file matched by a Parquet input
    participates in equality, including files without any parqit keys. A
    difference, malformed JSON or invalid top-level shape produces a warning
    and skips the full channel, never a mixture of trustworthy and untrustworthy
    fields. Duplicate physical-name provenance is positional. `sortedby` is a
    valid ascending prefix only: direct/lazy saves persist it, projection may
    truncate it, `gsort` does not claim it, and loads stably sort before marking
    the data so native `by:` can trust the restored state.
80. **Replace metadata never forces a value back into stale narrow storage
    (2026-07-14, TYPE-007 and the PQ-AUD-003 non-regression).** Replaced integer
    and string columns clear their old width/type intent and are re-sized from
    the materialized result, preserving native byte→int, int→long and str#
    promotion. A replaced FLOAT retains physical FLOAT only when the bind-probed
    result family (integer or FLOAT) is range-safe; a DOUBLE/DECIMAL/wider result
    is conservatively promoted to DOUBLE before save. Existing DOUBLE remains
    DOUBLE. The selected FLOAT/DOUBLE cast is inserted into the lazy plan at the
    `replace` commit boundary, so subsequent expressions see the stored value,
    not a transient inferred integer/decimal. This keeps values lossless and
    prevents Parquet physical type from contradicting `parqit.schema`.
81. **Deterministic fault hooks are inert, bounded and fail-only
    (2026-07-14, LIFE-018 / OUTPUT-XPROC-1).** The release binary recognises
    `PARQIT_TEST_FAIL_THREAD_AT`, `PARQIT_TEST_HOLD_OUTPUT_LOCK_MS`,
    `PARQIT_TEST_FAIL_OUTPUT_PUBLISH` and
    `PARQIT_TEST_FAIL_OUTPUT_ROLLBACK` solely to drive otherwise unreachable
    lifecycle and recovery regressions. They do not publish unverified data or
    bypass validation: the first injects a normal worker-construction error,
    the second only delays while the real lock is held (capped at 30 seconds),
    and the final pair force loud publication/rollback failures. A double
    failure deliberately retains the prior target under the recovery path named
    in the error. With the variables absent (the production default), none of
    these paths executes.
82. **README/help identity framing is documentation-only (2026-07-14,
    maintainer direction).** The README additions — "How parqit thinks — the
    lazy view", "First contact with a large file", the "Explore before you
    load" bullet and the "Explore the view" verb-grammar table — plus the
    matching help-file Description/lazy-view paragraphs and the "Exploring a
    view" viewer jump, document behaviour that already exists and is covered
    by the Stata suites (t08/t09 explore, t05 power, tour §exploration). No
    command syntax, option, default or semantics changed; the public surface
    is additive per the non-regression rule. Framing parqit as "explore
    first, load last" (a fast first pass over large data, not a plain
    reader) follows the maintainer's 2026-07-14 direction.
83. **The basics guide is additive executable documentation (2026-07-14,
    maintainer direction).** `examples/parqit_basics.do` teaches the four base
    operations (use/save/merge/append) in both philosophies — eager
    ("pq-style", data in memory first) and lazy (view + verbs + materialise) —
    using only parqit itself (no pq dependency, no Python; data generated in
    pure Stata). It asserts each lazy result against a native-Stata oracle
    (`cf _all` for exact copies, merge + `reldif < 1e-12` for aggregates,
    the tour's pattern) and is pinned by `tests/integration/t14_basics.do`.
    The README/help pointers to it are documentation-only; no command
    surface changed. The untracked `examples/pq_to_parqit_common_workflows.do`
    (which requires the `pq` package) remains a separate migration/parity
    script; its comments were translated to English (2026-07-15, house rule:
    researcher-facing text is English — code unchanged) and it was re-run
    green against the installed `pq`.
84. **Root reorganization is content-preserving (2026-07-15, maintainer
    direction: tidy for online sharing).** The audit, certification, parity
    and audit-prompt documents plus the external verification kit moved
    verbatim (git renames, no content edits to historical reports) from the
    repository root to `docs/audits/`, indexed by `docs/audits/README.md`;
    `parqit_clean_demo.do` moved to `examples/`. References were updated in
    `README.md`, `CLAUDE.md`, `tests/integration/t10_audit_fixes.do` and
    `.gitignore` (the audit-kit scratch pattern now points at the new path).
    Working material that was never tracked — the 2026-07-14 holistic-audit
    draft, agent implementation prompts, `scratch_inj/`, stray root logs,
    the audit bundle zip, and `examples/parqit_dlg.do` (which embeds a
    private data path and must never be committed; the path-leak gate would
    reject it) — now lives in the new git-ignored `local/` folder.
    `release_lint.sh` remains green: the path-leak gate scans code files
    only, so the relocated `.md` evidence stays exempt, and no version/date
    surface moved.

85. **Independent 2026-08-08 semantic audit decisions (F1-F6).** Every
    behavioural claim was first reproduced against live StataNow/MP 19.5 and
    then pinned by `v61`-`v65` plus focused audit reproducers.
    - `strpos(s,"")`, quoted `" in "` inside `list if`, explicit-float overflow
      and wildcard projection were confirmed defects and fixed at their common
      translation/planning boundary. Bare `list` now applies its 20-row default
      as a query limit rather than fabricating `in 1/20`, which native Stata
      rejects when the view has fewer than 20 rows.
    - Extended-missing identity is irrecoverable after a Parquet boundary.
      Lazy `.a`-`.z` literals are therefore a loud, atomic error; silently
      treating every category as ordinary `.` was rejected as false fidelity.
    - Varlist `?` means one Unicode codepoint, not one UTF-8 byte. Eager and
      lazy reads, lazy projections and the `mergein`/`appendin` projection
      bridges share ordered, deduplicated expansion over exposed Stata names.
    - F6's proposed all-column fallback sort was not applied. No runtime
      divergence was established; ordering every column can impose a large
      CPU/memory cost, fails to recover Stata's discarded physical tie order
      and silently changes the query contract. The honest contract is that a
      tied slice is unspecified unless the user declares a unique sort key;
      README/help now say so.
    - DuckDB 1.5.3's vendored source defines its default memory ceiling as 80%
      of available system memory. Shared-host guidance therefore recommends an
      explicit `parqit set memory_limit` without changing the engine default.

86. **Every `SF_strldata()` read is checked before a Stata strL is published
    (2026-08-08, STRL-RC-1).** Despite its `ST_retcode` spelling in Stata's
    public prototype, the documented result is the number of bytes copied and
    `-1` on error; treating any nonzero result as failure would reject every
    non-empty strL. Both the default Arrow writer and
    `PARQIT_SAVE_NOARROW=1` staged fallback now reject a negative
    `SF_sdatalen()` and require the copied byte count to equal that reported
    length, with variable, observation, copied and expected counts in the
    diagnostic. `v19_strl_boundary` invalidates the unchanged-source fast path
    and verifies a 1-MiB strL plus a multibyte boundary through both writers
    against pyarrow.

87. **String columns above regular Arrow's 2-GiB offset ceiling retry through
    the staged writer (2026-08-08, ARROW-OFFSET-FALLBACK-1).** The fast in-memory
    writer keeps its compact int32 offsets for ordinary workloads. If cumulative
    bytes would exceed `INT32_MAX`, it stops before the narrowing conversion and,
    before any output transaction exists, `cmd_save_data` clears attempt-local
    warnings and re-reads the data through the existing chunked DuckDB appender.
    `v19_strl_boundary` lowers the boundary with a test-only environment hook and
    simultaneously blocks Arrow registration: success plus the pyarrow payload
    oracle therefore proves that the automatic staged retry actually ran.

88. **`_n`/`_N` are refused, not implemented, in the read-only stats and
    preview filters (2026-08-08, ROWCTX-1).** Only the view compiler resolves
    the `__PARQIT_ROW__`/`__PARQIT_NROWS__` placeholders, and it does so where
    a plan STAGE is appended (`keep if`/`drop if`, `View::gen`). `count if` and
    the `list`/`head` preview instead apply their filter to an already-compiled
    SELECT, so the placeholder used to reach DuckDB and return as a raw
    `Binder Error` naming an internal token. Implementing them there
    (wrapping the compiled SELECT the way `rowctx_wrap` does) is a legitimate
    future enhancement, deliberately NOT taken now: it would widen a public
    contract that `parqit.sthlp` §Expressions, `v66_help_contract` and
    `v67_runtime_message_contract` currently pin as unavailable. `ExprResult`
    carries a new `uses_rowctx` flag so both call sites refuse precisely, with
    parqit's own message and rc 198, instead of leaking engine internals. If
    the enhancement is ever taken, those three surfaces must change together.

89. **`quietly` suppresses neither native error text nor the plugin's
    `SF_error` output (2026-08-08, BRIDGE-QUIET-1) — verified, not assumed.**
    Required before quieting the package-owned bridge import, which previously
    printed `import`/`use` chatter and the temporary bridge path on every
    non-Parquet `using` side. Measured in `stata-mp` 19.5: `capture noisily
    quietly use <missing>.dta` still prints `file … not found` (rc 601);
    `capture noisily quietly parqit merge 1:1 <bad key> …` still prints the
    plugin's `SF_error` text (rc 920), including inside `capture noisily {
    quietly { … } }`. The real path was then confirmed end to end: a corrupted
    `.dta` on the `using` side fails with rc 610 and a visible `file … not
    Stata format`, while a successful CSV bridge prints nothing at all.
    `v67_runtime_message_contract` pins both halves so a future `quietly` can
    never trade loudness for tidiness silently.

90. **`ty(yyyy)` is a documented parqit extension, not native Stata syntax
    (2026-08-09, TY-EXT-1).** Reproduced in StataNow MP 19.5:
    `capture noisily display ty(2026)` returns r(133), "unknown function
    ty()"; the native `%ty` value is the bare integer year. parqit has long
    accepted `ty(2026)` and returns 2026, which is the correct period count.
    Removing it would shrink an already published expression surface without
    fixing a value error, so the conservative decision is to retain it and
    mark it explicitly as an extension in the help. The rejected alternative
    is to refuse `ty()` and direct users to a bare year; that would require a
    separately authorised surface change in the translator, tests, help and
    changelog.

91. **Every `reshape` name used by a validation query is checked against the
    live manifest first (2026-08-09, RESHAPE-NAME-1).** A 51-case test-first
    sweep found one root defect with four manifestations: `reshape long` with
    an unknown first or later `i()` variable, and `reshape wide` with an
    unknown `i()` or `j()` variable, entered the eager uniqueness/missingness
    scans before `View::reshape_long()`/`reshape_wide()` could validate the
    names. DuckDB therefore returned rc 920 and exposed a `Binder Error`, the
    generated query and `__parqit_s0`. The plugin now checks those query inputs
    against `g_view_ref().cols()` first and returns Stata's variable-not-found
    rc 111 with a parqit-owned message; the live view is unchanged. A final
    native probe also showed that an unmatched long or wide stub is rc 111,
    whereas parqit returned a clean but late rc 198 after validation work; stub
    presence is therefore preflighted by the same guard. Validation remains
    duplicated inside the engine as a defence for mutation paths that do not
    use these scans. The sweep retains all 51 already-clean cases and a positive
    control proving that a new `rename` destination is not incorrectly treated
    as an unknown input variable.

92. **A `merge`/`joinby` key type conflict returns native rc 106, while an
    absent key name remains rc 111 (2026-08-09, JOINKEY-RC-1).** Reproduced in
    StataNow MP 19.5 with both native `merge` and native `joinby`: a numeric
    master key paired with a string using key returns rc 106; a missing name
    returns rc 111. The earlier parqit preflight correctly prevented raw
    DuckDB errors but collapsed both cases to rc 111. `View::join_keys_error`
    remains the single source of the diagnostic and now optionally classifies
    a type mismatch for the plugin, which returns rc 106 without duplicating
    the name/type checks. Engine-only callers retain the same message contract,
    and every refusal remains atomic.

93. **Apple-Silicon GUI and console Stata require separate package selectors
    for the same arm64 plugin (2026-08-09, PKG-MAC-CONSOLE-1).** Stata's local
    `usersite.sthlp` lists `MACARM64` for GUI and `OSX.ARM64` for console
    sessions. The release already builds and publishes one arm64 Mach-O binary,
    `parqit_macarm64.plugin`, which is valid for both launch modes; the package
    manifest previously selected it only for `MACARM64`. A console `net install`
    would therefore reach `h parqit.plugin` without having installed the
    required target. The manifest now maps both platform names to the same
    source and destination, and release lint requires the two mappings to stay
    present and identical. No Intel selector is added because this release does
    not build an Intel-macOS plugin.

94. **`parqit save` transcodes legacy 8-bit text instead of refusing it
    (2026-08-22, ENC-2; supersedes #49).** Live finding: a 30 GB Stata-14
    panel whose merged `_EMP_QP` variable labels are raw Latin-1 (`Regi\xe3o`,
    `Econ\xf3mica`) failed with `internal error: [json.exception.type_error.316]
    invalid UTF-8 byte` (rc 920) — the KV-metadata JSON serialiser threw on
    the label — and the documented per-cell refusal would have stopped any
    dataset with legacy string cells and sent the user to `unicode translate`.
    The maintainer's requirement: parqit must handle non-UTF-8 files with no
    `unicode translate` step. Decision: both writers validate every string cell
    and every metadata item (variable/data labels, value-label names and texts,
    notes, characteristics) with the strict UTF-8 check and transcode the
    invalid ones from a declared single-byte code page
    (`engine/legacy_encoding`), item by item — the `unicode translate`
    default, which also leaves strings that are already valid UTF-8 alone.
    Default `windows-1252` (identical to ISO-8859-1 for the accented letters,
    printable in 0x80–0x9F where Latin-1 has C1 controls; the WHATWG/browser
    convention and Stata's own Windows default); `encoding(latin1|latin9|
    macroman)` otherwise (tables generated from Python's codecs; the five
    undefined cp1252 bytes map WHATWG-style to C1 so every mapping is total
    and reversible). Chosen over lossy U+FFFD replacement (destroys
    recoverable text) and over keeping the refusal (the requirement). Loudness
    is kept: a `note:` with counts, `r(transcoded_cells/meta/vars/encoding)`;
    `str#` widths follow the widest transcoded cell (`unicode translate`
    widens too) and past 2045 bytes the recorded type becomes strL — the KV
    metadata is therefore built after the data pass. Known limitation, shared
    with `unicode translate`: a legacy string that happens to be well-formed
    UTF-8 cannot be told apart. The read side (ENC1, v52) is unchanged: a
    foreign file with invalid UTF-8 payload still refuses loudly. Unit test
    `test_legacy_encoding`; verify `v32` (rewritten) and `v38` block E.

95. **Variable names that differ only by case are exact at both boundaries
    (2026-08-22, NAME-CASE-1).** Stata keeps `nuemp` and `NUEMP` apart; DuckDB
    identifiers are case-insensitive even when quoted: `COPY … TO` dedups such
    output names in the binder (`bind_copy.cpp`,
    `QueryResult::DeduplicateColumns` — `NUEMP` becomes `NUEMP_1` in the
    written file), `read_parquet` dedups them in the scan, and a CTE silently
    binds a reference to whichever it dedups first (`WITH s AS (SELECT 1 AS
    nuemp, 2 AS NUEMP) SELECT "NUEMP" FROM s` → 1). Live finding on the same
    panel (five such pairs): `parqit save` wrote `NUEMP_1`, the `parqit.*`
    manifest no longer matched that column (label/format silently lost on
    read-back), and reading the pq-written file loaded `NUEMP_1`. Decisions:
    (a) the engine keeps ONE name per view column (`ViewCol.name` — the SQL
    identifier and the name lazy verbs and expressions use; none of the ~100
    SQL emission sites change) plus a Stata-facing `ViewCol.stata` set only
    for a case-clashing column; open, `collect`, eager `use`, `describe`,
    view save and the KV manifest expose that name; (b) the written file gets
    its exact names back by re-serialising the Parquet footer
    (`engine/parquet_footer`: Thrift compact protocol, SchemaElement.name and
    every ColumnChunk path_in_schema, positionally, flat schemas only — the
    only shape parqit writes — with a byte-identical no-op self-check before
    touching the file and a re-read of the names afterwards), chosen over
    writing the alias into the file with a manifest mapping because third-party
    readers (pq/Polars/pandas) must see the same names Stata has; (c) engine
    aliases come from `engine_unique_ci` (deterministic `_k` suffixes dodging
    every name, every alias and — for a using side — the master's names; a
    using column with the same Stata name as a master column shares the
    master's engine name so keys resolve on both sides); (d) creating a lazy
    name that differs only by case from a live one is refused loudly in `gen`,
    `egen`, `rename`, `collapse` targets, `contract` freq, `merge`/`append`/
    `joinby` (gen and brought using columns) and `reshape` — closing the silent
    mis-bind hazard that existed before; (e) `partition_by()` is refused for
    such datasets (a Hive tree would expose the alias in directory names);
    (f) the direct (source-copy) save path falls back to the general path for
    them. Unit test `test_name_case`; verify `v70`.

96. **The unchanged-source copy save is an explicit opt-in, not an automatic
    fast path (2026-08-22, COPYSOURCE-1; audit A4-1/A4-2/A1-2; supersedes the
    automatic path of #20c).** The 2026-06-23 fast path let `parqit save` copy
    the Parquet file loaded by a prior `parqit use ..., clear` whenever
    `c(changed)==0`, the load nonce matched and a size+mtime fingerprint held.
    The adversarial audit showed this is unsound: Stata does NOT set
    `c(changed)` for `sort`/`gsort`, `order`, `tsset`, `label drop`, nor for
    Mata `st_store`/`st_sstore`/`st_view` writes, so the file written could
    differ from memory in row order or values (rc 0), and the manifest then
    claimed a `sortedby` the rows did not have; and a size+mtime fingerprint is
    not content-sensitive (a same-size in-place rewrite with a restored mtime —
    `cp -p`, `rsync -a`, `tar -x`, or a TOCTOU writer — slipped through).
    Decision (rigor over performance, capability preserved): the default
    `parqit save` ALWAYS reads the dataset in memory (the general writer). The
    copy is retained only under an explicit `parqit save ..., data copysource`
    (the user asserts nothing changed) and every check is a loud refusal, never
    a silent fallback: (a) the dataset must be the one loaded by the last
    `parqit use ..., clear` of a single Parquet file (the
    `_dta[_parqit_fast_source_nonce]` characteristic ties it) and unchanged
    (`c(changed)==0`, `c(filename)==""`); (b) the source's full identity — abs
    path, size, mtime, POSIX `st_ctim` (which `utime` cannot restore),
    dev/inode and an FNV-1a digest of the Parquet footer bytes — must match the
    load-time identity, re-checked immediately BEFORE and (via a pre-publish
    hook) AFTER the COPY; (c) the in-memory variable names/kinds must equal the
    file's columns, `_N` the file's row count, `: sortedby` the file's
    manifest sortedby, and the first/last 64 observations of every variable
    must equal the file's (catching a sort, a gsort and Mata edits that touch
    either end — an edit confined to the middle rows is NOT detected: a full
    compare would scan the whole file and memory, the general writer's cost,
    and for an opt-in where the user vouches for the data the sampled check is
    the documented contract; round 2, V2.1);
    (d) datasets the copy cannot reproduce are refused with the remedy
    (case-distinct names, a variable renamed from the file's column, a `%tc`
    variable needing the instant conversion, a binary strL); (e) the KV
    `sortedby` the copy writes is the SOURCE file's own sortedby claim, copied
    as is (the copy is byte-faithful to the source, so a claim the source
    carries travels with it; parqit re-sorts on load, so memory's `: sortedby`
    equals that claim and the copy cannot know better without the full scan),
    and `r(copysource)` reports the file. Chosen over dropping the capability
    (some workflows re-emit an untouched file and the copy is much faster) and
    over any cheap automatic criterion (none is rigorous). Verify `v72`,
    integration `t11` (both rewritten); the load-time identity is recorded by
    `_parqit_use` into `PARQIT_FAST_SOURCE_*` globals.

97. **The lazy date-function domain is a single clean window
    (2026-08-22, DATE-DOMAIN-1; audit A3-3/A3-4).** `year/month/day/quarter/dow/
    doy/mofd/yofd/mdy/dofm` return system missing outside Stata's calendar
    domain — day counts `-679350..2936549` (01jan0100..31dec9999), `mdy` year
    `100..9999`, `dofm` month count `-22320..96479` — and never abort the query
    (the whole expression is `try()`-wrapped and the out-of-range branch is
    NULL). This is one uniform, documented rule chosen over matching every
    native quirk exactly: native `dow` and `doy` in fact extend one day past
    31dec9999 (`dow(2936550)=6`) before going missing, an astronomical edge
    parqit deliberately does not reproduce — the clean `[01jan0100,31dec9999]`
    window is simpler and safe for the entire realistic range. Verify `v75`.

98. **A float/double/`%tc` partition key is cast back from its Hive directory
    text to the recorded type (2026-08-22, HIVE-TYPE-1; audit A1-3).** DuckDB's
    `hive_partitioning` only autocasts integer- and date-looking directory
    values, so a `float`/`double` key (`"2020.0"`) or a `%tc` key (a timestamp
    string) arrives as VARCHAR. When `parqit.schema` records a numeric/`%tc`
    type for such a text-typed key, parqit casts it back (float keys to FLOAT so
    the storage type round-trips, matching the eager and lazy paths); a key
    DuckDB already read as DATE/integer is left as scanned. A zero-observation
    `partition_by()` save writes an explicit empty tree (the directory plus one
    0-row file carrying the full schema) so a later read returns 0 observations
    with every variable rather than a raw engine IO error (A1-5). Verify `v74`.

99. **Exact-name recovery in relaxed unions, the Hive key case clash, empty
    names and float `%tc` (2026-08-22 round 2; RELAXED-NAMES-1 / HIVE-CLASH-1 /
    A2-15(1) / FLOAT-EXACT-1).** (a) DuckDB's `union_by_name` lists the first
    file's reader-deduped columns, then every later file's new names, matched
    ASCII-case-insensitively (`UnionByName::CombineUnionTypes`); the Parquet
    reader dedups case-insensitive duplicates inside one file with a running
    `_<counter>` suffix (`parquet_reader.cpp ParseSchemaRecursive`); the binder
    names an empty column `C<index>`; `HivePartitioning::Parse` takes the
    `key=value` directory components with exactly one `=`. parqit carries
    line-by-line replicas of these rules (`engine/sanitize`, unit-tested
    against the fetched v1.5.3 source) and PREDICTS the scan names, then maps
    them back to the true leaf names — chosen over DESCRIBE-ing every file (N
    footer reads) and over the old one-file heuristic, which never aligned a
    union with a differing schema. (b) The union's case-insensitive matching is
    an engine hazard parqit cannot undo: a later file's `NUEMP` flowing into an
    earlier `nuemp` is accepted with a note (the first file's name wins — the
    user asked for a union by name); a true name the union would split across
    TWO columns (one file with both `nuemp` and `NUEMP`, another with `NUEMP`
    only) is refused, because the data would be wired into the wrong variables.
    (c) A Hive key that is ci-equal but not equal to a file column is refused on
    every path (the engine overrides the file column in place,
    `StringUtil::CIEquals`); an exactly equal key is read with a note
    (consistent writers put the same values in both; the directory value wins,
    as in every engine). Active for a directory source and for a glob the
    engine auto-detects as Hive (every file shares one key set), replicated
    from `AutoDetectHivePartitioningInternal`. (d) An empty leaf name becomes
    `v<position>` with a note and no `src_name` char. (e) A manifest `float`
    held in a non-FLOAT engine column (a `%tc` TIMESTAMP, the lazy ms count, a
    period INTEGER, a cast Hive key) is restored to float only when a scan
    proves every value float32-exact, double otherwise — rigor over the old
    "restore when the integer range fits" rule, which could not see a `%tc`'s
    ms range at all; the lazy collect hands the view's carried type and format
    to the planner so the scan runs in-plan on both paths, and overlays the
    view's metadata by engine name rather than position. Verify `v70`, `v74`;
    unit `DUCKDB-DEDUP-1`, `DUCKDB-UNION-1`, `DUCKDB-HIVE-1`, `FLOAT-EXACT-1`.
100. **The point-and-click surface follows StataCorp's own dialog guidelines
    and menu idioms rather than a parqit-specific style (2026-08-24; audited
    and remediated 2026-08-25).** The
    brief asks for a package that looks like it came from the same hand as
    Stata itself; for dialogs the reference is [P] Dialog programming,
    Appendix C (interface guidelines) and the shipped data-management
    dialogs (`use_option.dlg`, `describe.dlg`, `merge.dlg`, `append.dlg`,
    `collapse.dlg`, `import_parquet.dlg`, ...). Decisions recorded here so
    they are not relitigated: (a) one submenu **User > parqit** (not the
    built-in `stUserData`/`stUserStatistics` submenus, which would split a
    single session's workflow across three menus and bury it a level deeper)
    whose entries are task phrases in the wording of Stata's Data/File menus,
    grouped by separators in session order — read, describe/explore, change,
    combine, materialise, manage — and ending with direct commands
    (Version, Self-test, Help), as official menus do; (b) dialog titles in
    the official "command - Description" form; labels with a trailing colon
    when they name the control below, right-aligned when they sit left of a
    small field, always given the full column width (Appendix C: never let a
    label truncate); every dialog keeps the family's single geometry policy
    (`_std_wide`, release-lint enforced) and is sized to its content;
    (c) a radio group holds at most seven choices — beyond that, a
    `LISTBOX` with `onselchangelist` (explore: 15 operations; views: 10
    actions), with `forceselchange`/`POSTINIT` dispatch so a remembered
    selection submitted with OK/Submit reopens with the right inputs enabled
    (Cancel discards changes); (d) variable pickers
    are editable `append dropdown` comboboxes filled on demand by a
    **Populate** button — the `use`/`describe`/`merge` idiom — through the
    internal `parqit _dlgvars <dlg> <list> [using <source>] [, data]` helper,
    which reads the current view (`parqit ds`), Stata's current dataset for a
    write with `data`, or a Parquet footer (`parqit describe`, so a
    CSV/.dta/Excel source cannot be populated, by design: populating would
    mean opening it); it clears the list before every fill, never silently
    caps wide schemas, validates class/list names defensively and signals
    failure through the dialog's `pq_populate_error` property (the official
    `main_des_error` pattern); the fill is on demand, never at open, so no
    dialog needs `SYNCHRONOUS_ONLY` and opening a dialog never runs the plugin;
    (e) report buttons (Describe, Views, Show SQL, Explain, Variables,
    Version, Self-test) use the plain `stata` directive so the command is
    echoed to Results and Review — the help's reproducibility promise —
    while Populate alone stays `stata hidden immediate`; (f) validation in
    the official style: `require` on mandatory fields, `stopbox stop` for a
    pivot/collapse without a statistic and for a lazy `merge m:m` (the ado
    would refuse it anyway; the dialog says so before submitting), `repfile`
    on the save dialog's FILE control when `replace` is not ticked (the
    explicit box stays, because `repfile` cannot see a partition directory),
    every single-path FILE/spill control uses the documented `/smartquote`
    command construction,
    and `copysource` is enabled only with `data`; (g) the combine dialog takes
    an **Options** tab for the native merge options `mergein` forwards and
    the code page, the official Main/Options split, so the Main tab stays
    readable; (h) the help declares each dialog with `{viewerdialog}` (the
    Viewer's Dialog menu lists them) and documents the menu in a Menu
    section placed after Syntax, the official position, and states that
    StataNow's native `import parquet` (Stata 19.5, File > Import) is
    complementary — parqit's contribution is the lazy grammar, the writer and
    the metadata round-trip — without a `{vieweralsosee}` link to it, which
    would be a dead link on Stata 16–19.0; (i) `VERSION 16.0` stays on every
    dialog (the package baseline), so the Stata 19 `_frame_aware_pr` include
    and other newer idioms are deliberately not used. The dialog→ado
    contract is pinned by `tests/integration/t15_dialog_shapes.do`, which
    executes every command shape the dialogs can emit, and by
    `tests/dialog_lint.py`, which resolves controls, LIST triplets, option
    targets, smart-quoted paths and Populate-source invariants; the dialogs
    themselves are checked by opening each one in GUI Stata under Xvfb and
    driving its `PROGRAM command` through the dialog class instance
    (`.parqit_<name>_dlg.main.<control>.setvalue` + `.command`), since batch
    Stata cannot open a dialog; real clicks synthesised through the XTest
    extension (a 20-line C helper against `libXtst`) confirmed that Populate
    fills the picker from the Parquet footer, that OK builds and echoes the
    command to Results and Review, and that the combine Options tab renders.
    The harness lives in `local/gui_dialog_harness/` (git-ignored,
    per-machine). `window menu clear` is deliberately never called by parqit:
    Stata can remove only all packages' additions at once and exposes no
    package-local existence query, so that external command leaves the
    session flag stale; the documented recovery is `global PARQIT_MENU_ON`
    followed by `parqit menu`.
101. **Raw SQL accepts trailing statement terminators (2026-08-25).**
    `parqit sql` embeds one statement as a lazy subquery, where a terminal
    semicolon is syntactically invalid inside parentheses even though it is
    conventional at an interactive SQL prompt. After outer Stata quoting is
    removed, parqit trims one or more semicolons only from the end of the SQL;
    semicolons inside expressions or string literals are preserved. An input
    consisting only of terminators remains an empty-query error. Pinned by
    `tests/integration/t15_dialog_shapes.do`.
102. **Examples are the SSC crash courses; the oracle-checked tours are tests
    (2026-08-28, maintainer direction).** Kit Baum's SSC review asked for the
    example do-files named in the help. The two self-contained crash courses
    written for the submission replaced `examples/parqit_basics.do` and
    `examples/parqit_tour.do` (the staging copies in `ssc_submission/examples/`
    must stay byte-identical; `build_candidate.sh` refuses drift): they take no
    arguments, print no verdict, use artificial NLS-style data under
    `c(tmpdir)`, name the matching **User > parqit** dialog per block and never
    mention the comparison command. Their former self-verifying versions moved
    content-preserving into `tests/integration/t14_basics.do` and
    `t13_tour.do` (the wrappers that already ran them), so the regression
    contract is unchanged. Stata's `net` classifies package `.do` files as
    ancillary — delivered by `net get` / `ssc install, all`, never installed on
    the adopath — and the help's Examples paragraph says so. Since v0.1.30
    (2026-09-01) `parqit.pkg` lists them with `f` lines, the release workflow
    copies them beside the package files into every zip and into the loose
    release assets, and `release_lint.sh` verifies that each `f` file exists
    (installation files in `src/ado/p/`, `.do` files in `examples/`) and is
    copied by the workflow; the SSC candidate script's own two-line `.pkg`
    overlay is redundant from this tag on.
103. **Float-column comparisons are evaluated in double by typing the literal
    (2026-09-01, FLOAT-LIT-1, audit F2).** DuckDB binds `FLOAT <op> DECIMAL`
    by casting the literal to FLOAT, so `x == 0.1` was true for a float x —
    native Stata, an all-double evaluator, says false. The translator now
    emits `CAST(<lit> AS DOUBLE)` for a literal in a comparison (`relational()`,
    `inrange()`, and `round()`'s operands) when the literal is non-integral
    and not exactly representable in float32; the engine then widens the
    column (SQL rule) and the filter still pushes into the Parquet scan
    (verified with EXPLAIN). Integral literals are deliberately left untyped
    even beyond 2^24: typing them DOUBLE would change every integer-key
    filter's plan, and a float variable compared with such a literal is rare;
    the residual (`x == 16777217` on a float x) is documented in the help.
    A column-level `CAST(FLOAT AS DOUBLE)` at the boundary was rejected as
    the fix because it changes the collected storage type of foreign FLOAT
    columns and the physical type of lazy saves. FLOAT-vs-INTEGER column
    comparisons (no literal) still follow the engine's promotion (FLOAT).
104. **A string partition value the Hive reader maps to a missing partition is
    refused on save; foreign trees note it (2026-09-01, PART-STRKEY-1, audit
    F1).** DuckDB 1.5.3 writes a string value `NULL` to the directory `k=NULL`
    and a missing numeric key to `k=__HIVE_DEFAULT_PARTITION__`; its reader
    maps both tokens to SQL NULL, so the string value loaded as `""`. The
    audit plan proposed a generic source-vs-tree DISTINCT comparison of every
    key; that re-executes the whole pipeline for a lazy save, so the check is
    instead a walk of the staged tree's directory names for a VARCHAR key
    holding either token — exact, because parqit never writes a NULL string
    (the boundary and both writers fold it to ''), and free of any extra
    scan. Every other value was verified to round-trip (`v78`): the engine
    URL-encodes `=`, `/`, space, `%`, `\`, Unicode, and keeps `.`, `..`,
    `01` and the empty string (`k=`). The read-side note is emitted from
    `plan_columns` (eager) and forwarded by `cmd_view_open` (lazy).
105. **`describe` pairs engine types by name (2026-09-01, DESCRIBE-ALIGN-1,
    audit F3).** The `dtype` response records now carry the Stata name in
    the manifest order of the `var` records; `_parqit_resp_describe` looks the
    type up by name with a positional fallback only for a name it cannot
    find. The printed table and `r(type_i)` of flat files are byte-identical
    to before.
106. **CSV header names recovered through the sniffed dialect (2026-09-01,
    CSV-HEADER-1, audit F4).** `sniff_csv()` reports the dialect but its own
    column list is already deduplicated, and it reports an unset quote,
    escape or comment character as the literal text `(empty)` (passing that
    to `read_csv` fails with "The quote option cannot exceed a size of 1
    byte"). parqit therefore reads the header line as data
    (`read_csv(header = false, all_varchar = true, delim/quote/escape/skip/
    comment from the sniff)`) from the first matched file, aligns it
    positionally with the scan when the widths agree and `HasHeader` is true,
    and reuses the Parquet recovery (`parquet_names`), so the Stata names,
    `src_name` characteristics, case aliases and notes follow the same rules
    as Parquet. Any probe failure or width mismatch keeps the engine's names
    exactly as before (never a refusal); a headerless file keeps
    `column0…`. Only the first file of a glob is sniffed (strict mode proves
    one schema).
107. **`mod()` reproduces native's truncated remainder (2026-09-01,
    MOD-TRUNC-1, audit F7).** Stata's manual defines `mod(x,y) = x - y*floor(x/y)`,
    but the executable behaviour for a non-integer modulus is
    `r = x - y*trunc(x/y); r < 0 ? r + y : r` (native `mod(7, 0.00001)` is
    `9.99999999911182e-06`, exactly `-8.88e-16 + 1e-5`, while the manual's
    formula evaluated natively gives `-8.88e-16`). parqit emits that formula in
    double; DuckDB's `fmod()` is the floor form and was rejected. Every value
    verified against native: (7,1e-5) (1,0.1) (0.3,0.1) (5.5,2) (-5.5,2)
    (1e15+0.5,1) (-7,3) (7,3) (2.5,0.3) (10,1e-5); `y <= 0` stays missing.
108. **`%d` is a date format (2026-09-01, DFMT-1, audit F6).** Stata documents
    `%d` (and `%-d`, `%d` with display tokens) as the older synonym of `%td`.
    `classify_format` maps any `%d`/`%-d` not followed by a digit or `.` to
    `Td`, so both writers and the lazy boundary treat it as a day count and
    the file carries a `DATE`; the display format itself is restored verbatim.
    No numeric display format begins that way (`%9.2f`, `%-12.0g`, `%9,2f`).
109. **A string `(count)` target carries `%8.0g` (2026-09-01, COUNT-FMT-1,
    audit F5).** Native `collapse` keeps the source's display format on every
    target, count included (verified: `(count) n = price` keeps `%12.2f`), so
    parqit keeps doing that for numeric sources; for a string source — a
    parqit extension native refuses — the `%s` format is replaced by `%8.0g`
    rather than being rejected at collect time with a note.
110. **`drop in` numbers the rows instead of slicing (2026-09-01, DROP-IN-1,
    audit F10).** `keep in` is a `LIMIT/OFFSET` over the ordered pipeline;
    the complement keeps rows whose `row_number()` over the declared order
    (engine order when none is declared — the same caveat as `_n` and `keep
    in`, documented) lies outside `f..l`, and registers the same pending
    range so an out-of-range bound fails loudly at materialisation, like
    native's r(198). Native semantics reproduced: `2/3`, `3/l`, `-2/l`, `5`,
    `f/2`, `1/l`, reversed/zero bounds refused, composition with a prior
    `keep if` and with `_n`.
111. **Tabulate labels travel as response records (2026-09-01, TAB-LABEL-1,
    audit F12).** The plugin emits the tabulated variable's value-label
    entries (`tvl`, `tvl1`/`tvl2`) from the view's carried definitions, and
    the ado maps a numeric level to its label by numeric key comparison;
    `nolabel` (a new option) or an unlabelled level falls back to the
    formatted code. Counts, ordering and every `r()` result are unchanged.
112. **Duplicates-list cells are joined with the unit separator (2026-09-01,
    DUPLIST-SEP-1, audit F13)** — `\x1f` cannot occur in the hex-decoded text
    of a Stata string the way a TAB can.
113. **The Stata runner warns on a long temp root (2026-09-01,
    HARNESS-PATH-1, audit F15).** Batch Stata wraps output at `linesize`
    (255 in the tests), so a temp root beyond ~100 bytes pushes messages that
    quote a path past the wrap and log-grep assertions miss their phrase.
    `run_stata.sh` warns; `v67` compares blank-free as well, and `v70` undoes
    the wrap (`\n> `) before searching. The runner keeps `/tmp` by default.
114. **Percentiles are ranks, not lists (2026-09-01, PCT-WINDOW-1, audit
    F16).** `collapse (median/p##)` and `tabstat` built each group's sorted
    value list in memory (`list_sort(list(x))`); DuckDB cannot spill a list
    aggregate, so under a 1 GB `memory_limit` that query failed with an
    out-of-memory error at 200 million rows, with and without `by()`. The
    rank formulation — `row_number()` over the group's nonmissing values and
    `count()` per group, then Stata's rule on those ranks — completed under
    the same limit (63 s for one group, 35 s for 200,000 groups) and ran the
    20-million-row probe in 20 s instead of 24 s; the optimizer prunes the
    unused columns before the window (verified with EXPLAIN). One cost:
    without a memory limit the window materialisation used more memory than
    the list did on that probe (peak RSS of the whole Stata process 8.7 GB
    against 6.0 GB), but it is bounded by `memory_limit` (DuckDB's default is
    80% of RAM) and spills, which the list never did. The arithmetic is the
    same — `(x[np] + x[np+1])/2` for an integral `np`, `x[ceil(np)]`
    otherwise, NULL for an all-missing group; `summarize, detail` already used
    an order-based path and is unchanged. `v83` pins collapse against native
    `collapse` (`cf`) and tabstat against native `tabstat, save`.
115. **Numbers the plugin splices into engine SQL are typed DOUBLE, and the
    column beside them is cast (2026-09-02, DETAIL-DECIMAL-1).** DuckDB types
    a bare decimal literal `DECIMAL(p, scale)` and, in arithmetic with an
    INT8/16/32 column, converts the column to `DECIMAL(18, scale)`: a mean
    with a small integer part uses most of the 18 digits as scale, so any
    value at or above 10^(18−scale) fails the cast ("Could not cast value
    99999 to DECIMAL(18,14)"). `summarize, detail`'s second pass hit it on
    skewed identifier-like columns (`nuest`); `v44` never did because its
    uniform `long` has a 6-digit mean. A FLOAT column takes the opposite
    path (the literal is bound down to FLOAT, as in FLOAT-LIT-1). The detail
    moments now read `CAST(x AS DOUBLE) - CAST(<mean> AS DOUBLE)`;
    `CAST(<17-digit literal> AS DOUBLE)` was checked to yield the same
    double as the value that printed it. The translator already computes
    user arithmetic in double (INF-1, v0.1.14) and `histogram`'s
    `(x - lo) / width` binds DOUBLE because the division does, so neither
    was changed; a FLOAT column's `x - lo` there still binds in single
    precision when `lo` is fractional (bin edges only), noted for a later
    pass. Two ulp-level observations surfaced while pinning this (`v84`
    tolerates them at 1e-15 relative; `v44` always did at 1e-8): a
    percentile that averages two order statistics can differ from native by
    one ulp, since native does not round `(a+b)/2` the IEEE way; and
    `min`/`max`/`mean` travel as the engine's text rendering of the double,
    which is not always the shortest round-trip form (`0.057518312144544399`
    arrived as `…392`) — emitting them through `dtoa` would make them
    exact, also for a later pass.
116. **Partition modes swap leaf directories and demand a homogeneous tree
    (2026-09-02, PART-MODE-1, BPLIM request).** `partitions(replace|append)`
    stages the result as a whole tree (the existing COPY ... PARTITION_BY,
    verified by a scan and by the string-key check), then publishes it leaf
    by leaf under the transaction lock: the swap unit is the full key chain
    (`year=…/month=…`), never a top-level directory, so a sibling month is
    never touched; the old leaf is renamed aside first and restored on any
    failure, newest first, and a failed restore retains the aside copies
    under the transaction root and says so. Before anything is staged the
    destination is probed as a Hive tree over exactly the requested keys in
    order (every non-hidden entry at each level must be `key=` of that
    level's key; the first branch is descended to a sample file), and the
    sample file decides the contract: its non-key columns and engine types
    must equal the result's (a `DESCRIBE` of one staged file), and its
    `parqit.*` key–value pairs must equal the result's byte for byte — the
    same rule `plan_columns` applies when reading a glob, where any
    disagreement drops every label. A tree with no `parqit.*` keys is taken
    as written by another tool: the new partitions are written without the
    metadata fragment so the files keep agreeing (a note says so), rather
    than making the tree inconsistent or refusing. A tree whose files carry
    the partition key as a column is refused, because parqit writes keys
    only as directories and a union read would misalign. DuckDB's own
    `OVERWRITE_OR_IGNORE`/`APPEND` COPY modes were not used: they write
    straight into the destination and cannot be verified before publishing
    or rolled back. `replace` (whole tree) and `partitions()` are mutually
    exclusive by design; `copysource` copies one file and refuses the
    option. Open question left to the field: whether BPLIM keys its trees by
    month (replace) or stores months as files inside a year partition
    (append) — both are covered.
117. **User manual and technical reference are two help entries
    (2026-09-02, HELP-SPLIT-1).** Early institutional users found the single
    entry "too detailed to start from": it explained the machinery before
    the use. The split keeps every paragraph (a script moved sections and
    named paragraphs verbatim and a check counted them: 150 before, 0 lost)
    and puts the line where a reader's need changes: `parqit.sthlp` answers
    "how do I do X" (Quick start, verbs, materialisers, exploration, the one
    missing-value rule, examples, eight limitations, stored results);
    `parqit_technical.sthlp` answers "why is the result what it is" (Stata
    metadata in Parquet, input formats and bridges, verb-result metadata,
    atomicity/copysource/encoding/locks, performance tips, the expression
    dialect, type mapping and column names, environment variables, the
    complete limitations). Markers were preserved so existing links work:
    the dialogs' `parqit##menu`, the ado's performance tip (now
    `parqit_technical##perf`); moved sections keep their marker names in the
    technical file and cross-file links were rewritten. The function-list
    block stays in the user manual because the lint reads it there and users
    look for it there. `release_lint.sh` now checks the technical banner,
    the overclaim phrases and every `parqit_technical##` / `parqit##` link
    across the two files.
118. **SMCL help source lines stay under 160 bytes (2026-09-02, HELP-LINE-1).**
    Observation, not a documented Stata contract: the GUI Viewer of Stata
    19.5 for Linux truncates each source line of a help file at 245
    characters (probe files: a 248-char line displays its tail as `{p_en`, a
    254-char line loses its `{p_end}`; whether the unit is bytes or code
    points, and whether every Stata version and platform behaves the same,
    was not established — the ASCII probes cannot tell). `translate ...
    translator(smcl2txt|smcl2pdf)` has no such limit, and `help smcl`/`help
    limits` do not document one. The v0.1.30 and v0.1.31
    help carried two syntax lines over the limit (`parqit tabulate`, 265;
    `parqit save`, 255), so the Viewer rendered everything after them as one
    run-on paragraph with literal `{p_end}`/`{pstd}`. Decision: syntax lines
    are re-flowed inside their `{p 8 16 2}...{p_end}` paragraph (SMCL joins
    continuation lines with a space, so the rendering is unchanged), and
    `release_lint.sh` fails on any `src/ado/p/*.sthlp` line over 160 bytes
    (bytes, measured with `LC_ALL=C`, so UTF-8 counts conservatively; a wide
    margin under the observed 245, and both files already stay below 150),
    on any physical line whose braces do not balance (a directive cannot
    span lines), and on a help file that is not valid UTF-8 or LF-only; the
    SSC candidate build applies the same line gate to the staged help. The
    GUI Viewer, not `translate`, is the release check for help layout.
119. **Author order on the package surfaces (2026-09-03).** `parqit` is
    co-authored by Miguel Portela, Rute Costa, Paulo Guimarães and Marta
    Silva (the same four author its Stata Journal paper); the three BPLIM
    co-authors were added to every author surface. The paper lists its
    authors alphabetically; the package keeps Miguel Portela first, because the SSC
    listing, the `.pkg` support contact and the ado banner treat the first
    author as the maintainer and point of contact, and lists the co-authors
    alphabetically after him. `parqit.pkg` stays ASCII (Kit Baum's SSC
    tooling and RePEc metadata), so it spells "Guimaraes" there; every UTF-8
    surface (help, README, CITATION.cff, stata.toc, ado banner) carries the
    accent. The MIT copyright holder line was not changed: copyright is a
    legal statement the maintainer must make explicitly.
