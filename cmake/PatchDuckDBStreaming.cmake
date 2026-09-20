# Preserve a worker's original failure when it also interrupts the stream.
set(_parqit_stream_file "${duckdb_SOURCE_DIR}/src/main/buffered_data/simple_buffered_data.cpp")
file(SHA256 "${_parqit_stream_file}" _parqit_stream_hash)
if(_parqit_stream_hash STREQUAL "dcf458a82a20c3b779fedd3395da9570f217c27ecbbfa2fb7588d6681df464fe")
    return()
endif()
if(NOT _parqit_stream_hash STREQUAL "2fdfc20c2cc277335f41f4f1f84cc62103e31660c6fee66304853a49da8241b3")
    message(FATAL_ERROR "Unexpected DuckDB streaming buffer source; preserving local changes")
endif()
file(READ "${_parqit_stream_file}" _parqit_stream_source)
string(REPLACE "#include \"duckdb/main/client_context.hpp\""
    "#include \"duckdb/main/client_context.hpp\"\n#include \"duckdb/execution/executor.hpp\""
    _parqit_stream_source "${_parqit_stream_source}")
string(REPLACE [=[	if (cc->interrupted.load(std::memory_order_relaxed)) {
		throw InterruptException();
	}]=] [=[	if (cc->interrupted.load(std::memory_order_relaxed)) {
		// Pair with the executor publishing its error before setting interrupted.
		std::atomic_thread_fence(std::memory_order_acquire);
		auto &executor = cc->GetExecutor();
		if (executor.HasError()) {
			executor.GetError().Throw();
		}
		throw InterruptException();
	}]=] _parqit_stream_source "${_parqit_stream_source}")
string(SHA256 _parqit_stream_patched_hash "${_parqit_stream_source}")
if(NOT _parqit_stream_patched_hash STREQUAL "dcf458a82a20c3b779fedd3395da9570f217c27ecbbfa2fb7588d6681df464fe")
    message(FATAL_ERROR "DuckDB streaming buffer patch did not match the corrected source")
endif()
if(EXISTS "${_parqit_stream_file}.parqit-streaming-patch")
    message(FATAL_ERROR "Existing DuckDB streaming staging file preserved")
endif()
file(WRITE "${_parqit_stream_file}.parqit-streaming-patch" "${_parqit_stream_source}")
file(RENAME "${_parqit_stream_file}.parqit-streaming-patch" "${_parqit_stream_file}")
message(STATUS "Applied parqit streaming worker-error correction to pinned DuckDB")
