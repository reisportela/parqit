/* parqit — Stata's text-to-date functions (DATEFN-1, BPLIM feedback 2026-10-13).
 *
 * date(s, mask[, topyear]), clock(s, mask[, topyear]) and the period readers
 * monthly()/quarterly()/halfyearly()/weekly()/yearly() parse text the way
 * native Stata does (verified against StataNow 19.5, local/rute_2026-10-13/
 * oracles/nat_dates2.log): the string is cut into fields — runs of digits,
 * month names, am/pm — separated by anything else; the mask letters take the
 * fields in order (# skips one); a string with more or fewer fields than the
 * mask is missing; digits run together (20060125) are split by the letters'
 * widths (Y 4, the others 2, the last letter of a run taking the rest); a
 * one- or two-digit year needs a century in the mask (20Y, 19Y) or topyear,
 * which maps it to the latest year not after topyear; an impossible date, an
 * hour of 24, a 60th second or more than three decimals of a second are
 * missing. These are pure functions, unit-tested without DuckDB; the session
 * registers them as __parqit_date, __parqit_clock and __parqit_period. */
#pragma once

#include <string>

namespace parqit::datefn {

/* "" when `mask` is valid for the function kind: 'd' (date: Y M D #, with an
 * optional century before Y), 'c' (clock: Y M D h m s #) or one of the period
 * units 'm' 'q' 'h' 'w' 'y' (Y and that unit letter, Q H W M, or Y alone);
 * else a message naming the problem. */
std::string check_mask(const std::string &mask, char kind);

/* day count since 01jan1960, or false (Stata missing) */
bool parse_date(const std::string &s, const std::string &mask, long long topyear,
                long long *days);
/* milliseconds since 01jan1960 00:00:00, or false */
bool parse_clock(const std::string &s, const std::string &mask, long long topyear,
                 long long *ms);
/* the period count (months, quarters, half-years or weeks since 1960; the
 * year itself for 'y'), or false */
bool parse_period(const std::string &s, const std::string &mask, char unit, long long topyear,
                  long long *value);

/* calendar helpers shared with the translator's tests */
long long days_from_civil(int y, int m, int d); /* since 01jan1960 */
bool civil_from_days(long long days, int *y, int *m, int *d);
int days_in_month(int y, int m);

} // namespace parqit::datefn
