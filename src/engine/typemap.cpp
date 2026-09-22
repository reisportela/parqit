#include "engine/typemap.hpp"

#include <cmath>
#include <limits>

#include "engine/session.hpp" /* quote_ident */

namespace parqit {

/* XMISS-1 (see typemap.hpp): the 27 Stata missing doubles and their codes.
 * (1 + k/4096) and its product with 2^1023 are exact in binary64, so the
 * round trip is bit-exact; anything that is not one of the 27 values (a
 * magnitude between two codes, ±Inf, NaN, a value) yields -1 and is treated
 * by the callers as it always was. */
double stata_missing_value(int code) {
    if (code < 0 || code > kStataExtMissMax)
        return std::numeric_limits<double>::quiet_NaN();
    return kStataMissThreshold * (1.0 + static_cast<double>(code) / 4096.0);
}

int stata_missing_code(double d) {
    if (!(d >= kStataMissThreshold) || std::isinf(d)) return -1;
    const double k = (d / kStataMissThreshold - 1.0) * 4096.0;
    if (k < 0.0 || k > static_cast<double>(kStataExtMissMax)) return -1;
    const int code = static_cast<int>(k);
    return k == static_cast<double>(code) ? code : -1;
}

std::string xmissing_companion_name(const std::string &var) {
    return std::string(kXmissingPrefix) + var;
}

std::string sttype_code(StType t, int str_bytes) {
    switch (t) {
    case StType::Byte: return "byte";
    case StType::Int: return "int";
    case StType::Long: return "long";
    case StType::Float: return "float";
    case StType::Double: return "double";
    case StType::Str: return "str" + std::to_string(str_bytes < 1 ? 1 : str_bytes);
    case StType::StrL: return "strL";
    }
    return "double";
}

bool sttype_parse(const std::string &code, StType *t, int *str_bytes) {
    *str_bytes = 0;
    if (code == "byte") { *t = StType::Byte; return true; }
    if (code == "int") { *t = StType::Int; return true; }
    if (code == "long") { *t = StType::Long; return true; }
    if (code == "float") { *t = StType::Float; return true; }
    if (code == "double") { *t = StType::Double; return true; }
    if (code == "strL") { *t = StType::StrL; return true; }
    if (code.size() > 3 && code.compare(0, 3, "str") == 0) {
        int n = std::atoi(code.c_str() + 3);
        if (n <= 0) return false;
        *t = StType::Str;
        *str_bytes = n;
        return true;
    }
    return false;
}

FmtClass classify_format(const std::string &fmt) {
    /* Prefix classification only — display tokens after the class prefix
     * (HH:MM, CCYY, …) must never change the storage class (charter §6.5).
     * Stata time formats are %t? or %-t? (left-justified). */
    if (fmt.size() < 2 || fmt[0] != '%') return FmtClass::None;
    size_t i = 1;
    if (fmt[i] == '-') i++;
    /* DFMT-1 (audit 2026-09-01, F6): the old-style daily date format %d
     * (with or without display tokens: %d, %-d, %dCCYY-NN-DD, %dM_d,_CY) is
     * documented by Stata as a synonym of %td; it used to fall through to
     * "plain numeric" and was written as a raw INT32 instead of a DATE. No
     * numeric format starts with %d followed by anything but a digit or '.'
     * (%9.2f, %-12.0g …), so the letter alone decides. */
    if (i < fmt.size() && fmt[i] == 'd' &&
        (i + 1 == fmt.size() ||
         !((fmt[i + 1] >= '0' && fmt[i + 1] <= '9') || fmt[i + 1] == '.')))
        return FmtClass::Td;
    if (i + 1 >= fmt.size() || fmt[i] != 't') return FmtClass::None;
    switch (fmt[i + 1]) {
    case 'd': return FmtClass::Td;
    case 'c': return FmtClass::Tc;
    case 'C': return FmtClass::TC;
    case 'm': return FmtClass::Tm;
    case 'q': return FmtClass::Tq;
    case 'h': return FmtClass::Th;
    case 'w': return FmtClass::Tw;
    case 'y': return FmtClass::Ty;
    case 'b': return FmtClass::Tb;
    default: return FmtClass::Other; /* %tg or unknown t-class: keep raw */
    }
}

bool fmt_is_period_count(FmtClass c) {
    switch (c) {
    case FmtClass::Tm:
    case FmtClass::Tq:
    case FmtClass::Th:
    case FmtClass::Tw:
    case FmtClass::Ty:
    case FmtClass::Tb:
    case FmtClass::TC:
        return true;
    default:
        return false;
    }
}

StType integer_type_for_range(double min, double max) {
    if (min >= kStataByteMin && max <= kStataByteMax) return StType::Byte;
    if (min >= kStataIntMin && max <= kStataIntMax) return StType::Int;
    if (min >= kStataLongMin && max <= kStataLongMax) return StType::Long;
    return StType::Double;
}

bool int64_mode_parse(const std::string &s, Int64Mode *m) {
    if (s == "refuse") { *m = Int64Mode::Refuse; return true; }
    if (s == "round") { *m = Int64Mode::Round; return true; }
    if (s == "string") { *m = Int64Mode::String; return true; }
    return false;
}

bool binary_mode_parse(const std::string &s, BinaryMode *m) {
    if (s == "drop") { *m = BinaryMode::Drop; return true; }
    if (s == "text") { *m = BinaryMode::Text; return true; }
    if (s == "hex") { *m = BinaryMode::Hex; return true; }
    return false;
}

ColumnPlan plan_read_column(const std::string &source_name, duckdb_logical_type t,
                            BinaryMode binary) {
    ColumnPlan p;
    p.source_name = source_name;
    p.src_type = duckdb_get_type_id(t);
    const std::string ref = quote_ident(source_name);

    switch (p.src_type) {
    case DUCKDB_TYPE_BOOLEAN:
        p.cast_sql = "CAST(" + ref + " AS TINYINT)";
        p.transfer = Transfer::Int8;
        p.stata_type = StType::Byte;
        break;
    case DUCKDB_TYPE_TINYINT:
        p.transfer = Transfer::Int8;
        p.stata_type = StType::Int; /* refined: -128/101..127 exceed byte */
        p.needs_minmax = true;
        break;
    case DUCKDB_TYPE_SMALLINT:
        p.transfer = Transfer::Int16;
        p.stata_type = StType::Long; /* refined by range */
        p.needs_minmax = true;
        break;
    case DUCKDB_TYPE_INTEGER:
        p.transfer = Transfer::Int32;
        p.stata_type = StType::Double; /* refined: int32 edges exceed long */
        p.needs_minmax = true;
        break;
    case DUCKDB_TYPE_BIGINT:
        p.transfer = Transfer::Int64;
        p.stata_type = StType::Double; /* refined by range */
        p.needs_minmax = true;
        p.needs_big53 = true;
        break;
    case DUCKDB_TYPE_UTINYINT:
        p.cast_sql = "CAST(" + ref + " AS SMALLINT)";
        p.transfer = Transfer::Int16;
        p.stata_type = StType::Int;
        p.needs_minmax = true;
        break;
    case DUCKDB_TYPE_USMALLINT:
        p.cast_sql = "CAST(" + ref + " AS INTEGER)";
        p.transfer = Transfer::Int32;
        p.stata_type = StType::Long;
        p.needs_minmax = true;
        break;
    case DUCKDB_TYPE_UINTEGER:
        /* charter §6.6: values ≥ 2^31 must survive — go through BIGINT */
        p.cast_sql = "CAST(" + ref + " AS BIGINT)";
        p.transfer = Transfer::Int64;
        p.stata_type = StType::Double; /* refined: long when max allows */
        p.needs_minmax = true;
        break;
    case DUCKDB_TYPE_UBIGINT:
    case DUCKDB_TYPE_HUGEINT:
    case DUCKDB_TYPE_UHUGEINT:
        p.cast_sql = "__parqit_double(" + ref + ")";
        p.transfer = Transfer::Float64;
        p.stata_type = StType::Double;
        p.needs_big53 = true; /* loud when > 2^53 rounds */
        break;
    case DUCKDB_TYPE_FLOAT:
        p.transfer = Transfer::Float32;
        p.stata_type = StType::Float;
        /* float32 reaches ±3.4e38 but Stata float stops at ±1.70e38:
         * finite values in between must widen the column, not vanish */
        p.needs_float_range = true;
        break;
    case DUCKDB_TYPE_DOUBLE:
        p.transfer = Transfer::Float64;
        p.stata_type = StType::Double;
        break;
    case DUCKDB_TYPE_DECIMAL:
        /* charter §6.11: warehouse money loads as numbers, never missing */
        p.cast_sql = "__parqit_double(" + ref + ")";
        p.transfer = Transfer::Float64;
        p.stata_type = StType::Double;
        p.note = "decimal converted to double";
        /* DEC-1: a wide DECIMAL whose integer part can exceed 2^53 loses
         * low-order digits when cast to double. Flag it so refine_plan emits the
         * same explicit ">2^53 rounded" note that BIGINT/HUGEINT already get,
         * instead of leaving the loss indistinguishable from an exact load.
         * Narrow decimals (integer part < 2^53) skip the extra range pass. */
        if (duckdb_decimal_width(t) - duckdb_decimal_scale(t) >= 16)
            p.needs_big53 = true;
        break;
    case DUCKDB_TYPE_DATE:
        p.transfer = Transfer::Date32;
        p.stata_type = StType::Long;
        p.stata_format = "%td";
        break;
    case DUCKDB_TYPE_TIMESTAMP:
        p.transfer = Transfer::TimestampUs;
        p.stata_type = StType::Double;
        p.stata_format = "%tc";
        p.note_subms = true; /* us -> ms: note only if sub-ms is really lost */
        break;
    case DUCKDB_TYPE_TIMESTAMP_S:
    case DUCKDB_TYPE_TIMESTAMP_MS:
        p.cast_sql = "CAST(" + ref + " AS TIMESTAMP)";
        p.transfer = Transfer::TimestampUs;
        p.stata_type = StType::Double;
        p.stata_format = "%tc";
        break;
    case DUCKDB_TYPE_TIMESTAMP_NS:
        /* TS-NS-FLOOR-1 (audit 2026-08-22, A1-9): CAST(TIMESTAMP_NS AS
         * TIMESTAMP) truncates toward ZERO, so a pre-1970 instant within 1 us
         * below a millisecond boundary landed 1 ms LATER than the documented
         * floor. Floor the nanosecond count to microseconds in integer
         * arithmetic (correcting DuckDB's // toward zero) and rebuild
         * the instant; the ms floor then happens in the fill/ts_ms_sql. */
        p.cast_sql = timestamp_ns_floor_us_sql(ref);
        p.transfer = Transfer::TimestampUs;
        p.stata_type = StType::Double;
        p.stata_format = "%tc";
        p.note = "nanosecond timestamp truncated to Stata millisecond resolution";
        break;
    case DUCKDB_TYPE_TIMESTAMP_TZ:
        p.cast_sql = "CAST(" + ref + " AS TIMESTAMP)";
        p.transfer = Transfer::TimestampUs;
        p.stata_type = StType::Double;
        p.stata_format = "%tc";
        p.note = "timezone-aware timestamp stored as the UTC instant";
        p.note_subms = true; /* the TZ note is about the instant, not precision */
        break;
    case DUCKDB_TYPE_TIME:
        /* charter §6.5: a time-of-day column must never arrive all-null.
         * ms since midnight displayed with %tcHH:MM:SS is exact because
         * Stata's %tc day zero is 1960-01-01 00:00. */
        p.cast_sql = "CAST(DATE '1970-01-01' + " + ref + " AS TIMESTAMP)";
        p.transfer = Transfer::TimeUs;
        p.stata_type = StType::Double;
        p.stata_format = "%tcHH:MM:SS";
        p.note = "time-of-day stored as milliseconds since midnight";
        p.note_subms = true; /* encoding note above is not about sub-ms loss */
        break;
    case DUCKDB_TYPE_TIME_NS:
        p.cast_sql = "CAST(DATE '1970-01-01' + CAST(" + ref + " AS TIME) AS TIMESTAMP)";
        p.transfer = Transfer::TimeUs;
        p.stata_type = StType::Double;
        p.stata_format = "%tcHH:MM:SS";
        /* TS-NS-1: like TIMESTAMP_NS, sub-millisecond precision is discarded —
         * say so explicitly rather than leaving the loss unannounced. */
        p.note = "nanosecond time-of-day truncated to Stata millisecond "
                 "resolution; stored as milliseconds since midnight";
        break;
    case DUCKDB_TYPE_TIME_TZ:
        p.cast_sql = "CAST(DATE '1970-01-01' + CAST(" + ref + " AS TIME) AS TIMESTAMP)";
        p.transfer = Transfer::TimeUs;
        p.stata_type = StType::Double;
        p.stata_format = "%tcHH:MM:SS";
        p.note = "time-of-day stored as milliseconds since midnight (offset discarded)";
        p.note_subms = true;
        break;
    case DUCKDB_TYPE_VARCHAR:
        /* JSON-logical columns report their type-id as VARCHAR but reject
         * strlen()/direct projection on the native JSON type (a binder error),
         * exactly like ENUM/UUID below. Cast to VARCHAR so the sizing scan and
         * the fetch/save SELECT both bind, and a JSON column loads as its text
         * form instead of failing the whole file (N1). A no-op for a true
         * VARCHAR; DuckDB folds CAST(varchar AS VARCHAR) away. */
        p.cast_sql = "CAST(" + ref + " AS VARCHAR)";
        p.transfer = Transfer::Utf8;
        p.stata_type = StType::Str;
        p.needs_strlen = true;
        break;
    case DUCKDB_TYPE_ENUM:
        p.cast_sql = "CAST(" + ref + " AS VARCHAR)";
        p.transfer = Transfer::Utf8;
        p.stata_type = StType::Str;
        p.needs_strlen = true;
        break;
    case DUCKDB_TYPE_UUID:
        p.cast_sql = "CAST(" + ref + " AS VARCHAR)";
        p.transfer = Transfer::Utf8;
        p.stata_type = StType::Str;
        p.needs_strlen = true;
        break;
    case DUCKDB_TYPE_BLOB:
        /* BINARY-DECODE-1: raw bytes have no Stata representation, so the
         * default still drops the column with a message — but the message now
         * names the two ways to load it, and both produce TEXT sized by the
         * ordinary strlen pass (strL beyond 2045 bytes, as for any string).
         *   decode(BLOB) -> VARCHAR, and THROWS a ConversionException on an
         *     invalid UTF-8 byte sequence instead of substituting replacement
         *     characters (verified in the fetched DuckDB v1.5.3 source:
         *     extension/core_functions/scalar/blob/encode.cpp:36-51 for the
         *     check/throw, :105-107 for the BLOB->VARCHAR signature);
         *   hex(BLOB)    -> VARCHAR, two UPPERCASE hex digits per byte, never
         *     fails (extension/core_functions/scalar/string/hex.cpp:66-85
         *     HexStrOperator, registered at :394; the digits come from
         *     Blob::HEX_TABLE = "0123456789ABCDEF",
         *     src/include/duckdb/common/types/blob.hpp:21). */
        if (binary == BinaryMode::Drop) {
            p.dropped = true;
            p.drop_reason = "BLOB has no Stata representation (add binary(text) "
                            "or binary(hex) to load it)";
            break;
        }
        p.cast_sql = (binary == BinaryMode::Text ? "decode(" : "hex(") + ref + ")";
        p.transfer = Transfer::Utf8;
        p.stata_type = StType::Str;
        p.needs_strlen = true;
        p.note = binary == BinaryMode::Text
                     ? "binary column decoded as UTF-8 text (binary(text))"
                     : "binary column loaded as uppercase hex text (binary(hex))";
        break;
    default: {
        /* charter §6.11: unrepresentable types are dropped with a message
         * (the caller errors out if every column would be dropped). A
         * NULL-typed column carries no type and no data, so it is dropped
         * loudly here too — never silently loaded as an all-missing byte
         * variable indistinguishable from a real one (brief §4, §6.11). */
        const char *what = "unsupported";
        switch (p.src_type) {
        case DUCKDB_TYPE_SQLNULL: what = "NULL"; break;
        /* BLOB no longer reaches here: it has its own case above, whose drop
         * message also names binary(text)/binary(hex) */
        case DUCKDB_TYPE_BIT: what = "BIT"; break;
        case DUCKDB_TYPE_INTERVAL: what = "INTERVAL"; break;
        case DUCKDB_TYPE_LIST: what = "LIST"; break;
        case DUCKDB_TYPE_ARRAY: what = "ARRAY"; break;
        case DUCKDB_TYPE_STRUCT: what = "STRUCT"; break;
        case DUCKDB_TYPE_MAP: what = "MAP"; break;
        case DUCKDB_TYPE_UNION: what = "UNION"; break;
        case DUCKDB_TYPE_BIGNUM: what = "BIGNUM"; break;
        case DUCKDB_TYPE_GEOMETRY: what = "GEOMETRY"; break;
        case DUCKDB_TYPE_VARIANT: what = "VARIANT"; break;
        default: break;
        }
        p.dropped = true;
        p.drop_reason = std::string(what) + " has no Stata representation";
        break;
    }
    }
    return p;
}

void plan_big53_as_text(ColumnPlan &p) {
    /* INT64-PROTECT-1: the ONLY exact way into Stata for an integer beyond
     * 2^53. CAST(<integer> AS VARCHAR) is digit-for-digit exact — DuckDB
     * formats the integer value itself (NumericHelper::FormatSigned, fetched
     * v1.5.3 src/include/duckdb/common/types/cast_helpers.hpp:64-78, with the
     * hugeint_t specialisation declared at :107 and DecimalToString at
     * :109-127) — no double is ever constructed, unlike __parqit_double().
     * The width comes from the caller's max(strlen(...)) over the SAME
     * expression, so str19/str20 (or wider for HUGEINT/DECIMAL) is exact. */
    p.cast_sql = "CAST(" + quote_ident(p.source_name) + " AS VARCHAR)";
    p.transfer = Transfer::Utf8;
    p.stata_type = StType::Str;
    p.str_bytes = 0;
    p.needs_strlen = true;
    /* the numeric range/precision passes no longer describe this column */
    p.needs_minmax = false;
    p.needs_big53 = false; /* nothing rounds: refine_plan must not say it does */
    p.needs_float_range = false;
    p.needs_float_exact = false;
    p.float_exact_checked = false;
    /* a numeric display format or a value label cannot be applied to a Stata
     * string variable (both would abort the load); the exact digits are the
     * payload */
    p.stata_format.clear();
    p.vallab.clear();
    /* REPLACES any earlier note rather than appending to it: a wide DECIMAL
     * arrives carrying "decimal converted to double", which is no longer true
     * of this column and would contradict the line right after it. No other
     * needs_big53 note exists at this point (BIGINT/UBIGINT/HUGEINT set none,
     * and refine_plan's all-missing note comes later, from needs_minmax,
     * which is cleared above). */
    p.note = "loaded as text because values exceed 2^53 (exact)";
}

void refine_plan(ColumnPlan &p, const ColumnStats &s) {
    if (p.dropped) return;
    if (p.needs_minmax) {
        if (!s.has_minmax) {
            /* all-null integer column: smallest type that exists. Say so — an
             * all-missing column must never be silent (brief §4/§6.11). */
            p.stata_type = StType::Byte;
            p.note = (p.note.empty() ? "" : p.note + "; ") +
                     std::string("every value is missing; loaded as an "
                                 "all-missing byte variable");
        } else {
            p.stata_type = integer_type_for_range(s.min, s.max);
        }
        /* period/date formats keep their integer storage wide enough; a
         * plain display format (%8.0g, %9.2f, …) says nothing about range
         * and must never widen the storage type (TYPE-1: parqit-written
         * files always carry a fmt, so byte columns loaded back as int) */
        if (p.stata_type == StType::Byte &&
            classify_format(p.stata_format) != FmtClass::None)
            p.stata_type = StType::Int;
    }
    if (p.needs_big53 && s.any_beyond_2p53) {
        p.note = (p.note.empty() ? "" : p.note + "; ") +
                 std::string("values beyond 2^53 rounded to nearest double");
    }
    if (p.needs_float_range && s.has_minmax &&
        (std::fabs(s.min) > kStataFloatMax || std::fabs(s.max) > kStataFloatMax)) {
        p.stata_type = StType::Double;
        p.note = (p.note.empty() ? "" : p.note + "; ") +
                 std::string("float32 values beyond Stata's float range; "
                             "stored as double");
    }
    /* FLOAT-EXACT-1: carry the scan's verdict to apply_meta_type */
    if (p.needs_float_exact && s.float_exact_checked) {
        p.float_exact_checked = true;
        p.float_exact = s.float_exact;
    }
    if (p.needs_strlen) {
        long long len = s.max_strlen;
        if (len <= 0) len = 1; /* all-null/empty strings: str1 */
        if (len > kStataStrMax) {
            p.stata_type = StType::StrL;
            p.str_bytes = 0;
        } else {
            p.stata_type = StType::Str;
            p.str_bytes = static_cast<int>(len);
        }
    }
}

static int int_rank(StType t) {
    switch (t) {
    case StType::Byte: return 1;
    case StType::Int: return 2;
    case StType::Long: return 3;
    case StType::Double: return 4;
    default: return 0; /* not in the integer-capacity ladder */
    }
}

void apply_meta_type(ColumnPlan &p) {
    if (p.meta_type.empty() || p.dropped) return;
    StType mt;
    int mbytes = 0;
    if (!sttype_parse(p.meta_type, &mt, &mbytes)) return;

    if (p.stata_type == StType::Str || p.stata_type == StType::StrL) {
        if (mt == StType::StrL) {
            p.stata_type = StType::StrL;
            p.str_bytes = 0;
        } else if (mt == StType::Str && p.stata_type == StType::Str) {
            /* observed width still rules an upper bound; saved width rules
             * the round-trip floor */
            if (mbytes > p.str_bytes) p.str_bytes = mbytes;
            if (p.str_bytes > kStataStrMax) {
                p.stata_type = StType::StrL;
                p.str_bytes = 0;
            }
        }
        return;
    }
    /* numeric: float/double round-trip as themselves; integer ladder takes
     * the wider of (saved type, observed range type) */
    if (mt == StType::Float &&
        (p.stata_type == StType::Float || int_rank(p.stata_type) > 0)) {
        if (p.stata_type == StType::Float) return;
        /* FLOAT-EXACT-1 (audit 2026-08-22, V2.3): a manifest float held in a
         * non-FLOAT engine column (a %tc TIMESTAMP, an integer ms/period
         * count, a cast Hive key) comes back float when the scan proved every
         * value float32-exact — a %tc whose ms range exceeds long was stuck at
         * double although its values fit — and double when a value does not
         * fit (a foreign edit), never a silently rounded float. Without a scan
         * (describe) the saved type is the honest display unless the range
         * already forced double. */
        if (p.float_exact_checked)
            p.stata_type = p.float_exact ? StType::Float : StType::Double;
        else if (p.stata_type != StType::Double)
            p.stata_type = StType::Float;
        return;
    }
    int mr = int_rank(mt), pr = int_rank(p.stata_type);
    if (mr > 0 && pr > 0 && mr > pr) p.stata_type = mt;
}

std::string duck_type_for(StType t, FmtClass fmt) {
    switch (fmt) {
    case FmtClass::Td: return "DATE";
    case FmtClass::Tc: return "TIMESTAMP";
    case FmtClass::TC:
        return "BIGINT"; /* leap-second ms counts exceed int32 (charter §6.3) */
    case FmtClass::Tm:
    case FmtClass::Tq:
    case FmtClass::Th:
    case FmtClass::Tw:
    case FmtClass::Ty:
    case FmtClass::Tb:
        return "INTEGER"; /* charter §6.3: period counts stay integers */
    default: break;
    }
    switch (t) {
    case StType::Byte: return "TINYINT";
    case StType::Int: return "SMALLINT";
    case StType::Long: return "INTEGER";
    case StType::Float: return "FLOAT";
    case StType::Double: return "DOUBLE";
    case StType::Str:
    case StType::StrL: return "VARCHAR";
    }
    return "DOUBLE";
}

std::string stata_round_temporal_sql(const std::string &ref) {
    /* TEMPORAL-ROUND-1: integer-valued passes through; else floor(x + 0.5) */
    return "(CASE WHEN (" + ref + ") = trunc(" + ref + ") THEN (" + ref +
           ") ELSE floor((" + ref + ") + 0.5) END)";
}

bool stata_tc_ms_to_epoch_us(double stata_ms, long long *epoch_us) {
    /* the caller passes a whole-ms count; anything that is not a finite
     * integer-valued double inside int64 cannot be an instant on disk */
    if (!std::isfinite(stata_ms) || stata_ms != std::trunc(stata_ms)) return false;
    if (!(stata_ms > -9.2e18 && stata_ms < 9.2e18)) return false;
    const long long r = static_cast<long long>(stata_ms); /* exact: integer-valued */
    const long long ms = r - kEpochShiftMs;               /* exact int64 */
    constexpr long long kMaxMs = 9223372036854775LL;      /* INT64_MAX / 1000 */
    if (ms > kMaxMs || ms < -kMaxMs) return false;
    *epoch_us = ms * 1000LL;
    return true;
}

std::string timestamp_ns_floor_us_sql(const std::string &ref) {
    return "__parqit_timestamp_ns_us(" + ref + ")";
}

namespace {

constexpr long long kLLMax = 9223372036854775807LL; /* INT64_MAX */

long long sat_mul(long long a, long long b) {
    if (a <= 0 || b <= 0) return 0;
    return a > kLLMax / b ? kLLMax : a * b;
}

long long sat_add(long long a, long long b) {
    if (a < 0 || b < 0) return 0;
    return a > kLLMax - b ? kLLMax : a + b;
}

/* Bytes one row of this column occupies in a DuckDB vector. */
long long column_row_bytes(const ColumnPlan &p) {
    switch (p.transfer) {
    case Transfer::Int8: return 1;
    case Transfer::Int16: return 2;
    case Transfer::Int32:
    case Transfer::Float32:
    case Transfer::Date32: return 4;
    case Transfer::Int64:
    case Transfer::Float64:
    case Transfer::TimestampUs:
    case Transfer::TimeUs: return 8;
    case Transfer::Utf8: {
        /* duckdb_string_t is 16 bytes and inlines up to 12 payload bytes;
         * anything longer also lives in the chunk's string heap. */
        long long w = p.stata_type == StType::StrL ? kStataStrMax + 1 : p.str_bytes;
        return w > 12 ? 16 + w : 16;
    }
    }
    return 8; /* unreachable; the widest fixed width is the safe default */
}

} // namespace

long long estimate_transfer_bytes(const std::vector<ColumnPlan> &plans,
                                  long long nrows) {
    if (nrows <= 0) return 0;
    /* accumulate in eighths of a byte so the one validity bit per cell per
     * column stays exact */
    long long eighths = 0;
    for (const ColumnPlan &p : plans) {
        if (p.dropped) continue;
        eighths = sat_add(eighths, sat_add(sat_mul(column_row_bytes(p), 8), 1));
    }
    if (eighths == 0) return 0;
    const long long bytes = sat_mul(nrows, eighths) / 8;
    return sat_mul(bytes, 5) / 4; /* 25% chunk-capacity slack */
}

} // namespace parqit
