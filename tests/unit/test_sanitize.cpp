#include "doctest.h"

#include "engine/sanitize.hpp"

using namespace parqit;

TEST_CASE("clean names pass through untouched") {
    CHECK(sanitize_stata_name("wage", 1) == "wage");
    CHECK(sanitize_stata_name("_merge_key", 1) == "_merge_key");
    CHECK(sanitize_stata_name("V123", 1) == "V123");
}

TEST_CASE("reserved words gain a prefix (charter finding 2)") {
    CHECK(sanitize_stata_name("if", 1) == "_if");
    CHECK(sanitize_stata_name("in", 1) == "_in");
    CHECK(sanitize_stata_name("byte", 1) == "_byte");
    CHECK(sanitize_stata_name("str17", 1) == "_str17");
    CHECK(sanitize_stata_name("strL", 1) == "_strL");
    CHECK(sanitize_stata_name("_N", 1) == "__N");
    /* SAN-SE-1: `_se` is a Stata system variable like `_b`/`_coef`; unsanitised
     * it loads, then `summarize _se` silently resolves to the system variable
     * (an empty no-op) instead of the data column */
    CHECK(sanitize_stata_name("_se", 1) == "__se");
    CHECK(sanitize_stata_name("_b", 1) == "__b");
    /* not reserved: case differs or non-numeric tail */
    CHECK(sanitize_stata_name("If", 1) == "If");
    CHECK(sanitize_stata_name("strx", 1) == "strx");
}

TEST_CASE("leading digits and punctuation (charter findings 2 and 14)") {
    CHECK(sanitize_stata_name("1x", 1) == "_1x");
    CHECK(sanitize_stata_name("x y", 1) == "x_y");
    CHECK(sanitize_stata_name("a-b.c", 1) == "a_b_c");
    CHECK(sanitize_stata_name("", 3) == "v3");
    CHECK(sanitize_stata_name("!!!", 2) == "___");
}

TEST_CASE("unicode names use Stata's 32-character grammar and ceiling") {
    CHECK(sanitize_stata_name("ano_decisão", 1) == "ano_decis\xc3\xa3o");
    /* 31 ASCII characters plus one multibyte letter is exactly 32 chars. */
    std::string long31(31, 'a');
    CHECK(sanitize_stata_name(long31 + "\xc3\xa3x", 1) ==
          long31 + "\xc3\xa3");
    std::string long40(40, 'b');
    CHECK(sanitize_stata_name(long40, 1) == std::string(32, 'b'));
    const std::string cjk20 = "資料資料資料資料資料資料資料資料資料資料";
    CHECK(sanitize_stata_name(cjk20, 1) == cjk20);
    CHECK(sanitize_stata_name("🦆id", 1) == "_id");
    CHECK(sanitize_stata_name("\xcc\x81name", 1) == "_name");
    CHECK(sanitize_stata_name("a\xcc\x81", 1) == "a\xcc\x81");
}

TEST_CASE("duplicates are disambiguated deterministically (charter finding 10)") {
    std::vector<bool> renamed;
    auto out = sanitize_unique({"dup", "dup", "dup"}, &renamed);
    CHECK(out[0] == "dup");
    CHECK(out[1] == "dup_2");
    CHECK(out[2] == "dup_3");
    CHECK_FALSE(renamed[0]);
    CHECK(renamed[1]);
    CHECK(renamed[2]);

    /* sanitisation-induced collisions too: "a b" and "a-b" both → a_b */
    out = sanitize_unique({"a b", "a-b"});
    CHECK(out[0] == "a_b");
    CHECK(out[1] == "a_b_2");

    /* truncation-induced collisions stay within 32 characters */
    std::string base(32, 'z');
    out = sanitize_unique({base + "1", base + "2"});
    CHECK(out[0] == std::string(32, 'z'));
    CHECK(out[1] == std::string(30, 'z') + "_2");
    CHECK(out[1].size() <= 32);
}

TEST_CASE("case-sensitive: A and a never collide") {
    auto out = sanitize_unique({"A", "a"});
    CHECK(out[0] == "A");
    CHECK(out[1] == "a");
}
