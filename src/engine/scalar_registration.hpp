#pragma once

#include "duckdb.h"

namespace parqit {

// Only internal callbacks whose input contracts have been reviewed use these
// proofs. Value-dependent errors retain DuckDB's conservative error ordering.
enum class ScalarErrorPolicy { ValueDependent, Total, NumericUnary, NumericBinary, MatchingNumeric, HistogramBounds };

struct NumericTypeInfo {
    duckdb_type type = DUCKDB_TYPE_INVALID;
    unsigned scale = 0;
    bool floating = false, supported = false, null_type = false;
};

// Shared by the bound-type proof and the callback's actual vector reader.
// The logical type is borrowed; ownership stays with the caller.
inline NumericTypeInfo numeric_type_info(duckdb_logical_type logical) {
    NumericTypeInfo info;
    info.type = duckdb_get_type_id(logical);
    if (info.type == DUCKDB_TYPE_DECIMAL) {
        info.scale = duckdb_decimal_scale(logical);
        info.type = duckdb_decimal_internal_type(logical);
    }
    info.floating = info.type == DUCKDB_TYPE_FLOAT || info.type == DUCKDB_TYPE_DOUBLE;
    info.null_type = info.type == DUCKDB_TYPE_SQLNULL;
    switch (info.type) {
    case DUCKDB_TYPE_BOOLEAN:
    case DUCKDB_TYPE_TINYINT: case DUCKDB_TYPE_SMALLINT:
    case DUCKDB_TYPE_INTEGER: case DUCKDB_TYPE_BIGINT:
    case DUCKDB_TYPE_UTINYINT: case DUCKDB_TYPE_USMALLINT:
    case DUCKDB_TYPE_UINTEGER: case DUCKDB_TYPE_UBIGINT:
    case DUCKDB_TYPE_HUGEINT: case DUCKDB_TYPE_UHUGEINT:
    case DUCKDB_TYPE_FLOAT: case DUCKDB_TYPE_DOUBLE:
    case DUCKDB_TYPE_SQLNULL: info.supported = true; break;
    default: break;
    }
    return info;
}

inline bool numeric_families_match(const NumericTypeInfo &a, const NumericTypeInfo &b) {
    return a.supported && b.supported &&
        (a.null_type || b.null_type || (a.floating == b.floating && a.scale == b.scale));
}

duckdb_state register_known_scalar(duckdb_connection connection, duckdb_scalar_function function,
                                   ScalarErrorPolicy policy);

} // namespace parqit
