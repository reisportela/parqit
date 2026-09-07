#pragma once

#include <array>
#include <cstdint>
#include <string>
#include "duckdb.h"

namespace parqit::statistics {
struct Number {
    std::array<uint64_t, 4> integer{};
    double real = 0;
    unsigned scale = 0;
    bool integral = false;
    bool valid = false;
    double as_double() const;
};

Number result_number(duckdb_result &result, idx_t column, idx_t row = 0);
double midpoint(const Number &a, const Number &b, uint64_t right_weight = 1,
                uint64_t total_weight = 2);
double difference(const Number &a, const Number &b, uint64_t divisor = 1);
int compare(const Number &a, const Number &b);
bool register_functions(duckdb_connection connection, std::string *error);
}
