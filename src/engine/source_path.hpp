/* Local Parquet paths retain their Hive context when cwd later changes. */
#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace parqit {

struct ParquetSourcePath {
    std::string path;          /* unescaped absolute local path, or original URI */
    std::string prefix;        /* known literal cwd prefix, including separator */
    std::string tail;          /* caller's remaining path/glob components */
    bool anchored = false;
};

class ParquetPathResolver {
public:
    ParquetPathResolver() = default;
    explicit ParquetPathResolver(const std::filesystem::path &cwd);
    ParquetSourcePath resolve(std::string input);

private:
    std::optional<std::filesystem::path> cwd_;
};

} // namespace parqit
