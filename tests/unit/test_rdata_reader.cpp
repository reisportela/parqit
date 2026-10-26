/* R-READ-1: the native reader of R data files (engine/rdata_reader,
 * rdata_plan, rdata_table). The streams here are written byte by byte by a
 * small writer that follows R's serialization format (src/main/serialize.c,
 * R 4.6.0), so every structure the reader must handle — attributes,
 * references, pairlists, byte code, environments, ALTREP vectors, long
 * vectors, encodings, gzip and zstd — and every malformation it must refuse
 * can be pinned without R. The Stata verify tests compare the whole path
 * against R itself (readRDS / load) on files R wrote. */
#include "doctest.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <new>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "duckdb.h"
#include "engine/rdata_convert.hpp"
#include "engine/rdata_parallel.hpp"
#include "engine/rdata_plan.hpp"
#include "engine/rdata_reader.hpp"
#include "engine/rdata_table.hpp"
#include "engine/session.hpp"
#include "engine/test_fault.hpp"
#include "miniz.hpp"
#include "test_env.hpp"
#include "test_tmp.hpp"
#include "zstd.h"

using namespace parqit::rdata;

namespace {

bool contains(const std::string &s, const std::string &part) { return s.find(part) != std::string::npos; }

constexpr int kAscii = 64, kUtf8 = 8, kLatin1 = 4;

/* an R serialization stream, XDR (big-endian) */
struct X {
    std::string b;
    void i32(int32_t v) {
        const uint32_t u = static_cast<uint32_t>(v);
        const char c[4] = {char(u >> 24), char(u >> 16), char(u >> 8), char(u)};
        b.append(c, 4);
    }
    void bits(uint64_t u) {
        for (int k = 7; k >= 0; k--) b.push_back(static_cast<char>(u >> (8 * k)));
    }
    void f64(double d) {
        uint64_t u;
        std::memcpy(&u, &d, 8);
        bits(u);
    }
    void flags(int type, int attr = 0, int tag = 0, int obj = 0, int levels = 0) {
        i32(type | (obj << 8) | (attr << 9) | (tag << 10) | (levels << 12));
    }
    void header(int version = 3, const std::string &native = "UTF-8") {
        b += "X\n";
        i32(version);
        i32(0x040600); /* R 4.6.0 */
        i32(version == 3 ? 0x030500 : 0x020300);
        if (version == 3) {
            i32(static_cast<int32_t>(native.size()));
            b += native;
        }
    }
    void chr(const std::string &s, int levels = kAscii) {
        flags(CHARSXP, 0, 0, 0, levels);
        i32(static_cast<int32_t>(s.size()));
        b += s;
    }
    void na_chr() {
        flags(CHARSXP);
        i32(-1);
    }
    void sym(const std::string &s) {
        flags(SYMSXP);
        chr(s);
    }
    void ref(int index) { i32((index << 8) | REFSXP); }
    void nil() { i32(NILVALUE_SXP); }
    void ints(const std::vector<int32_t> &v, int attr = 0, int obj = 0) {
        flags(INTSXP, attr, 0, obj);
        i32(static_cast<int32_t>(v.size()));
        for (int32_t x : v) i32(x);
    }
    void lgls(const std::vector<int32_t> &v) {
        flags(LGLSXP);
        i32(static_cast<int32_t>(v.size()));
        for (int32_t x : v) i32(x);
    }
    void reals(const std::vector<double> &v, int attr = 0, int obj = 0) {
        flags(REALSXP, attr, 0, obj);
        i32(static_cast<int32_t>(v.size()));
        for (double x : v) f64(x);
    }
    void strs(const std::vector<std::string> &v, int attr = 0) {
        flags(STRSXP, attr);
        i32(static_cast<int32_t>(v.size()));
        for (const auto &s : v) s == "<NA>" ? na_chr() : chr(s, levels_for(s));
    }
    static int levels_for(const std::string &s) {
        for (unsigned char c : s)
            if (c >= 0x80) return kUtf8;
        return kAscii;
    }
    /* an attribute cell (tag given as a new symbol); the value follows */
    void cell(const std::string &tag) {
        flags(LISTSXP, 0, 1);
        sym(tag);
    }
    void class_attr(const std::vector<std::string> &cls) {
        cell("class");
        strs(cls);
    }
};

const uint64_t kNaBits = 0x7FF00000000007A2ULL;
double na_real() {
    double d;
    std::memcpy(&d, &kNaBits, 8);
    return d;
}
double tagged(char t) {
    const uint64_t u = kNaBits | (uint64_t(static_cast<unsigned char>(t)) << 32);
    double d;
    std::memcpy(&d, &u, 8);
    return d;
}

/* the column vectors of a data frame, then its names/class/row.names */
struct Col {
    std::string name;
    std::function<void(X &)> write;
};

void data_frame(X &x, const std::vector<Col> &cols, int nrow, const std::vector<std::string> &cls = {"data.frame"},
                const std::function<void(X &)> &extra_attrs = nullptr) {
    x.flags(VECSXP, 1, 0, 1);
    x.i32(static_cast<int32_t>(cols.size()));
    for (const auto &c : cols) c.write(x);
    x.cell("names");
    std::vector<std::string> names;
    for (const auto &c : cols) names.push_back(c.name);
    x.strs(names);
    if (extra_attrs) extra_attrs(x);
    x.class_attr(cls);
    x.cell("row.names");
    x.ints({INT_MIN, -nrow});
    x.nil();
}

std::string write_file(const std::string &name, const std::string &bytes) {
    const std::string path = parqit_test::tmp_path(name);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return path;
}

ReadOptions opts(const std::string &object = std::string(), const std::string &encoding = std::string()) {
    ReadOptions o;
    o.object = object;
    o.encoding = encoding;
    o.tmpdir = parqit_test::tmp_path("rdata_tmp_dir_unused");
    const std::string::size_type slash = o.tmpdir.find_last_of("/\\");
    o.tmpdir = o.tmpdir.substr(0, slash);
    return o;
}

/* the basic data frame: data.frame(i = c(1L, NA, 3L), d = c(1.5, NA, -2),
 * s = c("x", NA, "é"), l = c(TRUE, NA, FALSE)) */
std::string basic_rds(int version = 3) {
    X x;
    x.header(version);
    data_frame(x,
               {{"i", [](X &x) { x.ints({1, INT_MIN, 3}); }},
                {"d", [](X &x) { x.reals({1.5, na_real(), -2.0}); }},
                {"s", [](X &x) { x.strs({"x", "<NA>", "\xc3\xa9"}); }},
                {"l", [](X &x) { x.lgls({1, INT_MIN, 0}); }}},
               3);
    return x.b;
}

std::string gzip_of(const std::string &raw) {
    namespace mz = duckdb_miniz;
    mz::mz_stream z;
    std::memset(&z, 0, sizeof z);
    REQUIRE(mz::mz_deflateInit2(&z, 6, MZ_DEFLATED, -MZ_DEFAULT_WINDOW_BITS, 9, 0) == mz::MZ_OK);
    std::string out = std::string("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x03", 10);
    std::vector<unsigned char> buf(raw.size() + 1024);
    z.next_in = reinterpret_cast<const unsigned char *>(raw.data());
    z.avail_in = static_cast<unsigned int>(raw.size());
    z.next_out = buf.data();
    z.avail_out = static_cast<unsigned int>(buf.size());
    REQUIRE(mz::mz_deflate(&z, mz::MZ_FINISH) == mz::MZ_STREAM_END);
    out.append(reinterpret_cast<const char *>(buf.data()), buf.size() - z.avail_out);
    mz::mz_deflateEnd(&z);
    const uint32_t crc = static_cast<uint32_t>(
        mz::mz_crc32(0, reinterpret_cast<const unsigned char *>(raw.data()), raw.size()));
    const uint32_t n = static_cast<uint32_t>(raw.size());
    for (int i = 0; i < 4; i++) out.push_back(static_cast<char>(crc >> (8 * i)));
    for (int i = 0; i < 4; i++) out.push_back(static_cast<char>(n >> (8 * i)));
    return out;
}

std::string zstd_of(const std::string &raw) {
    std::vector<char> buf(raw.size() + 1024);
    const size_t n = duckdb_zstd::ZSTD_compress(buf.data(), buf.size(), raw.data(), raw.size(), 3);
    REQUIRE_FALSE(duckdb_zstd::ZSTD_isError(n));
    return std::string(buf.data(), n);
}

std::string read_error(const std::string &path, const ReadOptions &o = opts()) {
    try {
        read_file(path, o);
    } catch (const RError &e) {
        return e.what();
    }
    return "(no error)";
}

} // namespace

TEST_CASE("R-READ-1: a data frame's columns are located and read back") {
    for (int version : {2, 3}) {
        const std::string path = write_file("rdata_basic.rds", basic_rds(version));
        Parsed p = read_file(path, opts());
        CHECK(p.info.version == version);
        CHECK_FALSE(p.info.rdata);
        CHECK(p.info.compression == Compression::None);
        CHECK(p.info.native_encoding == (version == 3 ? "UTF-8" : ""));
        CHECK(r_version_string(p.info.writer_version) == "4.6.0");
        REQUIRE(p.frame.cols.size() == 4);
        CHECK(p.frame.nrow == 3);
        CHECK(p.frame.cols[0].name == "i");
        CHECK(p.frame.cols[3].name == "l");
        for (const auto &c : p.frame.cols) CHECK(c.unsupported.empty());
        Cursor ci(p.source, p.frame.cols[0].vec, p.codec);
        CHECK(ci.next_int() == 1);
        CHECK(ci.next_int() == kNaInteger);
        CHECK(ci.next_int() == 3);
        Cursor cd(p.source, p.frame.cols[1].vec, p.codec);
        CHECK(cd.next_real() == 1.5);
        CHECK(is_na_real(cd.next_real()));
        CHECK(cd.next_real() == -2.0);
        Cursor cs(p.source, p.frame.cols[2].vec, p.codec, 64);
        std::string t;
        bool tr = true;
        CHECK(cs.next_string(&t, &tr));
        CHECK(t == "x");
        CHECK_FALSE(tr);
        CHECK_FALSE(cs.next_string(&t, &tr));
        CHECK(cs.next_string(&t, &tr));
        CHECK(t == "\xc3\xa9");
        Cursor cl(p.source, p.frame.cols[3].vec, p.codec);
        CHECK(cl.next_int() == 1);
        CHECK(cl.next_int() == kNaInteger);
        CHECK(cl.next_int() == 0);
        std::remove(path.c_str());
    }
}

TEST_CASE("R-READ-1: NA, NaN and haven's tagged NA are told apart") {
    CHECK(is_na_real(na_real()));
    CHECK(na_tag(na_real()) == 0);
    CHECK(is_na_real(tagged('a')));
    CHECK(na_tag(tagged('a')) == 'a');
    CHECK(na_tag(tagged('z')) == 'z');
    CHECK_FALSE(is_na_real(std::nan("")));
    CHECK(na_tag(std::nan("")) == 0);
    CHECK_FALSE(is_na_real(1.0));
    /* a quiet NA (arithmetic on NA sets the quiet bit) is still NA */
    const uint64_t quiet = kNaBits | (uint64_t(1) << 51);
    double q;
    std::memcpy(&q, &quiet, 8);
    CHECK(is_na_real(q));
    CHECK(na_tag(q) == 0);
}

TEST_CASE("R-READ-1: gzip and zstd files are inflated and checked") {
    const std::string raw = basic_rds();
    for (const auto &kind : {std::string("gzip"), std::string("zstd"), std::string("gzip2")}) {
        std::string bytes = kind == "zstd" ? zstd_of(raw) : gzip_of(raw);
        if (kind == "gzip2") { /* R accepts concatenated members */
            const size_t half = raw.size() / 2;
            bytes = gzip_of(raw.substr(0, half)) + gzip_of(raw.substr(half));
        }
        const std::string path = write_file("rdata_comp.rds", bytes);
        Parsed p = read_file(path, opts());
        CHECK(p.info.compression == (kind == "zstd" ? Compression::Zstd : Compression::Gzip));
        CHECK(p.frame.nrow == 3);
        CHECK(p.source->data_path() != path);
        const std::string tmp = p.source->data_path();
        {
            std::ifstream t(tmp);
            CHECK(t.good());
        }
        p = Parsed(); /* the temporary copy goes with the last owner */
        {
            std::ifstream t(tmp);
            CHECK_FALSE(t.good());
        }
        std::remove(path.c_str());
    }
    /* a damaged CRC, a damaged length, a cut file */
    std::string g = gzip_of(raw);
    std::string bad = g;
    bad[bad.size() - 8] ^= 0x01;
    std::string path = write_file("rdata_badcrc.rds", bad);
    CHECK(contains(read_error(path), "CRC-32"));
    bad = g;
    bad[bad.size() - 1] ^= 0x01;
    path = write_file("rdata_badlen.rds", bad);
    CHECK(contains(read_error(path), "length check"));
    path = write_file("rdata_cut.rds", g.substr(0, g.size() / 2));
    CHECK(contains(read_error(path), "truncated"));
    const std::string z = zstd_of(raw);
    path = write_file("rdata_cutz.rds", z.substr(0, z.size() - 3));
    CHECK(contains(read_error(path), "zstd"));
    path = write_file("rdata_trail.rds", g + "garbage");
    CHECK(contains(read_error(path), "not gzip data"));
    path = write_file("rdata_pad.rds", g + std::string(16, '\0'));
    CHECK(read_file(path, opts()).frame.nrow == 3);
    std::remove(path.c_str());
}

TEST_CASE("R-READ-1: formats parqit does not read are refused with the way out") {
    struct Case {
        std::string bytes, expect;
    };
    std::vector<Case> cases = {
        {"A\n3\n", "ASCII"},
        {"B\n\0\0\0\3", "native binary"},
        {"RDA3\nA\n", "ASCII"},
        {"RDB3\nB\n", "native binary"},
        {"RDX1\n", "before version 1.4.0"},
        {"BZh91AY&SY", "bzip2"},
        {std::string("\xfd" "7zXZ\0", 6) + "xx", "xz"},
        {"hello world, not R", "not an R data file"},
        {"", "not an R data file"},
    };
    for (const auto &c : cases) {
        const std::string path = write_file("rdata_refuse.rds", c.bytes);
        const std::string e = read_error(path);
        CHECK_MESSAGE(contains(e, c.expect), e);
        std::remove(path.c_str());
    }
    X x;
    x.header(3);
    x.b[5] = 4; /* version 4 */
    const std::string path = write_file("rdata_v4.rds", x.b);
    CHECK(contains(read_error(path), "serialization version 4"));
    std::remove(path.c_str());
}

TEST_CASE("R-READ-1: every truncation and every flipped byte fails loudly, never crashes") {
    const std::string raw = basic_rds();
    for (size_t n = 0; n < raw.size(); n++) {
        const std::string path = write_file("rdata_trunc.rds", raw.substr(0, n));
        const std::string e = read_error(path);
        CHECK_MESSAGE(e != "(no error)", "prefix of " << n << " bytes read without an error");
        std::remove(path.c_str());
    }
    for (size_t i = 0; i < raw.size(); i++) {
        std::string m = raw;
        m[i] = static_cast<char>(m[i] ^ 0x5A);
        const std::string path = write_file("rdata_flip.rds", m);
        try {
            Parsed p = read_file(path, opts());
            /* a flip that still parses must read back without surprises */
            for (const auto &c : p.frame.cols)
                if (c.unsupported.empty() && c.vec.type != NILSXP) {
                    Cursor cur(p.source, c.vec, p.codec);
                    std::string t;
                    bool tr;
                    for (uint64_t k = 0; k < c.vec.length; k++) {
                        if (c.vec.type == STRSXP) cur.next_string(&t, &tr);
                        else if (c.vec.type == REALSXP) cur.next_real();
                        else cur.next_int();
                    }
                }
        } catch (const RError &) {
        }
        std::remove(path.c_str());
    }
}

TEST_CASE("R-READ-1: references, symbols and malformed references") {
    X x;
    x.header();
    x.flags(VECSXP, 1, 0, 1);
    x.i32(1);
    x.ints({7, 8}, 1);          /* a column with attributes: */
    x.cell("label");            /* ref 1 = label */
    x.strs({"Seven"});
    x.flags(LISTSXP, 0, 1);
    x.ref(1);                   /* the tag `label` again, by reference */
    x.strs({"ignored duplicate"});
    x.nil();
    x.cell("names");            /* ref 2 */
    x.strs({"v"});
    x.cell("class");            /* ref 3 */
    x.strs({"data.frame"});
    x.cell("row.names");        /* ref 4 */
    x.ints({INT_MIN, -2});
    x.nil();
    std::string path = write_file("rdata_refs.rds", x.b);
    Parsed p = read_file(path, opts());
    REQUIRE(p.frame.cols.size() == 1);
    REQUIRE(p.frame.cols[0].attrs.size() == 2);
    CHECK(p.frame.cols[0].attrs[0].first == "label");
    CHECK(p.frame.cols[0].attrs[1].first == "label");
    /* a reference past the table */
    std::string bad = x.b;
    const std::string ref1("\x00\x00\x01\xff", 4);
    const size_t at = bad.find(ref1);
    REQUIRE(at != std::string::npos);
    bad[at + 2] = 9;
    path = write_file("rdata_badref.rds", bad);
    CHECK(contains(read_error(path), "reference to an unknown object"));
    /* an unknown item type */
    bad = x.b;
    bad[bad.size() - 1] = 99; /* the closing NILVALUE becomes type 99 */
    path = write_file("rdata_badtype.rds", bad);
    CHECK(contains(read_error(path), "unknown type 99"));
    std::remove(path.c_str());
}

TEST_CASE("R-READ-1: an .RData workspace: functions, byte code, environments and deep calls are passed over") {
    X x;
    x.b = "RDX3\n";
    x.header();
    /* object 1: f <- a byte-compiled function */
    x.flags(LISTSXP, 0, 1);
    x.sym("f");
    x.flags(CLOSXP, 0, 1);      /* tag = its environment */
    x.i32(GLOBALENV_SXP);
    x.nil();                    /* formals */
    x.flags(BCODESXP);          /* body */
    x.i32(2);                   /* its own reference table */
    x.ints({12, 1, 2, 3});      /* code */
    x.i32(4);                   /* four constants */
    x.i32(LANGSXP);             /* a call: tag, car, cdr */
    x.nil();
    x.i32(0);                   /*   car: pad + a symbol */
    x.sym("g");
    x.i32(LISTSXP);             /*   cdr: an argument list */
    x.nil();
    x.i32(0);
    x.reals({1.0});
    x.i32(0);
    x.nil();
    x.i32(BCREPDEF);            /* a shared cell, defined ... */
    x.i32(0);
    x.i32(ATTRLANGSXP);
    x.nil();                    /*   its attributes */
    x.nil();                    /*   tag */
    x.i32(BCREPREF);            /*   car: ... and referred to */
    x.i32(0);
    x.i32(0);
    x.nil();
    x.i32(BCODESXP);            /* a nested byte code */
    x.ints({12});
    x.i32(0);
    x.i32(REALSXP);             /* a plain constant */
    x.reals({2.5});
    /* object 2: e <- an environment holding itself and an external pointer */
    x.flags(LISTSXP, 0, 1);
    x.sym("e");
    x.flags(ENVSXP);
    x.i32(0);                   /* locked */
    x.i32(EMPTYENV_SXP);        /* enclosure */
    x.flags(LISTSXP, 0, 1);     /* frame: self = <itself> */
    x.sym("self");
    x.ref(4);                   /* refs: f 1, g 2, e 3, env 4 */
    x.flags(LISTSXP, 0, 1);
    x.sym("p");
    x.flags(EXTPTRSXP);
    x.nil();
    x.nil();
    x.nil();
    x.nil();                    /* hash table */
    x.nil();                    /* attributes */
    /* object 3: deep <- a call nested 200,000 levels deep */
    x.flags(LISTSXP, 0, 1);
    x.sym("deep");
    const int depth = 200000;
    for (int i = 0; i < depth; i++) x.flags(LANGSXP);
    x.sym("h");
    for (int i = 0; i < depth; i++) x.nil();
    /* object 4: the data frame */
    x.flags(LISTSXP, 0, 1);
    x.sym("df");
    data_frame(x, {{"a", [](X &x) { x.ints({5, 6}); }}}, 2);
    x.nil();
    const std::string path = write_file("rdata_ws.RData", x.b);
    Parsed p = read_file(path, opts());
    CHECK(p.info.rdata);
    CHECK(p.frame.object == "df");
    REQUIRE(p.info.objects.size() == 4);
    CHECK(p.info.objects[0].second == "a function");
    CHECK(p.info.objects[1].second == "an environment");
    CHECK(p.info.objects[2].second == "a call");
    CHECK(contains(p.info.objects[3].second, "a data frame (2 rows, 1 column)"));
    REQUIRE(p.notes.size() == 1);
    CHECK(contains(p.notes[0], "read its only data frame, df"));
    Cursor c(p.source, p.frame.cols[0].vec, p.codec);
    CHECK(c.next_int() == 5);
    CHECK(c.next_int() == 6);
    std::remove(path.c_str());
}

TEST_CASE("R-READ-1: which object is read: one data frame, object(), or a refusal naming them") {
    auto rdata = [](int nframes) {
        X x;
        x.b = "RDX2\n";
        x.header(2);
        for (int k = 0; k < nframes; k++) {
            x.flags(LISTSXP, 0, 1);
            x.sym("df" + std::to_string(k + 1));
            data_frame(x, {{"a", [k](X &x) { x.ints({k, k}); }}}, 2);
        }
        x.flags(LISTSXP, 0, 1);
        x.sym("v");
        x.reals({1, 2, 3});
        x.nil();
        return x.b;
    };
    std::string path = write_file("rdata_two.RData", rdata(2));
    std::string e = read_error(path);
    CHECK(contains(e, "holds 2 data frames (df1, df2); choose one with object()"));
    try {
        read_file(path, opts());
    } catch (const RChoiceError &) {
        CHECK(true);
    }
    Parsed p = read_file(path, opts("df2"));
    CHECK(p.frame.object == "df2");
    Cursor c(p.source, p.frame.cols[0].vec, p.codec);
    CHECK(c.next_int() == 1);
    CHECK(contains(read_error(path, opts("nope")), "holds no object of that name"));
    CHECK(contains(read_error(path, opts("v")), "is a numeric vector of length 3, not a data frame"));
    path = write_file("rdata_none.RData", rdata(0));
    CHECK(contains(read_error(path), "holds no data frame; it holds v (a numeric vector of length 3)"));
    /* an .rds holding a named list of data frames: its elements are the objects */
    X x;
    x.header();
    x.flags(VECSXP, 1);
    x.i32(2);
    data_frame(x, {{"a", [](X &x) { x.ints({1}); }}}, 1);
    x.strs({"not a frame"});
    x.cell("names");
    x.strs({"first", "second"});
    x.nil();
    path = write_file("rdata_list.rds", x.b);
    p = read_file(path, opts());
    CHECK(p.frame.object == "first");
    CHECK(contains(read_error(path, opts("second")), "not a data frame"));
    /* object() on a plain data frame .rds is a mistake */
    path = write_file("rdata_plain.rds", basic_rds());
    CHECK(contains(read_error(path, opts("x")), "holds a single data frame"));
    /* an .rds holding a plain vector */
    X v;
    v.header();
    v.reals({1});
    path = write_file("rdata_vec.rds", v.b);
    CHECK(contains(read_error(path), "holds a numeric vector of length 1, not a data frame"));
    std::remove(path.c_str());
}

TEST_CASE("R-READ-1: ALTREP columns: compact sequences, wrappers, deferred strings, unknown classes") {
    auto altrep_info = [](X &x, const std::string &cls, int type) {
        x.flags(LISTSXP);
        x.sym(cls);
        x.flags(LISTSXP);
        x.sym("base");
        x.flags(LISTSXP);
        x.ints({type});
        x.nil();
    };
    X x;
    x.header();
    x.flags(VECSXP, 1, 0, 1);
    x.i32(6);
    /* 1:3 */
    x.i32(ALTREP_SXP);
    altrep_info(x, "compact_intseq", INTSXP);
    x.reals({3, 1, 1});
    x.nil();
    /* c(3, 2, 1) as a compact real sequence */
    x.i32(ALTREP_SXP);
    altrep_info(x, "compact_realseq", REALSXP);
    x.reals({3, 3, -1});
    x.nil();
    /* a sorted-wrapper around a double vector; the wrapper holds the label */
    x.i32(ALTREP_SXP);
    altrep_info(x, "wrap_real", REALSXP);
    x.flags(LISTSXP);
    x.reals({0.5, 1.5, 2.5});
    x.ints({1, 1});
    x.flags(LISTSXP, 0, 1);
    x.sym("label");
    x.strs({"wrapped"});
    x.nil();
    /* as.character(c(10L, NA, -3L)), not yet expanded */
    x.i32(ALTREP_SXP);
    altrep_info(x, "deferred_string", STRSXP);
    x.flags(LISTSXP);
    x.ints({10, INT_MIN, -3});
    x.ints({0});
    x.nil();
    /* as.character(c(1.5, 2, 3)), not yet expanded: the numbers are carried */
    x.i32(ALTREP_SXP);
    altrep_info(x, "deferred_string", STRSXP);
    x.flags(LISTSXP);
    x.reals({1.5, 2, 3});
    x.ints({0});
    x.nil();
    /* a package's own class */
    x.i32(ALTREP_SXP);
    altrep_info(x, "vroom_dbl", REALSXP);
    x.strs({"state only vroom reads"});
    x.nil();
    x.cell("names");
    x.strs({"is", "rs", "w", "ds", "dr", "pk"});
    x.class_attr({"data.frame"});
    x.cell("row.names");
    x.ints({INT_MIN, -3});
    x.nil();
    const std::string path = write_file("rdata_altrep.rds", x.b);
    Parsed p = read_file(path, opts());
    REQUIRE(p.frame.cols.size() == 6);
    Cursor is(p.source, p.frame.cols[0].vec, p.codec);
    CHECK(is.next_int() == 1);
    CHECK(is.next_int() == 2);
    CHECK(is.next_int() == 3);
    Cursor rs(p.source, p.frame.cols[1].vec, p.codec);
    CHECK(rs.next_real() == 3);
    CHECK(rs.next_real() == 2);
    CHECK(rs.next_real() == 1);
    Cursor w(p.source, p.frame.cols[2].vec, p.codec);
    CHECK(w.next_real() == 0.5);
    CHECK(p.frame.cols[2].attrs.size() == 1);
    CHECK(p.frame.cols[2].attrs[0].first == "label");
    Cursor ds(p.source, p.frame.cols[3].vec, p.codec);
    std::string t;
    bool tr;
    CHECK(ds.next_string(&t, &tr));
    CHECK(t == "10");
    CHECK_FALSE(ds.next_string(&t, &tr));
    CHECK(ds.next_string(&t, &tr));
    CHECK(t == "-3");
    CHECK(p.frame.cols[4].unsupported.empty());
    CHECK(p.frame.cols[4].deferred_numbers);
    CHECK(p.frame.cols[4].vec.type == REALSXP);
    Cursor dr(p.source, p.frame.cols[4].vec, p.codec);
    CHECK(dr.next_real() == 1.5);
    CHECK(contains(p.frame.cols[5].unsupported, "vroom_dbl"));
    std::remove(path.c_str());
}

TEST_CASE("R-READ-1: strings are decoded by their flag or the writer's native encoding") {
    auto frame = [](const std::string &native, int levels, const std::string &bytes) {
        X x;
        x.header(3, native);
        data_frame(x,
                   {{"s", [&](X &x) {
                         x.flags(STRSXP);
                         x.i32(1);
                         x.chr(bytes, levels);
                     }}},
                   1);
        return x.b;
    };
    auto first = [](const std::string &path, const ReadOptions &o, bool *tr) {
        Parsed p = read_file(path, o);
        Cursor c(p.source, p.frame.cols[0].vec, p.codec);
        std::string t;
        c.next_string(&t, tr);
        return t;
    };
    bool tr = false;
    /* latin1-flagged é */
    std::string path = write_file("rdata_enc.rds", frame("UTF-8", kLatin1, "\xe9"));
    CHECK(first(path, opts(), &tr) == "\xc3\xa9");
    CHECK_FALSE(tr);
    /* unflagged, native latin1 */
    path = write_file("rdata_enc.rds", frame("latin1", 0, "\xe9"));
    CHECK(first(path, opts(), &tr) == "\xc3\xa9");
    CHECK_FALSE(tr);
    /* unflagged, native CP1252: the euro sign */
    path = write_file("rdata_enc.rds", frame("CP1252", 0, "\x80"));
    CHECK(first(path, opts(), &tr) == "\xe2\x82\xac");
    /* UTF-8 flag on bytes that are not UTF-8: read as windows-1252, counted */
    path = write_file("rdata_enc.rds", frame("UTF-8", kUtf8, "\xe9"));
    CHECK(first(path, opts(), &tr) == "\xc3\xa9");
    CHECK(tr);
    /* encoding() replaces the native encoding of unflagged strings only */
    path = write_file("rdata_enc.rds", frame("UTF-8", 0, "\xe9"));
    CHECK(first(path, opts("", "latin1"), &tr) == "\xc3\xa9");
    CHECK_FALSE(tr);
    CHECK(contains(read_error(path, opts("", "klingon")), "encoding(klingon)"));
    /* ENC-3: any language's native encoding (R on Windows before 4.2, or in a
     * legacy locale) */
    path = write_file("rdata_enc.rds", frame("CP932", 0, "\x82\xa0"));
    CHECK(first(path, opts(), &tr) == "\xe3\x81\x82"); /* あ */
    CHECK_FALSE(tr);
    path = write_file("rdata_enc.rds", frame("CP1251", 0, "\xcf\xf0\xe8\xe2\xe5\xf2"));
    CHECK(first(path, opts(), &tr) == "\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82");
    path = write_file("rdata_enc.rds", frame("GBK", 0, "\xc5\xae")); /* 女, also valid UTF-8 */
    CHECK(first(path, opts(), &tr) == "\xe5\xa5\xb3");
    path = write_file("rdata_enc.rds", frame("GBK", 0, "a\xd6")); /* a lead byte at the end */
    CHECK(first(path, opts(), &tr) == "a\xef\xbf\xbd");
    CHECK(tr); /* counted, never silent */
    /* a UTF-8 mark on bytes that are not UTF-8: the fallback is encoding(), when given */
    path = write_file("rdata_enc.rds", frame("UTF-8", kUtf8, "\xcf\xf0\xe8"));
    CHECK(first(path, opts("", "cp1251"), &tr) == "\xd0\x9f\xd1\x80\xd0\xb8");
    CHECK(tr);
    /* a native encoding parqit cannot decode: refused, unless the text is UTF-8 */
    path = write_file("rdata_enc.rds", frame("EUC-TW", 0, "\xa4\xa1"));
    bool refused = false;
    try {
        first(path, opts(), &tr);
    } catch (const RError &e) {
        refused = contains(e.what(), "EUC-TW");
    }
    CHECK(refused);
    std::remove(path.c_str());
}

TEST_CASE("R-READ-1: unsupported columns are named, not silently lost") {
    X x;
    x.header();
    data_frame(x,
               {{"ok", [](X &x) { x.ints({1, 2}); }},
                {"lst", [](X &x) {
                     x.flags(VECSXP);
                     x.i32(2);
                     x.ints({1});
                     x.ints({2});
                 }},
                {"cx", [](X &x) {
                     x.flags(CPLXSXP);
                     x.i32(2);
                     x.f64(1);
                     x.f64(0);
                     x.f64(2);
                     x.f64(0);
                 }},
                {"m", [](X &x) {
                     x.ints({1, 2, 3, 4}, 1);
                     x.cell("dim");
                     x.ints({2, 2});
                     x.nil();
                 }},
                {"short", [](X &x) { x.ints({1}); }}},
               2);
    const std::string path = write_file("rdata_unsup.rds", x.b);
    Parsed p = read_file(path, opts());
    REQUIRE(p.frame.cols.size() == 5);
    CHECK(p.frame.cols[0].unsupported.empty());
    CHECK(p.frame.cols[1].unsupported == "a list column");
    CHECK(p.frame.cols[2].unsupported == "complex numbers");
    CHECK(p.frame.cols[3].unsupported == "a matrix column");
    CHECK(contains(p.frame.cols[4].unsupported, "differs from the number of rows"));
    Plan plan = make_plan(path, opts());
    REQUIRE(plan.cols.size() == 1);
    REQUIRE_FALSE(plan.notes.empty());
    CHECK(contains(plan.notes[0], "lst (a list column); cx (complex numbers); m (a matrix column)"));
    std::remove(path.c_str());
}

TEST_CASE("R-READ-1: the plan: types, labels, dates, missing values, names and metadata") {
    X x;
    x.header();
    const double d0 = 18322.0; /* 2020-03-01 */
    data_frame(
        x,
        {{"fac", [](X &x) {
              x.ints({2, 1, INT_MIN}, 1, 1);
              x.cell("levels");
              x.strs({"low", "high"});
              x.class_attr({"factor"});
              x.nil();
          }},
         {"day", [d0](X &x) {
              x.reals({d0, d0 + 1, na_real()}, 1, 1);
              x.class_attr({"Date"});
              x.nil();
          }},
         {"when", [](X &x) {
              x.reals({1583020800.5, 0, na_real()}, 1, 1);
              x.class_attr({"POSIXct", "POSIXt"});
              x.cell("tzone");
              x.strs({"Europe/Lisbon"});
              x.nil();
          }},
         {"clock", [](X &x) {
              x.reals({3661, 0, 86399.5}, 1, 1);
              x.cell("units");
              x.strs({"secs"});
              x.class_attr({"hms", "difftime"});
              x.nil();
          }},
         {"q", [](X &x) { /* haven_labelled_spss: 98 and 99 user-missing, tagged NA a */
              x.reals({1, 98, tagged('a')}, 1, 1);
              x.cell("label");
              x.strs({"Question one"});
              x.cell("labels");
              x.reals({1, 98, 99, tagged('a')}, 1);
              x.cell("names");
              x.strs({"Yes", "Refused", "Unknown", "Skipped"});
              x.nil();
              x.cell("na_values");
              x.reals({98, 99});
              x.class_attr({"haven_labelled_spss", "haven_labelled", "vctrs_vctr", "double"});
              x.nil();
          }},
         {"big", [](X &x) {
              const int64_t v[3] = {INT64_C(9007199254740993), INT64_MIN, -5};
              x.flags(REALSXP, 1, 0, 1);
              x.i32(3);
              for (int64_t b : v) x.bits(static_cast<uint64_t>(b));
              x.class_attr({"integer64"});
              x.nil();
          }},
         {"x", [](X &x) { x.ints({1, 2, 3}); }},
         {"X", [](X &x) { x.ints({4, 5, 6}); }},
         {"x", [](X &x) { x.ints({7, 8, 9}); }},
         {"", [](X &x) { x.strs({"a", "b", "c"}); }}},
        3, {"tbl_df", "tbl", "data.frame"}, [](X &x) {
            x.cell("label");
            x.strs({"My data"});
            x.cell("comment");
            x.strs({"first note", "second note"});
        });
    const std::string path = write_file("rdata_plan.rds", x.b);
    Plan plan = make_plan(path, opts());
    REQUIRE(plan.cols.size() == 10);
    auto col = [&](size_t i) -> const ColumnSpec & { return plan.cols[i]; };
    CHECK(col(0).kind == OutKind::Integer);
    CHECK(col(0).stata_type == "byte");
    CHECK(col(1).kind == OutKind::Date);
    CHECK(col(1).stata_format == "%td");
    CHECK(col(2).kind == OutKind::Timestamp);
    CHECK(col(3).kind == OutKind::Time);
    CHECK(col(4).kind == OutKind::Double);
    CHECK(col(4).companion);
    CHECK(col(4).tagged);
    /* the tagged NA keeps .a; the user-missing values take the next letters */
    CHECK(col(4).code_of(98) == 2);
    CHECK(col(4).code_of(99) == 3);
    CHECK(col(5).kind == OutKind::BigInt);
    CHECK(col(5).stata_type == "double");
    /* names: x, X, x (again), "" */
    CHECK(col(6).name == "x");
    CHECK(col(7).name == "X");
    CHECK(col(8).name == "x_1");
    CHECK(col(9).name == "V10");
    CHECK(plan.rename_leaves); /* x and X need case-distinct engine names */
    CHECK(col(6).engine_name == "x");
    CHECK(col(7).engine_name != "X");
    CHECK(col(6).stata_name == "x");
    CHECK(col(7).stata_name == "X");
    CHECK(plan.leaf_names.back() == "_parqit_xm_q");
    CHECK(contains(plan.kv_metadata_sql, "'parqit.xmissing'"));
    CHECK(contains(plan.kv_metadata_sql, "\"r_tzone\":\"Europe/Lisbon\""));
    CHECK(contains(plan.kv_metadata_sql, "\"r_class\":\"tbl_df tbl data.frame\""));
    CHECK(contains(plan.kv_metadata_sql, "\"note2\":\"second note\""));
    CHECK(contains(plan.kv_metadata_sql, "'parqit.dtalabel': '\"My data\"'"));
    CHECK(contains(plan.kv_metadata_sql, "[\".a\",\"Skipped\"]"));
    CHECK(contains(plan.kv_metadata_sql, "\"r_missing_map\":\".b=98 .c=99\""));
    CHECK(contains(plan.kv_metadata_sql, "\"r_name\":\"x\""));

    /* the scan writes what the plan promised */
    auto sp = std::make_shared<const Plan>(std::move(plan));
    parqit::Session &s = parqit::Session::instance();
    REQUIRE(s.ensure_open());
    const std::string token = register_plan(sp);
    const std::string out = parqit_test::tmp_path("rdata_plan.parquet");
    std::string err;
    REQUIRE_MESSAGE(s.exec("COPY (SELECT * FROM parqit_read_rdata(" + parqit::quote_literal(token) +
                               ")) TO " + parqit::quote_literal(out) + " (FORMAT PARQUET, " +
                               sp->kv_metadata_sql + ")",
                           &err),
                    err);
    release_plan(token);
    std::string got;
    REQUIRE(s.query_scalar(
        "SELECT string_agg(concat_ws('|', coalesce(CAST(fac AS VARCHAR), 'NULL'), "
        "coalesce(CAST(day AS VARCHAR), 'NULL'), coalesce(CAST(\"when\" AS VARCHAR), 'NULL'), "
        "CAST(clock AS VARCHAR), coalesce(CAST(q AS VARCHAR), 'NULL'), "
        "coalesce(CAST(big AS VARCHAR), 'NULL'), CAST(_parqit_xm_q AS VARCHAR), V10), ';' "
        "ORDER BY file_row_number) FROM read_parquet(" + parqit::quote_literal(out) +
            ", file_row_number = true)",
        &got, &err));
    CHECK(got == "2|2020-03-01|2020-03-01 00:00:00.5|01:01:01|1.0|9007199254740993|0|a;"
                 "1|2020-03-02|1970-01-01 00:00:00|00:00:00|NULL|NULL|2|b;"
                 "NULL|NULL|NULL|23:59:59.5|NULL|-5|1|c");

    /* RDATA-PARALLEL-1: the Arrow batches (rows in batches of 1 and 2, four
     * workers over the columns) write the table function's file */
    auto dump = [&](const std::string &f) {
        std::string types, kv;
        REQUIRE(s.query_scalar("SELECT string_agg(column_name || ' ' || column_type, ',') FROM (DESCRIBE "
                               "SELECT * FROM read_parquet(" + parqit::quote_literal(f) + "))",
                               &types, &err));
        REQUIRE(s.query_scalar("SELECT string_agg(decode(key) || '=' || decode(value), ';' ORDER BY "
                               "decode(key)) FROM parquet_kv_metadata(" + parqit::quote_literal(f) + ")",
                               &kv, &err));
        return types + "#" + kv;
    };
    auto every_cell = [&](const std::string &f) {
        std::string cells;
        REQUIRE(s.query_scalar("SELECT string_agg(concat_ws('|', COLUMNS(*)::VARCHAR), ';' ORDER BY "
                               "file_row_number) FROM read_parquet(" + parqit::quote_literal(f) +
                                   ", file_row_number = true)",
                               &cells, &err));
        return cells;
    };
    for (int64_t batch_rows : {1, 2}) {
        const std::string out_ar = parqit_test::tmp_path("rdata_plan_arrow.parquet");
        RdataArrowSource src(sp, 4, batch_rows);
        src.start();
        REQUIRE(duckdb_arrow_scan(s.con(), "rdata_arrow_view",
                                  reinterpret_cast<duckdb_arrow_stream>(src.stream())) == DuckDBSuccess);
        REQUIRE_MESSAGE(s.exec("COPY (SELECT * FROM rdata_arrow_view) TO " + parqit::quote_literal(out_ar) +
                                   " (FORMAT PARQUET, " + sp->kv_metadata_sql + ")",
                               &err),
                        err);
        CHECK(src.finish().empty());
        CHECK(src.batches() == (batch_rows == 1 ? 3 : 2));
        s.exec("DROP VIEW IF EXISTS rdata_arrow_view", &err);
        CHECK(every_cell(out_ar) == every_cell(out));
        CHECK(dump(out_ar) == dump(out));
        std::remove(out_ar.c_str());
    }
    std::remove(out.c_str());
    std::remove(path.c_str());
}

TEST_CASE("R-READ-1: row names") {
    auto frame = [](const std::function<void(X &)> &rn) {
        X x;
        x.header();
        x.flags(VECSXP, 1, 0, 1);
        x.i32(1);
        x.ints({1, 2});
        x.cell("names");
        x.strs({"rowname"});
        x.class_attr({"data.frame"});
        x.cell("row.names");
        rn(x);
        x.nil();
        return x.b;
    };
    std::string path = write_file("rdata_rn.rds", frame([](X &x) { x.strs({"Mazda", "Fiat"}); }));
    Plan plan = make_plan(path, opts());
    REQUIRE(plan.cols.size() == 2);
    CHECK(plan.cols[0].col == -1);
    CHECK(plan.cols[0].name == "rowname_1"); /* the data column keeps its own name */
    CHECK(plan.cols[1].name == "rowname");
    path = write_file("rdata_rn.rds", frame([](X &x) { x.ints({4, 9}); }));
    plan = make_plan(path, opts());
    REQUIRE(plan.cols.size() == 1);
    bool noted = false;
    for (const auto &n : plan.notes) noted = noted || contains(n, "integer row names");
    CHECK(noted);
    path = write_file("rdata_rn.rds", frame([](X &x) { x.ints({1, 2}); }));
    plan = make_plan(path, opts());
    for (const auto &n : plan.notes) CHECK_FALSE(contains(n, "row names"));
    std::remove(path.c_str());
}

TEST_CASE("R-READ-1: a long-vector length is read") {
    X x;
    x.header();
    x.flags(VECSXP, 1, 0, 1);
    x.i32(1);
    x.flags(INTSXP);
    x.i32(-1); /* long form: upper, lower */
    x.i32(0);
    x.i32(2);
    x.i32(11);
    x.i32(12);
    x.cell("names");
    x.strs({"a"});
    x.class_attr({"data.frame"});
    x.cell("row.names");
    x.ints({INT_MIN, -2});
    x.nil();
    const std::string path = write_file("rdata_long.rds", x.b);
    Parsed p = read_file(path, opts());
    Cursor c(p.source, p.frame.cols[0].vec, p.codec);
    CHECK(c.next_int() == 11);
    CHECK(c.next_int() == 12);
    std::remove(path.c_str());
}

TEST_CASE("R-READ-1: fallbacks the profile decides, and names that clash with companions") {
    X x;
    x.header();
    const std::string long_level(32001, 'z');
    data_frame(
        x,
        {{"clock", [](X &x) { /* 25 hours: beyond a day, so seconds */
              x.reals({3600, 90000}, 1, 1);
              x.cell("units");
              x.strs({"secs"});
              x.class_attr({"hms", "difftime"});
              x.nil();
          }},
         {"far", [](X &x) { /* 1e13 s is beyond a TIMESTAMP: numbers */
              x.reals({0, 1e13}, 1, 1);
              x.class_attr({"POSIXct", "POSIXt"});
              x.nil();
          }},
         {"q", [](X &x) { /* a tag that is not a letter, and a real one */
              x.reals({tagged('#'), tagged('b')});
          }},
         {"_parqit_xm_q", [](X &x) { x.ints({7, 8}); }},
         {"fac", [&long_level](X &x) { /* a level no value label can hold */
              x.ints({1, 2}, 1, 1);
              x.cell("levels");
              x.strs({"short", long_level});
              x.class_attr({"factor"});
              x.nil();
          }},
         {"frac", [](X &x) { /* a Date holding half a day */
              x.reals({18322.5, 18323}, 1, 1);
              x.class_attr({"Date"});
              x.nil();
          }}},
        2);
    const std::string path = write_file("rdata_fallbacks.rds", x.b);
    Plan plan = make_plan(path, opts());
    REQUIRE(plan.cols.size() == 6);
    CHECK(plan.cols[0].kind == OutKind::Double);    /* hms beyond a day */
    CHECK(plan.cols[1].kind == OutKind::Double);    /* POSIXct beyond TIMESTAMP */
    CHECK(plan.cols[2].tagged);
    CHECK(plan.cols[2].companion);
    CHECK(plan.cols[3].name == "_parqit_xm_q");
    CHECK(plan.cols[2].companion_name == "_parqit_xm_q_1"); /* never the user's column */
    CHECK(plan.cols[4].kind == OutKind::Varchar);
    CHECK(plan.cols[4].conv == Conv::FactorText);
    CHECK(plan.cols[4].stata_type == "strL"); /* 32,001 bytes: beyond str2045 */
    CHECK(plan.cols[5].kind == OutKind::Timestamp);
    std::string notes;
    for (const auto &n : plan.notes) notes += n + "\n";
    CHECK(contains(notes, "outside 00:00-24:00 after rounding to microseconds were stored as seconds: clock"));
    CHECK(contains(notes, "seconds since 1970-01-01 UTC: far"));
    CHECK(contains(notes, "whose tag is not a letter a-z became plain missing values in q"));
    CHECK(contains(notes, "were stored as text: fac"));
    CHECK(contains(notes, "fractions of a day were stored as date-times (%tc): frac"));
    /* the scan: the non-letter tag has no code, the letter keeps its own */
    auto sp = std::make_shared<const Plan>(std::move(plan));
    parqit::Session &s = parqit::Session::instance();
    REQUIRE(s.ensure_open());
    const std::string token = register_plan(sp);
    const std::string out = parqit_test::tmp_path("rdata_fallbacks.parquet");
    std::string err;
    REQUIRE_MESSAGE(s.exec("COPY (SELECT * FROM parqit_read_rdata(" + parqit::quote_literal(token) +
                               ")) TO " + parqit::quote_literal(out) + " (FORMAT PARQUET)", &err),
                    err);
    release_plan(token);
    std::string got;
    REQUIRE(s.query_scalar("SELECT string_agg(concat_ws('|', CAST(clock AS VARCHAR), CAST(far AS VARCHAR), "
                           "coalesce(CAST(q AS VARCHAR), 'NULL'), CAST(_parqit_xm_q AS VARCHAR), "
                           "CAST(_parqit_xm_q_1 AS VARCHAR), length(fac), CAST(frac AS VARCHAR)), ';' "
                           "ORDER BY file_row_number) FROM read_parquet(" + parqit::quote_literal(out) +
                               ", file_row_number = true)",
                           &got, &err));
    CHECK(got == "3600.0|0.0|NULL|7|0|5|2020-03-01 12:00:00;90000.0|10000000000000.0|NULL|8|2|32001|2020-03-02 00:00:00");
    std::remove(out.c_str());
    std::remove(path.c_str());
}

TEST_CASE("R-READ-1: object() picks a data frame from an .rds list through the whole plan") {
    X x;
    x.header();
    x.flags(VECSXP, 1);
    x.i32(3);
    data_frame(x, {{"a", [](X &x) { x.ints({1, 2}); }}}, 2);
    x.strs({"text"});
    data_frame(x, {{"b", [](X &x) { x.strs({"u", "v", "w"}); }}}, 3);
    x.cell("names");
    x.strs({"first", "note", "second"});
    x.nil();
    const std::string path = write_file("rdata_list_plan.rds", x.b);
    CHECK(contains(read_error(path), "holds 2 data frames (first, second)"));
    Plan plan = make_plan(path, opts("second"));
    CHECK(plan.nrow == 3);
    REQUIRE(plan.cols.size() == 1);
    CHECK(plan.cols[0].name == "b");
    CHECK(contains(plan.kv_metadata_sql, "\"r_object\":\"second\""));
    std::remove(path.c_str());
}

TEST_CASE("R-AUDIT: generated object names do not hide a real R name") {
    X x;
    x.header();
    x.flags(VECSXP, 1);
    x.i32(2);
    data_frame(x, {{"id", [](X &x) { x.ints({111}); }}}, 1);
    data_frame(x, {{"id", [](X &x) { x.ints({222}); }}}, 1);
    x.cell("names");
    x.strs({"", "[[1]]"});
    x.nil();
    const std::string path = write_file("rdata_object_collision.rds", x.b);
    Parsed p = read_file(path, opts("[[1]]"));
    Cursor c(p.source, p.frame.cols[0].vec, p.codec);
    CHECK(c.next_int() == 222);
    std::remove(path.c_str());
}

TEST_CASE("R-AUDIT: temporal output units take precedence over an inherited format") {
    X x;
    x.header();
    data_frame(x, {{"d", [](X &x) {
                   x.reals({0, .5}, 1, 1);
                   x.class_attr({"Date"});
                   x.cell("format.stata");
                   x.strs({"%td"});
                   x.nil();
               }}}, 2);
    const std::string path = write_file("rdata_date_format.rds", x.b);
    const Plan p = make_plan(path, opts());
    REQUIRE(p.cols.size() == 1);
    CHECK(p.cols[0].kind == OutKind::Timestamp);
    CHECK(p.cols[0].stata_format == "%tc");
    CHECK(contains(p.kv_metadata_sql, "format.stata"));
    std::remove(path.c_str());
}

TEST_CASE("R-AUDIT: finite Date extremes and rounded midnight use numeric fallbacks") {
    int32_t days = 0;
    int64_t us = 0;
    CHECK_FALSE(days_to_date(-2147483647.0, &days));
    CHECK_FALSE(days_to_date(2147483647.0, &days));
    CHECK(days_to_date(-2147483646.0, &days));
    CHECK(days_to_date(2147483646.0, &days));
    CHECK_FALSE(seconds_to_time_us(86399.9999999, &us));
    CHECK(seconds_to_time_us(86399.999999, &us));
    CHECK(us == 86399999999LL);
    for (bool integral : {false, true}) {
        X x;
        x.header();
        data_frame(x, {{"d", [integral](X &x) {
                       if (integral) x.ints({-2147483647, 0, 2147483647}, 1, 1);
                       else x.reals({-2147483647, 0, 2147483647}, 1, 1);
                       x.class_attr({"Date"});
                       x.nil();
                   }}, {"t", [](X &x) {
                       x.reals({0, 86399.9999999, 86399.999999}, 1, 1);
                       x.class_attr({"hms", "difftime"});
                       x.cell("units");
                       x.strs({"secs"});
                       x.nil();
                   }}}, 3);
        const std::string path = write_file("rdata_time_bounds.rds", x.b);
        const Plan p = make_plan(path, opts());
        CHECK(p.cols[0].kind == OutKind::Double);
        CHECK(p.cols[1].kind == OutKind::Double);
        CHECK(p.cols[0].stata_format.empty());
        CHECK(p.cols[1].stata_format.empty());
        std::remove(path.c_str());
    }
}

TEST_CASE("R-AUDIT: malformed compact sequences are rejected before integer casts") {
    for (const std::vector<double> state : {
             std::vector<double>{1e100, 1, 1}, {3.5, 1, 1}, {3, 1e100, 1},
             {3, 1.5, 1}, {3, 2147483646, 1}, {3, -2147483647, -1}}) {
        for (bool attribute : {false, true}) {
            auto compact = [&](X &x) {
                x.i32(ALTREP_SXP);
                x.flags(LISTSXP); x.sym("compact_intseq");
                x.flags(LISTSXP); x.sym("base");
                x.flags(LISTSXP); x.ints({INTSXP}); x.nil();
                x.reals(state); x.nil();
            };
            X x;
            x.header();
            if (attribute) {
                data_frame(x, {{"id", [](X &x) { x.ints({1, 2, 3}); }}}, 3, {"data.frame"},
                           [&](X &x) { x.cell("bad"); compact(x); });
            } else {
                data_frame(x, {{"id", compact}}, 3);
            }
            const std::string path = write_file("rdata_bad_sequence.rds", x.b);
            CHECK(contains(read_error(path), "invalid compact sequence"));
            std::remove(path.c_str());
        }
    }
}

TEST_CASE("RDATA-PARALLEL-1: the eight-bytes-at-a-time gzip CRC-32 is miniz's") {
    std::mt19937 rng(20261024);
    std::vector<unsigned char> buf(1 << 16);
    for (auto &b : buf) b = static_cast<unsigned char>(rng());
    /* every length and alignment up to 64, then whole buffers in uneven pieces */
    for (size_t off = 0; off < 9; off++)
        for (size_t n = 0; n <= 64; n++)
            CHECK(parqit::rdata::gzip_crc32(0, buf.data() + off, n) ==
                  static_cast<uint32_t>(duckdb_miniz::mz_crc32(0, buf.data() + off, n)));
    uint32_t a = 0;
    duckdb_miniz::mz_ulong b = 0;
    for (size_t at = 0; at < buf.size();) {
        const size_t n = std::min<size_t>(buf.size() - at, 1 + rng() % 5000);
        a = parqit::rdata::gzip_crc32(a, buf.data() + at, n);
        b = duckdb_miniz::mz_crc32(b, buf.data() + at, n);
        at += n;
    }
    CHECK(a == static_cast<uint32_t>(b));
    const unsigned char check[] = "123456789"; /* the CRC-32 check value */
    CHECK(parqit::rdata::gzip_crc32(0, check, 9) == 0xCBF43926u);
}

namespace {

/* A5: a conversion's stream read as the engine reads it, alone; every run
 * must end within 5 seconds (a hang fails the test instead of the suite) */
struct Drained {
    int64_t rows = 0, peak = 0, max = 0;
    int workers = 0;
    bool failed = false, held_after_error = false;
    std::string message, finish;
};

/* sync forces an interleaving. pause_error holds the consumer once the named
 * error is observed with its queue full, without accumulating timer ticks. */
Drained drain(const std::shared_ptr<const Plan> &plan, int threads, int64_t batch_rows,
              const std::string &fault, int64_t stop_after = -1, const std::string &sync = "",
              int64_t pause_error = -1) {
    parqit_test::ScopedEnv env("PARQIT_TEST_FAULT", fault);
    parqit_test::ScopedEnv order("PARQIT_TEST_SYNC", sync);
    parqit::test_sync_reset();
    auto src = std::make_shared<RdataArrowSource>(plan, threads, batch_rows);
    std::packaged_task<Drained()> task([src, stop_after, pause_error] {
        Drained d;
        src->start();
        ArrowArrayStream *st = src->stream();
        for (int64_t k = 0; stop_after < 0 || k < stop_after; k++) {
            ArrowArray a{};
            if (st->get_next(st, &a) != 0) {
                d.failed = true;
                d.message = st->get_last_error(st);
                break;
            }
            if (!a.release) break;
            d.rows += a.length;
            a.release(&a);
            if (pause_error >= 0 && !d.held_after_error && k + 1 + src->max_batches() > pause_error) {
                auto &sync = parqit::test_sync_state();
                const std::string site = "rdata_failed:" + std::to_string(pause_error);
                std::unique_lock<std::mutex> lock(sync.m);
                if (!sync.cv.wait_for(lock, std::chrono::seconds(2), [&] { return sync.reached.count(site) > 0; }))
                    throw std::runtime_error("consumer gate did not observe the conversion error");
                lock.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                d.held_after_error = true;
            }
        }
        d.finish = src->finish();
        d.peak = src->peak_batches();
        d.max = src->max_batches();
        d.workers = src->workers();
        return d;
    });
    auto done = task.get_future();
    std::thread(std::move(task)).detach();
    REQUIRE_MESSAGE(done.wait_for(std::chrono::seconds(5)) == std::future_status::ready, "hung: " << fault);
    return done.get();
}

/* a destination of convert_rows that fails at one row */
struct ThrowAt {
    uint64_t at;
    void hit(uint64_t r) const {
        if (r == at) throw std::bad_alloc();
    }
    void text(uint64_t r, const char *, size_t) { hit(r); }
    void boolean(uint64_t r, bool) { hit(r); }
    void i64(uint64_t r, int64_t) { hit(r); }
    void i32(uint64_t r, int32_t) { hit(r); }
    void f64(uint64_t r, double) { hit(r); }
    void days(uint64_t r, int32_t) { hit(r); }
    void timestamp(uint64_t r, int64_t) { hit(r); }
    void time(uint64_t r, int64_t) { hit(r); }
    void valid(uint64_t, bool) {}
    void code(uint64_t, int8_t) {}
};

} // namespace

TEST_CASE("A5 (F12): a failing read or destination names the row being converted") {
    std::vector<double> v(3000);
    for (size_t i = 0; i < v.size(); i++) v[i] = static_cast<double>(i) + 0.5;
    X x;
    x.header();
    data_frame(x, {{"d", [&](X &x) { x.reals(v); }}}, 3000);
    const std::string path = write_file("rdata_a5_rows.rds", x.b);
    const Plan plan = make_plan(path, opts());
    REQUIRE(plan.cols.size() == 1);
    std::string text;
    {
        /* the destination fails after the read: the row is the one read */
        Cursor cur(plan.parsed.source, plan.cols[0].vec, plan.parsed.codec);
        ThrowAt o{2047};
        uint64_t row = 0;
        CHECK_THROWS_AS(convert_rows(plan, plan.cols[0], cur, 0, 3000, o, &text, &row), std::bad_alloc);
        CHECK(row == 2047);
        CHECK(cur.index() == 2048);
    }
    {
        /* the read itself fails at element 2047 (it crosses the end of the
         * stream): the cursor stays on the failing element */
        VecRef near_end = plan.cols[0].vec;
        near_end.offset = plan.parsed.source->size() - 2047 * 8 - 4;
        Cursor cut(plan.parsed.source, near_end, plan.parsed.codec, 4096);
        for (int i = 0; i < 2047; i++) cut.next_real();
        CHECK(cut.index() == 2047);
        CHECK_THROWS_AS(cut.next_real(), RError);
        CHECK(cut.index() == 2047);
    }
    std::remove(path.c_str());
}

TEST_CASE("A5 (F12): the first error is the table function's whatever the batch size") {
    /* two Date columns of 3000 rows, changed in place after the profile (same
     * size and time): column b no longer fits at row 1, column a at row 4. The
     * table function meets column a's first (vector 0, column 0, row 3), and
     * so must the batches, of 3 rows (a later batch) and of the default */
    const int n = 3000;
    std::vector<double> days(static_cast<size_t>(n));
    for (int i = 0; i < n; i++) days[static_cast<size_t>(i)] = 18000.0 + i % 400;
    X x;
    x.header();
    data_frame(x,
               {{"a", [&](X &x) {
                     x.reals(days, 1, 1);
                     x.class_attr({"Date"});
                     x.nil();
                 }},
                {"b", [&](X &x) {
                     x.reals(days, 1, 1);
                     x.class_attr({"Date"});
                     x.nil();
                 }}},
               n);
    const std::string path = write_file("rdata_a5_order.rds", x.b);
    auto plan = std::make_shared<const Plan>(make_plan(path, opts()));
    REQUIRE(plan->cols.size() == 2);
    const auto stamp = std::filesystem::last_write_time(path);
    auto patch = [&](size_t col, int row) {
        std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
        const auto at = static_cast<std::streamoff>(plan->cols[col].vec.offset + 8 * static_cast<uint64_t>(row));
        char be[8];
        f.seekg(at);
        f.read(be, 8);
        uint64_t was = 0;
        for (int k = 0; k < 8; k++) was = (was << 8) | static_cast<unsigned char>(be[k]);
        double old;
        std::memcpy(&old, &was, 8);
        REQUIRE(old == days[static_cast<size_t>(row)]);
        const double bad = 1e300;
        uint64_t u;
        std::memcpy(&u, &bad, 8);
        for (int k = 0; k < 8; k++) be[k] = static_cast<char>(u >> (8 * (7 - k)));
        f.seekp(at);
        f.write(be, 8);
    };
    patch(1, 0);
    patch(0, 3);
    std::filesystem::last_write_time(path, stamp);
    parqit::Session &s = parqit::Session::instance();
    REQUIRE(s.ensure_open());
    std::string want;
    {
        const PlanRegistration reg(plan);
        const std::string out = parqit_test::tmp_path("rdata_a5_order.parquet");
        CHECK_FALSE(s.exec("COPY (SELECT * FROM parqit_read_rdata(" + parqit::quote_literal(reg.token()) +
                               ")) TO " + parqit::quote_literal(out) + " (FORMAT PARQUET)",
                           &want));
        std::remove(out.c_str());
    }
    CHECK(contains(want, "column a, row 4"));
    for (int64_t rows : {3, 0})
        for (int threads : {2, 4}) {
            INFO("batch rows " << rows << ", threads " << threads);
            const Drained d = drain(plan, threads, rows, "");
            CHECK(d.failed);
            CHECK(contains(d.message, "column a, row 4"));
            CHECK(contains(want, d.message));
            CHECK(d.finish == d.message);
        }
    std::remove(path.c_str());
}

/* two Date columns, a and b, of 3000 rows, changed in place after the profile
 * (same size and time) so that each (column, row) given no longer fits */
static std::shared_ptr<const Plan> changed_dates(const std::string &name,
                                                 const std::vector<std::pair<size_t, int>> &bad) {
    const int n = 3000;
    std::vector<double> days(static_cast<size_t>(n));
    for (int i = 0; i < n; i++) days[static_cast<size_t>(i)] = 18000.0 + i % 400;
    auto date = [&](X &x) {
        x.reals(days, 1, 1);
        x.class_attr({"Date"});
        x.nil();
    };
    X x;
    x.header();
    data_frame(x, {{"a", date}, {"b", date}}, n);
    const std::string path = write_file(name, x.b);
    auto plan = std::make_shared<const Plan>(make_plan(path, opts()));
    REQUIRE(plan->cols.size() == 2);
    const auto stamp = std::filesystem::last_write_time(path);
    for (const auto &[col, row] : bad) {
        std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
        const auto at = static_cast<std::streamoff>(plan->cols[col].vec.offset + 8 * static_cast<uint64_t>(row));
        const double worse = 1e300;
        uint64_t u;
        std::memcpy(&u, &worse, 8);
        char be[8];
        for (int k = 0; k < 8; k++) be[k] = static_cast<char>(u >> (8 * (7 - k)));
        f.seekp(at);
        f.write(be, 8);
    }
    std::filesystem::last_write_time(path, stamp);
    return plan;
}

TEST_CASE("G5: a batch with a failed column is never handed on, in any order of the workers") {
    /* Column b no longer fits at row 1, column a at row 4; batches of 3 on two
     * workers, one column each. The forced order: b's worker records its error
     * in batch 0, then waits until a's has recorded the earlier one (vector 0,
     * column a, row 4) in batch 1, and only then finishes batch 0, which must
     * not be handed on with b unconverted. Every batch lies in the error's
     * vector, so none is handed on in any order. */
    auto plan = changed_dates("rdata_g5.rds", {{1, 0}, {0, 3}});
    for (const char *sync : {"rdata_failed:0>rdata_failed:1", "rdata_failed:1>rdata_failed:0", ""})
        for (int rep = 0; rep < 5; rep++) {
            INFO(sync << ", rep " << rep);
            const Drained d = drain(plan, 2, 3, "", -1, sync);
            CHECK(d.failed);
            CHECK(contains(d.message, "column a, row 4"));
            CHECK(d.rows == 0);
            CHECK(d.finish == d.message);
        }
    /* the same order when b's conversion throws in batch 0 (rdata_column:
     * batch * 100000 + column) */
    auto thrown = changed_dates("rdata_g5x.rds", {{0, 3}});
    for (int rep = 0; rep < 5; rep++) {
        INFO("thrown, rep " << rep);
        const Drained d = drain(thrown, 2, 3, "rdata_column:1", -1, "rdata_failed:0>rdata_failed:1");
        CHECK(d.failed);
        CHECK(contains(d.message, "column a, row 4"));
        CHECK(d.rows == 0);
    }
    std::remove(plan->path.c_str());
    std::remove(thrown->path.c_str());
}

TEST_CASE("G8: after an error the R conversion keeps no more batches than before it") {
    /* column b no longer fits at row 2001; the work before it (column a to the
     * end of vector 0) runs on while a slow engine reads: the batches alive
     * never exceed the bound kept before the error, and every batch before the
     * error's is handed on whole */
    auto plan = changed_dates("rdata_g8.rds", {{1, 2000}});
    for (int rep = 0; rep < 3; rep++) {
        INFO("rep " << rep);
        const Drained d = drain(plan, 2, 3, "", -1, "rdata_failed:666>rdata_failed:666", 666);
        CHECK(d.held_after_error);
        CHECK(d.failed);
        CHECK(contains(d.message, "column b, row 2001"));
        CHECK(d.rows == 1998);
        CHECK(d.peak >= 1);
        CHECK(d.peak <= d.max);
        CHECK(d.workers == 2);
    }
    std::remove(plan->path.c_str());
}

TEST_CASE("A5: a failing worker, allocation or thread ends the parallel R conversion with its error") {
    /* 300 rows of three columns in batches of 3 (100 batches) on three
     * workers; PARQIT_TEST_FAULT throws std::bad_alloc at one place */
    std::vector<double> d(300);
    std::vector<int32_t> iv(300);
    std::vector<std::string> sv(300);
    for (int k = 0; k < 300; k++) {
        d[static_cast<size_t>(k)] = k + 0.25;
        iv[static_cast<size_t>(k)] = k;
        sv[static_cast<size_t>(k)] = "s" + std::to_string(k % 7);
    }
    X x;
    x.header();
    data_frame(x,
               {{"d", [&](X &x) { x.reals(d); }},
                {"i", [&](X &x) { x.ints(iv); }},
                {"s", [&](X &x) { x.strs(sv); }}},
               300);
    const std::string path = write_file("rdata_a5_faults.rds", x.b);
    auto plan = std::make_shared<const Plan>(make_plan(path, opts()));
    for (int rep = 0; rep < 10; rep++) {
        const Drained clean = drain(plan, 4, 3, "");
        CHECK_FALSE(clean.failed);
        CHECK(clean.rows == 300);
        CHECK(clean.finish.empty());
        CHECK(clean.workers == 3); /* G10: the workers created, one per column */
        CHECK(clean.peak <= clean.max);
        for (const char *site : {"rdata_batch", "rdata_convert", "rdata_ready"})
            for (int b : {0, 3, 99}) {
                const std::string fault = std::string(site) + ":" + std::to_string(b);
                INFO(fault);
                const Drained o = drain(plan, 4, 3, fault);
                CHECK(o.failed);
                CHECK(o.rows <= 3 * b);
                CHECK_FALSE(o.message.empty());
                CHECK(o.finish == o.message);
            }
        const Drained no_thread = drain(plan, 4, 3, "rdata_thread:2");
        CHECK(no_thread.failed);
        CHECK(contains(no_thread.message, "could not start the R conversion workers"));
        const Drained cancelled = drain(plan, 4, 3, "", 2); /* the engine stops reading */
        CHECK(cancelled.rows == 6);
        CHECK(cancelled.finish.empty());
    }
    /* an exception between registering a plan and the COPY leaves none behind */
    const size_t before = registered_plans();
    try {
        const PlanRegistration reg(plan);
        CHECK(registered_plans() == before + 1);
        throw std::runtime_error("between registering and the COPY");
    } catch (const std::runtime_error &) {
    }
    CHECK(registered_plans() == before);
    std::remove(path.c_str());
}

TEST_CASE("A2: an R batch stays within 64 MB, down to one row") {
    CHECK(batch_rows_for(1) == 122880);
    CHECK(batch_rows_for(546) == 122880);
    CHECK(batch_rows_for(547) == 120832);  /* 122685 rows, in whole vectors */
    CHECK(batch_rows_for(32768) == 2048);
    CHECK(batch_rows_for(32769) == 2047);  /* below a vector: any number */
    CHECK(batch_rows_for(int64_t(40) << 20) == 1);
    CHECK(batch_rows_for(int64_t(1) << 40) == 1);
    for (int64_t b = 1; b < (int64_t(1) << 28); b = b * 3 + 1) {
        const int64_t rows = batch_rows_for(b);
        INFO("row bytes " << b);
        CHECK(rows >= 1);
        CHECK((rows * b <= (int64_t(64) << 20) || rows == 1));
    }
}

TEST_CASE("A3: the R route is chosen from the frame's rows; both profiles plan the same") {
    ReadOptions o = opts();
    o.threads = 4;
    const Compression gz = Compression::Gzip;
    o.parallel_min_rows = 1000;
    CHECK(parallel_route(1000, gz, o));
    CHECK_FALSE(parallel_route(999, gz, o));
    o.parallel_min_rows = 0;
    CHECK(parallel_route(1, gz, o));
    o.route = 0;
    CHECK_FALSE(parallel_route(1 << 20, gz, o));
    o.route = 1;
    o.parallel_min_rows = int64_t(1) << 40;
    CHECK(parallel_route(1, gz, o));
    o.threads = 1;
    CHECK_FALSE(parallel_route(1, gz, o));
    /* the measured rule (ASSUMPTIONS #229): by compression and thread regime */
    CHECK(parallel_min_rows(48, Compression::Gzip) == 1000000);
    CHECK(parallel_min_rows(8, Compression::Zstd) == 500000);
    CHECK(parallel_min_rows(8, Compression::None) == 500000);
    CHECK(parallel_min_rows(2, Compression::None) == 2000000);
    o.route = -1;
    o.parallel_min_rows = -1;
    o.threads = 48;
    CHECK(parallel_route(1000000, gz, o));
    CHECK_FALSE(parallel_route(999999, gz, o));
    CHECK(parallel_route(500000, Compression::Zstd, o));
    o.threads = 4;
    CHECK_FALSE(parallel_route(1999999, Compression::None, o));
    /* the serial and the parallel profile of the same file plan the same */
    const std::string path = write_file("rdata_a3.rds", basic_rds());
    ReadOptions ser = opts(), par = opts();
    ser.threads = par.threads = 4;
    ser.route = 0;
    par.route = 1;
    const Plan a = make_plan(path, ser), b = make_plan(path, par);
    CHECK_FALSE(a.profile_parallel);
    CHECK_FALSE(a.parallel);
    CHECK(b.profile_parallel);
    CHECK(b.parallel);
    CHECK(a.nrow == b.nrow);
    REQUIRE(a.cols.size() == b.cols.size());
    for (size_t i = 0; i < a.cols.size(); i++) {
        CHECK(a.cols[i].kind == b.cols[i].kind);
        CHECK(a.cols[i].stata_type == b.cols[i].stata_type);
        CHECK(a.cols[i].max_bytes == b.cols[i].max_bytes);
        CHECK(a.cols[i].companion == b.cols[i].companion);
    }
    CHECK(a.kv_metadata_sql == b.kv_metadata_sql);
    CHECK(a.notes == b.notes);
    CHECK(a.transcoded_cells == b.transcoded_cells);
    std::remove(path.c_str());
}

TEST_CASE("G4: the measured R route just below, at and just above each threshold, at 7 and 8 threads") {
    ReadOptions o = opts();
    o.route = -1;
    o.parallel_min_rows = -1;
    for (int threads : {7, 8, 48})
        for (Compression c : {Compression::None, Compression::Gzip, Compression::Zstd}) {
            INFO("threads " << threads << ", compression " << static_cast<int>(c));
            o.threads = threads;
            const int64_t t = parallel_min_rows(threads, c);
            CHECK_FALSE(parallel_route(t - 1, c, o));
            CHECK(parallel_route(t, c, o));
            CHECK(parallel_route(t + 1, c, o));
        }
    for (Compression c : {Compression::None, Compression::Gzip, Compression::Zstd})
        CHECK(parallel_min_rows(7, c) == 2000000);
    CHECK(parallel_min_rows(8, Compression::Gzip) == 1000000);
    CHECK(parallel_min_rows(8, Compression::Zstd) == 500000);
    CHECK(parallel_min_rows(8, Compression::None) == 500000);
}
