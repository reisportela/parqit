/* parqit — Parquet footer column rename (NAME-CASE-1).
 *
 * DuckDB identifiers are case-insensitive, so its COPY … TO Parquet renames
 * the second of two columns that differ only by case (`nuemp`, `NUEMP` →
 * `nuemp`, `NUEMP_1`, binder-side, bind_copy.cpp), while Stata — and Parquet
 * itself — keep such names distinct. parqit writes through DuckDB with
 * case-unique engine aliases and then restores the exact Stata names here:
 * the footer (Thrift compact protocol) is re-serialised with the i-th leaf
 * SchemaElement.name and every ColumnChunk's path_in_schema replaced
 * positionally; data pages are untouched. Only FLAT schemas (root + scalar
 * leaves — the only shape parqit writes) are supported; anything else is
 * refused. Before writing, the transformer must reproduce the original footer
 * byte-for-byte when asked to change nothing — an unknown structure aborts
 * loudly instead of risking the file. No Stata or DuckDB API here; unit-tested
 * in tests/unit/test_name_case.cpp with a DuckDB-written file and read back by
 * DuckDB and (in the Stata suite) pyarrow.
 */
#pragma once

#include <string>
#include <vector>

namespace parqit {

/* Leaf column names (schema elements without children, in file order) read
 * from the footer. "" on success, else an error message. */
std::string parquet_leaf_names(const std::string &path, std::vector<std::string> *names);

/* Rename the leaf columns of a flat Parquet file in place, positionally.
 * new_names.size() must equal the leaf count. A rename that changes nothing
 * leaves the file untouched. "" on success, else an error message (the file
 * is not modified on error). */
std::string parquet_rename_leaf_columns(const std::string &path,
                                        const std::vector<std::string> &new_names);

} // namespace parqit
