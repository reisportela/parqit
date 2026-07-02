#include "engine/sanitize.hpp"

#include <set>

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

static bool ascii_alpha(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static bool ascii_digit(unsigned char c) { return c >= '0' && c <= '9'; }

/* truncate to at most n bytes without splitting a UTF-8 sequence */
static std::string utf8_truncate(const std::string &s, size_t n) {
    if (s.size() <= n) return s;
    size_t end = n;
    while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) end--;
    return s.substr(0, end);
}

std::string sanitize_stata_name(const std::string &source, size_t position_1based) {
    std::string out;
    out.reserve(source.size());
    for (unsigned char c : source) {
        if (ascii_alpha(c) || ascii_digit(c) || c == '_' || c >= 0x80)
            out.push_back(static_cast<char>(c));
        else
            out.push_back('_');
    }
    if (out.empty()) return "v" + std::to_string(position_1based);
    if (ascii_digit(static_cast<unsigned char>(out[0])))
        out.insert(out.begin(), '_');
    else if (is_reserved_stata_name(out))
        out.insert(out.begin(), '_');
    return utf8_truncate(out, 32);
}

std::vector<std::string> sanitize_unique(const std::vector<std::string> &sources,
                                         std::vector<bool> *renamed) {
    std::vector<std::string> out(sources.size());
    if (renamed) renamed->assign(sources.size(), false);
    std::set<std::string> taken;
    for (size_t i = 0; i < sources.size(); i++) {
        std::string cand = sanitize_stata_name(sources[i], i + 1);
        if (taken.count(cand)) {
            /* numbered suffix, deterministic; keep within 32 bytes */
            for (int k = 2;; k++) {
                std::string suffix = "_" + std::to_string(k);
                std::string base = utf8_truncate(cand, 32 - suffix.size());
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
