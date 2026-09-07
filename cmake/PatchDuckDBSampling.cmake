# Narrow, hash-guarded patch to the pinned DuckDB source. The archive stays pinned.
set(_parqit_sample_file "${duckdb_SOURCE_DIR}/src/execution/sample/reservoir_sample.cpp")
file(READ "${_parqit_sample_file}" _parqit_sample_source)
file(SHA256 "${_parqit_sample_file}" _parqit_sample_hash)
if(NOT _parqit_sample_hash STREQUAL "e502500aa94e0f1f89fba235fd02f69b32f4c6dfb335c4f882f15c34a3fb115b")
    if(NOT _parqit_sample_hash STREQUAL "6e09a723b908b6dc81b3ce203c83f51041176009edd42990bd7faa2831e616ea")
        message(FATAL_ERROR "Unexpected DuckDB sampler source; preserving local changes instead of patching it")
    endif()
    string(REPLACE
        [=[return static_cast<T>(sample_count +
	                      (FIXED_SAMPLE_SIZE_MULTIPLIER * MinValue<idx_t>(sample_count, FIXED_SAMPLE_SIZE)));]=]
        [=[// PARQIT_UNIFORM_RESERVOIR: one input vector may supply many replacements.
	const auto extra = FIXED_SAMPLE_SIZE_MULTIPLIER * MinValue<idx_t>(sample_count, FIXED_SAMPLE_SIZE);
	return static_cast<T>(sample_count + (stats_sample ? extra : MaxValue<idx_t>(extra, FIXED_SAMPLE_SIZE)));]=]
        _parqit_sample_source "${_parqit_sample_source}")
    string(REPLACE
        [=[base_reservoir_sample->next_index_to_sample - base_reservoir_sample->num_entries_to_skip_b4_next_sample;]=]
        [=[base_reservoir_sample->next_index_to_sample - base_reservoir_sample->num_entries_to_skip_b4_next_sample -
		    (stats_sample ? 0 : 1);]=]
        _parqit_sample_source "${_parqit_sample_source}")
    string(REPLACE
        [=[remaining -= offset;
		base_offset += offset;]=]
        [=[const idx_t consumed = offset + (stats_sample ? 0 : 1);
		remaining -= consumed;
		base_offset += consumed;]=]
        _parqit_sample_source "${_parqit_sample_source}")
    string(REPLACE
        [=[auto chunk_sel = GetReplacementIndexes(reservoir_chunk->chunk.size(), chunk.size());]=]
        [=[// SQL samples need uniform inclusion from the first replacement, including small k.
	if (!stats_sample && GetSamplingState() == SamplingState::RANDOM) {
		ConvertToReservoirSample();
	}
	auto chunk_sel = GetReplacementIndexes(reservoir_chunk->chunk.size(), chunk.size());]=]
        _parqit_sample_source "${_parqit_sample_source}")
    string(SHA256 _parqit_patched_hash "${_parqit_sample_source}")
    if(NOT _parqit_patched_hash STREQUAL "e502500aa94e0f1f89fba235fd02f69b32f4c6dfb335c4f882f15c34a3fb115b")
        message(FATAL_ERROR "DuckDB sampler patch did not match the expected corrected source")
    endif()
    file(WRITE "${_parqit_sample_file}.parqit-patch" "${_parqit_sample_source}")
    file(RENAME "${_parqit_sample_file}.parqit-patch" "${_parqit_sample_file}")
    message(STATUS "Applied parqit uniform-reservoir correction to pinned DuckDB")
endif()
unset(_parqit_sample_file)
unset(_parqit_sample_source)
unset(_parqit_sample_hash)
unset(_parqit_patched_hash)
