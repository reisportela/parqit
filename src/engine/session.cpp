#include "engine/session.hpp"
#include "engine/test_fault.hpp"
#include "engine/scalar_registration.hpp"
#include "engine/datefn.hpp"
#include "engine/typemap.hpp"
#include "engine/stats_overflow.hpp"
#include "engine/statistics.hpp"
#include "engine/spss_table.hpp"
#include "engine/rdata_table.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <locale>
#include <limits>
#include <type_traits>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#ifdef PARQIT_DUCKDB_200
extern "C" int32_t duckdb_register_static_extensions(void);
#endif
#if defined(__linux__)
#include <sched.h>
#endif

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace parqit {

namespace {

static bool valid_row(uint64_t *validity, idx_t row) {
    return !validity || duckdb_validity_row_is_valid(validity, row);
}

static std::string format_fixed(double v, int decimals) {
    std::ostringstream os;
    os.imbue(std::locale::classic());
    os << std::fixed << std::setprecision(decimals) << v;
    std::string s = os.str();
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s.empty() ? "0" : s;
}

static std::string format_fixed_keep(double v, int decimals) {
    std::ostringstream os;
    os.imbue(std::locale::classic());
    os << std::fixed << std::setprecision(decimals) << v;
    return os.str();
}

static int ndigits_int(int v) {
    int d = 1;
    while (v >= 10) {
        v /= 10;
        d++;
    }
    return d;
}

static std::string format_sci(double a) {
    int exp = static_cast<int>(std::floor(std::log10(a)));
    /* STRING-DENORMAL-1 (audit 2026-08-22, A3-7): for a subnormal `a`
     * (5e-324 .. 2.2e-308) pow(10, exp) underflows to 0 (exp < -307) or to a
     * subnormal with few significant bits, so the mantissa came out as inf
     * ("infe-324") or imprecise. Scale in two steps through 1e300 so every
     * intermediate is a normal double: a * 1e300 / 10^(exp + 300). */
    double mant = (exp < -290) ? (a * 1e300) / std::pow(10.0, exp + 300)
                               : a / std::pow(10.0, exp);

    for (;;) {
        int exp_width = std::max(2, ndigits_int(std::abs(exp)));
        int decimals = std::max(0, 8 - (4 + exp_width));
        std::string ms = format_fixed_keep(mant, decimals);
        if (ms.size() >= 2 && ms[0] == '1' && ms[1] == '0') {
            exp++;
            mant = 1.0;
            continue;
        }

        std::ostringstream es;
        es.imbue(std::locale::classic());
        es << ms << 'e' << (exp >= 0 ? '+' : '-')
           << std::setw(exp_width) << std::setfill('0') << std::abs(exp);
        return es.str();
    }
}

static std::string stata_string_default(double v) {
    if (!std::isfinite(v)) return ".";
    if (v == 0.0) return "0";

    bool neg = std::signbit(v);
    double a = std::fabs(v);
    std::string payload;

    if (a >= 1e-5 && a < 1e7) {
        if (a >= 1.0) {
            int digits = static_cast<int>(std::floor(std::log10(a))) + 1;
            int decimals = digits >= 8 ? 0 : std::max(0, 8 - digits - 1);
            payload = format_fixed(a, decimals);
            double rounded = 0.0;
            if (atod(payload, &rounded) && rounded < 1e7 && payload.size() <= 8)
                return (neg ? "-" : "") + payload;
        } else {
            payload = format_fixed(a, 7);
            if (payload.rfind("0.", 0) == 0) payload.erase(0, 1);
            if (payload != "0" && payload.size() <= 8)
                return (neg ? "-" : "") + payload;
        }
    }

    return (neg ? "-" : "") + format_sci(a);
}

static std::string duck_string_to_std(duckdb_string_t *s) {
    const char *p = duckdb_string_t_data(s);
    return std::string(p ? p : "", duckdb_string_t_length(*s));
}

static bool utf8_cont(unsigned char c) { return (c & 0xC0) == 0x80; }

static void append_replacement(std::string *out) { out->append("\xEF\xBF\xBD", 3); }

static std::string utf8_lossy(const std::string &bytes) {
    std::string out;
    out.reserve(bytes.size());
    for (size_t i = 0; i < bytes.size();) {
        unsigned char c = static_cast<unsigned char>(bytes[i]);
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
            i++;
        } else if (c >= 0xC2 && c <= 0xDF && i + 1 < bytes.size() &&
                   utf8_cont(static_cast<unsigned char>(bytes[i + 1]))) {
            out.append(bytes, i, 2);
            i += 2;
        } else if (c >= 0xE0 && c <= 0xEF && i + 2 < bytes.size() &&
                   utf8_cont(static_cast<unsigned char>(bytes[i + 1])) &&
                   utf8_cont(static_cast<unsigned char>(bytes[i + 2])) &&
                   !(c == 0xE0 && static_cast<unsigned char>(bytes[i + 1]) < 0xA0) &&
                   !(c == 0xED && static_cast<unsigned char>(bytes[i + 1]) >= 0xA0)) {
            out.append(bytes, i, 3);
            i += 3;
        } else if (c >= 0xF0 && c <= 0xF4 && i + 3 < bytes.size() &&
                   utf8_cont(static_cast<unsigned char>(bytes[i + 1])) &&
                   utf8_cont(static_cast<unsigned char>(bytes[i + 2])) &&
                   utf8_cont(static_cast<unsigned char>(bytes[i + 3])) &&
                   !(c == 0xF0 && static_cast<unsigned char>(bytes[i + 1]) < 0x90) &&
                   !(c == 0xF4 && static_cast<unsigned char>(bytes[i + 1]) >= 0x90)) {
            out.append(bytes, i, 4);
            i += 4;
        } else {
            append_replacement(&out);
            i++;
        }
    }
    return out;
}

static void parqit_stata_string_fn(duckdb_function_info, duckdb_data_chunk input,
                                 duckdb_vector output) {
    idx_t n = duckdb_data_chunk_get_size(input);
    duckdb_vector in = duckdb_data_chunk_get_vector(input, 0);
    auto *vals = static_cast<double *>(duckdb_vector_get_data(in));
    uint64_t *validity = duckdb_vector_get_validity(in);
    duckdb_vector_ensure_validity_writable(output);
    uint64_t *out_validity = duckdb_vector_get_validity(output);

    for (idx_t r = 0; r < n; r++) {
        std::string out = valid_row(validity, r) ? stata_string_default(vals[r]) : ".";
        duckdb_validity_set_row_valid(out_validity, r);
        duckdb_vector_assign_string_element_len(output, r, out.c_str(), out.size());
    }
}

static void parqit_substr_bytes_fn(duckdb_function_info, duckdb_data_chunk input,
                                 duckdb_vector output) {
    idx_t n = duckdb_data_chunk_get_size(input);
    duckdb_vector sv = duckdb_data_chunk_get_vector(input, 0);
    duckdb_vector pv = duckdb_data_chunk_get_vector(input, 1);
    duckdb_vector lv = duckdb_data_chunk_get_vector(input, 2);
    auto *sdata = static_cast<duckdb_string_t *>(duckdb_vector_get_data(sv));
    auto *pdata = static_cast<double *>(duckdb_vector_get_data(pv));
    auto *ldata = static_cast<double *>(duckdb_vector_get_data(lv));
    uint64_t *svalid = duckdb_vector_get_validity(sv);
    uint64_t *pvalid = duckdb_vector_get_validity(pv);
    uint64_t *lvalid = duckdb_vector_get_validity(lv);

    duckdb_vector_ensure_validity_writable(output);
    uint64_t *out_validity = duckdb_vector_get_validity(output);

    for (idx_t r = 0; r < n; r++) {
        std::string src = valid_row(svalid, r) ? duck_string_to_std(&sdata[r]) : "";
        std::string out;
        if (valid_row(pvalid, r) && std::isfinite(pdata[r])) {
            const double position = std::trunc(pdata[r]);
            const long long len = static_cast<long long>(src.size());
            if (position != 0 && position >= -double(len) && position <= double(len)) {
                const long long p = static_cast<long long>(position);
                long long start = p > 0 ? p - 1 : len + p;
                if (start >= 0 && start < len) {
                    long long take = 0;
                    if (!valid_row(lvalid, r) || !std::isfinite(ldata[r])) {
                        take = len - start; /* substr(s, p, .) */
                    } else {
                        const double length = std::trunc(ldata[r]);
                        if (length > 0)
                            take = length >= double(len - start) ? len - start
                                : static_cast<long long>(length);
                    }
                    if (take > 0) {
                        long long avail = len - start;
                        if (take > avail) take = avail;
                        out.assign(src.data() + start, static_cast<size_t>(take));
                        out = utf8_lossy(out);
                    }
                }
            }
        }
        duckdb_validity_set_row_valid(out_validity, r);
        duckdb_vector_assign_string_element_len(output, r, out.c_str(), out.size());
    }
}

/* DATEFN-1: __parqit_date(s, mask, topyear) / __parqit_clock / __parqit_period
 * (s, mask, unit) — Stata's text-to-date readers; NULL (missing) whenever the
 * text does not read as the mask says, as native. topyear <= 0 means none. */
template <char Kind>
static void parqit_datefn_fn(duckdb_function_info, duckdb_data_chunk input, duckdb_vector output) {
    idx_t n = duckdb_data_chunk_get_size(input);
    duckdb_vector sv = duckdb_data_chunk_get_vector(input, 0);
    duckdb_vector mv = duckdb_data_chunk_get_vector(input, 1);
    duckdb_vector tv = duckdb_data_chunk_get_vector(input, 2);
    auto *sdata = static_cast<duckdb_string_t *>(duckdb_vector_get_data(sv));
    auto *mdata = static_cast<duckdb_string_t *>(duckdb_vector_get_data(mv));
    uint64_t *svalid = duckdb_vector_get_validity(sv);
    uint64_t *mvalid = duckdb_vector_get_validity(mv);
    uint64_t *tvalid = duckdb_vector_get_validity(tv);
    auto *out_vals = static_cast<int64_t *>(duckdb_vector_get_data(output));
    duckdb_vector_ensure_validity_writable(output);
    uint64_t *out_validity = duckdb_vector_get_validity(output);
    for (idx_t r = 0; r < n; r++) {
        bool ok = valid_row(svalid, r) && valid_row(mvalid, r);
        long long v = 0;
        if (ok) {
            const std::string s = duck_string_to_std(&sdata[r]);
            const std::string mask = duck_string_to_std(&mdata[r]);
            if (Kind == 'd' || Kind == 'c') {
                auto *tdata = static_cast<int64_t *>(duckdb_vector_get_data(tv));
                const long long top = valid_row(tvalid, r) ? static_cast<long long>(tdata[r]) : 0;
                ok = Kind == 'd' ? parqit::datefn::parse_date(s, mask, top, &v)
                                 : parqit::datefn::parse_clock(s, mask, top, &v);
            } else {
                auto *udata = static_cast<duckdb_string_t *>(duckdb_vector_get_data(tv));
                const std::string unit = valid_row(tvalid, r) ? duck_string_to_std(&udata[r]) : "";
                ok = unit.size() == 1 && parqit::datefn::parse_period(s, mask, unit[0], 0, &v);
            }
        }
        if (ok) {
            out_vals[r] = v;
            duckdb_validity_set_row_valid(out_validity, r);
        } else {
            duckdb_validity_set_row_invalid(out_validity, r);
        }
    }
}

/* parqit_finite(x): x when it is a value Stata can hold, else NULL. A
 * generated double can be NaN/±Inf (exp(800), 1e300*1e300) or reach the
 * missing-code region |x| >= 2^1023; native Stata reports every one of those
 * as missing. A real scalar function (not a CASE) so the operand is written
 * and evaluated ONCE — the CASE idiom repeats the operand's SQL text, which
 * grows exponentially when arithmetic guards nest (a+b+c+…). */
static void parqit_finite_fn(duckdb_function_info, duckdb_data_chunk input,
                             duckdb_vector output) {
    idx_t n = duckdb_data_chunk_get_size(input);
    duckdb_vector in = duckdb_data_chunk_get_vector(input, 0);
    auto *vals = static_cast<double *>(duckdb_vector_get_data(in));
    uint64_t *validity = duckdb_vector_get_validity(in);
    auto *out_vals = static_cast<double *>(duckdb_vector_get_data(output));
    duckdb_vector_ensure_validity_writable(output);
    uint64_t *out_validity = duckdb_vector_get_validity(output);

    /* exactly 2^1023 (Stata's `.`). The decimal literal 8.988465674311579e307
     * this used to be parses to 2^1023 - 2^970 = Stata's maxdouble(), so the
     * guard nulled that one legitimate finite value (MAXDOUBLE-1). */
    static const double kMissThreshold = 0x1p1023;
    for (idx_t r = 0; r < n; r++) {
        if (valid_row(validity, r) && std::isfinite(vals[r]) &&
            std::fabs(vals[r]) < kMissThreshold) {
            out_vals[r] = vals[r];
            duckdb_validity_set_row_valid(out_validity, r);
        } else {
            duckdb_validity_set_row_invalid(out_validity, r);
        }
    }
}

/* The C API flattens scalar inputs before calling us (DuckDB 1.5.3,
 * scalar_function-c.cpp:CAPIScalarFunction), including constants/selections.
 * Convert once per cell: nested SQL guards repeat temporal operands and work.
 * All quotients plus shifts fit int64; nanoseconds -> ms also stays below 2^53. */
template <class Input, class Output, int64_t Divisor, int64_t Shift,
          bool CheckBinary64 = false>
static void temporal_boundary_fn(duckdb_function_info info, duckdb_data_chunk input,
                                  duckdb_vector output) {
    const idx_t n = duckdb_data_chunk_get_size(input);
    const duckdb_vector in = duckdb_data_chunk_get_vector(input, 0);
    const auto *values = static_cast<const Input *>(duckdb_vector_get_data(in));
    const uint64_t *validity = duckdb_vector_get_validity(in);
    auto *out = static_cast<Output *>(duckdb_vector_get_data(output));
    duckdb_vector_ensure_validity_writable(output);
    uint64_t *out_validity = duckdb_vector_get_validity(output);
    constexpr int64_t infinity = std::is_same_v<Input, duckdb_date>
        ? std::numeric_limits<int32_t>::max() : std::numeric_limits<int64_t>::max();
    constexpr const char *infinite_error = std::is_same_v<Input, duckdb_date>
        ? "infinite DATE cannot be represented in Stata"
        : std::is_same_v<Input, duckdb_timestamp>
            ? "infinite TIMESTAMP cannot be represented in Stata"
            : "infinite TIMESTAMP_NS cannot be represented in Stata";
    for (idx_t r = 0; r < n; ++r) {
        const uint64_t bit = uint64_t(1) << (r & 63);
        if (validity && !(validity[r >> 6] & bit)) {
            out_validity[r >> 6] &= ~bit;
            continue;
        }
        int64_t raw;
        if constexpr (std::is_same_v<Input, duckdb_date>) raw = values[r].days;
        else if constexpr (std::is_same_v<Input, duckdb_timestamp>) raw = values[r].micros;
        else raw = values[r].nanos;
        if (raw == infinity || raw == -infinity) {
            duckdb_scalar_function_set_error(info, infinite_error);
            return;
        }
        const int64_t value = raw / Divisor - (raw % Divisor < 0) + Shift;
        if constexpr (CheckBinary64) {
            if (!integer_exact_in_binary64(value)) {
                duckdb_scalar_function_set_error(info,
                    "timestamp millisecond is not exactly representable in Stata binary64");
                return;
            }
        }
        if constexpr (std::is_same_v<Output, duckdb_timestamp>) out[r].micros = value;
        else out[r] = value;
        out_validity[r >> 6] |= bit;
    }
}

/* Use DuckDB's selection-aware executor instead of the C API's unconditional
 * input.Flatten(). Keep fallible execution: dictionary entries excluded by a
 * selection must not be evaluated, since one of them could be infinite. */
template <class Input, class Output, int64_t Divisor, int64_t Shift,
          bool CheckBinary64 = false>
static void native_temporal_boundary_fn(duckdb::DataChunk &input,
                                        duckdb::ExpressionState &,
                                        duckdb::Vector &output) {
    if (input.size() == 0) return;
    auto convert = [](Input value) -> Output {
        constexpr int64_t infinity = std::is_same_v<Input, duckdb::date_t>
            ? std::numeric_limits<int32_t>::max() : std::numeric_limits<int64_t>::max();
        constexpr const char *infinite_error = std::is_same_v<Input, duckdb::date_t>
            ? "infinite DATE cannot be represented in Stata"
            : std::is_same_v<Input, duckdb::timestamp_t>
                ? "infinite TIMESTAMP cannot be represented in Stata"
                : "infinite TIMESTAMP_NS cannot be represented in Stata";
        int64_t raw;
        if constexpr (std::is_same_v<Input, duckdb::date_t>) raw = value.days;
        else raw = value.value;
        if (raw == infinity || raw == -infinity)
            throw duckdb::InvalidInputException(infinite_error);
        const int64_t result = raw / Divisor - (raw % Divisor < 0) + Shift;
        if constexpr (CheckBinary64) {
            if (!integer_exact_in_binary64(result))
                throw duckdb::InvalidInputException(
                    "timestamp millisecond is not exactly representable in Stata binary64");
        }
        if constexpr (std::is_same_v<Output, duckdb::timestamp_t>)
            return duckdb::timestamp_t(result);
        else return result;
    };
    duckdb::UnaryExecutor::Execute<Input, Output>(
        input.data[0], output, input.size(), convert,
        duckdb::FunctionErrors::CAN_THROW_RUNTIME_ERROR);
}

/* Numeric clock constructors: one vector pass validates and converts the
 * components. Every accepted result is an exact integer below 2^53. Keeping
 * the guards here avoids repeated SQL projections for each clock component. */
template <int N>
static void parqit_clock_parts_fn(duckdb_function_info, duckdb_data_chunk input,
                                  duckdb_vector output) {
    const double *values[N];
    uint64_t *validity[N];
    for (int j = 0; j < N; j++) {
        duckdb_vector v = duckdb_data_chunk_get_vector(input, j);
        values[j] = static_cast<const double *>(duckdb_vector_get_data(v));
        validity[j] = duckdb_vector_get_validity(v);
    }
    auto *result = static_cast<double *>(duckdb_vector_get_data(output));
    duckdb_vector_ensure_validity_writable(output);
    uint64_t *out_validity = duckdb_vector_get_validity(output);
    constexpr int first = N == 3 ? 0 : N == 4 ? 1 : 3;
    const idx_t count = duckdb_data_chunk_get_size(input);
    for (idx_t r = 0; r < count; r++) {
        bool valid = true;
        for (int j = 0; j < N; j++) valid = valid && valid_row(validity[j], r);
        if (!valid) {
            duckdb_validity_set_row_invalid(out_validity, r);
            continue;
        }
        const double h = values[first][r], m = values[first + 1][r], s = values[first + 2][r];
        valid = valid && h >= 0 && h < 24 && m >= 0 && m < 60 && s >= 0 && s < 60;
        long long day = 0;
        if constexpr (N == 4) {
            const double d = values[0][r];
            valid = valid && d >= -679350 && d < 2936550;
            if (valid) day = static_cast<long long>(std::floor(d));
        } else if constexpr (N == 6) {
            const double mo = values[0][r], d = values[1][r], y = values[2][r];
            valid = valid && y >= 100 && y < 10000 && mo >= 1 && mo < 13 && d >= 1 && d < 32;
            if (valid) {
                const int yy = static_cast<int>(y), mm = static_cast<int>(mo), dd = static_cast<int>(d);
                valid = dd <= datefn::days_in_month(yy, mm);
                if (valid) day = datefn::days_from_civil(yy, mm, dd);
            }
        }
        if (!valid) {
            duckdb_validity_set_row_invalid(out_validity, r);
            continue;
        }
        const int millis = static_cast<int>(h) * 3600000 + static_cast<int>(m) * 60000 +
                           static_cast<int>(s * 1000.0);
        result[r] = static_cast<double>(day * 86400000LL + millis);
        duckdb_validity_set_row_valid(out_validity, r);
    }
}

static bool register_scalar(duckdb_connection con, const char *name,
                            const std::vector<duckdb_type> &params,
                            duckdb_type ret_type, duckdb_scalar_function_t fn,
                            std::string *err, bool fallible = false,
                            duckdb::scalar_function_t native_fn = nullptr) {
    duckdb_scalar_function f = duckdb_create_scalar_function();
    if (!f) {
        if (err) *err = std::string("could not create internal function ") + name;
        return false;
    }
    duckdb_scalar_function_set_name(f, name);
    for (duckdb_type t : params) {
        duckdb_logical_type lt = duckdb_create_logical_type(t);
        duckdb_scalar_function_add_parameter(f, lt);
        duckdb_destroy_logical_type(&lt);
    }
    duckdb_logical_type ret = duckdb_create_logical_type(ret_type);
    duckdb_scalar_function_set_return_type(f, ret);
    duckdb_destroy_logical_type(&ret);
    duckdb_scalar_function_set_special_handling(f);
    duckdb_scalar_function_set_function(f, fn);
    /* DuckDB 1.5.3's C API handle is a ScalarFunction pointer (verified in
     * scalar_function-c.cpp), but the C API has no error-mode setter. Keep
     * optimizer error ordering consistent with the former SQL error() guards. */
    auto &scalar = *reinterpret_cast<duckdb::ScalarFunction *>(f);
    if (fallible) scalar.SetFallible();
    if (native_fn) {
        /* Keep the normal C API declaration/registration and its required
         * callback metadata, but invoke the native executor directly. Its
         * callback needs neither CScalarFunctionBindData nor local state. */
        scalar.SetFunctionCallback(native_fn);
        scalar.SetBindCallback(nullptr);
        scalar.SetInitStateCallback(nullptr);
    }
    duckdb_state st = register_known_scalar(con, f, fallible ? ScalarErrorPolicy::ValueDependent
                                                          : ScalarErrorPolicy::Total);
    duckdb_destroy_scalar_function(&f);
    if (st != DuckDBSuccess) {
        if (err) *err = std::string("could not register internal function ") + name;
        return false;
    }
    return true;
}

static bool register_internal_functions(duckdb_connection con, std::string *err) {
    return register_scalar(con, "parqit_stata_string", {DUCKDB_TYPE_DOUBLE},
                           DUCKDB_TYPE_VARCHAR, parqit_stata_string_fn, err) &&
           register_scalar(con, "parqit_substr_bytes",
                           {DUCKDB_TYPE_VARCHAR, DUCKDB_TYPE_DOUBLE,
                            DUCKDB_TYPE_DOUBLE},
                           DUCKDB_TYPE_VARCHAR, parqit_substr_bytes_fn, err) &&
           register_scalar(con, "parqit_finite", {DUCKDB_TYPE_DOUBLE},
                           DUCKDB_TYPE_DOUBLE, parqit_finite_fn, err) &&
           register_scalar(con, "__parqit_hms", std::vector<duckdb_type>(3, DUCKDB_TYPE_DOUBLE),
                           DUCKDB_TYPE_DOUBLE, parqit_clock_parts_fn<3>, err) &&
           register_scalar(con, "__parqit_dhms", std::vector<duckdb_type>(4, DUCKDB_TYPE_DOUBLE),
                           DUCKDB_TYPE_DOUBLE, parqit_clock_parts_fn<4>, err) &&
           register_scalar(con, "__parqit_mdyhms", std::vector<duckdb_type>(6, DUCKDB_TYPE_DOUBLE),
                           DUCKDB_TYPE_DOUBLE, parqit_clock_parts_fn<6>, err) &&
           register_scalar(con, "__parqit_date", {DUCKDB_TYPE_VARCHAR, DUCKDB_TYPE_VARCHAR, DUCKDB_TYPE_BIGINT},
                           DUCKDB_TYPE_BIGINT, parqit_datefn_fn<'d'>, err) &&
           register_scalar(con, "__parqit_clock", {DUCKDB_TYPE_VARCHAR, DUCKDB_TYPE_VARCHAR, DUCKDB_TYPE_BIGINT},
                           DUCKDB_TYPE_BIGINT, parqit_datefn_fn<'c'>, err) &&
           register_scalar(con, "__parqit_period", {DUCKDB_TYPE_VARCHAR, DUCKDB_TYPE_VARCHAR, DUCKDB_TYPE_VARCHAR},
                           DUCKDB_TYPE_BIGINT, parqit_datefn_fn<'p'>, err) &&
           register_scalar(con, "__parqit_date_days", {DUCKDB_TYPE_DATE},
                           DUCKDB_TYPE_BIGINT,
                           temporal_boundary_fn<duckdb_date, int64_t, 1, kEpochShiftDays>, err, true,
                           native_temporal_boundary_fn<duckdb::date_t, int64_t, 1, kEpochShiftDays>) &&
           register_scalar(con, "__parqit_timestamp_ms", {DUCKDB_TYPE_TIMESTAMP},
                           DUCKDB_TYPE_BIGINT,
                           temporal_boundary_fn<duckdb_timestamp, int64_t, 1000, kEpochShiftMs, true>, err, true,
                           native_temporal_boundary_fn<duckdb::timestamp_t, int64_t, 1000, kEpochShiftMs, true>) &&
           register_scalar(con, "__parqit_timestamp_ns_ms", {DUCKDB_TYPE_TIMESTAMP_NS},
                           DUCKDB_TYPE_BIGINT,
                           temporal_boundary_fn<duckdb_timestamp_ns, int64_t, 1000000, kEpochShiftMs>, err, true,
                           native_temporal_boundary_fn<duckdb::timestamp_ns_t, int64_t, 1000000, kEpochShiftMs>) &&
           register_scalar(con, "__parqit_timestamp_ns_us", {DUCKDB_TYPE_TIMESTAMP_NS},
                           DUCKDB_TYPE_TIMESTAMP,
                           temporal_boundary_fn<duckdb_timestamp_ns, duckdb_timestamp, 1000, 0>, err, true,
                           native_temporal_boundary_fn<duckdb::timestamp_ns_t, duckdb::timestamp_t, 1000, 0>) &&
           statistics::register_functions(con, err) &&
           spss::register_table_function(con, err) && /* SPSS-READ-1 */
           rdata::register_table_function(con, err);  /* R-READ-1 */
}

} // namespace

Session &Session::instance() {
    static Session s;
    return s;
}

Session::~Session() { close(); }

void Session::close() {
    stats_fallback_ready_ = false;
    if (con_) {
        duckdb_disconnect(&con_);
        con_ = nullptr;
    }
    if (db_) {
        duckdb_close(&db_);
        db_ = nullptr;
    }
}

int available_cpus() {
    /* CPUS-1 (see session.hpp). sched_getaffinity fails with EINVAL on a
     * machine with more CPUs than cpu_set_t holds (1024); the fallback covers
     * it. hardware_concurrency() may return 0 when detection fails: 1 then. */
    int n = 0;
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof set, &set) == 0) n = CPU_COUNT(&set);
#endif
    if (n <= 0) n = static_cast<int>(std::thread::hardware_concurrency());
    return n < 1 ? 1 : n;
}

long long Session::threads() const {
    return threads_ > 0 ? threads_ : available_cpus();
}

bool Session::ensure_open() {
    if (db_ && con_) return true;
    close();

#ifdef PARQIT_DUCKDB_200
    if (duckdb_register_static_extensions() != 0) {
        last_error_ = "could not register the statically linked DuckDB extensions";
        return false;
    }
#endif

    duckdb_config config = nullptr;
    if (duckdb_create_config(&config) != DuckDBSuccess) {
        last_error_ = "could not create DuckDB config";
        return false;
    }
#ifdef PARQIT_DUCKDB_200
    // 2.0 removed the compile-time switches. Keep the embedded engine offline;
    // raw SQL uses the selected engine's native dialect. Stata expressions are
    // translated separately and retain their own missing/error semantics.
    for (const auto &setting : {
            std::pair<const char *, const char *>{"autoload_known_extensions", "false"},
            {"autoinstall_known_extensions", "false"}}) {
        if (duckdb_set_config(config, setting.first, setting.second) != DuckDBSuccess) {
            last_error_ = std::string("could not configure DuckDB setting ") + setting.first;
            duckdb_destroy_config(&config);
            return false;
        }
    }
#endif
    /* CPUS-1: the engine's threads are bounded by the CPUs available to this
     * process unless the user chose a count (`parqit set threads`, itself
     * clamped to the same number). DuckDB's own default is
     * hardware_concurrency(), which ignores the affinity mask. */
    duckdb_set_config(config, "threads",
                      std::to_string(threads_ > 0 ? threads_ : available_cpus()).c_str());
#ifdef PARQIT_DUCKDB_200
    // 2.0 otherwise starts up to 256 I/O workers independently of the query
    // thread setting and the process affinity mask. Keep both pools bounded.
    if (duckdb_set_config(config, "async_threads",
            std::to_string(threads_ > 0 ? threads_ : available_cpus()).c_str()) != DuckDBSuccess) {
        last_error_ = "could not configure DuckDB asynchronous I/O threads";
        duckdb_destroy_config(&config);
        return false;
    }
#endif
    if (!memory_limit_.empty())
        duckdb_set_config(config, "memory_limit", memory_limit_.c_str());
    /* temp_directory is what lets the in-memory instance spill to disk:
     * always set one so pipelines stay out-of-core by default. */
    const std::string &tdir = temp_directory_.empty() ? default_temp_dir_ : temp_directory_;
    if (!tdir.empty())
        duckdb_set_config(config, "temp_directory", tdir.c_str());

    char *open_err = nullptr;
    if (duckdb_open_ext(nullptr, &db_, config, &open_err) != DuckDBSuccess) {
        last_error_ = open_err ? open_err : "could not open DuckDB database";
        if (open_err) duckdb_free(open_err);
        duckdb_destroy_config(&config);
        db_ = nullptr;
        return false;
    }
    duckdb_destroy_config(&config);

    if (duckdb_connect(db_, &con_) != DuckDBSuccess) {
        last_error_ = "could not connect to DuckDB database";
        duckdb_close(&db_);
        db_ = nullptr;
        con_ = nullptr;
        return false;
    }
    if (!register_internal_functions(con_, &last_error_)) {
        close();
        return false;
    }
    if (!replay_native_settings(con_, false, &last_error_)) {
        close();
        return false;
    }
    return true;
}

static bool apply_set(Session &s, const std::string &sql, std::string *err) {
    if (!s.con()) return true; /* not open yet: applied via config on open */
    return s.exec(sql, err);
}

bool Session::set_threads(long long n, std::string *err) {
    if (n <= 0) {
        if (err) *err = "threads must be a positive integer";
        return false;
    }
    if (!apply_set(*this, "SET threads = " + std::to_string(n), err)) return false;
#ifdef PARQIT_DUCKDB_200
    if (!native_overrides_.count("async_threads") &&
        !apply_set(*this, "SET async_threads = " + std::to_string(n), err)) {
        std::string ignored;
        apply_set(*this, "SET threads = " +
            std::to_string(threads_ > 0 ? threads_ : available_cpus()), &ignored);
        return false;
    }
#endif
    threads_ = n;
    release_native_setting("threads");
    return true;
}

bool Session::set_memory_limit(const std::string &limit, std::string *err) {
    /* cache only after a successful SET — a rejected value must not be kept and
     * silently re-applied on the next reopen */
    std::string prev = memory_limit_;
    memory_limit_ = limit;
    if (!apply_set(*this, "SET memory_limit = " + quote_literal(limit), err)) {
        memory_limit_ = prev;
        return false;
    }
    release_native_setting("max_memory");
    return true;
}

bool Session::set_temp_directory(const std::string &dir, std::string *err) {
    std::string prev = temp_directory_;
    temp_directory_ = dir;
    if (!apply_set(*this, "SET temp_directory = " + quote_literal(dir), err)) {
        temp_directory_ = prev;
        return false;
    }
    release_native_setting("temp_directory");
    return true;
}

/* PERF-STREAM-1. `streaming_buffer_size` is a LOCAL (connection) setting whose
 * embedded default is 1,000,000 bytes. In 1.5.6 its budget is split
 * 60% read queue / 40% in-progress batches
 * (main/buffered_data/batched_buffered_data.cpp:19-20). Once a producer's share
 * is full its sink parks, and the consumer only unparks sinks when the read
 * queue has gone completely EMPTY (BatchedBufferedData::ExecuteTaskInternal,
 * batched_buffered_data.cpp:127-140 -> UnblockSinks, 42-60): every time the cap
 * is reached the parallel scan stops and restarts. At the engine default a wide
 * multi-row-group read spends most of its time in that cycle, which is why the
 * fetch sizes this from the result it is about to read instead of using a fixed
 * number. Note DuckDB may still let the read queue overshoot (its own FIXME in
 * execution/operator/helper/physical_buffered_batch_collector.cpp:66-68), so
 * this is a soft cap; and it is a cap, not a reservation — only chunks the
 * engine actually produced occupy memory. DuckDB 2.0 aliases this setting to
 * max_streaming_buffer_size and uses a total byte budget with parked units;
 * it no longer uses that 60/40 split. The pinned compatibility correction
 * retains the existing default/RESET contract for parqit. */
bool Session::set_streaming_buffer_bytes(long long bytes, std::string *err) {
    if (!ensure_open()) {
        if (err) *err = last_error_;
        return false;
    }
    if (bytes < 1) bytes = 1;
    /* 'B' is bytes: StringUtil::TryParseFormattedBytes accepts byte/bytes/b
     * with multiplier 1 (duckdb src/common/string_util.cpp:316-317), and
     * StreamingBufferSizeSetting::SetLocal parses the value with
     * DBConfig::ParseMemoryLimit (src/main/settings/custom_settings.cpp:1600-1603). */
    return exec("SET streaming_buffer_size = '" + std::to_string(bytes) + "B'", err);
}

bool Session::set_optimizers(bool window_self_join, bool filter_pushdown, std::string *err) {
    if (!native_optimizer_dirty_ && window_self_join == window_self_join_ &&
        filter_pushdown == filter_pushdown_) return true;
    std::string off;
    if (native_disabled_optimizers_.empty()) {
        if (!window_self_join) off += "window_self_join";
        if (!filter_pushdown) off += std::string(off.empty() ? "" : ",") + "filter_pushdown";
    } else {
        off = native_disabled_optimizers_;
        auto disable = [&](const std::string &name) {
            if (("," + off + ",").find("," + name + ",") == std::string::npos)
                off += (off.empty() ? "" : ",") + name;
        };
        if (!window_self_join) disable("window_self_join");
        if (!filter_pushdown) disable("filter_pushdown");
    }
    if (!exec(off.empty() ? "RESET disabled_optimizers"
                          : "SET disabled_optimizers = " + quote_literal(off), err))
        return false;
    window_self_join_ = window_self_join;
    filter_pushdown_ = filter_pushdown;
    native_optimizer_dirty_ = false;
    return true;
}

bool Session::exec(const std::string &sql, std::string *err) {
    duckdb_result res;
    if (!query(sql, &res, err)) return false;
    duckdb_destroy_result(&res);
    return true;
}

bool Session::query(const std::string &sql, duckdb_result *out, std::string *err) {
    if (!ensure_open()) {
        if (err) *err = last_error_;
        return false;
    }
    if (duckdb_query(con_, sql.c_str(), out) != DuckDBSuccess) {
        const char *msg = duckdb_result_error(out);
        last_error_ = msg ? msg : "unknown DuckDB error";
        if (err) *err = last_error_;
        duckdb_destroy_result(out);
        return false;
    }
    return true;
}

bool Session::query_streaming(const std::string &sql, duckdb_result *out,
                              std::string *err) {
    if (!ensure_open()) {
        if (err) *err = last_error_;
        return false;
    }
    /* duckdb_prepare: duckdb.h:1899. On a throwing failure the out handle is
     * left untouched and the C wrapper is deleted (capi/prepared-c.cpp:84-103),
     * hence the nullptr init; duckdb_prepare_error (duckdb.h:1918,
     * prepared-c.cpp:105-117) and duckdb_destroy_prepare (duckdb.h:1907,
     * prepared-c.cpp:480-482) both accept a null handle. */
    duckdb_prepared_statement stmt = nullptr;
    if (duckdb_prepare(con_, sql.c_str(), &stmt) != DuckDBSuccess) {
        const char *msg = duckdb_prepare_error(stmt);
        last_error_ = (msg && *msg) ? msg : "unknown DuckDB error";
        if (err) *err = last_error_;
        duckdb_destroy_prepare(&stmt);
        return false;
    }
    /* duckdb_pending_prepared_streaming: duckdb.h:2315 (carries a deprecation
     * notice, so tests/unit/test_streaming.cpp pins it — a DuckDB bump that
     * drops it fails there, never at a user). It is the only entry point that
     * asks for a streaming result; capi/pending-c.cpp:41-44.
     * duckdb_pending_error: duckdb.h:2335; duckdb_destroy_pending: duckdb.h:2325
     * (required even when this returns DuckDBError). */
    duckdb_pending_result pending = nullptr;
    if (duckdb_pending_prepared_streaming(stmt, &pending) != DuckDBSuccess) {
        const char *msg = duckdb_pending_error(pending);
        last_error_ = (msg && *msg) ? msg : "unknown DuckDB error";
        if (err) *err = last_error_;
        duckdb_destroy_pending(&pending);
        duckdb_destroy_prepare(&stmt);
        return false;
    }
    /* In 1.5.6 execute_pending runs until the streaming result is available.
     * In 2.0 it can hand out the stream before producing data: execution and
     * its errors then reach the caller through fetch. The prepared EXECUTE
     * collector correction preserves the underlying plan's parallelism/order. */
    if (duckdb_execute_pending(pending, out) != DuckDBSuccess) {
        const char *msg = duckdb_result_error(out); /* duckdb.h:1368 */
        last_error_ = (msg && *msg) ? msg : "unknown DuckDB error";
        if (err) *err = last_error_;
        duckdb_destroy_result(out);
        duckdb_destroy_pending(&pending);
        duckdb_destroy_prepare(&stmt);
        return false;
    }
    /* Both handles die here, while the stream stays live. In 1.5.6 execution
     * resets the pending wrapper and the prepared destructor is empty. In
     * 2.0 execution moves the query into the stream; destroying the prepared
     * handle removes its catalogue entry, while PhysicalExecute retains the
     * shared plan until execution ends. The pending wrapper owns no query
     * after that move. The lifetime/abandonment tests cover both contracts. */
    duckdb_destroy_pending(&pending);
    duckdb_destroy_prepare(&stmt);
    return true;
}

bool Session::query_scalar(const std::string &sql, std::string *value, std::string *err) {
    duckdb_result res;
    if (!query(sql, &res, err)) return false;
    const ResultGuard guard{&res};
    const CText v = result_text(&res, 0, 0);
    *value = v ? v.get() : "";
    return true;
}

std::vector<Session::TextResult> Session::query_text_many(const std::vector<std::string> &sqls,
                                                          int threads) {
    std::vector<TextResult> out(sqls.size());
    if (sqls.empty()) return out;
    if (!ensure_open()) {
        for (TextResult &r : out) r.err = last_error_;
        return out;
    }
    /* one query on one connection; nothing escapes a worker thread */
    auto run = [&](duckdb_connection con, size_t k) {
        TextResult &r = out[k];
        duckdb_result res;
        try {
            test_fault("text_many", static_cast<long long>(k));
            const bool ok = duckdb_query(con, sqls[k].c_str(), &res) == DuckDBSuccess;
            const ResultGuard guard{&res}; /* Codex final audit, finding 3 */
            if (!ok) {
                const char *msg = duckdb_result_error(&res);
                r.err = msg ? msg : "unknown DuckDB error";
                return;
            }
            const idx_t n = duckdb_row_count(&res), m = duckdb_column_count(&res);
            r.rows.resize(n);
            for (idx_t i = 0; i < n; i++) {
                r.rows[i].resize(m);
                for (idx_t j = 0; j < m; j++) {
                    /* a text that cannot be copied fails the query, never reads
                     * as NULL (Codex audit, round 2, findings 2 and 6) */
                    std::unique_ptr<char, void (*)(void *)> v(result_varchar(&res, j, i), duckdb_free);
                    if (v) r.rows[i][j] = std::string(v.get());
                }
            }
        } catch (...) {
            /* nothing that allocates here (Codex audit, finding 2): worded
             * after the join, on the calling thread */
            r.rows.clear();
            r.failure = std::current_exception();
        }
    };
    std::atomic<size_t> next{0};
    auto loop = [&](duckdb_connection con) {
        for (size_t k; (k = next.fetch_add(1)) < sqls.size();) run(con, k);
    };
    const size_t workers = std::min(sqls.size(), threads > 1 ? static_cast<size_t>(threads) : size_t(1));
    std::vector<duckdb_connection> cons;
    std::vector<std::thread> pool;
    {
        struct Release {
            std::vector<std::thread> &pool;
            std::vector<duckdb_connection> &cons;
            ~Release() {
                for (std::thread &t : pool)
                    if (t.joinable()) t.join();
                for (duckdb_connection &c : cons) duckdb_disconnect(&c);
            }
        } release{pool, cons};
        try {
            cons.reserve(workers);
            pool.reserve(workers);
            for (size_t w = 1; w < workers; w++) {
                duckdb_connection c = nullptr;
                if (duckdb_connect(db_, &c) != DuckDBSuccess) break;
                if (!native_local_keys_.empty()) {
                    std::string setting_error;
                    if (!replay_native_settings(c, true, &setting_error)) {
                        duckdb_disconnect(&c);
                        for (auto &result : out) result.err = setting_error;
                        return out;
                    }
                }
                cons.push_back(c);
            }
            for (duckdb_connection c : cons) pool.emplace_back(loop, c);
        } catch (...) {
            /* fewer workers: the queries are shared among those that started */
        }
        loop(con_);
    }
    for (size_t k = 0; k < out.size(); k++) {
        TextResult &r = out[k];
        if (!r.failure) continue;
        try {
            std::rethrow_exception(r.failure);
        } catch (const std::exception &e) {
            test_fault("text_many_word", static_cast<long long>(k)); /* the wording failing too */
            r.err = e.what();
        } catch (...) {
            r.err = "unknown error reading a query result";
        }
        r.failure = nullptr;
    }
    return out;
}

bool Session::prepare_stats_fallback(std::string *err) {
    if (!ensure_open()) {
        if (err) *err = last_error_;
        return false;
    }
    if (!stats_fallback_ready_)
        stats_fallback_ready_ = stats_overflow::register_functions(con_, err);
    return stats_fallback_ready_;
}

std::string quote_literal(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('\'');
    for (char c : s) {
        if (c == '\'') out.push_back('\'');
        out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

std::string quote_ident(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        if (c == '"') out.push_back('"');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

bool c_api_text_type(duckdb_type t) {
    switch (t) {
    case DUCKDB_TYPE_BOOLEAN: case DUCKDB_TYPE_TINYINT: case DUCKDB_TYPE_SMALLINT:
    case DUCKDB_TYPE_INTEGER: case DUCKDB_TYPE_BIGINT: case DUCKDB_TYPE_UTINYINT:
    case DUCKDB_TYPE_USMALLINT: case DUCKDB_TYPE_UINTEGER: case DUCKDB_TYPE_UBIGINT:
    case DUCKDB_TYPE_FLOAT: case DUCKDB_TYPE_DOUBLE: case DUCKDB_TYPE_DATE:
    case DUCKDB_TYPE_TIME: case DUCKDB_TYPE_TIME_NS: case DUCKDB_TYPE_TIMESTAMP:
    case DUCKDB_TYPE_HUGEINT: case DUCKDB_TYPE_UHUGEINT: case DUCKDB_TYPE_DECIMAL:
    case DUCKDB_TYPE_INTERVAL: case DUCKDB_TYPE_VARCHAR: case DUCKDB_TYPE_BLOB:
        return true;
    default:
        return false;
    }
}

char *result_varchar(duckdb_result *res, idx_t col, idx_t row) {
    char *v = duckdb_value_varchar(res, col, row);
    if (test_text_copy_fails()) {
        duckdb_free(v);
        v = nullptr;
    }
    if (!v && !duckdb_value_is_null(res, col, row) && c_api_text_type(duckdb_column_type(res, col)))
        throw std::bad_alloc();
    return v;
}

std::string text_cell(duckdb_result &res, idx_t col, idx_t row,
                      const std::string &missing) {
    if (duckdb_value_is_null(&res, col, row)) return missing;
    /* Codex final audit, finding 3: own every C buffer before allocating. */
    if (duckdb_column_type(&res, col) == DUCKDB_TYPE_BLOB) {
        duckdb_blob value = duckdb_value_blob(&res, col, row);
        if (test_text_copy_fails()) {
            duckdb_free(value.data);
            value.data = nullptr;
        }
        const CText data(static_cast<char *>(value.data), duckdb_free);
        if (!data) {
            const CText text = result_text(&res, col, row);
            if (!text || *text != '\0') throw std::bad_alloc();
            return "";
        }
        return std::string(data.get(), value.size);
    }
    duckdb_string value = duckdb_value_string(&res, col, row);
    if (test_text_copy_fails()) {
        duckdb_free(value.data);
        value.data = nullptr;
    }
    const CText data(value.data, duckdb_free);
    if (!data && c_api_text_type(duckdb_column_type(&res, col))) throw std::bad_alloc();
    return data ? std::string(data.get(), value.size) : missing;
}

/* Locale-independent, full-precision (17-significant-digit, round-trippable)
 * formatting/parsing of a double. Uses the classic ("C") locale so the decimal
 * point is always '.' regardless of the process locale — std::to_chars/
 * from_chars would be ideal but their floating-point overloads are unavailable
 * before macOS 13.3, so a classic-imbued stream is the portable equivalent. */
std::string dtoa(double v) {
    std::ostringstream os;
    os.imbue(std::locale::classic());
    os << std::setprecision(17) << v;
    return os.str();
}

bool atod(const std::string &s, double *out) {
    std::istringstream is(s);
    is.imbue(std::locale::classic());
    is >> *out;             /* skips leading blanks, parses one number */
    if (is.fail()) return false;
    char extra;
    while (is >> extra)     /* anything left other than blanks is invalid */
        if (extra != ' ' && extra != '\t') return false;
    return true;
}

std::string spill_suffix() {
#ifdef _WIN32
    return "/_parqit_spill_" + std::to_string(_getpid());
#else
    return "/_parqit_spill_" + std::to_string(getpid());
#endif
}

} // namespace parqit
