# Bound lambda bodies are stored in bind data rather than expression children.
set(_parqit_lambda_file "${duckdb_SOURCE_DIR}/src/planner/expression/bound_function_expression.cpp")
include("${CMAKE_CURRENT_LIST_DIR}/ReadPinnedSource.cmake")
parqit_read_pinned_source("${_parqit_lambda_file}" _parqit_lambda_source _parqit_lambda_hash)
if(_parqit_lambda_hash STREQUAL "6cace450998dd5deae9de93e722004f354b3577fc53aae2ee6e842a86f55fd49")
    return()
endif()
if(NOT _parqit_lambda_hash STREQUAL "e4d2ed917ba3d114008f12f3fa10895b3c4957c98405bd4084adccfbca9b2a89")
    message(FATAL_ERROR "Unexpected DuckDB bound-function source; preserving local changes")
endif()
string(REPLACE "bool BoundFunctionExpression::IsVolatile() const {\n"
    [=[bool BoundFunctionExpression::IsVolatile() const {
	// PARQIT-LAMBDA-VOLATILE-1: lambda bodies live in bind data, not children.
	// Mirror IsFoldable's inspection so rewrites cannot duplicate their effects.
	if (function.HasBindLambdaCallback() && bind_info) {
		auto &lambda_bind_data = bind_info->Cast<ListLambdaBindData>();
		if (lambda_bind_data.lambda_expr && lambda_bind_data.lambda_expr->IsVolatile()) {
			return true;
		}
	}
]=] _parqit_lambda_source "${_parqit_lambda_source}")
string(SHA256 _parqit_lambda_patched_hash "${_parqit_lambda_source}")
if(NOT _parqit_lambda_patched_hash STREQUAL "6cace450998dd5deae9de93e722004f354b3577fc53aae2ee6e842a86f55fd49")
    message(FATAL_ERROR "DuckDB lambda volatility patch did not match the corrected source")
endif()
if(EXISTS "${_parqit_lambda_file}.parqit-lambda-patch")
    message(FATAL_ERROR "Existing DuckDB lambda staging file preserved: ${_parqit_lambda_file}.parqit-lambda-patch")
endif()
file(WRITE "${_parqit_lambda_file}.parqit-lambda-patch" "${_parqit_lambda_source}")
parqit_verify_pinned_write("${_parqit_lambda_file}.parqit-lambda-patch" "${_parqit_lambda_patched_hash}")
file(RENAME "${_parqit_lambda_file}.parqit-lambda-patch" "${_parqit_lambda_file}")
parqit_read_pinned_source("${_parqit_lambda_file}" _parqit_lambda_written _parqit_lambda_written_hash)
if(NOT _parqit_lambda_written_hash STREQUAL _parqit_lambda_patched_hash)
    message(FATAL_ERROR "DuckDB lambda source failed verification after writing")
endif()
message(STATUS "Applied parqit lambda-volatility correction to pinned DuckDB")
