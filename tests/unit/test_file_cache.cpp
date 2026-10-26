#ifdef PARQIT_CACHE_STANDALONE
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#endif
#include "doctest.h"
#include "engine/session.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#ifdef PARQIT_DUCKDB_200
#include "duckdb/storage/external_file_cache/caching_file_system.hpp"
#else
#include "duckdb/storage/caching_file_system.hpp"
#endif
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace {
struct CachePayload {
    std::string bytes, tag;
    duckdb::timestamp_t modified{1};
#ifdef PARQIT_DUCKDB_200
    duckdb::optional<duckdb::timestamp_t> deadline;
#endif
};

// Synthetic sources and a one-shot I/O callback make the invalidation order exact.
class CacheTestFS : public duckdb::FileSystem {
    struct Handle : duckdb::FileHandle {
        std::shared_ptr<CachePayload> payload;
        duckdb::idx_t position = 0;
        Handle(CacheTestFS &fs, const std::string &path, duckdb::FileOpenFlags flags)
            : FileHandle(fs, path, flags), payload(fs.current) {}
        void Close() override {}
    };
public:
    std::shared_ptr<CachePayload> current;
    std::atomic<int> opens{0}, reads{0};
    bool seekable = true;
    std::function<void()> before_read;

    void publish(char value, size_t size = 32768, bool tagged = true) {
        current = std::make_shared<CachePayload>();
        current->bytes.assign(size, value);
        current->tag = tagged ? std::string(1, value) : "";
        current->modified = tagged ? duckdb::timestamp_t(1) : duckdb::Timestamp::GetCurrentTimestamp();
    }
    duckdb::unique_ptr<duckdb::FileHandle> OpenFile(const std::string &path, duckdb::FileOpenFlags flags,
            duckdb::optional_ptr<duckdb::FileOpener> = nullptr) override {
        ++opens;
        return duckdb::make_uniq<Handle>(*this, path, flags);
    }
    void Read(duckdb::FileHandle &file, void *buffer, int64_t size, duckdb::idx_t offset) override {
        auto &handle = static_cast<Handle &>(file);
        if (before_read) std::exchange(before_read, {})();
        if (offset > handle.payload->bytes.size() || size < 0 ||
            static_cast<uint64_t>(size) > handle.payload->bytes.size() - offset)
            throw duckdb::IOException("synthetic cache read beyond EOF");
        ++reads;
        std::memcpy(buffer, handle.payload->bytes.data() + offset, size);
    }
    int64_t Read(duckdb::FileHandle &file, void *buffer, int64_t size) override {
        auto &handle = static_cast<Handle &>(file);
        size = std::min<int64_t>(size, handle.payload->bytes.size() - handle.position);
        Read(file, buffer, size, handle.position);
        handle.position += size;
        return size;
    }
    int64_t GetFileSize(duckdb::FileHandle &file) override {
        return static_cast<Handle &>(file).payload->bytes.size();
    }
    duckdb::timestamp_t GetLastModifiedTime(duckdb::FileHandle &file) override {
        return static_cast<Handle &>(file).payload->modified;
    }
    std::string GetVersionTag(duckdb::FileHandle &file) override {
        return static_cast<Handle &>(file).payload->tag;
    }
#ifdef PARQIT_DUCKDB_200
    duckdb::optional<duckdb::timestamp_t> GetCacheValidUntil(duckdb::FileHandle &file) override {
        return static_cast<Handle &>(file).payload->deadline;
    }
#endif
    void Seek(duckdb::FileHandle &file, duckdb::idx_t offset) override {
        static_cast<Handle &>(file).position = offset;
    }
    bool CanSeek() override { return seekable; }
    bool OnDiskFile(duckdb::FileHandle &) override { return true; }
    bool IsLocalFileSystem() const override { return false; }
    std::string GetName() const override { return "parqit-cache-identity-test"; }
};

struct CacheFixture {
    parqit::Session &session = parqit::Session::instance();
    CacheTestFS fs;
    std::unique_ptr<duckdb::CachingFileSystem> cache;
    CacheFixture() {
        session.close();
        std::string error;
        REQUIRE(session.set_threads(1, &error));
        REQUIRE_MESSAGE(session.exec("SET enable_external_file_cache=true", &error), error);
#ifdef PARQIT_DUCKDB_200
        REQUIRE_MESSAGE(session.exec("SET async_threads=0", &error), error);
        REQUIRE_MESSAGE(session.exec("SET cache_local_files=true", &error), error);
        REQUIRE_MESSAGE(session.exec("SET external_file_cache_local_max_block_size=4096", &error), error);
#endif
        auto &context = *reinterpret_cast<duckdb::Connection *>(session.con())->context;
        cache = std::make_unique<duckdb::CachingFileSystem>(fs, *context.db);
        fs.publish('A');
    }
    ~CacheFixture() { cache.reset(); session.close(); }
    duckdb::unique_ptr<duckdb::CachingFileHandle> open(bool validate = true) {
        duckdb::OpenFileInfo info("parqit-cache-identity.parquet");
        info.extended_info = duckdb::make_shared_ptr<duckdb::ExtendedOpenFileInfo>();
        info.extended_info->options["validate_external_file_cache"] = duckdb::Value::BOOLEAN(validate);
        return cache->OpenFile(info, duckdb::FileFlags::FILE_FLAGS_READ);
    }
};

struct CacheRead {
#ifdef PARQIT_DUCKDB_200
    duckdb::FileBufferHandleGroup pin;
#else
    duckdb::BufferHandle pin;
#endif
    std::string bytes;
    CacheRead(duckdb::CachingFileHandle &handle, duckdb::idx_t size, duckdb::idx_t offset,
              bool sequential = false, bool seek_first = true) {
        if (sequential && seek_first) handle.Seek(offset);
#ifdef PARQIT_DUCKDB_200
        pin = sequential ? handle.Read(size) : handle.Read(size, offset);
        bytes.resize(size);
        pin.CopyTo(reinterpret_cast<duckdb::data_ptr_t>(bytes.data()), size);
#else
        duckdb::data_ptr_t pointer = nullptr;
        pin = sequential ? handle.Read(pointer, size) : handle.Read(pointer, size, offset);
        bytes.assign(reinterpret_cast<const char *>(pointer), size);
#endif
    }
    bool is(char value, size_t size) const { return bytes == std::string(size, value); }
};
}

TEST_CASE("Codex cache identity: old insertion cannot poison current or stable reads") {
    CacheFixture f;
    auto old = f.open();
    f.fs.publish('B');
    auto current = f.open();
    CacheRead a(*old, 4096, 0);
    CacheRead b(*current, 4096, 0);
    auto stable = f.open();
    CacheRead c(*stable, 4096, 0);
    CHECK(a.is('A', 4096));
    CHECK(b.is('B', 4096));
    CHECK(c.is('B', 4096));
}

TEST_CASE("Codex cache identity: stale full hits and overlaps retain their own source") {
    CacheFixture f;
    auto old = f.open();
    f.fs.publish('B');
    auto current = f.open();
    SUBCASE("full") {
        CacheRead warm(*current, 4096, 0);
        CHECK(CacheRead(*old, 4096, 0).is('A', 4096));
    }
    SUBCASE("partial") {
        CacheRead warm(*current, 512, 1024);
        CHECK(CacheRead(*old, 2048, 0).is('A', 2048));
    }
    SUBCASE("sequential") {
        CacheRead warm(*current, 4096, 0);
        CHECK(CacheRead(*old, 2048, 0, true).is('A', 2048));
    }
    CHECK(CacheRead(*current, 4096, 0).is('B', 4096));
}

TEST_CASE("Codex cache identity: invalidate while a read is in flight") {
    CacheFixture f;
    auto old = f.open();
    CacheRead warm(*old, 512, 0);
    duckdb::unique_ptr<duckdb::CachingFileHandle> current;
    std::unique_ptr<CacheRead> pinned;
    bool toggled = false;
    SUBCASE("content generation") {}
    SUBCASE("global generation") { toggled = true; }
    f.fs.before_read = [&] {
        f.fs.publish('B');
        if (toggled) {
            std::string error;
            REQUIRE(f.session.exec("SET enable_external_file_cache=false", &error));
            REQUIRE(f.session.exec("SET enable_external_file_cache=true", &error));
        }
        current = f.open();
        pinned = std::make_unique<CacheRead>(*current, 4096, 0);
    };
    CHECK(CacheRead(*old, 8192, 0).is('A', 8192));
    REQUIRE(current);
    CHECK(CacheRead(*current, 4096, 0).is('B', 4096));
    CHECK(CacheRead(*old, 4096, 0).is('A', 4096));
    auto stable = f.open();
    CHECK(CacheRead(*stable, 4096, 0).is('B', 4096));
}

TEST_CASE("Codex cache identity: lazy hits avoid IO and a lazy miss never mixes sources") {
    CacheFixture f;
    auto first = f.open();
    CacheRead warm(*first, 512, 0);
    const int opens = f.fs.opens, reads = f.fs.reads;
    auto lazy = f.open(false);
    CHECK(f.fs.opens == opens);
    CHECK(CacheRead(*lazy, 256, 0).is('A', 256));
    CHECK(f.fs.opens == opens);
    CHECK(f.fs.reads == reads);
    f.fs.publish('B', 65536);
    SUBCASE("positional miss") { CHECK(CacheRead(*lazy, 8192, 0).is('B', 8192)); }
    SUBCASE("sequential miss") { CHECK(CacheRead(*lazy, 8192, 0, true).is('B', 8192)); }
    CHECK(lazy->GetFileSize() == 65536);
    auto stable = f.open();
    CHECK(CacheRead(*stable, 4096, 0).is('B', 4096));
}

TEST_CASE("Codex cache identity: stable handles really reuse the cache") {
    CacheFixture f;
    auto first = f.open();
    CacheRead warm(*first, 4096, 0);
    const int reads = f.fs.reads;
    REQUIRE(reads > 0);
    for (int i = 0; i < 20; ++i) {
        auto next = f.open();
        CHECK(CacheRead(*next, 1024, 1024).is('A', 1024));
    }
    CHECK(f.fs.reads == reads);
}

TEST_CASE("Codex cache identity: sequential cache hits preserve position across toggles") {
    CacheFixture f;
    SUBCASE("seekable") {}
    SUBCASE("stream") { f.fs.seekable = false; }
    for (size_t i = 0; i < f.fs.current->bytes.size(); ++i)
        f.fs.current->bytes[i] = static_cast<char>('a' + (i / 512) % 26);
    auto first = f.open();
    CacheRead warm(*first, 4096, 0);
    auto next = f.open();
    CHECK(CacheRead(*next, 512, 0, true, false).is('a', 512));
    std::string error;
    REQUIRE(f.session.exec("SET enable_external_file_cache=false", &error));
    REQUIRE(f.session.exec("SET enable_external_file_cache=true", &error));
    CHECK(CacheRead(*next, 512, 0, true, false).is('b', 512));
    CHECK(CacheRead(*next, 512, 0, true, false).is('c', 512));
    CHECK(next->SeekPosition() == 1536);
}

TEST_CASE("Codex cache identity: recent mtime without version tags still isolates invalidations") {
    CacheFixture f;
    f.fs.publish('A', 32768, false);
    auto old = f.open();
    f.fs.publish('B', 32768, false);
    auto current = f.open();
    CacheRead warm(*current, 4096, 0);
    CHECK(CacheRead(*old, 4096, 0).is('A', 4096));
    CHECK(CacheRead(*current, 4096, 0).is('B', 4096));
}

#ifdef PARQIT_DUCKDB_200
TEST_CASE("Codex cache identity: two consumers cannot share a lazy replacement block") {
    class GatedFS : public CacheTestFS {
    public:
        std::mutex mutex;
        std::condition_variable condition;
        std::atomic<bool> gate_next{false};
        bool entered = false, released = false;
        duckdb::unique_ptr<duckdb::FileHandle> OpenFile(const std::string &path, duckdb::FileOpenFlags flags,
                duckdb::optional_ptr<duckdb::FileOpener> opener = nullptr) override {
            auto handle = CacheTestFS::OpenFile(path, flags, opener);
            if (gate_next.exchange(false)) {
                std::unique_lock<std::mutex> lock(mutex);
                entered = true;
                condition.notify_all();
                if (!condition.wait_for(lock, std::chrono::seconds(10), [&] { return released; }))
                    throw duckdb::IOException("cache test open gate timed out");
            }
            return handle;
        }
    } fs;
    CacheFixture f;
    auto &context = *reinterpret_cast<duckdb::Connection *>(f.session.con())->context;
    f.cache = std::make_unique<duckdb::CachingFileSystem>(fs, *context.db);
    fs.publish('A');
    auto old = f.open();
    CacheRead warm(*old, 512, 8192);
    auto lazy = f.open(false);
    REQUIRE(fs.opens == 1);
    fs.publish('B');
    fs.gate_next = true;
    auto read_lazy = std::async(std::launch::async, [&] { return CacheRead(*lazy, 4096, 0).is('B', 4096); });
    {
        std::unique_lock<std::mutex> lock(fs.mutex);
        REQUIRE(fs.condition.wait_for(lock, std::chrono::seconds(5), [&] { return fs.entered; }));
    }
    auto &cache = duckdb::ExternalFileCache::Get(*context.db);
    auto entry = cache.GetOrCreateCachedFile("parqit-cache-identity.parquet");
    duckdb::shared_ptr<duckdb::CacheBlock> block;
    {
        duckdb::annotated_lock_guard<duckdb::annotated_mutex> lock(entry->map_lock);
        block = entry->blocks.at(0);
    }
    const auto references = block.use_count();
    auto read_old = std::async(std::launch::async, [&] { return CacheRead(*old, 4096, 0).is('A', 4096); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    // Observe the second reader's acquired block and fetch task before releasing the first opener.
    while (block.use_count() < references + 2 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    const bool both_acquired = block.use_count() >= references + 2;
    {
        std::lock_guard<std::mutex> lock(fs.mutex);
        fs.released = true;
    }
    fs.condition.notify_all();
    CHECK(both_acquired);
    CHECK(read_lazy.get());
    CHECK(read_old.get());
    auto stable = f.open();
    CHECK(CacheRead(*stable, 4096, 0).is('B', 4096));
}

TEST_CASE("Codex cache identity: a recent mtime can resume cache reuse after ageing") {
    CacheFixture f;
    f.fs.publish('A', 32768, false);
    const auto modified = duckdb::Timestamp::GetCurrentTimestamp().value - 9000000;
    f.fs.current->modified = duckdb::timestamp_t(modified);
    auto handle = f.open();
    auto &context = *reinterpret_cast<duckdb::Connection *>(f.session.con())->context;
    auto &cache = duckdb::ExternalFileCache::Get(*context.db);
    auto entry = cache.GetOrCreateCachedFile("parqit-cache-identity.parquet");
    const auto generation = cache.GetContentGeneration(*entry);
    CacheRead first(*handle, 4096, 0);
    REQUIRE(first.is('A', 4096));
    REQUIRE(cache.GetContentGeneration(*entry) > generation);
    // Age a metadata validator, not a race: the engine deliberately distrusts the last ten seconds.
    while (duckdb::Timestamp::GetCurrentTimestamp().value - modified <= 10100000)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CacheRead warm(*handle, 4096, 0);
    CHECK(warm.is('A', 4096));
    const int reads = f.fs.reads;
    CHECK(CacheRead(*handle, 4096, 0).is('A', 4096));
    CHECK(f.fs.reads == reads);
}

TEST_CASE("Codex cache identity: response expiry and no-store still prevent cache reuse") {
    CacheFixture f;
    f.fs.current->tag.clear();
    f.fs.current->modified = duckdb::timestamp_t(0);
    f.fs.current->deadline = duckdb::timestamp_t::infinity();
    auto handle = f.open();
    CacheRead warm(*handle, 4096, 0);
    const int cached_reads = f.fs.reads;
    CHECK(CacheRead(*handle, 4096, 0).is('A', 4096));
    CHECK(f.fs.reads == cached_reads);
    SUBCASE("expired") { f.fs.current->deadline = duckdb::timestamp_t(1); }
    SUBCASE("no-store") { f.fs.current->deadline = duckdb::timestamp_t::ninfinity(); }
    CHECK(CacheRead(*handle, 4096, 4096).is('A', 4096));
    const int expired_reads = f.fs.reads;
    CHECK(CacheRead(*handle, 4096, 0).is('A', 4096));
    CHECK(f.fs.reads > expired_reads);
    const int next_reads = f.fs.reads;
    CHECK(CacheRead(*handle, 4096, 0).is('A', 4096));
    CHECK(f.fs.reads > next_reads);
}
#endif
