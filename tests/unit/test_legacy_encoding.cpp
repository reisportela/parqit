#include "doctest.h"

#include "engine/legacy_encoding.hpp"

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
    CHECK_FALSE(legacy_encoding_parse("utf-16", &e));
    CHECK_FALSE(legacy_encoding_parse("windows-1250", &e));
    CHECK_FALSE(legacy_encoding_parse("foo", &e));

    CHECK(std::string(legacy_encoding_name(LegacyEncoding::Windows1252)) == "windows-1252");
    CHECK(std::string(legacy_encoding_name(LegacyEncoding::Latin1)) == "latin1");
    CHECK(std::string(legacy_encoding_name(LegacyEncoding::Latin9)) == "latin9");
    CHECK(std::string(legacy_encoding_name(LegacyEncoding::MacRoman)) == "macroman");
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
