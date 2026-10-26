# Vendored dependencies

| Component | Version | Source | License | Where |
|---|---|---|---|---|
| DuckDB | **2.0.0** (source tree, SHA256-pinned, fetched at configure; optional build: 1.5.6) | `https://github.com/duckdb/duckdb/archive/refs/tags/v2.0.0.tar.gz` | MIT | CMake FetchContent (see `CMakeLists.txt`); offline: `-DPARQIT_DUCKDB_ARCHIVE=` |
| Stata Plugin Interface | **3.0.0** | `https://www.stata.com/plugins/` | StataCorp (distributed for plugin authors) | `stata/stplugin.c`, `stata/stplugin.h` |
| Arrow C Data Interface | spec header (stable ABI) | `https://raw.githubusercontent.com/apache/arrow/main/cpp/src/arrow/c/abi.h` | Apache-2.0 | `arrow/abi.h` |
| nlohmann/json | **3.12.0** | `https://github.com/nlohmann/json/releases/download/v3.12.0/json.hpp` | MIT | `json/json.hpp` |
| doctest | **2.4.12** | `https://raw.githubusercontent.com/doctest/doctest/v2.4.12/doctest/doctest.h` | MIT | `doctest/doctest.h` (tests only, never shipped) |

Pins:

- DuckDB 2.0.0 is the default engine (the maintainer's decision); its
  acceptance record is in the migration/remediation reports. 1.5.6 remains an
  optional build (`PARQIT_DUCKDB_VERSION=1.5.6`), no longer distributed; its
  pins follow below. Its build sets `DUCKDB_OPTIMIZATION_PROFILE=NONE`, so the
  engine keeps the compiler's baseline CPU target, as 1.5.6 does (2.0's default
  profile compiles it for x86-64-v3, AVX2, on Linux). The 2.0.0 official tag is `v2.0.0`, commit
  `d54cbbe3df17e8e5ce68312f483b4155836112aa`, archive SHA256
  `7e89053255046bfb26afbd7f524a2fb84df7dbef496aa911ea35e4a1182fdc00`.
  `cmake/DuckDB200Sources.cmake` and its includes pin 50 reviewed sources:
  twenty-four verify upstream implementations, twenty-five apply guarded local edits,
  and one restores native 2.0 literal inference from the prior compatibility rewrite.
  These cover sampling, allocator teardown, aggregate-child window duplication,
  page validity/selection, CSV casts, physical NULL types, optional schemas,
  finite-result guards, join statistics, local paths, volatile window keys,
  legacy Hive markers, the legacy streaming-buffer
  default and parallel partitioned writes (delayed-partition routing and
  single-key packing in the partitioned COPY only). Prepared EXECUTE collection analyses the underlying plan's order and
  batch-index support, retaining the original plan's ownership; its dependencies
  are verified as well. The parqit scalar-registration adapter also verifies the C API's
  logical-type handle implementation. Histogram binding additionally verifies
  the stateless built-in literal casts and borrowed-vector access used to prove
  its valid constant bounds. Each original/corrected hash
  is recorded beside its exact transformation. Unknown edits, mixed line endings
  and existing staging are refused without overwriting them. C API v1 remains
  available; no switch to the new C API v2 is claimed.
- Version 0.1.37 removes all OpenMP runtimes. DuckDB continues to use its own
  scheduler and native worker threads. No `vcomp140.dll` is distributed.
- The GCC C/C++ runtimes linked into Linux/macOS plugins retain their GPL-3.0
  plus GCC Runtime Library Exception terms. Windows embeds the MSVC C/C++
  runtime under the applicable Microsoft terms. Notices retain the legacy
  filename `parqit_openmp_license.txt`; removing OpenMP does not relicense
  other compiler-runtime code.
- Optional 1.5.6 build: DuckDB tarball SHA256
  `1fadcbe9e69e1470f9093b6bcde08daf477d729c449e59a807f45c346622099b`
  (verified by CMake on every fetch). It receives the seven
  `cmake/PatchDuckDB*.cmake` corrections described in this list.
- The pinned sampler also receives the local, hash-guarded correction in
  `cmake/PatchDuckDBSampling.cmake`: SQL samples use uniform reservoir selection
  from the first replacement, with corrected skip indexing and bounded buffer
  capacity. Internal table-statistics sampling keeps its existing policy.
- `cmake/PatchDuckDBCapi.cmake` fixes the C aggregate bridge's state-vector
  flattening for window execution; the correction is also checked by source hash.
- `cmake/PatchDuckDBLifetime.cmake` removes access to a thread-local block cache
  from allocator destruction, when that cache may already have been destroyed.
  It also initializes transaction invalidation and automatic-rollback flags.
  Both source edits are hash-checked; the close/reopen/shutdown regression runs
  under Valgrind in Linux CI.
- `cmake/DuckDB200FileCache.cmake` and, for the optional 1.5.6 build,
  `cmake/PatchDuckDBFileCache.cmake` bind cached bytes to their source
  generation. They protect lookups, partial overlaps, in-flight invalidation
  and late insertions, including lazy opens and cache enable/disable cycles.
  Existing readers retain their source; new readers observe replacements.
  Stable cache hits still reuse buffers. Original/corrected hashes and patch
  guard tests cover both engines.
- `stata/stplugin.h` md5 `5916aa9797bdb05e9bdc0f5b2920dbaf`.
- `cmake/PatchDuckDBStreaming.cmake` preserves an executor worker's error when
  that error sets the stream's interrupt flag. Genuine user cancellation still
  reports interruption. An acquire fence on the interrupted path pairs with
  error publication; the successful fetch path is unchanged. The original and
  corrected source hashes are checked before any write.
- `cmake/PatchDuckDBWindowSelfJoin.cmake` prevents the window self-join
  optimiser from duplicating volatile input/window expressions or aggregate
  children, including order-dependent `first` and `any_value`. This preserves
  rows and keeps aggregates consistent with the returned values.
  Original and corrected source hashes are checked before any write.
- `cmake/PatchDuckDBLambdaVolatility.cmake` includes a lambda's bound body in
  `BoundFunctionExpression::IsVolatile()`, matching the engine's existing
  `IsFoldable()` check. Volatile lambda sources cannot bypass the window
  guard; pure lambdas retain their optimisation. Source hashes are checked.

Why a source build rather than the released amalgamation: verified on
2026-06-12 that `libduckdb-src.zip` for 1.5.x is the **bare engine** — the
`parquet` and `core_functions` extensions (both essential to parqit; even
`version()` lives in core_functions) are not part of it. A source build links
both statically by DuckDB's own default `extension/extension_config.cmake`,
which `tests/unit/test_session.cpp` asserts on every CI run. The plugin uses
DuckDB's **C API v1** (`duckdb.h`) for the bridge; registered native scalar
callbacks also use the reviewed C++ handles and vector interfaces.

Policy: pinned versions only; upgrades are deliberate commits that re-run the
full test suite.
