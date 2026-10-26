/* Deterministic fault injection for the tests of the worker paths (A5, audit
 * of 25 Oct 2026): PARQIT_TEST_FAULT=<site>:<index> makes test_fault(site,
 * index) throw std::bad_alloc where a real allocation could fail, so the
 * tests prove that every such failure ends the work with an error instead of
 * killing or hanging Stata. One getenv per call; unset, nothing happens.
 * Several sites, comma-separated, fault together (a failure, then a second
 * one while it is being reported). */
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <set>
#include <string>
#include <thread>

namespace parqit {

/* Codex final audit, finding 3: fail the C API's next text copy, inside
 * the extraction, without consulting the environment for every cell. */
inline std::atomic<bool> &test_text_copy_armed() {
    static std::atomic<bool> armed{false};
    return armed;
}

inline bool test_text_copy_fails() {
    return test_text_copy_armed().load(std::memory_order_relaxed) &&
           test_text_copy_armed().exchange(false, std::memory_order_relaxed);
}

inline void test_fault(const char *site, long long index) {
    const char *e = std::getenv("PARQIT_TEST_FAULT");
    if (!e) return;
    const size_t n = std::strlen(site);
    for (; *e; e = std::strchr(e, ',') ? std::strchr(e, ',') + 1 : e + std::strlen(e)) {
        if (std::strncmp(e, site, n) != 0 || e[n] != ':') continue;
        char *end = nullptr;
        const long long v = std::strtoll(e + n + 1, &end, 10);
        if (end != e + n + 1 && (*end == '\0' || *end == ',') && v == index) throw std::bad_alloc();
    }
}

/* PARQIT_TEST_DELAY=<site>:<index>:<ms> makes test_delay(site, index) sleep,
 * so the progress tests can hold one block back (A4) */
inline void test_delay(const char *site, long long index) {
    const char *e = std::getenv("PARQIT_TEST_DELAY");
    if (!e) return;
    const size_t n = std::strlen(site);
    if (std::strncmp(e, site, n) != 0 || e[n] != ':') return;
    char *end = nullptr;
    const long long v = std::strtoll(e + n + 1, &end, 10);
    if (end == e + n + 1 || *end != ':' || v != index) return;
    const long long ms = std::strtoll(end + 1, nullptr, 10);
    if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

/* PARQIT_TEST_SYNC=<site>:<index>><site2>:<index2> makes the thread that
 * reaches test_sync(site, index) wait, at most 2 seconds, until a thread has
 * reached test_sync(site2, index2): a test forces one interleaving of the
 * workers (G5). test_sync_reset() forgets the sites reached. */
struct TestSyncState {
    std::mutex m;
    std::condition_variable cv;
    std::set<std::string> reached;
};

inline TestSyncState &test_sync_state() {
    static TestSyncState s;
    return s;
}

inline void test_sync_reset() {
    TestSyncState &s = test_sync_state();
    std::lock_guard<std::mutex> lk(s.m);
    s.reached.clear();
}

inline void test_sync(const char *site, long long index) {
    const char *e = std::getenv("PARQIT_TEST_SYNC");
    if (!e) return;
    TestSyncState &s = test_sync_state();
    const std::string me = std::string(site) + ":" + std::to_string(index);
    const char *gt = std::strchr(e, '>');
    std::unique_lock<std::mutex> lk(s.m);
    s.reached.insert(me);
    s.cv.notify_all();
    if (!gt || std::string(e, gt) != me) return;
    const std::string want(gt + 1);
    s.cv.wait_for(lk, std::chrono::seconds(2), [&] { return s.reached.count(want) > 0; });
}

} // namespace parqit
