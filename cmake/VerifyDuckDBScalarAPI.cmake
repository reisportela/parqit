# Native scalar callbacks share the pinned C API's ScalarFunction handle.
file(SHA256 "${duckdb_SOURCE_DIR}/src/main/capi/scalar_function-c.cpp"
    _parqit_scalar_api_hash)
if(NOT PARQIT_DUCKDB_VERSION STREQUAL "1.5.3" OR
   NOT _parqit_scalar_api_hash STREQUAL
       "273bc843f366589a533da86ec338302aee72278cb924d349a19048c7cc2a6563")
    message(FATAL_ERROR
        "DuckDB scalar API changed: review the native callback handle, catalog copies, "
        "bind/init-state hooks and fallible execution before updating this guard")
endif()
