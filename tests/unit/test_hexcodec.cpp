#include "doctest.h"

#include "engine/hexcodec.hpp"

using parqit::hex_decode;
using parqit::hex_encode;

/* Canonical vectors shared with the Mata twin (tests/integration/m0_smoke.do
 * asserts the same pairs). Change them in both places or not at all. */
TEST_CASE("hex encode canonical vectors") {
    CHECK(hex_encode("") == "");
    CHECK(hex_encode("parqit") == "706172716974");
    CHECK(hex_encode("Ol\xc3\xa1 \xf0\x9f\xa6\x86") == "4f6cc3a120f09fa686"); /* "Olá 🦆" */
    CHECK(hex_encode("a\"b'c\\d") == "61226227635c64");
}

TEST_CASE("hex decode inverts encode") {
    const char *samples[] = {"", "parqit", "Ol\xc3\xa1 \xf0\x9f\xa6\x86", "a\"b'c\\d",
                             "/path with spaces/qp_2002*.parquet"};
    for (const char *s : samples) {
        std::string out;
        REQUIRE(hex_decode(hex_encode(s), out));
        CHECK(out == s);
    }
}

TEST_CASE("hex decode accepts uppercase") {
    std::string out;
    REQUIRE(hex_decode("706172716974", out));
    CHECK(out == "parqit");
}

TEST_CASE("hex decode rejects malformed input") {
    std::string out;
    CHECK_FALSE(hex_decode("abc", out));   /* odd length */
    CHECK_FALSE(hex_decode("zz", out));    /* non-hex */
    CHECK_FALSE(hex_decode("6 1", out));   /* embedded space */
}

TEST_CASE("hex decode round-trips every byte value") {
    std::string all;
    for (int i = 1; i < 256; i++) all.push_back(static_cast<char>(i));
    std::string out;
    REQUIRE(hex_decode(hex_encode(all), out));
    CHECK(out == all);
}
