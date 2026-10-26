/* Rows per block of the parallel cell pass of `parqit save, data`
 * (SAVE-PARALLEL-1), apart from the Stata calls so the unit tests pin it.
 *
 * A block is a whole number of requested row groups (chunk(), else the
 * engine's 122,880 rows), at least about half a million rows, and a multiple
 * of 8 rows: one Arrow batch per block points at the block's own bytes of the
 * shared validity bitmap with offset 0. A block that started inside a byte was
 * read shifted by the engine, and its neighbour's reset of the shared byte
 * erased nulls (F1, found by the audit of 25 Oct 2026 before any release; the
 * published 0.5.1 wrote one array). For a row-group size that is not a
 * multiple of 8 the unit is lcm(rg, 8), which can exceed the target.
 *
 * PARQIT_TEST_SAVE_BLOCK_ROWS (`hook`) lets the suite drive many blocks and
 * workers through small data: at most 4096 blocks, then whole validity bytes,
 * or lcm(rg, 8) rows when chunk() is given. A malformed hook is ignored. Every
 * product is checked; 0 means the arithmetic would overflow. */
#pragma once

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <numeric>

namespace parqit {

/* a * b for a, b > 0; false when it would overflow */
inline bool save_blocks_mul(long long a, long long b, long long *out) {
    if (a > std::numeric_limits<long long>::max() / b) return false;
    *out = a * b;
    return true;
}

/* ceil(a / b) for a >= 0, b > 0, without forming a + b - 1 */
inline long long save_blocks_ceil_div(long long a, long long b) { return a / b + (a % b != 0); }

inline long long save_block_rows(long long row_group_size, long long nrows, const char *hook) {
    constexpr long long kEngineRowGroup = 122880;
    constexpr long long kTarget = 4 * kEngineRowGroup;
    long long unit = 8;
    if (row_group_size > 0 &&
        !save_blocks_mul(row_group_size / std::gcd(row_group_size, 8LL), 8, &unit))
        return 0;
    long long rows = 0;
    if (hook) {
        char *end = nullptr;
        errno = 0;
        const long long v = std::strtoll(hook, &end, 10);
        if (end != hook && *end == '\0' && errno == 0 && v > 0) {
            const long long want = std::max(v, save_blocks_ceil_div(std::max(nrows, 0LL), 4096));
            return save_blocks_mul(save_blocks_ceil_div(want, unit), unit, &rows) ? rows : 0;
        }
    }
    if (row_group_size <= 0) unit = kEngineRowGroup;
    return save_blocks_mul(unit, save_blocks_ceil_div(kTarget, unit), &rows) ? rows : 0;
}

} // namespace parqit
