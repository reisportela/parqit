/* Engine-capability gate + perf microbenchmark for the Arrow-scan save path.
 *
 * cmd_save_data assembles each column as one Arrow array and COPYs straight from
 * a registered Arrow scan (duckdb_arrow_array_scan), which is ~2x faster on the
 * write assembly than staging through a DuckDB temp table. That API is marked
 * deprecated in DuckDB but is present and correct in the pinned 1.5.x; the
 * always-on capability test below pins its behaviour so a DuckDB upgrade that
 * drops or changes it fails HERE, before any Stata user can be affected (the
 * same discipline as test_session.cpp). PARQIT_SAVE_NOARROW selects the staged
 * fallback at run time, so a break is never silent data loss — only slower.
 *
 * The 10M-row A/B microbenchmark is separate and gated on PARQIT_ARROW_BENCH:
 *   PARQIT_ARROW_BENCH=1 ./parqit_tests --test-case="arrow-copy*" --no-skip
 */
#include "doctest.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "abi.h"
#include "duckdb.h"
#include "engine/session.hpp"

using parqit::Session;

namespace {
void noop_release_schema(ArrowSchema *s) { s->release = nullptr; }
void noop_release_array(ArrowArray *a) { a->release = nullptr; }

/* Portable scratch path — /tmp is Unix-only; on Windows use %TEMP% (mirrors
 * test_session.cpp). DuckDB accepts the native separators in SQL literals. */
std::string tmp_path(const char *name) {
#ifdef _WIN32
    const char *base = getenv("TEMP");
    std::string dir = base ? base : ".";
    return dir + "\\" + name;
#else
    return std::string("/tmp/") + name;
#endif
}
} // namespace

/* Pins duckdb_arrow_array_scan: a zero-copy Arrow struct array (one nullable
 * int32 column + one utf8 column) registered as a view and COPYd to Parquet
 * must read back with exact values and the null preserved. If a DuckDB bump
 * breaks the deprecated API or changes its semantics, this fails loudly. */
TEST_CASE("arrow ingestion capability (save path)") {
    Session &s = Session::instance();
    s.set_default_temp_dir(tmp_path("_parqit_arrowcap_spill"));
    REQUIRE(s.ensure_open());
    duckdb_connection con = s.con();
    const std::string outf = tmp_path("parqit_arrowcap.parquet");
    const std::string rp = "read_parquet('" + outf + "')";

    const int64_t N = 3;
    int32_t ints[3] = {10, 0 /*null*/, 30};
    uint8_t ival[1] = {0b00000101}; /* rows 0 and 2 valid, row 1 null */
    int32_t offs[4] = {0, 1, 3, 6};
    char bytes[6] = {'a', 'b', 'b', 'c', 'c', 'c'};

    ArrowSchema ci{};
    ci.format = "i";
    ci.name = "n";
    ci.flags = 2; /* nullable */
    ci.release = noop_release_schema;
    ArrowSchema cs{};
    cs.format = "u";
    cs.name = "s";
    cs.flags = 2;
    cs.release = noop_release_schema;
    ArrowSchema *child_s[2] = {&ci, &cs};
    ArrowSchema st{};
    st.format = "+s";
    st.name = "";
    st.n_children = 2;
    st.children = child_s;
    st.release = noop_release_schema;

    const void *ibuf[2] = {ival, ints};
    ArrowArray ai{};
    ai.length = N;
    ai.null_count = 1;
    ai.n_buffers = 2;
    ai.buffers = ibuf;
    ai.release = noop_release_array;
    const void *sbuf[3] = {nullptr, offs, bytes};
    ArrowArray as{};
    as.length = N;
    as.null_count = 0;
    as.n_buffers = 3;
    as.buffers = sbuf;
    as.release = noop_release_array;
    ArrowArray *child_a[2] = {&ai, &as};
    const void *stbuf[1] = {nullptr};
    ArrowArray sta{};
    sta.length = N;
    sta.n_buffers = 1;
    sta.buffers = stbuf;
    sta.n_children = 2;
    sta.children = child_a;
    sta.release = noop_release_array;

    duckdb_arrow_stream stream = nullptr;
    REQUIRE(duckdb_arrow_array_scan(
                con, "cap_view", reinterpret_cast<duckdb_arrow_schema>(&st),
                reinterpret_cast<duckdb_arrow_array>(&sta),
                &stream) == DuckDBSuccess);
    std::string err;
    REQUIRE(s.exec("COPY (SELECT * FROM cap_view) TO '" + outf +
                       "' (FORMAT parquet)",
                   &err));
    if (stream) duckdb_destroy_arrow_stream(&stream);

    std::string v, e;
    REQUIRE(s.query_scalar("SELECT count(*) FROM " + rp, &v, &e));
    CHECK(v == "3");
    REQUIRE(s.query_scalar("SELECT sum(n) FROM " + rp, &v, &e));
    CHECK(v == "40"); /* 10 + null + 30 */
    REQUIRE(s.query_scalar("SELECT count(*) FROM " + rp + " WHERE n IS NULL", &v,
                           &e));
    CHECK(v == "1");
    REQUIRE(s.query_scalar("SELECT string_agg(s, '|' ORDER BY s) FROM " + rp, &v,
                           &e));
    CHECK(v == "a|bb|ccc");
}

/* ------------------------------------------------------------------ bench -- */

using clk = std::chrono::steady_clock;
static double secs(clk::time_point a, clk::time_point b) {
    return std::chrono::duration<double>(b - a).count();
}

TEST_CASE("arrow-copy write-side microbenchmark" * doctest::skip(true)) {
    if (!std::getenv("PARQIT_ARROW_BENCH")) return;

    const int64_t N = 10'000'000;
    std::vector<int32_t> offsets(N + 1);
    std::vector<char> bytes;
    bytes.reserve(static_cast<size_t>(N) * 12);
    offsets[0] = 0;
    for (int64_t i = 0; i < N; i++) {
        std::string v = "sector_" + std::to_string(i % 200);
        bytes.insert(bytes.end(), v.begin(), v.end());
        offsets[i + 1] = static_cast<int32_t>(bytes.size());
    }

    Session &s = Session::instance();
    s.set_default_temp_dir(tmp_path("_parqit_arrowbench_spill"));
    const std::string fa = tmp_path("bench_a.parquet");
    const std::string fb = tmp_path("bench_b.parquet");
    REQUIRE(s.ensure_open());
    duckdb_connection con = s.con();

    double tA = 0;
    {
        std::string err;
        REQUIRE(s.exec("DROP TABLE IF EXISTS bench_a", &err));
        REQUIRE(s.exec("CREATE TEMP TABLE bench_a (s VARCHAR)", &err));
        auto t0 = clk::now();
        duckdb_appender app = nullptr;
        REQUIRE(duckdb_appender_create_ext(con, "temp", "main", "bench_a", &app) ==
                DuckDBSuccess);
        duckdb_logical_type vt = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
        duckdb_data_chunk ch = duckdb_create_data_chunk(&vt, 1);
        const idx_t CAP = duckdb_vector_size();
        idx_t fill = 0;
        for (int64_t i = 0; i < N; i++) {
            duckdb_vector vec = duckdb_data_chunk_get_vector(ch, 0);
            int32_t lo = offsets[i], hi = offsets[i + 1];
            duckdb_vector_assign_string_element_len(vec, fill, bytes.data() + lo,
                                                    hi - lo);
            if (++fill == CAP) {
                duckdb_data_chunk_set_size(ch, fill);
                REQUIRE(duckdb_append_data_chunk(app, ch) == DuckDBSuccess);
                duckdb_data_chunk_reset(ch);
                fill = 0;
            }
        }
        if (fill) {
            duckdb_data_chunk_set_size(ch, fill);
            REQUIRE(duckdb_append_data_chunk(app, ch) == DuckDBSuccess);
        }
        duckdb_destroy_data_chunk(&ch);
        duckdb_destroy_logical_type(&vt);
        REQUIRE(duckdb_appender_destroy(&app) == DuckDBSuccess);
        REQUIRE(s.exec("COPY (SELECT * FROM bench_a) TO '" + fa +
                           "' (FORMAT parquet)",
                       &err));
        tA = secs(t0, clk::now());
    }

    double tB = 0;
    {
        std::string err;
        auto t0 = clk::now();
        ArrowSchema child_s{};
        child_s.format = "u";
        child_s.name = "s";
        child_s.release = noop_release_schema;
        ArrowSchema *child_s_ptrs[1] = {&child_s};
        ArrowSchema struct_s{};
        struct_s.format = "+s";
        struct_s.name = "";
        struct_s.n_children = 1;
        struct_s.children = child_s_ptrs;
        struct_s.release = noop_release_schema;
        const void *child_buffers[3] = {nullptr, offsets.data(), bytes.data()};
        ArrowArray child_a{};
        child_a.length = N;
        child_a.n_buffers = 3;
        child_a.buffers = child_buffers;
        child_a.release = noop_release_array;
        ArrowArray *child_a_ptrs[1] = {&child_a};
        const void *struct_buffers[1] = {nullptr};
        ArrowArray struct_a{};
        struct_a.length = N;
        struct_a.n_buffers = 1;
        struct_a.n_children = 1;
        struct_a.buffers = struct_buffers;
        struct_a.children = child_a_ptrs;
        struct_a.release = noop_release_array;
        duckdb_arrow_stream stream = nullptr;
        REQUIRE(duckdb_arrow_array_scan(
                    con, "bench_b",
                    reinterpret_cast<duckdb_arrow_schema>(&struct_s),
                    reinterpret_cast<duckdb_arrow_array>(&struct_a),
                    &stream) == DuckDBSuccess);
        REQUIRE(s.exec("COPY (SELECT * FROM bench_b) TO '" + fb +
                           "' (FORMAT parquet)",
                       &err));
        if (stream) duckdb_destroy_arrow_stream(&stream);
        tB = secs(t0, clk::now());
    }
    std::printf("\n[ARROW-BENCH] strings=%lld  A(temp-table)=%.3fs  "
                "B(arrow-scan)=%.3fs  B/A=%.2f\n",
                static_cast<long long>(N), tA, tB, tB / tA);
}
