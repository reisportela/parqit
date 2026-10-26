/* parqit — the DuckDB table function that streams the cases of an SPSS file
 * (SPSS-READ-1).
 *
 *   parqit_read_sav('<token>')
 *
 * scans the file of a conversion plan registered under <token> (make_plan in
 * spss_plan.hpp), emitting exactly the columns and types the plan fixed, so
 * `COPY (SELECT * FROM parqit_read_sav(...)) TO '<file>' (FORMAT PARQUET,
 * KV_METADATA {...})` writes the Parquet file out of core: memory is bounded
 * by one DuckDB vector, never by the file. Registered once per session like
 * the scalar helpers; the C API only, and every failure inside a callback is
 * reported through the C API's error setters, never thrown across it.
 */
#pragma once

#include <memory>
#include <string>

#include "duckdb.h"

#include "engine/spss_plan.hpp"

namespace parqit {
namespace spss {

/* Registers parqit_read_sav(VARCHAR) on the connection. */
bool register_table_function(duckdb_connection con, std::string *err);

/* A plan is scanned by token: the converter registers it, runs its COPY and
 * releases it (also on failure). */
std::string register_plan(std::shared_ptr<const Plan> plan);
void release_plan(const std::string &token);
/* plans registered now (the tests' check that none is left behind) */
size_t registered_plans();

/* One conversion's registration, released on every path out of its scope, an
 * exception between registering and the COPY included (A5). */
class PlanRegistration {
  public:
    explicit PlanRegistration(std::shared_ptr<const Plan> plan) : token_(register_plan(std::move(plan))) {}
    ~PlanRegistration() { release_plan(token_); }
    PlanRegistration(const PlanRegistration &) = delete;
    PlanRegistration &operator=(const PlanRegistration &) = delete;
    const std::string &token() const { return token_; }

  private:
    std::string token_;
};

} // namespace spss
} // namespace parqit
