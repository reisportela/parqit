#include "engine/sanitize.hpp"

#include <map>
#include <set>

#include "utf8proc.hpp"

namespace parqit {

/* NAME-STR-1 (audit 2026-08-22, A1-4): plain `str` is NOT a reserved word —
 * native `gen int str = 1` is rc 0 (StataNow 19.5); only `strL` and the
 * `str#` family are. Reserving it renamed a legal variable to `_str` on
 * every read of a parqit-written file. */
static const std::set<std::string> kReserved = {
    "_all", "_b",   "byte", "_coef", "_cons",  "double", "float", "if",
    "in",   "int",  "long", "_n",    "_N",     "_pi",    "_pred", "_rc",
    "_se",  "_skip", "strL", "using",  "with"};

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

static bool utf8_codepoints(const std::string &s,
                            std::vector<utf8proc_int32_t> *out) {
    size_t pos = 0;
    while (pos < s.size()) {
        utf8proc_int32_t cp = 0;
        const auto used = duckdb::utf8proc_iterate(
            reinterpret_cast<const utf8proc_uint8_t *>(s.data() + pos),
            static_cast<utf8proc_ssize_t>(s.size() - pos), &cp);
        if (used <= 0) return false;
        out->push_back(cp);
        pos += static_cast<size_t>(used);
    }
    return true;
}

bool glob_match(const std::string &pattern, const std::string &name) {
    std::vector<utf8proc_int32_t> pat, text;
    if (!utf8_codepoints(pattern, &pat) || !utf8_codepoints(name, &text))
        return false;

    size_t p = 0, n = 0, star = std::string::npos, mark = 0;
    while (n < text.size()) {
        if (p < pat.size() && (pat[p] == '?' || pat[p] == text[n])) {
            p++;
            n++;
        } else if (p < pat.size() && pat[p] == '*') {
            star = p++;
            mark = n;
        } else if (star != std::string::npos) {
            p = star + 1;
            n = ++mark;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*') p++;
    return p == pat.size();
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

std::string ascii_lower(const std::string &s) {
    std::string out = s;
    for (auto &ch : out)
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    return out;
}

std::vector<std::string> engine_unique_ci(const std::vector<std::string> &names,
                                          const std::set<std::string> &taken_lower) {
    std::set<std::string> all_lower; /* every ORIGINAL name, folded */
    for (const auto &n : names) all_lower.insert(ascii_lower(n));
    std::set<std::string> seen; /* aliases handed out so far, folded */
    std::vector<std::string> out(names.size());
    for (size_t i = 0; i < names.size(); i++) {
        const std::string low = ascii_lower(names[i]);
        if (!seen.count(low) && !taken_lower.count(low)) {
            out[i] = names[i];
            seen.insert(low);
            continue;
        }
        for (int k = 1;; k++) {
            const std::string cand = names[i] + "_" + std::to_string(k);
            const std::string cl = ascii_lower(cand);
            if (!all_lower.count(cl) && !seen.count(cl) && !taken_lower.count(cl)) {
                out[i] = cand;
                seen.insert(cl);
                break;
            }
        }
    }
    return out;
}

/* ---- DuckDB naming-rule replicas (verified against the fetched v1.5.3
 * source; see sanitize.hpp) --------------------------------------------- */

std::vector<std::string> duckdb_reader_dedup(const std::vector<std::string> &leaves) {
    /* parquet_reader.cpp ParseSchemaRecursive:
     *   case_insensitive_map_t<idx_t> name_collision_count;
     *   for each child: while (map contains col_name) {
     *       map[col_name] += 1; col_name = col_name + "_" + to_string(map[col_name]); }
     *   child.name = col_name; map[col_name] = 0;
     * The counter bumped is the one of the CURRENT (possibly already
     * suffixed) candidate's case-insensitive key. */
    std::map<std::string, long long> counts; /* keyed by ascii_lower(name) */
    std::vector<std::string> out;
    out.reserve(leaves.size());
    for (const auto &leaf : leaves) {
        std::string name = leaf;
        for (;;) {
            auto it = counts.find(ascii_lower(name));
            if (it == counts.end()) break;
            it->second += 1;
            name = name + "_" + std::to_string(it->second);
        }
        counts[ascii_lower(name)] = 0;
        out.push_back(name);
    }
    return out;
}

UnionByNamePlan duckdb_union_by_name(const std::vector<std::vector<std::string>> &per_file) {
    /* multi_file_reader.cpp UnionByName::CombineUnionTypes, applied to the
     * readers in file order with one case-insensitive map across all files */
    UnionByNamePlan plan;
    std::map<std::string, size_t> union_index; /* ascii_lower(name) -> column */
    plan.member_of.resize(per_file.size());
    for (size_t f = 0; f < per_file.size(); f++) {
        plan.member_of[f].resize(per_file[f].size());
        for (size_t c = 0; c < per_file[f].size(); c++) {
            const std::string low = ascii_lower(per_file[f][c]);
            auto it = union_index.find(low);
            if (it == union_index.end()) {
                union_index[low] = plan.names.size();
                plan.member_of[f][c] = plan.names.size();
                plan.names.push_back(per_file[f][c]);
                plan.owner.emplace_back(f, c);
            } else {
                plan.member_of[f][c] = it->second;
            }
        }
    }
    return plan;
}

std::vector<std::string> duckdb_hive_keys(const std::string &path) {
    /* hive_partitioning.cpp HivePartitioning::Parse, key order preserved
     * (DuckDB stores them in a std::map, i.e. sorted; the SET of keys is what
     * callers compare, so order is immaterial) */
    std::vector<std::string> keys;
    std::set<std::string> seen; /* DuckDB's std::map::insert keeps the first */
    size_t partition_start = 0, equality_sign = 0;
    bool candidate = true;
    for (size_t c = 0; c < path.size(); c++) {
        const char ch = path[c];
        if (ch == '?' || ch == '\n') candidate = false;
        if (ch == '\\' || ch == '/') {
            if (candidate && equality_sign > partition_start) {
                const std::string key =
                    path.substr(partition_start, equality_sign - partition_start);
                if (seen.insert(key).second) keys.push_back(key);
            }
            partition_start = c + 1;
            candidate = true;
        } else if (ch == '=') {
            if (equality_sign > partition_start) candidate = false; /* two '=' */
            equality_sign = c;
        }
    }
    return keys;
}

std::string duckdb_empty_column_name(size_t index_0based) {
    return "C" + std::to_string(index_0based);
}

} // namespace parqit
