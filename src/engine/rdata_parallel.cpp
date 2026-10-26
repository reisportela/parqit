/* parqit — the R data conversion's batches on several threads
 * (RDATA-PARALLEL-1). See rdata_parallel.hpp. */
#include "engine/rdata_parallel.hpp"

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <tuple>
#include <vector>

#include "engine/rdata_convert.hpp"
#include "engine/rdata_reader.hpp"
#include "engine/test_fault.hpp"

namespace parqit {
namespace rdata {

namespace {

enum class Col { Boolean, Integer, BigInt, Double, Date, Timestamp, Time, Text, Code };

int width_of(Col c) {
    switch (c) {
    case Col::Integer:
    case Col::Date: return 4;
    case Col::Code: return 1;
    case Col::Boolean:
    case Col::Text: return 0;
    default: return 8;
    }
}

const char *format_of(Col c) {
    switch (c) {
    case Col::Boolean: return "b";
    case Col::Integer: return "i";
    case Col::BigInt: return "l";
    case Col::Date: return "tdD";
    case Col::Timestamp: return "tsu:";
    case Col::Time: return "ttu";
    case Col::Text: return "U";
    case Col::Code: return "c";
    default: return "g";
    }
}

struct Batch {
    int64_t index = 0, first = 0, n = 0;
    int remaining = 0; /* workers still filling it */
    bool partial = false; /* a worker skipped a column of it (after an error): never handed on */
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
void batch_release(ArrowArray *a) {
    delete static_cast<Batch *>(a->private_data);
    a->private_data = nullptr;
    a->release = nullptr;
}

/* convert_rows' destination: column c of one batch (and its companion) */
struct ArrowOut {
    Batch *b;
    size_t c;
    int comp; /* the companion's output column, -1 none */
    void text(uint64_t, const char *p, size_t n) { b->bytes[c].insert(b->bytes[c].end(), p, p + n); }
    void boolean(uint64_t r, bool v) {
        if (v) b->data[c][r >> 3] |= static_cast<uint8_t>(1u << (r & 7));
    }
    void i64(uint64_t r, int64_t v) { std::memcpy(b->data[c].data() + r * 8, &v, 8); }
    void i32(uint64_t r, int32_t v) { std::memcpy(b->data[c].data() + r * 4, &v, 4); }
    void f64(uint64_t r, double v) { std::memcpy(b->data[c].data() + r * 8, &v, 8); }
    void days(uint64_t r, int32_t d) { std::memcpy(b->data[c].data() + r * 4, &d, 4); }
    void timestamp(uint64_t r, int64_t us) { std::memcpy(b->data[c].data() + r * 8, &us, 8); }
    void time(uint64_t r, int64_t us) { std::memcpy(b->data[c].data() + r * 8, &us, 8); }
    void valid(uint64_t r, bool v) {
        if (!b->offs[c].empty()) b->offs[c][r + 1] = static_cast<int64_t>(b->bytes[c].size());
        if (!v) {
            b->valid[c][r >> 3] &= static_cast<uint8_t>(~(1u << (r & 7)));
            b->nulls[c]++;
        }
    }
    void code(uint64_t r, int8_t k) {
        if (comp >= 0) b->data[static_cast<size_t>(comp)][r] = static_cast<uint8_t>(k);
    }
};

/* convert_rows' destination that keeps nothing: after an error, the work
 * that precedes it only looks for an earlier one (G8) */
struct NoOut {
    void text(uint64_t, const char *, size_t) {}
    void boolean(uint64_t, bool) {}
    void i64(uint64_t, int64_t) {}
    void i32(uint64_t, int32_t) {}
    void f64(uint64_t, double) {}
    void days(uint64_t, int32_t) {}
    void timestamp(uint64_t, int64_t) {}
    void time(uint64_t, int64_t) {}
    void valid(uint64_t, bool) {}
    void code(uint64_t, int8_t) {}
};

} // namespace

struct RdataArrowSource::State {
    std::shared_ptr<const Plan> plan;
    int threads = 1, workers = 1;
    int64_t rows = 0, per = 122880, nbatches = 0;
    std::vector<Col> kinds;
    std::vector<int> companion_of;
    std::vector<std::string> names;
    std::vector<ArrowSchema> child_s;
    std::vector<ArrowSchema *> child_s_ptr;
    ArrowSchema schema{};
    ArrowArrayStream stream{};
    std::mutex mu;
    std::condition_variable cv;
    std::map<int64_t, Batch *> filling, ready;
    int64_t next_out = 0, cap = 2;
    /* the first batch dropped unhanded (the engine, which takes them in order,
     * gets none after it); the most batches alive at once */
    int64_t dropped = std::numeric_limits<int64_t>::max(), peak = 0;
    /* the first error in the table function's order (vector, column, row):
     * its text, or the exception it came as, worded only when reported (A5:
     * recording a failure allocates nothing) */
    std::tuple<int64_t, size_t, int64_t> err_key{std::numeric_limits<int64_t>::max(), 0, 0};
    std::string error, last_error;
    std::exception_ptr error_ex;
    bool error_prefixed = false, failed = false, last_error_lost = false;
    int running = 0, created = 0;
    bool aborted = false, started = false;
    std::vector<std::thread> pool;

    Batch *batch(int64_t b);
    void wire(Batch *b) const;
    void fail(size_t col, int64_t row, std::string msg, std::exception_ptr ex = nullptr,
              bool prefixed = false) noexcept;
    std::string message() const;
    bool precedes_error(int64_t first, size_t col) const;
    bool keep(int64_t b) const;
    void done(int64_t b, bool written);
    void work(int w);
    void work_columns(int w);
    void stop() noexcept;
};

namespace {

int stream_schema(ArrowArrayStream *s, ArrowSchema *out) {
    *out = static_cast<RdataArrowSource::State *>(s->private_data)->schema;
    out->release = noop_schema_release;
    return 0;
}

int stream_next(ArrowArrayStream *s, ArrowArray *out) {
    auto *st = static_cast<RdataArrowSource::State *>(s->private_data);
    std::unique_lock<std::mutex> lk(st->mu);
    st->cv.wait(lk, [&] { return st->ready.count(st->next_out) || st->running == 0; });
    auto it = st->ready.find(st->next_out);
    if (it != st->ready.end()) {
        Batch *b = it->second;
        st->ready.erase(it);
        st->next_out++;
        st->cv.notify_all();
        *out = b->top;
        out->private_data = b;
        out->release = batch_release;
        return 0;
    }
    if (st->failed) {
        try {
            st->last_error = st->message();
        } catch (...) {
            st->last_error_lost = true;
        }
        return EIO;
    }
    std::memset(out, 0, sizeof(*out)); /* a released array ends the stream */
    return 0;
}

const char *stream_error(ArrowArrayStream *s) {
    const auto *st = static_cast<RdataArrowSource::State *>(s->private_data);
    return st->last_error_lost ? "the R conversion failed" : st->last_error.c_str();
}

void stream_release(ArrowArrayStream *s) { s->release = nullptr; }

} // namespace

/* batch b, created (all its buffers sized) by the first worker that reaches
 * it; call with mu held */
Batch *RdataArrowSource::State::batch(int64_t b) {
    auto it = filling.find(b);
    if (it != filling.end()) return it->second;
    test_fault("rdata_batch", b);
    auto x = std::make_unique<Batch>(); /* owned until filling holds it */
    x->index = b;
    x->first = b * per;
    x->n = std::min(per, rows - x->first);
    x->remaining = workers;
    const size_t m = kinds.size(), n = static_cast<size_t>(x->n);
    x->data.resize(m);
    x->valid.resize(m);
    x->offs.resize(m);
    x->bytes.resize(m);
    x->nulls.assign(m, 0);
    for (size_t c = 0; c < m; c++) {
        if (kinds[c] == Col::Text) x->offs[c].assign(n + 1, 0);
        else if (kinds[c] == Col::Boolean) x->data[c].assign((n + 7) / 8, 0);
        else x->data[c].assign(n * static_cast<size_t>(width_of(kinds[c])), 0);
        if (kinds[c] != Col::Code) x->valid[c].assign((n + 7) / 8, 0xFF);
    }
    filling.emplace(b, x.get());
    peak = std::max<int64_t>(peak, static_cast<int64_t>(filling.size() + ready.size()));
    return x.release();
}

void RdataArrowSource::State::wire(Batch *b) const {
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
        a.null_count = b->nulls[c];
        bp[0] = b->nulls[c] ? b->valid[c].data() : nullptr;
        if (kinds[c] == Col::Text) {
            a.n_buffers = 3;
            bp[1] = b->offs[c].data();
            bp[2] = b->bytes[c].empty() ? &kNoBytes : b->bytes[c].data();
        } else {
            a.n_buffers = 2;
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

/* the table function meets errors vector by vector (2048 rows), column by
 * column, row by row; the smallest such key is the error reported. Recording
 * one allocates nothing (the text is swapped in, an exception kept as is). */
void RdataArrowSource::State::fail(size_t col, int64_t row, std::string msg, std::exception_ptr ex,
                                   bool prefixed) noexcept {
    std::lock_guard<std::mutex> lk(mu);
    const auto key = std::make_tuple(row / 2048, col, row);
    if (key < err_key) {
        err_key = key;
        error.swap(msg);
        error_ex = ex;
        error_prefixed = prefixed;
    }
    failed = true;
    cv.notify_all();
}

/* the recorded error as the table function words it (call with mu held) */
std::string RdataArrowSource::State::message() const {
    if (!error_ex) return error;
    try {
        std::rethrow_exception(error_ex);
    } catch (const std::exception &e) {
        return error_prefixed ? std::string("the R conversion failed: ") + e.what() : std::string(e.what());
    } catch (...) {
    }
    return "the R conversion failed";
}

/* could column col of the batch from row `first` on hold an error the table
 * function meets before the one recorded? (call with mu held) */
bool RdataArrowSource::State::precedes_error(int64_t first, size_t col) const {
    return std::make_tuple(first / 2048, col, first) < err_key;
}

/* can batch b still be handed on (call with mu held)? Before any error,
 * yes; after one, only a batch whose rows all come before the error's, whose
 * every column precedes it, and before any batch dropped. Each condition only
 * tightens, so a batch nobody may keep is never created afterwards. */
bool RdataArrowSource::State::keep(int64_t b) const {
    if (!failed) return true;
    if (b >= dropped || b >= std::get<2>(err_key) / per) return false;
    return plan->cols.empty() || precedes_error(b * per, plan->cols.size() - 1);
}

/* a worker is through with batch b (call with mu held): written is false when
 * it left its columns of it unfilled or one of them failed, and then the
 * batch is never handed on (G5). The last worker hands it on or drops it. */
void RdataArrowSource::State::done(int64_t b, bool written) {
    auto it = filling.find(b);
    if (it == filling.end()) return;
    Batch *x = it->second;
    if (!written) x->partial = true;
    if (--x->remaining > 0) return;
    if (!x->partial && keep(b)) {
        test_fault("rdata_ready", b);
        wire(x);
        ready.emplace(b, x);
        filling.erase(it);
    } else {
        filling.erase(it);
        delete x;
        dropped = std::min(dropped, b);
    }
    cv.notify_all();
}

void RdataArrowSource::State::work(int w) {
    /* no exception may leave a worker thread (charter §6.8) */
    try {
        work_columns(w);
    } catch (...) {
        fail(0, 0, std::string(), std::current_exception(), true);
    }
    std::lock_guard<std::mutex> lk(mu);
    running--;
    cv.notify_all();
}

void RdataArrowSource::State::work_columns(int w) {
    const Plan &p = *plan;
    /* the table function's cursors: its buffer size, so a read fails at the
     * same element */
    const size_t ncols = std::max<size_t>(1, p.cols.size());
    const size_t buffer = std::min<size_t>(size_t(1) << 20,
                                           std::max<size_t>(size_t(4) << 10, (size_t(256) << 20) / ncols));
    std::vector<size_t> mine;
    std::vector<std::unique_ptr<Cursor>> cursors;
    std::string text;
    try {
        for (size_t i = static_cast<size_t>(w); i < p.cols.size(); i += static_cast<size_t>(workers)) {
            mine.push_back(i);
            cursors.push_back(std::make_unique<Cursor>(p.parsed.source, p.cols[i].vec, p.parsed.codec, buffer));
        }
    } catch (...) {
        fail(mine.empty() ? 0 : mine.back(), 0, std::string(), std::current_exception());
    }
    /* F12: once an error is recorded, a worker still does the work the table
     * function does before it — the rest of the error's vector in the
     * columns before the error's — and nothing after it, so the error
     * reported does not depend on the batch size; a failed cursor is never
     * read again (every later batch of its column comes after its error).
     * G8: that work keeps nothing unless its batch can still be handed on, so
     * it needs no room and waits for none; a batch that can be handed on
     * waits for room as before the error. A batch is finished after every
     * worker passed every earlier one, so an error in an earlier batch is
     * recorded by then. */
    std::vector<char> todo(cursors.size(), 0);
    NoOut none;
    for (int64_t b = 0; b < nbatches; b++) {
        Batch *x = nullptr;
        {
            std::unique_lock<std::mutex> lk(mu);
            cv.wait(lk, [&] { return aborted || !keep(b) || b < next_out + cap; });
            if (aborted) break;
            bool any = false;
            for (size_t k = 0; k < cursors.size(); k++) {
                todo[k] = precedes_error(b * per, mine[k]);
                any = any || todo[k];
            }
            if (!any && !mine.empty()) {
                /* nothing of this worker's comes before the error: it leaves
                 * every batch from here on unfilled */
                std::vector<int64_t> left;
                for (const auto &kv : filling)
                    if (kv.first >= b) left.push_back(kv.first);
                for (int64_t i : left) done(i, false);
                break;
            }
            if (keep(b)) x = batch(b);
        }
        const int64_t first = b * per, n = std::min(per, rows - first);
        bool written = x != nullptr && cursors.size() == mine.size(), failed_here = false;
        for (size_t k = 0; k < cursors.size(); k++) {
            if (!todo[k]) {
                written = false;
                continue;
            }
            const size_t i = mine[k];
            const ColumnSpec &c = p.cols[i];
            uint64_t row = static_cast<uint64_t>(first);
            try {
                test_fault("rdata_convert", b);
                test_fault("rdata_column", b * 100000 + static_cast<int64_t>(i));
                std::string err;
                if (x) {
                    ArrowOut o{x, i, companion_of[i]};
                    err = convert_rows(p, c, *cursors[k], static_cast<uint64_t>(first), static_cast<uint64_t>(n),
                                       o, &text, &row);
                } else {
                    err = convert_rows(p, c, *cursors[k], static_cast<uint64_t>(first), static_cast<uint64_t>(n),
                                       none, &text, &row);
                }
                if (!err.empty()) {
                    failed_here = true;
                    fail(i, static_cast<int64_t>(row), std::move(err));
                }
            } catch (...) {
                failed_here = true;
                fail(i, static_cast<int64_t>(row), std::string(), std::current_exception());
            }
        }
        if (failed_here) {
            written = false;
            test_sync("rdata_failed", b); /* tests: hold this accounting back (G5) */
        }
        std::lock_guard<std::mutex> lk(mu);
        done(b, written);
    }
}

int64_t batch_rows_for(int64_t row_bytes) {
    constexpr int64_t kRowGroup = 122880, kVector = 2048, kBatchBytes = int64_t(64) << 20;
    int64_t rows = std::min(kRowGroup, std::max<int64_t>(1, kBatchBytes / std::max<int64_t>(1, row_bytes)));
    if (rows >= kVector) rows = rows / kVector * kVector;
    return rows;
}

RdataArrowSource::RdataArrowSource(std::shared_ptr<const Plan> plan, int threads, int64_t batch_rows)
    : st_(std::make_unique<State>()) {
    State &s = *st_;
    s.plan = std::move(plan);
    const Plan &p = *s.plan;
    s.threads = std::max(1, threads);
    s.workers = static_cast<int>(std::max<size_t>(1, std::min<size_t>(static_cast<size_t>(s.threads), p.cols.size())));
    s.rows = p.nrow;
    s.companion_of.assign(p.cols.size(), -1);
    for (const auto &c : p.cols) {
        switch (c.kind) {
        case OutKind::Boolean: s.kinds.push_back(Col::Boolean); break;
        case OutKind::Integer: s.kinds.push_back(Col::Integer); break;
        case OutKind::BigInt: s.kinds.push_back(Col::BigInt); break;
        case OutKind::Date: s.kinds.push_back(Col::Date); break;
        case OutKind::Timestamp: s.kinds.push_back(Col::Timestamp); break;
        case OutKind::Time: s.kinds.push_back(Col::Time); break;
        case OutKind::Varchar: s.kinds.push_back(Col::Text); break;
        default: s.kinds.push_back(Col::Double);
        }
        s.names.push_back(c.engine_name);
    }
    for (size_t i = 0; i < p.cols.size(); i++) {
        if (!p.cols[i].companion) continue;
        s.companion_of[i] = static_cast<int>(s.kinds.size());
        s.kinds.push_back(Col::Code);
        s.names.push_back(p.cols[i].companion_engine_name);
    }
    /* rows per batch: one row group of the engine's default, fewer for a wide
     * frame so a batch stays within 64 MB (whole vectors of 2048 down to 2048
     * rows, then any number down to one row: A2 — the first error no longer
     * depends on the batch size, F12) */
    int64_t row_bytes = 0;
    for (size_t c = 0; c < s.kinds.size(); c++)
        row_bytes += s.kinds[c] == Col::Text ? 8 + static_cast<int64_t>(c < p.cols.size() ? p.cols[c].max_bytes : 0)
                                             : std::max(1, width_of(s.kinds[c]));
    row_bytes = std::max<int64_t>(1, row_bytes);
    int64_t per = batch_rows_for(row_bytes);
    if (const char *e = std::getenv("PARQIT_TEST_RDATA_BATCH_ROWS")) {
        char *end = nullptr;
        const long long v = std::strtoll(e, &end, 10);
        if (end != e && *end == '\0' && v > 0) per = v;
    } else if (batch_rows > 0) {
        per = batch_rows;
    }
    s.per = per;
    s.nbatches = s.rows > 0 ? s.rows / per + (s.rows % per != 0) : 0;
    const int64_t batch_bytes = row_bytes > std::numeric_limits<int64_t>::max() / per
                                    ? std::numeric_limits<int64_t>::max()
                                    : row_bytes * per;
    s.cap = std::max<int64_t>(2, std::min<int64_t>(2 * s.threads, (int64_t(1) << 30) / batch_bytes));
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
}

RdataArrowSource::~RdataArrowSource() { st_->stop(); }

void RdataArrowSource::start() {
    State &s = *st_;
    const Plan &p = *s.plan;
    uint64_t size = 0;
    long long mtime = 0;
    std::lock_guard<std::mutex> lk(s.mu);
    s.started = true;
    if (!file_identity(p.path, &size, &mtime) || size != p.file_size || mtime != p.file_mtime) {
        s.error = "the R data file " + p.path + " changed while it was being converted; convert it again";
        s.err_key = std::make_tuple(0, 0, 0);
        s.failed = true;
        return;
    }
    /* LIFE-018: a worker that cannot be created is a loud failure, after the
     * created ones are stopped and joined (finish) */
    try {
        for (int w = 0; w < s.workers; w++) {
            test_fault("rdata_thread", w);
            s.pool.emplace_back([&s, w] { s.work(w); });
            s.running++;
            s.created++;
        }
    } catch (const std::exception &e) {
        s.error = std::string("could not start the R conversion workers: ") + e.what();
        s.error_ex = nullptr;
        s.err_key = std::make_tuple(0, 0, 0);
        s.failed = true;
        s.aborted = true;
        s.cv.notify_all();
    }
}

ArrowArrayStream *RdataArrowSource::stream() { return &st_->stream; }

int64_t RdataArrowSource::batches() const {
    std::lock_guard<std::mutex> lk(st_->mu);
    return st_->next_out;
}

int RdataArrowSource::workers() const {
    std::lock_guard<std::mutex> lk(st_->mu);
    return st_->created;
}

int64_t RdataArrowSource::peak_batches() const {
    std::lock_guard<std::mutex> lk(st_->mu);
    return st_->peak;
}

int64_t RdataArrowSource::max_batches() const { return st_->cap; }

/* stop and join the workers and free every batch the engine did not take */
void RdataArrowSource::State::stop() noexcept {
    {
        std::lock_guard<std::mutex> lk(mu);
        if (running > 0) aborted = true; /* the engine stopped reading */
        cv.notify_all();
    }
    for (std::thread &t : pool)
        if (t.joinable()) t.join();
    pool.clear();
    std::lock_guard<std::mutex> lk(mu);
    for (auto &kv : filling) delete kv.second;
    for (auto &kv : ready) delete kv.second;
    filling.clear();
    ready.clear();
}

std::string RdataArrowSource::finish() {
    State &s = *st_;
    s.stop();
    std::lock_guard<std::mutex> lk(s.mu);
    return s.failed ? s.message() : std::string();
}

} // namespace rdata
} // namespace parqit
