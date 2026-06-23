# parqit — autonomous build brief

You are building **`parqit`**, a Stata package that brings a *grammar of data
manipulation* (Stata-flavoured verbs) to columnar files, executed out-of-core on an
embedded DuckDB engine over Parquet. You will build it **top to bottom**: design,
C++ plugin, Stata `.ado`/`.sthlp`, build system, cross-platform CI, tests, and SSC
packaging. Work in small, verifiable increments and keep the repository green.

This brief is authoritative. Where it states a decision ("must", "do not"), treat it
as fixed and do not relitigate it. Where it is silent, use judgement consistent with
the design thesis and **record assumptions in `ASSUMPTIONS.md`** rather than guessing
silently.

---

## 0. The thesis (read this first, internalise it)

`parqit` is **dbplyr's architecture with Stata's vocabulary, on a DuckDB engine.**

- The user writes ordinary Stata verbs (`keep`, `drop`, `gen`, `replace`,
  `collapse`, `merge`, `append`, `sort`, `reshape`). They are **lazy**: each appends
  to a logical plan on the *current view*.
- `parqit` translates that plan to a single DuckDB SQL query. DuckDB executes it
  **out-of-core** over Parquet (datasets far larger than RAM).
- Only a *materialiser* runs the query: `parqit collect` streams the result into
  Stata's single in-memory dataset; `parqit save` writes the result to Parquet
  **without ever loading it into Stata**.
- SQL is an escape hatch (`parqit sql`, `parqit query`) and a teaching aid
  (`parqit show`/`parqit explain`), never a requirement.

`parqit` is **not** another Parquet reader. Reading/writing is table stakes (pq,
`import parquet`, legacy `stata-parquet` already do it). The product is the
manipulation layer and the two data paths above.

---

## 1. Required reading (do this before writing code)

All three are on this machine.

1. **House style — `/home/mangelo/Documents/GitHub/xhdfe`.**
   This is the maintainer's own Stata with plugin package. Mirror its conventions:
   the `*!` version-banner format at the top of the `.ado`, `.sthlp` (SMCL) layout,
   directory layout, build/Make/CMake style, author/support block, the **license**
   (use the *same* license `parqit`; copy its `LICENSE` text and adjust the year/title),
   and any naming/formatting idioms. `parqit` should look like it came from the same
   hand as `xhdfe`.

2. **Correctness charter — `/home/mangelo/Documents/BPLIM_GitHub/pq_audit_2026-06-11`.**
   A 14-finding + 16-note correctness audit of `pq 3.0`. Read `PQ_AUDIT_REPORT.md`,
   the `issues/`, and especially `verify_suite/`. Every finding is an
   engine-independent hazard of any Stata↔columnar bridge. You must make each one
   **impossible by design** in `parqit` (see §6), and you must **port the
   `verify_suite` pattern** as `parqit`'s regression tests: one self-contained do-file
   per invariant, generating its own synthetic data, asserting the *exact* failure
   signature, and verifying payloads with an **independent oracle** (pyarrow and/or
   duckdb CLI), printing `VERDICT: PASS/FAIL`. Do not trust parqit-only round-trips.

3. **Prior art (optional, for mechanics only) —
   `https://github.com/jrothbaum/stata_parquet_io`.**
   Study *only* for: the SSC `.pkg` format, the cross-platform GitHub Actions
   release workflow (per-OS binary → renamed to `*.plugin` → SSC zip; Linux built on
   an old-glibc base for HPC), and the Stata-plugin fill pattern (ado pre-creates
   variables / sets `obs`, plugin fills them). **Reimplement in C++/CMake/DuckDB —
   do not copy its Rust/Polars stack.** Its read path is solid; its bugs are your
   charter.

---

## 2. Non-negotiable architecture

```
Stata .ado  (verbs → logical plan → DuckDB SQL)
     │   Stata Plugin Interface (stplugin.c/.h, linked directly from C++)
     ▼
parqit  C++ plugin  ──────────────►  embedded DuckDB  (vendored amalgamation)
     │  builds & runs ONE query        │  reads/writes Parquet directly,
     │                                  │  out-of-core, predicate/projection
     ▼                                  ▼  pushdown, joins, windows, pivots
Apache Arrow C Data Interface  ◄──  DuckDB result as Arrow (zero-copy)
     │
     ├── collect → fill Stata's in-memory dataset (SF_* API)
     └── save    → COPY … TO Parquet (Stata memory untouched)
```

Fixed decisions:

- **Engine: DuckDB**, vendored as the single-file amalgamation (`duckdb.hpp` +
  `duckdb.cpp`) or `libduckdb`, pinned to a specific version under `vendor/duckdb/`.
  No external install. Verify the C/C++ API you use against the vendored headers;
  do not call functions from memory.
- **Plugin language: C++**, linking StataCorp's `stplugin.c`/`stplugin.h` directly
  via `extern "C"`. No FFI shim. Entry point registered through Stata's `plugin`
  mechanism.
- **Arrow: C Data Interface only** (the `ArrowArray`/`ArrowSchema`/`ArrowArrayStream`
  structs — header-only ABI). **Do not link Arrow C++. Do not use Arrow's Parquet
  reader** — DuckDB reads Parquet. DuckDB produces Arrow via its C API; use that for
  zero-copy transfer of results into Stata.
- **No Polars in v1.** Keep the verb→plan layer engine-agnostic (a thin internal
  interface) so a Polars/Rust backend could be slotted into specific ops later, but
  implement only the DuckDB backend now.
- **DuckDB session config:** set `threads`, `memory_limit`, and `temp_directory`
  (for spill) from `parqit`-settable globals; default to sensible values and document
  them. Reading the in-memory Stata dataset into DuckDB: for v1, the **temp-Parquet
  bridge** (plugin writes the in-memory data to a temp Parquet, DuckDB scans it) is
  acceptable and robust; the Arrow-C-Data-Interface scan (`duckdb_arrow_scan` or
  equivalent) is the optimisation — correctness first.

---

## 3. Public command surface (the contract)

The command is `parqit`, dispatched on the first token (`gettoken todo 0 : 0`, like
`pq`). Implement these verbs with the semantics shown. Keep this surface **stable**
once published; additive changes only.

**Source / open**
- `parqit use [varlist] using <files>` — open a lazy view over a Parquet file, glob,
  or Hive-partitioned directory (`read_parquet`). With `clear`, read the whole file
  into memory directly (the plain-I/O fast path).
- `parqit open _data` — promote the current in-memory dataset to a view.

**Single-table verbs (lazy; each pushes onto the current view's plan)**
- `parqit keep [varlist]` / `parqit drop [varlist]` → projection
- `parqit keep if <exp>` / `parqit drop if <exp>` → `WHERE` (translate Stata expressions
  to SQL: `&`→`AND`, `|`→`OR`, `==`→`=`, `!`/`~`→`NOT`, `missing(x)`→`x IS NULL`,
  Stata funcs→DuckDB funcs; honour the `statamissing` mode — see §6/§7)
- `parqit gen <type> v = <exp>` ; `parqit egen v = fcn(...) , by()` (window/aggregate)
- `parqit replace v = <exp> [if]` → `CASE WHEN`
- `parqit rename (old) (new)` ; `parqit order <varlist>`
- `parqit sort <varlist>` ; `parqit gsort [-]<varlist>` → `ORDER BY`
- `parqit collapse (stat) v ... , by(groupvars)` → `GROUP BY` + aggregates; support at
  least mean, sum, sd, median/p50, pXX, count, min, max, first, last (match Stata
  `collapse` names and behaviour)
- `parqit contract <varlist>` ; `parqit duplicates drop [varlist]`
- `parqit keep in <range>` → **validated** `LIMIT/OFFSET`
- `parqit sample <n|share> [, seed()]` → reproducible `USING SAMPLE`
- `parqit reshape long|wide ...` → `UNPIVOT`/`PIVOT`

**Two-table verbs (lazy)**
- `parqit merge 1:1|m:1|1:m|m:m <keys> using <file> [, keep() keepusing() gen() nogen]`
  → `JOIN`, producing a Stata-compatible `_merge`; the *using* side stays on disk
- `parqit append using <files>` → `UNION BY NAME`, aligning by name with safe recasts
- `parqit joinby <keys> using <file>` → m:m join

**Materialisers (execute the plan)**
- `parqit collect [, clear]` — execute; stream Arrow result into Stata's dataset,
  **atomically** (build into a temp frame / staged dataset and swap on success;
  never `clear` before the load is known good — charter §6.9)
- `parqit save <dest> [, replace partition_by() compression() compression_level()
  chunk()]` — execute; write Parquet, Stata memory untouched
- `parqit count` → `r(N)` ; `parqit head [n]` / `parqit list [n]`
- `parqit summarize` / `parqit tabulate` → `r()` ; `parqit describe` / `parqit glimpse` →
  schema/types/rows/row-groups returned as **scalars** in `r()` (charter S4-12)

**Escape / introspection**
- `parqit sql "<DuckDB SQL>" [, clear]` ; `parqit query "<sql fragment>"`
- `parqit show` (generated SQL, like dbplyr `show_query()`) ; `parqit explain` (plan)
- `parqit path <file>` (absolute path) ; `parqit set threads|memory_limit|tempdir ...`

---

## 4. Type & metadata contract

Implement an explicit, **tested** type map (and document it in the `.sthlp` and
README). Round-trip every Stata type byte-exact where the format allows.

- Integers `byte/int/long` ↔ `TINYINT/SMALLINT/INTEGER/BIGINT`, sized by range.
- `float/double` ↔ `FLOAT/DOUBLE`, precision preserved (test π, 1e±300, 2^53±,
  signed zero).
- `str#` ↔ `VARCHAR`; `strL` ↔ large `VARCHAR`; respect the 2045-char boundary;
  string length is **bytes, not characters** (UTF-8).
- Dates/times: `%td` ↔ `DATE`; `%tc` ↔ `TIMESTAMP` (ms; tz → UTC instant, documented);
  **`%tm %tq %th %ty %tw` are kept as INTEGER period counts with the format code —
  never mis-scaled to calendar dates** (charter §6.3); any `hh:mm`-style display
  format must be classified before token checks and never produce an all-null
  column (charter §6.5).
- Boolean ↔ `byte` 0/1.
- `DECIMAL(p,s)` → `double` on read (charter §6.11). `UINT32/UINT64` and any
  out-of-range integers → `double` (or `long` when in range), **bound-checked, never
  silent null** (charter §6.6). `LIST/STRUCT/NULL`-type → drop-with-message or error,
  **never a column of silent missings** (charter §6.11).

**Metadata round-trip (a differentiator):** on `parqit save`, write Stata variable
labels, value labels, notes, display formats and characteristics into Parquet
file/column **key–value metadata** under a `parqit.*` namespace; on `parqit use`/`collect`
restore them. The file stays standard Parquet for third parties; `parqit → parqit` is
lossless. Document that extended missings `.a`–`.z` survive only via this metadata.

---

## 5. Identity & index mapping (the root-cause discipline)

Most audit failures trace to losing track of *which column is which*. Enforce:

- A single **column manifest** travels with every operation: for each column carry
  `(source_name, stata_name, dtype, format, position)`. Key the engine by
  **source name**; use the Stata name only when creating the Stata variable
  (charter §6.2). Never let the plugin index the dataset positionally by accident
  (charter §6.1) — pass explicit names/indices on every `plugin call`.
- DuckDB quotes arbitrary identifiers, so keep original names internally and only
  sanitise at the Stata boundary, with a documented, reversible scheme for reserved
  words, leading digits, >32 chars, spaces, and duplicates (charter §6.2, §6.10,
  §6.14). Carry names in compound-quoted lists, never whitespace-tokenised macro
  lists.
- Any internal helper column uses a **tempname**, never a fixed magic name that
  could collide with a user column (charter §6.12).

---

## 6. Correctness charter — invariants that must hold by design

Each maps to an audit finding. For each, write a `verify_suite/`-style do-file that
*would* catch a regression, with an independent oracle.

1. **No positional corruption on subsetting/reordering.** `parqit save` and any
   verb with a varlist write/read exactly the named columns' data, in the named
   order, under the named types — for plain, `if`, `partition_by`, `format(csv/spss)`,
   chunked and streamed paths alike.
2. **Renamed/awkward columns carry their data.** Reserved-word, leading-digit,
   long, space-containing and duplicate column names load with their **values**, not
   as all-missing; round-trip the original name via metadata.
3. **No date mis-scaling.** `%tm/%tq/%th/%ty/%tw` are never written as garbage
   calendar dates; their semantics survive a round-trip.
4. **Append means append.** `partition_by` + chunk writes all chunks; the
   "add a new year without overwriting" workflow never destroys existing rows.
5. **No all-null time columns** from `hh:mm` display formats.
6. **No silent null on integer overflow.** `uint32/uint64`/large ints are
   bound-checked and represented, never turned into missing.
7. **Labels don't corrupt.** Partially labelled variables saved with a `label`
   option never collapse unlabelled values to indistinguishable empties; labelled
   extended missings are not turned into ordinary strings.
8. **Errors are loud.** Every plugin entry returns a real `ST_retcode`; the ado
   checks `_rc`; any write/read failure is a nonzero `rc` **plus** a message. Nothing
   ever fails silently with `rc 0` and a stale/missing file.
9. **Atomic, validate-then-mutate.** `parqit use/collect, clear` never destroys the
   in-memory dataset before the new data is confirmed loadable (stage in a temp
   frame; swap on success).
10. **No silent column loss.** Duplicate names error or are disambiguated with a
    warning; never dropped silently.
11. **Unsupported types are loud.** `decimal128` loads as numbers; truly
    unrepresentable types are dropped-with-message or error, never silent missings.
12. **No internal-name clobber** (see §5).
13. **Validated ranges.** `keep in`/`in()` validate first/last; negative, inverted,
    beyond-EOF and single-value forms either match native Stata or are rejected
    explicitly — never silently return 0 or all rows.
14. **Pathological names don't brick the file** (spaces etc.) — handled via §5.

Also honour the S4 notes: correct `c(changed)`/`c(filename)` after `use`/`save`;
`r()` describe results as scalars; no stray debug output; documented codec behaviour;
no leftover frames/variables on a failed operation (guard with `preserve`/temp
frames).

---

## 7. Engineering invariants (general)

- **Stata expression → SQL** translation is a focused, unit-tested module. Default to
  SQL missing semantics; implement a `statamissing` mode that emulates Stata's
  "missing is larger than any value" ordering. Document the difference.
- **Resource hygiene:** open/close the DuckDB connection cleanly; set spill
  `temp_directory`; return memory aggressively on Linux (HPC/cgroup friendliness).
- **Determinism:** seeded sampling is reproducible; no nondeterministic column order.
- **No fabrication:** never invent benchmark numbers, API signatures, or behaviour.
  If unsure of a DuckDB/Arrow/Stata API, read the vendored header or the local Stata
  docs; if still unsure, record it in `ASSUMPTIONS.md` and choose the conservative
  option.

---

## 8. Build, vendoring, packaging, CI

- **Build system: CMake.** Targets: the `parqit` plugin (shared library) linking
  `stplugin.c` and the vendored DuckDB amalgamation. Provide a top-level
  `CMakeLists.txt`, a `cmake --preset` for each platform, and a one-command local
  build documented in `BUILDING.md`.
- **Vendoring:** pin DuckDB under `vendor/duckdb/` (amalgamation committed or fetched
  at a pinned tag); commit `stplugin.c`/`stplugin.h`; commit the Arrow C Data
  Interface header (`abi.h`-style). Record versions in `vendor/VERSIONS.md`.
- **Cross-platform CI** (`.github/workflows/build.yml`), modelled on pq's release job
  but C++/CMake:
  - Matrix: Linux x86_64, macOS (build **both** `x86_64` and `arm64`), Windows
    x86_64 (MSVC).
  - **Build Linux against an old glibc** (e.g. inside `almalinux:8`/manylinux) so the
    `.so` runs on EL-family HPC clusters. macOS sets a low deployment target.
  - Package each platform's binary, **renamed to `parqit.plugin`**, alongside
    `parqit.ado`, `parqit.sthlp`, `parqit.pkg`; build the per-platform zips and an
    **SSC zip** containing all platform binaries + the Stata files.
  - On `v*` tags, create a GitHub Release with the zips and generated notes.
- **SSC manifest `parqit.pkg`** following the `d`/`f`/`g`/`h` format, with `g LINUX64`,
  `g MACINTEL64`, `g MACARM64`, `g WIN64` mapping each binary to `parqit.plugin` and an
  `h parqit.plugin` line. Fill author/support/keywords/`Distribution-Date`.

---

## 9. Testing & definition of done

- **Unit tests** (C++): the type map, the Stata-expr→SQL translator, the identifier
  sanitiser, the manifest, the metadata (de)serialiser.
- **Stata integration suites** (`tests/`): one do-file per verb and per data path
  (collect vs save), plus the **ported `verify_suite/`** (all 14 invariants + S4),
  each printing `VERDICT: PASS/FAIL`, each verified against pyarrow/duckdb oracles.
  A `run_all.sh`/`run_all.do` runs everything and exits nonzero on any FAIL.
- **Round-trip property tests:** every Stata type, with/without missings; 0 rows,
  1 row, 1 var, very wide (2500+ vars), multi-row-group; UTF-8/emoji; pathological
  names.
- **CI must be green on all three OSes** before any release tag.
- **Done** = the §3 surface implemented; §4–§7 honoured; §6 invariants each covered
  by a passing verify test; CI green; README/`.sthlp`/`BUILDING.md`/`CHANGELOG.md`
  current; `parqit.pkg` valid; a tagged release produces installable per-platform and
  SSC zips.

---

## 10. Repository layout (target)

```
parqit/
├─ README.md                 (already present)
├─ LICENSE                   (match xhdfe's license)
├─ CITATION.cff
├─ CHANGELOG.md
├─ BUILDING.md
├─ ASSUMPTIONS.md
├─ CMakeLists.txt
├─ CMakePresets.json
├─ .github/workflows/build.yml
├─ src/
│  ├─ plugin/                (C++: entry, dispatch, SF_* bridge, Arrow transfer)
│  ├─ engine/                (DuckDB session, SQL builder, type map, manifest)
│  └─ ado/s/                 (parqit.ado, parqit.sthlp, parqit.pkg)
├─ vendor/                   (duckdb/, stplugin.*, arrow C-data header, VERSIONS.md)
└─ tests/
   ├─ verify_suite/          (ported audit invariants + run_all)
   ├─ roundtrip/  unit/  integration/
   └─ fixtures/              (small committed .parquet/.dta)
```

---

## 11. Milestones (build in this order; commit at each)

- **M0 — Skeleton & build.** Repo layout, CMake, vendored DuckDB + stplugin, a
  trivial plugin that loads in Stata and echoes; CI builds the plugin on all three
  OSes. `parqit` dispatches subcommands (stubs).
- **M1 — I/O + type fidelity.** `parqit use/save/describe` for plain whole-file
  read/write with the full type map and metadata round-trip; the column manifest;
  loud errors; atomic `clear`. Round-trip + type verify tests pass. Charter §6.1–6.3,
  6.5–6.12 covered for the I/O path.
- **M2 — Single-table verbs.** Lazy view + plan; `keep/drop/keep if/gen/replace/`
  `rename/order/sort/gsort/collapse/contract/duplicates/keep in/sample`; the
  Stata-expr→SQL translator (+`statamissing`); `collect`/`save`/`count`/`head`/
  `show`/`explain`. §6.13 covered.
- **M3 — Two-table verbs.** `merge` (all kinds, `_merge`), `append` (union by name),
  `joinby`; out-of-core, using side on disk.
- **M4 — Power & polish.** `reshape long/wide`; `parqit sql`/`query`; `summarize`/
  `tabulate`; `set` config; full `.sthlp`.
- **M5 — Release.** SSC `.pkg`, release workflow producing per-platform + SSC zips;
  CHANGELOG; tagged `v0.1.0`.

---

## 12. Working agreement

- Small, focused commits; **Conventional Commits** messages; never commit a state
  that breaks the build. Keep `CHANGELOG.md` updated.
- Work on a feature branch per milestone; open a PR-style summary at each.
- Run the test suite locally before pushing. The maintainer develops on **macOS** and
  targets **Linux/HPC** (Singularity/HTCondor later) — keep everything portable and
  dependency-free at the user's end.
- Do **not** change the §3 public surface without flagging it in `CHANGELOG.md` and
  `ASSUMPTIONS.md`.
- Match `xhdfe`'s author/version-banner and license exactly.

## 13. First actions

1. Read the three sources in §1.
2. Write `ASSUMPTIONS.md` (DuckDB version pinned, Arrow transfer approach, Stata-data
   →DuckDB bridge choice for v1, license confirmed from `xhdfe`).
3. Scaffold M0 (layout, CMake, vendored deps, trivial loading plugin, CI) and get CI
   green on all three OSes.
4. Then proceed M1→M5, with passing tests at every step.

Build carefully, test against independent oracles, and keep `parqit` honest: loud on
failure, lossless on round-trip, and out-of-core by default.
