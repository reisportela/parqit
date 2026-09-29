#include "doctest.h"

#include "engine/legacy_encoding.hpp"
#include "engine/encoding_tables.hpp"

#include <cstdint>
#include <cstring>
#include <string>

using parqit::LegacyEncoding;
using parqit::legacy_encoding_name;
using parqit::legacy_encoding_parse;
using parqit::legacy_to_utf8;
using parqit::utf8_or_transcode;
using parqit::utf8_valid;

/* ENC-2: the save path transcodes legacy 8-bit text instead of refusing it.
 * These pin the name parser, the strict validator, the four code pages and
 * the "valid UTF-8 is left alone" policy (tests/verify_suite/v32 covers the
 * Stata-facing behaviour end to end with a pyarrow oracle). */

TEST_CASE("ENC-2: encoding names parse case-insensitively, empty means the default") {
    LegacyEncoding e;
    REQUIRE(legacy_encoding_parse("", &e));
    CHECK(e == LegacyEncoding::Windows1252);
    REQUIRE(legacy_encoding_parse("  Windows-1252 ", &e));
    CHECK(e == LegacyEncoding::Windows1252);
    REQUIRE(legacy_encoding_parse("CP1252", &e));
    CHECK(e == LegacyEncoding::Windows1252);
    REQUIRE(legacy_encoding_parse("latin1", &e));
    CHECK(e == LegacyEncoding::Latin1);
    REQUIRE(legacy_encoding_parse("ISO-8859-1", &e));
    CHECK(e == LegacyEncoding::Latin1);
    REQUIRE(legacy_encoding_parse("latin9", &e));
    CHECK(e == LegacyEncoding::Latin9);
    REQUIRE(legacy_encoding_parse("iso-8859-15", &e));
    CHECK(e == LegacyEncoding::Latin9);
    REQUIRE(legacy_encoding_parse("MacRoman", &e));
    CHECK(e == LegacyEncoding::MacRoman);
    REQUIRE(legacy_encoding_parse("mac-roman", &e));
    CHECK(e == LegacyEncoding::MacRoman);
    CHECK_FALSE(legacy_encoding_parse("foo", &e));
    CHECK_FALSE(legacy_encoding_parse("windows-9999", &e));
    CHECK_FALSE(legacy_encoding_parse("euc-tw", &e));

    CHECK(std::string(legacy_encoding_name(LegacyEncoding::Windows1252)) == "windows-1252");
    CHECK(std::string(legacy_encoding_name(LegacyEncoding::Latin1)) == "latin1");
    CHECK(std::string(legacy_encoding_name(LegacyEncoding::Latin9)) == "latin9");
    CHECK(std::string(legacy_encoding_name(LegacyEncoding::MacRoman)) == "macroman");
}

TEST_CASE("ENC-AUDIT: CP864 includes the Arabic percent sign below byte 128") {
    LegacyEncoding enc;
    REQUIRE(legacy_encoding_parse("ibm864", &enc));
    CHECK(legacy_to_utf8("%", enc) == "\xd9\xaa");
    CHECK(legacy_to_utf8("%\x80", enc) == "\xd9\xaa\xc2\xb0");
    CHECK(legacy_to_utf8("%", LegacyEncoding::Windows1252) == "%");
    for (const char *name : parqit::enctab::kSingleName) {
        REQUIRE(legacy_encoding_parse(name, &enc));
        for (int b = 0; b < 128; b++) {
            const std::string raw(1, static_cast<char>(b));
            const std::string want = std::string(name) == "ibm864" && b == 0x25 ? "\xd9\xaa" : raw;
            CHECK(legacy_to_utf8(raw, enc) == want);
        }
    }
}

TEST_CASE("ENC-2: strict UTF-8 validation (same boundary as the engine walker)") {
    CHECK(utf8_valid(std::string("")));
    CHECK(utf8_valid(std::string("ascii only")));
    CHECK(utf8_valid(std::string("caf\xc3\xa9")));                 /* café */
    CHECK(utf8_valid(std::string("a\xf0\x9f\x98\x80" "b")));      /* 😀 */
    CHECK(utf8_valid(std::string("\xef\xbf\xbd")));               /* U+FFFD */
    CHECK_FALSE(utf8_valid(std::string("\xe9")));                 /* lone Latin-1 é */
    CHECK_FALSE(utf8_valid(std::string("Regi\xe3o")));            /* Latin-1 ã */
    CHECK_FALSE(utf8_valid(std::string("ok\xc3x")));              /* lead, no continuation */
    CHECK_FALSE(utf8_valid(std::string("\xc3")));                 /* truncated at end */
    CHECK_FALSE(utf8_valid(std::string("\xc0\x80")));             /* overlong NUL */
    CHECK_FALSE(utf8_valid(std::string("\xe0\x80\x80")));         /* overlong 3-byte */
    CHECK_FALSE(utf8_valid(std::string("\xed\xa0\x80")));         /* UTF-16 surrogate */
    CHECK_FALSE(utf8_valid(std::string("\xf4\x90\x80\x80")));     /* > U+10FFFF */
    CHECK_FALSE(utf8_valid(std::string("\xff\xfe")));             /* never lead bytes */
    CHECK_FALSE(utf8_valid(std::string("\x80")));                 /* stray continuation */
}

TEST_CASE("ENC-2: code pages transcode to the documented code points") {
    /* the accented Latin letters are identical in windows-1252 and latin1 */
    CHECK(legacy_to_utf8("Regi\xe3o", LegacyEncoding::Windows1252) == "Regi\xc3\xa3o");
    CHECK(legacy_to_utf8("Regi\xe3o", LegacyEncoding::Latin1) == "Regi\xc3\xa3o");
    CHECK(legacy_to_utf8("Econ\xf3mica", LegacyEncoding::Windows1252) == "Econ\xc3\xb3mica");
    CHECK(legacy_to_utf8("n\xedvel", LegacyEncoding::Windows1252) == "n\xc3\xadvel");
    CHECK(legacy_to_utf8("\xe9", LegacyEncoding::Windows1252) == "\xc3\xa9");
    CHECK(legacy_to_utf8("\xff\xfe", LegacyEncoding::Windows1252) == "\xc3\xbf\xc3\xbe"); /* ÿþ */
    /* 0x80-0x9F: printable in windows-1252, C1 controls in latin1/latin9 */
    CHECK(legacy_to_utf8("\x80", LegacyEncoding::Windows1252) == "\xe2\x82\xac");   /* € */
    CHECK(legacy_to_utf8("\x80", LegacyEncoding::Latin1) == "\xc2\x80");            /* U+0080 */
    CHECK(legacy_to_utf8("\x80", LegacyEncoding::Latin9) == "\xc2\x80");
    CHECK(legacy_to_utf8("\x93q\x94", LegacyEncoding::Windows1252) ==
          "\xe2\x80\x9cq\xe2\x80\x9d");                                               /* “q” */
    CHECK(legacy_to_utf8("\x81", LegacyEncoding::Windows1252) == "\xc2\x81");       /* undefined → C1 */
    /* latin9 puts € at 0xA4 and Š/š/Ž/ž/Œ/œ/Ÿ in the A6..BE holes */
    CHECK(legacy_to_utf8("\xa4", LegacyEncoding::Latin9) == "\xe2\x82\xac");
    CHECK(legacy_to_utf8("\xa4", LegacyEncoding::Latin1) == "\xc2\xa4");            /* ¤ */
    CHECK(legacy_to_utf8("\xbc\xbd", LegacyEncoding::Latin9) == "\xc5\x92\xc5\x93"); /* Œœ */
    /* MacRoman: é is 0x8E, ã is 0x8B, € is 0xDB */
    CHECK(legacy_to_utf8("caf\x8e", LegacyEncoding::MacRoman) == "caf\xc3\xa9");
    CHECK(legacy_to_utf8("S\x8bo", LegacyEncoding::MacRoman) == "S\xc3\xa3o");
    CHECK(legacy_to_utf8("\xdb", LegacyEncoding::MacRoman) == "\xe2\x82\xac");
    /* ASCII passes through untouched under every code page */
    for (LegacyEncoding e : {LegacyEncoding::Windows1252, LegacyEncoding::Latin1,
                             LegacyEncoding::Latin9, LegacyEncoding::MacRoman}) {
        CHECK(legacy_to_utf8("", e) == "");
        CHECK(legacy_to_utf8("plain ASCII 123 ~", e) == "plain ASCII 123 ~");
    }
}

TEST_CASE("ENC-2: every code page is total — all 256 bytes yield valid, non-empty UTF-8") {
    for (LegacyEncoding e : {LegacyEncoding::Windows1252, LegacyEncoding::Latin1,
                             LegacyEncoding::Latin9, LegacyEncoding::MacRoman}) {
        std::string all;
        for (int b = 0; b < 256; b++) all.push_back(static_cast<char>(b));
        const std::string out = legacy_to_utf8(all, e);
        CHECK(utf8_valid(out));
        CHECK(out.size() >= all.size());
        for (int b = 0x80; b < 256; b++) {
            const std::string one = legacy_to_utf8(std::string(1, static_cast<char>(b)), e);
            CHECK(utf8_valid(one));
            CHECK(one.size() >= 2); /* every high byte is a non-ASCII code point */
        }
    }
}

TEST_CASE("ENC-2: utf8_or_transcode leaves valid UTF-8 byte-exact, fixes the rest") {
    std::string ok = "S\xc3\xa3o Jo\xc3\xa3o"; /* already UTF-8 */
    CHECK_FALSE(utf8_or_transcode(&ok, LegacyEncoding::Windows1252));
    CHECK(ok == "S\xc3\xa3o Jo\xc3\xa3o");
    std::string emoji = "\xf0\x9f\xa6\x86";
    CHECK_FALSE(utf8_or_transcode(&emoji, LegacyEncoding::MacRoman));
    CHECK(emoji == "\xf0\x9f\xa6\x86");
    std::string legacy = "S\xe3o Jo\xe3o";
    CHECK(utf8_or_transcode(&legacy, LegacyEncoding::Windows1252));
    CHECK(legacy == "S\xc3\xa3o Jo\xc3\xa3o");
    CHECK(utf8_valid(legacy));
    /* a mixed cell (UTF-8 text plus one stray legacy byte) is transcoded whole:
     * the already-multibyte sequences then read as mojibake, which is exactly
     * what unicode translate produces for the same input — documented. */
    std::string mixed = "\xc3\xa9\xe9";
    CHECK(utf8_or_transcode(&mixed, LegacyEncoding::Latin1));
    CHECK(utf8_valid(mixed));
    CHECK(mixed == "\xc3\x83\xc2\xa9\xc3\xa9");
}

/* ENC-3: every language's legacy code pages, not only the Western four. The
 * byte sequences were produced by Python's codecs; Stata's ICU (ustrfrom) is
 * the independent oracle of the verify suite (v136). */
namespace {
struct Sample {
    const char *encoding, *bytes, *utf8;
};
const Sample kSamples[] = {
    {"windows-1251", "\xcf\xf0\xe8\xe2\xe5\xf2\x2c\x20\xec\xe8\xf0", "\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82\x2c\x20\xd0\xbc\xd0\xb8\xd1\x80"}, /* Привет, мир */
    {"koi8-r", "\xf0\xd2\xc9\xd7\xc5\xd4", "\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82"}, /* Привет */
    {"ibm866", "\x8f\xe0\xa8\xa2\xa5\xe2", "\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82"}, /* Привет */
    {"windows-1253", "\xc1\xe8\xde\xed\xe1", "\xce\x91\xce\xb8\xce\xae\xce\xbd\xce\xb1"}, /* Αθήνα */
    {"iso-8859-7", "\xc1\xe8\xde\xed\xe1", "\xce\x91\xce\xb8\xce\xae\xce\xbd\xce\xb1"}, /* Αθήνα */
    {"windows-1250", "\xa3\xf3\x64\x9f\x20\x4b\x72\x61\x6b\xf3\x77", "\xc5\x81\xc3\xb3\x64\xc5\xba\x20\x4b\x72\x61\x6b\xc3\xb3\x77"}, /* Łódź Kraków */
    {"iso-8859-2", "\xa3\xf3\x64\xbc", "\xc5\x81\xc3\xb3\x64\xc5\xba"}, /* Łódź */
    {"windows-1254", "\xfe\x65\x68\x69\x72\x20\xdd\x73\x74\x61\x6e\x62\x75\x6c", "\xc5\x9f\x65\x68\x69\x72\x20\xc4\xb0\x73\x74\x61\x6e\x62\x75\x6c"}, /* şehir İstanbul */
    {"windows-1255", "\xf9\xec\xe5\xed", "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d"}, /* שלום */
    {"windows-1256", "\xe3\xd1\xcd\xc8\xc7", "\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7"}, /* مرحبا */
    {"windows-874", "\xca\xc7\xd1\xca\xb4\xd5", "\xe0\xb8\xaa\xe0\xb8\xa7\xe0\xb8\xb1\xe0\xb8\xaa\xe0\xb8\x94\xe0\xb8\xb5"}, /* สวัสดี */
    {"windows-1257", "\xd0\x69\x61\x75\x6c\x69\x61\x69\x20\x52\xee\x67\x61", "\xc5\xa0\x69\x61\x75\x6c\x69\x61\x69\x20\x52\xc4\xab\x67\x61"}, /* Šiauliai Rīga */
    {"windows-936", "\xd6\xd0\xce\xc4\xc5\xae", "\xe4\xb8\xad\xe6\x96\x87\xe5\xa5\xb3"}, /* 中文女 */
    {"gb18030", "\xd6\xd0\xce\xc4\x90\x30\x81\x30", "\xe4\xb8\xad\xe6\x96\x87\xf0\x90\x80\x80"}, /* 中文𐀀 */
    {"windows-932", "\x93\xfa\x96\x7b\x8c\xea\xb1\xb2\xb3", "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xef\xbd\xb1\xef\xbd\xb2\xef\xbd\xb3"}, /* 日本語ｱｲｳ */
    {"euc-jp", "\xc6\xfc\xcb\xdc\xb8\xec\x8e\xb1\x8f\xb0\xa1", "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xef\xbd\xb1\xe4\xb8\x82"}, /* 日本語ｱ丂 */
    {"windows-949", "\xc7\xd1\xb1\xb9\xbe\xee\x8c\x63\xb9\xe6\xb0\xa2\xc7\xcf", "\xed\x95\x9c\xea\xb5\xad\xec\x96\xb4\xeb\x98\xa0\xeb\xb0\xa9\xea\xb0\x81\xed\x95\x98"}, /* 한국어똠방각하 */
    {"windows-950", "\xa4\xa4\xa4\xe5\xbb\x4f\xc6\x57", "\xe4\xb8\xad\xe6\x96\x87\xe8\x87\xba\xe7\x81\xa3"}, /* 中文臺灣 */
};
} // namespace

TEST_CASE("ENC-3: names of every family and their aliases") {
    LegacyEncoding e;
    struct {
        const char *given, *canonical;
    } names[] = {
        {"cp1251", "windows-1251"},   {"Windows_1251", "windows-1251"}, {"KOI8-R", "koi8-r"},
        {"latin2", "iso-8859-2"},     {"ISO-8859-5", "iso-8859-5"},     {"ibm866", "ibm866"},
        {"CP866", "ibm866"},          {"Shift_JIS", "windows-932"},     {"sjis", "windows-932"},
        {"GBK", "windows-936"},       {"gb2312", "windows-936"},        {"EUC-KR", "windows-949"},
        {"Big5", "windows-950"},      {"GB18030", "gb18030"},           {"EUC-JP", "euc-jp"},
        {"UTF8", "utf-8"},            {"tis-620", "iso-8859-11"},       {"x-mac-cyrillic", "mac-cyrillic"},
        {"latin9", "latin9"},         {"iso-8859-15", "latin9"},        {"macintosh", "macroman"},
        {"utf-16", "utf-16"},         {"UTF-16LE", "utf-16le"},         {"windows-874", "windows-874"},
    };
    for (const auto &n : names) {
        REQUIRE_MESSAGE(legacy_encoding_parse(n.given, &e), n.given);
        CHECK_MESSAGE(std::string(legacy_encoding_name(e)) == n.canonical, n.given);
    }
    REQUIRE(legacy_encoding_parse("windows-936", &e));
    CHECK(e.multibyte());
    REQUIRE(legacy_encoding_parse("windows-1251", &e));
    CHECK_FALSE(e.multibyte());
}

TEST_CASE("ENC-3: text in Cyrillic, Greek, Central European, Turkish, Hebrew, Arabic, Thai, Baltic, "
          "Chinese, Japanese and Korean code pages decodes exactly") {
    for (const auto &s : kSamples) {
        LegacyEncoding e;
        REQUIRE_MESSAGE(legacy_encoding_parse(s.encoding, &e), s.encoding);
        std::string out;
        const size_t bad = parqit::legacy_decode(s.bytes, std::strlen(s.bytes), e, &out);
        CHECK_MESSAGE(out == s.utf8, s.encoding);
        CHECK_MESSAGE(bad == 0, s.encoding);
    }
}

TEST_CASE("ENC-3: a GBK text that happens to be valid UTF-8 still decodes as GBK when asked") {
    /* 女 in GBK is C5 AE, which is also UTF-8 for U+016E (Ů): the per-item
     * "valid UTF-8 is left alone" rule cannot see the difference, which is
     * why declared and column-level decoding exist (ENC-3) */
    const std::string female = "\xc5\xae";
    CHECK(utf8_valid(female));
    LegacyEncoding gbk;
    REQUIRE(legacy_encoding_parse("gbk", &gbk));
    CHECK(legacy_to_utf8(female, gbk) == "\xe5\xa5\xb3");
}

TEST_CASE("ENC-3: bytes an encoding does not define become U+FFFD and are counted") {
    LegacyEncoding e;
    std::string out;
    const std::string rep = "\xef\xbf\xbd";
    REQUIRE(legacy_encoding_parse("gbk", &e));
    CHECK(parqit::legacy_decode("a\xd6", 2, e, &out) == 1); /* a lead byte at the end */
    CHECK(out == "a" + rep);
    CHECK(parqit::legacy_decode("\xd6 x", 3, e, &out) == 1); /* an ASCII trail is read again */
    CHECK(out == rep + " x");
    REQUIRE(legacy_encoding_parse("windows-932", &e));
    CHECK(parqit::legacy_decode("\x81\x20", 2, e, &out) == 1);
    CHECK(out == rep + " ");
    REQUIRE(legacy_encoding_parse("windows-1253", &e));
    CHECK(parqit::legacy_decode("\xaa", 1, e, &out) == 1); /* undefined in 1253, not C1 */
    CHECK(out == rep);
    CHECK(parqit::legacy_decode("\x81", 1, e, &out) == 0); /* undefined C1 position: U+0081 */
    CHECK(out == "\xc2\x81");
    REQUIRE(legacy_encoding_parse("utf-8", &e));
    CHECK(parqit::legacy_decode("a\xe4\xb8x\xff", 5, e, &out) == 2); /* a cut sequence, a bad byte */
    CHECK(out == "a" + rep + "x" + rep);
    REQUIRE(legacy_encoding_parse("gb18030", &e));
    CHECK(parqit::legacy_decode("\x81\x30\x81", 3, e, &out) >= 1);
    CHECK(utf8_valid(out));
}

TEST_CASE("ENC-3: UTF-16 by its byte-order mark, surrogate pairs, and broken units") {
    LegacyEncoding e;
    std::string out;
    REQUIRE(legacy_encoding_parse("utf-16", &e));
    const char le[] = "\xff\xfe\x41\x00\x2d\x4e\x3d\xd8\x00\xde"; /* BOM A 中 😀 */
    CHECK(parqit::legacy_decode(le, 10, e, &out) == 0);
    CHECK(out == "A\xe4\xb8\xad\xf0\x9f\x98\x80");
    const char be[] = "\xfe\xff\x00\x41\x4e\x2d";
    CHECK(parqit::legacy_decode(be, 6, e, &out) == 0);
    CHECK(out == "A\xe4\xb8\xad");
    REQUIRE(legacy_encoding_parse("utf-16le", &e));
    CHECK(parqit::legacy_decode("\x3d\xd8\x41\x00\x42", 5, e, &out) == 2); /* lone surrogate, odd byte */
    CHECK(out == "\xef\xbf\xbd" "A" "\xef\xbf\xbd");
}

TEST_CASE("ENC-3: every single-byte code page is total over the 128 upper bytes") {
    for (const char *name : {"windows-874", "windows-1250", "windows-1251", "windows-1253", "windows-1254",
                             "windows-1255", "windows-1256", "windows-1257", "windows-1258", "iso-8859-2",
                             "iso-8859-5", "iso-8859-7", "iso-8859-8", "iso-8859-11", "koi8-r", "koi8-u",
                             "ibm437", "ibm850", "ibm866", "mac-cyrillic", "mac-greek"}) {
        LegacyEncoding e;
        REQUIRE_MESSAGE(legacy_encoding_parse(name, &e), name);
        for (int b = 0x80; b < 256; b++) {
            std::string out;
            parqit::legacy_decode(std::string(1, static_cast<char>(b)).data(), 1, e, &out);
            CHECK_MESSAGE(utf8_valid(out), name);
            CHECK_MESSAGE(out.size() >= 2, name);
        }
    }
}


namespace {
/* the one code point `bytes` decode to in `name`, or 0 on anything else */
uint32_t one_cp(const char *name, const std::string &bytes) {
    LegacyEncoding e;
    REQUIRE_MESSAGE(legacy_encoding_parse(name, &e), name);
    std::string out;
    if (parqit::legacy_decode(bytes.data(), bytes.size(), e, &out) != 0) return 0;
    const auto u = reinterpret_cast<const unsigned char *>(out.data());
    if (out.size() == 2) return (uint32_t(u[0] & 0x1F) << 6) | (u[1] & 0x3F);
    if (out.size() == 3) return (uint32_t(u[0] & 0x0F) << 12) | (uint32_t(u[1] & 0x3F) << 6) | (u[2] & 0x3F);
    if (out.size() == 4)
        return (uint32_t(u[0] & 0x07) << 18) | (uint32_t(u[1] & 0x3F) << 12) |
               (uint32_t(u[2] & 0x3F) << 6) | (u[3] & 0x3F);
    return 0;
}
} // namespace

/* Where the tables follow Windows (and ICU, and glibc) rather than Python's
 * codecs. The expected code points come from Stata's ICU (ustrfrom) and glibc
 * iconv, not from the generator's source; v136 compares every byte sequence. */
TEST_CASE("ENC-3: Windows' reading of its double-byte code pages, GB18030-2005, EUC-JP as WHATWG") {
    /* 936: the three EUDC areas, GBK's reserved cells (Private Use in 936, real
     * characters in GB18030) */
    CHECK(one_cp("gbk", "\xAA\xA1") == 0xE000);
    CHECK(one_cp("gbk", "\xF8\xA1") == 0xE234);
    CHECK(one_cp("gbk", "\xA1\x40") == 0xE4C6);
    CHECK(one_cp("gbk", "\xA2\xE3") == 0xE76C);
    CHECK(one_cp("gbk", "\xFE\x50") == 0xE815);
    CHECK(one_cp("gbk", "\xA8\xBC") == 0xE7C7);
    CHECK(one_cp("gbk", "\x80") == 0x20AC);
    /* GB18030 (2005): the same cells as real characters; A8BC and 8135F437 swapped */
    CHECK(one_cp("gb18030", "\xA2\xE3") == 0x20AC);
    CHECK(one_cp("gb18030", "\xFE\x50") == 0x2E81);
    CHECK(one_cp("gb18030", "\xA8\xBC") == 0x1E3F);
    CHECK(one_cp("gb18030", "\x81\x35\xF4\x37") == 0xE7C7);
    CHECK(one_cp("gb18030", "\xAA\xA1") == 0xE000);
    /* 949 and 950: EUDC in the Private Use Area; 950's C6A1-C8FE too (not ETEN) */
    CHECK(one_cp("cp949", "\xC9\xA1") == 0xE000);
    CHECK(one_cp("cp949", "\xFE\xA1") == 0xE05E);
    CHECK(one_cp("cp949", "\xFE\xFE") == 0xE0BB);
    CHECK(one_cp("big5", "\xFA\x40") == 0xE000);
    CHECK(one_cp("big5", "\x8E\x40") == 0xE311);
    CHECK(one_cp("big5", "\x81\x40") == 0xEEB8);
    CHECK(one_cp("big5", "\xC6\xA1") == 0xF6B1);
    CHECK(one_cp("big5", "\xC8\xFE") == 0xF848);
    CHECK(one_cp("big5", "\xA4\x40") == 0x4E00); /* 一: ordinary Big5 is untouched */
    /* 932: EUDC was already Windows' */
    CHECK(one_cp("shift_jis", "\xF0\x40") == 0xE000);
    CHECK(one_cp("shift_jis", "\x81\x60") == 0xFF5E);
    /* EUC-JP through the 932 table by pointer: Microsoft's mappings, NEC row 13,
     * the NEC-selected IBM extensions; JIS X 0212 0x2237 as ICU and glibc */
    CHECK(one_cp("euc-jp", "\xA1\xC1") == 0xFF5E);
    CHECK(one_cp("euc-jp", "\xA1\xDD") == 0xFF0D);
    CHECK(one_cp("euc-jp", "\xAD\xA1") == 0x2460);
    CHECK(one_cp("euc-jp", "\xF9\xA1") == 0x7E8A);
    CHECK(one_cp("euc-jp", "\xB0\xA1") == 0x4E9C); /* 亜 */
    CHECK(one_cp("euc-jp", "\x8E\xB1") == 0xFF71); /* half-width ｱ */
    CHECK(one_cp("euc-jp", "\x8F\xA2\xB7") == 0xFF5E);
    CHECK(one_cp("euc-jp", "\x8F\xB0\xA1") == 0x4E02); /* 丂, JIS X 0212 */
    /* no ICU converter in Stata for these two: the published tables */
    CHECK(one_cp("iso-8859-16", "\xA1") == 0x0104);
    CHECK(one_cp("iso-8859-16", "\xA4") == 0x20AC);
    CHECK(one_cp("iso-8859-16", "\xAA") == 0x0218);
    CHECK(one_cp("iso-8859-16", "\xDE") == 0x021A);
    CHECK(one_cp("iso-8859-16", "\xFE") == 0x021B);
    CHECK(one_cp("mac-iceland", "\xA0") == 0x00DD);
    CHECK(one_cp("mac-iceland", "\xDC") == 0x00D0);
    CHECK(one_cp("mac-iceland", "\xDD") == 0x00F0);
    CHECK(one_cp("mac-iceland", "\xDE") == 0x00DE);
    CHECK(one_cp("mac-iceland", "\xDF") == 0x00FE);
}
