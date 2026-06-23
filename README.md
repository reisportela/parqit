# parqit — a grammar of data manipulation for Stata, backed by Parquet

[![Build](https://github.com/reisportela/parqit/actions/workflows/build.yml/badge.svg)](https://github.com/reisportela/parqit/actions)
![Stata 16+](https://img.shields.io/badge/Stata-16%2B-blue)
![Platforms](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey)
![License](https://img.shields.io/badge/license-MIT-green)

`parqit` lets you read, write, join and **manipulate** columnar data files in Stata using
ordinary Stata verbs — `keep`, `drop`, `gen`, `replace`, `collapse`, `merge`,
`append`, `sort`, `reshape` — that run **out-of-core** on an embedded
[DuckDB](https://duckdb.org) engine over Parquet, and materialise **one result
table at a time** into Stata's memory.

It is, in one line, **dbplyr's architecture with Stata's vocabulary**: you write
Stata-flavoured verbs, `parqit` translates them to a DuckDB query, the engine executes
it lazily on disk (datasets far larger than RAM), and only the final result is
brought into Stata — or written straight back to Parquet without ever touching
memory. SQL is available for power users, but no one has to learn it.

> **Status:** v0.1.7 — the full surface below is implemented and covered by a
> correctness suite (C++ unit tests run against the embedded engine; Stata
> integration and audit-derived verify suites run against StataNow MP with
> pyarrow/duckdb as independent oracles). `parqit` is **not** affiliated with
> StataCorp.

> **About.** The conceptual design of `parqit` is by **Miguel Portela** — taking
> [`pq`](https://github.com/jrothbaum/stata_parquet_io) as the starting point and
> re-basing the manipulation layer on an embedded **DuckDB** engine through a
> **C++** plugin; the implementation was programmed by two AI coding agents,
> OpenAI's **Codex** and Anthropic's **Claude Code**, under his direction and
> review. `parqit` is an **ongoing project**, provided **"as is", without warranty
> of any kind** (see [LICENSE](LICENSE)); **feedback, issues and pull requests are
> very welcome**.

## Why parqit

Reading and writing Parquet in Stata is already well served — by
[`pq`](https://github.com/jrothbaum/stata_parquet_io), by the legacy
`stata-parquet`, and by StataCorp's own `import parquet`. `parqit` is **not another
reader**. Its identity is the layer above I/O:

- **Manipulation, not just transfer.** A full set of single-table and two-table
  verbs that compile to one DuckDB query and run before anything is loaded.
- **Out-of-core by default.** Filter, join, aggregate and reshape billion-row files
  on a laptop; DuckDB spills to disk transparently. The result that lands in Stata
  is only ever the *output* of the pipeline, not the input.
- **Two first-class data paths.** Small result → Stata's in-memory dataset
  (`parqit collect`); large transformation → **Parquet → Parquet, never touching Stata
  memory** (`parqit save`). The second is where the out-of-core story actually pays off.
- **Lossless metadata round-trips.** Variable labels, value labels, notes, display
  formats and characteristics survive a `parqit save` / `parqit use` cycle (stored in
  standard Parquet key–value metadata), while remaining plain Parquet for pandas,
  polars, R, Spark and friends. (One documented exception: extended-missing
  *categories* `.a`–`.z` collapse to a single `.` — their labels survive; see
  Limitations.)
- **Learn the SQL if you want to.** `parqit show` prints the generated query
  (like dbplyr's `show_query()`); `parqit explain` shows the plan; `parqit sql "…"`
  drops you to raw DuckDB.

## Installation

**Requirements:** Stata 16 or newer (MP recommended for large data). The compiled
plugin embeds DuckDB; there are **no external library dependencies** to install.

> The compiled plugin (`parqit.plugin`, ~40 MB) is **not** stored in the git tree —
> cloning alone does not give you a working command. Pick one of the two routes
> below.

### Option 1 — install with `net install` (no compiler needed)

`parqit` ships as a standard Stata package: download the zip for your platform from
the [latest release](https://github.com/reisportela/parqit/releases), extract it
into a folder, and run `net install` from that folder. This release covers
**Linux x86_64**, **Windows x86_64** and **Apple-Silicon macOS (arm64)**;
`parqit_all_platforms.zip` bundles all three. `net install` reads `parqit.pkg`, picks
the right binary for your machine, and installs it as `parqit.plugin` onto your
`PLUS` adopath (run `sysdir` in Stata to see where). `replace` upgrades an
existing install in place; `ado uninstall parqit` removes it. Each zip holds the
package files at its root, so always extract into a dedicated folder and point
`from()` at that folder (the one containing `parqit.pkg`).

**Linux (x86_64)** — runs on EL7/EL8+, Ubuntu 18.04+ and HPC clusters (the binary
needs only glibc 2.25):

```bash
cd ~/Downloads
mkdir -p parqit_pkg && unzip parqit_linux_x86_64.zip -d parqit_pkg
```
```stata
. net install parqit, from("/home/<you>/Downloads/parqit_pkg") replace
. parqit version        // confirms the plugin loaded
. parqit selftest       // end-to-end self-check, prints "ok"
```

**macOS — Apple Silicon (arm64: M1/M2/M3/M4):**

```bash
cd ~/Downloads
mkdir -p parqit_pkg && unzip parqit_macos_arm64.zip -d parqit_pkg
```
```stata
. net install parqit, from("/Users/<you>/Downloads/parqit_pkg") replace
. parqit version
. parqit selftest
```

(If macOS Gatekeeper quarantines the binary, clear it once with
`xattr -dr com.apple.quarantine ~/Downloads/parqit_pkg`.)

**Windows (x86_64)** — right-click `parqit_windows_x86_64.zip` → *Extract All…*
(this creates a `parqit_windows_x86_64\` folder), then in Stata use forward slashes
in the path:

```stata
. net install parqit, from("C:/Users/<you>/Downloads/parqit_windows_x86_64") replace
. parqit version
. parqit selftest
```

> The compiled plugin is deliberately **not** committed to the git tree, so
> `net install` from a GitHub raw URL is not available — install from a release
> zip as above.
>
> **macOS Intel (x86_64)** is not in this release yet (the CI Intel-Mac runner was
> unavailable); build from source (Option 2) for an Intel Mac in the meantime. A
> binary you build yourself on a newer Linux (glibc ≥ 2.34) will **not** run on an
> old-glibc HPC cluster (EL7/EL8) — use the AlmaLinux-8 binary from the release
> there.

### Option 2 — clone and build from source

Needs `git`, CMake ≥ 3.16 and a C++17 compiler (gcc ≥ 10, clang, or MSVC).
The first build downloads and compiles DuckDB 1.5.3 from source
(SHA256-pinned), so expect 10–20 minutes and a few GB of disk the first time.

```bash
git clone https://github.com/reisportela/parqit.git
cd parqit
cmake --preset dev
cmake --build build/dev -j
```

Every build refreshes the repo-local install tree **`ado/plus/p/`**
(ado, help, pkg and freshly stripped plugin — nothing is written to your
`~/ado`). Then point Stata at it:

```stata
. adopath ++ "/path/to/parqit/ado/plus/p"
. help parqit
```

To make that permanent, add the `adopath` line to your `profile.do`
(see `help profilew`). To update later: `git pull`, rebuild, restart Stata.

**From SSC** (planned): `ssc install parqit`.

## Quick start

```stata
* Open a lazy view over one or many Parquet files (nothing is read yet)
parqit use using /data/qp_*.parquet

* Build a pipeline with ordinary Stata verbs — still lazy
parqit keep if year >= 2010 & inrange(age, 25, 64)
parqit keep   id firm year wage
parqit gen     double lwage = log(wage)
parqit collapse (mean) lwage (count) n = wage, by(firm year)

* Inspect the DuckDB SQL parqit generated for you (optional, great for learning)
parqit show

* Materialise the result into Stata's memory…
parqit collect, clear

* …or write it straight to Parquet without ever loading it
parqit save firm_year_panel.parquet, replace
```

Plain I/O works exactly as you would expect, and the format is inferred from the
extension:

```stata
parqit use  mydata.parquet, clear          // read whole file into memory
parqit save mydata.parquet, replace        // write the in-memory dataset
parqit describe mydata.parquet             // schema, types, rows, row groups
```

## The verb grammar

Every verb appends to the **current view** (an implicit lazy table, just like Stata's
implicit current dataset). Nothing executes until a *materialiser* runs.

### Open / source

| Command | Compiles to | Notes |
|---|---|---|
| `parqit use [varlist] using <files>` | `read_parquet(...)` / `read_csv_auto(...)` | Parquet file/glob/Hive dir, or delimited text (`.csv`/`.tsv`/`.txt`), or a Stata `.dta` / Excel `.xls`/`.xlsx` (imported to a Parquet bridge). With `clear`, reads into memory. `relaxed` unions a mixed-schema glob by column name. |
| `parqit open _data` | scan of current dataset | Promote the in-memory dataset to a view to keep manipulating it out-of-core. |

**Input formats.** Parquet and delimited text are scanned *out of core* (the
file can exceed memory). Stata `.dta` and Excel `.xls`/`.xlsx` are not
engine-scannable, so parqit imports them into a throwaway frame (your data is
untouched) and snapshots them to a small Parquet *bridge* — ideal for a small
lookup, but for a large `.dta` master prefer `use` + `parqit open _data`. The same
extension rule applies to a `using` side of `merge`/`joinby`/`append`, so a
lazy Parquet master can join a `.dta` lookup and only the result is collected.

### Single-table verbs (lazy)

| Command | Compiles to |
|---|---|
| `parqit keep [varlist]` / `parqit drop [varlist]` | `SELECT` projection |
| `parqit keep if <exp>` / `parqit drop if <exp>` | `WHERE` (Stata expression → SQL, with documented missing-value semantics) |
| `parqit gen <type> v = <exp>` | computed column |
| `parqit egen v = <fcn>(...) , by()` | window / aggregate column |
| `parqit replace v = <exp> [if]` | `CASE WHEN` |
| `parqit rename (old) (new)` | column alias |
| `parqit order <varlist>` | column order |
| `parqit sort <varlist>` / `parqit gsort [-]<varlist>` | `ORDER BY` |
| `parqit collapse (stat) v ... , by()` | `GROUP BY` + aggregates (mean/sum/sd/median/pXX/count/min/max/first/last) |
| `parqit contract <varlist>` | grouped counts |
| `parqit duplicates drop [varlist]` | `DISTINCT` / dedup |
| `parqit keep in <range>` | validated `LIMIT/OFFSET` |
| `parqit sample # [, count seed()]` | reservoir sample: percent by default, rows with `count`; reproducible with `seed()` |
| `parqit reshape long\|wide ...` | `UNPIVOT` / `PIVOT` |

### Two-table verbs (lazy)

| Command | Compiles to |
|---|---|
| `parqit merge 1:1\|m:1\|1:m\|m:m <keys> using <file\|view:name> [, keep() keepusing() gen()]` | `JOIN`, with a Stata-compatible `_merge`; the *using* side stays on disk — a file or **another open view** |
| `parqit append using <files\|view:name ...>` | `UNION BY NAME`, aligning columns by name with safe recasts; sources may be files or views |
| `parqit joinby <keys> using <file\|view:name>` | many-to-many join |

**In-memory + disk, fast.** When your data is already in Stata's memory and you
want to join a disk file (a small lookup), `parqit mergein`/`parqit appendin` keep
the in-memory data put and run a *native* `merge`/`append`, reading only the
needed columns of the disk side — no DuckDB round-trip. On 10M ⋈ 500k this is
~3.4 s vs ~9.6 s for the `parqit open _data` bridge. For big-on-big, prefer the
out-of-core `parqit use … ; parqit merge` path.

| Command | Effect |
|---|---|
| `parqit mergein 1:1\|m:1\|1:m\|m:m <keys> using <file> [, <merge opts>]` | Native `merge` of the in-memory data with a disk lookup (read via parqit) |
| `parqit appendin using <file> [, keep()]` | Native `append` of a disk file onto the in-memory data |

### Materialisers (run the pipeline)

| Command | Effect |
|---|---|
| `parqit collect [, clear]` | Execute once; stream the result into Stata's memory atomically. The view stays open (collecting again re-executes). |
| `parqit save <dest> [, replace partition_by() compression() chunk()]` | Execute; write Parquet **without loading into Stata**. |
| `parqit count` | Row count → `r(N)` (no rows materialised). |
| `parqit head [n]` / `parqit list [n]` | Preview a small slice. |
| `parqit summarize` / `parqit tabulate` | Pushed-down summaries → `r()`. |
| `parqit describe` / `parqit glimpse` | Schema, types, rows, row groups → **scalars** in `r()`. |

### Escape hatches

| Command | Effect |
|---|---|
| `parqit sql "<DuckDB SQL>" [, clear]` | Run raw DuckDB SQL; collect or save the result. |
| `parqit query "<sql fragment>"` | Inject a raw fragment into the current pipeline (e.g. a `QUALIFY`). |
| `parqit show` / `parqit explain` | Print the generated SQL / the query plan. |

**Tuning the read.** Reads of 50,000+ rows fill Stata's memory in parallel (up
to `min(cores, 8)` worker threads), because that per-cell fill dominates the
cost. To force the single-threaded path — e.g. on a platform you have not yet
verified — set the **operating-system** environment variable
`PARQIT_FILL_THREADS=0` *before launching Stata* (`export PARQIT_FILL_THREADS=0` in
your shell; the plugin reads it via `getenv`, so a Stata `global` will not reach
it). `PARQIT_FILL_THREADS=n` pins `n` workers. The parallel and serial fills are
byte-identical — only the scheduling differs.

## Examples

```stata
* Out-of-core firm-year panel from raw worker-level microdata
parqit use using /data/qp_2002_2023/*.parquet
parqit keep if !missing(firmid) & wage > 0
parqit collapse (mean) wage (sd) sd_wage = wage (count) emp = id, by(firmid year)
parqit save firm_panel.parquet, replace partition_by(year)

* Join firm characteristics that live on disk (no in-memory using dataset)
parqit use using firm_panel.parquet
parqit merge m:1 firmid year using /data/scie.parquet, keep(match master) keepusing(tfp k)
parqit collect, clear

* Reshape a wide file too big for Stata's reshape
parqit use using wide_income.parquet
parqit reshape long inc, i(id) j(year)
parqit save long_income.parquet, replace

* Drop to SQL when a window function is clearest
parqit use using spells.parquet
parqit query "qualify row_number() over (partition by id order by start) = 1"
parqit collect, clear
```

## Tour & examples

`examples/` ships a complete, self-verifying tour of every feature over
small artificial datasets (workers panel, firms, patents, wide incomes and
a deliberately hostile file with reserved/duplicate/space column names,
uint32 overflow values and decimals):

```stata
. cd examples
. do parqit_tour.do                  // parqit installed on the adopath
. do parqit_tour.do <repo> <plugin>  // development tree
```

The data is generated by `examples/make_data.py` (pyarrow, deterministic)
on first run. Because the datasets are small, the tour loads a native twin
into memory and uses Stata itself as the oracle for every lazy result —
each of its eleven sections both demonstrates the commands and asserts
their correctness, ending in `VERDICT(PARQIT_TOUR): PASS`.

## Type mapping

`parqit` keeps an explicit, tested map between Stata types/formats and DuckDB/Arrow
logical types. It never silently nulls a value on overflow and never silently
rescales a date.

| Stata | DuckDB / Arrow | Notes |
|---|---|---|
| `byte` `int` `long` | `TINYINT` `SMALLINT` `INTEGER`/`BIGINT` | sized by range |
| `float` `double` | `FLOAT` `DOUBLE` | precision preserved; float32 beyond Stata's ±1.70e38 float ceiling widens to `double` (noted, never silently missing); NaN loads as missing, ±Inf loads as missing **with a note** |
| `str#` | `VARCHAR` | auto-sized on read |
| `strL` | `VARCHAR` (large) | >2045 chars |
| `%td` | `DATE` | days since epoch (correct conversion) |
| `%tc` | `TIMESTAMP` | milliseconds; tz instant preserved |
| `%tm %tq %th %ty %tw` | `INTEGER` (period count) | **kept as integers with the period code**, never mis-scaled to calendar dates |
| boolean | `BOOLEAN` → `byte` 0/1 | |
| `DECIMAL(p,s)` | → `double` on read | warehouse money types load as numbers, not missing |
| `UINT32` `UINT64` | → `double`/`long` (bound-checked) | values above the signed range never become missing |
| `LIST` `STRUCT` | error or drop-with-message | unrepresentable types are loud, never silent all-missing |

Unsigned integers, decimals and out-of-range values are bound-checked; unsupported
types are reported, never loaded as a column of silent missings.

## Stata metadata round-trip

A `parqit save` writes Stata's variable labels, value labels, notes, display formats
and characteristics into Parquet **key–value metadata** under a `parqit.*` namespace.
`parqit use` restores them. The file stays 100% standard Parquet for every other tool.
A `parqit → parqit` cycle preserves every value, type and metadatum exactly, with one
documented exception: extended-missing *categories* (`.a`–`.z`) collapse to a single
`.` (their value labels still survive) — see Limitations.

## Limitations

- **Views are plans, not data** — open as many as you like
  (`parqit use using f.parquet, name(qp)`, `parqit view qp`, `parqit views`);
  `parqit collect` materialises the current view and keeps it open
  (re-collecting re-executes). With a view open, `parqit save` materialises
  the current view (and says so); `parqit save ..., data` exports the
  in-memory dataset instead.
- **Stata `if` vs SQL semantics.** By default, expressions follow SQL semantics
  (missing is `NULL`, not "larger than everything"); `x < .`-style idioms are
  translated faithfully either way. `parqit set statamissing on` emulates Stata's
  ordering in every comparison where it matters.
- **Extended missings** `.a`–`.z` collapse to a single null in Parquet (the
  format has one missing concept); `parqit save` warns when this loses
  information. Labels attached to extended missings do survive (they live in
  `parqit.*` metadata).
- **`merge m:m`** reproduces Stata's sequential pairing exactly — and, as in
  Stata, is almost never what you want; use `parqit joinby`.
- **Binary strLs** are refused on save (text strLs round-trip).

## Acknowledgements

`parqit` stands on the shoulders of [DuckDB](https://duckdb.org), the
[Apache Arrow C Data Interface](https://arrow.apache.org/docs/format/CDataInterface.html),
and [`pq`](https://github.com/jrothbaum/stata_parquet_io) by Jon Rothbaum, whose
correctness work directly informed `parqit`'s test suite.

`parqit` was built with the assistance of two AI coding agents used in tandem —
Anthropic's **Claude** (via Claude Code) and OpenAI's **Codex** — for
implementation, adversarial cross-auditing and cross-platform release work, under
the author's direction and review. Both contributed to the making of `parqit`.

## License

MIT — see [LICENSE](LICENSE).

## Citation

If you use `parqit` in published work, please cite it (a `CITATION.cff` is included).
Author: **Miguel Portela** · Universidade do Minho & NIPE (miguel.portela@eeg.uminho.pt).
