# PARTITION-DELAY-1: DuckDB 2.0.0 routes every later batch of a partition with a
# delayed small buffer through the one-thread delayed writer. Date-clustered input
# then encodes almost all rows serially (500M rows: 48 s vs 5.4 s in 1.5.6).
# Without sorted runs or rotation, batches at the threshold use the parallel path.
# PARALLEL-PACK-1: the copy's single-key hash groups are packed by every
# materialize task instead of one, producing the same packed collection. Only
# the partitioned COPY enables it; window functions keep the upstream packing.
# Evidence: audit_repro/bigdata_500m_20261024/partition_engine_claude_01/.

parqit_duckdb200_source(
    "src/execution/operator/persistent/physical_copy_to_file.cpp"
    b80c038fd34131f4520fc6de265078425e11ffd7a15a8c78dbc95142dbee3b56
    e676b2f453fb0d0022fc899d0c5aacff160e370f48e63aeb9186f8a56e513d3b
    [=[	bool HasDelayedPartition(const vector<Value> &values);
	bool HasDelayedPartitions() const;]=]
    [=[	bool HasDelayedPartition(const vector<Value> &values);
	//! parqit PARTITION-DELAY-1: whether later input must join a partition's delayed buffer
	bool DelaysPartitionInput(const vector<Value> &values);
	bool HasDelayedPartitions() const;]=]
    [=[bool PartitionedCopy::HasDelayedPartition(const vector<Value> &values) {
	return delayed_partition_buffers.Has(values);
}]=]
    [=[bool PartitionedCopy::HasDelayedPartition(const vector<Value> &values) {
	return delayed_partition_buffers.Has(values);
}

bool PartitionedCopy::DelaysPartitionInput(const vector<Value> &values) {
	// parqit PARTITION-DELAY-1: without sorted runs or rotation, a batch at the flush threshold is
	// prepared in parallel beside an earlier delayed sliver instead of being encoded behind it.
	return RequiresSerializedPartitionWrites() && HasDelayedPartition(values);
}]=]
    [=[	const auto has_delayed_partition = partitioned_copy.HasDelayedPartition(values);]=]
    [=[	const auto has_delayed_partition = partitioned_copy.DelaysPartitionInput(values);]=]
    [=[		const auto has_delayed_partition = partitioned_copy.HasDelayedPartition(batch_state->Values());]=]
    [=[		const auto has_delayed_partition = partitioned_copy.DelaysPartitionInput(batch_state->Values());]=]
    [=[#include "duckdb/common/sorting/sort_strategy.hpp"]=]
    [=[#include "duckdb/common/sorting/sort_strategy.hpp"
#include "duckdb/common/sorting/hashed_sort.hpp"]=]
    [=[	return SortStrategy::Factory(context, partition_bys, op.order_columns, op.expected_types, partition_stats,
	                             op.children.empty() ? 0 : op.children[0].get().estimated_cardinality);
}]=]
    [=[	auto strategy = SortStrategy::Factory(context, partition_bys, op.order_columns, op.expected_types, partition_stats,
	                                      op.children.empty() ? 0 : op.children[0].get().estimated_cardinality);
	// parqit PARALLEL-PACK-1: only the partitioned COPY packs single-key groups with every materialize task
	auto hashed = dynamic_cast<HashedSort *>(strategy.get());
	if (hashed) {
		hashed->parallel_single_key_pack = true;
	}
	return std::move(strategy);
}]=])

parqit_duckdb200_source(
    "src/include/duckdb/common/sorting/hashed_sort.hpp"
    74d68c567f04669666f895dc9fdeb4def9575e0648d0744178fb9191bd1f4ccf
    ae5affee81d3fb5de26017de057fa38816a92ec817be4034f1427e17a1a8013c
    [=[	//! Whether single-key hash groups can skip sorting
	bool can_bypass_single_key_sort = false;]=]
    [=[	//! Whether single-key hash groups can skip sorting
	bool can_bypass_single_key_sort = false;
	//! parqit PARALLEL-PACK-1: every materialize task packs part of a single-key group (partitioned COPY)
	bool parallel_single_key_pack = false;]=])

parqit_duckdb200_source(
    "src/common/sort/hashed_sort.cpp"
    acc3bc3e4efbe9e797021a6953b45e8d27dec0ba7f9f9239caf364b9c871ab12
    205a3eff8b1687d5246f9e94251b0a4057c73ccdd4aa1f1636e0ec4a2d44bf92
    [=[	unique_ptr<ColumnDataCollection> direct_column_data;
	vector<unique_ptr<ColumnDataCollection>> direct_column_fragments;
};]=]
    [=[	unique_ptr<ColumnDataCollection> direct_column_data;
	vector<unique_ptr<ColumnDataCollection>> direct_column_fragments;
	//! parqit PARALLEL-PACK-1: fragment offsets and pieces of a parallel single-key pack
	vector<idx_t> direct_fragment_offsets;
	vector<unique_ptr<ColumnDataCollection>> direct_column_pieces;
	atomic<idx_t> direct_next_piece {0};
	atomic<idx_t> direct_pieces_done {0};
	bool direct_pack_planned = false;
};]=]
    [=[	D_ASSERT(result->ChunkCount() == ExpectedChunkCount(hash_group.count));
	hash_group.direct_column_data = std::move(result);
	fragments.clear();
}]=]
    [=[	D_ASSERT(result->ChunkCount() == ExpectedChunkCount(hash_group.count));
	hash_group.direct_column_data = std::move(result);
	fragments.clear();
}

// parqit PARALLEL-PACK-1: the same packed collection as PackDirectColumnData, built by every materialize
// task in fixed pieces of the fragments' concatenation and combined in order by the last finisher.
static void PackDirectColumnDataParallel(ClientContext &client, const vector<LogicalType> &payload_types,
                                         HashedSortGroup &hash_group) {
	static constexpr idx_t PIECE_ROWS = 64 * STANDARD_VECTOR_SIZE;
	idx_t piece_count;
	{
		lock_guard<mutex> direct_guard(hash_group.scan_lock);
		if (hash_group.direct_column_data || hash_group.direct_column_fragments.empty()) {
			return;
		}
		if (!hash_group.direct_pack_planned) {
			idx_t offset = 0;
			for (auto &fragment : hash_group.direct_column_fragments) {
				hash_group.direct_fragment_offsets.push_back(offset);
				offset += fragment->Count();
			}
			if (offset != hash_group.count) {
				throw InternalException("Single-key pack fragments do not cover their hash group");
			}
			hash_group.direct_column_pieces.resize((offset + PIECE_ROWS - 1) / PIECE_ROWS);
			hash_group.direct_pack_planned = true;
		}
		piece_count = hash_group.direct_column_pieces.size();
	}
	// Touch the fragments only after claiming a piece: the last finisher clears them.
	auto &fragments = hash_group.direct_column_fragments;
	auto &offsets = hash_group.direct_fragment_offsets;
	const idx_t total = hash_group.count;
	for (idx_t piece = hash_group.direct_next_piece++; piece < piece_count; piece = hash_group.direct_next_piece++) {
		const idx_t end = MinValue<idx_t>((piece + 1) * PIECE_ROWS, total);
		auto result =
		    make_uniq<ColumnDataCollection>(client, payload_types, ColumnDataAllocatorType::BUFFER_MANAGER_ALLOCATOR);
		ColumnDataAppendState append_state;
		result->InitializeAppend(append_state);
		idx_t row = piece * PIECE_ROWS;
		idx_t fragment_idx = 0;
		while (fragment_idx + 1 < offsets.size() && offsets[fragment_idx + 1] <= row) {
			++fragment_idx;
		}
		while (row < end) {
			auto &fragment = *fragments[fragment_idx];
			const idx_t base = offsets[fragment_idx];
			const idx_t local_end = MinValue<idx_t>(end, base + fragment.Count()) - base;
			idx_t local = row - base;
			ColumnDataScanState scan_state;
			fragment.InitializeScan(scan_state);
			DataChunk chunk;
			fragment.InitializeScanChunk(scan_state, chunk);
			if (!fragment.Seek(local, scan_state, chunk)) {
				throw InternalException("Single-key pack could not seek a fragment");
			}
			while (true) {
				const idx_t chunk_begin = scan_state.current_row_index;
				const idx_t slice_end = MinValue<idx_t>(chunk_begin + chunk.size(), local_end);
				chunk.Slice(local - chunk_begin, slice_end - local);
				result->Append(append_state, chunk);
				local = slice_end;
				if (local >= local_end || !fragment.Scan(scan_state, chunk)) {
					break;
				}
			}
			if (local != local_end) {
				throw InternalException("Single-key pack ended inside a fragment");
			}
			row = base + local;
			++fragment_idx;
		}
		hash_group.direct_column_pieces[piece] = std::move(result);
		if (++hash_group.direct_pieces_done != piece_count) {
			continue;
		}
		lock_guard<mutex> direct_guard(hash_group.scan_lock);
		auto &pieces = hash_group.direct_column_pieces;
		auto packed = std::move(pieces[0]);
		for (idx_t piece_idx = 1; piece_idx < pieces.size(); piece_idx++) {
			packed->Combine(*pieces[piece_idx]);
		}
		if (packed->Count() != hash_group.count ||
		    packed->ChunkCount() != (hash_group.count + STANDARD_VECTOR_SIZE - 1) / STANDARD_VECTOR_SIZE) {
			throw InternalException("Single-key pack did not produce a packed hash group");
		}
		hash_group.direct_column_data = std::move(packed);
		fragments.clear();
		offsets.clear();
		pieces.clear();
	}
}]=]
    [=[	if (!build_runs && gsink.CanBypassSort(hash_bin)) {
		PackDirectColumnData(hash_group);
		return SourceResultType::FINISHED;
	}]=]
    [=[	if (!build_runs && gsink.CanBypassSort(hash_bin)) {
		if (gsink.hashed_sort.parallel_single_key_pack) {
			PackDirectColumnDataParallel(gsink.client, gsink.hashed_sort.payload_types, hash_group);
		} else {
			PackDirectColumnData(hash_group);
		}
		return SourceResultType::FINISHED;
	}]=])

# The corrections rely on concurrent row-group preparation, the writer mutex that
# serialises row-group flushes to one file, and chunk-filling appends and segment
# moves of column data collections. Verify these sources unchanged.
parqit_duckdb200_source(
    "src/include/duckdb/execution/operator/persistent/physical_copy_to_file.hpp"
    e01bb3443615ce81ca91cf374d197ff298ead9ccadfdd90686995957d42ec750
    e01bb3443615ce81ca91cf374d197ff298ead9ccadfdd90686995957d42ec750)
parqit_duckdb200_source(
    "extension/parquet/parquet_writer.cpp"
    2a80a36bac7511a04109f56ab5258eccb864ffb139dfe42310759e5df451ac6b
    2a80a36bac7511a04109f56ab5258eccb864ffb139dfe42310759e5df451ac6b)
parqit_duckdb200_source(
    "src/common/types/column/column_data_collection.cpp"
    85168308125f4220da3db497951c775b105cd1054c44ad3bccf70430161aedc0
    85168308125f4220da3db497951c775b105cd1054c44ad3bccf70430161aedc0)
