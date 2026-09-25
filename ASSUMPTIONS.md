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
120. **An orientation map precedes the reference tables (2026-09-04).** The
    brief fixes the public command surface but says nothing about how the
    surface should be *presented*. Both `help parqit` and `README.md` now open
    their reference material with the same four-band map — open / shape / look
    / land — because users repeatedly asked what they could do with a view, a
    question neither the alphabetical syntax list nor the per-verb tables
    answer at a glance. The map is drawn with SMCL line characters
    (`{c TLC}`, `{c LT}`, `{c BLC}`, `{c |}`, `{c -}`) in the help so it
    renders as line-drawing in the Viewer and as ASCII in a console log, and
    as a fenced block in the README. It documents only commands that already
    exist: no command was added, removed or renamed, and the syntax section
    remains the normative surface — the map deliberately shows verb *names*
    without their options so it stays one screen. Verified in the GUI Viewer,
    which is the release check for help layout (see #118).

121. **Audit execution and column identity (2026-09-05).** An engine alias is
    not a stable cross-source identity. Named views retain their current alias
    map when embedded; combinations where equal engine names identify different
    exposed Stata names (or the same exposed name has incompatible aliases)
    are refused before mutation. Append also refuses unresolved case-only
    engine clashes across all sides. This preserves file-source alignment and
    already aligned case-distinct manifests without publishing wrong values.
    A name exposed by an existing aliased column remains occupied after its
    case sibling is dropped. V88 covers refusal, preserved state, named views,
    Parquet using inputs, compatible supersets and both materialisers.
122. **Statistical execution and precision (2026-09-05).** A pending slice
    contract applies to every statistics query, including count-if. Codebook
    and misstable cannot silently ignore nonexistent explicitly requested
    columns. Percentile interpolation and range/bin arithmetic run in double
    before narrow integer overflow can occur; collapse still rounds float-source
    percentiles to native's value. Storage can be wider than native collapse,
    and existing double mean/sd results are retained rather than reduced to
    float. V86/V87 and the native/pyarrow audit probes specify these boundaries.
123. **Documentation and evidence scope (2026-09-05).** Lazy describes the
    disk-backed plan, not zero memory, one physical query for the entire call,
    universal row-group pruning, or a cached snapshot. The memory writer may
    assemble full Arrow buffers; the existing PARQIT_SAVE_NOARROW environment
    switch selects batched staging. Partition metadata equality remains strict;
    no compatibility check was weakened. Single-file publication, handled-error
    rollback and multi-directory snapshot/crash guarantees are distinct. The
    audit used local Linux Stata, native and pyarrow oracles, the exact pinned
    1.5.3 engine for plan inspection, and two read-only Fable 5.1 max-effort
    reviews. Its 136 MB source / 16 MB engine-budget exercise establishes that
    bounded workflow, not a universal benchmark or cross-platform certification.
124. **Native exploratory output (2026-09-05).** The user requested consistent
    exploratory outputs aligned with native Stata, specifically summarize,
    detail, without expanding the catalogue of statistical procedures. The
    existing engine computations remain authoritative; display is formatted
    with Stata formats rather than substring truncation. Detail picks its four
    smallest/largest values from the existing percentile sort and takes labels
    from the view, not the current dataset. Tables and correlation panels
    follow observed native layouts. Codebook/misstable retain their compact
    documented information, including string missing and complete observations;
    no unsupported native fields are fabricated. Missing-pattern percentages
    use a window total before LIMIT 100. Tabstat's group-only result remains
    equivalent to native nototal; save returns those existing tables without
    an additional query. Correlation matrices use native names, while the
    existing N/rho scalar meanings are kept for compatibility. V89 compares
    native text layouts, result matrices, UTF-8 labels, degenerate samples,
    scientific notation and preserved dataset/view state. V83 now compares
    returned numeric matrices with its native oracle rather than parsing the
    former printed layout, with its numeric tolerance unchanged.
125. **Adversarial statistics follow-up (2026-09-05).** Non-finite numeric
    results are canonical Stata missing and never expressions that can bind
    to user variables/scalars. FLOAT extrema cross the response boundary after
    exact promotion to double. Successful statistics keep their built-in engine
    aggregates; only four pinned moment-finalizer errors permit one retry with
    fixed-size Welford/covariance states and NULL for non-finite final values.
    The fallback registry belongs to Session and is reset on close. Valid
    groups/columns survive an overflow elsewhere; arbitrary-range native bit
    parity is not claimed. No upstream lazy aggregate is silently rewritten.
126. **Statistical text and grammar (2026-09-05).** Long records are read as
    complete lines, with memory proportional to one record for streaming
    printers. Duplicate cells have individual hex fields. A VARCHAR in DuckDB's
    legacy C result is NUL-terminated even through its string getter, so text
    result columns use ENCODE/BLOB before that boundary; raw grouping keys stay
    unchanged. Numeric and string axes are sorted according to carried types.
    Exact statistical names resolve through the existing view name resolver;
    wildcard/qualifier expansion and automatic missing-pattern selection are
    deferred. Generate is a synonym, not a new transformation. Native matrix
    row names were verified; existing rho and empty-group conventions are
    preserved and their differences documented. V90/V91 pin the repaired
    numerical, text, grouping, alias, output and state contracts.
127. **Menu and dialog alignment (2026-09-06).** User authorized the proposed
    GUI revisions after the statistics/help audit. The ten task-oriented
    dialogs remain the surface; no all-in-one command or new statistical
    procedure is added. Tabulations have explicit row/column fields and
    nolabel; numeric computation pickers are filtered from the view schema.
    Context/source callbacks use `stata hidden queue`, not its implicit
    immediate default, because db can itself be running while the dialog
    initializes. Their command buffers are cleared and their private ado
    helpers preserve r(). Populate remains an explicit schema query and clears
    stale arrays even when it fails. No source rows are needed for context.
128. **Explicit GUI materialisation target (2026-09-06).** The write dialog
    starts with Parquet output and offers separate choices for a selected view,
    the dataset in Stata memory, and collection. View save/collect emit
    `parqit view name:` using the view shown in the context line, refreshed on
    opening, mode changes or Refresh. If the user later closes that view,
    the command must fail rather than fall through to memory-save semantics.
    CLI save defaults are unchanged. These GUI prefixes use the existing
    named-view command and keep the generated command reviewable.
129. **Numerical remediation (2026-09-06).** The user authorized all confirmed
    fixes after Codex's independent numerical audit and Fable co-review. No new
    public command or correctness opt-in mode is introduced. Returned summary
    statistics use stable, bounded states; means/sums preserve integer/decimal
    accumulation before conversion to Stata double. Native numerical failures
    are not acceptance oracles when exact/independent arithmetic is available.
130. **Storage and range (2026-09-06).** Exact integer/decimal SUM columns in a
    lazy table retain their existing engine types. Overflow of that exact
    result type remains a loud error; it is not converted to a silent missing
    or a globally rounded key. The existing typed `egen double ...=total(...)`
    requests a double result after wide accumulation. FLOAT combination and
    assignment semantics are resolved separately from display/storage metadata.
131. **Sampling and C API patches (2026-09-06).** DuckDB remains pinned at 1.5.3
    plus two reviewed source-hash patches in `cmake/`. SQL reservoir sampling
    uses uniform replacement from the start; only internal table-statistics
    sampling retains the prior approximation. The C aggregate bridge flattens
    constant state vectors before callbacks. An omitted/negative sample seed
    is chosen once in the lazy plan. Sources must remain stable during execution.
132. **Local validation surface (2026-09-06).** Builds during remediation use
    an isolated PARQIT_LOCAL_ADO_DIR under audit_repro. The habitual local ado
    tree is refreshed only after validation. Old audits, manifests, unrelated
    dirty changes, global ado/profile files and public release assets remain
    outside the implementation changes.
133. **Additional numerical audit (2026-09-06).** Supersedes the numerical
    implementation details in 125 and 129: production statistics no longer use
    the legacy overflow retry. Exact floating sums use 34 fixed integer limbs;
    integer/decimal totals and conversions round only after exact arithmetic.
    The previous fallback remains covered as legacy code. Dispersion/shape/rho
    now use exact power sums and certified rational/square-root rounding;
    approximate pivot classes are retained only as inactive reference code.
    Claude Fable 5.1 max reviewed the explicitly authorized snapshot read-only,
    with no time/turn cap; Codex reproduced, adjudicated and implemented fixes.
134. **Consistent exact-value semantics (2026-09-06).** The user's precision
    clarification takes precedence over reproducing native arithmetic defects.
    round/mod use the actual binary64 inputs, including decimal-looking ties.
    Bare wide integer/decimal comparisons are exact; mixed output coercions
    refuse unrepresentable values unless explicitly converted by the user.
    Percentiles, differences and histogram boundaries retain source precision.
    Percentage sampling replaces the engine's block percentage reservoir,
    whose zero-size block can crash and whose block rounding changes global N.
    A seeded priority sort over one captured input implements the globally
    rounded sample size lazily; its memory/scratch/time costs require measurement.
135. **Exact moments and significance (2026-09-06).** Additional Fable mathematical
    reviews confirmed the central-moment formulas and fixed limb bounds. Only
    Codex edits. Raw powers/cross-products are integer products; final algebraic
    statistics have one certified binary64 rounding. SD and shape do not infer
    constancy from a rounded variance. Correlation retains the independently
    rounded complement needed by significance, including when rho rounds to1.
    Tail probabilities use native beta/analytic forms and a bounded scaled
    series when Mata's exp path loses subnormal results. Their transcendental
    precision is measured separately, not described as universally correctly
    rounded. Exact states have a larger per-group footprint; performance and
    spill costs require explicit reporting. Numeric protocol3 checks both
    directions of ado/plugin compatibility before operation.
136. **Required OpenMP release builds (2026-09-07).** The user explicitly
    requested a version bump, commit, push and release with OpenMP enabled in
    every plugin build. Compile/link support is mandatory and a separate
    verifier loads the exact collected plugin and observes two workers.
    DuckDB retains its SQL scheduler; no audited numerical algorithm is moved
    into an OpenMP reduction. Linux and macOS embed PIC GNU libgomp 14.3.0,
    with the macOS plugin deployment target; macOS uses GCC 14. The system GNU
    static archive was rejected for executable-only TLS relocations. A pinned
    LLVM runtime then passed standalone CI-style probes but aborted inside
    Stata because Stata's private Intel runtime was already initialized. No
    duplicate-runtime override is allowed; rebuild GNU libgomp with PIC instead.
    Its internal TLS optimization is disabled in favor of the supported pthread
    key implementation: initial-exec TLS otherwise prevents dlopen after the
    statically embedded DuckDB increases the plugin's TLS footprint.
    MSVC requires its redistributable DLL, installed as parqit_vcomp140.dll
    beside the plugin and resolved by a delay-load hook. The selftest accepts
    a reduced team under explicit runtime limits; CI requires two workers.
137. **Release shutdown and transaction defaults (2026-09-07).** Linux CI
    exposed heap corruption after all assertions had passed. Memcheck traced
    it to DuckDB's BlockAllocator destructor reusing a thread-local cache after
    its destructor. Removing that access preserves ownership: cached entries
    are block IDs, the dead token prevents later queue access, and the pool is
    unmapped by the allocator. Cache initialization already resets stale IDs.
    Memcheck also found an uninitialized transaction invalidation policy;
    initialize it and auto_rollback to the standard policy/false defaults.
    Both pinned dependency edits are guarded by source hashes. A focused
    close/reopen/shutdown test must fail under Memcheck before the fixes and
    report zero errors afterwards; passing value assertions alone is insufficient.
138. **The Stata fill drains a STREAMED engine result, not a materialised one
    (2026-09-19, PERF-STREAM-1).** `cmd_use_fetch` — the single fetch behind
    `parqit use …, clear` and `parqit collect` — ran its SELECT through
    `duckdb_query`, which "stores the full (materialized) result"
    (duckdb.h:1209). The whole query therefore executed into one extra
    in-memory copy of the result *before* the first cell reached Stata, and
    the producer/consumer pipeline (#37) only overlapped the *walk* of that
    finished copy with the per-cell fill. It now runs through
    `Session::query_streaming`: `duckdb_prepare` (duckdb.h:1899) →
    `duckdb_pending_prepared_streaming` (duckdb.h:2315,
    capi/pending-c.cpp:41-44) → `duckdb_execute_pending` (duckdb.h:2375) →
    the same `duckdb_fetch_chunk` loops. The engine's chunks now reach the
    fill directly — no materialised collection, no per-chunk copy-out. With
    the default sizing (buffer ≥ the estimated result) the engine still
    completes its scan before the first fetch: a streaming execute returns
    only when the collector is blocked or the query has finished
    (parallel/executor.cpp:582-584), so the gain is the removed copies, not
    scan/fill overlap; overlap exists only in the explicitly capped mode, in
    stop-and-go cycles (auditor's verification, 2026-09-20). Verified facts
    and the rules that follow from them:
    - *Order and values are not at risk.* The collector is chosen by the same
      order predicates for streaming and materialised results — order-
      preserving + batch index → buffered-batch vs batch collector, otherwise
      buffered vs materialised
      (execution/operator/helper/physical_result_collector.cpp:25-51). All four
      build the result from the same `GetClientProperties()` snapshot, so the
      Arrow options now come from the result (duckdb.h:1281) instead of the
      connection — no call may be made on a connection whose stream is live.
    - *A NULL chunk is ambiguous.* `duckdb_fetch_chunk` returns NULL at
      end-of-stream **and** on an error, which it records on the result
      (capi/stream-c.cpp:17-37). The fill therefore reads
      `duckdb_result_error` (capi/result-c.cpp:526-532) after each loop and
      **before** `duckdb_destroy_result`: that is the streaming path's primary
      failure signal. Previously a mid-stream engine error could only be
      inferred from the row-count check, which named the wrong cause; that
      check stays as the secondary net. A failure raised before the first
      chunk is ready still surfaces from the execute call and takes the
      pre-existing error branch — both are loud.
    - *An abandoned stream must be cancelled now.* A result destroyed before
      end-of-stream leaves the executor with parked tasks and open Parquet
      handles until the next statement on the connection cleans up
      (`InitialCleanup` → `CleanupInternal` → `CancelTasks`,
      main/client_context.cpp:689-693, 311-318); a clean end-of-stream cleans
      up by itself (main/stream_query_result.cpp:82-85). The fill therefore
      tracks end-of-stream explicitly and, when it was not reached, runs one
      trivial statement right after the destroy. Without it an interrupted or
      failed read would hold a file handle open — on Windows that blocks a
      later replace of the very file being read.
    - *The buffer is sized per fetch, from the result.* `streaming_buffer_size`
      is a LOCAL (connection) setting defaulting to 1,000,000 bytes
      (main/client_config.hpp:83), split 60% read queue / 40% in-progress
      batches (main/buffered_data/batched_buffered_data.cpp:19-20). A producer
      whose share is full parks its sink, and the consumer unparks sinks only
      when the read queue has gone completely EMPTY
      (`BatchedBufferedData::ExecuteTaskInternal`,
      batched_buffered_data.cpp:127-140 → `UnblockSinks`, 42-60): **every time
      the cap is reached the parallel scan stops and restarts**, and below
      roughly one in-progress batch per scan thread the parallelism degrades
      too. Measured on the 58.8M×9 / 478-row-group reference file (min of 3,
      48 cores, same binary): 64 MB → 4.19 s, 256 MB → 3.67 s, 512 MB →
      2.36 s, 1024 MB → 1.55 s, 2048 MB → 1.33 s, against 1.70 s for the
      materialised fetch — i.e. a *fixed* 64 MB buffer would have been a 2.5×
      regression on exactly the read this change is meant to speed up. Hence
      the rule: **the buffer is normally at least the estimated size of the
      result**. `cmd_use_fetch` computes that estimate from the manifest with
      `typemap::estimate_transfer_bytes` (pure arithmetic, unit-tested: vector
      width per transfer type, 16-byte `duckdb_string_t` with 12 inlined bytes
      for strings, one validity bit per cell, 25% chunk-capacity slack, dropped
      columns free, saturating) and applies it with one
      `SET streaming_buffer_size = '<n>B'` *before* the stream exists —
      `byte`/`bytes`/`b` parse with multiplier 1
      (common/string_util.cpp:316-317) via `StreamingBufferSizeSetting::SetLocal`
      (main/settings/custom_settings.cpp:1600-1603). A 16 MB floor keeps tiny
      reads out of the park/unpark cycle. This costs no memory: the setting is
      a *cap*, not a reservation, and only chunks the engine actually produced
      occupy memory, so the peak is bounded by the result itself — never more
      than the materialised copy it replaces, and measured a little less
      (auditor, rep-1 RSS growth: 4.42 → 4.02 GB on the 58.8M×9 read, 6.67 →
      6.14 GB on keep+gen+collect, +0.25 GB on the string-heavy read, i.e.
      noise-level). It is not a large saving: with a buffer ≥ the result the
      whole result sits in chunks when the fetch starts. It follows that the
      estimate should err high, which is why a `strL` column (whose planned
      width is 0 by construction) is charged `kStataStrMax + 1`.
      `PARQIT_STREAM_BUFFER_MB=`{n} caps it explicitly — authoritative even
      below the floor, because a smaller buffer is a memory/time trade the user
      chooses — and `0` leaves the engine's 1 MB default (least memory, slowest
      fill-bound reads). DuckDB may still let the read queue overshoot (its own
      FIXME in
      execution/operator/helper/physical_buffered_batch_collector.cpp:66-68),
      so this is a soft cap and the real peak is measured, not assumed.
    - *Two regimes, and only one of them is free.* A **fill-bound** read (narrow
      numeric, many row groups, a scan far faster than the per-cell store — the
      58.8M×9 file) only beats the materialised fetch when the buffer is about
      the size of the result; a smaller cap buys memory at a real cost in time.
      A **scan-bound** read (strings, few row groups — the 7.9M×15 trades file,
      1 row group) keeps up with any buffer and is insensitive to it: measured
      64/256/1024 MB all within a ±0.3 s noise band on a loaded machine. Sizing
      from the result serves both, and the knob exists for the user who would
      rather have the memory. Auditor's same-binary A/B on the merged tree
      (min of 3; VmHWM growth of rep 1), streamed vs materialised: `use`
      58.8M×9 1.48 vs 1.70 s (4.02 vs 4.42 GB); `use` strings 7.9M×15 2.73
      vs 2.74 s (2.45 vs 2.20 GB); keep+gen+collect 58.8M×10 2.13 vs 2.31 s
      (6.14 vs 6.67 GB); sort+collect 4.07 vs 4.41 s (2.29 vs 2.50 GB);
      two-file append+collect 15.4M×15 7.83 vs 8.82 s (4.65 vs 4.85 GB).
      Explicit caps of 256 MB / 8 MB: the numeric read 3.16 / 4.67 s with
      2.83 / 2.58 GB, the string read unchanged in time with 1.13 / 0.92 GB;
      on the temp-table scenarios a cap saves little (the temp table
      dominates the peak) and costs time.
    - *The deprecated entry point is pinned by an always-on test.*
      `duckdb_pending_prepared_streaming` carries a deprecation notice
      (duckdb.h:2308) and compiles only while `DUCKDB_API_NO_DEPRECATED` is
      undefined, which parqit never defines. `tests/unit/test_streaming.cpp`
      pins its presence *and* its semantics — identical values and ORDER BY
      order as `duckdb_query` at 4 and 1 threads, a one-row result, an error
      raised deep in the stream, a stream abandoned mid-way (at 1 MB, so
      producers really are parked) followed by a healthy connection — exactly
      the discipline `test_arrow_copy_bench.cpp` applies to
      `duckdb_arrow_array_scan`. It also pins that `$`/`?` inside quotes stay
      literal: `query_streaming` runs a *prepared* statement, where they would
      otherwise become bind parameters and a Parquet path containing one would
      stop loading.
    - *Conservative fallback.* `PARQIT_FETCH_MATERIALIZED=1` restores the
      exact `duckdb_query` fetch (and skips the cleanup statement), so a
      platform where streaming ever misbehaves has a one-variable escape hatch
      and the two paths can be A/B'd in the same binary. Verify test
      **V100_COLLECT_STREAMING** proves the streamed read matches an
      independent pyarrow oracle cell for cell over a 300k-row, 5-row-group,
      8-column file (NULLs, 1.7e300 extremes, date/timestamp, emoji, empty
      strings, strL over the 2045-byte boundary), the lazy filter+gen+sort+
      collect path matches an independent duckdb-CLI oracle, the hatch is
      byte-identical (`cf _all`), and an injected fetch failure
      (`PARQIT_TEST_FAIL_FETCH_AT`, test-only) is loud on both the parallel
      and the serial path, leaves the dataset in memory untouched and leaves
      the session able to read again.
139. **zstd is the default Parquet codec (2026-09-19, maintainer's decision).**
    Until v0.1.37 an option-less `parqit save` emitted no `COMPRESSION` clause
    and therefore wrote whatever the pinned engine defaulted to (snappy). The
    COPY now always names the codec (CODEC-DEFAULT-1 in `copy_out_parquet`):
    `zstd` unless `compression()` says otherwise, so the on-disk default is
    parqit's own and survives an engine upgrade. It applies to every writer
    that reaches `copy_out_parquet` — view saves, `data` saves, partitioned
    trees and the package-owned bridge snapshots of `.dta`/Excel sources (one
    rule, no internal exception; the bridge's cost is dominated by the Stata
    import, not the codec). Not affected: `copysource` (a byte copy of the
    loaded file keeps its codec) and reading (the reader takes any codec per
    column chunk, so `partitions(append)` into an older snappy tree yields a
    valid mixed tree). Verify test **V101_SAVE_DEFAULT_CODEC** checks the
    written codec with a pyarrow oracle for the option-less, explicit-snappy,
    `data` and partitioned forms. Side observation from the same day's audit
    fixture work: a 64-byte corruption inside a snappy or zstd page decoded to
    garbage with rc 0 in the DuckDB CLI itself (no content checksum in either
    codec as Parquet writers emit them), while gzip and a damaged page header
    raise — an engine property, recorded here so nobody reads the codec change
    as an integrity feature.
140. **The triple-precision audit's key contracts (2026-09-19/20).** The
    adversarial audit of v0.1.37 against native Stata and `pq` 4.0.2
    (`docs/audits/AUDITORIA_TRIPLA_PRECISAO_PARQIT_NATIVO_PQ_2026-09-19.md`,
    evidence in `audit_repro/triple_precision_20260919/`) left four decisions,
    all pinned by `tests/verify_suite/v102_merge_key_contracts.do` and
    `v103_bigint_exact_paths.do`, whose native-parity claims are re-run
    natively inside the tests themselves.
    (i) **The join, not only the write, discloses a missing key.** Stata
    matches missing with missing in `merge` and in `joinby` (verified
    natively: `merge 1:1` and `merge m:1` pair `.` with `.`; `joinby` pairs
    them Cartesian-style), and parqit does the same. Parquet has one missing
    concept, so a side written from Stata arrives with `.a`–`.z` already
    folded into `.` and a master row keyed `.a` matches a using row keyed `.`
    that native Stata kept apart — `pq` does the same when both sides cross
    the bridge, so this is a hazard of any Stata↔columnar bridge, not of
    parqit. The `save` warning reaches the writer; the false match is suffered
    by the *reader*, possibly in another session over a file they did not
    write. `merge`/`joinby` therefore emit a `note:` when the same key carries
    missing on *both* sides — the only case in which a pairing can happen. It
    stays a note, never an error: matching missing with missing is the Stata
    behaviour and must not change. The counts use the same normalised key
    expressions the join and the uniqueness guards use (`nullif(k,'')` for
    text, NaN→NULL for numbers), so a `""` key counts as missing (MERGE-1);
    counting the raw column would make the note lie in exactly the case that
    contract exists for. Known one-sided gap: the engine's `key_value` also
    folds ±Inf and |x| ≥ Stata's missing threshold to missing and the
    plugin's `norm_key` does not, so such a key — reachable only from a
    foreign file — is under-counted, never over-counted.
    (ii) **Cost was measured before the shape was fixed, not assumed.** The
    master-side count executes the compiled view plan at *plan* time, which
    before this change happened only for the `1:1`/`1:m` uniqueness guard. One
    statement per side (`count(*) FILTER (WHERE <key> IS NULL)` per key), so
    per-key counts cost one scan per side, not one per key. Measured with
    `parqit use using <f>` + `parqit merge m:1 … ` and no `collect`, three runs
    each, StataNow MP 19.5 / Linux: a 7 940 851-row file keyed on a `VARCHAR`
    went from 0.062 / 0.026 / 0.018 s to 0.162 / 0.088 / 0.084 s, and a
    58 800 107-row file keyed on an `INTEGER` from 0.058 / 0.072 / 0.068 s to
    0.223 / 0.114 / 0.181 s (first run of each triple is cold-cache). Roughly
    +0.06 s and +0.05–0.11 s on warm runs. `EXPLAIN` on the generated query
    shows why it stays cheap and where it does not: `READ_PARQUET` carries
    `Projections: idcode`, so only the key column is read, but the normalising
    `CASE` is evaluated row by row (the plan estimates the full 58 800 107
    rows) — this is a real single-column scan, not a row-group-statistics
    shortcut, and it does not materialise the join. The per-key form was
    therefore kept and no side is skipped: a note that silently omitted a side
    would be worse than the scan it saves. On a file whose key column is wide
    or badly compressed the cost will be higher than measured here.
    (iii) **`r(459)` for a non-unique key.** Native Stata and `pq` both return
    459; parqit returned 198. The three implementations all failed loudly, so
    this was compatibility, not correctness: `capture … if _rc == 459` did not
    catch parqit. `check_unique` now returns 459, the message is unchanged,
    and the change is declared in `CHANGELOG.md` under `### Changed` because
    it is public semantics. No test asserted the old code.
    (iv) **The merge contract is multiset equality, not row order.** parqit
    returns the result grouped by the key and declares a `sortedby` marker
    that was verified true; native `merge` returns an interleaved order and
    declares nothing. Order is not a contract because native `merge`'s own
    within-key order is not reproducible — changing only the physical row
    order of the *using* file made native `merge m:1` return the master as
    `2 1 3 4 6 5 8 7` and as `1 2 3 4 5 6 8 7`. What parqit guarantees, and
    v102 pins, is cell-exact equality with native as a multiset plus
    determinism between runs of the same plan. Documented in the README and
    `help parqit` so users who depend on `_n`/`by:` sort explicitly.
    (v) **No lossless-read option for `int64`/`uint64` was added.** `pq` has
    `safe_int64`; parqit documents `parqit sql "SELECT …, CAST(col AS VARCHAR)
    …"`, which the audit verified returns `9007199254740993` and
    `9223372036854775807` exactly (`str19`). A new read option is plugin + ado
    + help + dialog + type contract + tests, and the mandate of this round was
    to pin and disclose, not to widen the public surface; the recipe is now in
    Limitations and the option is proposed for `ToDo.md`. The loss itself is
    already loud (`note: <var>: values beyond 2^53 rounded to nearest
    double`), and a *lazy* join over such a key is exact because it runs in
    the engine before any Stata `double` exists — v103 pins all three.
    Entry **138** is the streamed fill (PERF-STREAM-1), merged into `main` the
    same day from a separate worktree; the numbering 138/139/140 is complete.
141. **The parallel fill triggers on cells as well as rows; batched variable
    allocation was measured and not adopted (2026-09-20, WIDE-FILL-1).** A
    review of `pq` 4.0.x for ideas within parqit's scope singled out its
    "batched variable allocation" (up to 7x on wide files). Measured on a
    3,200-variable x 20,000-row parqit file (64M cells, zstd): variable
    creation (`_parqit_resp_create`, one `st_addvar` per variable plus one
    `format` per variable) costs 0.17-0.26 s of a 2.7-3.1 s load, so
    vectorising it could save at most ~0.15 s — not worth the change. What
    the profile did expose: the fill ran on ONE worker, because the parallel
    trigger (#37) counted rows only (50,000) and this file has 20,000. Forcing
    8 workers cut the fetch 2.1 -> 1.7 s; the trigger is now rows >= 50,000 OR
    rows x columns >= 2,000,000 (`kParallelMinCells`), with the same serial
    path for everything smaller. The remaining wide-file cost is the engine's
    own per-column scan: the DuckDB CLI alone takes 3.6 s to `CREATE TABLE AS`
    the same file, so parqit's 1.6-1.9 s fetch already beats a plain
    materialisation and the per-variable Stata work is not the bottleneck.
    Byte-identity of parallel vs serial is unchanged (v20 oracle, t01 with
    2,500+ variables). The worker cap of 8 (#37) was re-measured under the
    streamed fetch (`PARQIT_FILL_THREADS` sweep, min of 3, 48-core box under
    moderate load): `use` 58.8M×9 — 4 workers 2.18 s, 8 1.49 s, 12 1.49 s,
    16 1.60 s, 24 1.93 s; `use` 7.9M×15 with three string columns — flat at
    2.65–2.91 s for every count. So 8 is still the plateau: the single
    producer (one `duckdb_fetch_chunk` + one Arrow conversion per 2048-row
    chunk, ~29k chunks for 58.8M rows) is the floor, not the per-cell store.
    The next Stata-side lever is therefore to move the Arrow conversion into
    the workers (each converts the chunk it fills; `duckdb_fetch_chunk` stays
    on one thread), not a higher worker count; Stata/MP's own threads never
    apply to plugin stores, and the SPI has no bulk store. Other `pq` items
    judged in scope but not implemented in
    this round, in priority order: a protective default for `int64`/`uint64`
    beyond 2^53 (refuse, with a string-load option — `ToDo.md`); a source-file
    provenance column for globs/directories (`read_parquet(..., filename)`),
    which must be kept out of the Hive-key and name-recovery logic of
    `plan_columns`; read-time `cast()` with strict/`lax` semantics; a decode
    option for `BINARY` columns (dropped with a message today); and explicit
    CSV reader options (date parsing, sample size, delimiter). Out of scope by
    the brief or by the integrity rule: SAS/SPSS, `compress_string_to_numeric`,
    `metadata_only`, `fast`, and loading more than 2^31-1 rows into memory.
142. **INT64-PROTECT-1 — `int64(refuse|round|string)`, refusing by default
    (2026-09-20).** Entry #140(v) recorded that no read option for values
    beyond 2^53 had been added and proposed one; this is that option, and it
    goes further than `pq`'s `safe_int64` by making the *protective* case the
    default. The decisions:
    (i) **Refuse rather than round.** A `BIGINT`/`UBIGINT`/`HUGEINT`/wide
    `DECIMAL` column whose observed magnitude exceeds 2^53 now fails the read
    with rc 198 (`kRcUsage`), one message naming every such column and both
    remedies. The note it replaces was loud but easy to lose in a log, and the
    damage it announced (two keys becoming one observation) is silent
    afterwards. This is a public semantic change, so it is declared in
    `CHANGELOG.md` under `### Changed`; the tests that deliberately exercise
    the rounding now ask for `int64(round)` explicitly.
    (ii) **Where it fires.** In `plan_columns`, after the range pass that
    already measured `any_beyond_2p53`, so the trigger is the DATA, never the
    declared type: a `BIGINT` column whose values all fit loads exactly as
    before, with no option and no note, and the extra `max(strlen(...))`
    aggregate that sizes the text form is only added under `int64(string)`
    (one more aggregate in a query that already scans; no second pass). The
    refusal happens before anything is staged and before the response file is
    written, so `use`/`collect`'s atomic validate-then-mutate keeps the data
    in memory untouched — pinned by a sentinel dataset in v104.
    (iii) **`string` is exact by construction.** `CAST(col AS VARCHAR)` is
    evaluated in the engine over the integer itself (DuckDB v1.5.3
    `NumericHelper::FormatSigned`, `cast_helpers.hpp:64-78`, with the
    `hugeint_t` specialisation at `:107` and `DecimalToString` at `:109-127`);
    no double is constructed, unlike the `__parqit_double()` path. The width
    is the observed `max(strlen(...))` over the same expression, so it is
    exact (`str16` for 2^53+1, `str19`/`str20` for the signed extremes,
    `str20` for `UBIGINT`), and the existing `str#`/`strL` rule at 2045 bytes
    applies unchanged. A converted column loses its numeric display format and
    any value-label attachment, because neither can be applied to a Stata
    string variable; the digits are the payload, and the note says the column
    became text.
    (iv) **Previews never refuse and never round.** `parqit head`/`parqit
    list` force `string` for such columns: a preview exists to show what is in
    the file, changes nothing in memory, and a refusal there would hide the
    very values the user is looking for.
    (v) **Precedence and scope.** Explicit option > the value
    `parqit use using ..., int64()` opened the view with (carried on the
    `View`) > `parqit set int64` > `refuse`. `parqit mergein`/`appendin` read
    their disk side through `parqit use`, so they inherit the default and
    forward their own `int64()`; the message they surface carries the inner
    `parqit use:` prefix. That is a **known cosmetic limit, accepted** (ruling
    of the 2026-09-20 audit): naming the outer command would mean a `label`
    field on the use request, like collect's MSG-LABEL-1, and the actionable
    part — the option name, which both commands accept — is already in the
    message. Measured on
    a >2^53 disk side (2026-09-20): default → rc 198 with the data in memory
    intact; `int64(round)` → the note, and then native `merge` stopping with
    `r(459)` because the rounding itself made the key non-unique — the hazard,
    demonstrated; `int64(string)` on a *key* → native `r(106)` (double in the
    master, `str16` in the using data). So on a `mergein`/`appendin` key the
    usable remedy is `int64(round)`, and `string` is for the payload /
    `keepusing()` columns; `help parqit` says exactly that.
    `parqit save`, the lazy verbs, the join keys and every statistic are
    untouched: nothing there ever became a Stata double.
143. **BINARY-DECODE-1 — `binary(text|hex)` for Parquet BLOB columns
    (2026-09-20).** The default is unchanged (dropped with a message), because
    raw bytes have no Stata representation and a silent hex expansion of a
    100-byte blob column would be a surprise, not a service. The drop message
    now names the two options. `binary(text)` is `decode(blob)`, which DuckDB
    v1.5.3 defines as BLOB→VARCHAR that *throws* on an invalid UTF-8 sequence
    (`extension/core_functions/scalar/blob/encode.cpp:36-51`, registered at
    `:105-107`) — never a replacement character; `binary(hex)` is `hex(blob)`,
    two uppercase digits per byte (`extension/core_functions/scalar/string/
    hex.cpp:66-85`, registered at `:394`, digits from `Blob::HEX_TABLE`,
    `src/include/duckdb/common/types/blob.hpp:21`). Both are sized by the
    ordinary `strlen` pass, so a wide blob becomes a `strL` by the existing
    rule. Two consequences of the lazy architecture are accepted and
    documented: the option acts at `parqit use using` (the boundary decides a
    view's columns once, and a blob kept beyond it would change what every
    verb and `parqit save` see), so `parqit collect, binary()` is refused with
    a message naming where it works; and a preview cannot show a blob the view
    never had. The eager path names the offending column when `decode` fails
    (each binary column is re-probed alone, only on the failing plan); the
    lazy path cannot — it plans over an already-decoded VARCHAR — so the
    engine's SQL-flavoured advice is rewritten into parqit's remedy there
    (`rewrite_decode_failure`, signature-matched, so no other engine message
    or rc changes). There are **three** failure points, not two: the planner's
    sizing pass on a direct read, and — for a view with stages — the
    `CREATE TEMP TABLE` that materialises it *before* the planner runs. All
    three refuse with rc 198 and print parqit's own words, with no generated
    SQL and no `try(decode(...))`/`'replace'` advice (v69's no-raw-engine-text
    contract), leaving the data in memory untouched (v105, including the
    filter-then-collect case). One accepted consequence: a view opened
    `binary(text|hex)` and then saved writes TEXT where the source had BINARY.
    That is the representation the user asked for, the default still drops the
    column so no save changes without the option, and it is deliberately left
    without a save-time note (ruling of the 2026-09-20 audit).
144. **`filename(newvar)` is a first-class known column, not a file column
    (2026-09-20, FILENAME-1).** The brief fixes the public command surface but
    says nothing about a provenance column; users of a glob or a Hive tree
    repeatedly need to know which file a row came from. DuckDB already computes
    it: `filename = '<name>'` with a VARCHAR value names the column
    (`multi_file_reader.cpp:137-144`, default name `filename`,
    `multi_file_options.hpp:33`). The decision is how parqit *plans* it.
    `MultiFileReader::BindOptions` appends the column after the files' own
    columns and BEFORE the Hive partition keys (`multi_file_reader.cpp:219-229`
    adds it, `:231-247` then adds the keys; BindOptions itself runs after the
    file columns are bound — `:566-577` plain, `:544-551` union_by_name,
    `multi_file_function.hpp:109-111` custom bind), and `read_csv_auto` shares
    the option through `MultiFileReader::AddParameters`
    (`read_csv.cpp:100`). So `Source::filename_column` /
    `PlanContext::provenance_column` travel with the scan and the column is
    excluded from the leaf-vs-scan alignment count, from Hive tagging
    (positional and by name), from the `parqit.*` manifest and from the
    HIVE-CLASH-1 / PART-STRKEY-1 checks; the lazy view carries the name
    (`View::set_source_filename_column`) so a direct-read collect re-plans the
    same way. Without this the alignment `n == ncol` would fail on a flat glob
    (silently dropping exact-name recovery) and, over a Hive tree, the column
    would be tagged a partition key and retyped from the manifest. Two clash
    layers, both loud (rc 198), because the engine's own check is only an exact
    match against the FILE columns and its message spells DuckDB syntax: a
    probe of the source without the option refuses a case-insensitive clash
    with anything it exposes (partition keys included — DuckDB resolves
    identifiers case-insensitively, so two such columns could not both be
    addressed), and `plan_columns` sanitises the source's names WITHOUT the
    provenance column — exactly what they would be had the option not been
    given — and refuses when one of them claims the requested name (a header
    `my file` loads as `my_file`, an empty name as `v<position>`), which
    `sanitize_unique` would otherwise have suffixed silently. The value is the path as the engine
    reports it (absolute for an absolute pattern); the column carries a Stata
    note and no manifest metadata, is auto-appended when a varlist does not
    name it (`filename()` asks for it explicitly), and is not offered to
    `parqit save ..., copysource` — the dataset is no longer an image of one
    file. A `.dta`/Excel source is refused in the ado: it is scanned through a
    package-owned bridge whose path says nothing about the user's file. Verify
    `v106`.

145. **`csv()` forces the reader's dialect and types; the whitelist is the
    engine's own option names (2026-09-20, CSV-OPT-1).** Delimited-text type
    inference is a guess, and a wrong guess changes values with rc 0: with the
    pinned reader `1e5` becomes 100000, a 21-digit decimal is rounded to what a
    double holds, and `TRUE` becomes 1. A single Stata option `csv(...)` is
    parsed by the ado (`_parqit_csv_opts`) and validated again by the plugin
    against the names the pinned engine accepts (duckdb v1.5.3
    `csv_reader_options.cpp`: `delim`/`quote`/`escape`/`header`/`nullstr` at
    `SetBaseOption` :386-402, `sample_size` :237, `dateformat` :264,
    `timestampformat` :267, `types` :711 — a STRUCT keyed by column name whose
    values are type names — and `all_varchar` :750; all declared named
    parameters of `read_csv`/`read_csv_auto`, `read_csv.cpp:57-99`). Anything
    else is rc 198 naming the key. A `types()` type is not matched against a
    hard-coded list (that would drift from the pinned engine) and is not left
    to the engine either: DuckDB reports an unrecognised type only from the CSV
    bind (`csv_reader_options.cpp:711` → `TransformStringToLogicalType`), and
    that message arrives with the binder's `LINE 1:` dump of parqit's own probe
    SQL and the user's path — internal text parqit never shows (`v69`). So the
    plugin proves the type itself, before any scan SQL exists: the token must
    be letters, digits, `_`, parentheses and commas (which also makes it unable
    to carry SQL), then one `SELECT CAST(NULL AS <type>)` on the same
    connection, with no result stream open. A failure is rc 198 naming the type
    and the column. Every value crosses the
    wire hex-encoded and reaches SQL as a `quote_literal` literal (the struct
    keys of `types` included); the one number, `sample_size`, is proved to be a
    decimal integer before it is spliced (#115). `types()` items are tokenised
    in Mata and split at the last `:` in the plugin, so a column name is never
    whitespace-split across the boundary — the cost is that neither the header
    name nor the type spelling may contain whitespace (`DECIMAL(18,2)` works,
    `DECIMAL(18, 2)` does not), which is documented. A forced
    dialect (`delim`/`quote`/`escape`/`header`) is applied to the CSV-HEADER-1
    raw-header probe as well as to the scan, or name recovery would read the
    file with a different split than the data; only dialect keys go to that
    probe (a `nullstr` or a `types` would corrupt the names it reads back), and
    `header(off)` means there is no header line to recover from, so the columns
    keep the engine's `column0…` names. An empty sub-option value counts as not
    given — each of these wants a character or a word. `csv()` on a source that
    is not delimited text is rc 198, never a silent no-op, because unlike
    `encoding()` it changes values. A `csv()` written with nothing in it is a
    different matter and is simply not given: Stata's own parser drops an
    option with empty parentheses before `syntax` ever sees it (`passthru`
    reports it exactly as `string` does, `""` — verified on this Stata), so
    every Stata command behaves this way and parqit follows, rather than
    carrying code to recover the difference from the raw command line. The `using` side of
    `merge`/`joinby`/`append` is unaffected: it is bridged through Stata's
    `import delimited`, which has its own options (#41). Verify `v107`.
146. **The fill workers convert their own Arrow chunks; the worker cap is 16;
    `parqit set fill_threads` (2026-09-20, ARROW-IN-WORKERS / FILL-THREADS-SET-1).**
    Under the streamed fetch the fill was flat from 8 workers (#141): the
    producer's per-chunk `duckdb_data_chunk_to_arrow` was the floor. The
    producer now only fetches (`duckdb_fetch_chunk` must stay on one thread:
    one cursor) and enqueues the raw chunk with its observation offset and row
    count (`duckdb_data_chunk_get_size`); each worker converts the chunk it
    fills and releases both. Concurrent conversions of distinct chunks are safe
    by construction in the fetched DuckDB 1.5.3 source: the C entry point reads
    the options wrapper only and copies its `ClientProperties` by value
    (`capi/arrow-c.cpp:48-69`), the converter builds a per-call `ArrowAppender`
    over the chunk (`common/arrow/arrow_converter.cpp:19-25`), and its one
    shared lookup, the Arrow extension-type registry, runs under the config's
    own mutex (`function/table/arrow/arrow_duck_schema.cpp:431-440` →
    `common/arrow/arrow_type_extension.cpp:235-243`). A conversion failure
    takes the worker's abort path (loud rc, staged frame dropped). The serial
    path is unchanged. Measured, same binary, min of 3, 48-core box: at 8
    workers `use` 58.8M×9 1.41 → 1.29 s, `use` 7.9M×15 strings 2.64 → 2.37 s,
    keep+gen+collect 2.04 → 1.79 s, sort+collect 4.87 → 3.86 s, two-file
    append+collect 7.86 → 7.37 s; peak RSS within ±5%. The worker sweep on the
    numeric file changed regime — 4 → 1.89 s, 8 → 1.25 s, 12 → 1.00 s,
    16 → 0.95 s, 24 → 0.80 s (previously 8 → 1.49 s and worse beyond) — while
    the string-heavy file stayed flat at 2.35–2.9 s (scan-bound), so
    `kFillThreadCap` rises from 8 to 16: `min(cores, 16)` takes most of the
    gain while staying moderate on shared HPC nodes, whose core allocation
    `hardware_concurrency()` does not see; the default 58.8M×9 read is 0.90 s.
    `parqit set fill_threads auto|#` is the in-session knob (a function-local
    static in the plugin, `fill_threads_session()`, read at every fetch and
    reported by `parqit version` as `r(fill_threads)`); it outranks the
    `PARQIT_FILL_THREADS` environment variable, which is fixed before Stata
    starts, and the ado/plugin refuse anything but `auto`/`0` or 1..1024.
    Integrity: streamed vs materialised loads identical by `datasignature`
    and every per-variable statistic on the real files, and identical to the
    previous build's signatures; v20 (1.5M-row pyarrow oracle, parallel), v60
    (worker lifecycle and injection), v100, t01, v09 and the new v108 (1/2/4/8
    workers byte-identical on a tall and a wide file against pyarrow) pass.
    Lesson recorded: a function-local static used across translation units
    must be defined outside the file's anonymous namespace — the first build
    linked but the plugin failed to load with an unresolved symbol, caught by
    the `plugin_runtime` ctest before any Stata run.
147. **One missing-key rule for the lazy join and its contracts; the finite
    bound is exactly 2^1023 (2026-09-20, KEYFOLD-1 / MAXDOUBLE-1).** Reading
    the two predicates side by side: `key_value` in `view.cpp` (the join)
    folded a numeric key to missing when NULL, NaN, ±Inf or |x| >= 2^1023,
    while `norm_key` in `plugin_view.cpp` — the uniqueness contracts behind
    r(459) and the missing-key note — folded only NaN, although its comment
    claimed "exactly as the join does". Confirmed with the duckdb CLI over
    (NULL, nan, inf, 1e308): the contract saw 2 distinct keys and 2 missing,
    the join saw 4 missing. Consequence: a third-party using file with `inf`
    or `1e308` in a numeric key (parqit never writes those; the boundary
    normalises them) passed m:1/1:1 and the join, comparing with IS NOT
    DISTINCT FROM, matched every such row against the master's missing-key
    row — a silent duplication with rc 0, and an under-counting note. Fix: the
    rule is ONE function, `parqit::key_missing_fold_sql(ref, kind)`, used by
    `key_value` (the MISS-1 `normalized` shortcut is kept) and by the
    contracts/note; there is no second implementation to drift. Pinned by a
    unit test (9 values, 6 missing) and v102-F (pyarrow fixture: m:1 → 459 on
    the using side, 1:1 → 459 on a master view over such a file, joinby
    delivers all four using rows with three under a missing key, note counts
    "1 master and 3 using"). A refused merge prints no notes — by design, so
    the count is read from the joinby log. MAXDOUBLE-1, found while writing
    that test: the 16-digit literal `8.988465674311579e307` used as "2^1023"
    by `parqit_finite` (session.cpp) and `stata_stat_finite` (plugin_view.cpp)
    parses to 2^1023 − 2^970, which is Stata's `maxdouble()`, so the guard
    reported that one legitimate finite value as missing (verified in the CLI:
    `8.988465674311579e307::DOUBLE = power(2, 1023)` is false; the 17-digit
    form and `power(2, 1023)` are exact). Both now use the exact constant
    (`0x1p1023` / `parqit::kStataMissThreshold`); unit test MAXDOUBLE-1 keeps
    maxdouble and nulls 2^1023. No public behaviour changes for any value a
    Stata dataset can hold except that one; no file format change. Follow-up
    (same day): the two GROUP BY / PARTITION BY key folds — `norm_group_key`
    in the engine (GROUPKEY-1, whose comment promised the join's rule) and its
    plugin twin `norm_view_key` (tabulate, summarize, xtile, reshape) — and
    the reshape check "variable j contains missing values" folded only NaN;
    all three now call `key_missing_fold_sql` too, so there is no second
    implementation anywhere. Reachability today: every column a view holds
    is boundary-normalised (open, `sql`, and the using side of
    append/merge/joinby all go through `boundary_for`) or finite-guarded
    (gen/replace/aggregates), so no user path fed Inf or 1e308 to a group
    key — the change removes the drift, it fixes no observed result. Pinned
    by a unit test over a raw VALUES source (1, NULL, NaN, Inf, 1e308 → two
    groups, the missing one summing every special). Cost measured in the
    duckdb CLI on the 58.8M-row `firm` key: NaN-only fold 0.47–0.59 s, full
    fold 0.52–0.57 s per GROUP BY (noise; user CPU +15% spread over threads).
148. **Extended missings `.a`–`.z` survive the Parquet round trip on request:
    `parqit save ..., xmissing` (2026-09-20, XMISS-1).** The "one documented
    loss" of the format (Parquet has a single null) is now optional, as an
    additive opt-in that leaves the default byte-identical. Encoding: the
    27 Stata missing doubles are 2^1023·(1 + k/4096), k = 0..26 — bits
    0x7fe0000000000000 + (k << 40), verified against Stata's own %21x output
    (.a = +1.0010000000000X+3ff, .z = +1.01a0000000000X+3ff; a float .a
    promotes to the same double), and both directions are exact in binary64
    (unit test XMISS-1 checks every code bit for bit and rejects inf, NaN,
    maxdouble, 1e308 and a one-bit neighbour of .a). On disk: an `int8`
    companion column `_parqit_xm_<var>` per variable that holds at least one
    extended missing — dense, 0 = none, 1–26 = the code, no validity buffer,
    the primary cell null either way — and the pairs (exact Stata name →
    companion leaf name) under the `parqit.xmissing` KV key. Dense 0 rather
    than NULL because the staged writer would otherwise invalidate nearly
    every numeric cell one call at a time, and a zero column compresses to
    nothing; sparse (only affected variables) rather than one companion per
    numeric variable because a 3000-variable file would double its column
    count and every parqit read scans its companions. Both writers produce the
    same columns, codes and map (v109-E compares the Arrow and
    PARQIT_SAVE_NOARROW files with pyarrow). The reader folds the companion
    into its primary's plan (`ColumnPlan::xm_source`, matched by TRUE parquet
    name, hidden from every planner — use, describe, the lazy open's metadata
    pass, mergein/appendin), the fetch selects it after the planned columns as
    TINYINT (so the manifest, the ado and every count of k are unchanged;
    a wider integer that does not fit is a loud cast error), and the fill
    restores the cell before the value walk, which skips NULL cells; SF_vstore
    converts the double to the variable's own storage type (v109-A: byte,
    int, long, float, double all come back as .a/.z/.m). Integrity contract:
    a code outside 1–26, or a non-zero code on a cell that holds a value,
    fails the load with memory untouched; a companion paired with a string
    column or with an absent primary is hidden and ignored with a note; a
    companion of a non-integer type likewise. Lazy path, phase 1: the open
    hides the companions, folds the cells to `.` and prints a note naming the
    variables (a view save therefore writes plain nulls, announced at open);
    carrying (value, code) through the verbs and using it in merge-key
    equality is phase 2, deliberately not started here. Refusals: `xmissing`
    with `copysource` (the file is copied as it is) and with
    `partitions(replace|append)` (read_parqit_meta drops ALL metadata when the
    parqit.* keys differ across a tree's files, and the companion set depends
    on the data written, so a partial rewrite cannot promise equality; a
    whole-tree write can — v109-F reads one back); on a view save; and when a
    variable carries a companion's name (case-insensitively, as the engine
    resolves identifiers). `copysource` refuses a source that carries
    companions — its copy reads the memory's columns under a rebuilt KV and
    would have dropped both the companions and the key, silently returning
    the restored .a–.z as `.` (the ORDER-PROOF-1 comparison folds missings, so
    it would not have noticed). Reporting: `r(xmissing_vars)`, a "preserved"
    note instead of the loss note, and the loss note (without the option) now
    names the option; the phrase "extended missing values" keeps its own line
    (v29/v77). Arithmetic on an extended missing gives plain `.` in native
    Stata (`.a + 1`, `1 * .a` checked), so a computed column folding to `.`
    matches native semantics. Dialog: `parqit_write.dlg` gains the checkbox,
    enabled only for the memory save, height 450 → 475 (checked under Xvfb).
149. **No hard-coded thread limit: every count defaults to, and is bounded by,
    the CPUs available to the process (2026-09-20, CPUS-1).** The maintainer's
    rule: any thread count in parqit — the engine's `threads`, the fill
    workers — must be settable as 1, 2, …, N where N is the machine's, with
    nothing hard-coded. Implementation: `parqit::available_cpus()` = the
    affinity mask on Linux (`sched_getaffinity` / CPU_COUNT — what a
    SLURM/cgroup allocation or `taskset` leaves visible; `nproc` and Stata's
    c(processors_mach) agree with it), `std::thread::hardware_concurrency()`
    elsewhere and as the fallback (1 if detection fails). The fill's automatic
    rule is N workers (was `min(cores, 16)`; the 16 existed only because
    hardware_concurrency() cannot see an allocation — the affinity count
    can); `parqit set fill_threads` and `parqit set threads` accept 1..N and
    CLAMP a larger number to N with a note (`_parqit_set_note`, printed by the
    ado), never refuse it: refusing would change `parqit set threads`'s
    public semantics (it accepted up to 2^31−1) and break a script written
    for a bigger machine; PARQIT_FILL_THREADS is clamped the same way, said
    once per session. The engine's default is N too (`Session::ensure_open`
    sets `threads` when the user chose none): DuckDB's own default is the
    hardware count and ignores the mask — verified on this box, `SELECT
    current_setting('threads')` returned 48 under `taskset -c 0-7` while
    nproc said 8 — so a restricted job used to oversubscribe its allocation.
    `parqit version` prints and returns `r(cpus)`, `r(threads)`,
    `r(fill_threads)` and `r(stream_buffer_mb)`. Evidence for the auto rule,
    a matrix of engine threads {16, 24, 48} × fill workers {8, 16, 24, 32,
    48} on the 48-core box (min of 2): the 58.8M×9 numeric read takes 1.2–1.3 s
    with 8 workers, 0.91–0.98 with 16, 0.69–0.83 with 24, 0.70–0.79 with 32 and
    0.62–0.73 with 48 at every engine thread count (best 0.616 at 24×48, 0.637
    at 48×48), i.e. no oversubscription penalty with both at N; the
    string-heavy 7.9M×15 read is 2.36–2.83 s throughout (scan-bound, noise).
    The earlier sweeps in #37/#141/#146 are superseded. Small-read overhead,
    measured at the same time (1000 rows × 9 vars, 20 calls each): `parqit
    use` 19.8 ms per call, `parqit describe` 19.3 ms, lazy open+collect+close
    25.1 ms, native `use` 0.1 ms — the fixed cost is the plan's probes
    (schema, footer metadata, count, identities, stats) at a few ms each, not
    worth attacking; the earlier "0.1–0.3 s" figure was wrong. Pinned by
    v108: clamps and notes for both settings, r(threads) read back through
    `current_setting('threads')`, a child Stata under `taskset -c 0-1`
    reporting r(cpus) = r(threads) = 2 (the executable is found by probing
    stata-mp/stata-se/stata under c(sysdir_stata): c(flavor) says IC on this
    MP installation), the PARQIT_FILL_THREADS clamp said once, and
    byte-identical loads at every worker count. Edge recorded: when both
    detections fail (no affinity call, `hardware_concurrency()` = 0) N is 1
    and the engine runs single-threaded where it used to keep its own
    default — a safe choice for an undetectable machine, and `parqit set
    threads` overrides it. The Linux affinity call sits under `__linux__`;
    macOS/Windows compile the fallback only, so CI must be green before any
    release tag (the Stata suites cannot run there). The Views/SQL/settings
    dialog still lists only the four original `set` options (`int64`,
    `fill_threads`, `stream_buffer_mb` absent): a follow-up, with a real
    click, like #151.
150. **`parqit set stream_buffer_mb auto|0|#` (2026-09-20,
    STREAM-BUFFER-SET-1).** The in-session counterpart of
    PARQIT_STREAM_BUFFER_MB (#138), same idiom as `fill_threads` (a
    function-local static outside the anonymous namespace, `stream_buffer_session()`,
    consulted by `stream_buffer_cap_bytes()` before the variable), reported
    by `parqit version`. The variable's silent clamp at 4096 MB is removed —
    the buffer is a cap, not a reservation, and a quiet ceiling is exactly what
    #149 forbids; the parser still refuses non-integers and negatives, and
    bounds the value at 10^9 MB only to keep the byte arithmetic in range.
    v108 loads the tall and the wide file under 8 MB and under 0 (the engine's
    1 MB default, which bites on both) and gets the reference datasignatures.
151. **The read dialog exposes `csv()` (2026-09-20).** A free-text EDIT
    (`ed_csv`, `optionarg`) under the provenance field, enabled with the
    int64/binary controls in `use` mode, disabled for `open _data`/`path`;
    height 510 → 545 and the context line moved from y=465 to 500. Verified in
    GUI Stata under Xvfb with a real Submit: the emitted command was `parqit
    use using ….csv, clear csv(delim(;) header(on))` and 74 rows loaded; t15
    runs the same shape on an `export delimited` fixture. Lesson kept from the
    write dialog: `.command` and dialog-lint do not catch an option missing
    from PROGRAM command — only a click does. Not built this round, with the
    reasons recorded in ToDo.md: a per-column digest (needs a hash parqit
    controls — DuckDB's `hash()` is version-internal —, an order-independent
    per-column combination so parallel and projected reads can verify, an
    opt-in switch and adversarial fixtures; done quickly it would refuse valid
    files) and the rowid-addressed fill for scan/fill overlap (single-file
    only as far as the engine's row numbering goes, unknown payoff). Not
    actionable: the version bump belongs to the release step (release_lint
    keeps the surfaces in step), the DuckDB deprecations wait for the engine
    upgrade, and the Stata suites cannot run in CI without a licence.
152. **Codex adversarial audit remediation (2026-09-20, CA-01–CA-06).**
    The six findings and original repros are preserved in
    `docs/audits/AUDITORIA_ADVERSARIAL_HOLISTICA_CODEX_ASTRA_2026-09-20.md`.
    Decisions for the local correction:
    - `stream_buffer_mb 0` means RESET of the connection setting on every
      streamed fetch, not omission of SET: an earlier fetch can have changed
      it. The pinned engine implements RESET through
      `StreamingBufferSizeSetting::ResetLocal` (custom_settings.cpp:1605),
      which calls `ClientConfig::SetDefaultStreamingBufferSize`; no copied
      default constant is needed. This supersedes #150's ineffective zero
      transition, without changing the intended setting contract (v110).
    - Extended-missing metadata controls values. A primary cannot also be a
      hidden companion; reject that graph before hiding columns, including
      self-pairs and chains/cycles. A differing multi-file metadata set with
      an extended-missing channel is refused with rc 198 before the former
      all-metadata fallback can erase codes. `PlanContext::refusal` propagates
      this to lazy opens and using sides too. Per-file reconciliation is not
      implemented; separate reads plus `appendin` preserve the values. The
      restriction is announced in the changelog (v111/v113).
    - An integer column converted by `int64(string)` carries valid extended
      codes as the text `.a`–`.z`; a plain null remains `""`. Validation must
      still reject an invalid code or a code on a non-null primary, before
      swapping the staged dataset. This is an explicit text representation,
      not a change to the lazy view's phase-1 folding policy (v112).
    - `discard` does not reset the registered plugin in the local Stata
      runtime. Document restart for a rebuilt plugin, and explicit `close`
      for views; do not introduce an implicit plugin reset (v114).
    - The CSV conflict check follows the actual pinned source, not just its
      error wording: `BaseCSVData::Finalize` calls `StringDetection` with
      the delimiter as the needle and nullstr as the haystack
      (`src/function/table/copy_csv.cpp:45` and `:90`). Therefore nullstr may
      be contained in a multi-byte delimiter; the originally proposed
      symmetric check would have removed a supported input. SetDelimiter
      also expands literal `\t` sequences to TAB (csv_reader_options.cpp:124),
      whereas nullstr stays literal; normalize the comparison in the same
      way so a TAB-separated file may still use the two-byte `\t` null marker.
      Only the
      reproduced explicit-delimiter conflict is checked here (v115).
    - v108's 90,000-row "wide" fixture also crosses the row threshold, so
      v116 independently exercises 20,000 × 110 cells. A worker-start fault
      proves the automatic parallel path was selected; both serial and auto
      reads are checked against the independent generated values.
153. **Current menu/help alignment (2026-09-20).** The GUI has the same
    seven `set` choices as the ado, and `int64()` selectors on eager/lazy
    read, collect, mergein and appendin. For those selectors, the displayed
    `default` is a GUI-only inheritance choice and emits no option; explicit
    `refuse` must be emitted, because a session or view can default to
    `string`. The session-setting selector always emits its chosen policy.
    This removes the old read dialog's ambiguity from hiding its `refuse`
    default. The engine and public command grammar are unchanged.
    The helps distinguish presentation-metadata fallback from value-metadata
    refusals, document all version results, describe +/-2^53 as a conservative
    consecutively exact integer range, and allow lossless string merge keys
    when both native sides are strings. Session `auto` defers to environment
    overrides; a positive fill count or an explicit numeric buffer setting
    overrides them. The row/cell trigger is 50,000 rows OR 2M cells.
    Dialog lint checks that option-bearing controls reach command output,
    including conditional literal flags, and checks the current setting list
    against the ado. New t17 verifies the integer-policy command shapes.
    GUI validation uses a private Xvfb display and a read-only, network-isolated
    Stata sandbox with one writable scratch directory, avoiding personal
    preference changes; only the test processes are terminated.
154. **0.2.0 release boundary (2026-09-20).** The accumulated new input and
    fidelity controls, streamed fill and protective integer default warrant
    a minor-version increment rather than another 0.1.x patch. The user
    explicitly withdrew the OpenMP request provided multi-thread execution
    works. Retain DuckDB's scheduler and the existing C++ fill pool; prove
    worker execution and exact-result parity, not just a compiler flag.
    Windows keeps the verified MSVC /MT build and no companion DLL. Embedded
    Microsoft runtime code is distinct from a required external runtime and
    remains under its documented terms; do not claim an entirely MIT binary.
    The PE gate allows reviewed Windows system imports only. Release branches
    run all four CI targets before a tag; publish only after exact-asset and
    staged-install checks. Commit product/build/docs and regression tests,
    not personal examples, manuscripts, logs or local audit kits. Include the
    Linux build helper in the official sources without claiming it supports
    the other platforms; their presets remain available.
    The first preflight exposed a helper/CI interaction: forcing `gcc/g++`
    paths over cached `cc/c++` paths made CMake reset the preset configuration,
    build without tests and fail on the missing `test` target. The helper now
    selects compilers through CC/CXX for a new tree, preserves equivalent
    cached aliases, and refuses a different compiler without clearing cache.
155. **Preserve streaming worker failures (0.2.1, 2026-09-20).** A worker's
    `Executor::PushError` records its exception and then sets `interrupted`.
    DuckDB 1.5.3's simple-buffer shortcut threw a generic interruption first,
    masking the original type/message. The hash-guarded streaming patch checks
    the executor only after observing interruption and rethrows its error;
    an acquire fence on that error path pairs with publication of the flag.
    Genuine user cancellation is unchanged, as is the successful fetch path.
    The deterministic regression injects the actual executor error state
    between opening and fetching, using the verified pinned C API connection
    representation; it separately exercises `duckdb_interrupt`. The original
    scheduling-sensitive regression remains intact. The user authorised this
    correction, README review and a new release; preserve the unpublished
    v0.2.0 tag and publish v0.2.1 only after fresh local/CI/exact-asset gates.
    No OpenMP is introduced. README does not promise ordering within tied
    merge/join keys: the view orders by keys, not a unique tie-breaker.
156. **Adversarial boundary and name audit (2026-09-22).** Internal helpers
    must avoid ASCII case variants in every engine-visible namespace. Names
    produced by reshape and merge/append markers must be unique both as engine
    identifiers and as exact Stata output names. Existing distinct aliases may
    still restore case-distinct Stata names. Reject an ambiguous candidate before
    changing the view; retain the public name-error code r(198), rather than
    claiming native r(110) parity. Append retains its existing conservative
    refusal of a generate() name already present in a using source.
    For integer protection, compute min/max in the source type and compare their
    HUGEINT conversions with strict +/-2^53 bounds; UHUGEINT uses an unsigned
    maximum and unsigned bound. Mixed signed/unsigned 128-bit comparisons can
    promote to DOUBLE and miss 2^53+1. DECIMAL casts remain after the extrema:
    this preserves the previous monotone rounding decision, including +/-0.5
    ties, while removing per-row casts and abs(). Null and empty inputs remain
    within the protected range. Exact statistical accumulators are unchanged.
    Floor nanoseconds by dividing before correcting negative remainders, so
    finite values near INT64_MIN do not underflow. An environment buffer cap
    that cannot be represented in bytes follows the existing invalid-input
    automatic fallback, with no signed multiplication overflow.
    Regression oracles include native Stata, Python integer/binary64 arithmetic,
    PyArrow timestamps, raw typed DuckDB bindings, and abnormal process exits.
    Validation is Linux-local; neither a new release nor cross-platform runtime
    certification follows from these checks. Preserve the pre-existing local
    missing-key scan optimization unchanged and report its provenance separately.
157. **Missing-key scan contract and 0.2.2 release (2026-09-22).** The
    missing-match note requires missing values on both sides of the same key.
    Count using first, then only the master keys with nonzero using counts;
    retain a position map to preserve note names, counts and requested order.
    With no using missings, m:1 and joinby no longer execute the master just
    for this diagnostic. A master execution error is therefore deferred to
    collect/save, which still fail loudly and preserve the in-memory dataset.
    Master uniqueness checks for 1:1 and 1:m remain eager. If both missing-count
    queries fail, the using error now wins. These timing/diagnostic changes
    are explicit release behavior, not a promise that every lazy verb avoids
    validation queries. Regression v121 covers composite subsets, reversed
    key order, exact payloads and both early and deferred failures.
    The release includes the previously local MISSKEY-SCAN-1 change alongside
    the validated audit fixes. Build and publish GitHub-produced plugins only;
    verify the exact staged package and public installation after all four
    platform builds pass. Preserve the original dirty checkout and old tags.
158. **Help/menu contract for 0.2.2 (2026-09-22).** The two help files
    distinguish the default storage of numeric gen (double) from untyped egen
    (the aggregate's inferred engine type until materialisation). The shared
    dialog uses the label (default) and still omits the type via isdefault();
    choosing double continues to emit an explicit double. The shared selector
    explains that string types apply only to gen; egen refuses them in a
    dialog stopbox before command submission. A command-level
    regression verifies gen, inferred count/min and explicit double count.
    Help covers name identities and atomic refusals, full 128-bit boundaries,
    the existing DECIMAL threshold rounding, negative nanosecond flooring,
    malformed environment limits and lazy join error timing. The seven engine
    settings and current input/materialisation options remain unchanged.
159. **Final temporal boundary gate (2026-09-22).** The pre-tag review
    reproduced the subtraction-before-division overflow for finite
    TIMESTAMP(us), not only TIMESTAMP_NS. Divide first in ts_ms_sql, then
    subtract the negative-remainder indicator. Preserve the independent
    binary64 gate and the conservative int64 policy on lazy integer counts.
    A millisecond-floored instant outside timestamp_us cannot be saved;
    that refusal must preserve the destination and report a range error.
    Pinned headers and C API checks identify DATE +/-INT32_MAX and timestamp
    +/-INT64_MAX as infinities. Nanosecond epoch_ns copies those sentinel
    bits without filtering them; isfinite and VARCHAR confirm their meaning.
    Therefore reject infinite dates/timestamps on both read paths, with an
    early guard before NS conversion. The minimum finite NS test uses
    -INT64_MAX+1, while separate tests require the two sentinels to fail.
    Raw fill guards add no scan, and lazy guards share the existing boundary
    expression. v122/v123 compare finite payloads to Python/PyArrow and
    verify error messages, dataset atomicity and existing-destination hashes.
160. **Vectorized temporal conversion (2026-09-22).** Nested SQL
    infinity/floor/precision guards passed correctness but failed the temporal
    performance gate. Four private scalar functions now perform each boundary
    conversion and its validation once per input cell. DATE/us/ns inputs use
    the public C API structs and flattened chunks, preserve NULL validity,
    reject infinity sentinels and use integer quotient/remainder arithmetic.
    The us-to-ms path retains the binary64 exactness gate; ns-to-ms counts
    are always within 2^53. No materialized intermediate or extra scan is added.
    DuckDB 1.5.3 has no C API error-mode setter: its scalar handle is a verified
    ScalarFunction pointer, so registration uses its public SetFallible()
    before copying into the catalog. Keep these four functions fallible and
    deterministic; the former SQL error() boundaries could throw too. This
    pinned representation must be reviewed on a DuckDB upgrade. Scalar C API
    flattening and callback error-message ownership were checked in source.
    Seventy filter/projection cases match the SQL-guard candidate exactly;
    independent temporal payload, boundary and ABBA performance gates passed.
161. **Compiler-independent binary64 gate (2026-09-22).** Windows CI
    reproduced successful conversion of the two odd millisecond counts that
    must fail the temporal precision contract. A floating cast round-trip is
    not a reliable predicate under the release compiler's optimizations.
    Use an integer-only representability test: compute unsigned magnitude,
    find the low bits discarded beyond the 53 significant binary64 bits,
    and require them all to be zero. Unsigned subtraction handles INT64_MIN
    without signed overflow. Share this gate between eager timestamp filling
    and the vectorized us-to-ms kernel. Keep the failing Windows assertions;
    add direct positive/negative thresholds and signed-int64 endpoint tests.
162. **Native temporal execution (2026-09-22).** Keep the four temporal
    declarations and callback metadata in the C API registration, but use
    DuckDB's native UnaryExecutor for their execution. It consumes constants,
    flat vectors and selections without the C API's unconditional Flatten().
    Both ScalarFunction::SetFallible and CAN_THROW_RUNTIME_ERROR remain:
    a dictionary entry excluded by a selection must not raise an error.
    Types, NULLs, infinity sentinels, integer floors and the binary64 gate
    retain the 0.2.2 contract. These callbacks need neither C API bind data
    nor local execution state; the catalog retains the shared function_info.
    VerifyDuckDBScalarAPI.cmake checks the pinned version and scalar C API
    source before allowing the handle/callback integration to build. Review
    that contract explicitly when upgrading DuckDB. Tests exercise validity
    word/chunk boundaries, selected infinities and recovery after errors.
163. **Precision must inspect payloads (2026-09-22).** DuckDB 1.5.3 can
    replace bare MIN/MAX aggregates with footer extrema marked exact. A
    forged BIGINT maximum of 2^53, with a real value of 2^53+1, exposed a
    0.2.2 regression: refuse could succeed with rounding and round could
    omit its note. Positive/negative BIGINT, UBIGINT and DECIMAL probes
    reproduce the issue. Keep casts after the extrema, but include first(1)
    as an extra final SELECT field whenever the precision predicate is
    present. Its constant update is O(1) per chunk and prevents the pinned
    metadata-only aggregate substitution. It has no response slot: ignore
    the extra result in C++, not through an outer SQL projection, which
    would let the optimizer remove it. The portable C++ gate checks this
    engine behavior; v124 verifies refusal, exact text, rounding disclosure
    and dataset atomicity against payloads checked with PyArrow. This does
    not extend trust in footers or change the existing 32-bit sizing policy.
164. **Copied views share the plan, not the data (2026-09-25, VIEW-COPY-1;
    extends #21 and #74).** `parqit use [varlist] using view:<source>,
    name(<target>)` assigns a copy of the source `View` (scan, stages, column
    manifest, labels, characteristics, sort keys, pending `keep in` ranges,
    int64 mode, direct-read source paths) to the target, applies an optional
    varlist with `View::keep_vars` exactly as `parqit keep`, and makes the
    target current. The copy is a snapshot of the plan: later verbs on either
    view never reach the other, and both read the same source files when they
    execute. A live reference to the source plan was rejected because one
    view's result would depend on commands issued to another. Pending ranges
    are copied unvalidated and checked when the target materialises, like the
    source's; an unseeded `sample` step keeps the seed already written into its
    stage, so both views draw the same realisation. A copy without a varlist
    of an untouched file view keeps `n_stages()==0` and its source paths, so
    #38's direct-read collect applies unchanged. The target adds a reference
    to every bridge the source depends on, so closing or replacing either view
    never deletes a file the other still uses. `name()` is required:
    defaulting to `default` would silently replace a view, which is what the
    copy exists to avoid. A missing or equal `name()`, an unknown source, a
    varlist naming no column, and the file-reading options (`clear`,
    `relaxed`, `encoding()`, `int64()`, `binary()`, `filename()`, `csv()`)
    are refused before any state changes; the ado settles `view:` before it
    validates or resolves any file option. The copy's source description reads
    `view:<source> (<the source's own description>)`, for display only. The
    form reuses `use` and `view:` instead of a `parqit view copy` verb, because
    Stata's `frame copy` copies data and this operation does not; v125 pins it.
    The view records at open the file columns whose .a-.z it reads as `.`
    (XMISS-1) and a copy repeats that note. `mergein`/`appendin` join memory
    with a file, so a `view:` source is refused by name before their internal
    read, instead of reaching the copy's refusal of `clear` or a missing file.
165. **Audit of VIEW-COPY-1 (2026-09-25; Fable; report in
    `docs/audits/AUDITORIA_FABLE_VIEW_COPY_2026-09-25.md`).** Two pre-existing behaviours
    changed with it. VIEWNAME-ALL-1: `use`, `sql` and `open _data` refuse
    `name(_all)`, because `close _all` reserves the word and such a view could
    not be closed alone. PREFIX-RESTORE-1: `parqit view <name>: <command>`
    restores the previously current view whenever the command leaves another
    view current, including when `<name>` was already current, except when the
    command closed that very view; this is the documented "run, then restore"
    contract, which a prefixed `use ..., name()` used to break. The views
    listing keeps both ends of a long source (display only). The audit's
    suspicion that `sample N, count` could draw differently between executions
    does not hold for the pinned engine: `PhysicalReservoirSample::ParallelSink`
    returns `!repeatable`, parqit always passes REPEATABLE, and
    `Pipeline::ScheduleParallel` returns false for a non-parallel sink, so
    DuckDB runs the whole scan-to-sample pipeline on one thread and the chunks
    arrive in a fixed order. Four repeats over 40 row groups with 8 threads drew
    the same rows; v125 pins the repeat across 30 row groups with 4 threads.
    Recheck both engine functions when upgrading DuckDB.
166. **Sampling designs after sample2 (2026-09-25, SAMPLE-DESIGN-1).**
    `parqit sample # [if] [, count seed() by() cluster() any all generate()]`
    follows Weesie's `sample2` (STB-37 dm46). `if` is the sampling frame: rows
    outside it are kept and never drawn. The condition follows the session's
    missing semantics, as every `keep if` does. Under the default SQL mode a
    missing condition puts the row outside the frame. With `statamissing on`,
    `x > 60` holds for a missing `x`, as in `sample2`'s `mark`. The splitter
    that separates the `if` from the options respects parentheses, double
    quotes and nested compound quotes. The expression translator accepts one
    level of compound quote, as in `keep if`. `by()` stratifies, missing forming
    its own stratum (KEYFOLD-1).
    `cluster()` draws whole clusters, the strata must be constant within them,
    and a cluster split by `if` is an error unless `any` or `all` places it.
    `generate()` adds a 0/1 byte instead of dropping. The existing forms, `#`
    and `#, count` with or without `seed()`, compile exactly as before. Four
    decisions, taken by the implementer at the maintainer's request:
    (1) Counts per stratum keep #134's exact rule, which the plain percentage
    form already uses, rather than `sample`'s `int(n*#/100+.5)` in binary64. One
    verb keeps one rule, and #134 is the maintainer's precision decision. For
    whole-number percentages below about 10^12 units the two agree, because n*#
    is exact and /100 cannot cross a tie. For fractional ones they differ by one
    unit at knife-edge ties: 6,214 of 36.3M grid cases, e.g. 0.3 percent of 500
    gives 1 against 2. No exact rule reproduces `sample` there: rounding the typed
    decimal still differs in 839 cases.
    (2) A missing cluster (`''`, `NULL`, NaN or a Stata missing value) is outside
    the frame and kept, with a note counting the in-frame rows affected, as in
    xsamplefe. `sample2`'s help requires non-missing clusters, while its ado
    silently makes missing one more cluster.
    (3) A cluster's rank is parqit's own function of the seed and the cluster's
    value (engine/sample_key.hpp; numbers by binary64 bits, text by UTF-8
    bytes), not DuckDB's `hash()`. The drawn clusters therefore survive engine
    upgrades and do not depend on row order, file layout or threads. Fixed test
    vectors guard it, and v126 re-derives the draw in Python.
    (4) `keep()` is accepted as a synonym of `generate()`, as in xsamplefe and
    `sample2`.
    Rows (a design without clusters) use the plain percentage form's priority
    over the same row numbers, so `generate()` alone flags exactly the rows
    that form keeps; with `count`, a design keeps # rows per stratum by that
    priority, not by the plain count form's reservoir. The cluster checks run
    once over the current plan when the verb is issued, like merge's key
    checks (sources must stay stable). The cluster path aggregates and joins
    back without materializing its input, reading it twice. `in` is not
    supported: `sample2` refuses it with `by()`, and a lazy view has no
    physical order to cut. v126 compares frames, per-stratum counts and errors
    with native `sample` and `sample2` on the same data. A second Fable audit
    (`docs/audits/AUDITORIA_FABLE_SAMPLE_DESIGN_2026-09-25.md`) led to these
    changes:
    - `generate()`/`keep()` refuse reserved words such as `_n` and `_N`: the ado
      runs `confirm name`, and the engine refuses them, because expressions read
      those words as row context;
    - the indicator's name is excluded from the stage's helper names;
    - `in` and an empty `if` get parqit messages;
    - the documentation now says that integers beyond 2^53 rank by their binary64
      approximation, with ties broken by value;
    - it also says that only the cluster checks validate a pending `keep in` when
      the verb is issued.
