cmake_minimum_required(VERSION 3.16)
project(parqit VERSION 0.1.21 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# DuckDB's static libraries link the STATIC MSVC runtime (/MT); parqit must match
# it or MSVC fails with LNK2038 (RuntimeLibrary MT vs MD mismatch). A static CRT
# also makes the shipped .plugin self-contained (no VC++ redistributable). Set
# before any target is created; relies on CMP0091 NEW (default at cmake ≥ 3.15).
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

option(PARQIT_BUILD_TESTS "Build the C++ unit tests (doctest)." OFF)

find_package(Threads REQUIRED)

set(PARQIT_VENDOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor")

# ---------------------------------------------------------------------------
# DuckDB, pinned by tag + SHA256 and built from source so that the parquet
# and core_functions extensions are statically linked in (the released
# amalgamation is the bare engine without either — verified 2026-06-12).
# Offline builds: -DPARQIT_DUCKDB_ARCHIVE=/path/to/duckdb-<ver>.tar.gz
# ---------------------------------------------------------------------------
set(PARQIT_DUCKDB_VERSION "1.5.3")
set(PARQIT_DUCKDB_SHA256 "f22a7cfb3e72be3010f4a7f2fbdd8de7d62fa036b838543acb663a722a7a71df")
set(PARQIT_DUCKDB_ARCHIVE "" CACHE FILEPATH
    "Optional path to a local duckdb-${PARQIT_DUCKDB_VERSION}.tar.gz for offline builds")

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
include(FetchContent)

if(PARQIT_DUCKDB_ARCHIVE)
    set(_parqit_duckdb_url "${PARQIT_DUCKDB_ARCHIVE}")
else()
    set(_parqit_duckdb_url
        "https://github.com/duckdb/duckdb/archive/refs/tags/v${PARQIT_DUCKDB_VERSION}.tar.gz")
endif()

# Shape the DuckDB subproject: no shell, no unit tests, no extension
# auto-download machinery. parquet + core_functions are loaded statically by
# DuckDB's own default extension_config.cmake.
set(BUILD_SHELL FALSE CACHE BOOL "" FORCE)
set(BUILD_UNITTESTS FALSE CACHE BOOL "" FORCE)
set(ENABLE_EXTENSION_AUTOLOADING FALSE CACHE BOOL "" FORCE)
set(ENABLE_EXTENSION_AUTOINSTALL FALSE CACHE BOOL "" FORCE)
# The release tarball has no .git: tell DuckDB its own version explicitly so
# version()/duckdb_library_version() report the pinned release, not v0.0.1.
set(OVERRIDE_GIT_DESCRIBE "v${PARQIT_DUCKDB_VERSION}-0-g0000000000" CACHE STRING "" FORCE)

FetchContent_Declare(duckdb
    URL "${_parqit_duckdb_url}"
    URL_HASH SHA256=${PARQIT_DUCKDB_SHA256}
)
FetchContent_MakeAvailable(duckdb)
set(PARQIT_DUCKDB_INCLUDE "${duckdb_SOURCE_DIR}/src/include")

# ---------------------------------------------------------------------------
# Engine: DuckDB session, manifest, type map, SQL builder. No Stata API here,
# so the unit tests can link it without a Stata process.
# ---------------------------------------------------------------------------
set(PARQIT_ENGINE_SOURCES
    src/engine/hexcodec.cpp
    src/engine/session.cpp
    src/engine/typemap.cpp
    src/engine/sanitize.cpp
    src/engine/request.cpp
    src/engine/exprtrans.cpp
    src/engine/view.cpp
)
add_library(parqit_engine STATIC ${PARQIT_ENGINE_SOURCES})
target_include_directories(parqit_engine PUBLIC
    src
    "${PARQIT_DUCKDB_INCLUDE}"
    "${PARQIT_VENDOR_DIR}/arrow"
    "${PARQIT_VENDOR_DIR}/json"
)
# DuckDB's static build splits the engine (duckdb_static), the statically
# linked extensions (<name>_extension) and the generated loader that
# registers them into separate archives with circular references; group them
# on GNU ld (macOS ld64 and MSVC resolve archives globally and need no group).
set(PARQIT_DUCKDB_LIBS duckdb_static duckdb_generated_extension_loader
    parquet_extension core_functions_extension)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND NOT APPLE)
    target_link_libraries(parqit_engine PUBLIC
        "-Wl,--start-group" ${PARQIT_DUCKDB_LIBS} "-Wl,--end-group")
else()
    target_link_libraries(parqit_engine PUBLIC ${PARQIT_DUCKDB_LIBS})
endif()
target_link_libraries(parqit_engine PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
# parqit's own translation units include duckdb.h and link the STATIC DuckDB, so
# DUCKDB_C_API must be empty (not __declspec(dllimport)) on Windows — otherwise
# the linker looks for __imp_duckdb_* DLL stubs the static lib never provides.
# No-op on Linux/macOS (the macro only gates the Windows dllimport branch).
# PUBLIC so it reaches parqit_plugin and parqit_tests, which also include duckdb.h.
target_compile_definitions(parqit_engine PUBLIC DUCKDB_STATIC_BUILD)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(parqit_engine PRIVATE -fvisibility=hidden -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# The Stata plugin: SF_* bridge + dispatch, linked with stplugin.c and the
# static DuckDB into a single self-contained shared library named
# parqit.plugin on every platform. Only stata_call/pginit are exported.
# ---------------------------------------------------------------------------
add_library(parqit_plugin SHARED
    src/plugin/parqit_plugin.cpp
    src/plugin/plugin_io.cpp
    src/plugin/plugin_view.cpp
    "${PARQIT_VENDOR_DIR}/stata/stplugin.c"
)
target_include_directories(parqit_plugin PRIVATE
    src
    "${PARQIT_VENDOR_DIR}/stata"
    "${PARQIT_DUCKDB_INCLUDE}"
    "${PARQIT_VENDOR_DIR}/arrow"
    "${PARQIT_VENDOR_DIR}/json"
)
target_link_libraries(parqit_plugin PRIVATE parqit_engine Threads::Threads ${CMAKE_DL_LIBS})
set_target_properties(parqit_plugin PROPERTIES OUTPUT_NAME "parqit" PREFIX "" SUFFIX ".plugin")
target_compile_definitions(parqit_plugin PRIVATE PARQIT_VERSION="${PROJECT_VERSION}")
if(NOT WIN32)
    target_compile_definitions(parqit_plugin PRIVATE SYSTEM=OPUNIX)
    # stplugin.c defines pginit, which Stata resolves via dlsym: it must keep
    # default visibility even though the rest of the plugin is hidden.
    set_source_files_properties("${PARQIT_VENDOR_DIR}/stata/stplugin.c"
        PROPERTIES COMPILE_DEFINITIONS SYSTEM=OPUNIX
                   COMPILE_OPTIONS "-fvisibility=default")
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(parqit_plugin PRIVATE -fvisibility=hidden -Wall -Wextra)
endif()
if(APPLE)
    target_link_options(parqit_plugin PRIVATE
        "-Wl,-exported_symbol,_stata_call"
        "-Wl,-exported_symbol,_pginit")
elseif(UNIX)
    target_link_options(parqit_plugin PRIVATE
        "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/plugin/plugin.map")
endif()
if(UNIX AND NOT APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # Single self-contained .so for HPC clusters: no libstdc++/libgcc deps.
    # Only applied when the static archives are actually installed (release
    # containers have them; a dev box may not — dynamic is fine there).
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_LINK_OPTIONS -static-libstdc++ -static-libgcc)
    check_cxx_source_compiles("#include <string>\nint main(){ return std::string(\"x\").size()==1?0:1; }"
        PARQIT_HAVE_STATIC_LIBSTDCXX)
    unset(CMAKE_REQUIRED_LINK_OPTIONS)
    if(PARQIT_HAVE_STATIC_LIBSTDCXX)
        target_link_options(parqit_plugin PRIVATE -static-libstdc++ -static-libgcc)
    else()
        message(WARNING "libstdc++-static not installed: parqit.plugin will link libstdc++ dynamically (fine for development; release builds use the AlmaLinux 8 container, which has it)")
    endif()
endif()

# ---------------------------------------------------------------------------
# Repo-local installable ado tree: ado/plus/p mirrors a user installation
# and is refreshed by every build — the plugin is (stripped and) copied
# whenever it relinks, the Stata-side files whenever they change. Point
# Stata at it once:   adopath ++ "<repo>/ado/plus/p"
# ---------------------------------------------------------------------------
set(PARQIT_LOCAL_ADO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ado/plus/p")
file(MAKE_DIRECTORY "${PARQIT_LOCAL_ADO_DIR}")

# Stata-side files: synced whenever they change; building parqit_plugin (or
# anything that depends on it) triggers the sync via add_dependencies.
set(_parqit_ado_synced)
foreach(f parqit.ado parqit.sthlp parqit.pkg parqit_read.dlg
          parqit_explore.dlg parqit_stats.dlg parqit_filter.dlg
          parqit_vars.dlg parqit_gen.dlg parqit_pivot.dlg
          parqit_combine.dlg parqit_write.dlg parqit_views.dlg stata.toc)
    add_custom_command(
        OUTPUT "${PARQIT_LOCAL_ADO_DIR}/${f}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/src/ado/p/${f}"
                "${PARQIT_LOCAL_ADO_DIR}/${f}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/ado/p/${f}"
        COMMENT "ado/plus/p <- ${f}")
    list(APPEND _parqit_ado_synced "${PARQIT_LOCAL_ADO_DIR}/${f}")
endforeach()
add_custom_target(parqit_ado_sync DEPENDS ${_parqit_ado_synced})
add_dependencies(parqit_plugin parqit_ado_sync)

# The binary: stripped into the tree after every relink (POST_BUILD), and
# also reconciled on plain `cmake --build build/dev` via the ALL target
# below (covers a fresh checkout where the plugin is up to date but the
# tree copy is missing).
if(APPLE)
    set(_parqit_plugin_install ${CMAKE_STRIP} -x -o "${PARQIT_LOCAL_ADO_DIR}/parqit.plugin"
        "$<TARGET_FILE:parqit_plugin>")
elseif(UNIX AND CMAKE_STRIP)
    set(_parqit_plugin_install ${CMAKE_STRIP} -o "${PARQIT_LOCAL_ADO_DIR}/parqit.plugin"
        "$<TARGET_FILE:parqit_plugin>")
else()
    set(_parqit_plugin_install ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:parqit_plugin>" "${PARQIT_LOCAL_ADO_DIR}/parqit.plugin")
endif()
add_custom_command(TARGET parqit_plugin POST_BUILD
    COMMAND ${_parqit_plugin_install}
    COMMENT "ado/plus/p <- parqit.plugin (stripped)")
add_custom_command(
    OUTPUT "${PARQIT_LOCAL_ADO_DIR}/parqit.plugin"
    COMMAND ${_parqit_plugin_install}
    DEPENDS parqit_plugin
    COMMENT "ado/plus/p <- parqit.plugin (stripped)")

add_custom_target(parqit_local_ado ALL
    DEPENDS ${_parqit_ado_synced} "${PARQIT_LOCAL_ADO_DIR}/parqit.plugin")

# ---------------------------------------------------------------------------
# Unit tests (doctest) — never shipped.
# ---------------------------------------------------------------------------
if(PARQIT_BUILD_TESTS)
    enable_testing()
    add_executable(parqit_tests
        tests/unit/test_main.cpp
        tests/unit/test_hexcodec.cpp
        tests/unit/test_session.cpp
        tests/unit/test_typemap.cpp
        tests/unit/test_sanitize.cpp
        tests/unit/test_request.cpp
        tests/unit/test_exprtrans.cpp
        tests/unit/test_view.cpp
        tests/unit/test_arrow_copy_bench.cpp
    )
    target_include_directories(parqit_tests PRIVATE
        src
        "${PARQIT_VENDOR_DIR}/doctest"
        "${PARQIT_DUCKDB_INCLUDE}"
        "${PARQIT_VENDOR_DIR}/arrow"
        "${PARQIT_VENDOR_DIR}/json"
    )
    target_link_libraries(parqit_tests PRIVATE parqit_engine Threads::Threads ${CMAKE_DL_LIBS})
    add_test(NAME unit COMMAND parqit_tests)
    if(UNIX)
        add_test(NAME runner_no_match
            COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_run_stata_no_match.sh")
        add_test(NAME unit_concurrent
            COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_unit_concurrent.sh"
                         "$<TARGET_FILE:parqit_tests>")
    endif()
endif()
