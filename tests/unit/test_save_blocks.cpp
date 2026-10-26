/* The rows per block of the parallel cell pass of `parqit save, data` (F1,
 * 2026-10-25): every block is a multiple of 8 rows and of the requested row
 * group, so the blocks tile the validity bitmap in whole bytes. */
#include "doctest.h"
#include "engine/save_blocks.hpp"

#include <limits>
#include <numeric>

using parqit::save_block_rows;

namespace {
constexpr long long kTarget = 491520;
long long lcm8(long long rg) { return rg / std::gcd(rg, 8LL) * 8; }
} // namespace

TEST_CASE("save blocks: whole requested row groups and whole bitmap bytes") {
    CHECK(save_block_rows(-1, 2500003, nullptr) == kTarget);
    CHECK(save_block_rows(-1, 0, nullptr) == kTarget);
    CHECK(save_block_rows(2048, 2500003, nullptr) == kTarget);
    CHECK(save_block_rows(122880, 1, nullptr) == kTarget);
    CHECK(save_block_rows(1001, 1, nullptr) == 496496);      /* 8008 x 62 */
    CHECK(save_block_rows(100001, 1, nullptr) == 800008);    /* lcm above the target */
    CHECK(save_block_rows(250002, 1, nullptr) == 1000008);
    CHECK(save_block_rows(500010, 1, nullptr) == 2000040);
    CHECK(save_block_rows(1000000, 1, nullptr) == 1000000);
    CHECK(save_block_rows(1000001, 1, nullptr) == 8000008);
    CHECK(save_block_rows(7, 1, nullptr) == 491568);         /* 56 x 8778 */
    for (long long rg = 1; rg <= 20000; rg++) {
        const long long rows = save_block_rows(rg, 1, nullptr);
        const long long unit = lcm8(rg);
        INFO("chunk " << rg);
        REQUIRE(rows % 8 == 0);
        REQUIRE(rows % rg == 0);
        REQUIRE(rows >= kTarget);
        REQUIRE(rows < kTarget + unit);
    }
    for (long long rg : {491519LL, 491521LL, 999999LL, 2147483647LL}) {
        const long long rows = save_block_rows(rg, 1, nullptr);
        CHECK(rows == lcm8(rg));
    }
}

TEST_CASE("save blocks: the test hook keeps its contract") {
    /* without chunk(): whole bitmap bytes only (v179: 1003 rows in 126 blocks) */
    CHECK(save_block_rows(-1, 1003, "8") == 8);
    CHECK(save_block_rows(-1, 1003, "5") == 8);
    CHECK(save_block_rows(-1, 1003, "9") == 16);
    /* at most 4096 blocks, then whole bytes */
    CHECK(save_block_rows(-1, 100000, "8") == 32); /* ceil(100000/4096) = 25 */
    CHECK(save_block_rows(-1, 4096 * 8, "1") == 8);
    /* with chunk(): whole row groups and whole bytes */
    CHECK(save_block_rows(3, 1003, "8") == 24);
    CHECK(save_block_rows(4, 1003, "8") == 8);
    CHECK(save_block_rows(12, 1003, "8") == 24);
    CHECK(save_block_rows(1001, 1003, "8") == 8008);
    for (long long rg = 1; rg <= 300; rg++) {
        const long long rows = save_block_rows(rg, 1003, "8");
        INFO("chunk " << rg);
        REQUIRE(rows % 8 == 0);
        REQUIRE(rows % rg == 0);
        REQUIRE(rows < 8 + lcm8(rg));
    }
}

TEST_CASE("save blocks: a malformed hook is ignored, overflow is refused") {
    for (const char *bad : {"", "abc", "8x", "-5", "0", "99999999999999999999999"})
        CHECK(save_block_rows(-1, 1003, bad) == kTarget);
    CHECK(save_block_rows(1001, 1003, "abc") == 496496);
    const long long big = std::numeric_limits<long long>::max();
    CHECK(save_block_rows(big, 1, nullptr) == 0);                 /* odd: lcm overflows */
    CHECK(save_block_rows((1LL << 62) + 1, 1, nullptr) == 0);
    CHECK(save_block_rows(1LL << 62, 1, nullptr) == 1LL << 62);   /* a multiple of 8 */
    CHECK(save_block_rows(3, 1, "9223372036854775807") == 0);     /* rounding overflows */
}
