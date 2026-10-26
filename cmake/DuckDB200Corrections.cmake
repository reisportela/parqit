# Narrow compatibility/correctness corrections reviewed against final 2.0.0.
# Each transformation is guarded by original and corrected source hashes.

parqit_duckdb200_source(
    "extension/parquet/parquet_statistics.cpp"
    574c2d1b324038ef147a23ed6cadfa40f016a4c9586aa20882265c7560be2975
    5ad5f07954c58e10826316d3cd9f7950c0f420095d18c757b49c7f8a75c10eba
    [=[			StringStats::FromConstant(constant_stats, string_t(min_stats));
			return constant_stats.ToUnique();]=]
    [=[			StringStats::FromConstant(constant_stats, string_t(min_stats));
			// Page pruning emits NULL placeholders, so predicates accepting NULL
			// must not prune these pages. Row-group validity is refined by its caller.
			constant_stats.Set(StatsInfo::CAN_HAVE_NULL_AND_VALID_VALUES);
			return constant_stats.ToUnique();]=])

parqit_duckdb200_source(
    "src/function/table/read_csv.cpp"
    8a7bcac1da1b1492aa13007a48422b6b7f829495e993757e2214ae9c929f1380
    450fde953a392364ac6255706e7bd49d100c40de7477c7935ddf84f57fa64067
    [=[	bind_data.types[idx] = target_type;
	bind_data.columns[idx].type = target_type;]=]
    [=[	// parqit: parsing raw text as the target type is not equivalent to
	// casting the detected value (TRUE -> TINYINT, 1e5 -> VARCHAR, etc.).
	// Keep the conversion after the CSV reader, as in 1.5.6.
	if (bind_data.types[idx] != target_type) {
		return false;
	}
	bind_data.types[idx] = target_type;
	bind_data.columns[idx].type = target_type;]=])

parqit_duckdb200_source(
    "extension/parquet/parquet_reader.cpp"
    e30594644af996e0a4d94ea75c4a9b9784030ad032c9eb4f84932bf3635261ed
    fa54d30962ab503fdb526def3ed7230c6b9d0773adfabdc073cd3f1dee67b77f
    [=[		if (s_ele.logicalType.__isset.UNKNOWN) {
			return LogicalType::SQLNULL;
		} else if (s_ele.logicalType.__isset.UUID) {]=]
    [=[		// parqit: UNKNOWN annotates an always-null column whose physical type
		// was guessed. Retain that physical type, as in 1.5.6; a typeless SQL
		// expression is still SQLNULL and remains a separate boundary policy.
		if (s_ele.logicalType.__isset.UUID) {]=])

parqit_duckdb200_source(
    "extension/parquet/parquet_extension.cpp"
    668ecfeb7f37de312f1399c7b4dff672262ac73910f4177e39a1a6b5350d1288
    1618d13ac17ec617f7ccdefdb69e86d5d67b27bb1e9a3bfb2823bf2c2c7dd9a6
    [=[	auto &parquet_bind = input.bind_data.Cast<ParquetWriteBindData>();
	auto count = parquet_bind.sql_types.size();]=]
    [=[	auto &parquet_bind = input.bind_data.Cast<ParquetWriteBindData>();
	auto count = parquet_bind.sql_types.size();
	// Stata storage types admit missing values. Keep the published optional
	// Parquet schema independent of the selected parqit writer and its data.
	for (const auto &entry : parquet_bind.kv_metadata) {
		if (entry.first == "parqit.schema") {
			parquet_bind.not_null_columns.assign(count, false);
			return;
		}
	}]=])

parqit_duckdb200_source(
    "extension/core_functions/include/core_functions/aggregate/algebraic/stddev.hpp"
    2fdd6f1081b0f352232a376afc7445766c30e8e02f3d4bb906e533ee7573f8eb
    5b3b634d8cc9f353996cc98d70bb80d98f175647fb55670ddeec0954c19908af
    [=[			target = state.dsquared / (state.count - 1);]=]
    [=[			target = state.dsquared / (state.count - 1);
			if (!Value::DoubleIsFinite(target)) {
				throw OutOfRangeException("VARSAMP is out of range!");
			}]=]
    [=[			target = state.count > 1 ? (state.dsquared / state.count) : 0;]=]
    [=[			target = state.count > 1 ? (state.dsquared / state.count) : 0;
			if (!Value::DoubleIsFinite(target)) {
				throw OutOfRangeException("VARPOP is out of range!");
			}]=]
    [=[			target = sqrt(state.dsquared / (state.count - 1));]=]
    [=[			target = sqrt(state.dsquared / (state.count - 1));
			if (!Value::DoubleIsFinite(target)) {
				throw OutOfRangeException("STDDEV_SAMP is out of range!");
			}]=]
    [=[			target = state.count > 1 ? sqrt(state.dsquared / state.count) : 0;]=]
    [=[			target = state.count > 1 ? sqrt(state.dsquared / state.count) : 0;
			if (!Value::DoubleIsFinite(target)) {
				throw OutOfRangeException("STDDEV_POP is out of range!");
			}]=]
    [=[			target = sqrt(state.dsquared / state.count) / sqrt((state.count));]=]
    [=[			target = sqrt(state.dsquared / state.count) / sqrt((state.count));
			if (!Value::DoubleIsFinite(target)) {
				throw OutOfRangeException("SEM is out of range!");
			}]=])

parqit_duckdb200_source(
    "extension/core_functions/include/core_functions/aggregate/algebraic/corr.hpp"
    7098fdb75de7a4f9bb8e42761a34e5a7f141f6735640e8d8d1798ebce5600c9c
    63d6a811d52934148a1331f16945f8a59cc8bcf6c50411b9215c2b6020a65b06
    [=[			auto std_x = state.dev_pop_x.count > 1 ? sqrt(state.dev_pop_x.dsquared / state.dev_pop_x.count) : 0;]=]
    [=[			auto std_x = state.dev_pop_x.count > 1 ? sqrt(state.dev_pop_x.dsquared / state.dev_pop_x.count) : 0;
			if (!Value::DoubleIsFinite(std_x)) {
				throw OutOfRangeException("STDDEV_POP for X is out of range!");
			}]=]
    [=[			auto std_y = state.dev_pop_y.count > 1 ? sqrt(state.dev_pop_y.dsquared / state.dev_pop_y.count) : 0;]=]
    [=[			auto std_y = state.dev_pop_y.count > 1 ? sqrt(state.dev_pop_y.dsquared / state.dev_pop_y.count) : 0;
			if (!Value::DoubleIsFinite(std_y)) {
				throw OutOfRangeException("STDDEV_POP for Y is out of range!");
			}]=])

parqit_duckdb200_source(
    "src/optimizer/statistics/operator/propagate_join.cpp"
    5fe1bd79e9b66f4e89163fe06edd7bc8bb1a007187f219eb80c8621fd4d3ad26
    faecfe50011101f2d78443f5389703d207e16cf7c9de255626996da80df73579
    [=[	// first propagate through the children of the join
	node_stats = PropagateStatistics(join.children[0]);
	for (idx_t child_idx = 1; child_idx < join.children.size(); child_idx++) {
		auto child_stats = PropagateStatistics(join.children[child_idx]);
		if (!child_stats) {
			node_stats = nullptr;
		} else if (node_stats) {
			MultiplyCardinalities(node_stats, *child_stats);
		}
	}
]=]
    [=[	// Keep left statistics local: recursion into the right child changes node_stats.
	auto join_stats = PropagateStatistics(join.children[0]);
	for (idx_t child_idx = 1; child_idx < join.children.size(); child_idx++) {
		auto child_stats = PropagateStatistics(join.children[child_idx]);
		if (!child_stats) {
			join_stats = nullptr;
		} else if (join_stats) {
			if (!join_stats->has_estimated_cardinality || !child_stats->has_estimated_cardinality ||
			    !join_stats->has_max_cardinality || !child_stats->has_max_cardinality) {
				join_stats = nullptr;
				continue;
			}
			// Bound output rows by join semantics before aggregate range inference.
			// A preserved side cannot disappear merely because its partner is empty.
			switch (join.join_type) {
			case JoinType::SEMI:
			case JoinType::ANTI:
			case JoinType::MARK:
			case JoinType::SINGLE:
				break;
			case JoinType::RIGHT_SEMI:
			case JoinType::RIGHT_ANTI:
				join_stats = std::move(child_stats);
				break;
			case JoinType::LEFT:
				child_stats->max_cardinality = MaxValue<idx_t>(child_stats->max_cardinality, 1);
				MultiplyCardinalities(join_stats, *child_stats);
				break;
			case JoinType::RIGHT:
				join_stats->max_cardinality = MaxValue<idx_t>(join_stats->max_cardinality, 1);
				MultiplyCardinalities(join_stats, *child_stats);
				break;
			case JoinType::OUTER: {
				const auto left = join_stats->max_cardinality;
				const auto right = child_stats->max_cardinality;
				const auto limit = NumericCast<idx_t>(NumericLimits<int64_t>::Maximum());
				if (left > limit || right > limit || left > limit - right) {
					join_stats = nullptr;
					break;
				}
				MultiplyCardinalities(join_stats, *child_stats);
				if (join_stats) join_stats->max_cardinality = MaxValue<idx_t>(join_stats->max_cardinality, left + right);
				break;
			}
			default:
				MultiplyCardinalities(join_stats, *child_stats);
				break;
			}
		}
	}
	node_stats = std::move(join_stats);
]=]
    [=[	// first propagate through the children of the join
	node_stats = PropagateStatistics(join.children[0]);
	for (idx_t child_idx = 1; child_idx < join.children.size(); child_idx++) {
		auto child_stats = PropagateStatistics(join.children[child_idx]);
		if (!child_stats) {
			node_stats = nullptr;
		} else if (node_stats) {
			if (!node_stats->has_estimated_cardinality || !child_stats->has_estimated_cardinality ||
			    !node_stats->has_max_cardinality || !child_stats->has_max_cardinality) {
				node_stats = nullptr;
			} else {
				MaxCardinalities(node_stats, *child_stats);
			}
		}
	}
]=]
    [=[	// Keep left statistics local: recursion into the right child changes node_stats.
	auto join_stats = PropagateStatistics(join.children[0]);
	for (idx_t child_idx = 1; child_idx < join.children.size(); child_idx++) {
		auto child_stats = PropagateStatistics(join.children[child_idx]);
		if (!child_stats) {
			join_stats = nullptr;
		} else if (join_stats) {
			if (!join_stats->has_estimated_cardinality || !child_stats->has_estimated_cardinality ||
			    !join_stats->has_max_cardinality || !child_stats->has_max_cardinality) {
				join_stats = nullptr;
			} else {
				MaxCardinalities(join_stats, *child_stats);
			}
		}
	}
	node_stats = std::move(join_stats);
]=])
