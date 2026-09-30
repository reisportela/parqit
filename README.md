# parqit — a grammar of data manipulation for Stata, backed by Parquet

[![Build](https://github.com/reisportela/parqit/actions/workflows/build.yml/badge.svg)](https://github.com/reisportela/parqit/actions)
![Stata 16+](https://img.shields.io/badge/Stata-16%2B-blue)
![Platforms](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey)
![License](https://img.shields.io/badge/license-MIT-green)

`parqit` lets you **explore** and **manipulate** columnar data files in Stata —
not just read, write, merge or append them. Ordinary Stata verbs — `keep`,
`drop`, `gen`, `replace`, `collapse`, `pivot`, `merge`, `append`, `sort`,
`reshape` — run **out-of-core** on an embedded [DuckDB](https://duckdb.org)
engine over Parquet, and materialise **one result table at a time** into
Stata's memory. Files far larger than RAM can be profiled, filtered, joined
and aggregated without first loading the full source into the current dataset.

It is, in one line, **dbplyr's architecture with Stata's vocabulary**: you write
Stata-flavoured verbs, `parqit` translates them to a DuckDB query, and the engine
executes it lazily on disk (datasets far larger than RAM). The pipeline result
enters Stata's current dataset only when collected, or it can be written straight
back to Parquet without loading that result into the current dataset. SQL is
available for power users, but no one has to learn it.

> **Status:** v0.3.1 — the full surface below is implemented and covered by a
> correctness suite (C++ unit tests run against the embedded engine; Stata
> integration and audit-derived verify suites run against StataNow MP with
> pyarrow/duckdb as independent oracles). `parqit` is **not** affiliated with
> StataCorp.

The scoped evidence, closed findings, residual risks and institutional-use
conditions for the current data-reliability baseline are recorded in the
[v0.1.22 technical GO-GO reliability report](docs/audits/CERTIFICACAO_GO_GO_FIABILIDADE_DADOS_PARQIT_2026-07-14.md);
the full audit evidence chain is indexed in [docs/audits/](docs/audits/README.md).

Version 0.3.1 answers a second round of feedback from BPLIM users. Subcommands
and options take the abbreviations native Stata accepts (`parqit su price, d`,
`parqit ta`, `parqit d`, `parqit g`, …; see [The verb grammar](#the-verb-grammar)).
Rows that tie on a declared sort keep the order they have in the files, so
`_n`, `keep in`, `list in`, `collect` and the Obs of `duplicates list` agree
with native Stata (auto, sorted by `foreign` alone, is the classic case). Every
statistics command takes `[if exp]`; `append` takes `keep()` and `appendin`
`generate()`, labelled as native; `duplicates report|list`, `correlate` and
`pwcorr` run on every variable without a varlist; `distinct` gains `missing`;
`parqit sql` names a reserved word such as `foreign` and shows its quoting;
`save …, partitions()` names each difference it refuses; `tabulate` lays its
tables out as native does (a long variable label wraps instead of widening the
table); and a red warning —
silenced by `quietly`, or for the session by `parqit set statamissing on|off` —
marks each comparison whose result can differ from native Stata because a
value is missing. Some behaviours change (see the [changelog](CHANGELOG.md)):
`distinct`'s Obs counts the observations used, as the community-contributed
`distinct` does; `duplicates list` prints Group, Obs and the key variables, as
native, instead of every column; and a first word such as `sum` or `pat` after
`parqit misstable` is the subcommand, as native. The metadata format is
unchanged. `help parqit` is now a user's guide laid out as Stata's own help
files, opening with what parqit is for and a diagram of its four moves; the
contracts and the complete list of limitations are in `help parqit_technical`.

Version 0.3.0 reads R data files (`.rds`, `.rda`, `.RData`) with parqit's own
out-of-core reader of R's serialization, without R: factors, dates, times,
`integer64`, haven's labels and missing-value codes, comments and attributes
become Stata labels, formats, extended missing values and characteristics (see
[R data files](#r-data-files-rds-rda-rdata)). It also reads and writes text in
any language and encoding: `encoding()` takes the code pages of every script —
Cyrillic, Greek, Turkish, Hebrew, Arabic, Baltic, Vietnamese, Thai, Chinese,
Japanese, Korean — for the dataset in memory, `.dta`/Excel, SPSS, R and
delimited-text files (UTF-16 included), `parqit set encoding` sets a session
default, and a note says what was decoded (see [Text encodings](#text-encodings)).
Some behaviours change (see the [changelog](CHANGELOG.md)): a delimited-text
lookup is no longer read with Stata's guess of its encoding, `encoding()` now
applies to delimited text instead of being ignored, delimited text that looks
like UTF-32 without a byte-order mark is refused unless `encoding()` names its
encoding, and the disk-side read of `mergein`/`appendin` is shown. The metadata
format is unchanged.

Version 0.2.6 adds `parqit spssencode`, which turns a string variable read from
an SPSS file into a labelled numeric variable with the SPSS labels kept in its
characteristics (the SPSS codes when they are integers, otherwise 1, 2, … in
code order; user-missing codes as `.a`–`.z`). `parqit describe <file>` now
shows each variable's value and variable labels, marks notes and SPSS labels,
and lists them with its `labels` and `notes` options, all from the footer. In
the dialogs, **Browse** offers every supported file type (it offered only
Parquet), and the write dialog converts SPSS files. See
[SPSS files](#spss-files-sav-zsav). Existing commands and the metadata format
are unchanged.

Version 0.2.5 reads SPSS system files (`.sav`, and ZLIB-compressed `.zsav`)
with parqit's own out-of-core reader, and writes the corresponding Parquet file
with the whole SPSS dictionary — variable and value labels, user-missing values
(as `.a`–`.z`), formats, documents and the SPSS-only properties:
`parqit save survey.parquet using survey.sav`. A `.sav` is also accepted by
`parqit use`, the two-table verbs and `mergein`/`appendin`. See
[SPSS files](#spss-files-sav-zsav). Existing commands and the metadata format
are unchanged.

Version 0.2.4 adds view copies (`parqit use [varlist] using view:<name>, name(<new>)`),
which let you try verbs on a copy of a plan while the source view stays as it
was, and sampling designs after `sample2` for `parqit sample`: an `if` frame,
strata, whole clusters and a 0/1 indicator (see [Sampling designs](#sampling-designs)).
It also refuses `name(_all)`, always restores the previous view after a
`parqit view <name>:` prefix, and keeps long names readable in `parqit views`.
The metadata format is unchanged.

Version 0.2.3 checks wide-integer precision against Parquet values even when
footer extrema are incorrect, and uses DuckDB's native vector executor for
temporal conversion. The Stata command grammar and metadata behavior are unchanged.

Version 0.2.2 fixes ambiguous helper/output names and extreme integer and
nanosecond reads, strengthens failure detection in the test runner, and reduces
the cost of temporal conversion, wide-integer checks and missing-key diagnostics. The command grammar
and exact statistical algorithms are unchanged. See the [changelog](CHANGELOG.md)
for the documented timing of errors in lazy joins.

Version 0.2.1 introduced protective integer reads with exact-text alternatives,
opt-in extended-missing preservation, binary and CSV controls, source-file
provenance, streamed parallel collection, and aligned menus/help. It also
changes two defaults: wide integers are refused unless their conversion is
chosen explicitly, and Parquet saves use zstd. See the [changelog](CHANGELOG.md)
for the compatibility notes and audit-derived corrections.
It also preserves the original cause of a worker error during streaming,
while retaining genuine user cancellation. The v0.2.0 tag was an unpublished
candidate; v0.2.1 includes its features and the streaming-error correction.

Parallel execution does not require OpenMP: DuckDB schedules queries through
its own threads (`parqit set threads`), and C++ workers fill Stata's memory
(`parqit set fill_threads`). No companion Windows DLL is shipped. The
[build and packaging checks](BUILDING.md) validate the exact distributed
plugin; worker-observation and independent-oracle tests check the parallel paths.
Release plugins are built and checked by GitHub Actions for all four targets;
local development builds are not uploaded as release binaries.

> **About.** The conceptual design of `parqit` is by **Miguel Portela** — taking
> [`pq`](https://github.com/jrothbaum/stata_parquet_io) as the starting point and
> re-basing the manipulation layer on an embedded **DuckDB** engine through a
> **C++** plugin; the implementation was programmed by two AI coding agents,
> OpenAI's **Codex** and Anthropic's **Claude Code**, under his direction and
> review. `parqit` is an **ongoing project**, provided **"as is", without warranty
> of any kind** (see [LICENSE](LICENSE)); **feedback, issues and pull requests are
> very welcome**.

## How parqit thinks — the lazy view

If your Stata reflex is *load → work → save*, parqit asks for exactly one new
idea, and everything else follows from it: **mutation verbs build a plan
instead of materialising their result when you type them.**

`parqit use using` opens a **view**: your "current dataset", except that it
lives on disk and may be far larger than memory. Opening probes source schema
and metadata; a delimited source may be sampled for type inference, and Stata
or Excel inputs may first be copied to a package-owned Parquet bridge. No
result observations are loaded into Stata, and whatever dataset you already
have in memory is not touched.

Every mutation verb appends a step to the view's **pipeline** — a plan of work,
like a do-file you are still writing. Verbs validate their stated contracts;
ordinary single-table operations also bind-check the candidate SQL.
Contract-sensitive verbs may run validation queries (for example, key or
cell uniqueness and pivot-column discovery), but these checks do not
materialise the result in Stata. The full result plan runs when you ask for it
with a **materialiser**:

- `parqit collect` compiles the whole pipeline into a single DuckDB query, runs
  it, and loads **only the result** into Stata — atomically, so your in-memory
  data is replaced only after the new data is complete and valid;
- `parqit save` runs the same query and writes Parquet directly, so a dataset
  that never fits in memory can still be filtered, joined, aggregated and
  reshaped end to end.

The main advantage is that the engine sees the whole plan: column projection
can avoid unused data, eligible filters can prune Parquet row groups, and
vectorised, parallel execution can spill supported intermediate states to a
temporary directory. Stata needs room for the result you choose to collect,
instead of the source archive and every intermediate table. A direct
`parqit save` can finish the pipeline when even that result is too large for
Stata. Typing `parqit keep if year >= 2019` on a huge panel
validates the candidate plan without scanning the full panel — and at result
execution predicate pushdown can skip row groups whose statistics prove that
they cannot match.

Two habits complete the picture. `parqit show` prints the SQL your pipeline
compiled to (`parqit explain`, the engine's plan), so the invisible work is one
command away from visible. And because a view is a plan, not data, experiments
are cheap: a verb that errors leaves the view exactly as it was,
`parqit use using view:panel, name(try)` copies the plan so you can try verbs
on the copy while `panel` stays as it was, and `parqit close` + `parqit use`
starts a fresh plan with no big read to redo.

## Why parqit

Reading and writing Parquet in Stata is already well served — by
[`pq`](https://github.com/jrothbaum/stata_parquet_io), by the legacy
`stata-parquet`, and by StataCorp's own `import parquet`. `parqit` is **not another
reader**. Its identity is the layer above I/O:

- **Manipulation, not just transfer.** A full set of single-table and two-table
  verbs that compile to one DuckDB query and run before the full result is
  collected.
- **Explore before you load.** `describe`, `head`, `summarize`, `tabulate`,
  `codebook`, `misstable`, `distinct`, `duplicates report`, `count if`,
  `histogram`, … are computed by the engine as push-down queries over the lazy
  view; only bounded summary output (or a few preview rows) reaches Stata and
  the current dataset stays unchanged. `describe` reads only Parquet footer
  metadata (types, variable and value labels, notes); the other commands may
  scan the relevant data engine-side.
- **Out-of-core by default.** Filter, join, aggregate and reshape files larger
  than memory, using DuckDB's spillable operators and sufficient scratch space.
  Some operator states still need memory. The current Stata dataset is
  replaced only by an explicitly collected pipeline result, never by its source.
- **Two first-class data paths.** Small result → Stata's in-memory dataset
  (`parqit collect`); large transformation → **Parquet → Parquet without loading the
  result into Stata** (`parqit save`). The second is where the out-of-core story
  actually pays off.
- **Sample before you load.** `parqit sample` draws on disk, in the manner of
  `sample2`: a percentage or a count of rows, or of whole clusters (workers
  with all their spells, households with all their members), within strata and
  inside an `if` frame. A cluster's rank depends only on the seed and the
  cluster's value, so the same seed draws the same clusters whatever the row
  order, file layout or thread count. Only the sample reaches Stata.
- **Lossless metadata round-trips.** Variable labels, value labels, notes, display
  formats and characteristics survive a `parqit save` / `parqit use` cycle (stored in
  standard Parquet key–value metadata), while remaining plain Parquet for pandas,
  polars, R, Spark and friends. Extended-missing categories and fractional
  date counts require the documented conversions, with loss notes; see
  Limitations and `help parqit_technical`.
- **Many sources, any script.** Parquet, delimited text, Stata, Excel, SPSS
  and R files open as the same kind of view (SPSS and R without their
  software), and text in any language and code page arrives as UTF-8, with a
  note saying what was decoded.
- **Learn the SQL if you want to.** `parqit show` prints the generated query
  (like dbplyr's `show_query()`); `parqit explain` shows the plan; `parqit sql "…"`
  drops you to raw DuckDB.

## Installation

**Requirements:** Stata 16 or newer (MP recommended for large data), on a
supported 64-bit platform. The release plugin embeds DuckDB and its compiler
runtime; no companion DLL, OpenMP runtime or separate runtime installer is
required. Normal operating-system libraries are still needed. Windows embeds
Microsoft runtime code under its own terms; parqit's MIT licence does not
relicense that code. The package includes the compiler-runtime notices.

> The compiled plugin (`parqit.plugin`, ~40 MB) is **not** stored in the git tree —
> cloning alone does not give you a working command. Pick one of the two routes
> below.

### Option 1 — `net install` from GitHub (no compiler needed)

`parqit` ships as a standard Stata package. Its compiled plugin (`parqit.plugin`,
~40 MB, one binary per supported OS/architecture) is **not** in the git tree —
it is distributed through [GitHub Releases](https://github.com/reisportela/parqit/releases).
You can install it **straight from GitHub over the internet in one line**, or
download a zip and install offline. Both routes cover **Linux x86_64**,
**Windows x86_64**,
**Intel macOS (x86_64)** and **Apple-Silicon macOS (arm64)**, including GUI and
console Stata on both Mac architectures.

#### Install directly from GitHub (recommended)

In Stata, point `net install` at the release's download URL. Stata reads
`parqit.pkg`, picks the binary for your machine, and installs it as `parqit.plugin`
onto your `PLUS` adopath (run `sysdir` to see where):

```stata
. net install parqit, from("https://github.com/reisportela/parqit/releases/latest/download") replace
```

After upgrading a plugin already loaded in Stata, **restart Stata** before
running the checks below. `discard` alone does not guarantee a plugin reload.

```stata
. parqit version        // confirms the plugin loaded
. parqit selftest       // end-to-end self-check, prints "ok"
```

- `replace` upgrades an existing install in place; `ado uninstall parqit` removes it.
- The URL above always follows the newest public GitHub release.
- To pin a specific version instead, replace `latest/download` with
  `download/vX.Y.Z` (for example, `download/v0.3.1`).
- If your Stata cannot reach GitHub (a corporate proxy or an air-gapped HPC
  cluster), use the offline zip route below — it is byte-for-byte the same package.

#### Install from a downloaded zip (offline)

Download the zip for your platform from the
[latest release](https://github.com/reisportela/parqit/releases), extract it into a
**dedicated folder**, and run `net install` from that folder (the one holding
`parqit.pkg`). `parqit_all_platforms.zip` bundles all four OS/architecture
targets.

**Linux (x86_64)** — the release workflow builds on AlmaLinux 8 for older-glibc
compatibility on EL8-family systems and HPC clusters. Compatibility with an
older host must be checked against the exact release binary:

```bash
cd ~/Downloads
mkdir -p parqit_pkg && unzip parqit_linux_x86_64.zip -d parqit_pkg
```
```stata
. net install parqit, from("/home/<you>/Downloads/parqit_pkg") replace
. parqit version
. parqit selftest
```

**macOS — Intel (x86_64):**

```bash
cd ~/Downloads
mkdir -p parqit_pkg && unzip parqit_macos_x86_64.zip -d parqit_pkg
```
```stata
. net install parqit, from("/Users/<you>/Downloads/parqit_pkg") replace
. parqit version
. parqit selftest
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

(If macOS Gatekeeper quarantines either Mac binary, clear it once with
`xattr -dr com.apple.quarantine ~/Downloads/parqit_pkg`.)

**Windows (x86_64)** — right-click `parqit_windows_x86_64.zip` → *Extract All…*
(this creates a `parqit_windows_x86_64\` folder), then in Stata use forward slashes
in the path:

```stata
. net install parqit, from("C:/Users/<you>/Downloads/parqit_windows_x86_64") replace
. parqit version
. parqit selftest
```

> A binary built on a newer Linux may require newer glibc symbols and fail on
> an older HPC cluster. Use the GitHub-built AlmaLinux-8 release binary on
> EL8-family systems; compatibility depends on the exact binary, not just the
> machine's architecture.

### Option 2 — clone and build from source

Needs `git`, CMake ≥ 3.21 and a C++17 compiler. The supported release toolchains
are GCC on Linux (≥10), GCC 14 on macOS and MSVC 2019+ on Windows; see
[BUILDING.md](BUILDING.md) for prerequisites and per-platform presets.
The first build downloads and compiles DuckDB 1.5.3 from source
(SHA256-pinned, with hash-checked local corrections), so allow tens of minutes
and a few GB of disk, depending on the toolchain and hardware.

On Linux, the source archive includes a helper that builds and runs the C++
checks with two jobs by default (`bash build.sh 4` selects four):

```bash
git clone https://github.com/reisportela/parqit.git
cd parqit
bash build.sh
```

Windows and both macOS architectures use the presets in
[BUILDING.md](BUILDING.md). For a local developer build on Linux:
`cmake --preset dev` followed by `cmake --build build/dev -j4`.

Every build refreshes the repo-local install tree **`ado/plus/p/`**
(ado, help, pkg and freshly stripped plugin — nothing is written to your
`~/ado`). Then point Stata at it:

```stata
. adopath ++ "/path/to/parqit/ado/plus/p"
. help parqit             // the user manual; help parqit_technical is the technical reference
```

To make that permanent, add the `adopath` line to your `profile.do`
(see `help profilew`). To update later: `git pull`, rebuild, restart Stata.

**From SSC** (planned): `ssc install parqit`.

## Quick start

```stata
* Open a lazy view over one or many Parquet files (schema probed, no rows loaded)
parqit use using /data/qp_*.parquet

* Look around first — bounded outputs only; the current dataset stays unchanged
parqit head
parqit summarize wage

* Build a pipeline with ordinary Stata verbs — still lazy
parqit keep if year >= 2010 & inrange(age, 25, 64)
parqit keep   id firm year wage
parqit gen     double lwage = log(wage)
parqit collapse (mean) lwage (count) n = wage, by(firm year)

* Inspect the DuckDB SQL parqit generated for you (optional, great for learning)
parqit show

* Materialise the result into Stata's memory…
parqit collect, clear

* …or write the result straight to Parquet without loading it into Stata
parqit save firm_year_panel.parquet, replace
```

Reading selects the input adapter from the extension. Writing always produces
Parquet; `data` explicitly selects the dataset in Stata's memory:

```stata
parqit use  mydata.parquet, clear          // read whole file into memory
parqit save mydata.parquet, replace data   // write the in-memory dataset
parqit describe mydata.parquet             // schema, types, labels, rows, row groups
parqit use  survey.sav, clear              // or .csv .dta .xlsx .rds .RData ...
parqit use  prices.csv, clear encoding(windows-1251)   // text in another code page
```

## First contact with a large file

parqit is built for the first hour with unfamiliar — possibly enormous — data.
The whole exploration family (`describe`, `head`, `list`, `summarize`,
`tabulate`, `codebook`, `misstable`, `levelsof`, `distinct`,
`duplicates report`, `tabstat`, `correlate`, `histogram`, `count if`) runs as
push-down queries over the view: the engine computes, only the summary numbers
(or a few preview rows) reach Stata, and the current dataset stays unchanged
throughout. The queries may still scan the relevant source data engine-side;
you can profile a file that would never fit in RAM before deciding what — if
anything — to load:

```stata
parqit describe /data/deals_*.parquet   // rows, columns, types, labels: footer only, no data scan
parqit use using /data/deals_*.parquet  // lazy view after a schema/metadata probe
parqit head 10                          // first rows, nothing else materialised
parqit codebook                         // per variable: type, obs, missing, distinct, min/max
parqit misstable patterns amount client_id region // selected variables (at most 14)
parqit summarize amount, detail         // moments and percentiles using Stata's definitions
parqit tabulate region sector, row      // two-way table with row percentages
parqit count if missing(client_id)      // filtered count; the view's pipeline is untouched
parqit histogram amount, bins(30)       // bins computed on disk, drawn with twoway bar
parqit close
```

Each data-query call re-executes the lazy pipeline: columnar scans can read
only the variables involved, eligible filters may prune row groups, and the
engine can parallelise across cores. Exact statistics may still require a
full scan or sort. This is the intended workflow — **explore
first, load last** — and it is the sense in which parqit is not another
Parquet reader: the interesting work happens before the current dataset is
replaced by a collected result.

## The verb grammar

Mutation verbs append to the **current view** (an implicit lazy table, just
like Stata's implicit current dataset). Opening probes schema and metadata,
ordinary single-table operations bind-check the candidate SQL, and
contract-sensitive verbs may run validation queries. Only a *materialiser*
produces the full result table.

Subcommands and options take the abbreviations native Stata accepts for the
same commands (CMD-ABBREV-1): `parqit su price, d` is `parqit summarize price,
detail`, and likewise `u`, `sa`, `d`, `g`, `ren`, `so`, `cou`, `l`, `mer`, `ap`,
`ta`, `cor`, `se` and `hist` (a synonym, as in Stata: `histo` is refused).
`duplicates r|l` and `misstable sum|pat` follow native's subcommand rules, and
`tabulate …, r co`, `pwcorr …, o` and `tabstat …, stats()` take Stata's option
forms. The commands Stata spells out (`keep`, `drop`, `replace`, `egen`,
`collapse`, `reshape`, `tabstat`, …) and parqit's own verbs are typed in full.

### The view at a glance

A parqit session has four moves: **open** a view, **shape** it with lazy verbs,
**look** at it engine-side, and **land** the result. The last move materialises
the full result. Opening can create an input bridge and exploration can use
scratch data; lazy verbs build the plan while the current dataset stays in place.

```
┌─ 1  OPEN ─ start a view
│
│    parqit use <file>        a Parquet file, glob or Hive directory,
│                             or .csv .tsv .txt .tab .dta .xls .xlsx
│                             or an SPSS .sav .zsav
│                             or an R .rds .rda .RData
│    parqit use view:<name>   a copy of an open view's plan (with name())
│    parqit open _data        the dataset already in Stata's memory
│    parqit sql "SELECT ..."  any DuckDB query
│
├─ 2  SHAPE ─ lazy verbs; each one extends the plan
│
│    rows         keep  drop  sample (strata, whole clusters)  duplicates drop
│    columns      gen  egen  replace  rename  order
│    order        sort  gsort
│    aggregate    collapse  contract  pivot
│    restructure  reshape long   reshape wide
│    two tables   merge  append  joinby
│
├─ 3  LOOK ─ runs the plan, shows a summary
│
│    shape        describe  glimpse  ds  lookfor  codebook
│    rows         count  head  list  levelsof  distinct
│    statistics   summarize  tabstat  tabulate  histogram
│                 correlate  pwcorr
│    quality      misstable  duplicates report  duplicates list
│
└─ 4  LAND ─ produce the full result

     parqit collect           stream the result into Stata's dataset, atomically
     parqit save <file>       write Parquet; the dataset in memory is untouched
```

Each band is documented below: [Open / source](#open--source),
[Single-table verbs](#single-table-verbs-lazy) and
[Two-table verbs](#two-table-verbs-lazy),
[Explore the view](#explore-the-view-engine-side-current-dataset-unchanged),
[Materialisers](#materialisers-and-engine-side-result-commands).

The order is a habit, not a rule: look whenever you like, shape again after
looking, and collect or save as often as you need — the view stays open and
re-executes each time. Alongside the four moves, at any point in the session:

| | |
|---|---|
| the plan | `parqit show` `parqit explain` |
| views | `parqit views` `parqit view` `parqit use … using view:` `parqit close` |
| engine | `parqit set` `parqit path` |
| install | `parqit version` `parqit selftest` `parqit menu` |

Views are named (`default` unless `name()` says otherwise) and several can be
open at once, like frames. Verbs act on the *current* view; `parqit view <name>`
switches, and `parqit view <name>: <command>` runs one command against another
view and switches back (also when the command opens or copies a view). A verb
always changes the view it runs on; to try verbs without changing a view, copy
its plan first with `parqit use [varlist] using view:<name>, name(<new>)`. The
copy is a plan, not data: no rows are read, later verbs on either view do not
reach the other, closing either leaves the other usable, and both read the same
files when they execute.

Two commands are deliberately *not* view verbs: `parqit mergein` and
`parqit appendin` join the dataset *already in Stata's memory* with a disk file
through a **native** `merge`/`append`, reading only the columns of the file they
need. Use them when the disk side is a small lookup; use `parqit use` +
`parqit merge` when both sides are big.

### Open / source

| Command | Compiles to | Notes |
|---|---|---|
| `parqit use [varlist] using <files>` | `read_parquet(...)` / `read_csv_auto(...)` | Parquet file/glob/Hive dir, or delimited text (`.csv`/`.tsv`/`.txt`/`.tab`), or a Stata `.dta` / Excel `.xls`/`.xlsx` (imported to a Parquet bridge), or an SPSS `.sav`/`.zsav` (converted out of core to a Parquet bridge, see [SPSS files](#spss-files-sav-zsav)), or an R `.rds`/`.rda`/`.RData` (likewise, see [R data files](#r-data-files-rds-rda-rdata)). With `clear`, reads into memory. `name()` opens under a view name; `relaxed` unions a mixed-schema glob by column name; `encoding()` names the encoding of text that is not UTF-8, in any language — delimited text, a `.dta`/Excel bridge, the code page that replaces the one an SPSS file declares, the encoding R recorded for unmarked strings (see [Text encodings](#text-encodings)); `encoding(name, all)` decodes also text that happens to be valid UTF-8; `object(name)` names the data frame of an R file that holds several; `int64(refuse|round|string)` says what to do with integers outside the protected +/-2^53 range (default: refuse the read) and `binary(text|hex)` loads a `BLOB` column instead of dropping it; `filename(newvar)` adds a string variable holding the path each row was read from; `csv(...)` forces a delimited-text source's dialect and types instead of inferring them. |
| `parqit use [varlist] using view:<name>, name(<new>)` | a copy of that view's plan | Copy the plan of an open view into a new view, optionally keeping only `varlist` (without a varlist, `using` may be omitted: `parqit use view:<name>, name(<new>)`); `name()` is required and must differ from the source. The copy reads no rows and leaves the source view unchanged: later verbs on either view do not reach the other, the copy shares the source's temporary bridges, and an unseeded `sample` step keeps its draw. |
| `parqit open _data [, name() encoding()]` | temporary Parquet snapshot + scan | Snapshot the current in-memory dataset to a package-owned bridge and open a view over it; the current dataset stays in place. |

**Input formats.** Parquet and delimited text are scanned *out of core* (the
file can exceed memory); delimited text that is not UTF-8 is first decoded,
streaming, into a temporary UTF-8 copy (see [Text encodings](#text-encodings)).
Stata `.dta` and Excel `.xls`/`.xlsx` are not
engine-scannable, so parqit imports them into a throwaway frame (your data is
untouched) and snapshots them to a small Parquet *bridge* — ideal for a small
lookup, but for a large `.dta` master prefer `use` + `parqit open _data`. SPSS
`.sav`/`.zsav` files and R `.rds`/`.rda`/`.RData` files are converted to such a
bridge by parqit's own readers, out of core and without a frame (see
[SPSS files](#spss-files-sav-zsav) and [R data files](#r-data-files-rds-rda-rdata)). The same
extension rule applies to a `using` side of `merge`/`joinby`/`append`, so a
lazy Parquet master can join a `.dta` lookup and only the result is collected.
For these two-table using sides, Parquet stays directly on disk; delimited text,
Stata, Excel, SPSS and R are first imported to the package-owned bridge. Each bridge is
atomically reserved by the plugin (including when two Stata
processes share one temporary directory) and is package-owned: an operation
failure removes it, while a successful lazy operation keeps it until the last
view whose plan references it is closed or replaced. `parqit close _all` is the
final package-owned cleanup sweep.

**Where did this row come from?** `filename(newvar)` adds one string variable
holding the path each observation was read from — the path *as matched*, so an
absolute pattern gives absolute paths. It is an ordinary variable: lazy verbs
filter on it, `collect` and `save` carry it, and a saved file holds it as plain
Parquet text. It is never confused with a Hive partition key, and a name the
source already loads is refused rather than quietly renamed. A `.dta`/Excel/SPSS/R
source refuses it, because the path would be the temporary bridge.

**Delimited text you already know.** Type inference is a guess, and a wrong
guess changes values with no error: `1e5` becomes 100000, a decimal with more
digits than a double holds is rounded, `TRUE` becomes 1. `csv(...)` replaces the
guess — `delim("c")`, `quote("c")`, `escape("c")`, `header(on|off)`,
`dateformat("...")`, `timestampformat("...")`, `sample(#)` (`-1` = the whole
file), `allvarchar`, `types(name:TYPE ...)` and `nullstr("...")`. Any other key
is refused by name. The forced dialect drives the header-name recovery as well
as the scan, so the names come back from the same split as the data.

Column subsets in eager or lazy reads accept Stata wildcards, for example
`parqit use id wage* using panel.parquet, clear`. `*` matches any run and `?`
matches one Unicode character; the same expansion is used by lazy projections,
`mergein, keepusing()` and `appendin, keep()`.

### SPSS files (`.sav`, `.zsav`)

parqit reads SPSS system files — `.sav`, uncompressed or bytecode-compressed,
and ZLIB-compressed `.zsav`, in either byte order — with its own reader
(written from the published format; no new dependency), out of core: the file
is decoded straight into Parquet and never passes through a Stata frame. One
command writes the corresponding Parquet file with the whole SPSS dictionary:

```stata
parqit save survey.parquet using survey.sav, replace   // compression(), encoding() also accepted
parqit use using survey.parquet, clear                 // labels, .a-.z codes and SPSS properties restored
```

The whole file is decoded and checked first — a truncated or corrupt file is
refused, naming the case or record, before anything is written — and the rows
then stream through the same staged, verified writer as every `parqit save`; the
rows written must equal the cases read, and the source must not change meanwhile.
The dataset in memory and the open views stay as they were. A `.sav`/`.zsav` is
also accepted wherever parqit reads a file (`parqit use`, lazy or with `clear`;
the `using` side of `merge`/`joinby`/`append`; `mergein`/`appendin`): it is
converted the same way into a package-owned temporary Parquet bridge.

| SPSS | Parquet file, and Stata after `parqit use` |
|---|---|
| variable names | the SPSS names are the column names; a name Stata cannot hold (over 32 characters, or with `.` `@` `#` `$`) loads under a sanitised name, the SPSS name in `char var[src_name]` |
| numbers | `DOUBLE`; system-missing is missing |
| user-missing values | extended missing values `.a`–`.z` (below) |
| strings | text, the blank padding removed; beyond 2045 bytes a `strL` |
| dates (`DATE`, `ADATE`, `EDATE`, `JDATE`, `SDATE`, `QYR`, `MOYR`, `WKYR`) | `DATE`, `%td` with the SPSS look (`%tdDD-Mon-CCYY`, `%tdNN/DD/CCYY`, `%tdMon_CCYY`, `%tdq_!Q_CCYY`, …); a date that holds a time of day becomes a `TIMESTAMP` (`%tc`, same look), with a note |
| `DATETIME`, `YMDHMS` | `TIMESTAMP`, `%tcDD-Mon-CCYY_HH:MM:SS` / `%tcCCYY-NN-DD_HH:MM:SS` |
| `TIME`, `MTIME` | `TIME` (`%tcHH:MM:SS`) when every value lies within a day; `DTIME` and longer durations as seconds |
| display formats | `F`→`%w.df`, `COMMA`/`DOLLAR`→`%w.dfc`, `DOT`→`%w,dfc`, `E`→`%w.de`, `N`→`%0w.0f`; the SPSS format itself in `char var[spss_format]` |
| variable labels | variable labels; Stata keeps 80 characters, a longer label is kept whole in `char var[spss_label]` |
| value labels | a value label named after the variable with every integer key; what Stata cannot hold (labels of strings, of non-integer values, of dates) whole in `char var[spss_value_labels]` (JSON pairs), which `parqit spssencode` turns into a labelled numeric variable (see *String codes* below) |
| file label, documents | dataset label; notes on `_dta` |
| measurement level, display width, alignment, role, custom attributes | `char var[spss_measure]`, `[spss_display_width]`, `[spss_alignment]`, `[spss_role]`, `[spss_attributes]` |
| weight, encoding, product and creation stamp, file attributes, multiple-response and variable sets | `char _dta[spss_weight]`, `[spss_encoding]`, `[spss_product]`, `[spss_creation]`, `[spss_attributes]`, `[spss_mrsets]`, `[spss_varsets]` |

**User-missing values** become extended missing values, so they are missing in
every computation and keep their identity. The codes are assigned per variable
in a fixed order: the discrete missing values (ascending), then the labelled
values inside the missing range (ascending) — both from the SPSS dictionary, so
files of a survey series with the same definitions get the same codes — then
any other value observed inside the range (ascending). `char var[spss_missing]`
holds the SPSS definition (`LO THRU -1, 99`) and `char var[spss_missing_map]`
the codes (`.a=99 .b=-9 .c=-8`); the SPSS labels of those values are attached to
their codes too. In Parquet the cell is null — every reader sees a missing value
— and the code sits in a companion column (the `parqit save, xmissing` layout),
which `parqit use`, `mergein` and `appendin` restore. More than 26 distinct
user-missing values in one variable share `.z` from the 26th on, with a note.
A lazy view reads the codes as plain `.` and says so; to keep them in Parquet,
convert with `parqit save … using`, not by saving a lazy view. String
user-missing values stay text (Stata has no missing strings). Compare the
`spss_missing_map` characteristics before appending files converted one by one:
a value observed in only some files can get a different code in each.

**String codes.** Stata cannot label a string, so a string variable keeps its
SPSS value labels in `char var[spss_value_labels]`. With the data in memory,
`parqit spssencode var, generate(newvar)` creates the labelled numeric version:

```stata
parqit use using firms.sav, clear         // or the Parquet file converted from it
parqit spssencode region, generate(region_num)
tab region_num                            // labelled with the SPSS labels
```

When every SPSS code is a distinct integer, the values are the codes themselves
(as `destring` would give). Otherwise, as with letter codes, the codes are
numbered 1, 2, … in code order (as `encode` does); `sequential` asks for that
numbering even for integer codes. The dictionary, not the data, chooses the
numbering, so files that share a dictionary share codes. SPSS user-missing codes
become `.a`, `.b`, … with their labels (`char newvar[spss_missing_map]`). The
value label is named after `newvar` unless `label()` names another. Values that
do not fit the dictionary's numbering, an existing label or a malformed
characteristic are refused before anything is created.

**Encoding.** Text is decoded from the encoding the file declares (record 7,
subtype 20, or its code page number): UTF-8 or any code page parqit reads —
Windows 874 and 1250–1258, ISO-8859, KOI8, DOS and Mac, Shift_JIS, GBK, UHC,
Big5, EUC-JP, GB18030 (see [Text encodings](#text-encodings)); EBCDIC, UTF-16
and code pages parqit does not read are refused with a message. A file without
a declaration is decoded in the session code page, with a note, and
`encoding()` replaces a missing or wrong declaration. In a UTF-8 file, text that
is not valid UTF-8 is transcoded from the session code page (or the
`encoding()` one), item by item, with a note.

**Compared with Stata's `import spss`** (which also reads these files):
user-missing values stay distinct (`.a`–`.z`, not `.`), string user-missing
values are kept, dates are `%td` days rather than `%tc` milliseconds, a time of
day counts from 01jan1960, and value labels Stata cannot hold are kept in
characteristics instead of being dropped. Portable (`.por`) and encrypted SPSS
files are not read; `parqit describe` reads Parquet footers only (describe the
converted file, or a view opened over the SPSS file). On the converted file it
marks the string variables that carry SPSS labels, and its `labels` option lists
them. The tests compare the
conversion with three other readers: pyreadstat, Stata's `import spss` and R's
`foreign` (see `tests/verify_suite/v127`–`v130`).

### R data files (`.rds`, `.rda`, `.RData`)

parqit reads R data files — `.rds` (written by `saveRDS()`) and `.rda`/`.RData`
(written by `save()`) — with its own reader of R's serialization format, out of
core and without R: the data frame is decoded straight into Parquet and never
passes through a Stata frame. One command writes the corresponding Parquet file:

```stata
parqit save households.parquet using households.rds, replace          // compression(), encoding() also accepted
parqit save people.parquet using workspace.RData, replace object(people)
parqit use using households.parquet, clear                              // labels, .a-.z codes, R properties restored
```

As for an SPSS file, the whole file is parsed and every column read first — a
truncated or corrupt file is refused, naming the byte offset, before anything is
written — and the rows then stream through the staged, verified writer. An R
file is also accepted wherever parqit reads a file (`parqit use`, the `using`
side of `merge`/`joinby`/`append`, `mergein`/`appendin`), through a temporary
Parquet bridge.

**Which data frame.** An `.rds` holds one object: a data frame, or a list whose
data frames can be read by name. An `.RData` holds named objects; functions
(byte-compiled or not), environments and other objects are passed over, never
run. With exactly one data frame, that one is read (a note names it when there
are other objects); with several, `object(name)` names one and without it the
command stops, listing them. `object()` is an option of `parqit use` and
`parqit save … using`. Unnamed list elements are shown as `[[1]]`, `[[2]]`, …;
if a real name already uses that spelling, a suffix makes the generated name
unique. A real name always selects the element bearing that name.

**Formats.** R's binary (XDR) serialization, versions 2 and 3, uncompressed or
compressed with gzip (R's default) or zstd. bzip2, xz and R's ASCII format are
refused with a message saying how to re-save the file (the data sets of R
packages are often in xz or bzip2: CRAN keeps whichever compresses best).

| R | Parquet file, and Stata after `parqit use` |
|---|---|
| column names | the R names; an empty or `NA` name becomes `V#` (its position), a repeated name gets `_1`, `_2`, … (the R name in `char var[r_name]`); names that differ only by case are kept exactly |
| logical | `BOOLEAN` (a `byte` 0/1) |
| integer | `INTEGER`; `NA` is missing |
| double | `DOUBLE`; `NA` is missing; `NaN` and `Inf` stay in the Parquet file and load as `.` with a note |
| integer64 (bit64) | `BIGINT`, exact; values beyond 2^53 load only with `int64()` |
| character | UTF-8 text; `NA_character_` is a null (Stata loads it as `""`); beyond 2045 bytes a `strL` |
| factor, ordered | the integer codes with a value label of the levels (text when a level set does not fit one Stata value label) |
| `Date`, `IDate` | `DATE` (`%td`); a date holding a fraction of a day becomes a `TIMESTAMP` (`%tc`), with a note |
| `POSIXct` | `TIMESTAMP` (`%tc`), the UTC clock time of the instant R stores; the display time zone in `char var[r_tzone]` |
| `hms`, `ITime` | `TIME` (`%tcHH:MM:SS`) when every value remains within a day after microsecond rounding, otherwise seconds |
| `difftime` | its number, the unit in `char var[r_units]` |
| haven `label` / `labels` / `format.stata` | variable label (a label over 80 characters whole in `char var[r_label]`), value label with every integer key (the full set, text keys included, in `char var[r_value_labels]`, which `parqit spssencode` reads), display format |
| haven tagged NAs / `na_values`, `na_range` | extended missing values: a tagged NA keeps its letter, user-missing values take the other letters as for an SPSS file (`char var[r_missing]`, `[r_missing_map]`) |
| `comment()`, the data frame's `label` | notes; dataset label |
| character row names | a first variable, `rowname` |
| other attributes, within the limits below | `char var[r_class]`, `[r_attributes]` (JSON); `char _dta[r_class]`, `[r_attributes]`, `[r_object]`, `[r_written_by]`, `[r_encoding]` |

Temporal formats follow the units of the converted values. An incompatible
`format.stata` is retained in `r_attributes`, with a note; a fractional `Date`
therefore uses `%tc` even when its old format was `%td`. Finite dates that
coincide with the engine's infinity sentinels are stored as numbers of days
since 1970, with a note. A time that rounds to 24:00 is stored in seconds.

Other attributes are summarized when they exceed 1,000 elements or 20 levels
of JSON nesting, or the parser's size/depth limits. A JSON characteristic over
60,000 bytes is reduced to the attribute names, with a note; these summaries
do not retain the full attribute values.

Columns Stata has no type for (lists, nested data frames, matrices, complex,
raw, `POSIXlt`, S4) are left out, each named in a note. A character column R
still holds as the numbers of an `as.character()` it has not carried out — R
turns them into text only when used, with the reading computer's formatting —
is stored as those numbers, with a note and `char var[r_deferred]`. Strings are
decoded by the encoding R marked on each (UTF-8, latin1, bytes), unmarked ones by
the encoding the file records — any code page parqit reads (`encoding()`
replaces it). The tests compare
every value with R's own reading of the same objects, and with R itself when it
is installed (see `tests/verify_suite/v133`–`v135`).

### Text encodings

Names, values, labels, notes and characteristics in any script travel as
UTF-8, unchanged. Text in another encoding — Russian, Greek, Arabic, Hebrew,
Thai, Vietnamese, Chinese, Japanese, Korean as much as Portuguese — is decoded
to UTF-8 on the way in, never refused and never guessed in silence: a `note:`
says what was decoded, from what, and how much (ENC-3, CSV-ENC-1).

- **The encodings** `encoding()` and `parqit set encoding` accept: UTF-8;
  Windows 1250–1258 and 874; ISO-8859-1 to -16; KOI8-R/U; the DOS code pages
  (437, 737, 775, 850, 852, 855, 857, 858, 860–866, 869); the Mac code pages
  (Roman, Cyrillic, Central European, Greek, Turkish, Icelandic); Shift_JIS
  (932), EUC-JP, GBK (936), GB18030, Big5 (950), EUC-KR/UHC (949); and UTF-16
  for delimited text — by name or the usual aliases (`cp1251`, `sjis`,
  `gb2312`, `latin2`, …). They are read as Windows reads them (its
  user-defined characters become the Unicode Private Use characters Windows
  gives them); GB18030 as its 2005 edition. The tests compare the mappings
  with ICU and independent oracles (`tests/verify_suite/v136`, `v140`), with
  explicit vendor differences. CP864 maps byte `0x25` to Arabic `٪`, including
  in SPSS values and labels. Bytes a code page
  does not define become U+FFFD, counted (`r(undecodable)`).
- **Data in memory, `.dta` and Excel files.** Valid UTF-8 is kept; other text
  is decoded item by item from `encoding()` or the session code page
  (`windows-1252` unless `parqit set encoding`). With a multibyte code page —
  whose text is often valid UTF-8 by accident (GBK's 女 is UTF-8's Ů) — a
  string variable with any text that is not UTF-8 is decoded whole, and so is
  the metadata. `encoding(name, all)` decodes everything; a `.dta` of format
  117 or older (Stata 13, before Unicode) is read that way whenever
  `encoding()` is given. Stata decodes Excel text itself, so `all` is not
  applied to an Excel file (a note says so). `all` also applies to bytes below
  128 whose meaning differs from ASCII, such as CP864's percent byte.
  `copysource` refuses `encoding(name, all)`; use the normal memory writer
  when asking to decode the text.
- **Delimited text** is read as UTF-8, its byte-order mark (UTF-8, UTF-16)
  recognised, and UTF-16 without one by its NUL bytes or its line ends; UTF-32
  is refused, other NUL bytes (fixed-width padding) are read as before; an
  `encoding()` given is followed even where the bytes look otherwise, with a
  note. A file already valid UTF-8 throughout stays in place with a named
  legacy encoding unless `all` is requested, so `filename()` still reports
  its original path. Checking a named encoding may scan the whole file before
  opening the view. Files needing decoding are decoded into a
  temporary UTF-8 copy first — line by line with a single-byte code page, whole
  with a multibyte one; lines may end in LF, CRLF or CR; the copy keeps the
  file names and the Hive `key=value` directories — and `encoding(utf-8)`
  turns broken bytes into U+FFFD. Undeclared text that is not UTF-8 is refused when the file
  is scanned in place (the message names `encoding()`), and decoded from the
  session code page, with a note, when it is a lookup file of a two-table verb
  — never from Stata's own guess, which is wrong for most scripts.
- **The default** is `windows-1252` everywhere, so a do-file gives the same
  result on every computer. When undeclared text was decoded from it and the
  locale suggests another code page, a note names it (in a Russian locale,
  `encoding(windows-1251)` or `parqit set encoding windows-1251`).

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
| `parqit collapse (stat) v ... , by()` | `GROUP BY` + aggregates (mean/sum/sd/median/pXX/count/min/max/first/last/firstnm/lastnm) |
| `parqit contract <varlist> [, freq()]` | grouped counts |
| `parqit duplicates drop [varlist] [, force]` | `DISTINCT` / dedup |
| `parqit keep in <range>` / `parqit drop in <range>` | validated `LIMIT/OFFSET` and its complement (`f`/`l` and negative bounds accepted) |
| `parqit sample # [, count seed()]` | globally rounded percentage sample, or reservoir rows with `count`; seeded reproducibility requires stable inputs/settings |
| `parqit sample # [if] [, count seed() by() cluster() any all generate()]` | sampling designs after `sample2` (see [Sampling designs](#sampling-designs)): `if` frame, strata, whole clusters, and an indicator instead of dropping |
| `parqit reshape long\|wide ...` | `UNPIVOT` / `PIVOT` |
| `parqit pivot (stat) v ... , rows() cols()` | Excel-style pivot table: `GROUP BY` the rows()+cols() keys, then one column per distinct cols() value (`collapse` + `reshape wide`, applied atomically) |

### Sampling designs

`parqit sample # [if] [, count seed() by() cluster() any all generate()]` follows
Weesie's `sample2` (STB-37 dm46) and draws on disk, so only the sample has to fit
in Stata:

```stata
* a tenth of the workers, each with all their years
parqit use using /data/qp_*.parquet
parqit sample 10, cluster(worker) seed(20260925)
parqit collect, clear

* a tenth of the households of each region, with all their members
parqit use using households.parquet
parqit sample 10, cluster(hh) by(region) seed(20260925)
parqit collect, clear

* half of the households with someone over 60, flagged instead of dropped
parqit use using households.parquet
parqit sample 50 if age > 60, cluster(hh) any generate(pick)
parqit collect, clear                       // pick = 0: not drawn
```

- **Frame.** `if <exp>` chooses the rows that can be drawn; every other row is
  kept. Under the default SQL missing semantics a missing condition leaves the
  row outside; with `parqit set statamissing on`, `x > 60` holds for a missing
  `x`, as in `sample2`.
- **Strata.** `by()` draws # percent (or, with `count`, # units) in each
  stratum; a missing value is its own stratum.
- **Clusters.** `cluster()` draws whole clusters: every row of a drawn cluster
  stays, the others go. The strata must be constant within clusters. A cluster
  that `if` splits is an error, unless `any` (any selected row places it in the
  frame) or `all` (only all of them). Without `cluster()`, `any` and `all` are
  ignored with a note. Rows whose cluster is missing are outside the frame and
  kept, and a note says how many.
- **Indicator.** `generate(newvar)`, or `keep(newvar)` as in `sample2`, adds a
  0/1 byte (1 = kept) instead of dropping rows.
- **Counts.** Each stratum draws n × # / 100 rounded to the nearest whole
  number, half up, as the plain percentage form does. For whole-number
  percentages this is `sample`'s and `sample2`'s `int(n*#/100+.5)`. A
  fractional percentage can differ from them by one unit at a tie: 0.3 percent
  of 500 draws 1, they draw 2.
- **Reproducible draws.** A cluster's rank is a parqit function of the seed and
  the cluster's value (splitmix64 over its binary64 value, or its UTF-8 bytes).
  The same seed therefore draws the same clusters whatever the row order,
  file layout, thread count or DuckDB version. 1 stored as an integer or as a
  double is one cluster. Integers beyond 2^53 rank by their binary64
  approximation, with ties broken by value. The draw never matches `sample2`'s,
  which uses Stata's random numbers.
- **Rows.** Without `cluster()`, rows use the plain form's priority, so
  `generate()` alone flags exactly the rows the plain percentage form keeps.
  As in the plain form, rows are numbered in the view's sort order (input
  order when there is none), so the same seed draws the same rows only over
  the same order. With `count`, a design keeps # rows per stratum by that
  priority, while the plain count form keeps its reservoir.
- **Checks and cost.** The cluster checks (constant strata, split clusters) run
  once over the current plan when `sample` is issued, like `merge`'s key
  checks, and also validate a pending `keep in` at that point. A cluster
  design reads its input twice (one pass to rank the clusters, one to join the
  decision back) and materialises nothing. A design without clusters
  materialises its input once, like the percentage form. `in` is not
  supported, and `seed()` goes from 0 to 2,147,483,647.

### Two-table verbs (lazy)

| Command | Compiles to |
|---|---|
| `parqit merge 1:1\|m:1\|1:m <keys> using <file\|view:name> [, keep() keepusing() gen() nogenerate encoding()]` | `JOIN`, with a Stata-compatible `_merge`; the *using* side stays on disk — a file or **another open view**. A non-key variable on both sides takes the using value on using-only rows, as native. Lazy `m:m` is refused; use `joinby` or native `mergein m:m`. |
| `parqit append using <files\|view:name ...> [, generate() keep() encoding()]` | `UNION BY NAME`, aligning columns by name with safe recasts; sources may be files or views; `keep()` names the variables taken from the using sources (wildcards allowed), and `generate()` marks each row's source with native `append`'s labels |
| `parqit joinby <keys> using <file\|view:name> [, encoding()]` | many-to-many join |

**In-memory + disk, fast.** When your data is already in Stata's memory and you
want to join a disk file (a small lookup), `parqit mergein`/`parqit appendin` keep
the in-memory data put and run a *native* `merge`/`append`, reading only the
needed columns of the disk side — no DuckDB round-trip. It avoids the temporary
`parqit open _data` bridge and is therefore the preferred route for a small disk
lookup. For big-on-big, prefer the out-of-core `parqit use … ; parqit merge` path.

| Command | Effect |
|---|---|
| `parqit mergein 1:1\|m:1\|1:m\|m:m <keys> using <file> [, <merge opts> int64() encoding()]` | Native `merge` of the in-memory data with a disk lookup (read via parqit); the using side is a file, and a `view:` source is refused with the out-of-core alternative |
| `parqit appendin using <file> [, keep() generate() force int64() encoding()]` | Native `append` of a disk file onto the in-memory data (`generate()` marks the source of each observation, as native `append`); a `view:` source is refused likewise |

### Materialisers and engine-side result commands

`collect` and `save` materialise the full pipeline result. The remaining
commands below execute bounded preview, aggregate or metadata queries against
the view without replacing the current dataset.

| Command | Effect |
|---|---|
| `parqit collect [, clear int64(refuse|round|string)]` | Execute once; stream the result into Stata's memory atomically. The view stays open (collecting again re-executes). `int64()` decides what happens to a column whose integers exceed 2^53 (default: refuse); it overrides the value the view was opened with. |
| `parqit save <dest> [, replace data partition_by() partitions(replace\|append) compression() compression_level() chunk() encoding() copysource xmissing]` | Execute; write Parquet **without loading the result into Stata's current dataset**; `data` explicitly exports the in-memory dataset when a view is open; `partitions(replace)`/`partitions(append)` update an existing Hive tree partition by partition (only the partitions in the result are swapped or extended, the rest stay byte-identical; schema and `parqit.*` metadata must match the tree, and a refusal names what differs, variable by variable); `encoding()` names the code page for text that is not valid UTF-8 (any parqit reads; by default the session's, `windows-1252` unless `parqit set encoding`; `encoding(name, all)` for text that is valid UTF-8 by accident); `copysource` (with `data`) copies the unchanged file loaded by the last `parqit use ..., clear` instead of reading memory, refusing loudly unless the file's identity, names, count and sort order still match; `xmissing` (a memory save) preserves extended missing values `.a`–`.z` in one `int8` companion column per affected variable (`_parqit_xm_<var>`, 0 = none, 1–26 = `.a`–`.z`, listed under the `parqit.xmissing` footer key) that `parqit use`, `mergein` and `appendin` restore for every numeric storage type; the file stays ordinary Parquet for other readers. |
| `parqit save <dest> using <file.sav> [, replace compression() compression_level() encoding()]` | Convert an SPSS `.sav`/`.zsav` file to Parquet with its whole dictionary, out of core, leaving the dataset in memory and the open views as they were (see [SPSS files](#spss-files-sav-zsav)); returns `r(N)`, `r(k)`, `r(filename)`, `r(source)`, `r(xmissing_vars)`, `r(encoding)`, `r(spss_encoding)`, `r(spss_compression)`. |
| `parqit save <dest> using <file.rds\|file.RData> [, replace compression() compression_level() encoding() object()]` | Convert the data frame of an R data file to Parquet with its factors, dates, haven labels, missing-value codes and attributes, out of core and without R (see [R data files](#r-data-files-rds-rda-rdata)); `object()` names the data frame when the file holds several; returns `r(N)`, `r(k)`, `r(k_dropped)`, `r(filename)`, `r(source)`, `r(xmissing_vars)`, `r(r_object)`, `r(r_format)`, `r(r_version)`, `r(r_encoding)`, `r(r_compression)`. |
| `parqit spssencode <strvar>, generate(<newvar>) [label(<name>) sequential]` | Labelled numeric version of a string variable read from an SPSS file (or an R file with haven labels, `char var[r_value_labels]`), from its `char var[spss_value_labels]`: the SPSS codes when they are distinct integers, otherwise 1, 2, … in code order; user-missing codes → `.a`–`.z` (see [SPSS files](#spss-files-sav-zsav)); returns `r(mode)`, `r(label)`, `r(N_labels)`, `r(N_unlabeled)` and `r(missing_map)`. |
| `parqit count` | Row count → `r(N)` (only the scalar result is returned). |
| `parqit head [n]` / `parqit list [varlist] [if] [in]` | Preview a small slice. |
| `parqit summarize` / `parqit tabulate` | Pushed-down summaries → `r()`; `tabulate` shows value labels (`nolabel` for codes). Every statistics command (`summarize`, `tabulate`, `tabstat`, `distinct`, `duplicates`, `correlate`, `pwcorr`, `codebook`, `misstable`, `levelsof`, `histogram`) takes `[if exp]`, which restricts the rows it reads and leaves the view unchanged. |
| `parqit describe [file] [, labels notes]` / `parqit glimpse [file]` | File metadata (including rows and row groups), or the open view's schema; relevant results are returned in `r()`. The file form reads Parquet footers only (a `.csv`, `.dta`, Excel, SPSS or R file is refused with the alternative). For each variable it shows the value label and variable label, as Stata's `describe using` does. `*` marks variables with notes, `(spss)` marks string variables whose SPSS labels are kept in `char var[spss_value_labels]`, and `(r)` those whose R labels are in `char var[r_value_labels]`. `labels` lists the value-label sets and those SPSS labels; `notes` lists the notes. |

### Explore the view (engine-side, current dataset unchanged)

`count`, `head`, `list`, `summarize`, `tabulate` and `describe` above are
push-down queries too; these complete the exploration family — every one
computed by the engine, with only bounded summary output or preview rows
reaching Stata:

| Command | Effect |
|---|---|
| `parqit codebook [varlist]` | Per variable: type, obs, missing, distinct, min/max, label — in one scan |
| `parqit misstable [summarize\|patterns]` | Missing counts and shares; pattern frequencies and percentages of the full view (≤14 vars, top 100 patterns) |
| `parqit levelsof var [, limit()]` | Sorted distinct values → `r(levels)` (refuses beyond the cap, default 5,000) |
| `parqit distinct [varlist] [, joint missing]` | Distinct values per variable (and of the variables jointly), as the community-contributed `distinct` (SSC) reports them: `Obs` counts the nonmissing observations; `missing` counts missing as one more value |
| `parqit duplicates report\|list [varlist]` | Copies/observations/surplus table; the duplicated observations listed as native `duplicates list` does (Group, Obs, variables); all variables without a varlist |
| `parqit tabstat varlist, s() [by() save]` | Statistics × variables table (`n mean sd var sum min max range median p##`); `save` returns the computed tables in `r()` |
| `parqit correlate [varlist]` / `parqit pwcorr [varlist] [, obs sig]` | Listwise / pairwise correlations, with full returned matrices; every numeric variable without a varlist |
| `parqit histogram var [, bins() nodraw]` | Engine-computed bins, drawn with `twoway bar` |
| `parqit ds` / `parqit lookfor words` | Variable names → `r(varlist)`; search names and labels |

The statistical tables use native Stata layouts and numeric formats.
`summarize, detail` shows percentiles beside the four smallest/largest values,
with moments on the right. `tabstat, by()` preserves its group-only contract
(native `nototal` layout); `save` returns the same results without another
query. Printed rounding does not reduce the precision of the returned tables.
Labels come from the view, and the current dataset stays unchanged.

### Escape hatches

| Command | Effect |
|---|---|
| `parqit sql "<DuckDB SQL>" [, clear name()]` | Run raw DuckDB SQL; lazy by default (opens/replaces a view, current dataset untouched), or `clear` collects it. `name()` opens under a view name. A column named with a reserved SQL word (auto's `foreign`) is written in double quotes, `"foreign"`; parqit names the word when the engine stops at it. |
| `parqit query "<sql fragment>"` | Inject a raw fragment into the current pipeline (e.g. a `QUALIFY`). |
| `parqit show` / `parqit explain` | Print the generated SQL / the query plan. |
| `parqit set statamissing\|int64\|encoding\|fill_threads\|stream_buffer_mb\|threads\|memory_limit\|tempdir <value>` | Engine settings (missing-value mode, integer precision policy, the code page of legacy text that declares none, fill workers, streaming-buffer MB, DuckDB threads, memory budget and spill directory). Engine threads default to the available CPUs (`r(cpus)`; the affinity mask on Linux). Automatic fill workers also depend on result size and environment overrides; small reads use the serial path. Explicit positive counts up to the available CPUs are accepted; larger ones are clamped with a note. `version` reports `r(threads)`, `r(fill_threads)` and `r(stream_buffer_mb)`. |
| `parqit path <file>` / `parqit menu` / `db parqit_*` | Resolve a path (→ `r(path)`, `r(exists)`); install the **User > parqit** submenu (GUI Stata; one line in `profile.do` keeps it); the ten point-and-click dialogs, also listed in the help file's Dialog menu. |

### Point and click

`parqit menu` adds **User > parqit** to GUI Stata: Read data (lazy
view or into memory); Describe and explore data; Summary statistics, tables,
and correlations; Keep or drop observations, or draw a sample (with an `if`
frame, strata, whole clusters and an indicator); Keep, drop,
order, sort, or rename variables; Create or change variables (including the
labelled numeric version of an SPSS string variable); Collapse,
contract, pivot table, or reshape; Combine datasets (merge, append, joinby);
Save as Parquet or collect into memory; Views, SQL, and engine settings
(including copying a view into a new view);
Version; Self-test; Help; Technical reference. Every dialog builds an ordinary `parqit` command,
echoed to the Results and Review windows like a typed command, and follows
Stata's own dialog conventions: a **Populate** button fills the variable
pickers from the current view, from the dataset in memory when the write dialog
selects `data`, or from the Parquet footer of the file named in the dialog (as
Stata's `use`/`describe`/`merge` dialogs populate from a dataset on disk),
**Create...** opens the expression builder, an existing output file asks before
being replaced, and operations with many choices use a list box rather than a
wall of radio buttons. StataNow's native
`import parquet` (File > Import) reads a file into memory; parqit's dialogs
complement it and never alter Stata's own menus.

The context line and **Refresh** identify the view and update variable pickers.
Numeric calculations offer numeric variables; tabulations also offer strings,
separate row/column fields and `nolabel`. The write dialog starts with saving
a view and separates that from saving Stata memory, converting an SPSS or R
file (`parqit save … using`, with the R object to read) or collecting a view. The read and combine dialogs'
**Browse** lists every supported input type, together or one type at a time.
The read dialog names the data frame to load from an R file that holds several
(`object()`), and describing a Parquet file can list its value labels and notes.
View save/collect name the selected view in the emitted command, so closing it
cannot redirect a save to memory. Each Help button opens the relevant section.
Integer-precision selectors are available on read, collect, mergein and appendin:
`default` inherits the view/session policy, while choosing `refuse`, `round` or
`string` emits an explicit `int64()` option. The read, write and combine
dialogs have an editable encoding field that lists the common code pages of
every script and takes any other name (`name, all` included); its `default`
emits no `encoding()`, so the file's own declaration or the session code page
applies. The session dialog exposes all eight settings, including integer
precision, the session code page, fill workers and the streaming buffer.

**Tuning the read.** Reads of 50,000+ rows or 2 million+ cells fill Stata's memory in parallel (one
worker thread per CPU available to the process — the affinity mask on Linux, so
a cluster job's enforced affinity mask is honoured; no hard-coded cap).
The gain depends on whether scanning, conversion or filling dominates the
workload. `parqit set fill_threads 1` selects the
serial path in the session; any positive count up to the available CPUs is
accepted, and a larger one is clamped with a note. `auto` uses the environment
override when one is set, otherwise the size/CPU rule above.
To select serial filling before Stata starts, set the **operating-system** environment variable
`PARQIT_FILL_THREADS=0` *before launching Stata* (`export PARQIT_FILL_THREADS=0` in
your shell; the plugin reads it via `getenv`, so a Stata `global` will not reach
it). `PARQIT_FILL_THREADS=n` pins `n` workers. The parallel and serial fills are
byte-identical — only the scheduling differs.

That fill reads a *streamed* engine result: the engine's chunks are handed to
the fill directly instead of first being collected into a materialised result
and copied out again. The buffer that holds them is sized per read from the
result's estimated size to avoid repeated buffer stalls. This is a soft
engine-buffer cap; it does not bound the complete process or every possible
variable-width payload.
`parqit set stream_buffer_mb n` (in-session) or `PARQIT_STREAM_BUFFER_MB=n` caps
that buffer at `n` MB. Development measurements found lower peak memory on
some plain reads, with a slower fill once a small buffer forced repeated
stalls; this is a workload-dependent trade-off, not a fixed saving.
`0` restores the engine's own default before each fetch, normally
1 MB; `auto` uses the environment override or per-read sizing.
`PARQIT_FETCH_MATERIALIZED=1` restores the earlier materialised fetch — a
conservative fallback that changes no value.

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

* Excel-style pivot table: mean wage and a count, region rows × year columns
parqit use using panel.parquet
parqit pivot (mean) wage (count) n=wage, rows(region) cols(year)
parqit collect, clear                       // wage2019 n2019 wage2020 n2020 ...

* An SPSS survey to Parquet with its dictionary (labels, .a-.z codes, formats)
parqit save ess_round10.parquet using ess_round10.sav, replace
parqit use using ess_round10.parquet, clear
tabulate trstprl, missing                   // refusals, don't-knows: .a .b ... with labels

* An R data frame (factors, dates, haven labels) without R
parqit save households.parquet using households.rds, replace
parqit use using workspace.RData, clear object(people)   // one data frame of an .RData

* Text in other code pages: Cyrillic CSV files, a Chinese .dta from Stata 13
parqit use using prices_ru_*.csv, encoding(windows-1251)  // decoded to UTF-8, with a note
parqit collect, clear
parqit use using survey_zh.dta, clear encoding(gbk)
parqit set encoding windows-1251            // the session's code page for undeclared text

* Drop to SQL when a window function is clearest
parqit use using spells.parquet
parqit query "qualify row_number() over (partition by id order by start) = 1"
parqit collect, clear

* A 10% sample of workers with all their years since 2010, drawn on disk
parqit use using /data/qp_2002_2023/*.parquet
parqit keep if year >= 2010
parqit sample 10, cluster(id) seed(20260925)
parqit collect, clear

* Branch a pipeline: collect a narrow copy, keep the full view for later
parqit use using /data/qp_2002_2023/*.parquet, name(panel)
parqit keep if year >= 2010
parqit use id firmid year using view:panel, name(ids)
parqit collect, clear                       // three columns only
parqit view panel                           // all columns, still 2010 on
```

## Tour & examples

Two self-contained crash courses ship with parqit — as ancillary files of the
SSC package (`ssc install parqit, all replace` copies them into the current
directory) and in the repository's `examples/` directory. Both generate a small
artificial NLS-style labour panel under Stata's temporary directory, so they
need no data download, and each block names the matching **User > parqit**
dialog:

- **Start here:** `examples/parqit_basics.do` — writing and inspecting a
  Parquet file, eager and lazy reads, metadata round-trips, a reproducible
  engine-side sample, `collect` versus a direct Parquet `save` (including a
  partitioned directory and a multi-file glob), lazy merge and append,
  `mergein`/`appendin` for data already in memory, and a CSV converted to
  Parquet without loading it.
- `examples/parqit_tour.do` — exploring a lazy table without replacing the
  current data (the engine-side statistics family), the SQL-versus-Stata
  missing-value rule, richer lazy pipelines, `collapse`/`pivot`/`contract`/
  `reshape`, named views and a view-to-view merge, `joinby`, the `query`/`sql`
  escape hatches and the engine settings.

```stata
. adopath ++ "<repo>/ado/plus/p"      // development tree (every build refreshes it)
. do examples/parqit_basics.do
. do examples/parqit_tour.do
```

The oracle-checked twins of these tours are integration tests:
`tests/integration/t14_basics.do` (use/save/merge/append done the eager and
the lazy way, every lazy result asserted against a native-Stata twin) and
`tests/integration/t13_tour.do` (eleven sections over the artificial datasets
of `examples/make_data.py` — workers, firms, patents, wide incomes and a
deliberately hostile file — each asserted against a native twin). Both run
under `bash tests/run_stata.sh` and end in `VERDICT(...): PASS`.

The repository also holds two executed Jupyter notebooks, in
[`examples/notebooks/`](examples/notebooks/), that teach the same material step
by step on Stata's `nlsw88` data. The output of every cell is stored (produced
with parqit 0.2.1), so they can be read on GitHub without Stata; their README
explains how to rerun them in the nbstata kernel.

## Type mapping

`parqit` keeps an explicit, tested map between Stata types/formats and DuckDB/Arrow
logical types. It never silently nulls a value on overflow and never silently
rescales a date.

| Stata | DuckDB / Arrow | Notes |
|---|---|---|
| `byte` `int` `long` | `TINYINT` `SMALLINT` `INTEGER`/`BIGINT` | sized by range |
| `float` `double` | `FLOAT` `DOUBLE` | precision preserved; float32 beyond Stata's ±1.70e38 float ceiling widens to `double` (noted, never silently missing); NaN loads as missing, ±Inf loads as missing **with a note** |
| `str#` | `VARCHAR` | auto-sized on read |
| `strL` | `VARCHAR` (large) | values wider than 2045 UTF-8 bytes |
| `%td` | `DATE` | days since epoch (correct conversion) |
| `%tc` | `TIMESTAMP` | milliseconds; tz instant preserved |
| `%tm %tq %th %ty %tw` | `INTEGER` (period count) | **kept as integers with the period code**, never mis-scaled to calendar dates |
| `%tC %tb` | `BIGINT` / `INTEGER` counts | kept as integer counts with their display format; third-party readers see the raw counts |
| boolean | `BOOLEAN` → `byte` 0/1 | |
| `DECIMAL(p,s)` | → `double` on read | warehouse money types load as numbers, with a note because binary64 may round the decimal |
| `UINT32` `UINT64` | → `long`/`double` (bound-checked) | values above the signed range never become missing; values beyond 2^53 refuse the read unless `int64(round)`/`int64(string)` says what to do |
| `BIGINT` `HUGEINT` | → `double`, or `str#` with `int64(string)` | as above: exact while the values fit 2^53, and never rounded without being asked |
| `BLOB` | dropped, or `str#`/`strL` with `binary(text|hex)` | raw bytes have no Stata type; `text` refuses invalid UTF-8 loudly, `hex` is always exact |
| `LIST` `STRUCT` | error or drop-with-message | unrepresentable types are loud, never silent all-missing |

Unsigned integers, decimals and out-of-range values are bound-checked; unsupported
types are reported, never loaded as a column of silent missings.

## Stata metadata round-trip

A `parqit save` writes Stata's variable labels, value labels, notes, display formats
and characteristics into Parquet **key–value metadata** under a `parqit.*` namespace.
`parqit use` restores them. The file stays 100% standard Parquet for every other tool.
Representable values and metadata round-trip under the documented type
contract. Without `xmissing`, extended-missing categories collapse; fractional date/period counts
round, legacy text may be transcoded and binary strings have boundary limits.
These conversions are reported; see Limitations and `help parqit_technical`.

## Limitations

- **Resource budgets.** Out-of-core describes the disk-backed lazy pipeline.
  A collected result must fit in Stata, and DuckDB's `memory_limit` budgets its
  buffer manager rather than the complete process. The default memory writer
  (`save, data`, also used by `open _data` and adapters) assembles full Arrow
  buffers; `PARQIT_SAVE_NOARROW=1` before launching Stata selects the batched
  staging path. Reserve memory and temporary disk space for the chosen path.
- **Validation coverage.** CI builds and runs C++ tests for the four target
  OS/architecture combinations. The full licensed Stata suite is run locally
  on Linux; CI build success does not establish Stata runtime parity on every
  supported platform or Stata version.
- **Views are plans, not data** — open as many as you like
  (`parqit use using f.parquet, name(qp)`, `parqit view qp`, `parqit views`);
  `parqit collect` materialises the current view and keeps it open
  (re-collecting re-executes). With a view open, `parqit save` materialises
  the current view (and says so); `parqit save ..., data` exports the
  in-memory dataset instead. A copy (`parqit use … using view:qp, name(q2)`)
  copies the plan, not the data: if the files change, both views see the
  change. `parqit mergein`/`appendin` read a file, not a view.
- **Sampling designs** draw with parqit's own random numbers, so they never
  reproduce `sample2`'s draw, only its rules. See [Sampling designs](#sampling-designs)
  for the other differences: fractional-percentage ties, missing clusters and `count`.
- **Stata `if` vs SQL semantics.** By default, expressions follow SQL semantics
  (missing is `NULL`, not "larger than everything"); `x < .`-style idioms are
  translated faithfully either way. `parqit set statamissing on` emulates Stata's
  ordering in every comparison where it matters. Until a mode is chosen
  (`parqit set statamissing on|off`), parqit warns, in red, at each comparison
  whose result can differ from native Stata because a value is missing
  (in `keep`/`drop if`, `gen`, `replace`, `egen`, `sample if`, `count if`,
  `list if` and the `if` of the statistics commands); like any output, the
  warning is silenced by `quietly` and `capture`.
- **Extended missings** `.a`–`.z` collapse to a single null in Parquet (the
  format has one missing concept); `parqit save`, `parqit open _data` and any
  command that bridges a `.dta`/Excel source warn when this loses information.
  `parqit save ..., xmissing` (opt-in) keeps them: each affected variable gets
  an `int8` companion column `_parqit_xm_<var>` holding the code (1–26) and
  the pairs are recorded under the `parqit.xmissing` footer key; `parqit use`,
  `mergein` and `appendin` hide the companions and restore `.a`–`.z` in every
  numeric storage type, a corrupt companion is refused loudly, and a lazy view
  over such a file reads those cells as `.` and says so when opened (the
  companions are hidden there too). `copysource` and `partitions()` do not
  combine with it. A column declared both as a primary and a companion is
  refused on read. A multi-file source with differing `parqit.*` metadata and
  an extended-missing channel is also refused, even with `relaxed`: read the
  files separately and combine them with `parqit appendin` to preserve the
  codes. Labels attached to extended missings do survive (they live
  in `parqit.*` metadata). Since their identity is then unavailable in a view,
  `.a`–`.z` literals are rejected in lazy expressions; use `missing(x)` or
  compare with ordinary `.`.
  In a **`merge`/`joinby` key** the collapse stops being cosmetic: `.a` and `.`
  are then the same missing, and Stata matches missing with missing, so rows
  that native Stata kept apart now pair and `_merge` reports 3. Both verbs say
  so at join time — when the same key carries missing values on *both* sides,
  they print a `note:` naming the key and the two counts.
- **Row order after a lazy `merge`/`joinby`.** The result comes back grouped by
  the key, with a `sortedby` marker that is true, and that order is *not* the
  native one. What is guaranteed is the content — the same rows and cells as
  native `merge`, as a multiset. Within tied keys, order is not guaranteed,
  including on repeated execution. Native
  `merge`'s own within-key order is not reproducible: changing only the
  physical row order of the *using* file makes native `merge m:1` return the
  master in a different order. Code that depends on `_n` or `by:` should sort
  explicitly after collecting.
- **`int64`/`uint64` values above 2^53.** These are outside Stata's
  consecutively exact integer range, so parqit conservatively **refuses
  the read by default**, even if particular larger values are representable
  (`r(198)`, naming every such column; nothing is
  staged, the data in memory is untouched). Choose what should happen:
  `int64(string)` loads those columns as exact text (the cast happens in the
  engine, over the integer); on an eager read, preserved `.a`–`.z` codes become
  the text `.a`–`.z` in those converted columns, ordinary missing becomes an
  empty string, and corrupt codes still refuse the load. `int64(round)` accepts the nearest `double` with
  the note `values beyond 2^53 rounded to nearest double` — after which two
  distinct keys can be one observation. `parqit set int64 refuse|round|string`
  moves the session default; `parqit head`/`parqit list` always preview the
  exact digits. A **lazy** `merge` or `joinby` over such a key is exact
  whatever you choose, because the join runs in the engine before any Stata
  `double` exists; only what is then collected is affected. The manual recipe
  still works: `parqit sql "SELECT …, CAST(col AS VARCHAR) AS s FROM
  read_parquet('f.parquet')"`.
- **Binary (`BLOB`) columns** have no Stata representation and are dropped
  with a message. `parqit use ..., binary(text)` loads them as UTF-8 text
  (and refuses loudly, naming the column, if any row is not valid UTF-8);
  `binary(hex)` loads two uppercase hex digits per byte, which always works.
  On the lazy path the option belongs to `parqit use using` — a view's
  columns are decided when it is opened.
- **Legacy (non-UTF-8) text.** Parquet strings must be UTF-8. `parqit save`
  transcodes string cells, labels, value labels, notes and characteristics
  that carry the raw bytes of a legacy code page (data saved by Stata 13
  and earlier, or loaded without `unicode translate`) from the `encoding()`
  code page (default: the session's, `windows-1252` unless
  `parqit set encoding`) — item by item, like `unicode translate`,
  with no translate step on your side — and says so in a `note:`; valid UTF-8
  is written byte-exact. Legacy text that happens to be valid UTF-8 cannot be
  told apart item by item; see [Text encodings](#text-encodings) for how
  multibyte code pages, `encoding(name, all)` and old `.dta` files narrow that. Because a `.dta`/Excel source is read through a
  `parqit save` bridge, the same collapse/rounding/transcoding applies there
  and is now reported (with `encoding()` to choose the code page) by
  `parqit use`, `merge`/`joinby`/`append` and `parqit open _data`. A Parquet
  file is never transcoded on read: a foreign one whose string payload is not
  valid UTF-8 is refused loudly, naming the column.
- **Encoding limits.** ISO-2022-JP and other stateful
  encodings, EUC-TW, Big5-HKSCS, EBCDIC and UTF-32 are refused with a
  message. GB18030 is read as its 2005 edition (as ICU and glibc read it),
  without the 2022 edition's changes, and `big5` is Windows' code page 950, so
  the ETEN extensions of a Unix Big5 file (circled digits, kana) come out as
  Private Use characters. parqit does not guess a legacy code page from the
  text: it recognises only byte-order marks and the patterns of UTF-16 and
  UTF-32, and otherwise uses `encoding()` or the session code page. Delimited
  text that must be decoded is copied first, so it needs room in `c(tmpdir)`
  (UTF-8 can take more bytes than the code page did).
- **Temporary bridges** live in `c(tmpdir)` and belong to the Stata session
  that created them: they are removed when the last view using them is closed
  or replaced, but a session that ends with views open, or crashes, leaves its
  `_parqit_bridge_<kind>_<pid>_…` directories behind (`parqit close _all`
  before exit avoids it; such a directory can be deleted once no Stata session
  uses it). If the path of `c(tmpdir)` contains `=`, the engine reads the
  bridge directory as a Hive partition column of the bridged data, and parqit
  says so once a session — point `TMPDIR` (`STATATMP` on Windows) at a
  directory without `=`.
- **Names that differ only by case** (`nuemp`/`NUEMP`) are exact in the
  written file and in Stata (`save`, `use`, `collect`); inside a lazy view the
  second is addressed by a numbered alias (`NUEMP_1`, shown by
  `parqit describe`), and creating a lazy name that clashes only by case is
  refused — DuckDB's identifiers are case-insensitive. A `relaxed` union
  (the engine's `union_by_name`) matches the files' names case-insensitively:
  parqit recovers every column's exact name, notes a later file's column
  unioned into a case-variant, and refuses a union that would split one name
  across two columns. A Hive tree whose partition key differs only by case
  from a file column is refused (the engine would replace the column's values
  with the key); an empty column name loads as `v<position>`.
- **Identity checks on eager reads and direct collects.** Every
  matched file's identity is captured before planning and re-checked before
  and after the fetch (and the fetched types against the plan); a change fails
  loudly (`r(920)`) and leaves the dataset in memory untouched. This is not a
  snapshot transaction spanning all files and validation queries of a transformed
  pipeline; keep its sources stable during execution.
  `parqit save ..., data copysource` verifies identity, names,
  kinds, count, the sort marker and the first/last 64 observations only; an
  edit confined to the middle rows is not detected (you assert nothing
  changed).
- **`reshape long`/`reshape wide`/`pivot` generated names** must not clash with a live engine name
  even when differing only by case (`x1` vs `X1`): such a spread is refused
  rather than written as a duplicate-name file. The exact names restored in
  Stata must also remain unique; distinct existing aliases may still restore
  case-distinct Stata names.
- **Ties in a declared sort follow the files' row order** (ORDER-CARRIER-1). A
  view over Parquet files carries each row's position in its source through the
  verbs that keep rows, so within a tie `_n`, `keep in`, `list in`, `collect`,
  `save` and the Obs of `duplicates list` follow the rows' order in the files,
  the same at every evaluation — native Stata's order after `use`. After a verb
  that builds new rows (`merge`, `append`, `joinby`, `collapse`, `contract`,
  `reshape`, `pivot`, `query`, the `sample` designs, `duplicates drop` without a
  varlist), and in views over CSV files or `parqit sql`, the order within a tie
  is the engine's: add a unique tiebreaker to `parqit sort`/`gsort` before
  slicing when the identity of selected tied rows must be reproducible.
- **Lazy `parqit merge m:m` is refused before the plan or a using-side adapter
  is changed.** A lazy plan does not retain the physical within-key row order
  needed for Stata's sequential reuse rule. Use `parqit joinby` for the
  Cartesian many-to-many join, or `parqit mergein m:m` when native Stata's
  order-dependent sequential behaviour is deliberately required.
- **A `float` variable compared with a decimal literal** (`x == 0.1`,
  `x > 0.1`, `inrange()`, `inlist()`, `cond()`, `round()`) is compared in
  double, as native Stata does: `x == 0.1` is false for a float `x` holding
  0.1 and `x == float(0.1)` is the native idiom (`float()` is implemented).
  Integral literals beyond 2^24 also compare without narrowing the value to
  float. Bare wide integer/decimal columns retain exact comparison semantics:
  DOUBLE 2^53 differs from BIGINT 2^53+1, even though collection rounds the
  latter only if `int64(round)` explicitly accepts that conversion; the
  default refuses the load and `int64(string)` preserves the digits as text.
- **String partition keys.** `partition_by()` on a string variable refuses the
  values `NULL` and `__HIVE_DEFAULT_PARTITION__` (the engine names the
  directory of a *missing* partition that way and would read them back as
  missing); every other value round-trips exactly. A foreign tree carrying
  such a directory under a string key loads those rows with the key empty,
  with a note.
- **Binary `strL`s containing NUL** are refused on a direct memory-to-Parquet
  save because Stata's plugin interface exposes text; text `strL`s round-trip,
  and a lazy Parquet-to-Parquet save preserves those bytes without crossing the
  Stata boundary.
- **SPSS files.** User-missing values are `.a`–`.z` after `parqit use` (the
  Parquet file keeps them in companion columns) but plain `.` in a lazy view;
  more than 26 distinct user-missing values in one variable share `.z`; string
  user-missing values stay text. A value observed in only some files of a series
  can receive a different code in each, so compare `spss_missing_map` before
  appending files converted one by one. Portable (`.por`), encrypted, EBCDIC
  and non-IEEE files are refused, as is text declared in EBCDIC, UTF-16 or a
  code page parqit does not read, unless `encoding()` names the encoding the
  text is really in. A date
  variable holding times of day, or a time beyond 24 hours, changes type (with a
  note), so two files of a series can differ there too.
- **R data files.** Lists, nested data frames, matrices, complex and raw columns,
  `POSIXlt` and S4 columns are left out (each named in a note); a character
  column R still holds as the numbers of a deferred `as.character()` is stored as
  those numbers. `NA_character_` and `""` differ in the Parquet file only
  (Stata loads both as `""`); `NaN` and `Inf` load as `.`; integer row names are
  not kept. bzip2- and xz-compressed files and R's ASCII format are refused
  (re-save with R's default). A compressed file needs its uncompressed size free
  in `c(tmpdir)` while it is converted.

## Acknowledgements

`parqit` takes [`pq`](https://github.com/jrothbaum/stata_parquet_io) by **Jon
Rothbaum** as its starting point — the work from which the `parqit` solution was
designed — and re-bases the manipulation layer on an embedded
[DuckDB](https://duckdb.org) engine through the
[Apache Arrow C Data Interface](https://arrow.apache.org/docs/format/CDataInterface.html).
Jon Rothbaum's package, and the care he puts into its correctness, directly shaped
`parqit`'s design and test suite; the debt is gratefully acknowledged.

We warmly thank the **[BPLIM](https://bplim.bportugal.pt/)** team at **Banco de
Portugal**, whose interaction throughout greatly benefited the development of
`parqit`.

`parqit` was built with the assistance of two AI coding agents used in tandem —
Anthropic's **Claude** (via Claude Code) and OpenAI's **Codex** — for
implementation, adversarial cross-auditing and cross-platform release work, under
the maintainer's direction and review. Both contributed to the making of `parqit`.

## License

parqit's own code is MIT — see [LICENSE](LICENSE). Embedded dependencies and
compiler runtimes retain their own licences; the installable package includes
`parqit_openmp_license.txt` (a legacy filename for the current runtime notices).

## Citation

If you use `parqit` in published work, please cite it (a `CITATION.cff` is included).
Authors: **Miguel Portela** · NIPE / Universidade do Minho and BPLIM / Banco de Portugal
(miguel.portela@eeg.uminho.pt); **Rute Costa** (ricosta@bportugal.pt),
**Paulo Guimarães** (pfguimaraes@bportugal.pt) and **Marta Silva**
(msilva@bportugal.pt) · BPLIM / Banco de Portugal.
