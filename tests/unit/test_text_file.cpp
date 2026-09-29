#include "doctest.h"

#include "engine/legacy_encoding.hpp"
#include "engine/text_file.hpp"
#include "test_tmp.hpp"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>

using parqit::TextDecodeStats;
using parqit::TextEncoding;
using parqit::TextPlan;
using parqit::TextPlanRequest;
using parqit::TextSniff;
using parqit::TextSniffResult;

/* CSV-ENC-1: delimited text in any encoding is read through a UTF-8 copy.
 * The input bytes below are written by hand (not produced by parqit's own
 * tables) and the expected text is the Unicode original, so a table error
 * cannot hide behind a round trip. tests/verify_suite/v136 covers the
 * Stata-facing behaviour with ICU (Stata's ustrfrom) as a second oracle. */

namespace {

std::string write_file(const std::string &name, const std::string &bytes) {
    const std::string path = parqit_test::tmp_path(name);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return path;
}

std::string read_file(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

TextEncoding enc(const char *name) {
    TextEncoding e;
    REQUIRE(parqit::legacy_encoding_parse(name, &e));
    return e;
}

TextSniff sniff(const std::string &bytes, size_t *bom = nullptr, bool *by_lines = nullptr) {
    const std::string path = write_file("parqit_tf_sniff", bytes);
    TextSniffResult r;
    std::string err;
    REQUIRE(parqit::text_file_sniff(path, &r, &err));
    if (bom) *bom = r.bom_bytes;
    if (by_lines) *by_lines = r.by_line_ends;
    return r.kind;
}

/* decodes `bytes` with every chunk size from 1 to 17 and a large one (and a
 * line cap of max_line); all must agree */
std::string decode_all_chunks(const std::string &bytes, TextEncoding e, bool all, size_t skip,
                              TextDecodeStats *st, size_t max_line = parqit::kTextMaxLine) {
    const std::string src = write_file("parqit_tf_src", bytes);
    const std::string dest = parqit_test::tmp_path("parqit_tf_dest");
    std::string first, err;
    for (size_t chunk : {size_t(1), size_t(2), size_t(3), size_t(4), size_t(5), size_t(6),
                         size_t(7), size_t(8), size_t(9), size_t(11), size_t(13), size_t(17),
                         parqit::kTextChunk}) {
        TextDecodeStats one;
        REQUIRE_MESSAGE(parqit::text_file_decode(src, dest, e, all, skip, &one, &err, chunk, max_line),
                        err);
        const std::string out = read_file(dest);
        if (chunk == 1) {
            first = out;
            *st = one;
        }
        CHECK(out == first);
        CHECK(one.undecodable == st->undecodable);
        CHECK(one.lines == st->lines);
        CHECK(one.lines_decoded == st->lines_decoded);
        CHECK(one.lines_kept == st->lines_kept);
        CHECK(one.lines_revalid == st->lines_revalid);
    }
    return first;
}

TextPlan plan(const std::string &bytes, const std::string &encoding, bool all = false,
              bool using_mode = false, bool expect_ok = true, std::string *err_out = nullptr) {
    TextPlanRequest r;
    r.path = write_file("parqit_tf_plan", bytes);
    r.encoding = encoding;
    r.all = all;
    r.check_undeclared = using_mode;
    r.session_default = TextEncoding::Windows1252;
    TextPlan p;
    std::string err;
    const bool ok = parqit::text_file_plan(r, &p, &err);
    CHECK(ok == expect_ok);
    if (err_out) *err_out = err;
    return p;
}

} // namespace

TEST_CASE("CSV-AUDIT: an unchanged UTF-8 file needs no bridge for a single-byte encoding") {
    CHECK_FALSE(plan("id,name\n1,José\n", "cp1252").decode);
    CHECK_FALSE(plan("id,name\n1,Porto\n", "latin1").decode);
    CHECK(plan("id,name\n1,Jos\xe9\n", "cp1252").decode);
    CHECK(plan("id,name\n1,José\n", "cp1252", true).decode);
    TextDecodeStats st;
    CHECK(decode_all_chunks("x\n25%\n", enc("ibm864"), true, 0, &st) == "x\n25٪\n");
    CHECK(st.undecodable == 0);
}

TEST_CASE("CSV-ENC-1: byte-order marks and the NUL pattern of UTF-16 are recognised") {
    size_t bom = 9;
    CHECK(sniff("id,x\n1,2\n", &bom) == TextSniff::Plain);
    CHECK(bom == 0);
    CHECK(sniff("\xEF\xBB\xBFid\n", &bom) == TextSniff::Utf8Bom);
    CHECK(bom == 3);
    CHECK(sniff(std::string("\xFF\xFEi\0d\0", 6), &bom) == TextSniff::Utf16LeBom);
    CHECK(bom == 2);
    CHECK(sniff(std::string("\xFE\xFF\0i\0d", 6), &bom) == TextSniff::Utf16BeBom);
    CHECK(sniff(std::string("\xFF\xFE\0\0i\0\0\0", 8)) == TextSniff::Utf32Bom);
    CHECK(sniff(std::string("\0\0\xFE\xFF\0\0\0i", 8)) == TextSniff::Utf32Bom);
    /* ASCII text in UTF-16 without a mark: a NUL in every other byte */
    CHECK(sniff(std::string("i\0d\0,\0x\0\n\0" "1\0,\0" "2\0\n\0", 18)) == TextSniff::Utf16Le);
    CHECK(sniff(std::string("\0i\0d\0,\0x\0\n", 10)) == TextSniff::Utf16Be);
    /* one stray NUL in UTF-8 text is left to the reader */
    CHECK(sniff(std::string("id,city\n1,Lis\0boa\n2,Porto\n", 26)) == TextSniff::Plain);
    /* NUL padding of a fixed-width export (NUL bytes at both parities): the reader's too */
    CHECK(sniff(std::string("abc\0\0\0,x\nde\0\0\0\0,y\n", 20)) == TextSniff::Plain);
    /* UTF-32 without a mark: the two high bytes of every four are NUL */
    CHECK(sniff(std::string("i\0\0\0d\0\0\0\n\0\0\0", 12)) == TextSniff::Utf32Le);
    CHECK(sniff(std::string("\0\0\0i\0\0\0d\0\0\0\n", 12)) == TextSniff::Utf32Be);
    CHECK(sniff(std::string("\x2D\x4E\0\0\x87\x65\0\0\n\0\0\0", 12)) == TextSniff::Utf32Le);
    /* UTF-16 text in Chinese without a mark: fewer NUL bytes than the density
     * rule needs (京 = 4EAC, 30 a line), but UTF-16 line ends */
    std::string le16, be16;
    for (int line = 0; line < 3; line++) {
        for (int k = 0; k < 30; k++) {
            le16 += "\xAC\x4E";
            be16 += "\x4E\xAC";
        }
        le16 += std::string("\x0D\x00\x0A\x00", 4);
        be16 += std::string("\x00\x0D\x00\x0A", 4);
    }
    bool by_lines = false;
    CHECK(sniff(le16, nullptr, &by_lines) == TextSniff::Utf16Le);
    CHECK(by_lines);
    by_lines = false;
    CHECK(sniff(be16, nullptr, &by_lines) == TextSniff::Utf16Be);
    CHECK(by_lines);
    /* the same line feeds in text that is valid UTF-8 are not taken for UTF-16 */
    CHECK(sniff(std::string("a\n\0b\n\0c", 7)) == TextSniff::Plain);
    /* nor in random bytes (a binary file named .csv): read as UTF-16 they are
     * not text — lone surrogates, control characters (release audit F15) */
    std::string noise;
    uint32_t x = 598;
    while (noise.size() < 4096) {
        x = x * 1103515245u + 12345u;
        const char c = static_cast<char>((x >> 16) & 0xFF);
        if (c != 0 && c != 0x0A) noise.push_back(c);
    }
    noise.replace(100, 2, std::string("\x0A\x00", 2));
    noise.replace(200, 2, std::string("\x0A\x00", 2));
    CHECK(sniff(noise) == TextSniff::Plain);
}

TEST_CASE("CSV-ENC-1: a single-byte code page decodes line by line; valid UTF-8 lines are kept") {
    const TextEncoding cp1251 = enc("windows-1251");
    TextDecodeStats st;
    /* a line longer than most chunk sizes, CRLF endings, no final LF */
    const std::string in = "id,city\r\n1,\xCC\xEE\xF1\xEA\xE2\xE0\r\n2,\xCC\xEE\xF1\xEA\xE2\xE0 "
                           "\xCC\xEE\xF1\xEA\xE2\xE0 \xCC\xEE\xF1\xEA\xE2\xE0";
    CHECK(decode_all_chunks(in, cp1251, false, 0, &st) ==
          "id,city\r\n1,Москва\r\n2,Москва Москва Москва");
    CHECK(st.lines == 3);
    CHECK(st.lines_decoded == 2);
    CHECK(st.lines_kept == 0);
    CHECK(st.undecodable == 0);

    /* one line in windows-1252, one already in UTF-8: both become José */
    const TextEncoding cp1252 = enc("windows-1252");
    const std::string mixed = "n\nJos\xE9\nJos\xC3\xA9\n";
    CHECK(decode_all_chunks(mixed, cp1252, false, 0, &st) == "n\nJosé\nJosé\n");
    CHECK(st.lines_decoded == 1);
    CHECK(st.lines_kept == 1);
    /* all: every non-ASCII line is windows-1252, also the one that looks like UTF-8 */
    CHECK(decode_all_chunks(mixed, cp1252, true, 0, &st) == "n\nJosé\nJosÃ©\n");
    CHECK(st.lines_decoded == 2);
    CHECK(st.lines_kept == 0);
    CHECK(st.lines_revalid == 1); /* the UTF-8 line, decoded as windows-1252 under all */
}

TEST_CASE("CSV-ENC-1: CR-only line ends and lines without an end stay bounded and exact") {
    TextDecodeStats st;
    const TextEncoding cp1251 = enc("windows-1251");
    /* classic Mac line ends: every CR ends a line */
    CHECK(decode_all_chunks("id\r1,\xCC\xEE\r2,x\r", cp1251, false, 0, &st) == "id\r1,Мо\r2,x\r");
    CHECK(st.lines_decoded == 1);
    /* a line longer than the cap: cut anywhere in a single-byte code page */
    CHECK(decode_all_chunks("\xCC\xEE\xF1\xEA\xE2\xE0\xCC\xEE", cp1251, false, 0, &st, 3) ==
          "МоскваМо");
    /* UTF-8: at a character boundary */
    CHECK(decode_all_chunks("Jos\xC3\xA9 Jos\xC3\xA9 Jos\xE9", TextEncoding::Utf8, false, 0, &st, 4) ==
          "José José Jos\xEF\xBF\xBD");
    CHECK(st.undecodable == 1);
    /* a multibyte code page: after a byte below 0x30 (never inside a character) */
    CHECK(decode_all_chunks("\xB1\xB1\xBE\xA9,\xC9\xCF\xBA\xA3,\xC5\xAE", enc("gbk"), false, 0, &st, 5) ==
          "北京,上海,女");
    /* ... and refused when there is none */
    const std::string src = write_file("parqit_tf_nocut", "\xB1\xB1\xBE\xA9\xC9\xCF\xBA\xA3\xC5\xAE\xB1\xB1");
    TextDecodeStats one;
    std::string err;
    CHECK_FALSE(parqit::text_file_decode(src, parqit_test::tmp_path("parqit_tf_nocut_out"), enc("gbk"),
                                         false, 0, &one, &err, 4, 6));
    CHECK(err.find("cut safely") != std::string::npos);
    /* the UTF-8 scan is bounded too, and still finds text that is not UTF-8 */
    bool valid = true, non_ascii = false;
    const std::string cr = write_file("parqit_tf_cr", "a\rb\r\xC3\xA9\rJos\xE9\r");
    REQUIRE(parqit::text_file_utf8_scan(cr, 0, &valid, &non_ascii, &err, 2, 4));
    CHECK_FALSE(valid);
    CHECK(non_ascii);
}

TEST_CASE("CSV-ENC-1: multibyte code pages decode every non-ASCII line, trail bytes included") {
    TextDecodeStats st;
    /* Shift_JIS: ソ = 83 5C, whose trail byte is ASCII backslash */
    const std::string sjis = "id|city\n1|\x93\x8C\x8B\x9E\n2|\x83\x5C\x83\x74\x83\x67\n";
    CHECK(decode_all_chunks(sjis, enc("shift_jis"), false, 0, &st) ==
          "id|city\n1|東京\n2|ソフト\n");
    CHECK(st.lines_decoded == 2);
    CHECK(st.undecodable == 0);
    /* GBK: 女 (C5 AE) is valid UTF-8 (Ů) by accident, yet decoded with the file */
    const std::string gbk = "id,w\n1,\xB1\xB1\xBE\xA9\n2,\xC5\xAE\n";
    CHECK(decode_all_chunks(gbk, enc("gbk"), false, 0, &st) == "id,w\n1,北京\n2,女\n");
    CHECK(st.lines_decoded == 2);
    CHECK(st.lines_kept == 0);
    CHECK(st.lines_revalid == 1); /* the 女 line, valid UTF-8 (Ů) by accident */
    /* a lead byte cut by the end of the file becomes U+FFFD, counted */
    CHECK(decode_all_chunks("a,\xB1", enc("gbk"), false, 0, &st) == "a,\xEF\xBF\xBD");
    CHECK(st.undecodable == 1);
}

TEST_CASE("CSV-ENC-1: UTF-16 decodes whole, a surrogate pair across any chunk boundary") {
    TextDecodeStats st;
    /* "x,😀\n" in UTF-16LE after its byte-order mark (skipped) */
    const std::string le("\xFF\xFEx\0,\0\x3D\xD8\x00\xDE\n\0y\0", 14);
    CHECK(decode_all_chunks(le, enc("utf-16le"), false, 2, &st) == "x,😀\ny");
    CHECK(st.undecodable == 0);
    CHECK(st.lines == 2);
    CHECK(st.lines_decoded == 2);
    const std::string be("\0A\0b\xD8\x3D\xDE\x00", 8);
    CHECK(decode_all_chunks(be, enc("utf-16be"), false, 0, &st) == "Ab😀");
    /* an odd final byte and a lone high surrogate: U+FFFD each */
    CHECK(decode_all_chunks(std::string("a\0\x3D\xD8" "b", 5), enc("utf-16le"), false, 0, &st) ==
          "a\xEF\xBF\xBD\xEF\xBF\xBD");
    CHECK(st.undecodable == 2);
}

TEST_CASE("CSV-ENC-1: UTF-8 declared: each invalid sequence becomes U+FFFD, the rest is kept") {
    TextDecodeStats st;
    CHECK(decode_all_chunks("a,Jos\xC3\xA9\nb,Jos\xE9\n", TextEncoding::Utf8, false, 0, &st) ==
          "a,José\nb,Jos\xEF\xBF\xBD\n");
    CHECK(st.lines_decoded == 1);
    CHECK(st.undecodable == 1);
}

TEST_CASE("CSV-ENC-1: the plan — what is decoded, from what, and what is refused") {
    std::string err;
    /* undeclared UTF-8 scanned in place: left to the engine, no scan */
    TextPlan p = plan("a\n\xE9\n", "");
    CHECK_FALSE(p.decode);
    /* a lookup file (using mode) that is not UTF-8: the session default */
    p = plan("a\n\xE9\n", "", false, true);
    CHECK(p.decode);
    CHECK(p.defaulted);
    CHECK(p.enc == TextEncoding::Windows1252);
    p = plan("a\nJos\xC3\xA9\n", "", false, true);
    CHECK_FALSE(p.decode);
    /* a UTF-8 byte-order mark: UTF-8, over a code page given */
    p = plan("\xEF\xBB\xBFJos\xC3\xA9\n", "windows-1251");
    CHECK_FALSE(p.decode);
    CHECK(p.bom_overrode);
    CHECK(p.skip == 3);
    p = plan("\xEF\xBB\xBFJos\xE9\n", "windows-1251");
    CHECK(p.decode);
    CHECK(p.enc == TextEncoding::Utf8);
    /* a UTF-16 byte-order mark: UTF-16, over a code page given */
    p = plan(std::string("\xFF\xFEi\0d\0", 6), "windows-1251");
    CHECK(p.decode);
    CHECK(p.enc == enc("utf-16le"));
    CHECK(p.bom_overrode);
    CHECK(p.found == "utf-16le bom");
    /* UTF-16 without a mark: decoded when nothing is declared; an encoding()
     * given is followed, and the plan says the bytes looked otherwise */
    const std::string nul16("i\0d\0\n\0" "1\0\n\0", 10);
    p = plan(nul16, "");
    CHECK(p.decode);
    CHECK(p.found == "utf-16le nul");
    p = plan(nul16, "latin1");
    CHECK_FALSE(p.decode); /* byte-exact UTF-8, including its NUL padding */
    CHECK(p.enc == TextEncoding::Latin1);
    CHECK(p.pattern_overruled == "utf-16le");
    p = plan(nul16, "utf-16be");
    CHECK(p.enc == enc("utf-16be"));
    CHECK(p.pattern_overruled == "utf-16le"); /* the other byte order, followed and said (F23) */
    p = plan(nul16, "utf-16");
    CHECK(p.enc == enc("utf-16le"));
    CHECK(p.pattern_overruled.empty());
    p = plan(std::string("\xFF\xFE\0\0i\0\0\0", 8), "", false, false, false, &err);
    CHECK_FALSE(p.usage_error);
    CHECK(err.find("UTF-32") != std::string::npos);
    /* UTF-32 without a mark: refused when nothing is declared, followed when encoding() is */
    const std::string u32("i\0\0\0d\0\0\0\n\0\0\0", 12);
    p = plan(u32, "", false, false, false, &err);
    CHECK(err.find("UTF-32LE") != std::string::npos);
    p = plan(u32, "utf-8");
    CHECK(p.pattern_overruled == "utf-32le");
    CHECK_FALSE(p.decode); /* NUL bytes are valid UTF-8: the reader reads it as it is */
    /* NUL padding in UTF-8 text: read as it is, declared or not (as before CSV-ENC-1) */
    const std::string padded("abc\0\0\0,x\nde\0\0\0\0,Z\xC3\xBCrich\n", 26);
    p = plan(padded, "");
    CHECK_FALSE(p.decode);
    CHECK(p.has_nul);
    p = plan(padded, "utf-8");
    CHECK_FALSE(p.decode);
    p = plan(padded, "", false, true);
    CHECK_FALSE(p.decode);
    p = plan("a\n", "klingon", false, false, false, &err);
    CHECK(p.usage_error);
    /* a multibyte code page: valid UTF-8 throughout is read as UTF-8 unless all */
    p = plan("w\n\xC5\xAE\n", "gbk");
    CHECK_FALSE(p.decode);
    CHECK(p.kept_valid);
    p = plan("w\n\xC5\xAE\n", "gbk", true);
    CHECK(p.decode);
    p = plan("w\n\xB1\xB1\xBE\xA9\n\xC5\xAE\n", "gbk");
    CHECK(p.decode);
    CHECK(p.enc == enc("cp936"));
    p = plan("w\nx\n", "gbk");
    CHECK_FALSE(p.decode);
    CHECK_FALSE(p.kept_valid); /* ASCII: nothing to decode, nothing to say */
    /* a wholly UTF-8 file also stays in place with a single-byte code page */
    p = plan("w\nJos\xC3\xA9\n", "windows-1252");
    CHECK_FALSE(p.decode);
}

TEST_CASE("CSV-ENC-1: DuckDB's advice for text that is not UTF-8 becomes parqit's encoding()") {
    const std::string duck =
        "Invalid Input Error: CSV Error on Line: 2\nOriginal Line: id,city\n"
        "Invalid unicode (byte sequence mismatch) detected. This file is not utf-8 encoded.\n\n"
        "Possible Solution: Set the correct encoding, if available, to read this CSV File "
        "(e.g., encoding='UTF-16')\n"
        "Possible Solution: Enable ignore errors (ignore_errors=true) to skip this row\n\n"
        "  file = x.csv\n";
    const std::string out = parqit::with_encoding_hint(duck);
    CHECK(out.find("encoding='UTF-16'") == std::string::npos);
    CHECK(out.find("ignore_errors") == std::string::npos);
    CHECK(out.find("parqit's encoding() option") != std::string::npos);
    CHECK(out.find("file = x.csv") != std::string::npos);
    CHECK(parqit::with_encoding_hint("Binder Error: x") == "Binder Error: x");
}
