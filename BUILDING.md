# Building parqit

`parqit` ships as two artifacts: the platform-independent Stata files
(`parqit.ado`, `parqit.sthlp`, `parqit.pkg`) and one compiled plugin per platform
(`parqit.plugin`). The plugin statically embeds DuckDB (with its parquet and
core_functions extensions) — end users install nothing else.

## Prerequisites

- CMake ≥ 3.16 and a C++17 compiler (GCC ≥ 10, AppleClang, or MSVC 2019+).
- Network access on the first configure (CMake fetches the pinned DuckDB
  source tarball and verifies its SHA256), **or** a pre-downloaded tarball
  passed via `-DPARQIT_DUCKDB_ARCHIVE=/path/to/duckdb-1.5.3.tar.gz`.
- No other dependencies: the Stata Plugin Interface, the Arrow C Data
  Interface header, nlohmann/json and doctest are vendored in `vendor/`.

## One-command builds

```bash
# Linux
cmake --preset linux && cmake --build --preset linux -j

# macOS (build the architecture you are on, or both)
cmake --preset macos-arm64  && cmake --build --preset macos-arm64 -j
cmake --preset macos-x86_64 && cmake --build --preset macos-x86_64 -j

# Windows (x64 Native Tools prompt)
cmake --preset windows && cmake --build --preset windows
```

The plugin lands at `build/<preset>/parqit.plugin`. The first build compiles
DuckDB from source and takes several minutes; afterwards only parqit's own
files recompile.

## Developer build + tests

```bash
cmake --preset dev
cmake --build build/dev --target parqit_plugin parqit_tests -j
ctest --preset dev          # C++ unit tests (doctest)
```

## Using parqit from the repo (recommended)

Every build maintains a repo-local installable ado tree at
**`<repo>/ado/plus/p`** — `parqit.ado`, `parqit.sthlp`, `parqit.pkg` are re-synced
whenever they change, and a **stripped** `parqit.plugin` is refreshed on every
relink (also when building just `--target parqit_plugin`). Point Stata at it
once (e.g. in your `profile.do`):

```stata
adopath ++ "/home/mangelo/Documents/GitHub/parqit/ado/plus/p"
parqit version
parqit selftest
```

Nothing else is needed: the ado finds the plugin in the same directory.
Note that a running Stata keeps the plugin it already loaded — restart the
session (or `discard`) after rebuilding.

## Alternative: explicit dev override

To run against the unstripped build artifact directly (what the test
runner does):

```stata
adopath ++ "/path/to/parqit/src/ado/p"
global PARQIT_PLUGIN_PATH "/path/to/parqit/build/dev/parqit.plugin"
```

## Stata integration tests

CI cannot run Stata (no license on the runners); the integration and verify
suites run on a licensed machine:

```bash
bash tests/run_stata.sh                # everything, prints a verdict summary
STATA=/usr/local/stata/stata-mp bash tests/run_stata.sh m0_smoke
STATA=stata-mp BUILD_DIR="$PWD/build/dev" \
  bash tests/run_stata.sh x01_bridge_xproc  # two Stata processes, one TMPDIR
```

Each `.do` test is self-contained, generates its own data, prints a final
`VERDICT(...): PASS/FAIL` line, and where it checks on-disk payloads it does
so with an independent oracle (pyarrow via Stata's Python, or the duckdb
CLI) — never with parqit alone.

The `x01_bridge_xproc` gate is a purpose-built wrapper rather than a single
do-file. It starts two licensed Stata processes with the same temporary
directory (including spaces and Unicode), coordinates them with marker files,
checks both logs and bridge paths, and removes only its own scratch. The full
local runner includes this gate; GitHub-hosted C++ CI does not provide Stata
runtime coverage.

## Release packaging

Pushing a `v*` tag runs `.github/workflows/build.yml`, which builds three
binaries (Linux x86_64 against glibc 2.28 in an AlmaLinux 8 container, macOS
arm64 with deployment target 11.0, and Windows x86_64 with MSVC). The workflow
publishes one net-installable zip per platform plus
`parqit_all_platforms.zip`, and uploads the loose Stata files and per-platform
plugins needed for direct `net install` from the GitHub release URL. macOS
Intel is intentionally omitted until a reliable hosted runner is available,
as documented in the README and the workflow matrix.

The upload workflow collects `ado/plus/p/parqit.plugin`, the repo-local
distribution surface produced by CMake, rather than the raw
`build/<preset>/parqit.plugin`. On Unix that staged copy has passed the
platform strip command; on Windows it is the MSVC Release binary. Immediately
after collection, the workflow verifies the exact `out/parqit.plugin` it will
upload:

```bash
bash tests/verify_collected_plugin.sh out/parqit.plugin linux   # or macos/windows
```

The Linux check requires ELF64, exported `stata_call`/`pginit`, no ordinary
`.symtab` or debug sections, and no runtime `libstdc++`/`libgcc_s` dependency.
The macOS check recognises Mach-O and the required exports after `strip -x`;
the Windows check recognises PE/COFF and the required exports. Those structural
checks do not substitute for running Stata on each platform.
