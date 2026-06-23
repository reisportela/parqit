# Vendored dependencies

| Component | Version | Source | License | Where |
|---|---|---|---|---|
| DuckDB | **1.5.3** (source tree, SHA256-pinned, fetched at configure) | `https://github.com/duckdb/duckdb/archive/refs/tags/v1.5.3.tar.gz` | MIT | CMake FetchContent (see `CMakeLists.txt`); offline: `-DPARQIT_DUCKDB_ARCHIVE=` |
| Stata Plugin Interface | **3.0.0** | `https://www.stata.com/plugins/` | StataCorp (distributed for plugin authors) | `stata/stplugin.c`, `stata/stplugin.h` |
| Arrow C Data Interface | spec header (stable ABI) | `https://raw.githubusercontent.com/apache/arrow/main/cpp/src/arrow/c/abi.h` | Apache-2.0 | `arrow/abi.h` |
| nlohmann/json | **3.12.0** | `https://github.com/nlohmann/json/releases/download/v3.12.0/json.hpp` | MIT | `json/json.hpp` |
| doctest | **2.4.12** | `https://raw.githubusercontent.com/doctest/doctest/v2.4.12/doctest/doctest.h` | MIT | `doctest/doctest.h` (tests only, never shipped) |

Pins:

- DuckDB tarball SHA256
  `f22a7cfb3e72be3010f4a7f2fbdd8de7d62fa036b838543acb663a722a7a71df`
  (verified by CMake on every fetch).
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
