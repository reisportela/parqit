/* SPSS-READ-1: the native SPSS reader (engine/spss_reader, spss_plan,
 * spss_table). The files here are written byte by byte by a small writer that
 * follows the published format, so every layout the reader must handle —
 * both byte orders, the three data layouts, very long strings, every
 * extension record, legacy code pages — and every malformation it must refuse
 * can be pinned without an external program. The Stata verify tests
 * (v127-v130) compare the whole path against independent readers
 * (pyreadstat, Stata's import spss, pyarrow). */
#include "doctest.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "duckdb.h"
#include "engine/session.hpp"
#include "engine/spss_plan.hpp"
#include "engine/spss_reader.hpp"
#include "engine/spss_table.hpp"
#include "miniz.hpp"
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
    /* a code page parqit cannot decode is refused, and encoding() overrides it */
    W w;
    header(w, "$FL2", 0, 0, 1);
    numeric(w, "X");
    rec7(w, 20, 1, 12, "windows-1251");
    end_dict(w);
    data_plain(w, {{num8(1, false)}});
    const std::string path = write_file("spss_enc_bad.sav", w.b);
    CHECK(contains(error_of(path), "windows-1251"));
    ReadOptions opt;
    opt.encoding = "latin1";
    CHECK(read_dictionary(path, opt).encoding_used == "latin1");
    opt.encoding = "koi8-r";
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
