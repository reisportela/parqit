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

enum class StType { Byte, Int, Long, Float, Double, Str, StrL };

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

    /* range pass requirements */
    bool needs_minmax = false;     /* integer family: size byte/int/long/double */
    bool needs_strlen = false;     /* VARCHAR family: max octet_length */
    bool needs_big53 = false;      /* double-from-wide-int: check > 2^53 */
    bool needs_float_range = false; /* float32: promote to double > float max */
};

/* Decide the plan for one column given its DuckDB logical type (no data
 * seen yet). source_ref_sql is the quoted identifier to wrap in casts. */
ColumnPlan plan_read_column(const std::string &source_name, duckdb_logical_type t);

/* Refine a plan with observed statistics (NULL stats = all-null column). */
struct ColumnStats {
    bool has_minmax = false;
    double min = 0.0, max = 0.0;
    long long max_strlen = 0;
    bool any_beyond_2p53 = false;
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

} // namespace parqit
