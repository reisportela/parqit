#include "doctest.h"
#include "engine/source_path.hpp"

#include <filesystem>
#include <stdexcept>

namespace fs = std::filesystem;
using parqit::ParquetPathResolver;

static fs::path source_base() {
    return fs::temp_directory_path() / fs::u8path("parqit paths[1]") / "year=2020" / "month=1";
}

TEST_CASE("PARQUET-PATH-1: relative files, directories and globs keep cwd Hive keys") {
    const auto base = source_base();
    ParquetPathResolver resolver(base);
    for (const std::string input : {"part-000.parquet", "./part-000.parquet"}) {
        const auto p = resolver.resolve(input);
        CHECK(p.anchored);
        CHECK(p.path == (base / "part-000.parquet").u8string());
        CHECK(p.prefix == (base.u8string() + static_cast<char>(fs::path::preferred_separator)));
        CHECK(p.tail == "part-000.parquet");
    }
    CHECK(resolver.resolve(".").path == base.u8string());
    CHECK(resolver.resolve("../month=1/part-000.parquet").path ==
          (base.parent_path() / "month=1" / "part-000.parquet").u8string());
    CHECK(resolver.resolve("../../part-000.parquet").path ==
          (base.parent_path().parent_path() / "part-000.parquet").u8string());
    const auto glob = resolver.resolve("*.parquet");
    CHECK(glob.path == (base / "*.parquet").u8string());
    CHECK(glob.tail == "*.parquet");
    CHECK(glob.prefix.find("year=2020") != std::string::npos);
}

TEST_CASE("PARQUET-PATH-1: literal cwd bytes stay separate from a live glob") {
    const auto base = fs::temp_directory_path() / fs::u8path("á literal*?[1] space");
    ParquetPathResolver resolver(base);
    const std::string input = "year=*/month=?/part[1].parquet";
    const auto p = resolver.resolve(input);
    CHECK(p.anchored);
    CHECK(p.prefix == base.u8string() + static_cast<char>(fs::path::preferred_separator));
    CHECK(p.tail == fs::u8path(input).u8string());
    CHECK(p.path == (base / fs::u8path(input)).u8string());
    const auto physical = resolver.resolve("link/../parent.parquet");
    CHECK(physical.path == (base / "link" / ".." / "parent.parquet").u8string());
    CHECK(physical.tail == (fs::path("link") / ".." / "parent.parquet").u8string());
}

TEST_CASE("PARQUET-PATH-1: absolute paths and URI schemes retain their spelling") {
    ParquetPathResolver resolver(source_base());
    const std::string absolute = (source_base() / "link" / ".." / "part.parquet").u8string();
    for (const auto &input : {absolute, std::string("https://host/year=2020/part.parquet"),
                             std::string("s3://bucket/year=*/part?.parquet"),
                             std::string("hf://datasets/name/data.parquet"),
                             std::string("file:///data/year=2020/part.parquet"),
                             std::string("file:/data/year=2020/part.parquet"),
                             std::string("file://localhost/data/year=2020/part.parquet"),
                             std::string("~"), std::string("~/year=2020/part.parquet"),
                             std::string("~\\year=2020\\part.parquet"),
                             std::string("~suffix/part.parquet"), std::string()}) {
        const auto p = resolver.resolve(input);
        CHECK_FALSE(p.anchored);
        CHECK(p.path == input);
        CHECK(p.prefix.empty());
        CHECK(p.tail.empty());
    }
}

TEST_CASE("PARQUET-PATH-1: each relative source uses the same cached cwd") {
    const auto base = source_base();
    ParquetPathResolver resolver(base);
    CHECK(resolver.resolve("../other.parquet").path == (base.parent_path() / "other.parquet").u8string());
    CHECK(resolver.resolve("part.parquet").path == (base / "part.parquet").u8string());
    CHECK(resolver.resolve("./.././month=1/part.parquet").path ==
          (base.parent_path() / "month=1" / "part.parquet").u8string());
    CHECK_THROWS_AS(ParquetPathResolver(fs::path("relative")), std::invalid_argument);
#ifdef _WIN32
    CHECK(resolver.resolve(base.root_name().u8string() + "part.parquet").path ==
          (base / "part.parquet").u8string());
    CHECK(resolver.resolve("\\part.parquet").path == (base.root_path() / "part.parquet").u8string());
#endif
}
