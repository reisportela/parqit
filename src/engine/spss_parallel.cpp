/* parqit — the SPSS profile and conversion passes on several threads
 * (SPSS-PARALLEL-1). See spss_parallel.hpp. */
#include "engine/spss_parallel.hpp"

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <limits>
#include <map>
#include <mutex>
#include <thread>

#include "engine/spss_reader.hpp"
#include "engine/test_fault.hpp"

namespace parqit {
namespace spss {

int64_t block_cases_for(const Dictionary &d) {
    if (const char *e = std::getenv("PARQIT_TEST_SPSS_BLOCK_CASES")) {
        char *end = nullptr;
        const long long v = std::strtoll(e, &end, 10);
        if (end != e && *end == '\0' && v > 0) return v;
    }
    constexpr int64_t kRowGroup = 122880, kVector = 2048;
    constexpr int64_t kBlockBytes = int64_t(64) << 20;
    const int64_t bytes = static_cast<int64_t>(std::max<size_t>(1, d.nslots)) * 8;
    int64_t cases = std::min(kRowGroup, std::max<int64_t>(1, kBlockBytes / bytes));
    if (cases >= kVector) cases = cases / kVector * kVector;
    return cases;
}

int64_t planned_cases(const Dictionary &d) {
    if (d.ncases >= 0) return d.ncases;
    if (d.compression != Compression::None) return -1;
    const uint64_t case_bytes = static_cast<uint64_t>(std::max<size_t>(1, d.nslots)) * 8;
    return d.file_size > d.data_offset ? static_cast<int64_t>((d.file_size - d.data_offset) / case_bytes) : 0;
}

int64_t for_each_block(const std::string &path, const Dictionary &d, int threads,
                       int64_t block_cases, const std::function<void(const CaseBlock &, int)> &work,
                       const std::function<void(int64_t, int64_t)> &throttle, BlockStats *stats,
                       int64_t raw_budget, int64_t scheduled_cases,
                       const std::function<void()> &on_worker_failure) {
    if (threads < 1) threads = 1;
    if (block_cases < 1) block_cases = 1;
    if (raw_budget <= 0) raw_budget = kRawBudgetBytes;
    /* A3 (F8, G2): no more workers than the blocks the file declares, or than
     * those of the count known or estimated when it declares none */
    const int64_t planned = d.ncases >= 0 ? d.ncases : scheduled_cases;
    if (planned >= 0)
        threads = static_cast<int>(std::min<int64_t>(
            threads, std::max<int64_t>(1, planned / block_cases + (planned % block_cases != 0))));
    const size_t ns = d.nslots;
    const int64_t case_bytes = static_cast<int64_t>(ns) * 8;
    /* the slots of a block, checked: block_cases * ns elements of 8 bytes */
    if (ns > 0 && static_cast<uint64_t>(block_cases) > (std::numeric_limits<size_t>::max() / 8) / ns)
        throw SavError("the SPSS file " + path + " has cases too wide to convert in blocks");
    std::mutex m;
    std::condition_variable cv_items, cv_space;
    std::deque<std::unique_ptr<CaseBlock>> q;
    const size_t cap = static_cast<size_t>(threads) * 2;
    bool done = false, stop = false;
    int64_t alive = 0; /* A4: raw room reserved by the blocks produced and not yet freed */
    int64_t err_block = std::numeric_limits<int64_t>::max();
    std::exception_ptr err_worker;
    auto fail = [&](int64_t index, std::exception_ptr e) {
        std::lock_guard<std::mutex> lk(m);
        if (index < err_block) {
            err_block = index;
            err_worker = e;
        }
        stop = true;
        cv_items.notify_all();
        cv_space.notify_all();
    };
    /* a block is freed before its room is given back */
    auto release = [&](std::unique_ptr<CaseBlock> &b) {
        const int64_t bytes = b->room * case_bytes;
        b.reset();
        std::lock_guard<std::mutex> lk(m);
        alive -= bytes;
        cv_space.notify_all();
    };
    auto loop = [&](int w) {
        for (;;) {
            std::unique_ptr<CaseBlock> b;
            {
                std::unique_lock<std::mutex> lk(m);
                cv_items.wait(lk, [&] { return !q.empty() || done || stop; });
                if (stop || q.empty()) return;
                b = std::move(q.front());
                q.pop_front();
                cv_space.notify_one();
            }
            try {
                if (b->expand) {
                    /* B1: the cases the producer walked, expanded here */
                    test_fault("spss_expand", b->index);
                    CaseReader x(path, d, b->at);
                    b->slots.reserve(static_cast<size_t>(b->room) * ns);
                    for (int64_t k = 0; k < b->n; k++) {
                        if (!x.next())
                            throw SavError("the SPSS file " + path + " changed while it was being read; read it again");
                        b->slots.insert(b->slots.end(), x.slots(), x.slots() + ns);
                    }
                }
                work(*b, w);
            } catch (...) {
                fail(b->index, std::current_exception());
                /* a failure before work() (expanding the block) must also wake
                 * a caller waiting on its own state (Codex audit, finding 1) */
                if (on_worker_failure) {
                    try {
                        on_worker_failure();
                    } catch (...) {
                    }
                }
                release(b);
                return;
            }
            release(b);
        }
    };
    /* LIFE-018: a worker that cannot be created leaves its blocks to the
     * others (none at all: the producer converts each block itself); every
     * created worker is joined before anything returns or rethrows. */
    std::vector<std::thread> pool;
    if (threads > 1) {
        try {
            pool.reserve(static_cast<size_t>(threads));
            for (int t = 0; t < threads; t++) {
                test_fault("spss_thread", t);
                pool.emplace_back(loop, t);
            }
        } catch (...) {
        }
    }
    if (stats) stats->workers = pool.empty() ? 1 : static_cast<int>(pool.size());
    /* B1: only workers expand, so only with them (a pool that could not start
     * leaves the producer decoding, as before) */
    const bool expand = d.compression != Compression::None && !pool.empty() && b1_restart_fits(d, pool.size());
    if (stats) stats->restart_readers = expand;
    std::unique_ptr<CaseReader> rp;
    std::exception_ptr err_producer;
    try {
        rp = std::make_unique<CaseReader>(path, d, threads > 1 ? threads : 0); /* B2 */
        CaseReader &r = *rp;
        if (stats) stats->ahead_threads = r.ahead_threads();
        for (int64_t index = 0;; index++) {
            /* A2: room for the cases this block can still hold — the cases
             * left when the file declares their number (the reader stops
             * there) — filled case by case, never zeroed first */
            int64_t room = block_cases;
            if (d.ncases >= 0) room = std::min(room, std::max<int64_t>(0, d.ncases - r.cases_read()));
            if (throttle) throttle(index, room);
            {
                /* A4: the room fits the raw budget, or the block goes alone */
                std::unique_lock<std::mutex> lk(m);
                cv_space.wait(lk, [&] { return alive == 0 || alive + room * case_bytes <= raw_budget || stop; });
                if (stop) break;
                alive += room * case_bytes;
                if (stats) stats->peak_raw = std::max(stats->peak_raw, alive);
            }
            auto b = std::make_unique<CaseBlock>();
            b->index = index;
            b->first = r.cases_read();
            b->room = room;
            /* a reader failure inside a block still hands the cases before it
             * to the workers first: serially those cases are profiled or
             * converted, and fail, before the failing case is read */
            int64_t k = 0;
            std::exception_ptr fill_err;
            try {
                if (expand) {
                    b->expand = true;
                    b->at = r.restart();
                    while (k < block_cases && r.skip()) k++;
                } else {
                    b->slots.reserve(static_cast<size_t>(room) * ns);
                    while (k < block_cases && r.next()) {
                        b->slots.insert(b->slots.end(), r.slots(), r.slots() + ns);
                        k++;
                    }
                }
            } catch (...) {
                fill_err = std::current_exception();
            }
            if (k == 0) {
                release(b);
                if (fill_err) std::rethrow_exception(fill_err);
                break;
            }
            b->n = k;
            const bool last = k < block_cases || fill_err;
            if (pool.empty()) {
                try {
                    work(*b, 0);
                } catch (...) {
                    fail(b->index, std::current_exception());
                    release(b);
                    break;
                }
                release(b);
            } else {
                std::unique_lock<std::mutex> lk(m);
                cv_space.wait(lk, [&] { return q.size() < cap || stop; });
                if (stop) break;
                q.push_back(std::move(b));
                cv_items.notify_one();
            }
            if (fill_err) std::rethrow_exception(fill_err);
            if (last) break;
        }
    } catch (...) {
        err_producer = std::current_exception();
    }
    {
        std::lock_guard<std::mutex> lk(m);
        done = true;
        cv_items.notify_all();
    }
    for (std::thread &t : pool) t.join();
    if (err_worker) std::rethrow_exception(err_worker);
    if (err_producer) std::rethrow_exception(err_producer);
    return rp ? rp->cases_read() : 0;
}

bool b1_restart_fits(const Dictionary &d, size_t workers) {
    if (d.compression != Compression::Zlib) return true;
    /* Codex audit, round 2, finding 5: a reader's two buffers keep their
     * capacity from block to block, so it may hold the file's largest
     * inflated block and its largest compressed one, with its copy of the
     * block directory; summed in 64 bits */
    uint64_t inflated = 0, compressed = 0;
    for (const auto &b : d.zblocks) {
        inflated = std::max<uint64_t>(inflated, b.uncompressed_size);
        compressed = std::max<uint64_t>(compressed, b.compressed_size);
    }
    const uint64_t reader = inflated + compressed + d.zblocks.size() * sizeof(Dictionary::ZBlock);
    uint64_t budget = static_cast<uint64_t>(kRawBudgetBytes);
    if (const char *e = std::getenv("PARQIT_TEST_B1_BUDGET_BYTES")) { /* the tests' smaller bound */
        const long long v = std::atoll(e);
        if (v > 0) budget = static_cast<uint64_t>(v);
    }
    return reader <= budget / std::max<size_t>(workers, 1);
}

/* ------------------------------------------------------------ the batches */

namespace {

enum class Col { Double, Date, Timestamp, Time, Text, Code };

/* one Arrow record batch: the converted values of one block of cases */
struct Batch {
    int64_t index = 0, n = 0;
    std::vector<std::vector<uint8_t>> data, valid;
    std::vector<std::vector<int64_t>> offs;
    std::vector<std::vector<char>> bytes;
    std::vector<int64_t> nulls;
    std::vector<ArrowArray> child;
    std::vector<ArrowArray *> child_ptr;
    std::vector<const void *> bufs;
    const void *top_bufs[1] = {nullptr};
    ArrowArray top{};
};

void noop_schema_release(ArrowSchema *s) { s->release = nullptr; }
void noop_array_release(ArrowArray *a) { a->release = nullptr; }

int width_of(Col c) {
    switch (c) {
    case Col::Date: return 4;
    case Col::Code: return 1;
    case Col::Text: return 0;
    default: return 8;
    }
}

const char *format_of(Col c) {
    switch (c) {
    case Col::Date: return "tdD";
    case Col::Timestamp: return "tsu:";
    case Col::Time: return "ttu";
    case Col::Text: return "U"; /* large utf8: SPSS strings reach 32767 bytes */
    case Col::Code: return "c";
    default: return "g";
    }
}

} // namespace

struct SavArrowSource::State {
    std::shared_ptr<const Plan> plan;
    int threads = 1;
    int64_t block = 0;
    std::vector<Col> kinds;          /* output columns: the plan's, then companions */
    std::vector<int> companion_of;   /* per plan column: its companion's output column, -1 */
    std::vector<std::string> names;
    std::vector<ArrowSchema> child_s;
    std::vector<ArrowSchema *> child_s_ptr;
    ArrowSchema schema{};
    ArrowArrayStream stream{};
    std::mutex mu;
    std::condition_variable cv;
    std::map<int64_t, Batch *> ready;
    int64_t next_out = 0, outstanding = 0, cap = 2;
    /* A4: the bytes of converted batches reserved from each block's production
     * until the engine takes its batch (by block), within `budget`; the
     * batches the engine holds are not counted (it releases one only after
     * taking the scan's lock, which a thread waiting in stream_next holds) */
    int64_t row_bytes = 1, budget = kBatchBudgetBytes, raw_budget = kRawBudgetBytes;
    int64_t reserved = 0, peak_reserved = 0;
    BlockStats stats;
    std::map<int64_t, int64_t> reservation;
    bool producer_done = false, aborted = false, failed = false, last_error_lost = false;
    /* the outcome of the producer (A5: recorded without allocating, worded
     * only when reported): the exception it ended with, the cases it read */
    std::exception_ptr failure;
    int64_t cases_read = -1;
    std::string error, last_error;
    std::thread driver;

    void convert(const CaseBlock &b, Batch *out, std::string *raw, std::string *utf8) const;
    void wire(Batch *b) const;
    std::string message() const;
    void stop() noexcept;
};

namespace {

/* the engine is done with a batch: it owns nothing else, so a release that
 * comes after the source has finished is still safe */
void batch_release(ArrowArray *a) {
    delete static_cast<Batch *>(a->private_data);
    a->private_data = nullptr;
    a->release = nullptr;
}

int stream_schema(ArrowArrayStream *s, ArrowSchema *out) {
    *out = static_cast<SavArrowSource::State *>(s->private_data)->schema;
    out->release = noop_schema_release;
    return 0;
}

int stream_next(ArrowArrayStream *s, ArrowArray *out) {
    auto *st = static_cast<SavArrowSource::State *>(s->private_data);
    std::unique_lock<std::mutex> lk(st->mu);
    st->cv.wait(lk, [&] { return st->ready.count(st->next_out) || st->producer_done; });
    auto it = st->ready.find(st->next_out);
    if (it != st->ready.end()) {
        Batch *b = it->second;
        st->ready.erase(it);
        const auto r = st->reservation.find(st->next_out);
        if (r != st->reservation.end()) {
            st->reserved -= r->second;
            st->reservation.erase(r);
        }
        st->next_out++;
        st->outstanding--; /* handed over: the producer may read ahead again */
        st->cv.notify_all();
        *out = b->top;
        out->private_data = b;
        out->release = batch_release;
        return 0;
    }
    try {
        st->last_error = st->message();
    } catch (...) {
        st->last_error_lost = true;
    }
    if (st->last_error_lost || !st->last_error.empty()) return EIO;
    std::memset(out, 0, sizeof(*out)); /* a released array ends the stream */
    return 0;
}

const char *stream_error(ArrowArrayStream *s) {
    const auto *st = static_cast<SavArrowSource::State *>(s->private_data);
    return st->last_error_lost ? "the SPSS conversion failed" : st->last_error.c_str();
}

void stream_release(ArrowArrayStream *s) { s->release = nullptr; }

} // namespace

/* The table function's decoding (spss_table.cpp, sav_scan), case by case into
 * the batch; a value that no longer fits the profile is the file changing. */
void SavArrowSource::State::convert(const CaseBlock &b, Batch *out, std::string *raw,
                                    std::string *utf8) const {
    const Plan &p = *plan;
    const Dictionary &d = p.dict;
    const size_t m = kinds.size();
    const size_t n = static_cast<size_t>(b.n);
    out->index = b.index;
    out->n = b.n;
    out->data.resize(m);
    out->valid.resize(m);
    out->offs.resize(m);
    out->bytes.resize(m);
    out->nulls.assign(m, 0);
    for (size_t c = 0; c < m; c++) {
        if (kinds[c] == Col::Text) {
            out->offs[c].assign(n + 1, 0);
        } else {
            out->data[c].assign(n * static_cast<size_t>(width_of(kinds[c])), 0);
            if (kinds[c] != Col::Code) out->valid[c].assign((n + 7) / 8, 0xFF);
        }
    }
    for (size_t k = 0; k < n; k++) {
        const uint64_t *slots = &b.slots[k * d.nslots];
        for (size_t i = 0; i < p.cols.size(); i++) {
            const ColumnSpec &c = p.cols[i];
            const Variable &v = d.vars[static_cast<size_t>(c.var)];
            if (c.kind == OutKind::Varchar) {
                slot_raw_string(v, slots, raw);
                decode_text(d, raw->data(), raw->size(), utf8);
                out->bytes[i].insert(out->bytes[i].end(), utf8->begin(), utf8->end());
                out->offs[i][k + 1] = static_cast<int64_t>(out->bytes[i].size());
                continue;
            }
            const double x = slot_number(v, slots);
            int8_t code = 0;
            bool null = false, ok = true;
            uint8_t *dst = out->data[i].data();
            if (slot_is_sysmis(d, v, slots)) {
                null = true;
            } else if (!v.missing.empty() && v.missing.matches(x)) {
                null = true;
                code = static_cast<int8_t>(c.code_of(x));
            } else {
                switch (c.kind) {
                case OutKind::Date: {
                    int32_t days = 0;
                    ok = seconds_to_unix_days(x, &days);
                    std::memcpy(dst + k * 4, &days, 4);
                    break;
                }
                case OutKind::Timestamp: {
                    int64_t us = 0;
                    ok = seconds_to_unix_us(x, &us);
                    std::memcpy(dst + k * 8, &us, 8);
                    break;
                }
                case OutKind::Time: {
                    int64_t us = 0;
                    ok = seconds_to_time_us(x, &us);
                    std::memcpy(dst + k * 8, &us, 8);
                    break;
                }
                default:
                    std::memcpy(dst + k * 8, &x, 8);
                }
                if (!ok)
                    throw SavError("the SPSS file " + p.path + " changed while it was being "
                                   "converted (variable " + v.name + ", case " +
                                   std::to_string(b.first + static_cast<int64_t>(k) + 1) +
                                   "); convert it again");
            }
            if (null) {
                out->valid[i][k >> 3] &= static_cast<uint8_t>(~(1u << (k & 7)));
                out->nulls[i]++;
            }
            const int comp = companion_of[i];
            if (comp >= 0) out->data[static_cast<size_t>(comp)][k] = static_cast<uint8_t>(code);
        }
    }
    wire(out);
}

void SavArrowSource::State::wire(Batch *b) const {
    const size_t m = kinds.size();
    b->child.assign(m, ArrowArray{});
    b->child_ptr.assign(m, nullptr);
    b->bufs.assign(m * 3, nullptr);
    static const char kNoBytes = 0;
    for (size_t c = 0; c < m; c++) {
        ArrowArray &a = b->child[c];
        const void **bp = &b->bufs[c * 3];
        a.length = b->n;
        a.release = noop_array_release;
        if (kinds[c] == Col::Text) {
            a.n_buffers = 3;
            bp[1] = b->offs[c].data();
            bp[2] = b->bytes[c].empty() ? &kNoBytes : b->bytes[c].data();
        } else {
            a.n_buffers = 2;
            a.null_count = b->nulls[c];
            bp[0] = b->nulls[c] ? b->valid[c].data() : nullptr;
            bp[1] = b->data[c].data();
        }
        a.buffers = bp;
        b->child_ptr[c] = &a;
    }
    b->top = ArrowArray{};
    b->top.length = b->n;
    b->top.n_buffers = 1;
    b->top.buffers = b->top_bufs;
    b->top.n_children = static_cast<int64_t>(m);
    b->top.children = m ? b->child_ptr.data() : nullptr;
}

SavArrowSource::SavArrowSource(std::shared_ptr<const Plan> plan, int threads, int64_t block_cases,
                               int64_t batch_budget, int64_t raw_budget)
    : st_(std::make_unique<State>()) {
    State &s = *st_;
    s.plan = std::move(plan);
    s.threads = std::max(1, threads);
    const Plan &p = *s.plan;
    s.block = block_cases > 0 ? block_cases : block_cases_for(p.dict);
    s.companion_of.assign(p.cols.size(), -1);
    for (const auto &c : p.cols) {
        switch (c.kind) {
        case OutKind::Date: s.kinds.push_back(Col::Date); break;
        case OutKind::Timestamp: s.kinds.push_back(Col::Timestamp); break;
        case OutKind::Time: s.kinds.push_back(Col::Time); break;
        case OutKind::Varchar: s.kinds.push_back(Col::Text); break;
        default: s.kinds.push_back(Col::Double);
        }
        s.names.push_back(c.name);
    }
    for (size_t i = 0; i < p.cols.size(); i++) {
        if (!p.cols[i].companion) continue;
        s.companion_of[i] = static_cast<int>(s.kinds.size());
        s.kinds.push_back(Col::Code);
        s.names.push_back(p.cols[i].companion_name);
    }
    const size_t m = s.kinds.size();
    s.child_s.assign(m, ArrowSchema{});
    s.child_s_ptr.assign(m, nullptr);
    for (size_t c = 0; c < m; c++) {
        s.child_s[c].format = format_of(s.kinds[c]);
        s.child_s[c].name = s.names[c].c_str();
        s.child_s[c].flags = 2; /* ARROW_FLAG_NULLABLE */
        s.child_s[c].release = noop_schema_release;
        s.child_s_ptr[c] = &s.child_s[c];
    }
    s.schema.format = "+s";
    s.schema.name = "";
    s.schema.n_children = static_cast<int64_t>(m);
    s.schema.children = m ? s.child_s_ptr.data() : nullptr;
    s.schema.release = noop_schema_release;
    s.stream.get_schema = stream_schema;
    s.stream.get_next = stream_next;
    s.stream.get_last_error = stream_error;
    s.stream.release = stream_release;
    s.stream.private_data = &s;
    /* blocks read ahead of the engine (being converted, or converted and not
     * yet handed over): two per worker at most, and their converted batches
     * within the batch budget (A4: the bytes a block's batch can take,
     * reserved when the block is produced, so an admitted block always
     * completes; a block wider than the budget goes alone). Only blocks not
     * yet handed over count: an engine thread releases its batch only after it
     * next takes the scan's lock, which a thread waiting in stream_next holds,
     * so counting held batches could deadlock. The engine holds at most one
     * batch per thread besides; the raw cases have their own budget
     * (for_each_block). */
    int64_t row = 0;
    for (size_t c = 0; c < m; c++)
        row += s.kinds[c] == Col::Text
                   ? 8 + static_cast<int64_t>(p.cols[c].max_bytes)
                   : width_of(s.kinds[c]) + (s.kinds[c] == Col::Code ? 0 : 1);
    s.row_bytes = std::max<int64_t>(1, row);
    s.cap = std::max<int64_t>(1, 2 * s.threads);
    if (batch_budget > 0) s.budget = batch_budget;
    if (raw_budget > 0) s.raw_budget = raw_budget;
}

SavArrowSource::~SavArrowSource() { st_->stop(); }

/* the first error in case order, worded as the table function words it, ""
 * when none (call with mu held) */
std::string SavArrowSource::State::message() const {
    if (!error.empty()) return error;
    if (failure) {
        try {
            std::rethrow_exception(failure);
        } catch (const std::exception &e) {
            return e.what();
        } catch (...) {
        }
        return "the SPSS conversion failed";
    }
    if (producer_done && !aborted && cases_read != plan->ncases)
        return "the SPSS file " + plan->path + " changed while it was being converted (" +
               std::to_string(cases_read) + " cases now, " + std::to_string(plan->ncases) +
               " when profiled); convert it again";
    return "";
}

void SavArrowSource::start() {
    State &s = *st_;
    uint64_t size = 0;
    long long mtime = 0;
    const Plan &p = *s.plan;
    if (!file_identity(p.path, &size, &mtime) || size != p.file_size || mtime != p.file_mtime) {
        std::lock_guard<std::mutex> lk(s.mu);
        s.error = "the SPSS file " + p.path + " changed while it was being converted; convert it again";
        s.producer_done = true;
        return;
    }
    /* A5 (F11): no exception leaves the driver, and every failure of a
     * worker's lambda — allocating the batch, converting, inserting it —
     * wakes a producer waiting for read-ahead room before it is rethrown; a
     * batch stays owned until the ready map holds it */
    auto drive = [&s] {
        std::exception_ptr ep;
        int64_t cases = -1;
        try {
            std::vector<std::string> raws(static_cast<size_t>(s.threads)),
                utf8s(static_cast<size_t>(s.threads));
            cases = for_each_block(
                s.plan->path, s.plan->dict, s.threads, s.block,
                [&](const CaseBlock &b, int w) {
                    try {
                        test_fault("spss_batch", b.index);
                        auto batch = std::make_unique<Batch>();
                        test_delay("spss_convert", b.index);
                        test_fault("spss_convert", b.index);
                        s.convert(b, batch.get(), &raws[static_cast<size_t>(w)],
                                  &utf8s[static_cast<size_t>(w)]);
                        std::lock_guard<std::mutex> lk(s.mu);
                        test_fault("spss_ready", b.index);
                        s.ready.emplace(b.index, batch.get());
                        batch.release();
                        s.cv.notify_all();
                    } catch (...) {
                        /* no block after this one will be handed over */
                        std::lock_guard<std::mutex> lk(s.mu);
                        s.failed = true;
                        s.cv.notify_all();
                        throw;
                    }
                },
                [&](int64_t index, int64_t room) {
                    const int64_t bytes = room * s.row_bytes;
                    std::unique_lock<std::mutex> lk(s.mu);
                    s.cv.wait(lk, [&] {
                        return (s.outstanding < s.cap && (s.reserved == 0 || s.reserved + bytes <= s.budget)) ||
                               s.aborted || s.failed;
                    });
                    /* G8: nothing is admitted once a block failed (its error, a
                     * worker's, is the one rethrown) */
                    if (s.aborted || s.failed) throw SavError("aborted");
                    s.reservation[index] = bytes;
                    s.outstanding++;
                    s.reserved += bytes;
                    s.peak_reserved = std::max(s.peak_reserved, s.reserved);
                },
                &s.stats, s.raw_budget, s.plan->ncases,
                [&s] {
                    std::lock_guard<std::mutex> lk(s.mu);
                    s.failed = true;
                    s.cv.notify_all();
                });
        } catch (...) {
            ep = std::current_exception();
        }
        std::lock_guard<std::mutex> lk(s.mu);
        /* G8: the room of a block whose batch will never be handed over — the
         * probe past the last case, a block that failed or was cut short — is
         * given back once the producer is through */
        for (auto r = s.reservation.begin(); r != s.reservation.end();) {
            if (s.ready.count(r->first)) {
                ++r;
                continue;
            }
            s.reserved -= r->second;
            s.outstanding--;
            r = s.reservation.erase(r);
        }
        if (!s.aborted) {
            s.failure = ep;
            s.cases_read = cases;
        }
        s.producer_done = true;
        s.cv.notify_all();
    };
    try {
        test_fault("spss_driver", 0);
        s.driver = std::thread(drive);
    } catch (const std::exception &e) {
        std::lock_guard<std::mutex> lk(s.mu);
        s.error = std::string("could not start the SPSS conversion: ") + e.what();
        s.producer_done = true;
        s.cv.notify_all();
    }
}

ArrowArrayStream *SavArrowSource::stream() { return &st_->stream; }

int64_t SavArrowSource::batches() const {
    std::lock_guard<std::mutex> lk(st_->mu);
    return st_->next_out;
}

int64_t SavArrowSource::peak_batch_bytes() const {
    std::lock_guard<std::mutex> lk(st_->mu);
    return st_->peak_reserved;
}

int64_t SavArrowSource::peak_raw_bytes() const {
    std::lock_guard<std::mutex> lk(st_->mu);
    return st_->stats.peak_raw;
}

int SavArrowSource::workers() const {
    std::lock_guard<std::mutex> lk(st_->mu);
    return st_->stats.workers;
}

/* stop and join the producer, and free every batch the engine did not take */
void SavArrowSource::State::stop() noexcept {
    {
        std::lock_guard<std::mutex> lk(mu);
        if (!producer_done) aborted = true; /* the engine stopped reading */
        cv.notify_all();
    }
    if (driver.joinable()) driver.join();
    std::lock_guard<std::mutex> lk(mu);
    for (auto &kv : ready) delete kv.second;
    ready.clear();
}

std::string SavArrowSource::finish() {
    State &s = *st_;
    s.stop();
    std::lock_guard<std::mutex> lk(s.mu);
    return s.message();
}

} // namespace spss
} // namespace parqit
