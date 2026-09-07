/* Exceptional statistics retry. Keep DuckDB 1.5.3's Welford/covariance
 * update and parallel-merge arithmetic, but finalize overflow as NULL.
 * These C-API aggregates are registered and used only after a built-in
 * aggregate throws; successful queries keep their original engine path. */
#pragma once

#include <cmath>
#include <cstdint>
#include <new>
#include <string>

#include "duckdb.h"

namespace parqit::stats_overflow {

struct Moments {
    uint64_t n = 0;
    double mean = 0, m2 = 0;

    void add(double x) {
        ++n;
        const double next = mean + (x - mean) / n;
        m2 += (x - next) * (x - mean);
        mean = next;
    }
    void combine(const Moments &src) {
        if (n == 0) { *this = src; return; }
        if (src.n == 0) return;
        const double total = static_cast<double>(n + src.n);
        const double delta = src.mean - mean;
        m2 = src.m2 + m2 + delta * delta * src.n * n / total;
        mean = std::fma(static_cast<double>(src.n) / total, delta, mean);
        n += src.n;
    }
};

struct Correlation {
    Moments x, y;
    uint64_t n = 0;
    double mean_x = 0, mean_y = 0, cross = 0;

    void add(double a, double b) {
        ++n;
        const double dx = a - mean_x, dy = b - mean_y;
        mean_x += dx / n;
        mean_y += dy / n;
        cross += dx * (b - mean_y);
        x.add(a);
        y.add(b);
    }
    void combine(const Correlation &src) {
        if (n == 0) { *this = src; return; }
        if (src.n == 0) return;
        const double total = static_cast<double>(n + src.n);
        const double dx = mean_x - src.mean_x, dy = mean_y - src.mean_y;
        cross = src.cross + cross + dx * dy * src.n * n / total;
        mean_x = (src.n * src.mean_x + n * mean_x) / total;
        mean_y = (src.n * src.mean_y + n * mean_y) / total;
        n += src.n;
        x.combine(src.x);
        y.combine(src.y);
    }
};

template <class State> idx_t state_size(duckdb_function_info) { return sizeof(State); }
template <class State> void init(duckdb_function_info, duckdb_aggregate_state state) {
    new (state) State{};
}
template <class State>
void combine(duckdb_function_info, duckdb_aggregate_state *src,
             duckdb_aggregate_state *dst, idx_t n) {
    for (idx_t i = 0; i < n; ++i)
        reinterpret_cast<State *>(dst[i])->combine(*reinterpret_cast<State *>(src[i]));
}

inline bool valid(uint64_t *mask, idx_t row) {
    return !mask || duckdb_validity_row_is_valid(mask, row);
}

inline void update_unary(duckdb_function_info, duckdb_data_chunk chunk,
                         duckdb_aggregate_state *states) {
    duckdb_vector v = duckdb_data_chunk_get_vector(chunk, 0);
    auto *x = static_cast<double *>(duckdb_vector_get_data(v));
    auto *mask = duckdb_vector_get_validity(v);
    for (idx_t i = 0; i < duckdb_data_chunk_get_size(chunk); ++i)
        if (valid(mask, i)) reinterpret_cast<Moments *>(states[i])->add(x[i]);
}

inline void update_corr(duckdb_function_info, duckdb_data_chunk chunk,
                        duckdb_aggregate_state *states) {
    duckdb_vector a = duckdb_data_chunk_get_vector(chunk, 0),
                  b = duckdb_data_chunk_get_vector(chunk, 1);
    auto *x = static_cast<double *>(duckdb_vector_get_data(a)),
         *y = static_cast<double *>(duckdb_vector_get_data(b));
    auto *mx = duckdb_vector_get_validity(a), *my = duckdb_vector_get_validity(b);
    for (idx_t i = 0; i < duckdb_data_chunk_get_size(chunk); ++i)
        if (valid(mx, i) && valid(my, i))
            reinterpret_cast<Correlation *>(states[i])->add(y[i], x[i]);
}

template <bool SD>
void finalize_unary(duckdb_function_info, duckdb_aggregate_state *states,
                    duckdb_vector output, idx_t n, idx_t offset) {
    auto *values = static_cast<double *>(duckdb_vector_get_data(output));
    duckdb_vector_ensure_validity_writable(output);
    auto *mask = duckdb_vector_get_validity(output);
    for (idx_t i = 0; i < n; ++i) {
        const auto &s = *reinterpret_cast<Moments *>(states[i]);
        double value = s.n > 1 ? s.m2 / (s.n - 1) : NAN;
        if (SD) value = std::sqrt(value);
        if (std::isfinite(value)) {
            values[offset + i] = value;
            duckdb_validity_set_row_valid(mask, offset + i);
        } else duckdb_validity_set_row_invalid(mask, offset + i);
    }
}

inline void finalize_corr(duckdb_function_info, duckdb_aggregate_state *states,
                          duckdb_vector output, idx_t n, idx_t offset) {
    auto *values = static_cast<double *>(duckdb_vector_get_data(output));
    duckdb_vector_ensure_validity_writable(output);
    auto *mask = duckdb_vector_get_validity(output);
    for (idx_t i = 0; i < n; ++i) {
        const auto &s = *reinterpret_cast<Correlation *>(states[i]);
        const double sx = s.n > 1 ? std::sqrt(s.x.m2 / s.n) : 0,
                     sy = s.n > 1 ? std::sqrt(s.y.m2 / s.n) : 0;
        const double value = s.n && std::isfinite(sx) && std::isfinite(sy) && sx * sy != 0
                                 ? (s.cross / s.n) / (sx * sy) : NAN;
        if (std::isfinite(value)) {
            values[offset + i] = value;
            duckdb_validity_set_row_valid(mask, offset + i);
        } else duckdb_validity_set_row_invalid(mask, offset + i);
    }
}

template <class State>
bool register_function(duckdb_connection con, const char *name, idx_t nargs,
                       duckdb_aggregate_update_t update,
                       duckdb_aggregate_finalize_t finalize, std::string *err) {
    auto function = duckdb_create_aggregate_function();
    duckdb_aggregate_function_set_name(function, name);
    auto type = duckdb_create_logical_type(DUCKDB_TYPE_DOUBLE);
    for (idx_t i = 0; i < nargs; ++i)
        duckdb_aggregate_function_add_parameter(function, type);
    duckdb_aggregate_function_set_return_type(function, type);
    duckdb_destroy_logical_type(&type);
    duckdb_aggregate_function_set_functions(function, state_size<State>, init<State>,
                                            update, combine<State>, finalize);
    const auto rc = duckdb_register_aggregate_function(con, function);
    duckdb_destroy_aggregate_function(&function);
    if (rc != DuckDBSuccess && err)
        *err = std::string("could not register internal aggregate ") + name;
    return rc == DuckDBSuccess;
}

inline bool register_functions(duckdb_connection con, std::string *err) {
    return register_function<Moments>(con, "__parqit_sd_fallback", 1,
                                      update_unary, finalize_unary<true>, err) &&
           register_function<Moments>(con, "__parqit_var_fallback", 1,
                                      update_unary, finalize_unary<false>, err) &&
           register_function<Correlation>(con, "__parqit_corr_fallback", 2,
                                          update_corr, finalize_corr, err);
}

} // namespace parqit::stats_overflow
