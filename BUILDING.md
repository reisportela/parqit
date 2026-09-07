# Building parqit

`parqit` ships as two artifacts: the platform-independent Stata files
(`parqit.ado`, `parqit.sthlp`, `parqit.pkg`) and one compiled plugin per platform
(`parqit.plugin`). The plugin statically embeds DuckDB (with its parquet and
core_functions extensions) and has OpenMP enabled on every platform. Linux
and macOS also embed the OpenMP runtime. The Windows package supplies
`parqit_vcomp140.dll` beside the plugin; standard `net install` installs both.
End users do not need a compiler or a separate runtime installer.

## Prerequisites

- CMake ≥ 3.21 for the supplied version-3 presets and a C++17 compiler
  (GCC ≥ 10 on Linux, GCC 14 on macOS, or MSVC 2019+).
- Network access on the first configure (CMake fetches pinned sources and
  verifies their SHA256), **or** pre-downloaded archives. DuckDB uses
  `-DPARQIT_DUCKDB_ARCHIVE=/path/to/duckdb-1.5.3.tar.gz`.
  Use a UTF-8 locale when configuring: the GCC source archive includes Unicode
  test filenames, even though only its OpenMP runtime is built.
- Linux/macOS build GNU libgomp 14.3.0 from source as a PIC static library.
  Its internal per-thread state uses pthread keys (`--disable-tls`), so loading
  the plugin does not depend on spare initial-exec TLS space in the Stata process.
  Offline builds also set `PARQIT_GCC_RUNTIME_ARCHIVE` to `gcc-14.3.0.tar.xz`.
  The compiler itself is not built from that archive. The pin is in
  [ParqitOpenMP.cmake](cmake/ParqitOpenMP.cmake).
- macOS uses Homebrew `gcc@14`; put its `bin` directory on PATH. Build each
  architecture on its corresponding host. The preset rejects a compiler
  targeting the wrong architecture. GNU OpenMP avoids the duplicate-runtime
  failure reproduced when LLVM OpenMP initializes beside Stata's private Intel runtime.
- Windows uses MSVC `/openmp`. CMake locates the installed x64 redistributable
  `vcomp140.dll`; `PARQIT_WINDOWS_OPENMP_DLL` can supply its explicit path.
  Debug configurations use the matching nonredistributable debug runtime and
  are never packaged as releases.
- The Stata Plugin Interface, the Arrow C Data
  Interface header, nlohmann/json and doctest are vendored in `vendor/`.

## One-command builds

```bash
# Linux
cmake --preset linux && cmake --build --preset linux -j

# macOS (run each preset on a host of that architecture)
cmake --preset macos-arm64  && cmake --build --preset macos-arm64 -j
cmake --preset macos-x86_64 && cmake --build --preset macos-x86_64 -j

# Windows (x64 Native Tools prompt)
cmake --preset windows && cmake --build --preset windows
```

The plugin lands at `build/<preset>/parqit.plugin`. The first build compiles
DuckDB from source and takes several minutes; afterwards only parqit's own
files recompile. The pinned engine also receives hash-checked local fixes:
uniform SQL reservoir sampling, C-API aggregate state flattening for windows,
safe block-allocator shutdown and initialized transaction error-policy flags.
See [PatchDuckDBSampling.cmake](cmake/PatchDuckDBSampling.cmake),
[PatchDuckDBCapi.cmake](cmake/PatchDuckDBCapi.cmake) and
[PatchDuckDBLifetime.cmake](cmake/PatchDuckDBLifetime.cmake). Configure refuses an
unexpected edited dependency file instead of overwriting it.

The statistics implementation requires IEEE binary64 evaluation without
fast-math reassociation. Its compile-time checks reject incompatible options.
Optimized builds compile its row callbacks at `-O3`. GCC on ELF x86-64 emits
both generic and FMA callbacks, selected by runtime CPU detection; the plugin
does not require an FMA-capable CPU. Other toolchains retain the generic path.

## Developer build + tests

```bash
cmake --preset dev
cmake --build build/dev --target parqit_plugin parqit_tests -j
ctest --preset dev          # C++ unit tests (doctest)
```

When Valgrind is available, CTest also runs the close/reopen/process-shutdown
regression under Memcheck. The Linux release runner installs it and requires
zero memory errors, including errors after the assertions have passed.

`PARQIT_LOCAL_ADO_DIR` can point to a separate staging directory while validating
a change. Its default is the repo's `ado/plus/p`; restore that location after
the staged build passes the relevant checks.

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

Pushing a `v*` tag runs `.github/workflows/build.yml`, which builds
binaries for four targets (Linux x86_64 in an AlmaLinux 8 container, macOS
x86_64 and arm64 with deployment target 11.0, and Windows x86_64 with MSVC). The workflow
prepares a draft release with one net-installable zip per platform plus
`parqit_all_platforms.zip`, and uploads the loose Stata files and per-platform
plugins needed for direct `net install` from the GitHub release URL. The
workflow uses `macos-15-intel` for the Intel build. Licensed Stata integration
tests remain a separate local gate, rather than a GitHub-hosted CI check.
The draft is published after those licensed checks pass against a staged
install and the collected artifacts have been verified.

The upload workflow collects `ado/plus/p/parqit.plugin`, the repo-local
distribution surface produced by CMake, rather than the raw
`build/<preset>/parqit.plugin`. On Unix that staged copy has passed the
platform strip command; on Windows it is the MSVC Release binary. Immediately
after collection, the workflow verifies the exact `out/parqit.plugin` it will
upload:

```bash
PARQIT_OPENMP_PROBE=build/linux/parqit_openmp_probe \
  bash tests/verify_collected_plugin.sh "$PWD/out/parqit.plugin" linux
```

The Linux check requires ELF64, exported `stata_call`/`pginit`, no ordinary
`.symtab` or debug sections, and no dynamic C++ or OpenMP runtime dependency.
The macOS link disables GCC's automatic runtime exports with `-nodefaultexport`,
then uses a two-symbol strip keep-list and ad-hoc signing. Its check
recognises Mach-O, verifies the signature and rejects leaked runtime exports;
the Windows check recognises PE/COFF, the required exports and the bundled
`parqit_vcomp140.dll`. Set the verifier path to `build/<preset>/parqit_openmp_probe`
on macOS, or `build/windows/Release/parqit_openmp_probe.exe` on Windows.
The verifier links no OpenMP runtime itself: it loads the exact collected plugin,
checks the compiled OpenMP capability, executes a two-worker region inside it
and runs the engine's Parquet/metadata selftest after stripping.
Missing OpenMP, ignored pragmas or a missing packaged DLL fail that check.
These checks do not substitute for running Stata on each platform.

OpenMP does not replace DuckDB's scheduler. SQL still uses `parqit set threads`,
and audited sums/moments are not converted into floating OpenMP reductions.
`parqit version` reports OpenMP support; `parqit selftest` exercises it in Stata.
The release includes SHA-256 checksums for its ZIPs and package files.
