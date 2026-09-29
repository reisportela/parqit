/* parqit — the DuckDB table function that streams the rows of an R data
 * frame (R-READ-1).
 *
 *   parqit_read_rdata('<token>')
 *
 * scans the data frame of a conversion plan registered under <token>
 * (make_plan in rdata_plan.hpp), emitting exactly the columns and types the
 * plan fixed, so `COPY (SELECT * FROM parqit_read_rdata(...)) TO '<file>'
 * (FORMAT PARQUET, KV_METADATA {...})` writes the Parquet file out of core:
 * R stores a data frame column by column, so each column has its own cursor
 * and buffer, and memory is bounded by those buffers and one DuckDB vector,
 * never by the file. Registered once per session like the SPSS scan; the C
 * API only, and every failure inside a callback is reported through the C
 * API's error setters, never thrown across it.
 */
#pragma once

#include <memory>
#include <string>

#include "duckdb.h"

#include "engine/rdata_plan.hpp"

namespace parqit {
namespace rdata {

/* Registers parqit_read_rdata(VARCHAR) on the connection. */
bool register_table_function(duckdb_connection con, std::string *err);

/* A plan is scanned by token: the converter registers it, runs its COPY and
 * releases it (also on failure). */
std::string register_plan(std::shared_ptr<const Plan> plan);
void release_plan(const std::string &token);

} // namespace rdata
} // namespace parqit
