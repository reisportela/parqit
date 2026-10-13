# Native scalar callbacks share the pinned C API's ScalarFunction handle.
file(SHA256 "${duckdb_SOURCE_DIR}/src/main/capi/scalar_function-c.cpp"
    _parqit_scalar_api_hash)
# 1.5.6 (reviewed 2026-10-13): the only change since 1.5.3 wraps
# duckdb_scalar_function_bind_get_argument in a try/catch that reports the
# error through duckdb_scalar_function_bind_set_error; the handle, the
# catalog copies, the bind/init-state hooks and the fallible execution are
# as before. The regenerated duckdb.h marks as deprecated functions parqit
# already used under 1.5.3 (duckdb_pending_prepared_streaming, the Arrow
# array scan and stream destroy, the duckdb_value_* readers, duckdb_row_count,
# duckdb_appender_error); they stay declared because the header enables
# DUCKDB_API_ALLOW_DEPRECATED when no API version is requested (the pinned
# static build), so this release keeps them and the migration is a separate
# task (ASSUMPTIONS #191).
if(NOT PARQIT_DUCKDB_VERSION STREQUAL "1.5.6" OR
   NOT _parqit_scalar_api_hash STREQUAL
       "dce59bfc27f1fc59ca3df6ee472ec6e44fba61daf9a91e7c4383a9c04286974a")
    message(FATAL_ERROR
        "DuckDB scalar API changed: review the native callback handle, catalog copies, "
        "bind/init-state hooks and fallible execution before updating this guard")
endif()
