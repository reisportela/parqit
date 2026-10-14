# The C aggregate update bridge receives the state vector a window aggregator
# passes. DuckDB's window_constant_aggregator.cpp keeps a CONSTANT pointer
# vector (statep) and rewrites its single slot per partition, holding its own
# pointer into that buffer; the bridge upstream reads the vector as flat
# (out of bounds past slot 0), and parqit 0.1.35's correction flattened it in
# place, which replaces the buffer under the aggregator's pointer (use after
# free: a crash, or rows of a later partition added to an earlier state,
# whenever window_self_join is off — WINDOW-NEST-1). The bridge now spreads
# the state pointers into a local array through UnifiedVectorFormat and
# never touches the vector.
set(_parqit_capi_file "${duckdb_SOURCE_DIR}/src/main/capi/aggregate_function-c.cpp")
include("${CMAKE_CURRENT_LIST_DIR}/ReadPinnedSource.cmake")
parqit_read_pinned_source("${_parqit_capi_file}" _parqit_capi_source _parqit_capi_hash)
set(_parqit_capi_original "c9cf979295e5203a0d4dc24771c485dbea4e641e6a08bd29093d7510dfbc7f14")
set(_parqit_capi_flatten "548c18c2bcf2b32eeba9da8cfe7c037788d10c76d5556ae114db06322b2c848d")
set(_parqit_capi_patched "d23313bad9ea72842b705c9b783a25ad896a41eafeaf74deaafd39fc332e60b6")
if(NOT _parqit_capi_hash STREQUAL _parqit_capi_patched)
    if(_parqit_capi_hash STREQUAL _parqit_capi_flatten)
        # the 0.1.35 in-place flatten: back to the pinned source first
        string(REPLACE [=[	chunk.SetCardinality(count);
	state.Flatten(count); // parqit: window aggregates may pass a constant state vector

	auto &bind_data]=] [=[	chunk.SetCardinality(count);

	auto &bind_data]=] _parqit_capi_source "${_parqit_capi_source}")
    elseif(NOT _parqit_capi_hash STREQUAL _parqit_capi_original)
        message(FATAL_ERROR "Unexpected DuckDB C aggregate bridge; preserving local changes")
    endif()
    string(REPLACE [=[	auto &bind_data = aggr_input_data.bind_data->Cast<CAggregateFunctionBindData>();
	auto state_data = FlatVector::GetDataUnsafe<duckdb_aggregate_state>(state);
	auto c_input_chunk = reinterpret_cast<duckdb_data_chunk>(&chunk);

	CAggregateExecuteInfo exec_info(bind_data.info);
	auto c_function_info = reinterpret_cast<duckdb_function_info>(&exec_info);
	bind_data.info.update(c_function_info, c_input_chunk, state_data);]=] [=[	// parqit: a window aggregator may pass a CONSTANT state vector and keep
	// its own pointer into it (window_constant_aggregator.cpp rewrites the
	// single slot per partition), so the vector is never flattened in place;
	// the state pointers are spread into a local array instead
	UnifiedVectorFormat state_format;
	state.ToUnifiedFormat(count, state_format);
	auto state_ptrs = UnifiedVectorFormat::GetData<data_ptr_t>(state_format);
	vector<duckdb_aggregate_state> state_data(count);
	for (idx_t i = 0; i < count; i++) {
		state_data[i] = reinterpret_cast<duckdb_aggregate_state>(state_ptrs[state_format.sel->get_index(i)]);
	}

	auto &bind_data = aggr_input_data.bind_data->Cast<CAggregateFunctionBindData>();
	auto c_input_chunk = reinterpret_cast<duckdb_data_chunk>(&chunk);

	CAggregateExecuteInfo exec_info(bind_data.info);
	auto c_function_info = reinterpret_cast<duckdb_function_info>(&exec_info);
	bind_data.info.update(c_function_info, c_input_chunk, state_data.data());]=] _parqit_capi_source "${_parqit_capi_source}")
    string(SHA256 _parqit_capi_patched_hash "${_parqit_capi_source}")
    if(NOT _parqit_capi_patched_hash STREQUAL _parqit_capi_patched)
        message(FATAL_ERROR "DuckDB C aggregate bridge patch did not match the corrected source (${_parqit_capi_patched_hash})")
    endif()
    if(EXISTS "${_parqit_capi_file}.parqit-patch")
        message(FATAL_ERROR "Existing DuckDB C API staging file preserved")
    endif()
    file(WRITE "${_parqit_capi_file}.parqit-patch" "${_parqit_capi_source}")
    parqit_verify_pinned_write("${_parqit_capi_file}.parqit-patch" "${_parqit_capi_patched_hash}")
    file(RENAME "${_parqit_capi_file}.parqit-patch" "${_parqit_capi_file}")
    parqit_read_pinned_source("${_parqit_capi_file}" _written _written_hash)
    if(NOT _written_hash STREQUAL _parqit_capi_patched_hash)
        message(FATAL_ERROR "DuckDB C aggregate source failed verification after writing")
    endif()
    message(STATUS "Applied parqit window-aggregate C API correction to pinned DuckDB")
endif()
unset(_parqit_capi_file)
unset(_parqit_capi_source)
unset(_parqit_capi_hash)
unset(_parqit_capi_original)
unset(_parqit_capi_flatten)
unset(_parqit_capi_patched)
unset(_parqit_capi_patched_hash)
