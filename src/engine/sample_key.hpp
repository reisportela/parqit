/* parqit — SAMPLE-DESIGN-1: the random priority of a sampling cluster.
 *
 * A cluster's key depends only on the seed and on the cluster's value, never
 * on row order, file layout, thread count or the DuckDB version: parqit owns
 * the function so a seed keeps drawing the same clusters across engine
 * upgrades. Numbers hash their binary64 value (so 1 stored as INTEGER, BIGINT
 * or DOUBLE is one cluster with one key); text hashes its UTF-8 bytes.
 *
 *   mix(z)       splitmix64's finalizer (modulo 2^64)
 *   number x     mix(bits(x) ^ mix(seed)), with -0 read as +0
 *   text s       mix(fnv1a64(s) ^ mix(seed ^ 0x5bd1e9955bd1e995))
 *
 * mix is a bijection, so distinct binary64 values never share a key. Integers
 * beyond 2^53 are ranked by their binary64 approximation: two that round to the
 * same double share a key, and the caller then orders them by value. */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace parqit {
namespace sample_key {

inline uint64_t mix(uint64_t z) {
    z += 0x9e3779b97f4a7c15ULL;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

inline uint64_t of_double(uint64_t seed, double x) {
    if (x == 0) x = 0.0;
    uint64_t bits;
    std::memcpy(&bits, &x, sizeof bits);
    return mix(bits ^ mix(seed));
}

inline uint64_t of_bytes(uint64_t seed, const char *data, size_t length) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < length; i++) {
        h ^= static_cast<unsigned char>(data[i]);
        h *= 0x100000001b3ULL;
    }
    return mix(h ^ mix(seed ^ 0x5bd1e9955bd1e995ULL));
}

} // namespace sample_key
} // namespace parqit
