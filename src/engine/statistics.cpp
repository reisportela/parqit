#include "engine/statistics.hpp"
#include "engine/statistics_math.hpp"
#include "engine/typemap.hpp"

#include <new>
#include <vector>

namespace parqit::statistics {

double Number::as_double() const {
    if (!valid) return NAN;
    if (!integral) return real;
    Int256 value;
    value.word = integer;
    return numeric::integer_ratio(value, 1, scale);
}

Number result_number(duckdb_result &result, idx_t column, idx_t row) {
    Number out;
    if (duckdb_value_is_null(&result, column, row)) return out;
    out.valid = true;
    out.integral = true;
    Int256 value;
    switch (duckdb_column_type(&result, column)) {
    case DUCKDB_TYPE_DECIMAL: {
        const auto x = duckdb_value_decimal(&result, column, row);
        value = Int256::signed128(x.value.lower, x.value.upper);
        out.scale = x.scale;
        break;
    }
    case DUCKDB_TYPE_HUGEINT: {
        const auto x = duckdb_value_hugeint(&result, column, row);
        value = Int256::signed128(x.lower, x.upper);
        break;
    }
    case DUCKDB_TYPE_UHUGEINT: {
        const auto x = duckdb_value_uhugeint(&result, column, row);
        value = Int256::unsigned128(x.lower, x.upper);
        break;
    }
    case DUCKDB_TYPE_UTINYINT: case DUCKDB_TYPE_USMALLINT:
    case DUCKDB_TYPE_UINTEGER: case DUCKDB_TYPE_UBIGINT:
        value = Int256::unsigned128(duckdb_value_uint64(&result, column, row));
        break;
    case DUCKDB_TYPE_BOOLEAN: case DUCKDB_TYPE_TINYINT:
    case DUCKDB_TYPE_SMALLINT: case DUCKDB_TYPE_INTEGER: case DUCKDB_TYPE_BIGINT:
        value = Int256::signed64(duckdb_value_int64(&result, column, row));
        break;
    default:
        out.integral = false;
        out.real = duckdb_value_double(&result, column, row);
    }
    out.integer = value.word;
    return out;
}

double midpoint(const Number &a, const Number &b, uint64_t right_weight,
                uint64_t total_weight) {
    if (!a.valid || !b.valid || !total_weight || right_weight > total_weight ||
        a.integral != b.integral || a.scale != b.scale) return NAN;
    if (!a.integral) {
        numeric::BinarySum sum;
        sum.add_multiple(a.real, total_weight - right_weight);
        sum.add_multiple(b.real, right_weight);
        return sum.mean(total_weight);
    }
    auto product = [](const std::array<uint64_t, 4> &words, uint64_t count) {
        Int256 value;
        value.word = words;
        return value.multiplied(count);
    };
    Int256 value = product(a.integer, total_weight - right_weight);
    value.add(product(b.integer, right_weight));
    return numeric::integer_ratio(value, total_weight, a.scale);
}

double difference(const Number &a, const Number &b, uint64_t divisor) {
    if (!a.valid || !b.valid || a.integral != b.integral || a.scale != b.scale) return NAN;
    if (!a.integral) {
        if (divisor == 1) return a.real - b.real;
        numeric::BinarySum sum;
        sum.add(a.real);
        sum.add(-b.real);
        return sum.mean(divisor);
    }
    Int256 left, right;
    left.word = a.integer;
    right.word = b.integer;
    return numeric::integer_ratio(left - right, divisor, a.scale);
}

int compare(const Number &a, const Number &b) {
    if (!a.integral && !b.integral) return (a.real > b.real) - (a.real < b.real);
    auto sign = [](const Number &x) {
        if (!x.integral) return (x.real > 0) - (x.real < 0);
        Int256 value;
        value.word = x.integer;
        return value.is_zero() ? 0 : (value.negative() ? -1 : 1);
    };
    const int sa = sign(a), sb = sign(b);
    if (sa != sb) return (sa > sb) - (sa < sb);
    if (!sa) return 0;
    auto magnitude = [](const Number &x) {
        Int256 value;
        value.word = x.integer;
        return value.negative() ? value.negated() : value;
    };
    if (a.integral && b.integral) {
        numeric::UInt320 left(magnitude(a).word), right(magnitude(b).word);
        for (unsigned s = a.scale; s < b.scale; ++s) left.multiply(10);
        for (unsigned s = b.scale; s < a.scale; ++s) right.multiply(10);
        return sa * left.compare(right);
    }
    const Number &integer = a.integral ? a : b, &floating = a.integral ? b : a;
    numeric::UInt320 left(magnitude(integer).word), right = numeric::decimal_power(integer.scale);
    uint64_t bits;
    std::memcpy(&bits, &floating.real, sizeof bits);
    const unsigned field = (bits >> 52) & 2047;
    const uint64_t mantissa = (bits & 0xfffffffffffffULL) | (field ? 1ULL << 52 : 0);
    const int exponent = field ? int(field) - 1023 - 52 : -1074;
    right.multiply(mantissa);
    const int lh = left.highest(), rh = right.highest() + exponent;
    int result = (lh > rh) - (lh < rh);
    if (!result) {
        if (exponent < 0) left.shift_left(unsigned(-exponent));
        else right.shift_left(unsigned(exponent));
        result = left.compare(right);
    }
    return sa * (a.integral ? result : -result);
}

namespace {

// Dispatch once per callback on ELF x86-64; retain a generic CPU implementation.
#if defined(__GNUC__) && !defined(__clang__) && defined(__ELF__) && defined(__x86_64__)
#define PARQIT_AGGREGATE_TARGETS __attribute__((target_clones("fma", "default")))
#else
#define PARQIT_AGGREGATE_TARGETS
#endif

struct Input {
    void *data;
    uint64_t *mask;
    duckdb_type type;
    unsigned scale = 0;
    bool floating = false, supported = true, null_type = false;

    explicit Input(duckdb_vector vector)
        : data(duckdb_vector_get_data(vector)), mask(duckdb_vector_get_validity(vector)) {
        auto logical = duckdb_vector_get_column_type(vector);
        type = duckdb_get_type_id(logical);
        if (type == DUCKDB_TYPE_DECIMAL) {
            scale = duckdb_decimal_scale(logical);
            type = duckdb_decimal_internal_type(logical);
        }
        duckdb_destroy_logical_type(&logical);
        floating = type == DUCKDB_TYPE_DOUBLE || type == DUCKDB_TYPE_FLOAT;
        null_type = type == DUCKDB_TYPE_SQLNULL;
        switch (type) {
        case DUCKDB_TYPE_BOOLEAN:
        case DUCKDB_TYPE_TINYINT: case DUCKDB_TYPE_SMALLINT:
        case DUCKDB_TYPE_INTEGER: case DUCKDB_TYPE_BIGINT:
        case DUCKDB_TYPE_UTINYINT: case DUCKDB_TYPE_USMALLINT:
        case DUCKDB_TYPE_UINTEGER: case DUCKDB_TYPE_UBIGINT:
        case DUCKDB_TYPE_HUGEINT: case DUCKDB_TYPE_UHUGEINT:
        case DUCKDB_TYPE_FLOAT: case DUCKDB_TYPE_DOUBLE:
        case DUCKDB_TYPE_SQLNULL: break;
        default: supported = false;
        }
    }
    bool present(idx_t row) const {
        if (null_type || (mask && !(mask[row / 64] & (uint64_t(1) << (row % 64))))) return false;
        return true;
    }
    bool valid(idx_t row) const {
        if (!present(row)) return false;
        if (!floating) return true;
        const double x = real(row);
        return std::isfinite(x) && std::fabs(x) < kStataMissThreshold;
    }
    double real(idx_t row) const {
        return type == DUCKDB_TYPE_FLOAT ? static_cast<float *>(data)[row]
                                        : static_cast<double *>(data)[row];
    }
    Int256 integer(idx_t row) const {
        switch (type) {
        case DUCKDB_TYPE_BOOLEAN: return Int256::unsigned128(static_cast<bool *>(data)[row]);
        case DUCKDB_TYPE_TINYINT: return Int256::signed64(static_cast<int8_t *>(data)[row]);
        case DUCKDB_TYPE_SMALLINT: return Int256::signed64(static_cast<int16_t *>(data)[row]);
        case DUCKDB_TYPE_INTEGER: return Int256::signed64(static_cast<int32_t *>(data)[row]);
        case DUCKDB_TYPE_BIGINT: return Int256::signed64(static_cast<int64_t *>(data)[row]);
        case DUCKDB_TYPE_UTINYINT: return Int256::unsigned128(static_cast<uint8_t *>(data)[row]);
        case DUCKDB_TYPE_USMALLINT: return Int256::unsigned128(static_cast<uint16_t *>(data)[row]);
        case DUCKDB_TYPE_UINTEGER: return Int256::unsigned128(static_cast<uint32_t *>(data)[row]);
        case DUCKDB_TYPE_UBIGINT: return Int256::unsigned128(static_cast<uint64_t *>(data)[row]);
        case DUCKDB_TYPE_HUGEINT: {
            const auto x = static_cast<duckdb_hugeint *>(data)[row];
            return Int256::signed128(x.lower, x.upper);
        }
        case DUCKDB_TYPE_UHUGEINT: {
            const auto x = static_cast<duckdb_uhugeint *>(data)[row];
            return Int256::unsigned128(x.lower, x.upper);
        }
        default: return {};
        }
    }
    Number number(idx_t row) const {
        Number out;
        out.valid = valid(row);
        out.integral = !floating;
        out.scale = scale;
        if (out.valid) {
            if (floating) out.real = real(row);
            else out.integer = integer(row).word;
        }
        return out;
    }
};

template <class State> idx_t state_size(duckdb_function_info) { return sizeof(State); }
template <class State> void init(duckdb_function_info, duckdb_aggregate_state state) {
    new (state) State{};
}
template <class State>
void combine(duckdb_function_info, duckdb_aggregate_state *source,
             duckdb_aggregate_state *target, idx_t count) {
    for (idx_t i = 0; i < count; ++i)
        reinterpret_cast<State *>(target[i])->combine(*reinterpret_cast<State *>(source[i]));
}
template <class State>
void update_real(const Input &input, duckdb_aggregate_state *states, idx_t count) {
    const auto add = [&](auto *values) {
        for (idx_t i = 0; i < count; ++i) {
            if (input.mask && !(input.mask[i / 64] & (uint64_t(1) << (i % 64)))) continue;
            const double x = values[i];
            if (std::isfinite(x) && std::fabs(x) < kStataMissThreshold)
                reinterpret_cast<State *>(states[i])->add(x);
        }
    };
    if (input.type == DUCKDB_TYPE_FLOAT) add(static_cast<float *>(input.data));
    else add(static_cast<double *>(input.data));
}
template <class State>
PARQIT_AGGREGATE_TARGETS
void update(duckdb_function_info info, duckdb_data_chunk chunk, duckdb_aggregate_state *states) {
    Input input(duckdb_data_chunk_get_vector(chunk, 0));
    if (!input.supported) {
        duckdb_aggregate_function_set_error(info, "parqit statistics require numeric values");
        return;
    }
    const idx_t count = duckdb_data_chunk_get_size(chunk);
    if (input.floating) {
        update_real<State>(input, states, count);
    } else {
        for (idx_t i = 0; i < count; ++i)
            if (input.valid(i)) reinterpret_cast<State *>(states[i])->add(input.integer(i), input.scale);
    }
}

PARQIT_AGGREGATE_TARGETS
void update_corr(duckdb_function_info info, duckdb_data_chunk chunk, duckdb_aggregate_state *states) {
    Input a(duckdb_data_chunk_get_vector(chunk, 0)), b(duckdb_data_chunk_get_vector(chunk, 1));
    if (!a.supported || !b.supported) {
        duckdb_aggregate_function_set_error(info, "parqit correlation requires numeric values");
        return;
    }
    for (idx_t i = 0; i < duckdb_data_chunk_get_size(chunk); ++i) {
        if (!a.valid(i) || !b.valid(i)) continue;
        auto &state = *reinterpret_cast<Correlation *>(states[i]);
        if (a.floating && b.floating) state.add(a.real(i), b.real(i));
        else if (a.floating) state.add(a.real(i), b.integer(i));
        else if (b.floating) state.add(a.integer(i), b.real(i));
        else state.add(a.integer(i), b.integer(i));
    }
}
#undef PARQIT_AGGREGATE_TARGETS

struct RealOutput {
    double *values;
    uint64_t *mask;
    explicit RealOutput(duckdb_vector vector) : values(static_cast<double *>(duckdb_vector_get_data(vector))) {
        duckdb_vector_ensure_validity_writable(vector);
        mask = duckdb_vector_get_validity(vector);
    }
    void set(idx_t row, double value, bool stata_range = true) {
        if (std::isfinite(value) && (!stata_range || std::fabs(value) < kStataMissThreshold)) {
            values[row] = value;
            duckdb_validity_set_row_valid(mask, row);
        } else {
            values[row] = NAN;
            duckdb_validity_set_row_invalid(mask, row);
        }
    }
};

void scalar_double(duckdb_function_info info, duckdb_data_chunk chunk, duckdb_vector output) {
    auto vector = duckdb_data_chunk_get_vector(chunk, 0);
    Input input(vector);
    if (!input.supported) {
        duckdb_scalar_function_set_error(info, "parqit numeric conversion requires a number");
        return;
    }
    if (input.type == DUCKDB_TYPE_DOUBLE) {
        duckdb_vector_reference_vector(output, vector);
        return;
    }
    duckdb_vector_ensure_validity_writable(output);
    auto *values = static_cast<double *>(duckdb_vector_get_data(output));
    auto *mask = duckdb_vector_get_validity(output);
    for (idx_t i = 0; i < duckdb_data_chunk_get_size(chunk); ++i) {
        if (!input.present(i)) {
            values[i] = NAN;
            duckdb_validity_set_row_invalid(mask, i);
        } else {
            values[i] = input.floating ? input.real(i) : input.number(i).as_double();
            duckdb_validity_set_row_valid(mask, i);
        }
    }
}

void scalar_exact_double_key(duckdb_function_info info, duckdb_data_chunk chunk, duckdb_vector output) {
    Input input(duckdb_data_chunk_get_vector(chunk, 0));
    if (!input.supported) {
        duckdb_scalar_function_set_error(info, "parqit numeric key requires a number");
        return;
    }
    RealOutput result(output);
    for (idx_t i = 0; i < duckdb_data_chunk_get_size(chunk); ++i) {
        const Number source = input.number(i);
        Number rounded;
        rounded.valid = source.valid;
        rounded.real = source.as_double();
        result.set(i, source.valid && compare(source, rounded) == 0 ? rounded.real : NAN);
    }
}

void scalar_compare(duckdb_function_info info, duckdb_data_chunk chunk, duckdb_vector output) {
    Input a(duckdb_data_chunk_get_vector(chunk, 0)), b(duckdb_data_chunk_get_vector(chunk, 1));
    if (!a.supported || !b.supported) {
        duckdb_scalar_function_set_error(info, "parqit numeric comparison requires numbers");
        return;
    }
    duckdb_vector_ensure_validity_writable(output);
    auto *values = static_cast<int32_t *>(duckdb_vector_get_data(output));
    auto *mask = duckdb_vector_get_validity(output);
    for (idx_t i = 0; i < duckdb_data_chunk_get_size(chunk); ++i) {
        const Number left = a.number(i), right = b.number(i);
        if (!left.valid || !right.valid) {
            values[i] = 0;
            duckdb_validity_set_row_invalid(mask, i);
        } else {
            values[i] = compare(left, right);
            duckdb_validity_set_row_valid(mask, i);
        }
    }
}

enum class BinaryOperation { Midpoint, Difference, Round, Mod };
template<BinaryOperation operation>
void scalar_binary(duckdb_function_info info, duckdb_data_chunk chunk, duckdb_vector output) {
    Input a(duckdb_data_chunk_get_vector(chunk, 0)), b(duckdb_data_chunk_get_vector(chunk, 1));
    if (!a.supported || !b.supported) {
        duckdb_scalar_function_set_error(info, "parqit numeric operation requires numbers");
        return;
    }
    if constexpr (operation == BinaryOperation::Midpoint || operation == BinaryOperation::Difference) {
        if (!a.null_type && !b.null_type && (a.floating != b.floating || a.scale != b.scale)) {
            duckdb_scalar_function_set_error(info, "parqit endpoints must have the same numeric family and scale");
            return;
        }
    }
    RealOutput result(output);
    for (idx_t i = 0; i < duckdb_data_chunk_get_size(chunk); ++i) {
        const Number left = a.number(i), right = b.number(i);
        double value = NAN;
        if constexpr (operation == BinaryOperation::Midpoint) value = midpoint(left, right);
        if constexpr (operation == BinaryOperation::Difference) value = difference(left, right);
        if constexpr (operation == BinaryOperation::Round)
            value = numeric::round_units(left.as_double(), right.as_double());
        if constexpr (operation == BinaryOperation::Mod)
            value = numeric::positive_mod(left.as_double(), right.as_double());
        result.set(i, value, operation != BinaryOperation::Difference);
    }
}

void scalar_sample_count(duckdb_function_info info, duckdb_data_chunk chunk, duckdb_vector output) {
    auto nv = duckdb_data_chunk_get_vector(chunk, 0), pv = duckdb_data_chunk_get_vector(chunk, 1);
    auto *n = static_cast<uint64_t *>(duckdb_vector_get_data(nv));
    auto *p = static_cast<double *>(duckdb_vector_get_data(pv));
    auto *nm = duckdb_vector_get_validity(nv), *pm = duckdb_vector_get_validity(pv);
    duckdb_vector_ensure_validity_writable(output);
    auto *out = static_cast<uint64_t *>(duckdb_vector_get_data(output));
    auto *mask = duckdb_vector_get_validity(output);
    bool cached = false;
    uint64_t previous_n = 0, previous_result = 0;
    double previous_p = 0;
    for (idx_t i = 0; i < duckdb_data_chunk_get_size(chunk); ++i) {
        if ((nm && !(nm[i / 64] & (1ULL << (i % 64)))) ||
            (pm && !(pm[i / 64] & (1ULL << (i % 64))))) {
            duckdb_validity_set_row_invalid(mask, i);
            continue;
        }
        if (!std::isfinite(p[i]) || p[i] < 0 || p[i] > 100) {
            duckdb_scalar_function_set_error(info, "parqit sample percentage out of range");
            return;
        }
        if (!cached || n[i] != previous_n || p[i] != previous_p) {
            previous_n = n[i]; previous_p = p[i]; cached = true;
            previous_result = numeric::sample_count(n[i], p[i]);
        }
        out[i] = previous_result;
        duckdb_validity_set_row_valid(mask, i);
    }
}

template<bool floating>
void histogram_bins(duckdb_function_info info, duckdb_data_chunk chunk, duckdb_vector output,
                    const Input &values, const Input &low, const Input &high) {
    using Exact = std::conditional_t<floating, numeric::BinarySum, Int256>;
    auto multiple = [](const Number &value, uint64_t count) {
        Exact out;
        if constexpr (floating) out.add_multiple(value.real, count);
        else {
            out.word = value.integer;
            out = out.multiplied(count);
        }
        return out;
    };
    const Input bin_input(duckdb_data_chunk_get_vector(chunk, 3));
    auto *bins = static_cast<uint64_t *>(bin_input.data);
    duckdb_vector_ensure_validity_writable(output);
    auto *out = static_cast<int64_t *>(duckdb_vector_get_data(output));
    auto *mask = duckdb_vector_get_validity(output);
    std::vector<Exact> edges;
    Number previous_low, previous_high;
    uint64_t previous_bins = 0;
    for (idx_t row = 0; row < duckdb_data_chunk_get_size(chunk); ++row) {
        const Number x = values.number(row), lo = low.number(row), hi = high.number(row);
        if (!x.valid || !lo.valid || !hi.valid || !bin_input.present(row)) {
            duckdb_validity_set_row_invalid(mask, row);
            continue;
        }
        const uint64_t count = bins[row];
        if (!count || count > 1000 || compare(lo, hi) > 0) {
            duckdb_scalar_function_set_error(info, "parqit histogram: invalid bin boundaries");
            return;
        }
        if (count != previous_bins || compare(lo, previous_low) || compare(hi, previous_high)) {
            edges.assign(count, Exact{});
            for (uint64_t k = 1; k < count; ++k) {
                edges[k] = multiple(lo, count - k);
                const Exact right = multiple(hi, k);
                if constexpr (floating) edges[k].combine(right);
                else edges[k].add(right);
            }
            previous_low = lo; previous_high = hi; previous_bins = count;
        }
        const Exact scaled = multiple(x, count);
        uint64_t first = 1, last = count;
        while (first < last) {
            const uint64_t middle = first + (last - first) / 2;
            if (scaled.compare(edges[middle]) >= 0) first = middle + 1;
            else last = middle;
        }
        out[row] = static_cast<int64_t>(first - 1);
        duckdb_validity_set_row_valid(mask, row);
    }
}

void scalar_histogram(duckdb_function_info info, duckdb_data_chunk chunk, duckdb_vector output) {
    Input x(duckdb_data_chunk_get_vector(chunk, 0)), low(duckdb_data_chunk_get_vector(chunk, 1)),
          high(duckdb_data_chunk_get_vector(chunk, 2));
    if (!x.supported || !low.supported || !high.supported || x.floating != low.floating ||
        x.floating != high.floating || x.scale != low.scale || x.scale != high.scale) {
        duckdb_scalar_function_set_error(info, "parqit histogram: inconsistent numeric types");
        return;
    }
    if (x.floating) histogram_bins<true>(info, chunk, output, x, low, high);
    else histogram_bins<false>(info, chunk, output, x, low, high);
}

bool register_scalar(duckdb_connection connection, const char *name,
                     const std::vector<duckdb_type> &types, duckdb_type result_type,
                     duckdb_scalar_function_t callback, std::string *error) {
    auto function = duckdb_create_scalar_function();
    duckdb_scalar_function_set_name(function, name);
    for (auto type : types) {
        auto logical = duckdb_create_logical_type(type);
        duckdb_scalar_function_add_parameter(function, logical);
        duckdb_destroy_logical_type(&logical);
    }
    auto result = duckdb_create_logical_type(result_type);
    duckdb_scalar_function_set_return_type(function, result);
    duckdb_destroy_logical_type(&result);
    duckdb_scalar_function_set_special_handling(function);
    duckdb_scalar_function_set_function(function, callback);
    const auto rc = duckdb_register_scalar_function(connection, function);
    duckdb_destroy_scalar_function(&function);
    if (rc != DuckDBSuccess && error) *error = std::string("could not register internal scalar ") + name;
    return rc == DuckDBSuccess;
}

template <class State>
void finalize(duckdb_function_info info, duckdb_aggregate_state *states,
              duckdb_vector output, idx_t count, idx_t offset) {
    duckdb_vector_ensure_validity_writable(output);
    auto *parent = duckdb_vector_get_validity(output);
    auto *ns = static_cast<uint64_t *>(duckdb_vector_get_data(duckdb_struct_vector_get_child(output, 0)));
    RealOutput sum(duckdb_struct_vector_get_child(output, 1));
    RealOutput mean(duckdb_struct_vector_get_child(output, 2));
    for (idx_t i = 0; i < count; ++i) {
        const auto &s = *reinterpret_cast<State *>(states[i]);
        const Total &t = [&]() -> const Total & {
            if constexpr (std::is_same<State, Total>::value) return s;
            else return s.total;
        }();
        duckdb_validity_set_row_valid(parent, offset + i);
        ns[offset + i] = t.n;
        sum.set(offset + i, t.value());
        mean.set(offset + i, t.mean());
    }
    if constexpr (!std::is_same<State, Total>::value) {
        RealOutput sd(duckdb_struct_vector_get_child(output, 3));
        RealOutput var(duckdb_struct_vector_get_child(output, 4));
        if constexpr (std::is_same<State, Moments<4>>::value) {
            RealOutput skew(duckdb_struct_vector_get_child(output, 5));
            RealOutput kurt(duckdb_struct_vector_get_child(output, 6));
            for (idx_t i = 0; i < count; ++i) {
                const auto result = reinterpret_cast<State *>(states[i])->result();
                if (result.invalid) {
                    duckdb_aggregate_function_set_error(info, "parqit moments: exact central-moment invariant failed");
                    return;
                }
                sd.set(offset + i, result.sd);
                var.set(offset + i, result.variance);
                skew.set(offset + i, result.skewness);
                kurt.set(offset + i, result.kurtosis);
            }
        } else {
            for (idx_t i = 0; i < count; ++i) {
                const auto result = reinterpret_cast<State *>(states[i])->result();
                if (result.invalid) {
                    duckdb_aggregate_function_set_error(info, "parqit moments: exact central-moment invariant failed");
                    return;
                }
                sd.set(offset + i, result.sd);
                var.set(offset + i, result.variance);
            }
        }
    }
}

void finalize_corr(duckdb_function_info info, duckdb_aggregate_state *states,
                   duckdb_vector output, idx_t count, idx_t offset) {
    duckdb_vector_ensure_validity_writable(output);
    auto *parent = duckdb_vector_get_validity(output);
    RealOutput rho(duckdb_struct_vector_get_child(output, 0));
    auto *ns = static_cast<uint64_t *>(duckdb_vector_get_data(duckdb_struct_vector_get_child(output, 1)));
    auto *bad = static_cast<bool *>(duckdb_vector_get_data(duckdb_struct_vector_get_child(output, 2)));
    RealOutput sine(duckdb_struct_vector_get_child(output, 3));
    auto *perfect = static_cast<bool *>(duckdb_vector_get_data(duckdb_struct_vector_get_child(output, 4)));
    for (idx_t i = 0; i < count; ++i) {
        const auto &s = *reinterpret_cast<Correlation *>(states[i]);
        const auto result = s.result();
        if (result.invalid) {
            duckdb_aggregate_function_set_error(info, "parqit correlation: exact covariance invariant failed");
            return;
        }
        duckdb_validity_set_row_valid(parent, offset + i);
        rho.set(offset + i, result.rho);
        sine.set(offset + i, result.sine);
        perfect[offset + i] = result.perfect;
        ns[offset + i] = s.n;
        bad[offset + i] = false;
    }
}

template <class State>
bool register_aggregate(duckdb_connection connection, const char *name, idx_t nargs,
                        const std::vector<const char *> &names,
                        const std::vector<duckdb_type> &types,
                        duckdb_aggregate_update_t updater,
                        duckdb_aggregate_finalize_t finalizer, std::string *error) {
    auto function = duckdb_create_aggregate_function();
    duckdb_aggregate_function_set_name(function, name);
    auto any = duckdb_create_logical_type(DUCKDB_TYPE_ANY);
    for (idx_t i = 0; i < nargs; ++i) duckdb_aggregate_function_add_parameter(function, any);
    duckdb_destroy_logical_type(&any);
    std::vector<duckdb_logical_type> children;
    for (auto type : types) children.push_back(duckdb_create_logical_type(type));
    auto result_type = duckdb_create_struct_type(children.data(), const_cast<const char **>(names.data()), names.size());
    duckdb_aggregate_function_set_return_type(function, result_type);
    duckdb_destroy_logical_type(&result_type);
    for (auto &type : children) duckdb_destroy_logical_type(&type);
    duckdb_aggregate_function_set_special_handling(function);
    duckdb_aggregate_function_set_functions(function, state_size<State>, init<State>,
                                            updater, combine<State>, finalizer);
    const auto rc = duckdb_register_aggregate_function(connection, function);
    duckdb_destroy_aggregate_function(&function);
    if (rc != DuckDBSuccess && error) *error = std::string("could not register internal aggregate ") + name;
    return rc == DuckDBSuccess;
}
} // namespace

bool register_functions(duckdb_connection connection, std::string *error) {
    const auto d = DUCKDB_TYPE_DOUBLE, n = DUCKDB_TYPE_UBIGINT, any = DUCKDB_TYPE_ANY;
    return register_scalar(connection, "__parqit_double", {any}, d, scalar_double, error) &&
           register_scalar(connection, "__parqit_exact_double_key", {any}, d, scalar_exact_double_key, error) &&
           register_scalar(connection, "__parqit_compare", {any, any}, DUCKDB_TYPE_INTEGER, scalar_compare, error) &&
           register_scalar(connection, "__parqit_midpoint", {any, any}, d,
                           scalar_binary<BinaryOperation::Midpoint>, error) &&
           register_scalar(connection, "__parqit_difference", {any, any}, d,
                           scalar_binary<BinaryOperation::Difference>, error) &&
           register_scalar(connection, "__parqit_round", {any, any}, d,
                           scalar_binary<BinaryOperation::Round>, error) &&
           register_scalar(connection, "__parqit_mod", {any, any}, d,
                           scalar_binary<BinaryOperation::Mod>, error) &&
           register_scalar(connection, "__parqit_sample_count", {n, d}, n, scalar_sample_count, error) &&
           register_scalar(connection, "__parqit_histbin", {any, any, any, n}, DUCKDB_TYPE_BIGINT,
                           scalar_histogram, error) &&
           register_aggregate<Total>(connection, "__parqit_total", 1,
               {"n", "sum", "mean"}, {n, d, d}, update<Total>, finalize<Total>, error) &&
           register_aggregate<Moments<2>>(connection, "__parqit_stats", 1,
               {"n", "sum", "mean", "sd", "variance"}, {n, d, d, d, d},
               update<Moments<2>>, finalize<Moments<2>>, error) &&
           register_aggregate<Moments<4>>(connection, "__parqit_detail", 1,
               {"n", "sum", "mean", "sd", "variance", "skewness", "kurtosis"}, {n, d, d, d, d, d, d},
               update<Moments<4>>, finalize<Moments<4>>, error) &&
           register_aggregate<Correlation>(connection, "__parqit_corr", 2,
               {"rho", "n", "unstable", "sine", "perfect"},
               {d, n, DUCKDB_TYPE_BOOLEAN, d, DUCKDB_TYPE_BOOLEAN}, update_corr, finalize_corr, error);
}
} // namespace parqit::statistics
