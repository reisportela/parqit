# Reviewed DuckDB 2.0.0 source pins. Each edit is reversible and hash-checked.
# The unchanged pins cover upstream replacements for the former local patches.

parqit_duckdb200_source(
    "src/execution/sample/reservoir_sample.cpp"
    0b6f30c3e61028ff050fe1cc6ad06270db2f17efb96f818dc66418bc67176166
    8d2567891982ce8e36c078ced3ae90cdc42dddeb7414524df40874bb7174f715
    [=[return static_cast<T>(sample_count +
	                      (FIXED_SAMPLE_SIZE_MULTIPLIER * MinValue<idx_t>(sample_count, FIXED_SAMPLE_SIZE)));]=]
    [=[// PARQIT_UNIFORM_RESERVOIR: one input vector may supply many replacements.
	const auto extra = FIXED_SAMPLE_SIZE_MULTIPLIER * MinValue<idx_t>(sample_count, FIXED_SAMPLE_SIZE);
	return static_cast<T>(sample_count + (stats_sample ? extra : MaxValue<idx_t>(extra, FIXED_SAMPLE_SIZE)));]=]
    [=[base_reservoir_sample->next_index_to_sample - base_reservoir_sample->num_entries_to_skip_b4_next_sample;]=]
    [=[base_reservoir_sample->next_index_to_sample - base_reservoir_sample->num_entries_to_skip_b4_next_sample -
		    (stats_sample ? 0 : 1);]=]
    [=[remaining -= offset;
		base_offset += offset;]=]
    [=[const idx_t consumed = offset + (stats_sample ? 0 : 1);
		remaining -= consumed;
		base_offset += consumed;]=]
    [=[auto chunk_sel = GetReplacementIndexes(reservoir_chunk->chunk.size(), chunk.size());]=]
    [=[// SQL samples need uniform inclusion from the first replacement, including small k.
	if (!stats_sample && GetSamplingState() == SamplingState::RANDOM) {
		ConvertToReservoirSample();
	}

	auto chunk_sel = GetReplacementIndexes(reservoir_chunk->chunk.size(), chunk.size());]=])

parqit_duckdb200_source(
    "src/storage/block_allocator.cpp"
    cb98fab1c981a39fd5d21cc9c343f105912bb50201ac6464e4ab1229f6e0de6b
    4e88e2f6670f159331ba06e3562651167b2cdb6fb09a10f18ed657d4d7f44f00
    [=[BlockAllocator::~BlockAllocator() {
	alive_token->store(false);
	GetBlockAllocatorThreadLocalState(*this).Clear();]=]
    [=[BlockAllocator::~BlockAllocator() {
	alive_token->store(false);]=])

parqit_duckdb200_source(
    "src/transaction/transaction_context.cpp"
    af813f514785c62317a12d4d5125d480ab93300c50a0088981862b71200b672a
    af813f514785c62317a12d4d5125d480ab93300c50a0088981862b71200b672a)

parqit_duckdb200_source(
    "src/main/capi/v1/aggregate_function-c.cpp"
    5a4517a2e39e7dc20db1f9c2d50047b11968f402abd1f385934f5a0cb42746dd
    5a4517a2e39e7dc20db1f9c2d50047b11968f402abd1f385934f5a0cb42746dd)

parqit_duckdb200_source(
    "src/main/capi/v1/scalar_function-c.cpp"
    817dd5cec0f936491d3db3076688a54f7a38b2d3ab0fdbe240e343d61680c26e
    817dd5cec0f936491d3db3076688a54f7a38b2d3ab0fdbe240e343d61680c26e)

parqit_duckdb200_source(
    "src/main/buffered_data/buffered_data.cpp"
    0b88e8bd300a1d29ecb7fc0a534990b0fb4af2040a728bac7260c629c7657ad5
    0b88e8bd300a1d29ecb7fc0a534990b0fb4af2040a728bac7260c629c7657ad5)

parqit_duckdb200_source(
    "src/optimizer/window_self_join.cpp"
    dcbc76ae2429faddda8d2634cbeccd5a174478c1e65903becc603bf741f81aae
    2e184d7285fe06ca0861f98933685e4eafc818fad5622eeef5bf5780c9fa6b98
    [=[	case LogicalOperatorType::LOGICAL_PROJECTION:
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY:]=]
    [=[	case LogicalOperatorType::LOGICAL_PROJECTION:]=])

parqit_duckdb200_source(
    "src/planner/expression/bound_function_expression.cpp"
    2c04b044880bc18d6e479e51732eae2250874c506f48fef892187db6cc3fccfa
    2c04b044880bc18d6e479e51732eae2250874c506f48fef892187db6cc3fccfa)

include("${CMAKE_CURRENT_LIST_DIR}/DuckDB200Corrections.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/DuckDB200Compatibility.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/DuckDB200WindowKeys.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/DuckDB200Hive.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/DuckDB200PageSelection.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/DuckDB200Prepared.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/DuckDB200PartitionedCopy.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/DuckDB200FileCache.cmake")

# The scalar bind proof reads borrowed logical-type handles through C API v1.
parqit_duckdb200_source(
    "src/main/capi/v1/logical_types-c.cpp"
    df4e01493f79fb97b5fabec32b1363163de368d541f9659bb0c94bee59b681da
    df4e01493f79fb97b5fabec32b1363163de368d541f9659bb0c94bee59b681da)

# Histogram totality is proved only for stateless built-in casts of literals.
# Pin the cast identity, evaluation and borrowed-vector contracts it uses.
parqit_duckdb200_source(
    "src/planner/expression/bound_cast_expression.cpp"
    7cd788c005a9d6754648a6ec0722aea40213cd4431d722b40465c44e4cf8eb00
    7cd788c005a9d6754648a6ec0722aea40213cd4431d722b40465c44e4cf8eb00)
parqit_duckdb200_source(
    "src/function/cast/cast_function_set.cpp"
    146f12ab8e15644e101b8b34fba63783818700a7b9a356842b097bfcc293700b
    146f12ab8e15644e101b8b34fba63783818700a7b9a356842b097bfcc293700b)
parqit_duckdb200_source(
    "src/execution/expression_executor.cpp"
    ef09458749ee8a0c75c509e3a63f621ee3725511ac7194b74a01a405d4a2ff7a
    ef09458749ee8a0c75c509e3a63f621ee3725511ac7194b74a01a405d4a2ff7a)
parqit_duckdb200_source(
    "src/main/capi/v1/data_chunk-c.cpp"
    a9398f5cdbabe2b3f91b79e2babbfbc377e84a2b6aa87a9a16a1530de34febff
    a9398f5cdbabe2b3f91b79e2babbfbc377e84a2b6aa87a9a16a1530de34febff)
parqit_duckdb200_source(
    "src/include/duckdb/function/cast/default_casts.hpp"
    8094261749a95c4c45e6dc17151370338bf9e1e9e0a83b56326711a35d984bb0
    8094261749a95c4c45e6dc17151370338bf9e1e9e0a83b56326711a35d984bb0)
parqit_duckdb200_source(
    "src/include/duckdb/planner/expression/bound_cast_expression.hpp"
    e2f8e80cf6e4b976ea8c83b2e4ba94167ff1b15ba54ab21c4ba4182ca933dd47
    e2f8e80cf6e4b976ea8c83b2e4ba94167ff1b15ba54ab21c4ba4182ca933dd47)
parqit_duckdb200_source(
    "src/include/duckdb/function/scalar_function.hpp"
    e4c44b31c3c853a13160da48d69c8209deec936fe0e4d14e94e31adf9e4fb7dd
    e4c44b31c3c853a13160da48d69c8209deec936fe0e4d14e94e31adf9e4fb7dd)
