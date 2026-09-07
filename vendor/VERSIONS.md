# Vendored dependencies

| Component | Version | Source | License | Where |
|---|---|---|---|---|
| DuckDB | **1.5.3** (source tree, SHA256-pinned, fetched at configure) | `https://github.com/duckdb/duckdb/archive/refs/tags/v1.5.3.tar.gz` | MIT | CMake FetchContent (see `CMakeLists.txt`); offline: `-DPARQIT_DUCKDB_ARCHIVE=` |
| Stata Plugin Interface | **3.0.0** | `https://www.stata.com/plugins/` | StataCorp (distributed for plugin authors) | `stata/stplugin.c`, `stata/stplugin.h` |
| Arrow C Data Interface | spec header (stable ABI) | `https://raw.githubusercontent.com/apache/arrow/main/cpp/src/arrow/c/abi.h` | Apache-2.0 | `arrow/abi.h` |
| nlohmann/json | **3.12.0** | `https://github.com/nlohmann/json/releases/download/v3.12.0/json.hpp` | MIT | `json/json.hpp` |
| doctest | **2.4.12** | `https://raw.githubusercontent.com/doctest/doctest/v2.4.12/doctest/doctest.h` | MIT | `doctest/doctest.h` (tests only, never shipped) |

Pins:

- GNU libgomp **14.3.0** is built as a PIC static runtime on Linux/macOS by
  `cmake/ParqitOpenMP.cmake`. Only libgomp is compiled from
  `https://ftp.gnu.org/gnu/gcc/gcc-14.3.0/gcc-14.3.0.tar.xz`, SHA256
  `e0dc77297625631ac8e50fa92fffefe899a4eb702592da5c32ef04e2293aca3a`.
  License: GPL-3.0 with the GCC Runtime Library Exception, copied into the
  shipped `parqit_openmp_license.txt`.
- Windows OpenMP uses the installed MSVC toolchain's x64 `vcomp140.dll`
  redistributable. Its exact packaged bytes are identified by the release
  checksum manifest; no runtime is downloaded at plugin execution time.
- DuckDB tarball SHA256
  `f22a7cfb3e72be3010f4a7f2fbdd8de7d62fa036b838543acb663a722a7a71df`
  (verified by CMake on every fetch).
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
- `stata/stplugin.h` md5 `5916aa9797bdb05e9bdc0f5b2920dbaf`.

Why a source build rather than the released amalgamation: verified on
2026-06-12 that `libduckdb-src.zip` for 1.5.x is the **bare engine** — the
`parquet` and `core_functions` extensions (both essential to parqit; even
`version()` lives in core_functions) are not part of it. A source build links
both statically by DuckDB's own default `extension/extension_config.cmake`,
which `tests/unit/test_session.cpp` asserts on every CI run. The plugin uses
DuckDB's **C API** (`duckdb.h`) exclusively.

Policy: pinned versions only; upgrades are deliberate commits that re-run the
full test suite.
