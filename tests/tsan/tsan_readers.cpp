// B9 (audit of 25 Oct 2026): a ThreadSanitizer harness of the reader workers, without the DuckDB engine (a TSan
// binary linking the whole static engine segfaulted at start-up on the host where this was written; only the
// engine's bundled miniz/zstd are linked here). The consumer of the ArrowArrayStream is
// emulated as the engine's Arrow scan does it: under one lock, each consumer releases its previous batch and takes
// the next (arrow.cpp:103-123); it reads every buffer outside the lock. Faults and delays come from
// PARQIT_TEST_FAULT / PARQIT_TEST_DELAY. Modes:
//   parqit_tsan_readers spss <file.sav> <threads> <block_cases> <consumers> <batch_budget> <raw_budget> <stop_after>
//   parqit_tsan_readers rdata <file.rds> <threads> <batch_rows> <consumers> <stop_after>
//   parqit_tsan_readers zsav-split <in.zsav> <out.zsav> <block_bytes>   (the same cases in ZLIB blocks of
//     block_bytes inflated bytes: many blocks, so the B2 threads that inflate ahead run; little-endian files)
#include <atomic>
#include <fstream>
#include <iterator>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "abi.h"
#include "engine/rdata_parallel.hpp"
#include "engine/rdata_plan.hpp"
#include "engine/spss_parallel.hpp"
#include "engine/spss_plan.hpp"
#include "engine/spss_reader.hpp"
#include "miniz.hpp"

namespace {

uint64_t read_all(const ArrowArray *a, const ArrowSchema *schema) {
    uint64_t sum = 0;
    for (int64_t c = 0; c < a->n_children; c++) {
        const ArrowArray *ch = a->children[c];
        const char *fmt = schema->children[c]->format;
        const int64_t n = ch->length;
        if (ch->buffers[0]) {
            const auto *v = static_cast<const uint8_t *>(ch->buffers[0]);
            for (int64_t i = 0; i < (n + 7) / 8; i++) sum += v[i];
        }
        if (!std::strcmp(fmt, "U")) {
            const auto *offs = static_cast<const int64_t *>(ch->buffers[1]);
            const auto *bytes = static_cast<const char *>(ch->buffers[2]);
            for (int64_t i = 0; i < n; i++)
                for (int64_t k = offs[i]; k < offs[i + 1]; k++) sum += static_cast<uint8_t>(bytes[k]);
        } else {
            int width = 8;
            if (!std::strcmp(fmt, "tdD") || !std::strcmp(fmt, "i")) width = 4;
            else if (!std::strcmp(fmt, "c")) width = 1;
            else if (!std::strcmp(fmt, "b")) width = 0;
            const auto *d = static_cast<const uint8_t *>(ch->buffers[1]);
            const int64_t bytes = width ? n * width : (n + 7) / 8;
            for (int64_t i = 0; i < bytes; i++) sum += d[i];
        }
    }
    return sum;
}

int64_t drain(ArrowArrayStream *st, int consumers, int64_t stop_after, std::string *err) {
    ArrowSchema schema{};
    if (st->get_schema(st, &schema) != 0) {
        *err = "get_schema failed";
        return -1;
    }
    std::mutex scan;
    std::atomic<int64_t> batches{0}, rows{0};
    std::atomic<uint64_t> checksum{0};
    std::atomic<bool> done{false};
    auto loop = [&] {
        ArrowArray held{};
        for (;;) {
            {
                std::lock_guard<std::mutex> lk(scan);
                if (held.release) held.release(&held);
                if (done || (stop_after > 0 && batches >= stop_after)) return;
                ArrowArray next{};
                if (st->get_next(st, &next) != 0) {
                    *err = st->get_last_error(st) ? st->get_last_error(st) : "error";
                    done = true;
                    return;
                }
                if (!next.release) {
                    done = true;
                    return;
                }
                batches++;
                held = next;
            }
            rows += held.length;
            checksum += read_all(&held, &schema);
        }
    };
    std::vector<std::thread> pool;
    for (int i = 0; i < consumers; i++) pool.emplace_back(loop);
    for (auto &t : pool) t.join();
    if (schema.release) schema.release(&schema);
    std::printf("drained batches=%lld rows=%lld checksum=%llu err=%s\n", static_cast<long long>(batches.load()),
                static_cast<long long>(rows.load()), static_cast<unsigned long long>(checksum.load()), err->c_str());
    return rows.load();
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    const std::string mode = argv[1], path = argv[2];
    auto arg = [&](int i, long long d) { return argc > i ? std::atoll(argv[i]) : d; };
    std::string err;
    if (mode == "spss") {
        const int threads = static_cast<int>(arg(3, 8));
        parqit::spss::ReadOptions opt;
        opt.threads = threads;
        opt.block_cases = arg(4, 0);
        /* PARQIT_TSAN_CONVERT_FAULT: a PARQIT_TEST_FAULT for the conversion only, not for the profile make_plan
         * runs over the same blocks; set and unset while no thread runs */
        const char *cf = std::getenv("PARQIT_TSAN_CONVERT_FAULT");
        const std::string convert_fault = cf ? cf : "";
        for (int rep = 0; rep < 3; rep++) {
            auto plan = std::make_shared<const parqit::spss::Plan>(parqit::spss::make_plan(path, opt));
            if (!convert_fault.empty()) setenv("PARQIT_TEST_FAULT", convert_fault.c_str(), 1);
            parqit::spss::SavArrowSource src(plan, threads, arg(4, 0), arg(6, 0), arg(7, 0));
            src.start();
            drain(src.stream(), static_cast<int>(arg(5, 4)), arg(8, 0), &err);
            const std::string e = src.finish();
            if (!convert_fault.empty()) unsetenv("PARQIT_TEST_FAULT");
            std::printf("finish: batches=%lld peak_batch=%lld peak_raw=%lld err=%s\n",
                        static_cast<long long>(src.batches()), static_cast<long long>(src.peak_batch_bytes()),
                        static_cast<long long>(src.peak_raw_bytes()), e.c_str());
        }
        return 0;
    }
    if (mode == "zsav-split") {
        if (argc < 5) return 2;
        const parqit::spss::Dictionary d = parqit::spss::read_dictionary(path);
        std::ifstream f(path, std::ios::binary);
        const std::string in((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::string stream;
        for (const auto &b : d.zblocks) {
            std::string u(b.uncompressed_size, '\0');
            duckdb_miniz::mz_ulong got = b.uncompressed_size;
            if (duckdb_miniz::mz_uncompress(reinterpret_cast<unsigned char *>(&u[0]), &got,
                                            reinterpret_cast<const unsigned char *>(in.data() + b.compressed_ofs),
                                            b.compressed_size) != duckdb_miniz::MZ_OK || got != b.uncompressed_size)
                return 3;
            stream += u;
        }
        auto put64 = [](std::string &o, int64_t v) { o.append(reinterpret_cast<const char *>(&v), 8); };
        auto put32 = [](std::string &o, int32_t v) { o.append(reinterpret_cast<const char *>(&v), 4); };
        const size_t block = static_cast<size_t>(std::atoll(argv[4]));
        int64_t ztrailer_in;
        std::memcpy(&ztrailer_in, in.data() + d.data_offset + 8, 8);
        std::vector<std::string> comp;
        std::vector<size_t> usz;
        for (size_t off = 0; off < stream.size(); off += block) {
            const std::string chunk = stream.substr(off, block);
            duckdb_miniz::mz_ulong bound = duckdb_miniz::mz_compressBound(chunk.size());
            std::string c(bound, '\0');
            if (duckdb_miniz::mz_compress(reinterpret_cast<unsigned char *>(&c[0]), &bound,
                                          reinterpret_cast<const unsigned char *>(chunk.data()), chunk.size()) !=
                duckdb_miniz::MZ_OK)
                return 3;
            c.resize(bound);
            comp.push_back(c);
            usz.push_back(chunk.size());
        }
        std::string out = in.substr(0, d.data_offset);
        const int64_t zh = static_cast<int64_t>(d.data_offset);
        int64_t total = 0;
        for (const auto &c : comp) total += static_cast<int64_t>(c.size());
        put64(out, zh);
        put64(out, zh + 24 + total);
        put64(out, 24 * static_cast<int64_t>(comp.size() + 1));
        for (const auto &c : comp) out += c;
        out += in.substr(static_cast<size_t>(ztrailer_in), 16); /* int_bias, zero */
        put32(out, static_cast<int32_t>(block));
        put32(out, static_cast<int32_t>(comp.size()));
        int64_t uofs = zh, cofs = zh + 24;
        for (size_t k = 0; k < comp.size(); k++) {
            put64(out, uofs);
            put64(out, cofs);
            put32(out, static_cast<int32_t>(usz[k]));
            put32(out, static_cast<int32_t>(comp[k].size()));
            uofs += static_cast<int64_t>(usz[k]);
            cofs += static_cast<int64_t>(comp[k].size());
        }
        std::ofstream(argv[3], std::ios::binary) << out;
        std::printf("blocks=%zu\n", comp.size());
        return 0;
    }
    if (mode == "rdata") {
        const int threads = static_cast<int>(arg(3, 8));
        parqit::rdata::ReadOptions opt;
        opt.threads = threads;
        const char *tmp = std::getenv("TMPDIR");
        opt.tmpdir = tmp && *tmp ? tmp : "/tmp";
        for (int rep = 0; rep < 3; rep++) {
            auto plan = std::make_shared<const parqit::rdata::Plan>(parqit::rdata::make_plan(path, opt));
            parqit::rdata::RdataArrowSource src(plan, threads, arg(4, 0));
            src.start();
            drain(src.stream(), static_cast<int>(arg(5, 4)), arg(6, 0), &err);
            const std::string e = src.finish();
            std::printf("finish: batches=%lld err=%s\n", static_cast<long long>(src.batches()), e.c_str());
        }
        return 0;
    }
    return 2;
}
