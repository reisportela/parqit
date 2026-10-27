#include "engine/statistics.hpp"
#include "engine/scalar_registration.hpp"
#include "engine/sample_key.hpp"
#include "engine/statistics_math.hpp"
#include "engine/standard_errors.hpp"
#include "engine/integer_storage.hpp"
#include "engine/sort_storage.hpp"
#include "engine/sum_storage.hpp"
#include "engine/weighted_moments.hpp"
#include "engine/typemap.hpp"

#include <new>
#include <cfenv>
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

struct Input : NumericTypeInfo {
    void *data;
    uint64_t *mask;

    explicit Input(duckdb_vector vector)
        : data(duckdb_vector_get_data(vector)), mask(duckdb_vector_get_validity(vector)) {
        auto logical = duckdb_vector_get_column_type(vector);
        static_cast<NumericTypeInfo &>(*this) = numeric_type_info(logical);
        duckdb_destroy_logical_type(&logical);
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

void update_integer_storage(duckdb_function_info info, duckdb_data_chunk chunk,
                            duckdb_aggregate_state *states) {
    Input input(duckdb_data_chunk_get_vector(chunk, 0));
    const auto count = duckdb_data_chunk_get_size(chunk);
    if (input.type == DUCKDB_TYPE_DATE) {
        const auto *values = static_cast<const duckdb_date *>(input.data);
        for (idx_t i = 0; i < count; ++i) {
            if (!input.present(i)) continue;
            auto &state = *reinterpret_cast<IntegerStorage *>(states[i]);
            const auto days = values[i].days;
            if (days == std::numeric_limits<int32_t>::max() ||
                days == -std::numeric_limits<int32_t>::max()) state.invalid_date = true;
            else state.add(static_cast<double>(days) + kEpochShiftDays);
        }
        return;
    }
    if (input.type != DUCKDB_TYPE_DOUBLE) {
        duckdb_aggregate_function_set_error(info, "integer storage proof requires DOUBLE or DATE input");
        return;
    }
    const auto *values = static_cast<const double *>(input.data);
    for (idx_t i = 0; i < count; ++i)
        if (input.present(i)) reinterpret_cast<IntegerStorage *>(states[i])->add(values[i]);
}

void update_sort_storage(duckdb_function_info info, duckdb_data_chunk chunk,
                         duckdb_aggregate_state *states) {
    Input input(duckdb_data_chunk_get_vector(chunk, 0));
    if (input.type != DUCKDB_TYPE_DOUBLE) {
        duckdb_aggregate_function_set_error(info, "sort storage proof requires DOUBLE input");
        return;
    }
    const auto count = duckdb_data_chunk_get_size(chunk);
    const auto *values = static_cast<const double *>(input.data);
    for (idx_t i = 0; i < count; ++i)
        if (input.present(i)) reinterpret_cast<SortStorage *>(states[i])->add(values[i]);
}

void update_sum_storage(duckdb_function_info info, duckdb_data_chunk chunk,
                         duckdb_aggregate_state *states) {
    Input input(duckdb_data_chunk_get_vector(chunk, 0));
    if (input.type != DUCKDB_TYPE_DOUBLE) {
        duckdb_aggregate_function_set_error(info, "binary sum proof requires DOUBLE input");
        return;
    }
    const auto count = duckdb_data_chunk_get_size(chunk);
    const auto *values = static_cast<const double *>(input.data);
    for (idx_t i = 0; i < count; ++i)
        if (input.present(i)) reinterpret_cast<SumStorage *>(states[i])->add(values[i]);
}

void finalize_sum_storage(duckdb_function_info, duckdb_aggregate_state *states,
                           duckdb_vector output, idx_t count, idx_t offset) {
    duckdb_vector_ensure_validity_writable(output);
    auto *parent = duckdb_vector_get_validity(output);
    duckdb_vector child[5];
    uint64_t *valid[5];
    for (idx_t col = 0; col < 5; ++col) {
        child[col] = duckdb_struct_vector_get_child(output, col);
        duckdb_vector_ensure_validity_writable(child[col]);
        valid[col] = duckdb_vector_get_validity(child[col]);
    }
    auto *n = static_cast<uint64_t *>(duckdb_vector_get_data(child[0]));
    auto *exponent = static_cast<int32_t *>(duckdb_vector_get_data(child[1]));
    auto *width = static_cast<int32_t *>(duckdb_vector_get_data(child[2]));
    auto *safe = static_cast<bool *>(duckdb_vector_get_data(child[3]));
    auto *integer32 = static_cast<bool *>(duckdb_vector_get_data(child[4]));
    for (idx_t i = 0; i < count; ++i) {
        const auto &state = *reinterpret_cast<SumStorage *>(states[i]);
        const auto row = offset + i;
        duckdb_validity_set_row_valid(parent, row);
        for (auto *mask : valid) duckdb_validity_set_row_valid(mask, row);
        n[row] = state.n;
        exponent[row] = state.exponent();
        width[row] = static_cast<int32_t>(state.width());
        safe[row] = state.safe();
        integer32[row] = !state.invalid && (state.integer_mask & 4);
    }
}

void scalar_scaled_integer(duckdb_function_info info, duckdb_data_chunk chunk,
                            duckdb_vector output) {
    Input input(duckdb_data_chunk_get_vector(chunk, 0));
    Input scales(duckdb_data_chunk_get_vector(chunk, 1));
    if (input.type != DUCKDB_TYPE_DOUBLE) {
        duckdb_scalar_function_set_error(info, "binary scaling requires DOUBLE input");
        return;
    }
    duckdb_vector_ensure_validity_writable(output);
    auto *valid = duckdb_vector_get_validity(output);
    auto *values = static_cast<duckdb_hugeint *>(duckdb_vector_get_data(output));
    const auto *exponent = static_cast<const int32_t *>(scales.data);
    const auto count = duckdb_data_chunk_get_size(chunk);
    for (idx_t i = 0; i < count; ++i) {
        if (!input.present(i) || !scales.present(i)) {
            duckdb_validity_set_row_invalid(valid, i);
            continue;
        }
        ScaledInteger value;
        if (!value.assign(input.real(i), exponent[i])) {
            duckdb_scalar_function_set_error(info, "source no longer fits its exact binary scaling proof");
            return;
        }
        values[i].lower = value.low;
        std::memcpy(&values[i].upper, &value.high, sizeof value.high);
        duckdb_validity_set_row_valid(valid, i);
    }
}

template <class Integer, uint32_t Bit>
void scalar_sort_integer(duckdb_function_info info, duckdb_data_chunk chunk,
                          duckdb_vector output) {
    Input input(duckdb_data_chunk_get_vector(chunk, 0));
    duckdb_vector_ensure_validity_writable(output);
    auto *valid = duckdb_vector_get_validity(output);
    auto *values = static_cast<Integer *>(duckdb_vector_get_data(output));
    const auto count = duckdb_data_chunk_get_size(chunk);
    for (idx_t i = 0; i < count; ++i) {
        if (!input.present(i)) {
            duckdb_validity_set_row_invalid(valid, i);
            continue;
        }
        const double value = input.real(i);
        if (!(SortStorage::fits(value) & Bit)) {
            duckdb_scalar_function_set_error(info,
                "parqit sort: source no longer fits its lossless integer proof");
            return;
        }
        duckdb_validity_set_row_valid(valid, i);
        values[i] = static_cast<Integer>(value);
    }
}

template <bool Restore>
void scalar_sort_bits(duckdb_function_info info, duckdb_data_chunk chunk, duckdb_vector output) {
    Input input(duckdb_data_chunk_get_vector(chunk, 0));
    duckdb_vector_ensure_validity_writable(output);
    auto *valid = duckdb_vector_get_validity(output);
    auto *values = static_cast<unsigned char *>(duckdb_vector_get_data(output));
    const auto *source = static_cast<const unsigned char *>(input.data);
    const auto count = duckdb_data_chunk_get_size(chunk);
    for (idx_t i = 0; i < count; ++i) {
        if (!input.present(i)) { duckdb_validity_set_row_invalid(valid, i); continue; }
        uint64_t bits;
        std::memcpy(&bits, source + i*sizeof bits, sizeof bits);
        if constexpr (Restore) bits = SortStorage::original_bits(bits);
        else {
            if (!SortStorage::bits_safe(bits)) {
                duckdb_scalar_function_set_error(info, "source no longer fits its lossless floating sort-key proof");
                return;
            }
            bits = SortStorage::ordered_bits(bits);
        }
        std::memcpy(values + i*sizeof bits, &bits, sizeof bits);
        duckdb_validity_set_row_valid(valid, i);
    }
}

template <class State>
void finalize_storage(duckdb_function_info, duckdb_aggregate_state *states,
                       duckdb_vector output, idx_t count, idx_t offset) {
    duckdb_vector_ensure_validity_writable(output);
    auto *parent = duckdb_vector_get_validity(output);
    auto child = duckdb_struct_vector_get_child(output, 0);
    duckdb_vector_ensure_validity_writable(child);
    auto *valid = duckdb_vector_get_validity(child);
    auto *mask = static_cast<uint32_t *>(duckdb_vector_get_data(child));
    for (idx_t i = 0; i < count; ++i) {
        duckdb_validity_set_row_valid(parent, offset + i);
        duckdb_validity_set_row_valid(valid, offset + i);
        mask[offset + i] = reinterpret_cast<State *>(states[i])->result();
    }
    if constexpr (std::is_same<State, SortStorage>::value) {
        auto child_bits = duckdb_struct_vector_get_child(output, 1);
        duckdb_vector_ensure_validity_writable(child_bits);
        auto *bits_valid = duckdb_vector_get_validity(child_bits);
        auto *reversible = static_cast<bool *>(duckdb_vector_get_data(child_bits));
        for (idx_t i = 0; i < count; ++i) {
            duckdb_validity_set_row_valid(bits_valid, offset + i);
            reversible[offset + i] = reinterpret_cast<SortStorage *>(states[i])->reversible;
        }
    }
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
    // 2.0 can derive cardinality from vector buffers. The input chunk is
    // immutable throughout this callback; do not repeat that walk per row.
    const idx_t input_rows = duckdb_data_chunk_get_size(chunk);
    for (idx_t i = 0; i < input_rows; ++i) {
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
    const idx_t input_rows = duckdb_data_chunk_get_size(chunk);
    for (idx_t i = 0; i < input_rows; ++i) {
        if (!input.present(i)) {
            values[i] = NAN;
            duckdb_validity_set_row_invalid(mask, i);
        } else {
            values[i] = input.floating ? input.real(i) : input.number(i).as_double();
            duckdb_validity_set_row_valid(mask, i);
        }
    }
}

void scalar_integer_mean(duckdb_function_info info, duckdb_data_chunk chunk, duckdb_vector output) {
    Input numerator(duckdb_data_chunk_get_vector(chunk, 0));
    Input denominator(duckdb_data_chunk_get_vector(chunk, 1));
    if (!numerator.supported || numerator.floating) {
        duckdb_scalar_function_set_error(info, "exact integer mean requires an integral or DECIMAL numerator");
        return;
    }
    if (!denominator.supported || denominator.floating || denominator.scale) {
        duckdb_scalar_function_set_error(info, "exact integer mean requires an integer count");
        return;
    }
    RealOutput result(output);
    const auto count = duckdb_data_chunk_get_size(chunk);
    constexpr uint64_t exact = uint64_t(1) << 53;
    const bool nearest = std::fegetround() == FE_TONEAREST;
    for (idx_t i = 0; i < count; ++i) {
        if (!numerator.present(i) || !denominator.present(i)) {
            result.set(i, NAN);
            continue;
        }
        const auto count_value = denominator.integer(i);
        if (count_value.negative() || count_value.word[1] || count_value.word[2] || count_value.word[3]) {
            duckdb_scalar_function_set_error(info, "exact integer mean count is outside uint64 range");
            return;
        }
        const uint64_t n = count_value.word[0];
        if (!n) { result.set(i, NAN); continue; }
        const auto value = numerator.integer(i);
        const bool negative = value.negative();
        const auto magnitude = negative ? value.negated() : value;
        // With two exact binary64 operands, one IEEE division rounds once.
        // Wider numerators/counts and decimal scaling use the existing exact
        // midpoint comparison, without first rounding the numerator to double.
        if (nearest && !numerator.scale && n <= exact &&
            !(magnitude.word[1] | magnitude.word[2] | magnitude.word[3]) &&
            magnitude.word[0] <= exact) {
            const double x = static_cast<double>(magnitude.word[0]);
            result.set(i, (negative ? -x : x) / static_cast<double>(n));
        } else {
            result.set(i, numeric::integer_ratio(value, n, numerator.scale));
        }
    }
}

template <bool Mean>
void scalar_scaled_result(duckdb_function_info info, duckdb_data_chunk chunk, duckdb_vector output) {
    Input numerator(duckdb_data_chunk_get_vector(chunk, 0));
    Input scales(duckdb_data_chunk_get_vector(chunk, Mean ? 2 : 1));
    if (!numerator.supported || numerator.floating || numerator.scale) {
        duckdb_scalar_function_set_error(info, "scaled total requires an integer numerator");
        return;
    }
    RealOutput result(output);
    const auto count = duckdb_data_chunk_get_size(chunk);
    const auto *exponent = static_cast<const int32_t *>(scales.data);
    if constexpr (Mean) {
        Input counts(duckdb_data_chunk_get_vector(chunk, 1));
        if (!counts.supported || counts.floating || counts.scale) {
            duckdb_scalar_function_set_error(info, "scaled mean requires an integer count");
            return;
        }
        for (idx_t i = 0; i < count; ++i) {
            if (!numerator.present(i) || !scales.present(i) || !counts.present(i)) {
                result.set(i, NAN);
                continue;
            }
            const auto n = counts.integer(i);
            if (n.negative() || n.word[1] || n.word[2] || n.word[3] ||
                exponent[i] < -1074 || exponent[i] > 1022) {
                duckdb_scalar_function_set_error(info, "scaled mean count or exponent is outside its exact domain");
                return;
            }
            result.set(i, scaled_mean(numerator.integer(i), n.word[0], exponent[i]));
        }
    } else {
        for (idx_t i = 0; i < count; ++i) {
            if (!numerator.present(i) || !scales.present(i)) { result.set(i, NAN); continue; }
            if (exponent[i] < -1074 || exponent[i] > 1022) {
                duckdb_scalar_function_set_error(info, "scaled total exponent is outside the binary64 domain");
                return;
            }
            result.set(i, scaled_sum(numerator.integer(i), exponent[i]));
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
    const idx_t input_rows = duckdb_data_chunk_get_size(chunk);
    for (idx_t i = 0; i < input_rows; ++i) {
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
    const idx_t input_rows = duckdb_data_chunk_get_size(chunk);
    for (idx_t i = 0; i < input_rows; ++i) {
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
        if (!numeric_families_match(a, b)) {
            duckdb_scalar_function_set_error(info, "parqit endpoints must have the same numeric family and scale");
            return;
        }
    }
    RealOutput result(output);
    const idx_t input_rows = duckdb_data_chunk_get_size(chunk);
    for (idx_t i = 0; i < input_rows; ++i) {
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
    const idx_t input_rows = duckdb_data_chunk_get_size(chunk);
    for (idx_t i = 0; i < input_rows; ++i) {
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

/* SAMPLE-DESIGN-1: the priority of a sampling cluster (engine/sample_key.hpp);
 * __parqit_sample_key_d takes the binary64 value, __parqit_sample_key_s the text */
template <bool text>
void scalar_sample_key(duckdb_function_info, duckdb_data_chunk chunk, duckdb_vector output) {
    auto sv = duckdb_data_chunk_get_vector(chunk, 0), xv = duckdb_data_chunk_get_vector(chunk, 1);
    auto *seed = static_cast<uint64_t *>(duckdb_vector_get_data(sv));
    auto *sm = duckdb_vector_get_validity(sv), *xm = duckdb_vector_get_validity(xv);
    duckdb_vector_ensure_validity_writable(output);
    auto *out = static_cast<uint64_t *>(duckdb_vector_get_data(output));
    auto *mask = duckdb_vector_get_validity(output);
    const idx_t input_rows = duckdb_data_chunk_get_size(chunk);
    for (idx_t i = 0; i < input_rows; ++i) {
        if ((sm && !(sm[i / 64] & (1ULL << (i % 64)))) ||
            (xm && !(xm[i / 64] & (1ULL << (i % 64))))) {
            duckdb_validity_set_row_invalid(mask, i);
            continue;
        }
        if constexpr (text) {
            auto *s = static_cast<duckdb_string_t *>(duckdb_vector_get_data(xv));
            out[i] = sample_key::of_bytes(seed[i], duckdb_string_t_data(&s[i]),
                                          duckdb_string_t_length(s[i]));
        } else {
            out[i] = sample_key::of_double(seed[i], static_cast<double *>(duckdb_vector_get_data(xv))[i]);
        }
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
    const idx_t input_rows = duckdb_data_chunk_get_size(chunk);
    for (idx_t row = 0; row < input_rows; ++row) {
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
                     duckdb_scalar_function_t callback, std::string *error,
                     ScalarErrorPolicy policy = ScalarErrorPolicy::ValueDependent) {
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
    const auto rc = register_known_scalar(connection, function, policy);
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

void update_mass_threshold(duckdb_function_info info,duckdb_data_chunk chunk,duckdb_aggregate_state *states) {
    Input w(duckdb_data_chunk_get_vector(chunk,0)), k(duckdb_data_chunk_get_vector(chunk,1));
    if (!w.supported || !k.supported || k.floating || k.scale) {
        duckdb_aggregate_function_set_error(info,"parqit weighted selection requires an integer rank"); return;
    }
    const idx_t rows=duckdb_data_chunk_get_size(chunk);
    for (idx_t i=0;i<rows;++i) {
        if (!w.valid(i) || !k.valid(i)) continue;
        const double value=w.number(i).as_double();
        if (value>0) reinterpret_cast<MassThreshold *>(states[i])->add(value,k.integer(i).word[0]);
    }
}

void update_scaled_mass(duckdb_function_info info,duckdb_data_chunk chunk,duckdb_aggregate_state *states) {
    Input w(duckdb_data_chunk_get_vector(chunk,0));
    if (!w.supported) { duckdb_aggregate_function_set_error(info,"parqit weight mass requires numeric weights"); return; }
    const idx_t rows=duckdb_data_chunk_get_size(chunk);
    for (idx_t i=0;i<rows;++i) if (w.valid(i)) {
        const double weight=w.number(i).as_double();
        if (weight>0) reinterpret_cast<ScaledMass *>(states[i])->add(weight);
    }
}
void finalize_scaled_mass(duckdb_function_info,duckdb_aggregate_state *states,
                          duckdb_vector output,idx_t count,idx_t offset) {
    duckdb_vector_ensure_validity_writable(output);
    const auto exponent=duckdb_struct_vector_get_child(output,1);
    duckdb_vector_ensure_validity_writable(exponent);
    auto *powers=static_cast<int32_t *>(duckdb_vector_get_data(exponent));
    RealOutput mantissa(duckdb_struct_vector_get_child(output,0));
    for (idx_t i=0;i<count;++i) {
        const auto p=reinterpret_cast<ScaledMass *>(states[i])->parts();
        duckdb_validity_set_row_valid(duckdb_vector_get_validity(output),offset+i);
        duckdb_validity_set_row_valid(duckdb_vector_get_validity(exponent),offset+i);
        powers[offset+i]=p.second; mantissa.set(offset+i,p.first);
    }
}
void scalar_mass_share(duckdb_function_info,duckdb_data_chunk chunk,duckdb_vector output) {
    Input a(duckdb_data_chunk_get_vector(chunk,0)), ae(duckdb_data_chunk_get_vector(chunk,1)),
          b(duckdb_data_chunk_get_vector(chunk,2)), be(duckdb_data_chunk_get_vector(chunk,3));
    RealOutput result(output);
    const idx_t rows=duckdb_data_chunk_get_size(chunk);
    for (idx_t i=0;i<rows;++i) {
        if (!a.valid(i) || !b.valid(i) || !ae.valid(i) || !be.valid(i)) { result.set(i,NAN); continue; }
        const double am=a.number(i).as_double(), bm=b.number(i).as_double();
        const int ap=static_cast<int>(ae.number(i).as_double()), bp=static_cast<int>(be.number(i).as_double());
        if (!bm) { result.set(i,NAN); continue; }
        if (!am) { result.set(i,0); continue; }
        const double av=std::scalbn(am,ap), bv=std::scalbn(bm,bp), ratio=av/bv;
        const double share=std::isnormal(ratio) && std::isfinite(ratio) ? ratio*100 :
            std::scalbn((am/bm)*100,ap-bp);
        result.set(i,share);
    }
}

void update_ordered_gini(duckdb_function_info info,duckdb_data_chunk chunk,duckdb_aggregate_state *states) {
    Input x(duckdb_data_chunk_get_vector(chunk,0)),w(duckdb_data_chunk_get_vector(chunk,1));
    if (!x.supported || !w.supported) { duckdb_aggregate_function_set_error(info,"parqit gini requires numeric values"); return; }
    const idx_t rows=duckdb_data_chunk_get_size(chunk);
    for (idx_t i=0;i<rows;++i) if (x.valid(i) && w.valid(i)) {
        const double weight=w.number(i).as_double();
        if (weight>0) reinterpret_cast<OrderedGini *>(states[i])->add(x.number(i),weight);
    }
}
void finalize_ordered_gini(duckdb_function_info info,duckdb_aggregate_state *states,
                           duckdb_vector output,idx_t count,idx_t offset) {
    duckdb_vector_ensure_validity_writable(output);
    RealOutput value(duckdb_struct_vector_get_child(output,0));
    for (idx_t i=0;i<count;++i) {
        const auto &state=*reinterpret_cast<OrderedGini *>(states[i]);
        if (state.invalid) { duckdb_aggregate_function_set_error(info,"parqit gini: sorted-state invariant failed"); return; }
        duckdb_validity_set_row_valid(duckdb_vector_get_validity(output),offset+i);
        value.set(offset+i,state.result());
    }
}
void finalize_mass_threshold(duckdb_function_info info,duckdb_aggregate_state *states,
                             duckdb_vector output,idx_t count,idx_t offset) {
    duckdb_vector_ensure_validity_writable(output);
    const auto child=duckdb_struct_vector_get_child(output,0);
    duckdb_vector_ensure_validity_writable(child);
    auto *values=static_cast<bool *>(duckdb_vector_get_data(child));
    for (idx_t i=0;i<count;++i) {
        const auto &state=*reinterpret_cast<MassThreshold *>(states[i]);
        if (state.invalid) { duckdb_aggregate_function_set_error(info,"parqit weighted selection rank must be constant"); return; }
        duckdb_validity_set_row_valid(duckdb_vector_get_validity(output),offset+i);
        duckdb_validity_set_row_valid(duckdb_vector_get_validity(child),offset+i);
        values[offset+i]=state.reached();
    }
}

template<unsigned Order>
void update_weighted(duckdb_function_info info, duckdb_data_chunk chunk,
                     duckdb_aggregate_state *states) {
    Input x(duckdb_data_chunk_get_vector(chunk,0)), w(duckdb_data_chunk_get_vector(chunk,1));
    if (!x.supported || !w.supported) {
        duckdb_aggregate_function_set_error(info,"parqit weighted aggregates require numeric values");
        return;
    }
    const idx_t rows=duckdb_data_chunk_get_size(chunk);
    for (idx_t i=0;i<rows;++i) {
        if (!x.valid(i) || !w.valid(i)) continue;
        const double weight=w.number(i).as_double();
        if (weight>0) reinterpret_cast<WeightedMoments<Order> *>(states[i])->add(x.number(i),weight);
    }
}

template<unsigned Order,bool Errors=false>
void finalize_weighted(duckdb_function_info info, duckdb_aggregate_state *states,
                       duckdb_vector output,idx_t count,idx_t offset) {
    duckdb_vector_ensure_validity_writable(output);
    auto *mask=duckdb_vector_get_validity(output);
    auto *ns=static_cast<uint64_t *>(duckdb_vector_get_data(duckdb_struct_vector_get_child(output,0)));
    for (idx_t i=0;i<count;++i) {
        const auto r=reinterpret_cast<WeightedMoments<Order> *>(states[i])->result(Errors);
        if (r.invalid) {
            duckdb_aggregate_function_set_error(info,"parqit weighted central-moment invariant failed");
            return;
        }
        duckdb_validity_set_row_valid(mask,offset+i); ns[offset+i]=r.n;
        const double values[]={r.mass,r.sum,r.mean,Errors ? r.sebinomial : r.variance,Errors ? r.sepoisson : r.variance_aw,r.sd,r.sd_aw,r.skewness,r.kurtosis};
        for (idx_t c=0;c<(Errors ? 5 : Order==1 ? 3 : Order==2 ? 7 : 9);++c)
            RealOutput(duckdb_struct_vector_get_child(output,c+1)).set(offset+i,values[c]);
        if constexpr (!Errors)
            RealOutput(duckdb_struct_vector_get_child(output,Order==1 ? 4 : Order==2 ? 8 : 10)).set(offset+i,r.sum_aw);
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
void finalize_standard_error(duckdb_function_info, duckdb_aggregate_state *states,
                             duckdb_vector output, idx_t count, idx_t offset) {
    duckdb_vector_ensure_validity_writable(output);
    auto *parent = duckdb_vector_get_validity(output);
    RealOutput result(duckdb_struct_vector_get_child(output, 0));
    for (idx_t i = 0; i < count; ++i) {
        duckdb_validity_set_row_valid(parent, offset + i);
        result.set(offset + i, reinterpret_cast<State *>(states[i])->result());
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

Number vector_number(duckdb_vector vector, idx_t row) {
    Input input(vector);
    return input.supported ? input.number(row) : Number{};
}

bool register_functions(duckdb_connection connection, std::string *error) {
    const auto d = DUCKDB_TYPE_DOUBLE, n = DUCKDB_TYPE_UBIGINT, any = DUCKDB_TYPE_ANY;
    return register_scalar(connection, "__parqit_double", {any}, d, scalar_double, error, ScalarErrorPolicy::NumericUnary) &&
           register_scalar(connection, "__parqit_integer_mean", {any, any}, d, scalar_integer_mean, error) &&
           register_scalar(connection, "__parqit_scaled_integer", {any, DUCKDB_TYPE_INTEGER}, DUCKDB_TYPE_HUGEINT,
                           scalar_scaled_integer, error) &&
           register_scalar(connection, "__parqit_scaled_sum", {any, DUCKDB_TYPE_INTEGER}, d,
                           scalar_scaled_result<false>, error) &&
           register_scalar(connection, "__parqit_scaled_mean", {any, any, DUCKDB_TYPE_INTEGER}, d,
                           scalar_scaled_result<true>, error) &&
           register_scalar(connection, "__parqit_sort_i8", {d}, DUCKDB_TYPE_TINYINT,
                           scalar_sort_integer<int8_t, 1>, error) &&
           register_scalar(connection, "__parqit_sort_i16", {d}, DUCKDB_TYPE_SMALLINT,
                           scalar_sort_integer<int16_t, 2>, error) &&
           register_scalar(connection, "__parqit_sort_i32", {d}, DUCKDB_TYPE_INTEGER,
                           scalar_sort_integer<int32_t, 4>, error) &&
           register_scalar(connection, "__parqit_sort_bits", {d}, n, scalar_sort_bits<false>, error) &&
           register_scalar(connection, "__parqit_sort_restore", {n}, d, scalar_sort_bits<true>, error,
                           ScalarErrorPolicy::Total) &&
           register_scalar(connection, "__parqit_exact_double_key", {any}, d, scalar_exact_double_key, error, ScalarErrorPolicy::NumericUnary) &&
           register_scalar(connection, "__parqit_compare", {any, any}, DUCKDB_TYPE_INTEGER, scalar_compare, error, ScalarErrorPolicy::NumericBinary) &&
           register_scalar(connection, "__parqit_midpoint", {any, any}, d,
                           scalar_binary<BinaryOperation::Midpoint>, error, ScalarErrorPolicy::MatchingNumeric) &&
           register_scalar(connection, "__parqit_difference", {any, any}, d,
                           scalar_binary<BinaryOperation::Difference>, error, ScalarErrorPolicy::MatchingNumeric) &&
           register_scalar(connection, "__parqit_round", {any, any}, d,
                           scalar_binary<BinaryOperation::Round>, error, ScalarErrorPolicy::NumericBinary) &&
           register_scalar(connection, "__parqit_mod", {any, any}, d,
                           scalar_binary<BinaryOperation::Mod>, error, ScalarErrorPolicy::NumericBinary) &&
           register_scalar(connection, "__parqit_sample_count", {n, d}, n, scalar_sample_count, error) &&
           register_scalar(connection, "__parqit_sample_key_d", {n, d}, n, scalar_sample_key<false>, error, ScalarErrorPolicy::Total) &&
           register_scalar(connection, "__parqit_sample_key_s", {n, DUCKDB_TYPE_VARCHAR}, n,
                           scalar_sample_key<true>, error, ScalarErrorPolicy::Total) &&
           register_scalar(connection, "__parqit_histbin", {any, any, any, n}, DUCKDB_TYPE_BIGINT,
                           scalar_histogram, error, ScalarErrorPolicy::HistogramBounds) &&
           register_aggregate<Total>(connection, "__parqit_total", 1,
               {"n", "sum", "mean"}, {n, d, d}, update<Total>, finalize<Total>, error) &&
           register_aggregate<IntegerStorage>(connection, "__parqit_integer_storage", 1,
               {"mask"}, {DUCKDB_TYPE_UINTEGER}, update_integer_storage, finalize_storage<IntegerStorage>, error) &&
           register_aggregate<SortStorage>(connection, "__parqit_sort_storage", 1,
               {"mask", "reversible"}, {DUCKDB_TYPE_UINTEGER, DUCKDB_TYPE_BOOLEAN},
               update_sort_storage, finalize_storage<SortStorage>, error) &&
           register_aggregate<SumStorage>(connection, "__parqit_sum_storage", 1,
               {"n", "exponent", "width", "safe", "integer32"},
               {n, DUCKDB_TYPE_INTEGER, DUCKDB_TYPE_INTEGER, DUCKDB_TYPE_BOOLEAN, DUCKDB_TYPE_BOOLEAN},
               update_sum_storage, finalize_sum_storage, error) &&
           register_aggregate<BinomialError>(connection, "__parqit_sebinomial", 1,
               {"value"}, {d}, update<BinomialError>, finalize_standard_error<BinomialError>, error) &&
           register_aggregate<PoissonError>(connection, "__parqit_sepoisson", 1,
               {"value"}, {d}, update<PoissonError>, finalize_standard_error<PoissonError>, error) &&
           register_aggregate<Moments<2>>(connection, "__parqit_stats", 1,
               {"n", "sum", "mean", "sd", "variance"}, {n, d, d, d, d},
               update<Moments<2>>, finalize<Moments<2>>, error) &&
           register_aggregate<Moments<4>>(connection, "__parqit_detail", 1,
               {"n", "sum", "mean", "sd", "variance", "skewness", "kurtosis"}, {n, d, d, d, d, d, d},
               update<Moments<4>>, finalize<Moments<4>>, error) &&
           register_aggregate<MassThreshold>(connection,"__parqit_mass_ge",2,
               {"value"},{DUCKDB_TYPE_BOOLEAN},update_mass_threshold,finalize_mass_threshold,error) &&
           register_aggregate<ScaledMass>(connection,"__parqit_mass_parts",1,
               {"mantissa","exponent"},{d,DUCKDB_TYPE_INTEGER},update_scaled_mass,finalize_scaled_mass,error) &&
           register_aggregate<OrderedGini>(connection,"__parqit_gini",2,
               {"value"},{d},update_ordered_gini,finalize_ordered_gini,error) &&
           register_scalar(connection,"__parqit_mass_share",{d,DUCKDB_TYPE_INTEGER,d,DUCKDB_TYPE_INTEGER},d,scalar_mass_share,error) &&
           register_aggregate<WeightedMoments<1>>(connection, "__parqit_wtotal", 2,
               {"n","mass","sum","mean","sum_aw"},{n,d,d,d,d},
               update_weighted<1>,finalize_weighted<1>,error) &&
           register_aggregate<WeightedMoments<1>>(connection, "__parqit_werrors", 2,
               {"n","mass","sum","mean","sebinomial","sepoisson"},{n,d,d,d,d,d},
               update_weighted<1>,finalize_weighted<1,true>,error) &&
           register_aggregate<WeightedMoments<2>>(connection, "__parqit_wstats", 2,
               {"n","mass","sum","mean","variance","variance_aw","sd","sd_aw","sum_aw"},{n,d,d,d,d,d,d,d,d},
               update_weighted<2>,finalize_weighted<2>,error) &&
           register_aggregate<WeightedMoments<4>>(connection, "__parqit_wdetail", 2,
               {"n","mass","sum","mean","variance","variance_aw","sd","sd_aw","skewness","kurtosis","sum_aw"},
               {n,d,d,d,d,d,d,d,d,d,d},update_weighted<4>,finalize_weighted<4>,error) &&
           register_aggregate<Correlation>(connection, "__parqit_corr", 2,
               {"rho", "n", "unstable", "sine", "perfect"},
               {d, n, DUCKDB_TYPE_BOOLEAN, d, DUCKDB_TYPE_BOOLEAN}, update_corr, finalize_corr, error);
}
} // namespace parqit::statistics
