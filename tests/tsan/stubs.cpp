// Stubs for the DuckDB-free ThreadSanitizer harness: the two SQL quoting
// helpers (copied verbatim from src/engine/session.cpp:843-865 so the plans'
// KV metadata text is identical) and the DuckDB C API entry points typemap.o
// references but the SPSS/R/text paths never call (they abort if reached).
#include <cstdio>
#include <cstdlib>
#include <string>

#include "duckdb.h"

namespace parqit {
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
} // namespace parqit

extern "C" {
uint8_t duckdb_decimal_scale(duckdb_logical_type) { std::fprintf(stderr, "stub called\n"); std::abort(); }
uint8_t duckdb_decimal_width(duckdb_logical_type) { std::fprintf(stderr, "stub called\n"); std::abort(); }
duckdb_type duckdb_get_type_id(duckdb_logical_type) { std::fprintf(stderr, "stub called\n"); std::abort(); }
}
