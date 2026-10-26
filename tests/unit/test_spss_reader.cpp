/* SPSS-READ-1: the native SPSS reader (engine/spss_reader, spss_plan,
 * spss_table). The files here are written byte by byte by a small writer that
 * follows the published format, so every layout the reader must handle —
 * both byte orders, the three data layouts, very long strings, every
 * extension record, legacy code pages — and every malformation it must refuse
 * can be pinned without an external program. The Stata verify tests
 * (v127-v130) compare the whole path against independent readers
 * (pyreadstat, Stata's import spss, pyarrow). */
#include "doctest.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "duckdb.h"
#include "engine/session.hpp"
#include "engine/spss_parallel.hpp"
#include "engine/spss_plan.hpp"
#include "engine/spss_reader.hpp"
#include "engine/spss_table.hpp"
#include "miniz.hpp"
#include "test_env.hpp"
#include "test_tmp.hpp"

using namespace parqit::spss;

namespace {

bool host_big() {
    const uint16_t one = 1;
    unsigned char c;
    std::memcpy(&c, &one, 1);
    return c == 0;
}
uint32_t sw32(uint32_t x) {
    return (x >> 24) | ((x >> 8) & 0xFF00u) | ((x << 8) & 0xFF0000u) | (x << 24);
}
uint64_t sw64(uint64_t x) { return (uint64_t(sw32(uint32_t(x))) << 32) | sw32(uint32_t(x >> 32)); }

/* bytes in the file's byte order */
struct W {
    bool big = false;
    std::string b;
    void put(const void *p, size_t n) { b.append(static_cast<const char *>(p), n); }
    void i32(int32_t v) {
        uint32_t u;
        std::memcpy(&u, &v, 4);
        if (big != host_big()) u = sw32(u);
        put(&u, 4);
    }
    void i64(int64_t v) {
        uint64_t u;
        std::memcpy(&u, &v, 8);
        if (big != host_big()) u = sw64(u);
        put(&u, 8);
    }
    void f64(double v) {
        uint64_t u;
        std::memcpy(&u, &v, 8);
        if (big != host_big()) u = sw64(u);
        put(&u, 8);
    }
    void text(const std::string &s, size_t n, char pad = ' ') {
        std::string t = s.substr(0, n);
        t.resize(n, pad);
        b += t;
    }
};

std::string num8(double v, bool big) {
    W w;
    w.big = big;
    w.f64(v);
    return w.b;
}
std::string bits8(uint64_t bits, bool big) {
    double v;
    std::memcpy(&v, &bits, 8);
    return num8(v, big);
}
std::string str8(const std::string &s) {
    std::string t = s.substr(0, 8);
    t.resize(8, ' ');
    return t;
}

const uint64_t kSysmis = 0xFFEFFFFFFFFFFFFFull;
int32_t fmt(int type, int width, int dec) { return (type << 16) | (width << 8) | dec; }

void header(W &w, const char *magic, int32_t compression, int32_t weight, int32_t ncases,
            const std::string &label = "", double bias = 100.0,
            const std::string &product = "@(#) SPSS DATA FILE parqit unit test") {
    w.text(magic, 4);
    w.text(product, 60);
    w.i32(2);
    w.i32(-1);
    w.i32(compression);
    w.i32(weight);
    w.i32(ncases);
    w.f64(bias);
    w.text("26 Sep 26", 9);
    w.text("21:40:00", 8);
    w.text(label, 64);
    w.text("", 3, '\0');
}

void var(W &w, int32_t type, const std::string &name, int32_t print, const std::string &label = "",
         int32_t nmiss = 0, const std::vector<std::string> &miss = {}) {
    w.i32(2);
    w.i32(type);
    w.i32(label.empty() ? 0 : 1);
    w.i32(nmiss);
    w.i32(print);
    w.i32(print);
    w.text(name, 8);
    if (!label.empty()) {
        w.i32(static_cast<int32_t>(label.size()));
        std::string l = label;
        l.resize((label.size() + 3) / 4 * 4, ' ');
        w.b += l;
    }
    for (const auto &m : miss) w.b += m;
}
void numeric(W &w, const std::string &name, const std::string &label = "", int32_t nmiss = 0,
             const std::vector<double> &miss = {}, int32_t print = fmt(5, 8, 2)) {
    std::vector<std::string> m;
    for (double x : miss) m.push_back(num8(x, w.big));
    var(w, 0, name, print, label, nmiss, m);
}
void string_var(W &w, int width, const std::string &name, const std::string &label = "",
                int32_t nmiss = 0, const std::vector<std::string> &miss = {}) {
    std::vector<std::string> m;
    for (const auto &s : miss) m.push_back(str8(s));
    var(w, width, name, fmt(1, width, 0), label, nmiss, m);
    for (int k = 1; k < (width + 7) / 8; k++) var(w, -1, "", 0);
}
void rec7(W &w, int32_t subtype, int32_t size, int32_t count, const std::string &data) {
    w.i32(7);
    w.i32(subtype);
    w.i32(size);
    w.i32(count);
    w.b += data;
}
void labels(W &w, const std::vector<std::pair<std::string, std::string>> &entries,
            const std::vector<int32_t> &idx) {
    w.i32(3);
    w.i32(static_cast<int32_t>(entries.size()));
    for (const auto &e : entries) {
        w.b += e.first;
        w.b += static_cast<char>(e.second.size());
        std::string l = e.second;
        l.resize((e.second.size() + 1 + 7) / 8 * 8 - 1, ' ');
        w.b += l;
    }
    w.i32(4);
    w.i32(static_cast<int32_t>(idx.size()));
    for (int32_t i : idx) w.i32(i);
}
void end_dict(W &w) {
    w.i32(999);
    w.i32(0);
}
std::string ints(bool big, const std::vector<int32_t> &v) {
    W w;
    w.big = big;
    for (int32_t x : v) w.i32(x);
    return w.b;
}

using Case = std::vector<std::string>; /* one 8-byte slot each, file order */

void data_plain(W &w, const std::vector<Case> &cases) {
    for (const auto &c : cases)
        for (const auto &s : c) w.b += s;
}

std::string bytecode(const std::vector<Case> &cases, const std::vector<bool> &is_str, bool big,
                     bool end_marker, double bias = 100.0) {
    std::string out, cmd, pend;
    auto flush = [&]() {
        cmd.resize(8, '\0');
        out += cmd;
        out += pend;
        cmd.clear();
        pend.clear();
    };
    auto code = [&](unsigned char c, const std::string *data) {
        cmd.push_back(static_cast<char>(c));
        if (data) pend += *data;
        if (cmd.size() == 8) flush();
    };
    for (const auto &c : cases) {
        for (size_t i = 0; i < c.size(); i++) {
            const std::string &s = c[i];
            if (is_str[i]) {
                if (s == "        ") code(254, nullptr);
                else code(253, &s);
                continue;
            }
            uint64_t u;
            std::memcpy(&u, s.data(), 8);
            if (big != host_big()) u = sw64(u);
            double v;
            std::memcpy(&v, &u, 8);
            if (u == kSysmis) code(255, nullptr);
            else if (v == std::trunc(v) && v >= 1 - bias && v <= 251 - bias)
                code(static_cast<unsigned char>(v + bias), nullptr);
            else code(253, &s);
        }
    }
    if (end_marker) code(252, nullptr);
    if (!cmd.empty()) flush();
    return out;
}

void data_zlib(W &w, const std::string &stream, size_t block, double bias = 100.0) {
    const int64_t zheader_ofs = static_cast<int64_t>(w.b.size());
    std::vector<std::string> comp;
    std::vector<size_t> usz;
    for (size_t off = 0; off < stream.size(); off += block) {
        const std::string chunk = stream.substr(off, block);
        duckdb_miniz::mz_ulong bound = duckdb_miniz::mz_compressBound(chunk.size());
        std::string c(bound, '\0');
        REQUIRE(duckdb_miniz::mz_compress(reinterpret_cast<unsigned char *>(&c[0]), &bound,
                                          reinterpret_cast<const unsigned char *>(chunk.data()),
                                          chunk.size()) == duckdb_miniz::MZ_OK);
        c.resize(bound);
        comp.push_back(c);
        usz.push_back(chunk.size());
    }
    int64_t total = 0;
    for (const auto &c : comp) total += static_cast<int64_t>(c.size());
    w.i64(zheader_ofs);
    w.i64(zheader_ofs + 24 + total);
    w.i64(24 * static_cast<int64_t>(comp.size() + 1));
    for (const auto &c : comp) w.b += c;
    w.i64(-static_cast<int64_t>(bias));
    w.i64(0);
    w.i32(static_cast<int32_t>(block));
    w.i32(static_cast<int32_t>(comp.size()));
    int64_t uofs = zheader_ofs, cofs = zheader_ofs + 24;
    for (size_t k = 0; k < comp.size(); k++) {
        w.i64(uofs);
        w.i64(cofs);
        w.i32(static_cast<int32_t>(usz[k]));
        w.i32(static_cast<int32_t>(comp[k].size()));
        uofs += static_cast<int64_t>(usz[k]);
        cofs += static_cast<int64_t>(comp[k].size());
    }
}

std::string write_file(const std::string &name, const std::string &bytes) {
    const std::string path = parqit_test::tmp_path(name);
    std::ofstream f(path, std::ios::binary);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return path;
}

/* the small reference file: id (numeric), sex (A1), city (A20), score (F8.2) */
struct Ref {
    std::vector<Case> cases;
    std::vector<bool> is_str;
};
Ref reference(bool big) {
    Ref r;
    r.is_str = {false, true, true, true, true, false};
    auto row = [&](double id, const std::string &sex, const std::string &city, double score) {
        Case c{num8(id, big), str8(sex)};
        std::string cc = city;
        cc.resize(24, ' ');
        c.push_back(cc.substr(0, 8));
        c.push_back(cc.substr(8, 8));
        c.push_back(cc.substr(16, 8));
        c.push_back(std::isnan(score) ? bits8(kSysmis, big) : num8(score, big));
        r.cases.push_back(c);
    };
    row(1, "M", "Braga", 12.5);
    row(2, "F", "Porto   with blanks ", 3);
    row(-7, "", "", NAN);
    row(1234567.25, "X", "Lisboa", -99);
    return r;
}
void reference_dict(W &w) {
    numeric(w, "ID", "Identifier");
    string_var(w, 1, "SEX", "Sex");
    string_var(w, 20, "CITY");
    numeric(w, "SCORE", "", 0, {}, fmt(5, 8, 2));
}

std::string reference_file(bool big, int compression, bool end_marker, int32_t ncases, size_t zblock = 64) {
    W w;
    w.big = big;
    const Ref r = reference(big);
    header(w, compression == 2 ? "$FL3" : "$FL2", compression, 0, ncases, "Reference file");
    reference_dict(w);
    rec7(w, 3, 4, 8, ints(big, {21, 0, 0, -1, 1, 1, big ? 1 : 2, 65001}));
    end_dict(w);
    if (compression == 0) data_plain(w, r.cases);
    else if (compression == 1) w.b += bytecode(r.cases, r.is_str, big, end_marker);
    else data_zlib(w, bytecode(r.cases, r.is_str, big, end_marker), zblock);
    return w.b;
}

void check_reference(const std::string &path) {
    const Dictionary d = read_dictionary(path);
    REQUIRE(d.vars.size() == 4);
    CHECK(d.vars[0].name == "ID");
    CHECK(d.vars[0].label == "Identifier");
    CHECK(d.vars[2].width == 20);
    CHECK(d.vars[2].segments.size() == 1);
    CHECK(d.vars[2].segments[0].nslots == 3);
    CHECK(d.nslots == 6);
    CHECK(d.file_label == "Reference file");
    CHECK(d.encoding_used == "UTF-8");
    CaseReader r(path, d);
    std::string s;
    REQUIRE(r.next());
    CHECK(r.number(d.vars[0]) == 1);
    r.raw_string(d.vars[1], &s);
    CHECK(s == "M");
    r.raw_string(d.vars[2], &s);
    CHECK(s == "Braga");
    CHECK(r.number(d.vars[3]) == 12.5);
    REQUIRE(r.next());
    r.raw_string(d.vars[2], &s);
    CHECK(s == "Porto   with blanks"); /* inner blanks kept, trailing trimmed */
    CHECK(r.number(d.vars[3]) == 3);
    REQUIRE(r.next());
    CHECK(r.number(d.vars[0]) == -7);
    r.raw_string(d.vars[1], &s);
    CHECK(s.empty());
    CHECK(r.is_sysmis(d.vars[3]));
    REQUIRE(r.next());
    CHECK(r.number(d.vars[0]) == 1234567.25);
    CHECK(r.number(d.vars[3]) == -99);
    CHECK_FALSE(r.next());
    CHECK(r.cases_read() == 4);
}

std::string error_of(const std::string &path) {
    try {
        const Dictionary d = read_dictionary(path);
        CaseReader r(path, d);
        while (r.next()) {
        }
    } catch (const SavError &e) {
        return e.what();
    }
    return "";
}

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("SPSS-READ-1: every data layout and byte order reads the same cases") {
    int n = 0;
    for (bool big : {false, true}) {
        for (int comp : {0, 1, 2}) {
            for (int32_t ncases : {4, -1}) {
                if (comp == 0 && ncases < 0) continue; /* uncompressed: counted by length */
                const std::string path = write_file(
                    "spss_ref_" + std::to_string(n++) + ".sav",
                    reference_file(big, comp, /*end_marker=*/ncases < 0, ncases));
                INFO("big=" << big << " compression=" << comp << " ncases=" << ncases);
                check_reference(path);
                std::remove(path.c_str());
            }
        }
    }
    /* a .zsav whose bytecode stream spans many small ZLIB blocks, with a
     * case cut across two blocks */
    const std::string path = write_file("spss_zblocks.zsav", reference_file(false, 2, true, 4, 13));
    check_reference(path);
    std::remove(path.c_str());
}

TEST_CASE("SPSS-READ-1: header, machine records and case counts") {
    W w;
    header(w, "$FL2", 0, 1, -1, "  label with trailing blanks   ");
    numeric(w, "WT");
    rec7(w, 3, 4, 8, ints(false, {25, 0, 1, -1, 1, 1, 2, 1252}));
    W fp;
    fp.f64(-std::numeric_limits<double>::max());
    fp.f64(std::numeric_limits<double>::max());
    fp.f64(-std::numeric_limits<double>::max());
    rec7(w, 4, 8, 3, fp.b);
    W nc;
    nc.i64(1);
    nc.i64(2);
    rec7(w, 16, 8, 2, nc.b);
    rec7(w, 10, 1, 9, "prodinfo ");
    rec7(w, 24, 1, 5, "<xml>");
    end_dict(w);
    data_plain(w, {{num8(1.5, false)}, {num8(2.5, false)}, {num8(9, false)}});
    const std::string path = write_file("spss_header.sav", w.b);
    const Dictionary d = read_dictionary(path);
    CHECK(d.ncases == 2); /* the 64-bit record wins over the header's -1 */
    CHECK(d.weight_var == 0);
    CHECK(d.file_label == "  label with trailing blanks"); /* only the right padding goes */
    CHECK(d.product == "@(#) SPSS DATA FILE parqit unit test");
    CHECK(d.creation_date == "26 Sep 26");
    CHECK(d.creation_time == "21:40:00");
    CHECK(d.machine_version[0] == 25);
    CHECK(d.encoding_used == "windows-1252");
    CHECK(d.declared_encoding == "code page 1252 (windows-1252)");
    CHECK(d.product_info == "prodinfo");
    REQUIRE(d.ignored.size() == 1);
    CHECK(contains(d.ignored[0], "7/24"));
    CaseReader r(path, d);
    int n = 0;
    while (r.next()) n++;
    CHECK(n == 2); /* exactly the declared count; the trailing case is not data */
    std::remove(path.c_str());
}

TEST_CASE("SPSS-READ-1: long names, very long strings and display parameters") {
    for (bool big : {false, true}) {
        W w;
        w.big = big;
        header(w, "$FL2", 1, 0, 1);
        numeric(w, "Q1");
        /* a 600-byte string: three segments of widths 255, 255 and 96 */
        string_var(w, 255, "LONGS");
        string_var(w, 255, "LONGS0");
        string_var(w, 96, "LONGS1");
        string_var(w, 12, "NOTE");
        const std::string ln = "Q1=satisfaction_with_public_services_overall\tLONGS=Long_Text\tNOTE=note";
        rec7(w, 13, 1, static_cast<int32_t>(ln.size()), ln);
        const std::string vls = std::string("LONGS=00600") + '\0' + '\t';
        rec7(w, 14, 1, static_cast<int32_t>(vls.size()), vls);
        /* display: one set of 3 per variable record: Q1, 3 segments, NOTE */
        rec7(w, 11, 4, 15, ints(big, {3, 10, 1, 1, 60, 0, 1, 5, 0, 1, 5, 0, 2, 9, 2}));
        end_dict(w);
        std::string text;
        for (int k = 0; k < 600; k++) text += static_cast<char>('A' + k % 26);
        text[299] = ' '; /* a blank inside the string survives */
        /* segment data: 255 used bytes each for the first two (256 with
         * padding), 90 used of 96 in the last */
        std::string seg0 = text.substr(0, 255), seg1 = text.substr(255, 255), seg2 = text.substr(510, 90);
        seg0.resize(256, ' ');
        seg1.resize(256, ' ');
        seg2.resize(96, ' ');
        Case c{num8(4, big)};
        std::vector<bool> is_str{false};
        for (const std::string *seg : {&seg0, &seg1, &seg2})
            for (size_t k = 0; k < seg->size(); k += 8) {
                c.push_back(seg->substr(k, 8));
                is_str.push_back(true);
            }
        c.push_back(str8("hello"));
        c.push_back(str8(""));
        is_str.push_back(true);
        is_str.push_back(true);
        w.b += bytecode({c}, is_str, big, true);
        const std::string path = write_file("spss_vls.sav", w.b);
        const Dictionary d = read_dictionary(path);
        REQUIRE(d.vars.size() == 3);
        CHECK(d.vars[0].name == "satisfaction_with_public_services_overall");
        CHECK(d.vars[0].short_name == "Q1");
        CHECK(d.vars[1].name == "Long_Text");
        CHECK(d.vars[1].width == 600);
        CHECK(d.vars[1].print.width == 600);
        CHECK(format_name(d.vars[1].print) == "A600");
        REQUIRE(d.vars[1].segments.size() == 3);
        CHECK(d.vars[1].segments[2].used == 90);
        CHECK(d.vars[2].name == "note");
        CHECK(d.vars[0].measure == 3);
        CHECK(d.vars[0].display_width == 10);
        CHECK(d.vars[0].alignment == 1);
        CHECK(d.vars[1].measure == 1);
        CHECK(d.vars[1].display_width == 60);
        CHECK(d.vars[2].measure == 2);
        CHECK(d.vars[2].alignment == 2);
        CaseReader r(path, d);
        REQUIRE(r.next());
        std::string s;
        r.raw_string(d.vars[1], &s);
        CHECK(s == text);
        r.raw_string(d.vars[2], &s);
        CHECK(s == "hello");
        CHECK_FALSE(r.next());
        std::remove(path.c_str());
    }
}

TEST_CASE("SPSS-READ-1: value labels, missing values and attributes") {
    const bool big = false;
    W w;
    header(w, "$FL2", 0, 0, 1);
    numeric(w, "Q1", "Question", 2, {98, 99});                  /* dict index 1 */
    numeric(w, "Q2", "", -3, {-9, -1, 97});                     /* 2: range + discrete */
    /* 3: LO THRU 0, LO written as -DBL_MAX (SPSS 21 and later) */
    var(w, 0, "INC", fmt(5, 8, 2), "", -2, {bits8(kSysmis, big), num8(0, big)});
    string_var(w, 3, "SEX", "", 1, {"X"});                      /* 4 */
    string_var(w, 12, "CITY");                                  /* 5-6 */
    labels(w, {{num8(1, big), "Yes"}, {num8(2, big), "No"}, {num8(98, big), "Refused"},
               {num8(1, big), "Yes!"}},
           {1, 2});
    labels(w, {{str8("M"), "Male"}, {str8("F"), "Female"}, {"XYZ12345", "too wide"}}, {4});
    /* long string labels (7/21) and missing values (7/22) for CITY */
    W l21;
    l21.i32(4);
    l21.b += "CITY";
    l21.i32(12);
    l21.i32(2);
    l21.i32(5);
    l21.b += "Braga";
    l21.i32(9);
    l21.b += "Minho cap";
    l21.i32(12);
    l21.b += "Porto       ";
    l21.i32(4);
    l21.b += "Invi";
    rec7(w, 21, 1, static_cast<int32_t>(l21.b.size()), l21.b);
    W l22;
    l22.i32(4);
    l22.b += "CITY";
    l22.b += static_cast<char>(2);
    l22.i32(8);
    l22.b += "NA      ";
    l22.b += "none    ";
    rec7(w, 22, 1, static_cast<int32_t>(l22.b.size()), l22.b);
    const std::string va = "Q1:$@Role('1'\n)Source('survey'\n'wave 2'\n)/Q2:Note('x'\n)";
    rec7(w, 18, 1, static_cast<int32_t>(va.size()), va);
    const std::string fa = "Producer('BPLIM'\n)";
    rec7(w, 17, 1, static_cast<int32_t>(fa.size()), fa);
    rec7(w, 7, 1, 20, "$mr=C 1 Q1 Q2       ");
    std::vector<std::string> lines = {"First document line", "Second"};
    w.i32(6);
    w.i32(2);
    for (const auto &l : lines) w.text(l, 80);
    end_dict(w);
    data_plain(w, {{num8(1, big), num8(-5, big), num8(-3, big), str8("X"), str8("NA"), str8("")}});
    const std::string path = write_file("spss_labels.sav", w.b);
    const Dictionary d = read_dictionary(path);
    REQUIRE(d.vars.size() == 5);
    const Variable &q1 = d.vars[0];
    REQUIRE(q1.labels.size() == 3); /* 1 labelled twice: the last label wins */
    CHECK(q1.labels[0].number == 1);
    CHECK(q1.labels[0].label == "Yes!");
    CHECK(q1.missing.values == std::vector<double>{98, 99});
    CHECK(q1.missing.matches(99));
    CHECK_FALSE(q1.missing.matches(97));
    CHECK(d.vars[1].labels.size() == 3); /* the set is shared by Q1 and Q2 */
    const MissingSpec &m2 = d.vars[1].missing;
    CHECK(m2.has_range);
    CHECK(m2.lo == -9);
    CHECK(m2.hi == -1);
    CHECK(m2.values == std::vector<double>{97});
    CHECK(m2.matches(-5));
    CHECK(m2.matches(97));
    CHECK_FALSE(m2.matches(0));
    const MissingSpec &m3 = d.vars[2].missing;
    CHECK(m3.lo_open);
    CHECK(m3.matches(-1e300));
    CHECK(m3.matches(0));
    CHECK_FALSE(m3.matches(0.5));
    const Variable &sex = d.vars[3];
    REQUIRE(sex.labels.size() == 2); /* the key wider than the variable is dropped */
    CHECK(sex.labels[1].text_key == "F");
    CHECK(sex.missing.strings == std::vector<std::string>{"X"});
    bool warned = false;
    for (const auto &wn : d.warnings) warned = warned || contains(wn, "wider than the variable");
    CHECK(warned);
    const Variable &city = d.vars[4];
    REQUIRE(city.labels.size() == 2);
    CHECK(city.labels[1].text_key == "Porto");
    CHECK(city.labels[1].label == "Invi");
    CHECK(city.missing.strings == std::vector<std::string>{"NA", "none"});
    CHECK(q1.role == 1);
    REQUIRE(q1.attributes.size() == 1);
    CHECK(q1.attributes[0].name == "Source");
    CHECK(q1.attributes[0].values == std::vector<std::string>{"survey", "wave 2"});
    REQUIRE(d.vars[1].attributes.size() == 1);
    CHECK(d.vars[1].attributes[0].values[0] == "x");
    REQUIRE(d.file_attributes.size() == 1);
    CHECK(d.file_attributes[0].values[0] == "BPLIM");
    CHECK(d.mrsets == "$mr=C 1 Q1 Q2");
    CHECK(d.documents == std::vector<std::string>{"First document line", "Second"});
    std::remove(path.c_str());
}

TEST_CASE("SPSS-READ-1: text is decoded from the declared code page") {
    /* windows-1252 declared by record 7/20: 0xE9 is e-acute */
    for (int variant = 0; variant < 3; variant++) {
        W w;
        header(w, "$FL2", 0, 0, 1, std::string("Ol\xE1"));
        numeric(w, "X", std::string("r\xE9sum\xE9"));
        string_var(w, 8, "S");
        if (variant == 0) rec7(w, 20, 1, 12, "windows-1252");
        if (variant == 1) rec7(w, 3, 4, 8, ints(false, {20, 0, 0, -1, 1, 1, 2, 28591}));
        if (variant == 2) rec7(w, 20, 1, 5, "UTF-8"); /* declared UTF-8, bytes are not */
        end_dict(w);
        data_plain(w, {{num8(1, false), str8(std::string("caf\xE9"))}});
        const std::string path = write_file("spss_enc.sav", w.b);
        const Dictionary d = read_dictionary(path);
        CHECK(d.file_label == "Ol\xC3\xA1");
        CHECK(d.vars[0].label == "r\xC3\xA9sum\xC3\xA9");
        CaseReader r(path, d);
        REQUIRE(r.next());
        std::string raw, out;
        r.raw_string(d.vars[1], &raw);
        const bool transcoded = decode_text(d, raw.data(), raw.size(), &out);
        CHECK(out == "caf\xC3\xA9");
        CHECK(transcoded == (variant == 2)); /* only a UTF-8 file transcodes */
        if (variant == 2) CHECK(d.transcoded_meta == 2);
        std::remove(path.c_str());
    }
    /* ENC-3: every language's code pages are decoded, by name or by number;
     * one parqit does not know is refused, and encoding() overrides it */
    struct Cp {
        const char *record20;
        int code_page;
        const char *label, *utf8;
    } cps[] = {
        {"windows-1251", 0, "\xcf\xf0\xe8\xe2\xe5\xf2", "\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82"},
        {nullptr, 1251, "\xcf\xf0\xe8\xe2\xe5\xf2", "\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82"},
        {"GBK", 0, "\xd6\xd0\xce\xc4\xc5\xae", "\xe4\xb8\xad\xe6\x96\x87\xe5\xa5\xb3"},
        {nullptr, 936, "\xd6\xd0\xce\xc4\xc5\xae", "\xe4\xb8\xad\xe6\x96\x87\xe5\xa5\xb3"},
        {"Shift_JIS", 0, "\x93\xfa\x96\x7b", "\xe6\x97\xa5\xe6\x9c\xac"},
        {nullptr, 932, "\x93\xfa\x96\x7b", "\xe6\x97\xa5\xe6\x9c\xac"},
        {nullptr, 949, "\xc7\xd1\xb1\xb9", "\xed\x95\x9c\xea\xb5\xad"},
        {nullptr, 950, "\xa4\xa4\xa4\xe5", "\xe4\xb8\xad\xe6\x96\x87"},
        {nullptr, 1253, "\xc1\xe8\xde\xed\xe1", "\xce\x91\xce\xb8\xce\xae\xce\xbd\xce\xb1"},
        {nullptr, 1256, "\xe3\xd1\xcd\xc8\xc7", "\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7"},
        {nullptr, 874, "\xca\xc7\xd1\xca\xb4\xd5", "\xe0\xb8\xaa\xe0\xb8\xa7\xe0\xb8\xb1\xe0\xb8\xaa\xe0\xb8\x94\xe0\xb8\xb5"},
        {nullptr, 20866, "\xf0\xd2\xc9\xd7\xc5\xd4", "\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82"},
    };
    for (const auto &cp : cps) {
        W w;
        header(w, "$FL2", 0, 0, 1, std::string(cp.label));
        numeric(w, "X");
        if (cp.record20) rec7(w, 20, 1, static_cast<int>(std::strlen(cp.record20)), cp.record20);
        else rec7(w, 3, 4, 8, ints(false, {20, 0, 0, -1, 1, 1, 2, cp.code_page}));
        end_dict(w);
        data_plain(w, {{num8(1, false)}});
        const std::string path = write_file("spss_enc_cp.sav", w.b);
        const Dictionary d = read_dictionary(path);
        CHECK_MESSAGE(d.file_label == cp.utf8, (cp.record20 ? cp.record20 : std::to_string(cp.code_page)));
        CHECK(d.undecodable_meta == 0);
        std::remove(path.c_str());
    }
    W w;
    header(w, "$FL2", 0, 0, 1);
    numeric(w, "X");
    rec7(w, 20, 1, 11, "ISO-2022-JP");
    end_dict(w);
    data_plain(w, {{num8(1, false)}});
    const std::string path = write_file("spss_enc_bad.sav", w.b);
    CHECK(contains(error_of(path), "ISO-2022-JP"));
    ReadOptions opt;
    opt.encoding = "latin1";
    CHECK(read_dictionary(path, opt).encoding_used == "latin1");
    opt.encoding = "koi8-r";
    CHECK(read_dictionary(path, opt).encoding_used == "koi8-r");
    opt.encoding = "klingon";
    CHECK_THROWS_AS(read_dictionary(path, opt), SavError);
    opt.encoding = "utf-16";
    CHECK_THROWS_AS(read_dictionary(path, opt), SavError);
    std::remove(path.c_str());
}

TEST_CASE("SPSS-READ-1: the header's own texts are decoded like the rest") {
    /* a localized product string in a legacy code page, declared (1252) or
     * mis-declared (UTF-8): decoded, never a JSON library error */
    for (int variant = 0; variant < 2; variant++) {
        W w;
        header(w, "$FL2", 0, 0, 1, "", 100.0,
               std::string("@(#) SPSS DATA FILE Vers\xE3o portugu\xEAs"));
        numeric(w, "X");
        if (variant == 0) rec7(w, 20, 1, 12, "windows-1252");
        else rec7(w, 20, 1, 5, "UTF-8");
        end_dict(w);
        data_plain(w, {{num8(1, false)}});
        const std::string path = write_file("spss_prod.sav", w.b);
        Plan plan;
        REQUIRE_NOTHROW(plan = make_plan(path));
        CHECK(plan.dict.product == "@(#) SPSS DATA FILE Vers\xC3\xA3o portugu\xC3\xAAs");
        CHECK(contains(plan.kv_metadata_sql, "Vers\xC3\xA3o portugu\xC3\xAAs"));
        CHECK(plan.transcoded_meta == (variant == 1 ? 1 : 0));
        std::remove(path.c_str());
    }
}

TEST_CASE("SPSS-READ-1: malformed files fail loudly, naming the place") {
    const bool big = false;
    auto base = [&](int compression, int32_t ncases) {
        W w;
        header(w, "$FL2", compression, 0, ncases);
        numeric(w, "X");
        string_var(w, 8, "S");
        end_dict(w);
        return w;
    };
    /* not an SPSS file / a portable file / truncated header */
    std::string e = error_of(write_file("spss_bad0.sav", "PK\x03\x04 not spss at all, just bytes"));
    CHECK(contains(e, "not an SPSS system file"));
    std::string por(500, ' ');
    por.replace(464, 8, "SPSSPORT");
    CHECK(contains(error_of(write_file("spss_bad1.por", por)), "portable file"));
    CHECK(contains(error_of(write_file("spss_bad2.sav", "$FL2@(#) SPSS")), "truncated"));
    /* fewer cases than the header declares */
    {
        W w = base(0, 3);
        data_plain(w, {{num8(1, big), str8("a")}, {num8(2, big), str8("b")}});
        CHECK(contains(error_of(write_file("spss_bad3.sav", w.b)), "truncated"));
    }
    /* a case cut in the middle */
    {
        W w = base(0, -1);
        data_plain(w, {{num8(1, big), str8("a")}});
        w.b += num8(2, big);
        CHECK(contains(error_of(write_file("spss_bad4.sav", w.b)), "truncated"));
    }
    /* bytecode 254 (blanks) standing for a number, 255 (sysmis) for a string */
    {
        W w = base(1, 1);
        std::string cmd(8, '\0');
        cmd[0] = static_cast<char>(254);
        cmd[1] = static_cast<char>(254);
        w.b += cmd;
        CHECK(contains(error_of(write_file("spss_bad5.sav", w.b)), "code 254"));
        W w2 = base(1, 1);
        cmd[0] = static_cast<char>(101);
        cmd[1] = static_cast<char>(255);
        w2.b += cmd;
        CHECK(contains(error_of(write_file("spss_bad6.sav", w2.b)), "code 255"));
        W w3 = base(1, 2);
        cmd[0] = static_cast<char>(101);
        cmd[1] = static_cast<char>(252); /* end of data inside a case */
        w3.b += cmd;
        CHECK(contains(error_of(write_file("spss_bad7.sav", w3.b)), "252"));
    }
    /* an unknown record type, with its offset */
    {
        W w;
        header(w, "$FL2", 0, 0, 0);
        numeric(w, "X");
        w.i32(5);
        const std::string e8 = error_of(write_file("spss_bad8.sav", w.b));
        CHECK(contains(e8, "unknown record type 5"));
        CHECK(contains(e8, "byte offset"));
    }
    /* a string without its continuation records */
    {
        W w;
        header(w, "$FL2", 0, 0, 0);
        var(w, 20, "S", fmt(1, 20, 0));
        numeric(w, "X");
        end_dict(w);
        CHECK(contains(error_of(write_file("spss_bad9.sav", w.b)), "continuation"));
    }
    /* a ZLIB block that does not inflate */
    {
        W w;
        w.big = big;
        header(w, "$FL3", 2, 0, 1);
        numeric(w, "X");
        end_dict(w);
        std::string stream = bytecode({{num8(3.25, big)}}, {false}, big, true);
        data_zlib(w, stream, 64);
        const size_t data_at = w.b.find("$FL3") + 176 + 32 + 8 + 24; /* first block */
        w.b[data_at + 4] ^= 0x5A;
        CHECK(contains(error_of(write_file("spss_bad10.zsav", w.b)), "ZLIB block"));
    }
    /* no variables at all */
    {
        W w;
        header(w, "$FL2", 0, 0, 0);
        end_dict(w);
        CHECK(contains(error_of(write_file("spss_bad11.sav", w.b)), "no variables"));
    }
}

TEST_CASE("SPSS-READ-1: the plan types, codes, names and metadata") {
    const bool big = false;
    W w;
    header(w, "$FL2", 0, 0, 5);
    const double day = 86400.0;
    const double d2020 = (141428.0 + 18322.0) * day; /* 2020-03-01 in SPSS seconds */
    numeric(w, "BDAY", "", 0, {}, fmt(20, 11, 0));          /* DATE11, whole days */
    numeric(w, "HALF", "", 0, {}, fmt(20, 11, 0));          /* DATE11 with noon */
    numeric(w, "TS", "", 0, {}, fmt(22, 23, 2));            /* DATETIME23.2 */
    numeric(w, "T", "", 0, {}, fmt(21, 8, 0));              /* TIME8 */
    numeric(w, "DUR", "", 0, {}, fmt(21, 8, 0));            /* TIME8 beyond a day */
    numeric(w, "R", "", -2, {90, 200});                     /* many range values */
    numeric(w, "Q", "", -3, {-9, -1, 99});                  /* range + discrete */
    numeric(w, "A.B", "a name Stata cannot hold");
    labels(w, {{num8(-8, big), "Don't know"}, {num8(1.5, big), "one and a half"}}, {7});
    rec7(w, 20, 1, 5, "UTF-8");
    end_dict(w);
    std::vector<Case> cases;
    for (int k = 0; k < 5; k++) {
        Case c;
        c.push_back(num8(d2020 + k * day, big));
        c.push_back(num8(d2020 + (k == 2 ? 0.5 * day : 0.0), big));
        c.push_back(num8(d2020 + 3661.25, big));
        c.push_back(num8(3600.0 * k, big));
        c.push_back(num8(k == 4 ? 90000.0 : 60.0, big));
        c.push_back(num8(100.0 + k, big)); /* 5 distinct in-range values */
        c.push_back(num8(k == 0 ? -8 : (k == 1 ? -2 : (k == 2 ? 99 : 5)), big));
        c.push_back(num8(k, big));
        cases.push_back(c);
    }
    data_plain(w, cases);
    const std::string path = write_file("spss_plan.sav", w.b);
    const Plan plan = make_plan(path);
    REQUIRE(plan.cols.size() == 8);
    CHECK(plan.ncases == 5);
    CHECK(plan.cols[0].kind == OutKind::Date);
    CHECK(plan.cols[0].stata_format == "%tdDD-Mon-CCYY");
    CHECK(plan.cols[1].kind == OutKind::Timestamp);
    CHECK(plan.cols[1].stata_format == "%tcDD-Mon-CCYY");
    CHECK(plan.cols[2].kind == OutKind::Timestamp);
    CHECK(plan.cols[2].stata_format == "%tcDD-Mon-CCYY_HH:MM:SS.ss");
    CHECK(plan.cols[3].kind == OutKind::Time);
    CHECK(plan.cols[3].stata_format == "%tcHH:MM:SS");
    CHECK(plan.cols[4].kind == OutKind::Double);
    /* R: 90 THRU 200 observed 100..104 -> .a..e ascending, companion written */
    const ColumnSpec &r = plan.cols[5];
    CHECK(r.companion);
    CHECK(r.companion_name == "_parqit_xm_R");
    CHECK(r.code_of(100) == 1);
    CHECK(r.code_of(104) == 5);
    CHECK(r.user_missing_cells == 5);
    /* Q: discrete 99 first (.a), then the labelled -8 inside the range (.b),
     * then the observed unlabelled -2 (.c) */
    const ColumnSpec &q = plan.cols[6];
    CHECK(q.code_of(99) == 1);
    CHECK(q.code_of(-8) == 2);
    CHECK(q.code_of(-2) == 3);
    CHECK(q.code_of(5) == 0);
    CHECK(q.user_missing_cells == 3);
    CHECK(plan.cols[7].name == "A.B");
    CHECK(plan.cols[7].stata_name == "A_B");
    CHECK(contains(plan.kv_metadata_sql, "\"spss_missing_map\":\".a=99 .b=-8 .c=-2\""));
    CHECK(contains(plan.kv_metadata_sql, "\"spss_missing\":\"-9 THRU -1, 99\""));
    /* the -8 label goes native and to .b; the 1.5 label only to the char */
    CHECK(contains(plan.kv_metadata_sql, "[\"-8\",\"Don''t know\"]"));
    CHECK(contains(plan.kv_metadata_sql, "[\".b\",\"Don''t know\"]"));
    CHECK(contains(plan.kv_metadata_sql,
                   "[[-8,\\\"Don''t know\\\"],[1.5,\\\"one and a half\\\"]]"));
    CHECK(contains(plan.kv_metadata_sql, "'parqit.xmissing': '{\"Q\":\"_parqit_xm_Q\",\"R\":\"_parqit_xm_R\"}'"));
    std::remove(path.c_str());
}

TEST_CASE("SPSS-READ-1: more user-missing values than extended missing codes share .z") {
    const bool big = false;
    W w;
    header(w, "$FL2", 0, 0, 40);
    numeric(w, "R", "", -2, {0, 1000});
    end_dict(w);
    std::vector<Case> cases;
    for (int k = 0; k < 40; k++) cases.push_back({num8(10.0 + k * 0.5, big)});
    data_plain(w, cases);
    const std::string path = write_file("spss_overflow.sav", w.b);
    const Plan plan = make_plan(path);
    const ColumnSpec &c = plan.cols[0];
    CHECK(c.overflow_code == 26);
    CHECK(c.code_of(10.0) == 1);
    CHECK(c.code_of(22.0) == 25);  /* the 25th distinct value */
    CHECK(c.code_of(22.5) == 26);  /* beyond: shared .z */
    CHECK(c.code_of(29.5) == 26);
    bool noted = false;
    for (const auto &n : plan.notes) noted = noted || contains(n, "share .z");
    CHECK(noted);
    std::remove(path.c_str());
}

TEST_CASE("SPSS-READ-1: second conversions and display formats") {
    int64_t us = 0;
    int32_t days = 0;
    REQUIRE(seconds_to_unix_us(12219379200.0, &us));
    CHECK(us == 0);
    REQUIRE(seconds_to_unix_us(12219379200.0 + 0.25, &us));
    CHECK(us == 250000);
    REQUIRE(seconds_to_unix_us(12219379200.0 - 0.5, &us));
    CHECK(us == -500000);
    CHECK_FALSE(seconds_to_unix_us(1e300, &us));
    CHECK_FALSE(seconds_to_unix_us(NAN, &us));
    REQUIRE(seconds_to_unix_days(141428.0 * 86400.0, &days));
    CHECK(days == 0);
    REQUIRE(seconds_to_unix_days(86400.0, &days)); /* 15 Oct 1582 */
    CHECK(days == -141427);
    CHECK_FALSE(seconds_to_unix_days(141428.0 * 86400.0 + 1, &days));
    REQUIRE(seconds_to_time_us(3723.5, &us));
    CHECK(us == 3723500000LL);
    CHECK_FALSE(seconds_to_time_us(86400.0, &us));
    CHECK_FALSE(seconds_to_time_us(-1.0, &us));
    auto f = [](int type, int w, int d, OutKind k) { return stata_format_for(Format{type, w, d}, k); };
    CHECK(f(kFmtF, 8, 2, OutKind::Double) == "%8.2f");
    CHECK(f(kFmtF, 2, 2, OutKind::Double) == "%2.1f");
    CHECK(f(kFmtCOMMA, 12, 2, OutKind::Double) == "%12.2fc");
    CHECK(f(kFmtDOT, 10, 2, OutKind::Double) == "%10,2fc");
    CHECK(f(kFmtN, 6, 0, OutKind::Double) == "%06.0f");
    CHECK(f(kFmtE, 10, 3, OutKind::Double) == "%10.3e");
    CHECK(f(kFmtA, 20, 0, OutKind::Varchar).empty());
    CHECK(f(kFmtADATE, 10, 0, OutKind::Date) == "%tdNN/DD/CCYY");
    CHECK(f(kFmtQYR, 8, 0, OutKind::Date) == "%tdq_!Q_CCYY");
    CHECK(f(kFmtWKYR, 10, 0, OutKind::Date) == "%tdww_!W!K_CCYY");
    CHECK(f(kFmtMOYR, 6, 0, OutKind::Date) == "%tdMon_YY");
    CHECK(f(kFmtDATETIME, 17, 0, OutKind::Timestamp) == "%tcDD-Mon-CCYY_HH:MM");
    CHECK(f(kFmtYMDHMS, 19, 0, OutKind::Timestamp) == "%tcCCYY-NN-DD_HH:MM:SS");
    CHECK(f(kFmtTIME, 11, 2, OutKind::Time) == "%tcHH:MM:SS.ss");
    CHECK(f(kFmtMTIME, 5, 0, OutKind::Time) == "%tcMM:SS");
    CHECK(format_name(Format{kFmtF, 8, 0}) == "F8.0");
    CHECK(format_name(Format{kFmtDATETIME, 23, 2}) == "DATETIME23.2");
    CHECK(format_name(Format{kFmtN, 6, 0}) == "N6");
    CHECK(format_name(Format{99, 8, 0}).empty());
}

TEST_CASE("SPSS-READ-1: a file with no cases gives a Parquet file with every column") {
    for (int comp : {0, 1, 2}) {
        W w;
        header(w, comp == 2 ? "$FL3" : "$FL2", comp, 0, comp == 0 ? 0 : -1);
        numeric(w, "N");
        string_var(w, 30, "S");
        end_dict(w);
        if (comp == 1) w.b += bytecode({}, {false, true, true, true, true}, false, true);
        if (comp == 2) data_zlib(w, bytecode({}, {false, true, true, true, true}, false, true), 64);
        const std::string path = write_file("spss_empty.sav", w.b);
        const std::string out = parqit_test::tmp_path("spss_empty.parquet");
        auto plan = std::make_shared<const Plan>(make_plan(path));
        CHECK(plan->ncases == 0);
        CHECK(plan->cols[1].stata_type == "str1");
        parqit::Session &s = parqit::Session::instance();
        REQUIRE(s.ensure_open());
        const std::string token = register_plan(plan);
        std::string err, got;
        REQUIRE_MESSAGE(s.exec("COPY (SELECT * FROM parqit_read_sav(" + parqit::quote_literal(token) +
                                   ")) TO " + parqit::quote_literal(out) + " (FORMAT PARQUET)",
                               &err),
                        err);
        release_plan(token);
        REQUIRE(s.query_scalar("SELECT count(*)::VARCHAR || '/' || (SELECT count(*) FROM "
                               "parquet_schema(" + parqit::quote_literal(out) + ") WHERE "
                               "name IN ('N', 'S'))::VARCHAR FROM read_parquet(" +
                                   parqit::quote_literal(out) + ")",
                               &got, &err));
        CHECK(got == "0/2");
        std::remove(path.c_str());
        std::remove(out.c_str());
    }
}

TEST_CASE("SPSS-READ-1: the table function streams the plan into Parquet") {
    const bool big = true; /* exercise the byte swap on the whole path */
    W w;
    w.big = big;
    header(w, "$FL2", 1, 0, 3);
    numeric(w, "N", "", 1, {-1});
    string_var(w, 10, "S");
    numeric(w, "D", "", 0, {}, fmt(20, 11, 0));
    end_dict(w);
    const double d2020 = (141428.0 + 18322.0) * 86400.0;
    std::vector<Case> cases = {
        {num8(1.5, big), str8("abc"), str8(""), num8(d2020, big)},
        {num8(-1, big), str8("0123456789").substr(0, 8), std::string("89      "), bits8(kSysmis, big)},
        {bits8(kSysmis, big), str8(""), str8(""), num8(d2020 + 86400.0, big)}};
    w.b += bytecode(cases, {false, true, true, false}, big, true);
    const std::string path = write_file("spss_table.sav", w.b);
    const std::string out = parqit_test::tmp_path("spss_table.parquet");
    auto plan = std::make_shared<const Plan>(make_plan(path));
    parqit::Session &s = parqit::Session::instance();
    REQUIRE(s.ensure_open());
    const std::string token = register_plan(plan);
    std::string err;
    REQUIRE_MESSAGE(s.exec("COPY (SELECT * FROM parqit_read_sav(" + parqit::quote_literal(token) +
                               ")) TO " + parqit::quote_literal(out) + " (FORMAT PARQUET, " +
                               plan->kv_metadata_sql + ")",
                           &err),
                    err);
    release_plan(token);
    std::string got;
    REQUIRE(s.query_scalar(
        "SELECT string_agg(coalesce(CAST(N AS VARCHAR), 'NULL') || '|' || S || '|' || "
        "coalesce(CAST(D AS VARCHAR), 'NULL') || '|' || CAST(_parqit_xm_N AS VARCHAR), ';' "
        "ORDER BY file_row_number) FROM read_parquet(" + parqit::quote_literal(out) +
            ", file_row_number = true)",
        &got, &err));
    CHECK(got == "1.5|abc|2020-03-01|0;NULL|0123456789|NULL|1;NULL||2020-03-02|0");
    REQUIRE(s.query_scalar("SELECT decode(value) FROM parquet_kv_metadata(" +
                               parqit::quote_literal(out) + ") WHERE decode(key) = 'parqit.xmissing'",
                           &got, &err));
    CHECK(got == "{\"N\":\"_parqit_xm_N\"}");
    /* an unknown token is a bind error, not a crash */
    CHECK_FALSE(s.exec("SELECT * FROM parqit_read_sav('nope')", &err));
    CHECK(contains(err, "no prepared SPSS conversion"));
    std::remove(path.c_str());
    std::remove(out.c_str());
}

TEST_CASE("SPSS-PARALLEL-1: the parallel passes plan and write what the serial ones do") {
    /* 3001 cases in blocks of 7 on four workers, in all three layouts: a
     * user-missing range with more distinct values than codes (and -0.0 before
     * 0.0 inside it, which only the uncompressed layout can hold), strings with
     * a byte that is not UTF-8, dates and system-missing values */
    const bool big = false;
    const int ncases = 3001;
    std::vector<Case> cases;
    const std::vector<bool> is_str = {false, true, true, false};
    for (int i = 0; i < ncases; i++) {
        double n = i % 3 == 0 ? 2000.0 + i : static_cast<double>(i % 101);
        if (i == 4) n = -0.0;
        const std::string s = i % 11 == 0 ? "caf\xE9" : "s" + std::to_string(i % 50) + "-long";
        const double d = (141428.0 + 18322.0 + i % 400) * 86400.0;
        cases.push_back({i % 97 == 0 ? bits8(kSysmis, big) : num8(n, big), str8(s.substr(0, 8)),
                         str8(s.size() > 8 ? s.substr(8) : ""), num8(d, big)});
    }
    parqit::Session &s = parqit::Session::instance();
    REQUIRE(s.ensure_open());
    for (int comp : {0, 1, 2}) {
        INFO("compression=" << comp);
        W w;
        w.big = big;
        header(w, comp == 2 ? "$FL3" : "$FL2", comp, 0, ncases);
        numeric(w, "N", "", -2, {0.0, 1000.0});
        string_var(w, 10, "S");
        numeric(w, "D", "", 0, {}, fmt(20, 11, 0));
        rec7(w, 3, 4, 8, ints(big, {21, 0, 0, -1, 1, 1, 2, 65001}));
        end_dict(w);
        if (comp == 0) data_plain(w, cases);
        else if (comp == 1) w.b += bytecode(cases, is_str, big, false);
        else data_zlib(w, bytecode(cases, is_str, big, false), 4096);
        const std::string path = write_file("spss_parallel_" + std::to_string(comp) + ".sav", w.b);
        ReadOptions par;
        par.threads = 4;
        par.block_cases = 7;
        const Plan a = make_plan(path), b = make_plan(path, par);
        CHECK(a.ncases == ncases);
        CHECK(b.ncases == a.ncases);
        REQUIRE(b.cols.size() == a.cols.size());
        for (size_t i = 0; i < a.cols.size(); i++) {
            CHECK(b.cols[i].kind == a.cols[i].kind);
            CHECK(b.cols[i].stata_type == a.cols[i].stata_type);
            CHECK(b.cols[i].stata_format == a.cols[i].stata_format);
            CHECK(b.cols[i].overflow_code == a.cols[i].overflow_code);
            CHECK(b.cols[i].companion == a.cols[i].companion);
            CHECK(b.cols[i].user_missing_cells == a.cols[i].user_missing_cells);
            CHECK(b.cols[i].transcoded_cells == a.cols[i].transcoded_cells);
            CHECK(b.cols[i].max_bytes == a.cols[i].max_bytes);
            REQUIRE(b.cols[i].codes.size() == a.cols[i].codes.size());
            for (size_t k = 0; k < a.cols[i].codes.size(); k++) {
                CHECK(std::memcmp(&b.cols[i].codes[k].first, &a.cols[i].codes[k].first, 8) == 0);
                CHECK(b.cols[i].codes[k].second == a.cols[i].codes[k].second);
            }
        }
        CHECK(b.notes == a.notes);
        CHECK(b.kv_metadata_sql == a.kv_metadata_sql);
        CHECK(b.transcoded_cells == a.transcoded_cells);
        CHECK(a.cols[0].overflow_code > 0); /* more distinct range values than codes */
        CHECK(a.cols[0].companion);
        for (const auto &c : a.cols[0].codes)
            if (c.first == 0.0) CHECK(std::signbit(c.first) == (comp == 0)); /* the first zero kept */
        /* the Arrow batches write the table function's file */
        auto plan = std::make_shared<const Plan>(a);
        const std::string out_tf = parqit_test::tmp_path("spss_parallel_tf.parquet"),
                          out_ar = parqit_test::tmp_path("spss_parallel_arrow.parquet");
        const std::string token = register_plan(plan);
        std::string err;
        REQUIRE_MESSAGE(s.exec("COPY (SELECT * FROM parqit_read_sav(" + parqit::quote_literal(token) +
                                   ")) TO " + parqit::quote_literal(out_tf) + " (FORMAT PARQUET, " +
                                   plan->kv_metadata_sql + ")",
                               &err),
                        err);
        release_plan(token);
        SavArrowSource src(plan, 4, 7);
        src.start();
        REQUIRE(duckdb_arrow_scan(s.con(), "spss_parallel_view",
                                  reinterpret_cast<duckdb_arrow_stream>(src.stream())) == DuckDBSuccess);
        REQUIRE_MESSAGE(s.exec("COPY (SELECT * FROM spss_parallel_view) TO " + parqit::quote_literal(out_ar) +
                                   " (FORMAT PARQUET, " + plan->kv_metadata_sql + ")",
                               &err),
                        err);
        CHECK(src.finish().empty());
        s.exec("DROP VIEW IF EXISTS spss_parallel_view", &err);
        auto rows = [&](const std::string &f) {
            std::string got;
            REQUIRE(s.query_scalar(
                "SELECT string_agg(CAST(N AS VARCHAR) || '|' || S || '|' || CAST(D AS VARCHAR) || '|' || "
                "CAST(_parqit_xm_N AS VARCHAR), ';' ORDER BY file_row_number) FROM read_parquet(" +
                    parqit::quote_literal(f) + ", file_row_number = true)",
                &got, &err));
            return got;
        };
        auto describe = [&](const std::string &f) {
            std::string got;
            REQUIRE(s.query_scalar("SELECT string_agg(column_name || ' ' || column_type, ',') FROM "
                                   "(DESCRIBE SELECT * FROM read_parquet(" + parqit::quote_literal(f) + "))",
                                   &got, &err));
            return got;
        };
        auto kv = [&](const std::string &f) {
            std::string got;
            REQUIRE(s.query_scalar("SELECT string_agg(decode(key) || '=' || decode(value), ';' ORDER BY "
                                   "decode(key)) FROM parquet_kv_metadata(" + parqit::quote_literal(f) + ")",
                                   &got, &err));
            return got;
        };
        CHECK(rows(out_ar) == rows(out_tf));
        CHECK(describe(out_ar) == describe(out_tf));
        CHECK(kv(out_ar) == kv(out_tf));
        CHECK(contains(rows(out_ar), "|caf\xC3\xA9|")); /* the byte decoded the same way */
        std::remove(out_tf.c_str());
        std::remove(out_ar.c_str());
        /* a file that changed after it was profiled is refused, worded as before */
        if (comp == 1) {
            std::string changed = w.b;
            changed.resize(changed.size() - 8);
            { std::ofstream f(path, std::ios::binary | std::ios::trunc); f.write(changed.data(), static_cast<std::streamsize>(changed.size())); }
            SavArrowSource late(plan, 4, 7);
            late.start();
            REQUIRE(duckdb_arrow_scan(s.con(), "spss_parallel_late",
                                      reinterpret_cast<duckdb_arrow_stream>(late.stream())) == DuckDBSuccess);
            CHECK_FALSE(s.exec("COPY (SELECT * FROM spss_parallel_late) TO " + parqit::quote_literal(out_ar) +
                                   " (FORMAT PARQUET)",
                               &err));
            CHECK(contains(late.finish(), "changed while it was being converted"));
            s.exec("DROP VIEW IF EXISTS spss_parallel_late", &err);
            std::remove(out_ar.c_str());
        }
        std::remove(path.c_str());
    }
    /* a truncated file fails at the same case with the same message on four workers */
    {
        W w;
        header(w, "$FL2", 1, 0, ncases);
        numeric(w, "N");
        end_dict(w);
        std::vector<Case> few;
        for (int i = 0; i < 100; i++) few.push_back({num8(1000.5 + i, false)});
        w.b += bytecode(few, {false}, false, false);
        const std::string path = write_file("spss_parallel_cut.sav", w.b);
        std::string e1, e4;
        try {
            make_plan(path);
        } catch (const SavError &e) {
            e1 = e.what();
        }
        ReadOptions par;
        par.threads = 4;
        par.block_cases = 7;
        try {
            make_plan(path, par);
        } catch (const SavError &e) {
            e4 = e.what();
        }
        CHECK(contains(e1, "truncated"));
        CHECK(e4 == e1);
        std::remove(path.c_str());
    }
}

TEST_CASE("A5: a failing worker, allocation or thread ends the parallel SPSS conversion with its error") {
    /* 300 cases in blocks of 7 (43 blocks) on four workers. PARQIT_TEST_FAULT
     * throws std::bad_alloc at one place; the stream is read as the engine
     * reads it, alone, and every run must end within 5 seconds: with the
     * error, or (a worker that cannot be created, LIFE-018) with every case */
    const int ncases = 300;
    std::vector<Case> cases;
    for (int i = 0; i < ncases; i++) cases.push_back({num8(1.5 + i, false), str8("s" + std::to_string(i % 9))});
    W w;
    header(w, "$FL2", 0, 0, ncases);
    numeric(w, "N");
    string_var(w, 8, "S");
    end_dict(w);
    data_plain(w, cases);
    const std::string path = write_file("spss_a5.sav", w.b);
    auto plan = std::make_shared<const Plan>(make_plan(path));
    struct Outcome {
        int64_t rows = 0;
        bool failed = false;
        std::string message, finish;
    };
    auto run = [&](const std::string &fault, int64_t stop_after) {
        parqit_test::ScopedEnv env("PARQIT_TEST_FAULT", fault);
        auto src = std::make_shared<SavArrowSource>(plan, 4, 7);
        std::packaged_task<Outcome()> task([src, stop_after] {
            Outcome o;
            src->start();
            ArrowArrayStream *st = src->stream();
            for (int64_t k = 0; stop_after < 0 || k < stop_after; k++) {
                ArrowArray a{};
                if (st->get_next(st, &a) != 0) {
                    o.failed = true;
                    o.message = st->get_last_error(st);
                    break;
                }
                if (!a.release) break;
                o.rows += a.length;
                a.release(&a);
            }
            o.finish = src->finish();
            return o;
        });
        auto done = task.get_future();
        std::thread(std::move(task)).detach();
        REQUIRE_MESSAGE(done.wait_for(std::chrono::seconds(5)) == std::future_status::ready, "hung: " << fault);
        return done.get();
    };
    for (int rep = 0; rep < 10; rep++) {
        const Outcome clean = run("", -1);
        CHECK_FALSE(clean.failed);
        CHECK(clean.rows == ncases);
        CHECK(clean.finish.empty());
        for (const char *site : {"spss_batch", "spss_convert", "spss_ready"})
            for (int block : {0, 3, 42}) {
                const std::string fault = std::string(site) + ":" + std::to_string(block);
                INFO(fault);
                const Outcome o = run(fault, -1);
                CHECK(o.failed);
                CHECK(o.rows <= 7 * block);
                CHECK_FALSE(o.message.empty());
                CHECK(o.finish == o.message);
            }
        const Outcome fewer = run("spss_thread:2", -1);
        CHECK_FALSE(fewer.failed);
        CHECK(fewer.rows == ncases);
        const Outcome no_driver = run("spss_driver:0", -1);
        CHECK(no_driver.failed);
        CHECK(contains(no_driver.message, "could not start the SPSS conversion"));
        const Outcome cancelled = run("", 2); /* the engine stops reading */
        CHECK(cancelled.rows == 14);
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

TEST_CASE("A2 (F2): a wide file's blocks stay within 64 MB and reserve only the cases left") {
    /* whole row groups for a narrow file, whole vectors of 2048 down to 2048
     * cases, then any number down to one case (a floor of 2048 made a block of
     * 2.7 GB for 60 cases of long strings) */
    Dictionary d;
    const std::vector<std::pair<size_t, int64_t>> want = {{1, 122880},   {68, 122880},   {69, 120832},
                                                          {1000, 8192},  {4096, 2048},   {4097, 2047},
                                                          {166400, 50},  {9000000, 1}};
    for (const auto &w : want) {
        d.nslots = w.first;
        INFO("slots " << w.first);
        CHECK(block_cases_for(d) == w.second);
        CHECK((block_cases_for(d) * static_cast<int64_t>(w.first) * 8 <= (int64_t(64) << 20) ||
               block_cases_for(d) == 1));
    }
    /* 50 cases of 2000 numbers: a block may hold 4096 cases, but the
     * producer reserves the 50 the file declares, and fills them unzeroed;
     * with the count not declared it reserves one block (64 MB at most) */
    const int nv = 2000, nc = 50;
    std::vector<Case> cases(nc);
    for (int i = 0; i < nc; i++)
        for (int v = 0; v < nv; v++) cases[static_cast<size_t>(i)].push_back(num8(i * 10000.0 + v, false));
    for (int32_t declared : {nc, -1}) {
        INFO("declared " << declared);
        W w;
        header(w, "$FL2", 0, 0, declared);
        for (int v = 0; v < nv; v++) numeric(w, "V" + std::to_string(v));
        end_dict(w);
        data_plain(w, cases);
        const std::string path = write_file("spss_a2_wide.sav", w.b);
        const Plan serial = make_plan(path);
        REQUIRE(serial.dict.nslots == static_cast<size_t>(nv));
        const int64_t block = block_cases_for(serial.dict);
        CHECK(block == 4096);
        std::atomic<int64_t> seen{0};
        BlockStats stats;
        const int64_t read = for_each_block(
            path, serial.dict, 4, block, [&](const CaseBlock &b, int) { seen += b.n; }, nullptr, &stats);
        const int64_t peak = stats.peak_raw;
        CHECK(stats.workers == (declared >= 0 ? 1 : 4)); /* A3: one declared block, no thread started */
        /* G2: a count known or estimated plans the workers when none is
         * declared (this payload holds one block) */
        BlockStats planned;
        CHECK(planned_cases(serial.dict) == nc);
        CHECK(for_each_block(path, serial.dict, 4, block, [](const CaseBlock &, int) {}, nullptr, &planned, 0,
                             planned_cases(serial.dict)) == nc);
        CHECK(planned.workers == 1);
        CHECK(read == nc);
        CHECK(seen.load() == nc);
        CHECK(peak > 0);
        if (declared >= 0) CHECK(peak <= int64_t(nc) * nv * 8);
        else CHECK(peak <= block * nv * 8);
        /* the parallel passes still plan and write what the serial ones do */
        ReadOptions par;
        par.threads = 4;
        const Plan parallel = make_plan(path, par);
        CHECK(parallel.kv_metadata_sql == serial.kv_metadata_sql);
        CHECK(parallel.notes == serial.notes);
        auto plan = std::make_shared<const Plan>(serial);
        parqit::Session &s = parqit::Session::instance();
        REQUIRE(s.ensure_open());
        const std::string out_tf = parqit_test::tmp_path("spss_a2_tf.parquet"),
                          out_ar = parqit_test::tmp_path("spss_a2_arrow.parquet");
        std::string err;
        {
            const PlanRegistration reg(plan);
            REQUIRE_MESSAGE(s.exec("COPY (SELECT * FROM parqit_read_sav(" + parqit::quote_literal(reg.token()) +
                                       ")) TO " + parqit::quote_literal(out_tf) + " (FORMAT PARQUET)",
                                   &err),
                            err);
        }
        SavArrowSource src(plan, 4);
        src.start();
        REQUIRE(duckdb_arrow_scan(s.con(), "spss_a2_view", reinterpret_cast<duckdb_arrow_stream>(src.stream())) ==
                DuckDBSuccess);
        REQUIRE_MESSAGE(s.exec("COPY (SELECT * FROM spss_a2_view) TO " + parqit::quote_literal(out_ar) +
                                   " (FORMAT PARQUET)",
                               &err),
                        err);
        CHECK(src.finish().empty());
        CHECK(src.batches() == 1);
        CHECK(src.workers() == 1); /* G2: planned on the profile's count, declared or not */
        s.exec("DROP VIEW IF EXISTS spss_a2_view", &err);
        std::string diff;
        REQUIRE(s.query_scalar("SELECT (SELECT count(*) FROM (SELECT * FROM read_parquet(" +
                                   parqit::quote_literal(out_ar) + ") EXCEPT ALL SELECT * FROM read_parquet(" +
                                   parqit::quote_literal(out_tf) + "))) + (SELECT count(*) FROM (SELECT * FROM "
                                   "read_parquet(" + parqit::quote_literal(out_tf) + ") EXCEPT ALL SELECT * FROM "
                                   "read_parquet(" + parqit::quote_literal(out_ar) + "))) + abs((SELECT count(*) "
                                   "FROM read_parquet(" + parqit::quote_literal(out_ar) + ")) - " +
                                   std::to_string(nc) + ")",
                               &diff, &err));
        CHECK(diff == "0");
        std::remove(out_tf.c_str());
        std::remove(out_ar.c_str());
        std::remove(path.c_str());
    }
}

TEST_CASE("A4: the conversion's memory stays within its budgets and always progresses") {
    /* 300 cases in blocks of 7 on four workers, read by four consumers the
     * way the engine's Arrow scan reads: under one lock, each releases its
     * previous batch and takes the next. Budgets of one byte admit one block
     * at a time (a block wider than the budget goes alone); a held-back block
     * 0, a stopped reader and a failing worker all end within 5 seconds */
    const int ncases = 300;
    std::vector<Case> cases;
    for (int i = 0; i < ncases; i++) cases.push_back({num8(0.5 + i, false), str8("t" + std::to_string(i % 13))});
    W w;
    header(w, "$FL2", 0, 0, ncases);
    numeric(w, "N");
    string_var(w, 8, "S");
    end_dict(w);
    data_plain(w, cases);
    const std::string path = write_file("spss_a4.sav", w.b);
    auto plan = std::make_shared<const Plan>(make_plan(path));
    struct Outcome {
        int64_t rows = 0, peak_batch = 0, peak_raw = 0;
        bool failed = false;
        std::string finish;
    };
    auto run = [&](int64_t batch_budget, int64_t raw_budget, const std::string &delay, const std::string &fault,
                   int64_t stop_after) {
        parqit_test::ScopedEnv d("PARQIT_TEST_DELAY", delay), f("PARQIT_TEST_FAULT", fault);
        auto src = std::make_shared<SavArrowSource>(plan, 4, 7, batch_budget, raw_budget);
        std::packaged_task<Outcome()> task([src, stop_after] {
            Outcome o;
            src->start();
            ArrowArrayStream *st = src->stream();
            std::mutex scan;
            std::atomic<int64_t> rows{0}, taken{0};
            std::atomic<bool> failed{false};
            auto consumer = [&] {
                ArrowArray held{};
                for (;;) {
                    std::lock_guard<std::mutex> lk(scan);
                    if (held.release) held.release(&held); /* under the scan's lock, as arrow.cpp does */
                    if (failed || (stop_after >= 0 && taken >= stop_after)) return;
                    ArrowArray next{};
                    if (st->get_next(st, &next) != 0) {
                        failed = true;
                        return;
                    }
                    if (!next.release) return;
                    taken++;
                    rows += next.length;
                    held = next;
                }
            };
            std::vector<std::thread> consumers;
            for (int c = 0; c < 4; c++) consumers.emplace_back(consumer);
            for (auto &t : consumers) t.join();
            o.finish = src->finish();
            o.rows = rows;
            o.failed = failed;
            o.peak_batch = src->peak_batch_bytes();
            o.peak_raw = src->peak_raw_bytes();
            return o;
        });
        auto done = task.get_future();
        std::thread(std::move(task)).detach();
        REQUIRE_MESSAGE(done.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
                        "hung: " << batch_budget << " " << raw_budget << " " << delay << " " << fault);
        return done.get();
    };
    const int64_t block_raw = 7 * static_cast<int64_t>(plan->dict.nslots) * 8;
    for (int rep = 0; rep < 5; rep++) {
        const Outcome wide = run(0, 0, "", "", -1);
        CHECK_FALSE(wide.failed);
        CHECK(wide.rows == ncases);
        CHECK(wide.finish.empty());
        CHECK(wide.peak_batch <= kBatchBudgetBytes);
        CHECK(wide.peak_raw <= kRawBudgetBytes);
        const Outcome alone = run(1, 1, "", "", -1);
        CHECK_FALSE(alone.failed);
        CHECK(alone.rows == ncases);
        CHECK(alone.peak_raw <= block_raw); /* one block alive at a time */
        const Outcome slow0 = run(64, 64, "spss_convert:0:200", "", -1);
        CHECK_FALSE(slow0.failed);
        CHECK(slow0.rows == ncases);
        const Outcome stopped = run(64, 64, "spss_convert:2:100", "", 2);
        CHECK(stopped.rows == 14);
        CHECK(stopped.finish.empty());
        const Outcome failing = run(64, 64, "spss_convert:0:100", "spss_convert:1", -1);
        CHECK(failing.failed);
        CHECK(failing.rows == 7 * 1);
        CHECK_FALSE(failing.finish.empty());
    }
    std::remove(path.c_str());
}

TEST_CASE("A3: the SPSS route is chosen from the dictionary, before any case is read") {
    /* 100 cases in blocks of 7: 15 blocks */
    std::vector<Case> cases;
    for (int i = 0; i < 100; i++) cases.push_back({num8(i * 1.5, false), str8("c" + std::to_string(i % 7))});
    auto file = [&](int32_t declared, int compression, const std::string &name) {
        W w;
        header(w, "$FL2", compression, 0, declared);
        numeric(w, "N");
        string_var(w, 8, "S");
        end_dict(w);
        if (compression == 0) data_plain(w, cases);
        else w.b += bytecode(cases, {false, true}, false, false);
        return write_file(name, w.b);
    };
    const std::string plain = file(100, 0, "spss_a3_plain.sav"), undeclared = file(-1, 0, "spss_a3_undeclared.sav"),
                      packed = file(-1, 1, "spss_a3_packed.sav");
    ReadOptions o;
    o.threads = 4;
    o.block_cases = 7;
    for (const std::string &path : {plain, undeclared}) {
        INFO(path);
        const Dictionary d = read_dictionary(path);
        o.parallel_min_blocks = 15;
        CHECK(parallel_route(d, o));
        o.parallel_min_blocks = 16;
        CHECK_FALSE(parallel_route(d, o));
        o.parallel_min_blocks = 0;
        CHECK(parallel_route(d, o));
        o.route = 0;
        CHECK_FALSE(parallel_route(d, o));
        o.route = 1;
        o.parallel_min_blocks = int64_t(1) << 40;
        CHECK(parallel_route(d, o));
        o.threads = 1; /* one thread is always the serial route */
        CHECK_FALSE(parallel_route(d, o));
        o.threads = 4;
        o.route = -1;
    }
    /* compressed, no count: profiled serially, the conversion's route from the cases counted */
    o.parallel_min_blocks = 15;
    Plan p = make_plan(packed, o);
    CHECK_FALSE(p.profile_parallel);
    CHECK(p.parallel);
    o.parallel_min_blocks = 16;
    p = make_plan(packed, o);
    CHECK_FALSE(p.parallel);
    /* the measured rule (ASSUMPTIONS #229): by compression and thread regime */
    CHECK(parallel_min_blocks(8, Compression::None) == 9);
    CHECK(parallel_min_blocks(48, Compression::Zlib) == 17);
    CHECK(parallel_min_blocks(48, Compression::Bytecode) == 17);
    CHECK(parallel_min_blocks(2, Compression::None) == std::numeric_limits<int64_t>::max());
    CHECK(parallel_min_blocks(7, Compression::Zlib) == 17);
    CHECK(parallel_min_blocks(2, Compression::Bytecode) == 41);
    o.parallel_min_blocks = -1; /* 15 blocks: compressed or few threads stay serial */
    o.threads = 48;
    CHECK(parallel_route(read_dictionary(plain), o));
    o.threads = 4;
    CHECK_FALSE(parallel_route(read_dictionary(plain), o));
    /* the serial and the parallel profile plan the same file, in blocks of 7 */
    for (const std::string &path : {plain, packed}) {
        ReadOptions ser = o, par = o;
        ser.route = 0;
        par.route = 1;
        par.block_cases = 7;
        const Plan a = make_plan(path, ser), b = make_plan(path, par);
        CHECK_FALSE(a.profile_parallel);
        CHECK(b.profile_parallel);
        CHECK(a.ncases == b.ncases);
        REQUIRE(a.cols.size() == b.cols.size());
        for (size_t i = 0; i < a.cols.size(); i++) {
            CHECK(a.cols[i].kind == b.cols[i].kind);
            CHECK(a.cols[i].stata_type == b.cols[i].stata_type);
            CHECK(a.cols[i].max_bytes == b.cols[i].max_bytes);
        }
        CHECK(a.kv_metadata_sql == b.kv_metadata_sql);
        CHECK(a.notes == b.notes);
    }
    for (const std::string &path : {plain, undeclared, packed}) std::remove(path.c_str());
}

TEST_CASE("G4: the measured SPSS route just below, at and just above each threshold, at 7 and 8 threads") {
    /* blocks of 7 cases; the first parallel size of a threshold of t blocks is
     * 7 * (t - 1) + 1 cases. Only the dictionary decides. */
    ReadOptions o;
    o.block_cases = 7;
    o.route = -1;
    o.parallel_min_blocks = -1; /* the measured rule, as the plugin asks */
    for (int threads : {7, 8, 48})
        for (Compression c : {Compression::None, Compression::Bytecode, Compression::Zlib}) {
            INFO("threads " << threads << ", compression " << static_cast<int>(c));
            o.threads = threads;
            Dictionary d;
            d.compression = c;
            d.nslots = 2;
            const int64_t t = parallel_min_blocks(threads, c);
            if (t == std::numeric_limits<int64_t>::max()) {
                d.ncases = int64_t(7) << 40;
                CHECK_FALSE(parallel_route(d, o));
                continue;
            }
            d.ncases = 7 * (t - 1);
            CHECK_FALSE(parallel_route(d, o));
            d.ncases = 7 * (t - 1) + 1;
            CHECK(parallel_route(d, o));
            d.ncases = 7 * t + 1;
            CHECK(parallel_route(d, o));
        }
    CHECK(parallel_min_blocks(7, Compression::None) == std::numeric_limits<int64_t>::max());
    CHECK(parallel_min_blocks(8, Compression::None) == 9);
    CHECK(parallel_min_blocks(7, Compression::Bytecode) == 41);
    CHECK(parallel_min_blocks(8, Compression::Bytecode) == 17);
    CHECK(parallel_min_blocks(7, Compression::Zlib) == 17);
    CHECK(parallel_min_blocks(8, Compression::Zlib) == 17);
    /* no declared count, uncompressed: its payload of 56, then 57 cases */
    Dictionary u;
    u.nslots = 2;
    u.data_offset = 1000;
    u.file_size = 1000 + 56 * 16;
    o.threads = 8;
    CHECK(planned_cases(u) == 56);
    CHECK_FALSE(parallel_route(u, o));
    u.file_size += 16;
    CHECK(planned_cases(u) == 57);
    CHECK(parallel_route(u, o));
    u.compression = Compression::Bytecode; /* compressed: counted by the profile */
    CHECK(planned_cases(u) == -1);
    CHECK_FALSE(parallel_route(u, o));
}

TEST_CASE("B2: a .zsav's blocks inflated ahead give the one-thread reader's bytes and errors") {
    /* 3000 cases of a number and an 8-byte string, bytecode in ZLIB blocks of
     * 97 bytes: every slot read with the blocks inflated ahead on 4 threads is
     * the one-thread reader's; a reader that stops early returns; a corrupted
     * block and a corrupt code deep in the stream fail alike, after the same
     * cases, with the same message (where() names the reader's position) */
    std::vector<Case> cases;
    for (int i = 0; i < 3000; i++)
        cases.push_back({num8(i % 7 == 0 ? i * 1.25 : i % 50, false), str8(i % 3 ? "ab" + std::to_string(i % 11) : "")});
    std::string stream = bytecode(cases, {false, true}, false, false);
    auto zsav = [&](const std::string &bc, const std::string &name) {
        W w;
        header(w, "$FL3", 2, 0, 3000);
        numeric(w, "N");
        string_var(w, 8, "S");
        end_dict(w);
        data_zlib(w, bc, 97);
        return std::make_pair(write_file(name, w.b), w.b);
    };
    auto read_all = [](const std::string &p, int ahead, int64_t stop_after, std::string *err) {
        std::vector<uint64_t> slots;
        try {
            const Dictionary d = read_dictionary(p);
            CaseReader r(p, d, ahead);
            for (int64_t k = 0; (stop_after < 0 || k < stop_after) && r.next(); k++)
                slots.insert(slots.end(), r.slots(), r.slots() + d.nslots);
        } catch (const std::exception &e) {
            *err = e.what();
        }
        return slots;
    };
    const auto good = zsav(stream, "spss_b2.zsav");
    const Dictionary d = read_dictionary(good.first);
    REQUIRE(d.zblocks.size() > 100);
    std::string e0, e1, e2;
    const auto serial = read_all(good.first, 0, -1, &e0);
    CHECK(serial.size() == 3000 * d.nslots);
    CHECK(read_all(good.first, 4, -1, &e1) == serial);
    CHECK(e0.empty());
    CHECK(e1.empty());
    CHECK(read_all(good.first, 4, 10, &e2).size() == 10 * d.nslots);
    CHECK(e2.empty());
    /* corrupted compressed bytes in an early, a middle and the last block */
    int failed = 0;
    for (size_t k : {size_t(3), d.zblocks.size() / 2, d.zblocks.size() - 1}) {
        INFO("block " << k);
        std::string bytes = good.second;
        bytes[d.zblocks[k].compressed_ofs + d.zblocks[k].compressed_size / 2] ^= 0x5A;
        const std::string bad = write_file("spss_b2_bad.zsav", bytes);
        std::string a, b;
        const auto sa = read_all(bad, 0, -1, &a), sb = read_all(bad, 4, -1, &b);
        CHECK(sa == sb);
        CHECK(a == b);
        failed += !a.empty();
        std::remove(bad.c_str());
    }
    CHECK(failed == 3);
    /* code 254 (eight blanks) for a number in the 500th command block (of 750) */
    size_t off = 0;
    for (int blk = 0; blk < 500; blk++) {
        int lits = 0;
        for (int c = 0; c < 8; c++) lits += static_cast<unsigned char>(stream[off + c]) == 253;
        off += 8 + 8 * lits;
    }
    REQUIRE(off < stream.size());
    stream[off] = static_cast<char>(254);
    const auto corrupt = zsav(stream, "spss_b2_code.zsav");
    std::string a, b;
    const auto sa = read_all(corrupt.first, 0, -1, &a), sb = read_all(corrupt.first, 4, -1, &b);
    CHECK(sa == sb);
    CHECK(contains(a, "code 254"));
    CHECK(contains(a, "ZLIB block"));
    CHECK(a == b);
    std::remove(good.first.c_str());
    std::remove(corrupt.first.c_str());
}

TEST_CASE("Codex final: a failed prefetched ZLIB block fails twice within five seconds") {
    W w;
    header(w, "$FL3", 2, 0, 200);
    numeric(w, "N");
    end_dict(w);
    std::vector<Case> cases;
    for (int i = 0; i < 200; ++i) cases.push_back({num8(i % 50, false)});
    data_zlib(w, bytecode(cases, {false}, false, false), 17);
    const std::string good = write_file("spss_codex_repeat_good.zsav", w.b);
    const Dictionary d = read_dictionary(good);
    REQUIRE(d.zblocks.size() > 1);
    w.b[d.zblocks[0].compressed_ofs] = '\0'; /* corrupt the ZLIB header */
    const std::string bad = write_file("spss_codex_repeat_bad.zsav", w.b);
    std::packaged_task<std::pair<std::string, std::string>()> task([bad, d]() -> std::pair<std::string, std::string> {
        CaseReader reader(bad, d, 4);
        if (reader.ahead_threads() == 0) return std::make_pair("no read-ahead", "");
        auto error = [&] {
            try { reader.next(); }
            catch (const std::exception &e) { return std::string(e.what()); }
            return std::string();
        };
        const std::string first = error();
        return std::make_pair(first, error());
    });
    auto done = task.get_future();
    std::thread(std::move(task)).detach();
    REQUIRE_MESSAGE(done.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
                    "hung on the second request for the failed prefetched block");
    const auto errors = done.get();
    CHECK(contains(errors.first, "does not inflate"));
    CHECK(errors.first == errors.second);
    std::remove(good.c_str());
    std::remove(bad.c_str());
}

namespace {
/* B1: for_each_block's blocks, error and cases read, with the producer
 * decoding (threads 1) or walking while the workers expand (threads 4);
 * `fail_block` makes the work throw on that block */
struct B1Run {
    std::map<int64_t, std::vector<uint64_t>> blocks;
    std::string err;
    int64_t read = -1;
};
B1Run b1_run(const std::string &path, int threads, int64_t block, int64_t fail_block = -1) {
    B1Run out;
    std::mutex m;
    try {
        const Dictionary d = read_dictionary(path);
        out.read = for_each_block(path, d, threads, block, [&](const CaseBlock &b, int) {
            if (b.index == fail_block) throw std::runtime_error("work failed on block " + std::to_string(b.index));
            std::lock_guard<std::mutex> lk(m);
            out.blocks[b.index] = b.slots;
        });
    } catch (const std::exception &e) {
        out.err = e.what();
    }
    return out;
}
/* random bytecode: every code class for each slot type, padding codes (0)
 * anywhere, literals of either endianness */
std::string random_bytecode(std::mt19937_64 &g, int ncases, const std::vector<bool> &is_str, bool big, bool end,
                            int pad_pct) {
    std::string out, cmd, pend;
    auto flush = [&]() {
        cmd.resize(8, '\0');
        out += cmd;
        out += pend;
        cmd.clear();
        pend.clear();
    };
    auto code = [&](unsigned char c, const std::string *data) {
        cmd.push_back(static_cast<char>(c));
        if (data) pend += *data;
        if (cmd.size() == 8) flush();
    };
    std::uniform_int_distribution<int> pct(0, 99), small(1, 251);
    for (int c = 0; c < ncases; c++)
        for (bool str : is_str) {
            while (pct(g) < pad_pct) code(0, nullptr);
            const int kind = pct(g);
            if (str) {
                if (kind < 40) code(254, nullptr);
                else {
                    std::string s(8, ' ');
                    for (char &ch : s) ch = static_cast<char>('a' + pct(g) % 26);
                    code(253, &s);
                }
            } else if (kind < 50) {
                code(static_cast<unsigned char>(small(g)), nullptr);
            } else if (kind < 60) {
                code(255, nullptr);
            } else {
                const std::string lit = num8(static_cast<double>(g() % 100000) / 7.0, big);
                code(253, &lit);
            }
        }
    if (end) code(252, nullptr);
    if (!cmd.empty()) flush();
    return out;
}
std::string b1_file(const std::string &name, const std::string &stream, const std::vector<int> &widths, bool big,
                    int32_t declared, int compression, size_t zblock) {
    W w;
    w.big = big;
    header(w, compression == 2 ? "$FL3" : "$FL2", compression, 0, declared);
    for (size_t v = 0; v < widths.size(); v++) {
        if (widths[v] == 0) numeric(w, "N" + std::to_string(v));
        else string_var(w, widths[v], "S" + std::to_string(v));
    }
    end_dict(w);
    if (compression == 2) data_zlib(w, stream, zblock);
    else w.b += stream;
    return write_file(name, w.b);
}
std::vector<bool> slot_types(const std::vector<int> &widths) {
    std::vector<bool> out;
    for (int wd : widths) {
        if (wd == 0) out.push_back(false);
        else for (int k = 0; k < (wd + 7) / 8; k++) out.push_back(true);
    }
    return out;
}
} // namespace

TEST_CASE("B1: compressed cases walked by the producer and expanded by the workers are the one-thread reader's") {
    std::mt19937_64 g(20261025);
    const std::vector<std::vector<int>> layouts = {{0}, {0, 8}, {0, 16, 0}, {20}, {8, 0, 0, 24}};
    int files = 0;
    for (const auto &widths : layouts)
        for (bool big : {false, true})
            for (int compression : {1, 2})
                for (int variant = 0; variant < 4; variant++) {
                    /* 0: declared exactly; 1: none, end code; 2: none, the file ends; 3: declared fewer */
                    const std::vector<bool> types = slot_types(widths);
                    const int n = 40 + static_cast<int>(g() % 60);
                    const std::string stream = random_bytecode(g, n, types, big, variant == 1, 15);
                    const int32_t declared = variant == 0 ? n : variant == 3 ? n - 7 : -1;
                    const std::string path = b1_file("spss_b1.sav", stream, widths, big, declared, compression, 37 + 13 * variant);
                    for (int64_t block : {1, 2, 3, 5, 7, 8}) {
                        INFO("layout " << widths.size() << " big " << big << " compression " << compression
                                       << " variant " << variant << " block " << block);
                        const B1Run s = b1_run(path, 1, block), p = b1_run(path, 4, block);
                        CHECK(s.err.empty());
                        CHECK(p.err == s.err);
                        CHECK(p.read == s.read);
                        CHECK(p.read == (variant == 3 ? n - 7 : n));
                        CHECK(p.blocks == s.blocks);
                    }
                    files++;
                    std::remove(path.c_str());
                }
    CHECK(files == 80);
}

TEST_CASE("B1: a damaged compressed file fails in the walk as it does decoding, after the same cases") {
    std::mt19937_64 g(25102026);
    const std::vector<int> widths = {0, 8, 0};
    const std::vector<bool> types = slot_types(widths);
    const std::string stream = random_bytecode(g, 12, types, false, false, 10);
    /* every cut of the data (the dictionary stays whole), with blocks of 1 and 3 cases */
    W head;
    header(head, "$FL2", 1, 0, 12);
    for (size_t v = 0; v < widths.size(); v++) {
        if (widths[v] == 0) numeric(head, "N" + std::to_string(v));
        else string_var(head, widths[v], "S" + std::to_string(v));
    }
    end_dict(head);
    int failures = 0;
    for (size_t cut = 0; cut < stream.size(); cut++) {
        const std::string path = write_file("spss_b1_cut.sav", head.b + stream.substr(0, cut));
        for (int64_t block : {1, 3}) {
            INFO("cut " << cut << " block " << block);
            const B1Run s = b1_run(path, 1, block), p = b1_run(path, 4, block);
            CHECK(p.err == s.err);
            CHECK(p.blocks == s.blocks);
            failures += !s.err.empty();
        }
        std::remove(path.c_str());
    }
    CHECK(failures > 0);
    /* a declared count larger than the data, codes that do not fit their slot, an end code inside a case,
     * two defects (the first wins), and a worker's failure before or after a defect (case order) */
    std::string bad = stream;
    size_t first_code = 0;
    const B1Run base_s = b1_run(write_file("spss_b1_ok.sav", head.b + stream), 1, 2);
    REQUIRE(base_s.err.empty());
    auto with = [&](const std::string &bytes, int32_t declared) {
        W w;
        header(w, "$FL2", 1, 0, declared);
        for (size_t v = 0; v < widths.size(); v++) {
            if (widths[v] == 0) numeric(w, "N" + std::to_string(v));
            else string_var(w, widths[v], "S" + std::to_string(v));
        }
        end_dict(w);
        return write_file("spss_b1_bad.sav", w.b + bytes);
    };
    std::vector<std::pair<std::string, int32_t>> cases = {{stream, 20}};
    /* the command blocks' offsets: each is 8 codes then its literals */
    std::vector<size_t> blocks_at;
    for (size_t off = 0; off < stream.size();) {
        blocks_at.push_back(off);
        int lits = 0;
        for (int c = 0; c < 8; c++) lits += static_cast<unsigned char>(stream[off + c]) == 253;
        off += 8 + 8 * lits;
    }
    REQUIRE(blocks_at.size() >= 4);
    for (size_t bi : {size_t(1), blocks_at.size() / 2, blocks_at.size() - 1})
        for (unsigned char code : {static_cast<unsigned char>(252), static_cast<unsigned char>(254),
                                   static_cast<unsigned char>(255), static_cast<unsigned char>(7)})
            for (int pos : {0, 3, 7}) {
                std::string b = stream;
                b[blocks_at[bi] + static_cast<size_t>(pos)] = static_cast<char>(code);
                cases.push_back({b, 12});
            }
    std::string two = stream;
    two[blocks_at[1] + 2] = static_cast<char>(255);
    two[blocks_at[blocks_at.size() - 1] + 1] = static_cast<char>(254);
    cases.push_back({two, 12});
    (void)first_code;
    (void)bad;
    for (const auto &c : cases) {
        const std::string path = with(c.first, c.second);
        for (int64_t block : {1, 2, 5})
            for (int64_t fail_block : {int64_t(-1), int64_t(0), int64_t(3)}) {
                INFO("block " << block << " work fails on " << fail_block);
                const B1Run s = b1_run(path, 1, block, fail_block), p = b1_run(path, 4, block, fail_block);
                CHECK(p.err == s.err);
                if (fail_block < 0) CHECK(p.blocks == s.blocks);
            }
        std::remove(path.c_str());
    }
}

/* ---------------------------------------------- Codex audit of C28 (25 Oct) */

TEST_CASE("Codex finding 1: a failure expanding a compressed block ends a throttled conversion with its error") {
    /* bytecode, 300 cases in blocks of 7 on four workers, so B1 expands each
     * block on its worker; a batch budget of one byte admits one block at a
     * time, so the producer is waiting for room when a worker fails before
     * work(): the run must still end within 5 seconds, with the error */
    const int ncases = 300;
    std::vector<Case> cases;
    for (int i = 0; i < ncases; i++) cases.push_back({num8(1.5 + i, false), str8("s" + std::to_string(i % 9))});
    W w;
    header(w, "$FL2", 1, 0, ncases);
    numeric(w, "N");
    string_var(w, 8, "S");
    end_dict(w);
    w.b += bytecode(cases, {false, true}, false, true);
    const std::string path = write_file("spss_codex1.sav", w.b);
    auto plan = std::make_shared<const Plan>(make_plan(path));
    auto run = [&](const std::string &fault) {
        parqit_test::ScopedEnv env("PARQIT_TEST_FAULT", fault);
        auto src = std::make_shared<SavArrowSource>(plan, 4, 7, 1);
        std::packaged_task<std::pair<std::string, std::string>()> task([src] {
            src->start();
            ArrowArrayStream *st = src->stream();
            std::string message;
            for (;;) {
                ArrowArray a{};
                if (st->get_next(st, &a) != 0) {
                    message = st->get_last_error(st);
                    break;
                }
                if (!a.release) break;
                a.release(&a);
            }
            return std::make_pair(message, src->finish());
        });
        auto done = task.get_future();
        std::thread(std::move(task)).detach();
        REQUIRE_MESSAGE(done.wait_for(std::chrono::seconds(5)) == std::future_status::ready, "hung: " << fault);
        return done.get();
    };
    const auto clean = run("");
    CHECK(clean.first.empty());
    CHECK(clean.second.empty());
    for (int block : {0, 1, 3, 42}) {
        const std::string fault = "spss_expand:" + std::to_string(block);
        INFO(fault);
        const auto o = run(fault);
        CHECK_FALSE(o.first.empty());
        CHECK(o.second == o.first);
    }
    std::remove(path.c_str());
}

namespace {
/* codes only, no literal: small integers, system-missing and padding */
std::string codes_only(std::mt19937_64 &g, int ncases, int nvars) {
    std::string out, cmd;
    auto code = [&](unsigned char c) {
        cmd.push_back(static_cast<char>(c));
        if (cmd.size() == 8) {
            out += cmd;
            cmd.clear();
        }
    };
    for (int c = 0; c < ncases; c++)
        for (int v = 0; v < nvars; v++) {
            if (g() % 7 == 0) code(0);
            code(g() % 5 == 0 ? 255 : static_cast<unsigned char>(1 + g() % 250));
        }
    if (!cmd.empty()) {
        cmd.resize(8, '\0');
        out += cmd;
    }
    return out;
}
struct Rest {
    std::vector<std::vector<uint64_t>> slots;
    std::string outcome;
};
Rest read_rest(CaseReader &r, size_t nslots) {
    Rest s;
    try {
        while (r.next()) s.slots.emplace_back(r.slots(), r.slots() + nslots);
        s.outcome = "end " + std::to_string(r.cases_read());
    } catch (const std::exception &e) {
        s.outcome = std::string("error: ") + e.what();
    }
    return s;
}
} // namespace

TEST_CASE("Codex finding 3: a reader restarted at any case reads what the one-thread reader reads from there") {
    /* every case of every file: the restart point the walk records there, a
     * reader started from it, and the rest of the file it reads (slots, and
     * how it ends, the message included) against the one-thread reader's —
     * bytecode and .zsav with ZLIB blocks small enough that restart points
     * fall on their boundaries, corrupt codes after a boundary, and a damaged
     * last block the declared cases never reach */
    std::mt19937_64 g(26102026);
    int files = 0, restarts = 0, on_boundary = 0;
    auto check = [&](const std::string &path, size_t zblock) {
        const Dictionary d = read_dictionary(path);
        std::vector<CaseReader::Restart> rs;
        std::vector<std::vector<uint64_t>> all;
        std::string outcome;
        {
            CaseReader r(path, d);
            try {
                for (;;) {
                    rs.push_back(r.restart());
                    if (!r.next()) break;
                    all.emplace_back(r.slots(), r.slots() + d.nslots);
                }
                outcome = "end " + std::to_string(r.cases_read());
            } catch (const std::exception &e) {
                outcome = std::string("error: ") + e.what();
            }
        }
        for (size_t k = 0; k < rs.size(); k++) {
            INFO(path << " restart at case " << k << " offset " << rs[k].offset << " cmd_pos " << rs[k].cmd_pos);
            CaseReader r(path, d, rs[k]);
            const Rest s = read_rest(r, d.nslots);
            CHECK(s.outcome == outcome);
            REQUIRE(k <= all.size());
            CHECK(s.slots == std::vector<std::vector<uint64_t>>(all.begin() + static_cast<long>(k), all.end()));
            restarts++;
            if (zblock && rs[k].offset > 0 && rs[k].offset % zblock == 0 && rs[k].cmd_pos < 8) on_boundary++;
        }
        files++;
    };
    const std::vector<std::vector<int>> layouts = {{0}, {0, 8}, {0, 16, 0}, {8, 0, 0, 24}};
    for (const auto &widths : layouts)
        for (int compression : {1, 2})
            for (size_t zblock : {8, 16, 24, 37}) {
                if (compression == 1 && zblock != 8) continue;
                const std::vector<bool> types = slot_types(widths);
                const int n = 20 + static_cast<int>(g() % 30);
                const std::string stream = random_bytecode(g, n, types, false, false, 15);
                check(b1_file("spss_codex3.sav", stream, widths, false, n, compression, zblock),
                      compression == 2 ? zblock : 0);
                /* a code that does not fit its slot, right after a block boundary */
                for (int at = 0; at < 3; at++) {
                    std::string bad = stream;
                    size_t p = 0;
                    for (int skip = 0; p + 8 <= bad.size(); skip++) {
                        size_t lits = 0;
                        for (int i = 0; i < 8; i++) lits += static_cast<unsigned char>(bad[p + i]) == 253;
                        if (skip == 2 + 3 * at) break;
                        p += 8 + 8 * lits;
                    }
                    if (p + 8 > bad.size()) continue;
                    bad[p + 1] = static_cast<char>(widths.size() > 1 ? 254 : 100);
                    check(b1_file("spss_codex3_bad.sav", bad, widths, false, n, compression, zblock),
                          compression == 2 ? zblock : 0);
                }
            }
    /* .zsav whose last ZLIB block holds only padding the declared cases never
     * reach, damaged: the one-thread reader never inflates it, nor may a
     * restarted reader */
    for (int f = 0; f < 60; f++) {
        const int nvars = 1 + static_cast<int>(g() % 4), n = 3 + static_cast<int>(g() % 20);
        const std::string stream = codes_only(g, n, nvars);
        W w;
        header(w, "$FL3", 2, 0, n);
        for (int v = 0; v < nvars; v++) numeric(w, "N" + std::to_string(v));
        end_dict(w);
        data_zlib(w, stream + std::string(8, '\0'), stream.size());
        std::string path = write_file("spss_codex3_tail.zsav", w.b);
        const Dictionary d = read_dictionary(path);
        REQUIRE(d.zblocks.size() == 2);
        std::string bytes = w.b;
        bytes[static_cast<size_t>(d.zblocks[1].compressed_ofs + d.zblocks[1].compressed_size / 2)] ^= 0x5a;
        path = write_file("spss_codex3_tail.zsav", bytes);
        check(path, stream.size());
    }
    CHECK(files > 100);
    CHECK(restarts > 2000);
    CHECK(on_boundary > 20);
}

TEST_CASE("Codex finding 6: the ZLIB read-ahead and the B1 restart readers are bounded by bytes") {
    CHECK(zlib_ahead_window(8, uint64_t(4) << 20) == 16);
    CHECK(zlib_ahead_window(8, uint64_t(32) << 20) == 2);
    CHECK(zlib_ahead_window(8, (uint64_t(32) << 20) + 1) == 0);
    CHECK(zlib_ahead_window(0, 1024) == 0);
    CHECK(zlib_ahead_window(2, 1024) == 8);
    Dictionary d;
    d.compression = Compression::Zlib;
    d.zblocks.resize(3);
    for (auto &b : d.zblocks) {
        b.uncompressed_size = uint32_t(4) << 20;
        b.compressed_size = uint32_t(1) << 20;
    }
    CHECK(b1_restart_fits(d, 48));
    d.zblocks[1].uncompressed_size = uint32_t(64) << 20;
    CHECK_FALSE(b1_restart_fits(d, 48));
    CHECK(b1_restart_fits(d, 7));
    d.compression = Compression::Bytecode;
    CHECK(b1_restart_fits(d, 48));
}

TEST_CASE("Codex round 2, findings 4 and 5: the reading keeps to the read-ahead and restart budgets") {
    /* the bounds decide what the reading does, not only what the helpers
     * answer: with ZLIB blocks of 97 bytes, a read-ahead bound of 150 bytes
     * holds no two blocks (no threads) and one of 400 bytes holds four (four
     * threads of the eight asked for); a restart budget of 100 bytes holds no
     * worker's reader (the producer decodes); the slots are the one-thread
     * reader's every time */
    std::vector<Case> cases;
    for (int i = 0; i < 3000; i++)
        cases.push_back({num8(i % 7 == 0 ? i * 1.25 : i % 50, false), str8(i % 3 ? "ab" + std::to_string(i % 11) : "")});
    W w;
    header(w, "$FL3", 2, 0, 3000);
    numeric(w, "N");
    string_var(w, 8, "S");
    end_dict(w);
    data_zlib(w, bytecode(cases, {false, true}, false, false), 97);
    const std::string path = write_file("spss_codex_r2_45.zsav", w.b);
    const Dictionary d = read_dictionary(path);
    REQUIRE(d.zblocks.size() > 100);
    auto read_all = [&](int ahead, size_t *threads) {
        CaseReader r(path, d, ahead);
        *threads = r.ahead_threads();
        std::vector<uint64_t> slots;
        while (r.next()) slots.insert(slots.end(), r.slots(), r.slots() + d.nslots);
        return slots;
    };
    size_t threads = 99;
    const auto serial = read_all(0, &threads);
    CHECK(threads == 0);
    CHECK(serial.size() == 3000 * d.nslots);
    CHECK(read_all(8, &threads) == serial);
    CHECK(threads == 8);
    {
        parqit_test::ScopedEnv env("PARQIT_TEST_ZLIB_AHEAD_BYTES", "150");
        CHECK(read_all(8, &threads) == serial);
        CHECK(threads == 0);
    }
    {
        parqit_test::ScopedEnv env("PARQIT_TEST_ZLIB_AHEAD_BYTES", "400");
        CHECK(read_all(8, &threads) == serial);
        CHECK(threads == 4);
    }
    auto expand_run = [&](bool *restarted) {
        std::map<int64_t, std::vector<uint64_t>> blocks;
        std::mutex m;
        BlockStats st;
        for_each_block(path, d, 4, 7, [&](const CaseBlock &b, int) {
            std::lock_guard<std::mutex> lk(m);
            blocks[b.index] = b.slots;
        }, nullptr, &st);
        *restarted = st.restart_readers;
        std::vector<uint64_t> all;
        for (const auto &kv : blocks) all.insert(all.end(), kv.second.begin(), kv.second.end());
        return all;
    };
    bool restarted = false;
    CHECK(expand_run(&restarted) == serial);
    CHECK(restarted);
    {
        parqit_test::ScopedEnv env("PARQIT_TEST_B1_BUDGET_BYTES", "100");
        CHECK(expand_run(&restarted) == serial);
        CHECK_FALSE(restarted);
    }
    /* finding 5's arithmetic: the largest inflated block and the largest
     * compressed one, not the largest sum, and no wrap in 32 bits */
    Dictionary z;
    z.compression = Compression::Zlib;
    z.zblocks.resize(2);
    z.zblocks[0].uncompressed_size = uint32_t(32) << 20;
    z.zblocks[0].compressed_size = uint32_t(32) << 20;
    z.zblocks[1].uncompressed_size = uint32_t(64) << 20;
    z.zblocks[1].compressed_size = uint32_t(1) << 20;
    CHECK_FALSE(b1_restart_fits(z, 7)); /* 96 MiB a reader, 672 MiB for seven */
    CHECK(b1_restart_fits(z, 5));
    z.zblocks.resize(1);
    z.zblocks[0].uncompressed_size = 16;
    z.zblocks[0].compressed_size = 0xFFFFFFF8u; /* 8 bytes once summed in 32 bits */
    CHECK_FALSE(b1_restart_fits(z, 1));
    std::remove(path.c_str());
}
