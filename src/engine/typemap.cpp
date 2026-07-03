#include "engine/typemap.hpp"

#include <cmath>

#include "engine/session.hpp" /* quote_ident */

namespace parqit {

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
    if (fmt.size() < 3 || fmt[0] != '%') return FmtClass::None;
    size_t i = 1;
    if (fmt[i] == '-') i++;
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

ColumnPlan plan_read_column(const std::string &source_name, duckdb_logical_type t) {
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
        p.cast_sql = "CAST(" + ref + " AS DOUBLE)";
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
        p.cast_sql = "CAST(" + ref + " AS DOUBLE)";
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
        break;
    case DUCKDB_TYPE_TIMESTAMP_S:
    case DUCKDB_TYPE_TIMESTAMP_MS:
        p.cast_sql = "CAST(" + ref + " AS TIMESTAMP)";
        p.transfer = Transfer::TimestampUs;
        p.stata_type = StType::Double;
        p.stata_format = "%tc";
        break;
    case DUCKDB_TYPE_TIMESTAMP_NS:
        p.cast_sql = "CAST(" + ref + " AS TIMESTAMP)";
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
    default: {
        /* charter §6.11: unrepresentable types are dropped with a message
         * (the caller errors out if every column would be dropped). A
         * NULL-typed column carries no type and no data, so it is dropped
         * loudly here too — never silently loaded as an all-missing byte
         * variable indistinguishable from a real one (brief §4, §6.11). */
        const char *what = "unsupported";
        switch (p.src_type) {
        case DUCKDB_TYPE_SQLNULL: what = "NULL"; break;
        case DUCKDB_TYPE_BLOB: what = "BLOB"; break;
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
        if (p.stata_type != StType::Double) p.stata_type = StType::Float;
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

} // namespace parqit
