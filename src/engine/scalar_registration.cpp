#include "engine/scalar_registration.hpp"
#include "engine/statistics.hpp"
#include <new>

#ifdef PARQIT_DUCKDB_200
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"

namespace {

duckdb::bind_scalar_function_t capi_binder() {
    static const auto callback = [] {
        auto handle = duckdb_create_scalar_function();
        if (!handle) throw std::bad_alloc();
        auto callback = reinterpret_cast<duckdb::ScalarFunction *>(handle)->GetBindCallback();
        duckdb_destroy_scalar_function(&handle);
        return callback;
    }();
    return callback;
}

parqit::NumericTypeInfo bound_numeric_type(const duckdb::LogicalType &type) {
    // C API v1 logical-type handles are LogicalType pointers; these getters
    // are read-only. The pinned logical_types-c.cpp verifies that contract.
    auto borrowed = reinterpret_cast<duckdb_logical_type>(const_cast<duckdb::LogicalType *>(&type));
    return parqit::numeric_type_info(borrowed);
}

// Only literal values and built-in primitive casts of literals are inspected.
// Parameters, column-dependent bounds and arbitrary functions keep their
// runtime error ordering, even when an optimiser might later fold them.
bool primitive_literal(duckdb::ClientContext &context, const duckdb::Expression &expression,
                       unsigned depth = 0) {
    using namespace duckdb;
    if (expression.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) return true;
    if (depth >= 8 || !BoundCastExpression::IsCast(expression)) return false;
    const auto &cast = expression.Cast<BoundFunctionExpression>();
    if (!BoundCastExpression::HasValidBindData(cast)) return false;
    const auto source = BoundCastExpression::SourceType(cast);
    const auto &target = BoundCastExpression::TargetType(cast);
    const bool source_primitive = bound_numeric_type(source).supported ||
        source.id() == LogicalTypeId::VARCHAR || source.id() == LogicalTypeId::STRING_LITERAL ||
        source.id() == LogicalTypeId::INTEGER_LITERAL;
    if (!source_primitive || !bound_numeric_type(target).supported) return false;
    if (!BoundCastExpression::IsDefaultCast(cast) &&
        CastFunctionSet::Get(context).CanOverrideDefaultCast(source, target)) return false;
    // A cast expression can also be constructed with a custom implementation.
    // Require the actual stateless built-in, not merely a familiar cast name.
    CastFunctionSet builtins;
    GetCastFunctionInput lookup;
    auto expected = builtins.GetCastFunction(source, target, lookup);
    const auto &actual = BoundCastExpression::GetBoundCast(cast);
    if (!actual.Equals(expected) || actual.GetCastData() || expected.GetCastData() ||
        actual.HasInitLocalState() || expected.HasInitLocalState()) return false;
    return primitive_literal(context, BoundCastExpression::Child(cast), depth + 1);
}

bool histogram_bounds_prove_total(duckdb::BindScalarFunctionInput &input) {
    using namespace duckdb;
    const auto &args = input.GetArguments();
    if (args.size() != 4) return false;
    const auto x = bound_numeric_type(args[0]->GetReturnType());
    for (size_t i = 1; i < 3; ++i) {
        const auto bound = bound_numeric_type(args[i]->GetReturnType());
        // Match the callback's exact family/scale guard, including SQLNULL.
        if (!x.supported || !bound.supported || x.floating != bound.floating || x.scale != bound.scale)
            return false;
    }
    parqit::statistics::Number values[3];
    auto &context = input.GetClientContext();
    for (size_t i = 1; i < 4; ++i) {
        if (!primitive_literal(context, *args[i])) return false;
        Value value;
        if (!ExpressionExecutor::TryEvaluateScalar(context, *args[i], value)) return false;
        if (value.IsNull() || !bound_numeric_type(value.type()).supported) return false;
        Vector vector(value, count_t(1));
        values[i - 1] = parqit::statistics::vector_number(reinterpret_cast<duckdb_vector>(&vector));
        if (!values[i - 1].valid) return false;
    }
    parqit::statistics::Number minimum, maximum;
    minimum.valid = maximum.valid = true;
    minimum.integral = maximum.integral = false;
    minimum.real = 1; maximum.real = 1000;
    // The fourth declared parameter is UBIGINT. Every numeric constant in
    // this interval casts to an admissible bin count without an error.
    return parqit::statistics::compare(values[0], values[1]) <= 0 &&
        parqit::statistics::compare(values[2], minimum) >= 0 &&
        parqit::statistics::compare(values[2], maximum) <= 0;
}

template<parqit::ScalarErrorPolicy policy>
duckdb::unique_ptr<duckdb::FunctionData> bind_known_scalar(duckdb::BindScalarFunctionInput &input) {
    auto data = capi_binder()(input);
    bool proven = true;
    if constexpr (policy == parqit::ScalarErrorPolicy::HistogramBounds) {
        // An unsuccessful proof is not a bind-time error. Leave the original
        // expression intact and let its existing runtime path report errors.
        proven = histogram_bounds_prove_total(input);
    } else if constexpr (policy != parqit::ScalarErrorPolicy::Total) {
        const auto &args = input.GetArguments();
        constexpr size_t arity = policy == parqit::ScalarErrorPolicy::NumericUnary ? 1 : 2;
        proven = args.size() == arity;
        for (const auto &arg : args) proven = proven && bound_numeric_type(arg->GetReturnType()).supported;
        if constexpr (policy == parqit::ScalarErrorPolicy::MatchingNumeric) {
            if (proven) proven = parqit::numeric_families_match(
                bound_numeric_type(args[0]->GetReturnType()), bound_numeric_type(args[1]->GetReturnType()));
        }
    }
    input.GetBoundFunction().SetErrorMode(proven ? duckdb::FunctionErrors::CANNOT_ERROR
                                               : duckdb::FunctionErrors::CAN_THROW_RUNTIME_ERROR);
    return data;
}

} // namespace
#endif

namespace parqit {

duckdb_state register_known_scalar(duckdb_connection connection, duckdb_scalar_function function,
                                   ScalarErrorPolicy policy) {
    if (!connection || !function) return DuckDBError;
#ifdef PARQIT_DUCKDB_200
    // 2.0 marks every C API scalar fallible when registering it. Restore only
    // proved contracts on the bound copy, after validating the actual types.
    // Other C API functions and value-dependent errors keep the upstream rule.
    if (policy != ScalarErrorPolicy::ValueDependent) {
        auto &scalar = *reinterpret_cast<duckdb::ScalarFunction *>(function);
        duckdb::bind_scalar_function_t original;
        try { original = capi_binder(); }
        catch (...) { return DuckDBError; }
        if (!original || scalar.GetBindCallback() != original) return DuckDBError;
        struct RestoreBinder {
            duckdb::ScalarFunction &function;
            duckdb::bind_scalar_function_t original;
            ~RestoreBinder() { function.SetBindCallback(original); }
        } restore{scalar, original};
        switch (policy) {
        case ScalarErrorPolicy::Total:
            scalar.SetBindCallback(bind_known_scalar<ScalarErrorPolicy::Total>); break;
        case ScalarErrorPolicy::NumericUnary:
            scalar.SetBindCallback(bind_known_scalar<ScalarErrorPolicy::NumericUnary>); break;
        case ScalarErrorPolicy::NumericBinary:
            scalar.SetBindCallback(bind_known_scalar<ScalarErrorPolicy::NumericBinary>); break;
        case ScalarErrorPolicy::MatchingNumeric:
            scalar.SetBindCallback(bind_known_scalar<ScalarErrorPolicy::MatchingNumeric>); break;
        case ScalarErrorPolicy::HistogramBounds:
            scalar.SetBindCallback(bind_known_scalar<ScalarErrorPolicy::HistogramBounds>); break;
        default: break;
        }
        return duckdb_register_scalar_function(connection, function);
    }
#else
    (void)policy;
#endif
    return duckdb_register_scalar_function(connection, function);
}

} // namespace parqit
