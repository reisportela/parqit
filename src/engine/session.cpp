#include "engine/session.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

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
    double mant = a / std::pow(10.0, exp);

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
            long long p = static_cast<long long>(pdata[r]); /* Stata truncates */
            if (p != 0) {
                long long len = static_cast<long long>(src.size());
                long long start = p > 0 ? p - 1 : len + p;
                if (start >= 0 && start < len) {
                    long long take = 0;
                    if (!valid_row(lvalid, r) || !std::isfinite(ldata[r])) {
                        take = len - start; /* substr(s, p, .) */
                    } else {
                        take = static_cast<long long>(ldata[r]);
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

    static const double kMissThreshold = 8.988465674311579e307; /* 2^1023 */
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

static bool register_scalar(duckdb_connection con, const char *name,
                            const std::vector<duckdb_type> &params,
                            duckdb_type ret_type, duckdb_scalar_function_t fn,
                            std::string *err) {
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
    duckdb_state st = duckdb_register_scalar_function(con, f);
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
                           DUCKDB_TYPE_DOUBLE, parqit_finite_fn, err);
}

} // namespace

Session &Session::instance() {
    static Session s;
    return s;
}

Session::~Session() { close(); }

void Session::close() {
    if (con_) {
        duckdb_disconnect(&con_);
        con_ = nullptr;
    }
    if (db_) {
        duckdb_close(&db_);
        db_ = nullptr;
    }
}

bool Session::ensure_open() {
    if (db_ && con_) return true;
    close();

    duckdb_config config = nullptr;
    if (duckdb_create_config(&config) != DuckDBSuccess) {
        last_error_ = "could not create DuckDB config";
        return false;
    }
    if (threads_ > 0)
        duckdb_set_config(config, "threads", std::to_string(threads_).c_str());
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
    threads_ = n;
    return apply_set(*this, "SET threads = " + std::to_string(n), err);
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
    return true;
}

bool Session::set_temp_directory(const std::string &dir, std::string *err) {
    std::string prev = temp_directory_;
    temp_directory_ = dir;
    if (!apply_set(*this, "SET temp_directory = " + quote_literal(dir), err)) {
        temp_directory_ = prev;
        return false;
    }
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

bool Session::query_scalar(const std::string &sql, std::string *value, std::string *err) {
    duckdb_result res;
    if (!query(sql, &res, err)) return false;
    char *v = duckdb_value_varchar(&res, 0, 0);
    *value = v ? v : "";
    if (v) duckdb_free(v);
    duckdb_destroy_result(&res);
    return true;
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
