#include "engine/source_path.hpp"

#include <stdexcept>
#include <utility>

namespace parqit {
namespace {

bool source_uri(const std::string &s) {
    /* DuckDB also accepts the single-slash file:/ form. */
    if (s.compare(0, 6, "file:/") == 0) return true;
    auto alpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    if (s.empty() || !alpha(s[0])) return false;
    size_t i = 1;
    while (i < s.size() && (alpha(s[i]) || (s[i] >= '0' && s[i] <= '9') ||
                           s[i] == '+' || s[i] == '-' || s[i] == '.')) ++i;
    return s.compare(i, 3, "://") == 0;
}

} // namespace

ParquetPathResolver::ParquetPathResolver(const std::filesystem::path &cwd) : cwd_(cwd) {
    if (!cwd.is_absolute()) throw std::invalid_argument("Parquet source cwd must be absolute");
}

ParquetSourcePath ParquetPathResolver::resolve(std::string input) {
    namespace fs = std::filesystem;
    /* DuckDB expands every leading '~' through its home_directory setting.
     * Keep that native policy, just as URI schemes keep their own resolver. */
    if (input.empty() || input[0] == '~' || source_uri(input))
        return {std::move(input), {}, {}, false};
    const fs::path p = fs::u8path(input);
    if (p.is_absolute()) return {std::move(input), {}, {}, false};
    if (!cwd_) cwd_ = fs::current_path();
    fs::path anchor = *cwd_;
#ifdef _WIN32
    /* A drive-relative path uses that drive's cwd; a root-relative one uses
     * the current drive's root. Do not treat either spelling as a URI. */
    if (p.has_root_name() && p.root_name() != anchor.root_name()) {
        anchor = fs::absolute(p.root_name() / ".");
        if (anchor.filename() == ".") anchor = anchor.parent_path();
    }
    if (p.has_root_directory()) anchor = anchor.root_path();
#endif
    fs::path tail;
    bool leading = true;
    for (const auto &component : p.relative_path()) {
        if (leading && component == ".") continue;
        if (leading && component == "..") {
            if (anchor.has_relative_path()) anchor = anchor.parent_path();
            continue;
        }
        leading = false;
        tail /= component;
    }
    /* Only leading dot/parent components are consumed against the physical
     * cwd. Leave link/.. and every later component for the OS to resolve;
     * lexical normalisation or canonicalisation could select another file
     * or discard keys deliberately carried by a logical symlink path. */
    std::string prefix = anchor.u8string();
    if (!prefix.empty() && prefix.back() != '/' && prefix.back() != '\\')
        prefix.push_back(static_cast<char>(fs::path::preferred_separator));
    return {tail.empty() ? anchor.u8string() : (anchor / tail).u8string(),
            std::move(prefix), tail.u8string(), true};
}

} // namespace parqit
