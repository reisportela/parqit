#include "engine/sanitize.hpp"

#include <set>

#include "utf8proc.hpp"

namespace parqit {

static const std::set<std::string> kReserved = {
    "_all", "_b",   "byte", "_coef", "_cons",  "double", "float", "if",
    "in",   "int",  "long", "_n",    "_N",     "_pi",    "_pred", "_rc",
    "_se",  "_skip", "str", "strL",  "using",  "with"};

bool is_reserved_stata_name(const std::string &name) {
    if (kReserved.count(name)) return true;
    /* str# (str1..str2045) is reserved as a family */
    if (name.size() > 3 && name.compare(0, 3, "str") == 0) {
        bool digits = true;
        for (size_t i = 3; i < name.size(); i++)
            if (name[i] < '0' || name[i] > '9') { digits = false; break; }
        if (digits) return true;
    }
    return false;
}

static bool unicode_letter(utf8proc_int32_t cp) {
    const auto cat = duckdb::utf8proc_category(cp);
    return cat >= duckdb::UTF8PROC_CATEGORY_LU &&
           cat <= duckdb::UTF8PROC_CATEGORY_LO;
}

static bool unicode_name_continue(utf8proc_int32_t cp) {
    const auto cat = duckdb::utf8proc_category(cp);
    return unicode_letter(cp) || cat == duckdb::UTF8PROC_CATEGORY_ND ||
           cat == duckdb::UTF8PROC_CATEGORY_NL ||
           cat == duckdb::UTF8PROC_CATEGORY_MN ||
           cat == duckdb::UTF8PROC_CATEGORY_MC ||
           cat == duckdb::UTF8PROC_CATEGORY_ME;
}

/* Truncate by Unicode code points, which is how Stata applies its 32-character
 * name ceiling. Invalid UTF-8 is stopped before it can leak into a name. */
static std::string utf8_truncate_chars(const std::string &s, size_t n) {
    size_t pos = 0, count = 0;
    while (pos < s.size() && count < n) {
        utf8proc_int32_t cp = 0;
        const auto used = duckdb::utf8proc_iterate(
            reinterpret_cast<const utf8proc_uint8_t *>(s.data() + pos),
            static_cast<utf8proc_ssize_t>(s.size() - pos), &cp);
        if (used <= 0) break;
        pos += static_cast<size_t>(used);
        count++;
    }
    return s.substr(0, pos);
}

std::string sanitize_stata_name(const std::string &source, size_t position_1based) {
    std::string out;
    out.reserve(source.size());
    size_t pos = 0, nchars = 0;
    while (pos < source.size() && nchars < 32) {
        utf8proc_int32_t cp = 0;
        const auto used = duckdb::utf8proc_iterate(
            reinterpret_cast<const utf8proc_uint8_t *>(source.data() + pos),
            static_cast<utf8proc_ssize_t>(source.size() - pos), &cp);
        if (used <= 0) {
            out.push_back('_');
            pos++;
            nchars++;
            continue;
        }
        const auto cat = duckdb::utf8proc_category(cp);
        const bool leading_digit =
            nchars == 0 && (cat == duckdb::UTF8PROC_CATEGORY_ND ||
                            cat == duckdb::UTF8PROC_CATEGORY_NL);
        const bool ok = (cp == '_') ||
                        (nchars == 0 ? unicode_letter(cp)
                                     : unicode_name_continue(cp));
        if (leading_digit) {
            out.push_back('_');
            out.append(source, pos, static_cast<size_t>(used));
        } else if (ok)
            out.append(source, pos, static_cast<size_t>(used));
        else
            out.push_back('_');
        pos += static_cast<size_t>(used);
        nchars++;
    }
    if (out.empty()) return "v" + std::to_string(position_1based);
    if (is_reserved_stata_name(out))
        out.insert(out.begin(), '_');
    return utf8_truncate_chars(out, 32);
}

std::vector<std::string> sanitize_unique(const std::vector<std::string> &sources,
                                         std::vector<bool> *renamed) {
    std::vector<std::string> out(sources.size());
    if (renamed) renamed->assign(sources.size(), false);
    std::set<std::string> taken;
    for (size_t i = 0; i < sources.size(); i++) {
        std::string cand = sanitize_stata_name(sources[i], i + 1);
        if (taken.count(cand)) {
            /* numbered suffix, deterministic; keep within 32 characters */
            for (int k = 2;; k++) {
                std::string suffix = "_" + std::to_string(k);
                std::string base =
                    utf8_truncate_chars(cand, 32 - suffix.size());
                std::string trial = base + suffix;
                if (!taken.count(trial)) { cand = trial; break; }
            }
        }
        taken.insert(cand);
        out[i] = cand;
        if (renamed) (*renamed)[i] = (cand != sources[i]);
    }
    return out;
}

} // namespace parqit
