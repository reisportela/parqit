# A window-to-self-join rewrite must not duplicate nondeterministic inputs.
set(_parqit_window_file "${duckdb_SOURCE_DIR}/src/optimizer/window_self_join.cpp")
include("${CMAKE_CURRENT_LIST_DIR}/ReadPinnedSource.cmake")
parqit_read_pinned_source("${_parqit_window_file}" _parqit_window_source _parqit_window_hash)
if(_parqit_window_hash STREQUAL "9666fa348eec85dc76341819a5e599d0e1d1c4b3128416ccf85553d7c96ef104")
    return()
endif()
if(NOT _parqit_window_hash STREQUAL "e425667854d29c6337626f949e7f4941c5289ea9dc4b4bef92e229b2a662f6a4"
   AND NOT _parqit_window_hash STREQUAL "797752375813fc3d96b2a4c0130e80fe3856cfc1a31ff13ac273fb5d7f41210c")
    message(FATAL_ERROR "Unexpected DuckDB window self-join source; preserving local changes")
endif()
if(_parqit_window_hash STREQUAL "e425667854d29c6337626f949e7f4941c5289ea9dc4b4bef92e229b2a662f6a4")
string(REPLACE "#include \"duckdb/planner/logical_operator_deep_copy.hpp\""
    "#include \"duckdb/planner/logical_operator_deep_copy.hpp\"\n#include \"duckdb/planner/logical_operator_visitor.hpp\""
    _parqit_window_source "${_parqit_window_source}")
string(REPLACE "namespace duckdb {" [=[namespace duckdb {

// PARQIT-WINDOW-VOLATILE-1: the rewrite copies the input for a self-join.
// A volatile expression must see the same rows and values on both sides.
static bool ParqitHasVolatileWindowInput(LogicalOperator &op) {
	bool found = false;
	LogicalOperatorVisitor::EnumerateExpressions(op, [&](unique_ptr<Expression> *expr) {
		found = found || (*expr)->IsVolatile();
	});
	if (found) {
		return true;
	}
	for (auto &child : op.children) {
		if (ParqitHasVolatileWindowInput(*child)) {
			return true;
		}
	}
	return false;
}]=] _parqit_window_source "${_parqit_window_source}")
string(REPLACE "if (!CanOptimize(*window.children[0])) {"
    "if (!CanOptimize(*window.children[0]) || ParqitHasVolatileWindowInput(*window.children[0])) {"
    _parqit_window_source "${_parqit_window_source}")
string(REPLACE "if (!CanOptimize(w_expr, w_expr0)) {"
    "if (w_expr.IsVolatile() || !CanOptimize(w_expr, w_expr0)) {"
    _parqit_window_source "${_parqit_window_source}")
endif()
# first/any_value and floating aggregates can depend on evaluation order
# without being marked volatile. An aggregate child must be evaluated once.
string(REPLACE "\n\tcase LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY:" ""
    _parqit_window_source "${_parqit_window_source}")
string(SHA256 _parqit_window_patched_hash "${_parqit_window_source}")
if(NOT _parqit_window_patched_hash STREQUAL "9666fa348eec85dc76341819a5e599d0e1d1c4b3128416ccf85553d7c96ef104")
    message(FATAL_ERROR "DuckDB window self-join patch did not match the corrected source")
endif()
if(EXISTS "${_parqit_window_file}.parqit-window-patch")
    message(FATAL_ERROR "Existing DuckDB window staging file preserved: ${_parqit_window_file}.parqit-window-patch")
endif()
file(WRITE "${_parqit_window_file}.parqit-window-patch" "${_parqit_window_source}")
parqit_verify_pinned_write("${_parqit_window_file}.parqit-window-patch" "${_parqit_window_patched_hash}")
file(RENAME "${_parqit_window_file}.parqit-window-patch" "${_parqit_window_file}")
parqit_read_pinned_source("${_parqit_window_file}" _parqit_window_written _parqit_window_written_hash)
if(NOT _parqit_window_written_hash STREQUAL _parqit_window_patched_hash)
    message(FATAL_ERROR "DuckDB window self-join source failed verification after writing")
endif()
message(STATUS "Applied parqit nondeterministic window-input correction to pinned DuckDB")
