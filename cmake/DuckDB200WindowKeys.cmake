file(READ "${CMAKE_CURRENT_LIST_DIR}/duckdb200_window_keys.inc" parqit_window_keys)
parqit_duckdb200_source(
    "src/planner/binder/query_node/plan_select_node.cpp"
    e01d0efc2285dd961bf032af378a16bb0f71563017e70436f0ed0ea430a68f8d
    9f2039a884d66da11ef97b3a527cf39215cbda79f490662a46dbcca92716f1bc
    [=[#include "duckdb/planner/binder.hpp"]=]
    [=[#include "duckdb/planner/binder.hpp"
#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"]=]
    "namespace duckdb {\n" "namespace duckdb {\n${parqit_window_keys}"
    [=[	auto root = std::move(statement.from_table.plan);]=]
    [=[	auto root = std::move(statement.from_table.plan);
	optional_ptr<LogicalWindow> parqit_window;]=]
    [=[		auto win = make_uniq<LogicalWindow>(statement.window_index);]=]
    [=[		auto win = make_uniq<LogicalWindow>(statement.window_index);
		parqit_window = *win;]=]
    [=[	return root;
}

} // namespace duckdb]=]
    [=[	// Child SELECTs have already handled their own windows. Avoid revisiting
	// every descendant plan for queries without a window at this SELECT level.
	if (parqit_window) ParqitMaterialiseWindowKeys(*this, *root, *parqit_window);
	return root;
}

} // namespace duckdb]=])
unset(parqit_window_keys)
