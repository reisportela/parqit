# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`parqit` is a Stata package — "dbplyr's architecture with Stata's vocabulary" — that compiles lazy Stata-flavoured verbs (`keep`, `gen`, `collapse`, `merge`, …) into a single DuckDB SQL query executed out-of-core over Parquet. Only a materialiser moves data: `parqit collect` streams the result into Stata's memory (atomically), `parqit save` writes Parquet → Parquet without ever touching Stata memory. It is not another Parquet reader; the product is the manipulation layer and those two data paths.

**`parqit_build_prompt.md` is the authoritative build brief.** Where it states a decision ("must", "do not"), treat it as fixed — do not relitigate. Where it is silent, use judgement consistent with the thesis and record the assumption in `ASSUMPTIONS.md` instead of guessing silently. `README.md` documents the public command surface (brief §3) — stable once published, additive changes only; changes to it must be flagged in `CHANGELOG.md` and `ASSUMPTIONS.md`.

Build proceeds milestone by milestone (brief §11): M0 skeleton/CMake/vendoring/CI → M1 plain I/O + type fidelity → M2 single-table verbs + expr translator → M3 two-table verbs → M4 reshape/sql/polish → M5 SSC release. One feature branch per milestone, Conventional Commits, never commit a state that breaks the build.

## Required reading (external, read-only — do not modify)

- **House style:** `/home/mangelo/Documents/GitHub/xhdfe` — the maintainer's own Stata+plugin package. Match its conventions exactly: `*! version X.Y.Z DDmonYYYY` ado banner, `version 16.0` baseline, SMCL `.sthlp` layout, CMake style, author/support block, MIT license text (adjust year/title). parqit should look like it came from the same hand.
- **Correctness charter:** `/home/mangelo/Documents/BPLIM_GitHub/pq_audit_2026-06-11` — a 14-finding audit of `pq 3.0` (`PQ_AUDIT_REPORT.md`, `issues/`, `verify_suite/`). Every finding is an engine-independent hazard of any Stata↔columnar bridge: each must be impossible by design in parqit (brief §6) and covered by a ported verify test.
- Prior art for mechanics only: https://github.com/jrothbaum/stata_parquet_io — SSC `.pkg` format, per-OS release workflow (binary renamed to `*.plugin`, old-glibc Linux build), ado-pre-creates/plugin-fills pattern. Reimplement in C++/CMake/DuckDB; do not copy its Rust/Polars stack.

## Architecture (fixed decisions — brief §2)

```
parqit.ado (verbs → logical plan → DuckDB SQL)
   │  Stata Plugin Interface (stplugin.c/.h linked directly from C++, extern "C")
   ▼
C++ plugin ──► embedded DuckDB (vendored amalgamation, pinned under vendor/duckdb/)
   │                 │  reads/writes Parquet directly; out-of-core; pushdown
   ▼                 ▼
Arrow C Data Interface (header-only ABI; zero-copy DuckDB result transfer)
   ├─ collect → fill Stata's dataset via SF_* API
   └─ save    → COPY … TO Parquet (Stata memory untouched)
```

- **No Arrow C++ library, no Arrow Parquet reader.** DuckDB reads Parquet; Arrow is only the C Data Interface structs.
- **No Polars in v1.** Keep the verb→plan layer engine-agnostic behind a thin interface; implement only the DuckDB backend.
- v1 bridge for in-memory Stata data → DuckDB: temp-Parquet (correctness first); the Arrow-scan path is a later optimisation.
- Verify every DuckDB/Arrow/Stata API call against the vendored headers — never call functions from memory.

## Commands

Toolchain on this machine: gcc/g++, CMake 3.26, `duckdb` CLI, python3 with pyarrow (test oracles), Stata 16+ at `/usr/local/stata/` (`stata-mp` is on PATH). The maintainer develops on macOS and targets Linux/HPC — keep everything portable; end users install nothing beyond the plugin.

Agents may use the maintainer's local Stata by issuing `stata-mp` (batch preferred: `stata-mp -b do <file>.do`). Keep Stata test state repo-local — prepend the repository's ado directory inside the test do-file and capture logs next to the test output; never modify the global ado tree, profile, license, or Stata installation unless explicitly asked.

```bash
# Build (see BUILDING.md; presets per platform)
cmake --preset linux && cmake --build --preset linux

# Run a do-file in batch Stata (log lands as <name>.log in cwd)
stata-mp -b do tests/integration/<name>.do

# Full test suite — exits nonzero on any FAIL
bash tests/verify_suite/run_all.sh

# One verify test: run, then check its verdict line
stata-mp -b do tests/verify_suite/<name>.do && grep VERDICT <name>.log
```

`.gitignore` excludes all `*.parquet`/`*.dta` except `tests/fixtures/**` — commit small fixtures explicitly, never real data. `AGENTS.md`, `CLAUDE.local.md` and `.claude/` are intentionally untracked (per-machine).

## Testing discipline (brief §9)

- Verify tests follow the audit's `verify_suite` pattern: one self-contained do-file per invariant that generates its own synthetic data, asserts the exact failure signature, checks the on-disk payload with an **independent oracle** (pyarrow and/or duckdb CLI — never trust parqit-only round-trips), and prints `VERDICT: PASS/FAIL`.
- C++ unit tests for: the type map, the Stata-expr→SQL translator, the identifier sanitiser, the column manifest, the metadata (de)serialiser.
- Round-trip property tests: every Stata type with/without missings; 0 rows, 1 row, 1 var, 2500+ vars, multi-row-group, UTF-8/emoji, pathological column names.
- CI (`.github/workflows/build.yml`) must be green on Linux/macOS(x86_64+arm64)/Windows before any release tag; Linux builds against old glibc (almalinux:8/manylinux) for EL-family HPC clusters.

## Correctness invariants (brief §5–§6 — the root-cause discipline)

- A **column manifest** `(source_name, stata_name, dtype, format, position)` travels with every operation. Key the engine by source name; use the Stata name only when creating the Stata variable. Never index positionally by accident; carry names in compound-quoted lists, never whitespace-tokenised macros.
- Internal helper columns use tempnames, never fixed magic names that could collide with user columns.
- **Loud errors:** every plugin entry returns a real `ST_retcode`; the ado checks `_rc`; failures are nonzero rc **plus** a message — never rc 0 with a stale/missing file.
- **Atomic validate-then-mutate:** `collect`/`use, clear` stage into a temp frame and swap on success; never destroy the in-memory dataset before the new data is known good.
- Type contract highlights: `%tm/%tq/%th/%ty/%tw` stay INTEGER period counts (never mis-scaled to calendar dates); uint32/uint64/decimal are bound-checked numbers, never silent nulls; unsupported types (LIST/STRUCT) error or drop-with-message, never a silent all-missing column; string lengths are bytes (UTF-8), respecting the 2045-char `str#`/`strL` boundary.
- Stata metadata (variable/value labels, notes, formats, characteristics) round-trips via Parquet key–value metadata under a `parqit.*` namespace; the file stays standard Parquet for third parties.
- Expressions default to SQL missing semantics; a `statamissing` mode emulates Stata's "missing is larger than everything" ordering. The translator is a focused, unit-tested module.
- Never invent benchmark numbers or API behaviour. Unsure → read the vendored header / local Stata docs; still unsure → `ASSUMPTIONS.md` + the conservative option.

## Repository layout (target — brief §10)

```
src/plugin/    C++: entry, dispatch, SF_* bridge, Arrow transfer
src/engine/    DuckDB session, SQL builder, type map, manifest
src/ado/p/     parqit.ado, parqit.sthlp, parqit.pkg
vendor/        duckdb/, stplugin.c/.h, Arrow C-data header, VERSIONS.md
tests/         verify_suite/ (ported audit invariants + run_all), roundtrip/, unit/, integration/, fixtures/
```
