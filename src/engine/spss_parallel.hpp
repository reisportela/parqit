/* parqit — the SPSS profile and conversion passes on several threads
 * (SPSS-PARALLEL-1).
 *
 * One CaseReader, the producer, expands the cases in file order: bytecode and
 * ZLIB data can only be expanded in order, and a malformed file then fails at
 * the same case with the same message as the serial reader. It copies them into
 * blocks of raw case slots, which workers profile or convert whole. The profile
 * merges per-worker tallies whose merge does not depend on the order of the
 * cases; the conversion fills one Arrow record batch per block and serves the
 * batches in case order through a single-pass ArrowArrayStream, so the engine's
 * Arrow scan writes them on its own threads and in order (its batch indexes),
 * as `save, data` does (SAVE-PARALLEL-1). Every value is decoded by the code
 * the table function uses, so the Parquet file is the one it writes.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "abi.h"
#include "engine/spss_plan.hpp"

namespace parqit {
namespace spss {

struct CaseBlock {
    int64_t index = 0;           /* block number, in case order */
    int64_t first = 0;           /* index of its first case */
    int64_t n = 0;               /* its cases */
    int64_t room = 0;            /* the cases it was given room for */
    std::vector<uint64_t> slots; /* n * nslots: each case as CaseReader holds it */
    /* B1: a compressed file's block is walked by the producer and expanded by
     * its worker from here (slots then empty until it is) */
    bool expand = false;
    CaseReader::Restart at;
};

/* The bytes of raw cases the blocks alive may reserve together (A4): produced
 * and not yet freed, in the profile and in the conversion alike. */
constexpr int64_t kRawBudgetBytes = int64_t(512) << 20;
/* The bytes of converted batches read ahead of the engine (from the block's
 * production until the engine takes its batch) in the conversion. */
constexpr int64_t kBatchBudgetBytes = int64_t(512) << 20;

/* Cases per block for a dictionary: one row group of the engine's default
 * (122880), fewer for a wide file so a block of raw slots stays within 64 MB
 * (whole vectors of 2048 down to 2048 cases, then any number down to one case:
 * A2, F2 — a floor of 2048 cases made a block of 2.7 GB for a file of 60 cases
 * with long strings). PARQIT_TEST_SPSS_BLOCK_CASES overrides it so the suite
 * can drive many blocks through small files. */
int64_t block_cases_for(const Dictionary &d);

/* What a pass did (tests and traces): the most raw room reserved at once and
 * the threads that worked on the blocks (1: the producer alone). */
struct BlockStats {
    int64_t peak_raw = 0;
    int workers = 0;
    bool restart_readers = false; /* B1: the workers expanded the cases */
    size_t ahead_threads = 0;     /* B2: the producer's threads inflating ahead */
};

/* The cases of the file in blocks of up to `block_cases`, each handed to
 * work(block, worker) on one of `threads` workers while the calling thread
 * produces. A block reserves room for the cases it can still hold (the cases
 * left when the file declares their number) and is filled case by case, never
 * zeroed first. Before producing block `index` with room for `room` cases the
 * producer calls throttle(index, room), which may wait (the conversion's
 * batches), and waits until the raw room of the blocks alive plus this one
 * fits `raw_budget` bytes (0 = kRawBudgetBytes), or no block is alive, so one
 * case wider than the budget still goes through, alone (A4). There are never
 * more workers than the blocks the file declares (A3, F8: one block is worked
 * on by the producer, no thread started); a file that declares no count is
 * planned on `scheduled_cases` when known (counted by the profile, or
 * estimated from an uncompressed payload: G2), which sets only the number of
 * workers, never what is read. With workers, a compressed file's cases are
 * walked by the producer (every code checked, nothing decoded) and each block
 * is expanded by its worker from where the walk found it (B1): the same slots,
 * the same errors, while the restart readers fit (b1_restart_fits). Returns
 * the cases read. The first error in case order is rethrown once every worker
 * has stopped: a worker's (lowest block first), else the producer's. A
 * worker's failure, before work() or in it, also calls on_worker_failure, so
 * a throttle waiting on the caller's own state wakes (it would never see that
 * block's work). */
int64_t for_each_block(const std::string &path, const Dictionary &d, int threads,
                       int64_t block_cases, const std::function<void(const CaseBlock &, int)> &work,
                       const std::function<void(int64_t, int64_t)> &throttle = nullptr,
                       BlockStats *stats = nullptr, int64_t raw_budget = 0, int64_t scheduled_cases = -1,
                       const std::function<void()> &on_worker_failure = nullptr);

/* B1: each worker's restart reader of a .zsav holds up to the file's largest
 * inflated block and its largest compressed one (its buffers keep their
 * capacity), with a copy of the block directory (a bytecode .sav: a span of
 * 1 MB); they are used while `workers` such readers fit kRawBudgetBytes, else
 * the producer decodes (the same slots) */
bool b1_restart_fits(const Dictionary &d, size_t workers);

/* The cases to plan workers on: the declared count, else an estimate from an
 * uncompressed payload (whole cases), else -1 (a compressed file is counted by
 * its profile) */
int64_t planned_cases(const Dictionary &d);

/* The conversion's record batches, in case order. start() launches the
 * producer and the workers; stream() is the ArrowArrayStream to register (its
 * single pass ends after the last case); after the COPY, finish() joins them
 * and returns the first error in case order, worded as the table function words
 * it ("" when none). */
class SavArrowSource {
  public:
    /* block_cases 0 = block_cases_for(plan's dictionary) (tests use few);
     * batch_budget 0 = kBatchBudgetBytes, raw_budget 0 = kRawBudgetBytes */
    SavArrowSource(std::shared_ptr<const Plan> plan, int threads, int64_t block_cases = 0,
                   int64_t batch_budget = 0, int64_t raw_budget = 0);
    ~SavArrowSource();
    SavArrowSource(const SavArrowSource &) = delete;
    SavArrowSource &operator=(const SavArrowSource &) = delete;
    void start();
    ArrowArrayStream *stream();
    std::string finish();
    /* the batches handed to the engine (after finish) */
    int64_t batches() const;
    /* tests: the most bytes of batches reserved at once, and of raw room;
     * the threads that converted (after finish) */
    int64_t peak_batch_bytes() const;
    int64_t peak_raw_bytes() const;
    int workers() const;
    struct State;

  private:
    std::unique_ptr<State> st_;
};

} // namespace spss
} // namespace parqit
