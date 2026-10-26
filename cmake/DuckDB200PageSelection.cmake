# Keep skipped rows out of NULL-accepting predicates, across page boundaries.

parqit_duckdb200_source(
    "extension/parquet/include/column_reader.hpp"
    118ec48e54acdae9075d2683fe3ef8ed5ae405aa6eb909142d0a0ad2687a9831
    3bb118bd50de3d12c1354d85d1ec8cf56a0be5ca2b52e688a150678d6e0525ce
    [=[	idx_t ReadInternal(ColumnReaderInput &input, Vector &result);]=]
    [=[	idx_t ReadInternal(ColumnReaderInput &input, Vector &result);
	void ExcludePrunedPageRows(SelectionVector &sel, idx_t &count) const;
	ValidityMask unpruned_page_rows;]=])

parqit_duckdb200_source(
    "extension/parquet/column_reader.cpp"
    8d92990a182311fee5fef906f4df79eafcf47de73c86064d9a70949a954ab62a
    7b2d1fed4805aaa7d1c20923be2d9021e1a91b1bef1c7535592e23f08bc73990
    [=[			validity.SetInvalid(result_offset + i);]=]
    [=[			validity.SetInvalid(result_offset + i);
			unpruned_page_rows.SetInvalid(result_offset + i);]=]
    [=[void ColumnReader::Filter(ColumnReaderInput &input, Vector &result, const TableFilter &filter,]=]
    [=[// A pruned page is absent, not a page of SQL NULLs. Preserve that distinction
// even when a filter accepts NULL and a data vector straddles several pages.
void ColumnReader::ExcludePrunedPageRows(SelectionVector &sel, idx_t &count) const {
	if (unpruned_page_rows.CannotHaveNull()) return;
	// The input may be an implicit identity or shared selection storage.
	SelectionVector kept_sel(count);
	idx_t kept = 0;
	for (idx_t i = 0; i < count; ++i) {
		auto row = sel.get_index(i);
		if (unpruned_page_rows.RowIsValid(row)) kept_sel.set_index(kept++, row);
	}
	sel.Initialize(kept_sel);
	count = kept;
}

void ColumnReader::Filter(ColumnReaderInput &input, Vector &result, const TableFilter &filter,]=]
    [=[	if (SupportsDirectFilter() && is_first_filter && !(IsRoot() && AllValuesAreNull())) {]=]
    [=[	unpruned_page_rows.Reset();
	if (SupportsDirectFilter() && is_first_filter && !(IsRoot() && AllValuesAreNull())) {]=]
    [=[	Select(input, result, sel, approved_tuple_count);
	ApplyFilter(result, filter, filter_state, num_values, sel, approved_tuple_count);]=]
    [=[	Select(input, result, sel, approved_tuple_count);
	ExcludePrunedPageRows(sel, approved_tuple_count);
	ApplyFilter(result, filter, filter_state, num_values, sel, approved_tuple_count);]=]
    [=[	ReadInternal(input, result);
	ApplyFilter(result, filter, filter_state, num_values, sel, approved_tuple_count);]=]
    [=[	ReadInternal(input, result);
	ExcludePrunedPageRows(sel, approved_tuple_count);
	ApplyFilter(result, filter, filter_state, num_values, sel, approved_tuple_count);]=])
