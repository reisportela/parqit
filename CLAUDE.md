# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`parqit` is a Stata package — "dbplyr's architecture with Stata's vocabulary" — that compiles lazy Stata-flavoured verbs (`keep`, `gen`, `collapse`, `merge`, …) into a single DuckDB SQL query executed out-of-core over Parquet. Only a materialiser moves data: `parqit collect` streams the result into Stata's memory (atomically), `parqit save` writes Parquet → Parquet without ever touching Stata memory. It is not another Parquet reader; the product is the manipulation layer and those two data paths.

Current state: **v0.1.20** (see [CMakeLists.txt](CMakeLists.txt) `project(... VERSION)`). The full command surface in `README.md` is implemented (single-table verbs, two-table verbs, reshape, pivot, sql/query, native `mergein`/`appendin`, dialogs, metadata round-trip). Milestones M0–M4 (brief §11) are in the tree; M5 is SSC-release polish. The version/date lives in synchronised CMake, ado/help/dialog, README, package-manifest and citation surfaces; `tests/release_lint.sh` fails the build if they drift. Bump them together.

**`parqit_build_prompt.md` is the authoritative build brief.** Where it states a decision ("must", "do not"), treat it as fixed — do not relitigate. Where it is silent, use judgement consistent with the thesis and record the assumption in `ASSUMPTIONS.md` instead of guessing silently. `README.md` documents the public command surface (brief §3) — stable once published, additive changes only; changes to it must be flagged in `CHANGELOG.md` and `ASSUMPTIONS.md`. Per the non-regression rule (`AGENTS.md`): never remove a feature, reduce precision, corrupt metadata, weaken an error path, or change public command semantics silently. Correctness is the first gate; performance work only after the fidelity/type/metadata/oracle checks pass.

## Required reading (external, read-only — do not modify)

- **House style:** `/home/mangelo/Documents/GitHub/xhdfe` — the maintainer's own Stata+plugin package. Match its conventions exactly: `*! version X.Y.Z DDmonYYYY` ado banner, `version 16.0` baseline, SMCL `.sthlp` layout, CMake style, author/support block, MIT license text (adjust year/title). parqit should look like it came from the same hand.
- **Correctness charter:** `/home/mangelo/Documents/BPLIM_GitHub/pq_audit_2026-06-11` — a 14-finding audit of `pq 3.0` (`PQ_AUDIT_REPORT.md`, `issues/`, `verify_suite/`). Every finding is an engine-independent hazard of any Stata↔columnar bridge: each must be impossible by design in parqit (brief §6) and covered by a ported verify test. The in-repo `PARQIT_*AUDIT*.md` reports are later adversarial passes over parqit itself; their fixes are pinned by `tests/verify_suite/v27`–`v32` and `audit_repro/`.
- Prior art for mechanics only: https://github.com/jrothbaum/stata_parquet_io — SSC `.pkg` format, per-OS release workflow (binary renamed to `*.plugin`, old-glibc Linux build), ado-pre-creates/plugin-fills pattern. Reimplement in C++/CMake/DuckDB; do not copy its Rust/Polars stack.

## Architecture (fixed decisions — brief §2)

```
src/ado/p/parqit.ado   subcommand dispatch → _parqit_<verb> → Mata builds a JSON
   │                   request (every user string hex-encoded UTF-8) and calls:
   │   plugin call parqit_plugin [varlist] [in], <subcommand> <hex-args...>
   ▼
src/plugin/  C++ Stata Plugin Interface (stplugin.c/.h linked in, extern "C")
   │   parqit_plugin.cpp  entry stata_call + subcommand dispatch (catch-all → loud rc)
   │   plugin_io.cpp      use/describe/save + the SF_* collect (fills Stata cells)
   │   plugin_view.cpp    the lazy-view subcommands (open, op, twotable, reshape, …)
   ▼
src/engine/  no Stata API here — unit-testable without a Stata process
   │   session.cpp   one embedded DuckDB instance/session; temp_directory = spill
   │   view.cpp      THE verb→plan compiler: each verb appends a CTE stage; compile()
   │   exprtrans.cpp Stata expression → SQL (the focused, unit-tested translator)
   │   typemap.cpp   Stata type/format ↔ DuckDB/Arrow logical type
   │   sanitize.cpp  identifier sanitiser; request.cpp/hexcodec.cpp  the wire protocol
   ▼
embedded DuckDB (FetchContent, SHA256-pinned source tarball, built from source)
   │   reads/writes Parquet directly; out-of-core; pushdown
   ▼
Arrow C Data Interface (vendor/arrow/abi.h — header-only ABI; zero-copy result transfer)
   ├─ collect → fill Stata's dataset via SF_* API
   └─ save    → COPY … TO Parquet (Stata memory untouched)
```

- **DuckDB is built from source, not vendored.** `CMakeLists.txt` `FetchContent`s the pinned `v1.5.3` tarball (SHA256-checked; offline via `-DPARQIT_DUCKDB_ARCHIVE=...`) and links `duckdb_static` + the `parquet` and `core_functions` extensions statically — the released amalgamation ships *neither* extension, which is why the source build is required. `vendor/` holds only `stata/stplugin.{c,h}`, `arrow/abi.h`, `json/json.hpp`, `doctest/`.
- **No Arrow C++ library, no Arrow Parquet reader.** DuckDB reads Parquet; Arrow is only the C Data Interface structs.
- **No Polars in v1.** Keep the verb→plan layer engine-agnostic behind a thin interface; implement only the DuckDB backend.
- v1 bridge for in-memory Stata data → DuckDB: temp-Parquet (correctness first); the Arrow-scan path is a later optimisation. The `mergein`/`appendin` verbs instead keep data in Stata and run a *native* merge/append against a parqit-read disk side.
- **The ado↔plugin wire is all hex.** Every user-originated string crossing the boundary is lowercase hex of its UTF-8 bytes (Mata encodes; `engine/hexcodec` decodes), so arbitrary paths/labels/identifiers survive intact and column *names* are never whitespace-tokenised. Scalars return via `SF_macro_save` into `_parqit_*` caller locals; bulk results travel as `kind|field|...` line-records with text fields hex-encoded (`engine/request.cpp`). Honour this: never pass a raw user string as a plain plugin arg.
- Verify every DuckDB/Arrow/Stata API call against the fetched/vendored headers — never call functions from memory.

## Commands

Toolchain on this machine: gcc/g++, CMake 3.26, `duckdb` CLI, python3 with pyarrow (test oracles), Stata 16+ at `/usr/local/stata/` (`stata-mp` on PATH). The maintainer develops on macOS and targets Linux/HPC — keep everything portable; end users install nothing beyond the plugin.

```bash
# Configure + build. `dev` (RelWithDebInfo + tests) is the working preset;
# linux/macos-*/windows are the release presets. Plugin lands at build/<preset>/parqit.plugin.
# The FIRST build compiles DuckDB from source (several minutes); afterwards only parqit recompiles.
cmake --preset dev && cmake --build build/dev -j

# Every build also refreshes the repo-local install tree ado/plus/p/ (ado+help+pkg synced,
# plugin stripped-and-copied). Point Stata at it once:  adopath ++ "<repo>/ado/plus/p"
# A running Stata keeps the plugin it loaded — `discard` or restart Stata after a rebuild.

# C++ unit tests (doctest; the parqit_engine layer, no Stata needed)
ctest --preset dev                                   # or: ./build/dev/parqit_tests
./build/dev/parqit_tests --test-case='*sanitize*'    # one suite by name

# Stata suites — integration + verify_suite + roundtrip, clean batch processes,
# prints a VERDICT summary, exits nonzero on any FAIL. CI cannot run these (no license).
bash tests/run_stata.sh                  # everything
bash tests/run_stata.sh m0_smoke         # filter by name fragment (one test or family)
STATA=stata-mp BUILD_DIR="$PWD/build/dev" bash tests/run_stata.sh v06   # override plugin/stata

# Release version/date drift gate (text-only; runs without a build or Stata license)
bash tests/release_lint.sh
```

Each `.do` test takes `<repo> <plugin>` as arguments — `run_stata.sh` wires them and names the log `<test>.log`; do **not** run a verify `.do` bare. The runner judges purely by `VERDICT(...): PASS/FAIL` lines (a later PASS never masks an earlier FAIL). `BUILD_DIR` defaults to `build/dev`, `STATA` to `/usr/local/stata/stata-mp`.

When using local Stata directly, keep test state repo-local: prepend the repo ado dir and set `global PARQIT_PLUGIN_PATH` inside the do-file (or `adopath ++ "<repo>/ado/plus/p"`); never touch the global ado tree, profile, or license unless explicitly asked.

`.gitignore` excludes all build output, `*.plugin`/`*.so`, the whole `/ado/` and `/build/` trees, and every `*.parquet`/`*.dta` **except** `tests/fixtures/**` — commit small fixtures explicitly, never real data. `AGENTS.md`, `CLAUDE.local.md` and `.claude/` are intentionally untracked (per-machine). `release_lint.sh` also blocks any `/home/<user>/` or `/Users/<user>/` path from reaching tracked source/test/build files (the historical `.md` audit reports are exempt).

## Testing discipline (brief §9)

- Verify tests follow the audit's `verify_suite` pattern: one self-contained do-file per invariant that generates its own synthetic data, asserts the exact failure signature, checks the on-disk payload with an **independent oracle** (pyarrow and/or duckdb CLI — never trust parqit-only round-trips), and prints `VERDICT(...): PASS/FAIL`. `examples/parqit_tour.do` is the same idea at feature-tour scale (a native twin in memory as the oracle).
- C++ unit tests (`tests/unit/`) for: the type map, the Stata-expr→SQL translator, the identifier sanitiser, the request/response protocol, the view compiler, the hex codec.
- Round-trip property tests: every Stata type with/without missings; 0 rows, 1 row, 1 var, 2500+ vars, multi-row-group, UTF-8/emoji, pathological column names.
- CI (`.github/workflows/build.yml`) must be green on Linux/macOS(x86_64+arm64)/Windows before any release tag; Linux builds against old glibc (AlmaLinux 8) for EL-family HPC clusters. CI builds and runs the C++ tests but **cannot** run the Stata suites — those gate releases on a licensed machine.

## Correctness invariants (brief §5–§6 — the root-cause discipline)

- A **column manifest** (`ViewCol`: name, kind, format, varlabel, value-label, meta-type) travels with every operation in `view.cpp`. Key the engine by name; use the Stata name only when creating the Stata variable. Never index positionally by accident; carry names in compound-quoted lists / hex args, never whitespace-tokenised macros.
- Internal helper columns use generated names checked against the live schema (`View::fresh_helper`), never fixed magic names that could collide with user columns.
- **Loud errors:** every plugin entry returns a real `ST_retcode`; the `stata_call` catch-all converts any escaped C++ exception into a nonzero rc + `SF_error` (never let one cross the `extern "C"` boundary — it kills the Stata process). The ado checks `_rc`; failures are nonzero rc **plus** a message — never rc 0 with a stale/missing file.
- **Atomic validate-then-mutate:** `collect`/`use, clear` stage into a temp frame and swap on success; never destroy the in-memory dataset before the new data is known good.
- Type contract highlights (`typemap.cpp`): `%tm/%tq/%th/%ty/%tw` stay INTEGER period counts (never mis-scaled to calendar dates); uint32/uint64/decimal are bound-checked numbers, never silent nulls; unsupported types (LIST/STRUCT) error or drop-with-message, never a silent all-missing column; string lengths are bytes (UTF-8), respecting the 2045-char `str#`/`strL` boundary.
- Stata metadata (variable/value labels, notes, formats, characteristics) round-trips via Parquet key–value metadata under a `parqit.*` namespace; the file stays standard Parquet for third parties. One documented loss: extended-missing *categories* `.a`–`.z` collapse to a single `.` (labels survive; `save` warns).
- Expressions default to SQL missing semantics; `parqit set statamissing on` emulates Stata's "missing is larger than everything" ordering. The translator (`exprtrans.cpp`) is a focused, unit-tested module.
- Never invent benchmark numbers or API behaviour. Unsure → read the fetched DuckDB header / vendored Arrow/Stata header / local Stata docs; still unsure → `ASSUMPTIONS.md` + the conservative option.

## Repository layout

```
src/ado/p/     parqit.ado (dispatch + Mata wire), parqit.sthlp, parqit.pkg, stata.toc
src/plugin/    C++: parqit_plugin.cpp (entry+dispatch), plugin_io.cpp (I/O+collect), plugin_view.cpp
src/engine/    C++ (no Stata API): session, view (verb→plan), exprtrans, typemap, sanitize, request, hexcodec
vendor/        stata/stplugin.{c,h}, arrow/abi.h, json/json.hpp, doctest/, VERSIONS.md  (DuckDB NOT here — fetched)
tests/         unit/ (doctest), verify_suite/ (audit invariants), integration/, roundtrip/, fixtures/
               run_stata.sh (Stata runner), release_lint.sh (version/date + path-leak gate)
examples/      parqit_tour.do + make_data.py (self-verifying feature tour)
benchmarks/    parqit-vs-pq-vs-python harnesses (outputs git-ignored under benchmarks/_out)
audit_repro/   minimal repros for fixed adversarial-audit findings
ado/plus/p/    repo-local install tree, refreshed by every build (git-ignored)
build/<preset>/  CMake build dir; build/dev/parqit.plugin is what run_stata.sh loads (git-ignored)
```

One feature branch per milestone, Conventional Commits, never commit a state that breaks the build. Build the C++ and run the relevant unit + Stata tests before claiming a change is done.
