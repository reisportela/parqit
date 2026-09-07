# The C aggregate update bridge must flatten state pointers as well as inputs.
set(_parqit_capi_file "${duckdb_SOURCE_DIR}/src/main/capi/aggregate_function-c.cpp")
file(READ "${_parqit_capi_file}" _parqit_capi_source)
file(SHA256 "${_parqit_capi_file}" _parqit_capi_hash)
if(NOT _parqit_capi_hash STREQUAL "548c18c2bcf2b32eeba9da8cfe7c037788d10c76d5556ae114db06322b2c848d")
    if(NOT _parqit_capi_hash STREQUAL "c9cf979295e5203a0d4dc24771c485dbea4e641e6a08bd29093d7510dfbc7f14")
        message(FATAL_ERROR "Unexpected DuckDB C aggregate bridge; preserving local changes")
    endif()
    string(REPLACE [=[	chunk.SetCardinality(count);

	auto &bind_data]=] [=[	chunk.SetCardinality(count);
	state.Flatten(count); // parqit: window aggregates may pass a constant state vector

	auto &bind_data]=] _parqit_capi_source "${_parqit_capi_source}")
    string(SHA256 _parqit_capi_patched_hash "${_parqit_capi_source}")
    if(NOT _parqit_capi_patched_hash STREQUAL "548c18c2bcf2b32eeba9da8cfe7c037788d10c76d5556ae114db06322b2c848d")
        message(FATAL_ERROR "DuckDB C aggregate bridge patch did not match the corrected source")
    endif()
    file(WRITE "${_parqit_capi_file}.parqit-patch" "${_parqit_capi_source}")
    file(RENAME "${_parqit_capi_file}.parqit-patch" "${_parqit_capi_file}")
    message(STATUS "Applied parqit window-aggregate C API correction to pinned DuckDB")
endif()
unset(_parqit_capi_file)
unset(_parqit_capi_source)
unset(_parqit_capi_hash)
unset(_parqit_capi_patched_hash)
