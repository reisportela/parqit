/* Engine-capability gate for the streaming fetch used by the Stata fill.
 *
 * cmd_use_fetch drains its SELECT through Session::query_streaming
 * (duckdb_prepare -> duckdb_pending_prepared_streaming -> duckdb_execute_pending
 * -> duckdb_fetch_chunk) so the engine's chunks reach the per-cell SF_vstore
 * fill directly, without a materialised result collection and its per-chunk
 * copy-out in between. duckdb.h:2308 marks
 * duckdb_pending_prepared_streaming deprecated, so — exactly like
 * test_arrow_copy_bench.cpp pins duckdb_arrow_array_scan — these always-on
 * tests pin its presence AND its semantics: identical values and identical
 * ORDER BY order as the materialised path, an error raised deep in the stream
 * surfaced through duckdb_result_error, and a stream abandoned mid-way leaving
 * a healthy connection behind. A DuckDB bump that drops or changes any of it
 * fails HERE, before any Stata user is affected.
 */
#include "doctest.h"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "duckdb.h"
#include "engine/session.hpp"
#include "engine/typemap.hpp"
#include "test_tmp.hpp"

using parqit::ColumnPlan;
using parqit::Session;
using parqit::StType;
using parqit::Transfer;

namespace {

/* Drains a result with duckdb_fetch_chunk, collecting column 0 (BIGINT) and,
 * when want_second is set, column 1. Returns the number of rows fetched and
 * reports the result's error text (empty when there is none) — a NULL chunk is
 * end-of-stream OR an error set on the result, so the error must be read
 * before the result is destroyed (capi/stream-c.cpp:17-37). */
int64_t drain_bigints(duckdb_result *res, std::vector<int64_t> *col0,
                      std::vector<int64_t> *col1, std::string *err) {
    int64_t rows = 0;
    for (;;) {
        duckdb_data_chunk chunk = duckdb_fetch_chunk(*res); /* duckdb.h:5388 */
        if (!chunk) break;
        idx_t n = duckdb_data_chunk_get_size(chunk);
        duckdb_vector v0 = duckdb_data_chunk_get_vector(chunk, 0);
        auto *d0 = static_cast<int64_t *>(duckdb_vector_get_data(v0));
        int64_t *d1 = nullptr;
        if (col1) {
            duckdb_vector v1 = duckdb_data_chunk_get_vector(chunk, 1);
            d1 = static_cast<int64_t *>(duckdb_vector_get_data(v1));
        }
        for (idx_t r = 0; r < n; r++) {
            if (col0) col0->push_back(d0[r]);
            if (col1 && d1) col1->push_back(d1[r]);
        }
        rows += static_cast<int64_t>(n);
        duckdb_destroy_data_chunk(&chunk);
    }
    const char *e = duckdb_result_error(res); /* duckdb.h:1368 */
    if (err) *err = e ? e : "";
    return rows;
}

/* The fixed session singleton the whole unit-test binary shares. */
Session &open_session() {
    Session &s = Session::instance();
    s.set_default_temp_dir(parqit_test::tmp_path("_parqit_stream_spill"));
    REQUIRE(s.ensure_open());
    return s;
}

/* range() names its single column "range", hence the t(i) table alias. */
const char *kOrdered = "SELECT i, i * 2 AS j FROM range(300000) t(i) ORDER BY i DESC";
const char *kRows = "SELECT i FROM range(300000) t(i) ORDER BY i";

} // namespace

/* 1-3. The streaming result must carry EVERY row, in the query's order, for a
 * multi-threaded ORDER BY and for a single-threaded one, and must agree
 * cell-for-cell with the materialised duckdb_query path (2). */
TEST_CASE("streaming fetch matches the materialised result exactly") {
    Session &s = open_session();
    std::string err;
    REQUIRE(s.exec("SET threads = 4", &err));

    std::vector<int64_t> si, sj;
    duckdb_result sres;
    REQUIRE(s.query_streaming(kOrdered, &sres, &err));
    CHECK(static_cast<int>(duckdb_column_count(&sres)) == 2);
    CHECK(duckdb_column_type(&sres, 0) == DUCKDB_TYPE_BIGINT);
    std::string serr;
    int64_t srows = drain_bigints(&sres, &si, &sj, &serr);
    duckdb_destroy_result(&sres);
    CHECK(serr.empty());
    REQUIRE(srows == 300000);
    REQUIRE(si.size() == 300000u);
    bool values_ok = true, pairs_ok = true;
    for (int64_t r = 0; r < 300000; r++) {
        if (si[static_cast<size_t>(r)] != 299999 - r) values_ok = false;
        if (sj[static_cast<size_t>(r)] != 2 * si[static_cast<size_t>(r)]) pairs_ok = false;
    }
    CHECK(values_ok);
    CHECK(pairs_ok);

    std::vector<int64_t> mi, mj;
    duckdb_result mres;
    REQUIRE(s.query(kOrdered, &mres, &err));
    std::string merr;
    int64_t mrows = drain_bigints(&mres, &mi, &mj, &merr);
    duckdb_destroy_result(&mres);
    CHECK(merr.empty());
    REQUIRE(mrows == srows);
    CHECK(mi == si);
    CHECK(mj == sj);

    /* 3. single-threaded collectors */
    REQUIRE(s.exec("SET threads = 1", &err));
    std::vector<int64_t> ti;
    duckdb_result tres;
    REQUIRE(s.query_streaming(kOrdered, &tres, &err));
    std::string terr;
    int64_t trows = drain_bigints(&tres, &ti, nullptr, &terr);
    duckdb_destroy_result(&tres);
    CHECK(terr.empty());
    CHECK(trows == 300000);
    CHECK(ti == si);

    /* other test cases share the session singleton: give the threads back */
    REQUIRE(s.exec("RESET threads", &err));
}

/* 4. A result far smaller than the streaming buffer: one chunk, then NULL,
 * and no error. */
TEST_CASE("streaming fetch of a one-row result ends cleanly") {
    Session &s = open_session();
    std::string err;
    duckdb_result res;
    REQUIRE(s.query_streaming("SELECT 1::BIGINT AS x", &res, &err));
    std::vector<int64_t> x;
    std::string serr;
    int64_t rows = drain_bigints(&res, &x, nullptr, &serr);
    duckdb_destroy_result(&res);
    CHECK(serr.empty());
    REQUIRE(rows == 1);
    CHECK(x[0] == 1);
}

/* 5. An error raised deep inside the stream must reach the caller with the
 * engine's own text, never as a silent short read. error() is a core scalar
 * function (function/scalar/generic/error.cpp:27). Which side reports it is
 * inherently racy and BOTH are loud, so both are accepted here:
 *   - the executor runs tasks until the collector has a chunk, so a worker can
 *     raise before duckdb_execute_pending returns -> query_streaming fails with
 *     the message (cmd_use_fetch reports that through its existing error
 *     branch);
 *   - otherwise the stream starts and the error arrives as a NULL chunk with
 *     the text set on the result (capi/stream-c.cpp:31-35), which is what the
 *     fill's post-loop duckdb_result_error check catches.
 * No minimum row count is asserted: 0 rows fetched is legitimate. The
 * connection must stay usable either way. */
TEST_CASE("an error mid-stream is reported with the engine's text") {
    Session &s = open_session();
    std::string err, reported;
    duckdb_result res;
    const char *sql = "SELECT CASE WHEN i = 250000 THEN error('parqit-test-boom') "
                      "ELSE i END AS v FROM range(300000) t(i)";
    if (s.query_streaming(sql, &res, &err)) {
        int64_t rows = drain_bigints(&res, nullptr, nullptr, &reported);
        duckdb_destroy_result(&res);
        CHECK(rows < 300000);
    } else {
        reported = err;
    }
    CHECK(reported.find("parqit-test-boom") != std::string::npos);

    std::string v, e;
    REQUIRE(s.query_scalar("SELECT 1", &v, &e));
    CHECK(v == "1");
}

/* 6. A stream destroyed before end-of-stream leaves parked tasks behind until
 * the next statement on the connection cancels them (client_context.cpp:689-693
 * -> 311-318). The trivial statement must return (a hang fails as a test
 * timeout), and the connection must then serve a fresh streaming query in full.
 * The 1 MB variant is the one that really parks producers: BufferedData reads
 * streaming_buffer_size per query (buffered_data.cpp:7-10), so at 1 MB the
 * 300k-row scan blocks its sinks long before the second chunk is consumed. */
TEST_CASE("abandoning a stream mid-way leaves the connection healthy") {
    Session &s = open_session();
    std::string err;

    for (const char *buf : {"1MB", "64MB"}) {
        REQUIRE(s.exec(std::string("SET streaming_buffer_size = '") + buf + "'", &err));
        duckdb_result res;
        REQUIRE(s.query_streaming(kRows, &res, &err));
        for (int c = 0; c < 2; c++) {
            duckdb_data_chunk chunk = duckdb_fetch_chunk(res);
            REQUIRE(chunk != nullptr);
            duckdb_destroy_data_chunk(&chunk);
        }
        duckdb_destroy_result(&res);
        REQUIRE(s.exec("SELECT 1", &err)); /* forces the cancel of the abandoned executor */

        duckdb_result again;
        REQUIRE(s.query_streaming(kRows, &again, &err));
        std::string serr;
        int64_t rows = drain_bigints(&again, nullptr, nullptr, &serr);
        duckdb_destroy_result(&again);
        CHECK(serr.empty());
        CHECK(rows == 300000);
    }
    /* Hand the rest of the binary parqit's own default back without naming it:
     * a reopened session re-applies whatever ensure_open() sets. */
    s.close();
    REQUIRE(s.ensure_open());
}

/* duckdb_query runs a raw statement; query_streaming runs a PREPARED one, and a
 * prepared statement is where '?' and '$name' would become bind parameters. A
 * Parquet path or a column name containing them reaches this SQL inside quotes,
 * so it must stay literal text — otherwise duckdb_execute_pending would refuse
 * the statement for missing parameters and a Hive path with a '$' in it would
 * stop loading. */
TEST_CASE("dollar and question marks inside quotes are not bind parameters") {
    Session &s = open_session();
    std::string err;
    duckdb_result res;
    REQUIRE(s.query_streaming(
        "SELECT \"a$1\", '$2 ? $x' AS s FROM (SELECT 42::BIGINT AS \"a$1\")",
        &res, &err));
    duckdb_data_chunk chunk = duckdb_fetch_chunk(res);
    REQUIRE(chunk != nullptr);
    CHECK(duckdb_data_chunk_get_size(chunk) == 1);
    auto *d0 = static_cast<int64_t *>(
        duckdb_vector_get_data(duckdb_data_chunk_get_vector(chunk, 0)));
    CHECK(d0[0] == 42);
    duckdb_vector v1 = duckdb_data_chunk_get_vector(chunk, 1);
    auto *s1 = static_cast<duckdb_string_t *>(duckdb_vector_get_data(v1));
    CHECK(std::string(duckdb_string_t_data(&s1[0]),
                      duckdb_string_t_length(s1[0])) == "$2 ? $x");
    duckdb_destroy_data_chunk(&chunk);
    const char *e = duckdb_result_error(&res);
    CHECK(e == nullptr);
    duckdb_destroy_result(&res);
    std::string v, qe;
    REQUIRE(s.query_scalar("SELECT 1", &v, &qe));
}

/* The per-fetch buffer sizing rests on estimate_transfer_bytes: pure
 * arithmetic over the manifest, so it is pinned exactly here. Per column:
 * width*8 + 1 (the validity bit) eighths of a byte per row; the total carries
 * 25% chunk-capacity slack. */
namespace {
ColumnPlan plan_of(Transfer t, StType st = StType::Double, int str_bytes = 0) {
    ColumnPlan p;
    p.transfer = t;
    p.stata_type = st;
    p.str_bytes = str_bytes;
    return p;
}
/* what the estimator must return for one column of `w` bytes over `n` rows */
long long expect_one(long long w, long long n) { return (n * (w * 8 + 1) / 8) * 5 / 4; }
} // namespace

TEST_CASE("the streaming-buffer size estimate is exact over a manifest") {
    using parqit::estimate_transfer_bytes;

    /* fixed-width transfer types */
    CHECK(estimate_transfer_bytes({plan_of(Transfer::Int8, StType::Byte)}, 1000) ==
          expect_one(1, 1000));
    CHECK(estimate_transfer_bytes({plan_of(Transfer::Int16, StType::Int)}, 1000) ==
          expect_one(2, 1000));
    CHECK(estimate_transfer_bytes({plan_of(Transfer::Int32, StType::Long)}, 1000) ==
          expect_one(4, 1000));
    CHECK(estimate_transfer_bytes({plan_of(Transfer::Date32, StType::Long)}, 1000) ==
          expect_one(4, 1000));
    CHECK(estimate_transfer_bytes({plan_of(Transfer::Float32, StType::Float)}, 1000) ==
          expect_one(4, 1000));
    CHECK(estimate_transfer_bytes({plan_of(Transfer::Float64, StType::Double)}, 1000) ==
          expect_one(8, 1000));
    CHECK(estimate_transfer_bytes({plan_of(Transfer::TimestampUs, StType::Double)},
                                  1000) == expect_one(8, 1000));

    /* strings: <=12 bytes are inlined in the 16-byte duckdb_string_t, longer
     * ones also occupy their own bytes; a strL carries no width and is charged
     * its floor, kStataStrMax + 1 */
    CHECK(estimate_transfer_bytes({plan_of(Transfer::Utf8, StType::Str, 8)}, 1000) ==
          expect_one(16, 1000));
    CHECK(estimate_transfer_bytes({plan_of(Transfer::Utf8, StType::Str, 12)}, 1000) ==
          expect_one(16, 1000));
    CHECK(estimate_transfer_bytes({plan_of(Transfer::Utf8, StType::Str, 13)}, 1000) ==
          expect_one(29, 1000));
    CHECK(estimate_transfer_bytes({plan_of(Transfer::Utf8, StType::StrL, 0)}, 1000) ==
          expect_one(16 + parqit::kStataStrMax + 1, 1000));

    /* several columns add up; a dropped column contributes nothing */
    std::vector<ColumnPlan> mixed = {plan_of(Transfer::Int32, StType::Long),
                                     plan_of(Transfer::Float64, StType::Double)};
    CHECK(estimate_transfer_bytes(mixed, 10000) ==
          (10000LL * ((4 * 8 + 1) + (8 * 8 + 1)) / 8) * 5 / 4);
    ColumnPlan gone = plan_of(Transfer::Float64, StType::Double);
    gone.dropped = true;
    mixed.push_back(gone);
    CHECK(estimate_transfer_bytes(mixed, 10000) ==
          (10000LL * ((4 * 8 + 1) + (8 * 8 + 1)) / 8) * 5 / 4);

    /* degenerate inputs */
    CHECK(estimate_transfer_bytes({}, 1000000) == 0);
    CHECK(estimate_transfer_bytes({plan_of(Transfer::Float64, StType::Double)}, 0) == 0);
    CHECK(estimate_transfer_bytes({plan_of(Transfer::Float64, StType::Double)}, -5) == 0);
    CHECK(estimate_transfer_bytes({gone}, 1000000) == 0);

    /* saturates instead of overflowing: 2^62 rows of a double would not fit */
    const long long huge = estimate_transfer_bytes(
        {plan_of(Transfer::Float64, StType::Double)}, 4611686018427387904LL);
    CHECK(huge > 0);
    CHECK(huge <= 9223372036854775807LL);
}

/* A statement that cannot even be prepared fails loudly and leaves the session
 * usable — the caller's error branch depends on the message, not on rc alone. */
TEST_CASE("a streaming query that cannot bind fails loudly") {
    Session &s = open_session();
    std::string err;
    duckdb_result res;
    CHECK_FALSE(s.query_streaming("SELECT no_such_column FROM range(3)", &res, &err));
    CHECK_FALSE(err.empty());
    std::string v, e;
    REQUIRE(s.query_scalar("SELECT 1", &v, &e));
    CHECK(v == "1");
}
