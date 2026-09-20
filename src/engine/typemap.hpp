/* parqit — the Stata ↔ DuckDB type contract (build brief §4, charter §6).
 *
 * All type policy lives here, in pure functions the unit tests exercise
 * without a Stata process:
 *
 *   - every readable DuckDB type maps to a canonical transfer type via an
 *     explicit SQL cast, so the Arrow walker only ever sees the canonical
 *     set; unrepresentable types are dropped-with-message, never silent
 *     all-missing columns (charter §6.11);
 *   - integer and string columns are sized by an observed-range pass using
 *     Stata's exact storage limits — an int32 of −2,147,483,648 lands in
 *     double, never in a missing code (charter §6.6);
 *   - Stata display formats are classified by *prefix* before any display
 *     token is looked at, so %tcHH:MM:SS is a datetime, full stop
 *     (charter §6.5), and %tm/%tq/%th/%tw/%ty/%tb/%tC stay integer period
 *     counts on disk (charter §6.3).
 */
#pragma once

#include <cmath>
#include <string>
#include <vector>

#include "duckdb.h"

namespace parqit {

/* ---- Stata storage limits (exact; missing codes live above the max) ---- */
constexpr double kStataByteMin = -127.0, kStataByteMax = 100.0;
constexpr double kStataIntMin = -32767.0, kStataIntMax = 32740.0;
constexpr double kStataLongMin = -2147483647.0, kStataLongMax = 2147483620.0;
constexpr int kStataStrMax = 2045;             /* str#; longer → strL */
constexpr double kDoubleExactInt = 9007199254740992.0; /* 2^53 */
/* largest non-missing value of a Stata float variable: float32 sources
 * with finite |v| above this must be stored as double, never missing */
constexpr double kStataFloatMax = 1.7014117331926443e+38;
/* Stata's first missing value (.) sentinel == SV_missval == 2^1023.
 * A finite double whose magnitude is >= this collides with Stata's missing
 * codes and is unstorable as an ordinary number, exactly like NaN/±Inf; the
 * eager fill and direct-save paths map such values to missing via SV_missval,
 * and the lazy paths use this engine-side constant (which must equal the
 * runtime SV_missval) so the two agree bit-for-bit. */
constexpr double kStataMissThreshold = 0x1p1023; /* 8.98846567431158e+307 */

/* XMISS-1: Stata's extended missing values. `.` is 2^1023 and `.a`..`.z` are
 * 2^1023 * (1 + k/4096), k = 1..26 — bits 0x7fe0000000000000 + (k << 40),
 * verified against Stata's own %21x output (.a = +1.0010000000000X+3ff,
 * .z = +1.01a0000000000X+3ff; a float .a promotes to the same double).
 * Parquet has one null, so `parqit save, xmissing` writes the code k of each
 * such cell into a TINYINT companion column named xmissing_companion_name()
 * (0 = not an extended missing; the primary cell is null either way) and lists
 * primary -> companion under the kXmissingMetaKey KV key; the readers hide the
 * companion and `parqit use` restores the cell from its code. */
constexpr int kStataExtMissMax = 26;
constexpr const char *kXmissingMetaKey = "parqit.xmissing";
constexpr const char *kXmissingPrefix = "_parqit_xm_";
/* the double for code 0..26 (0 = `.`); NaN for any other code */
double stata_missing_value(int code);
/* the exact code 0..26 of a Stata missing double; -1 for a value, NaN, ±Inf or
 * a magnitude in the missing range that is not one of the 27 codes */
int stata_missing_code(double d);
std::string xmissing_companion_name(const std::string &var);

enum class StType { Byte, Int, Long, Float, Double, Str, StrL };

/* ---- read-time type options (INT64-PROTECT-1 / BINARY-DECODE-1) -------
 * Stata has no 64-bit integer and no binary type. Both families used to be
 * handled silently-enough to lose data: a BIGINT/UBIGINT/HUGEINT/wide
 * DECIMAL beyond 2^53 loaded as a rounded double with a note (two distinct
 * keys could become one — audit 2026-09-19, T12), and a BLOB was simply
 * dropped. The user now chooses, and the DEFAULT refuses rather than
 * rounds. */
enum class Int64Mode {
    Refuse, /* default: fail the read naming the columns and both remedies */
    Round,  /* the historical behaviour: nearest double + the loud note     */
    String  /* the affected columns load as exact decimal text             */
};
enum class BinaryMode {
    Drop, /* default: dropped-with-message, as before */
    Text, /* decode(blob): UTF-8 text, loud on invalid UTF-8 */
    Hex   /* hex(blob): uppercase hex text, always valid */
};
/* parse the wire/user spelling; false (and *m untouched) on anything else */
bool int64_mode_parse(const std::string &s, Int64Mode *m);
bool binary_mode_parse(const std::string &s, BinaryMode *m);

/* storage-type code used on the wire and by st_addvar(): "byte", "int",
 * "long", "float", "double", "str7", "strL" */
std::string sttype_code(StType t, int str_bytes);

/* inverse of sttype_code; false on unknown codes */
bool sttype_parse(const std::string &code, StType *t, int *str_bytes);

/* ---- display-format storage classes --------------------------------- */
enum class FmtClass {
    None,  /* plain numeric/string format (or none) */
    Td,    /* %td  → DATE */
    Tc,    /* %tc  → TIMESTAMP (ms instant, no leap seconds) */
    TC,    /* %tC  → INTEGER count + parqit.fmt metadata (leap seconds) */
    Tm, Tq, Th, Tw, Ty, Tb, /* period counts → INTEGER + parqit.fmt metadata */
    Other
};
FmtClass classify_format(const std::string &fmt);
bool fmt_is_period_count(FmtClass c); /* Tm/Tq/Th/Tw/Ty/Tb/TC */

/* ---- read plan: one source column → one Stata variable ---------------- */
enum class Transfer { Int8, Int16, Int32, Int64, Float32, Float64,
                      Date32, TimestampUs, TimeUs, Utf8 };

struct ColumnPlan {
    std::string source_name;   /* exact source column name — the engine key */
    std::string stata_name;    /* sanitised; filled by the sanitiser */
    std::string meta_type;     /* original Stata type from parqit.* metadata ("") */
    std::string varlab;        /* variable label to restore ("") */
    std::string vallab;        /* value-label name to attach ("") */
    duckdb_type src_type = DUCKDB_TYPE_INVALID;

    bool dropped = false;      /* unrepresentable: dropped with message */
    std::string drop_reason;

    std::string cast_sql;      /* SQL over the quoted source ref; "" = as-is */
    Transfer transfer = Transfer::Float64;
    StType stata_type = StType::Double;
    int str_bytes = 0;         /* str# width (bytes) once known */
    std::string stata_format;  /* display format to apply; "" = none */
    std::string note;          /* loud per-column note (precision etc.) */
    bool note_subms = false;   /* us-resolution temporal with no static
                                * precision note: the fill emits a data-driven
                                * "sub-ms truncated" note only if a value
                                * actually loses sub-ms (T1). NS/TZ variants
                                * that already carry a static note stay false. */
    /* XMISS-1: the scan name of this column's extended-missing companion
     * (the file's parqit.xmissing pairs it with this column, it is an integer
     * column, and this column is numeric); "" when none. The fetch selects it
     * after the planned columns and the fill restores .a-.z from its codes. */
    std::string xm_source;

    /* range pass requirements */
    bool needs_minmax = false;     /* integer family: size byte/int/long/double */
    bool needs_strlen = false;     /* VARCHAR family: max octet_length */
    bool needs_big53 = false;      /* double-from-wide-int: check > 2^53 */
    bool needs_float_range = false; /* float32: promote to double > float max */
    /* FLOAT-EXACT-1 (audit 2026-08-22, V2.3): the manifest records `float`
     * but the engine column is not FLOAT (a %tc TIMESTAMP, the lazy integer
     * millisecond count, an INTEGER period count, a cast Hive key): restore
     * float storage only when every observed value is exactly representable
     * as a float32 — proved by a scan (stats.float_exact), never assumed. */
    bool needs_float_exact = false;
    bool float_exact_checked = false; /* the scan ran (with_stats planning) */
    bool float_exact = false;         /* … and every non-null value fits float */
};

/* Decide the plan for one column given its DuckDB logical type (no data
 * seen yet). source_ref_sql is the quoted identifier to wrap in casts.
 * `binary` decides what happens to a BLOB column (default: dropped). */
ColumnPlan plan_read_column(const std::string &source_name, duckdb_logical_type t,
                            BinaryMode binary = BinaryMode::Drop);

/* INT64-PROTECT-1: turn a plan whose values were proved to exceed 2^53
 * (needs_big53 && stats.any_beyond_2p53) into an EXACT text column, for
 * int64(string). Must be applied BEFORE refine_plan: it hands the width
 * decision to the existing needs_strlen sizing (the caller measures
 * max(strlen(CAST(col AS VARCHAR))) in the same stats pass) and clears
 * needs_big53 so refine_plan does not also announce a rounding that no
 * longer happens. */
void plan_big53_as_text(ColumnPlan &p);

/* Refine a plan with observed statistics (NULL stats = all-null column). */
struct ColumnStats {
    bool has_minmax = false;
    double min = 0.0, max = 0.0;
    long long max_strlen = 0;
    bool any_beyond_2p53 = false;
    bool float_exact_checked = false; /* FLOAT-EXACT-1 scan ran */
    bool float_exact = false;         /* every non-null value is float32-exact */
};
void refine_plan(ColumnPlan &p, const ColumnStats &s);

/* Pick the smallest Stata integer type that exactly holds [min,max]. */
StType integer_type_for_range(double min, double max);

/* Reconcile a plan with the original Stata type recorded in parqit.*
 * metadata: the saved type round-trips (a long saved through int32 comes
 * back long), widened further if third-party edits put values beyond its
 * range. No-op when the plan has no meta_type. */
void apply_meta_type(ColumnPlan &p);

/* ---- write plan: one Stata variable → one parquet column -------------- */
struct WriteColumn {
    std::string stata_name;
    std::string col_name;     /* name to write (defaults to stata_name) */
    StType stata_type = StType::Double;
    int str_bytes = 0;
    std::string stata_format;
    FmtClass fmt = FmtClass::None;
    std::string duck_type;    /* DuckDB column type in the staging table */
};

/* DuckDB column type for a Stata variable under the §4 contract. */
std::string duck_type_for(StType t, FmtClass fmt);

/* ---- calendar offsets (Stata epoch 1960-01-01; unix epoch 1970-01-01) -- */
constexpr long long kEpochShiftDays = 3653;
constexpr long long kEpochShiftMs = kEpochShiftDays * 86400000LL;
constexpr long long kEpochShiftUs = kEpochShiftMs * 1000LL;

/* floor division for negative-safe epoch arithmetic */
inline long long floordiv(long long a, long long b) {
    long long q = a / b, r = a % b;
    return (r != 0 && ((r < 0) != (b < 0))) ? q - 1 : q;
}

/* ---- temporal write conversions shared by every writer ------------------ */

/* Native Stata's round() resolves exact half ties toward +infinity:
 * floor(x + 0.5). TEMPORAL-ROUND-1 (audit 2026-08-22, A1-7): an integer-valued
 * x passes through UNCHANGED — for an odd integer in [2^52, 2^53) x + 0.5 is
 * not representable and rounds to even, so floor(x + 0.5) bumped exact day/ms/
 * period counts by +1 and flagged them "fractional". Below 2^52 x + 0.5 is
 * exact for every non-integer x. Both physical writers and the lazy
 * compile_for_save (see stata_round_temporal_sql) use this one rule. */
inline double stata_round_temporal(double d) {
    return (d == std::trunc(d)) ? d : std::floor(d + 0.5);
}
/* The same rule as SQL over an expression `ref` (evaluated twice: pass a
 * column reference or a cheap expression). */
std::string stata_round_temporal_sql(const std::string &ref);

/* %tc on disk: a Stata ms count since 1960 (already rounded to a whole ms,
 * i.e. integer-valued) -> epoch microseconds since 1970, EXACT (TC-US-1, audit
 * 2026-08-22 A1-1/A1-11). The former `ms * 1000.0` double product is not
 * representable for |us| >= 2^56 (year ~4253) and llround() landed up to 8 us
 * off, so instants beyond that read back 1 ms early ~30% of the time. Integer
 * arithmetic throughout: the integer-valued double converts to int64 exactly,
 * the epoch shift is subtracted in int64, and |ms| is bounded by
 * INT64_MAX/1000 so the product cannot overflow (DT-001 ceiling, unchanged
 * domain: the old check `|ms*1000.0| < 2^63` admitted exactly the same ms
 * values). Returns false when the instant does not fit the on-disk int64. */
bool stata_tc_ms_to_epoch_us(double stata_ms, long long *epoch_us);

/* A TIMESTAMP_NS column floored (toward -infinity) to microseconds as a
 * TIMESTAMP expression over the quoted reference `ref` (TS-NS-FLOOR-1). */
std::string timestamp_ns_floor_us_sql(const std::string &ref);

/* PERF-STREAM-1: roughly how many bytes of DuckDB result these plans will
 * produce for `nrows` rows — the size the streaming fetch uses to pick its
 * buffer (`streaming_buffer_size`). Pure arithmetic over the manifest, no
 * engine call, so the unit tests exercise it directly.
 *
 * Per column: the transfer type's vector width (Utf8 = the 16-byte
 * duckdb_string_t plus, for values longer than its 12 inlined bytes, the
 * planned octet width) plus one validity bit per cell; dropped columns
 * contribute nothing; the total carries 25% slack because vectors are
 * allocated in whole 2048-row chunks and the buffer accounting counts
 * allocated capacity. Saturates instead of overflowing.
 *
 * Deliberately an over- rather than an under-estimate: the result is used as a
 * *cap*, and memory is only ever occupied by chunks the engine actually
 * produced, so a generous estimate costs nothing while a short one throttles
 * the scan. Two known inexactitudes, both bounded: a `strL` column records no
 * width (str_bytes == 0 by construction), so it is charged kStataStrMax + 1 —
 * its floor, since that is why it became strL — which over-charges the usual
 * "a few long rows" column and under-charges a uniformly huge one; and NULLs
 * in a string column make the real payload smaller than the planned width. */
long long estimate_transfer_bytes(const std::vector<ColumnPlan> &plans,
                                  long long nrows);

} // namespace parqit
