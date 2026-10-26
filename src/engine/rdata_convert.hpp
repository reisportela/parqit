/* parqit — the conversion of the rows of one R column (R-READ-1), shared by
 * the table function (parqit_read_rdata) and the Arrow batches of the
 * parallel conversion (RDATA-PARALLEL-1), so both write the same values.
 * Internal to the engine. */
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#include "engine/rdata_plan.hpp"
#include "engine/rdata_reader.hpp"

namespace parqit {
namespace rdata {

/* Reads `rows` elements of column c from its cursor (rows first+1.. of the
 * frame, 1-based) into `o`, which takes text(r, p, n), boolean(r, b),
 * i64(r, v), i32(r, v), f64(r, x), days(r, d), timestamp(r, us), time(r, us),
 * valid(r, bool) and code(r, int8_t) (the companion; ignored when there is
 * none). Returns "" or, for the first row whose value no longer fits the
 * profile, the table function's message; the cursors' own failures, and the
 * destination's, throw. *failed_row is the row being converted, so it names
 * the failing row whichever way the conversion stops (F12). */
template <class Out>
std::string convert_rows(const Plan &plan, const ColumnSpec &c, Cursor &cur, uint64_t first,
                         uint64_t rows, Out &o, std::string *text, uint64_t *failed_row = nullptr) {
    for (uint64_t r = 0; r < rows; r++) {
        if (failed_row) *failed_row = first + r;
        bool ok = true, null = false;
        int code = 0;
        switch (c.kind) {
        case OutKind::Varchar: {
            if (c.conv == Conv::FactorText) {
                const int32_t k = cur.next_int();
                if (k == kNaInteger || k < 1 || static_cast<size_t>(k) > c.levels.size()) {
                    null = true;
                } else {
                    const std::string &t = c.levels[static_cast<size_t>(k) - 1];
                    o.text(r, t.data(), t.size());
                }
            } else {
                bool tr = false;
                if (cur.next_string(text, &tr)) o.text(r, text->data(), text->size());
                else null = true;
            }
            break;
        }
        case OutKind::Boolean: {
            const int32_t x = cur.next_int();
            if (x == kNaInteger) null = true;
            else o.boolean(r, x != 0);
            break;
        }
        case OutKind::BigInt: {
            const double x = cur.next_real();
            int64_t b;
            std::memcpy(&b, &x, 8);
            if (b == INT64_MIN) null = true;
            else o.i64(r, b);
            break;
        }
        default: {
            /* numbers and dates: NA (tagged or not), user-missing, value */
            const bool is_int = c.vec.type == INTSXP;
            double x = 0.0;
            int32_t xi = 0;
            if (is_int) {
                xi = cur.next_int();
                if (xi == kNaInteger) {
                    null = true;
                    break;
                }
                x = xi;
            } else {
                x = cur.next_real();
                if (is_na_real(x)) {
                    null = true;
                    if (c.tagged) {
                        const char t = na_tag(x);
                        if (t >= 'a' && t <= 'z') code = t - 'a' + 1;
                    }
                    break;
                }
            }
            if (!std::isnan(x) && c.user_missing(x)) {
                null = true;
                code = c.code_of(x);
                break;
            }
            switch (c.conv) {
            case Conv::IntDays:
                o.days(r, xi);
                break;
            /* a value that does not convert is never written: the
             * converters leave their output unset when they fail (G5) */
            case Conv::RealDays: {
                if (!std::isfinite(x)) {
                    null = true;
                    break;
                }
                int32_t d;
                ok = days_to_date(x, &d);
                if (ok) o.days(r, d);
                break;
            }
            case Conv::RealDaysTs: {
                if (!std::isfinite(x)) {
                    null = true;
                    break;
                }
                int64_t us;
                ok = days_to_us(x, &us);
                if (ok) o.timestamp(r, us);
                break;
            }
            case Conv::IntSecTs:
                o.timestamp(r, static_cast<int64_t>(xi) * 1000000);
                break;
            case Conv::RealSecTs: {
                if (!std::isfinite(x)) {
                    null = true;
                    break;
                }
                int64_t us;
                ok = seconds_to_us(x, &us);
                if (ok) o.timestamp(r, us);
                break;
            }
            case Conv::IntSecTime:
                ok = xi >= 0 && xi < 86400;
                if (ok) o.time(r, static_cast<int64_t>(xi) * 1000000);
                break;
            case Conv::RealSecTime: {
                int64_t us;
                ok = seconds_to_time_us(x, &us);
                if (ok) o.time(r, us);
                break;
            }
            default:
                if (c.kind == OutKind::Integer) o.i32(r, xi);
                else o.f64(r, x);
            }
        }
        }
        if (!ok) {
            /* the profile proved every value fits; one that does not means
             * the file is not the one profiled */
            return "the R data file " + plan.path + " changed while it was being converted (column " +
                   c.name + ", row " + std::to_string(first + r + 1) + "); convert it again";
        }
        o.valid(r, !null);
        o.code(r, static_cast<int8_t>(code));
    }
    return "";
}

} // namespace rdata
} // namespace parqit
