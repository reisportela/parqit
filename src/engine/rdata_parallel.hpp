/* parqit — the R data conversion's batches on several threads
 * (RDATA-PARALLEL-1).
 *
 * The columns of a data frame lie apart in the (inflated) file, each read by
 * its own cursor, so workers convert different columns of the same rows: a
 * worker owns columns i with i % workers == w and fills them, batch after
 * batch, into one Arrow record batch per block of rows, with the table
 * function's code (convert_rows). A batch is handed on once every worker has
 * filled it, in row order, through a single-pass ArrowArrayStream, which the
 * engine's Arrow scan writes on its own threads and in order. The first error
 * is the one the table function meets first (its 2048-row vector, then the
 * column, then the row), with its wording.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "abi.h"
#include "engine/rdata_plan.hpp"

namespace parqit {
namespace rdata {

/* Rows per batch for rows of `row_bytes` (as the profile measured them): one
 * row group of the engine's default (122880), fewer for a wide frame so a
 * batch stays within 64 MB (whole vectors of 2048 down to 2048 rows, then any
 * number down to one row: A2). */
int64_t batch_rows_for(int64_t row_bytes);

class RdataArrowSource {
  public:
    /* batch_rows 0 = one row group (122880), fewer for a wide frame */
    RdataArrowSource(std::shared_ptr<const Plan> plan, int threads, int64_t batch_rows = 0);
    ~RdataArrowSource();
    RdataArrowSource(const RdataArrowSource &) = delete;
    RdataArrowSource &operator=(const RdataArrowSource &) = delete;
    void start();
    ArrowArrayStream *stream();
    /* after the COPY: joins the workers; the first error ("" when none) */
    std::string finish();
    int64_t batches() const;
    /* the workers created to convert the columns (min(threads, columns)
     * planned), still known after finish() */
    int workers() const;
    /* the most batches alive at once (being filled or waiting for the
     * engine), and the bound they keep to, before an error and after it (G8) */
    int64_t peak_batches() const;
    int64_t max_batches() const;
    struct State;

  private:
    std::unique_ptr<State> st_;
};

} // namespace rdata
} // namespace parqit
