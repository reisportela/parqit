# Preserve the underlying plan ordering when 2.0 executes a prepared statement.
# Source evidence and the no-Stata reproducer are recorded in the 2026-10-23 study.

parqit_duckdb200_source(
    "src/execution/operator/helper/physical_result_collector.cpp"
    02e198c7bad63e4b0dbd20d93ddd27a8fe42d49bc4f98fd0d22c2b1e7f713772
    abab01843d0422999b54dd9c6d1e200388143cfeff22c5a56c3416864d60d13d
    [=[#include "duckdb/execution/operator/helper/physical_result_sink.hpp"]=]
    [=[#include "duckdb/execution/operator/helper/physical_result_sink.hpp"
#include "duckdb/execution/operator/helper/physical_execute.hpp"]=]
    [=[	auto &root = physical_plan.Root();]=]
    [=[	// PREPARED-ORDER: EXECUTE forwards to a referenced plan, outside children.
	// Analyse that plan, but keep the original root and its keepalive in the sink.
	auto *root = &physical_plan.Root();
	while (root->type == PhysicalOperatorType::EXECUTE) {
		D_ASSERT(!root->IsSource() && !root->IsSink() && root->children.empty());
		root = &root->Cast<PhysicalExecute>().plan;
	}]=]
    [=[PreserveInsertionOrder(context, root)]=]
    [=[PreserveInsertionOrder(context, *root)]=]
    [=[UseBatchIndex(context, root)]=]
    [=[UseBatchIndex(context, *root)]=])

# The proof depends on a transparent EXECUTE wrapper and the default collector.

parqit_duckdb200_source(
    "src/execution/operator/helper/physical_execute.cpp"
    ade31351abba46e22778cf382afeb28f43980dd0de39d8e64e498d4ff1f3830d
    ade31351abba46e22778cf382afeb28f43980dd0de39d8e64e498d4ff1f3830d)

parqit_duckdb200_source(
    "src/include/duckdb/execution/operator/helper/physical_execute.hpp"
    73459f362e5aaa49c8d9365153060a4fa96ef859e7859dda303c1a5a58ac6ddb
    73459f362e5aaa49c8d9365153060a4fa96ef859e7859dda303c1a5a58ac6ddb)

parqit_duckdb200_source(
    "src/execution/physical_operator.cpp"
    262b0ae61698a73b9956bc9b0fa3e5a222f6ced4a554ce92f15eeca57e8081af
    262b0ae61698a73b9956bc9b0fa3e5a222f6ced4a554ce92f15eeca57e8081af)

parqit_duckdb200_source(
    "src/include/duckdb/execution/physical_operator.hpp"
    edfeb7ffbcf6c26c3f12676297a8598bc157a762733493d73deaa91b62eadb83
    edfeb7ffbcf6c26c3f12676297a8598bc157a762733493d73deaa91b62eadb83)

parqit_duckdb200_source(
    "src/execution/physical_plan/plan_insert.cpp"
    7ca8ace63a6ab0572226f4010130cd24c77afbbc8125d7340b2fe72375ab9b2a
    7ca8ace63a6ab0572226f4010130cd24c77afbbc8125d7340b2fe72375ab9b2a)

parqit_duckdb200_source(
    "src/execution/operator/helper/physical_result_sink.cpp"
    e2dfdd30c295030cbdb06fb5b73f57cf2e55653af54af7a981263ea27ee2966f
    e2dfdd30c295030cbdb06fb5b73f57cf2e55653af54af7a981263ea27ee2966f)

parqit_duckdb200_source(
    "src/main/client_context_execution.cpp"
    63c5df12a8ecf3a543635e00a15bbab7462cd24f9ca85ab82d1bceb041a98610
    63c5df12a8ecf3a543635e00a15bbab7462cd24f9ca85ab82d1bceb041a98610)

parqit_duckdb200_source(
    "src/include/duckdb/common/enums/result_ordering.hpp"
    e3f37cfbbda3508717487a01c0f371822555daea8fa1a99a6acb32374c535bb3
    e3f37cfbbda3508717487a01c0f371822555daea8fa1a99a6acb32374c535bb3)
