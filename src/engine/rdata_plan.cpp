#include "engine/rdata_plan.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <locale>
#include <map>
#include <set>
#include <sstream>

#include "json.hpp"

#include "engine/legacy_encoding.hpp"
#include "engine/sanitize.hpp"
#include "engine/session.hpp" /* quote_literal */
#include "engine/typemap.hpp" /* kXmissingMetaKey, xmissing_companion_name, integer_type_for_range */

namespace parqit {
namespace rdata {

using nlohmann::json;

namespace {

/* the shortest decimal text that reads back as the same double (98, 97.5,
 * 0.1), locale-independent */
std::string short_double(double v) {
    if (std::isnan(v)) return "NaN";
    if (std::isinf(v)) return v > 0 ? "Inf" : "-Inf";
    if (v == std::trunc(v) && std::fabs(v) < 9007199254740992.0) {
        std::ostringstream os;
        os.imbue(std::locale::classic());
        os << static_cast<long long>(v);
        return os.str();
    }
    for (int prec = 1; prec <= 17; prec++) {
        std::ostringstream os;
        os.imbue(std::locale::classic());
        os.precision(prec);
        os << v;
        std::istringstream is(os.str());
        is.imbue(std::locale::classic());
        double back = 0.0;
        is >> back;
        if (back == v) return os.str();
    }
    std::ostringstream os;
    os.imbue(std::locale::classic());
    os.precision(17);
    os << v;
    return os.str();
}

/* every text reaching the metadata is valid UTF-8 already; replacing (not
 * throwing on) an invalid sequence is the safety net */
std::string jdump(const json &j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

size_t utf8_chars(const std::string &s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) n++;
    return n;
}

std::string list_names(const std::vector<std::string> &names, size_t max = 8) {
    std::string out;
    for (size_t i = 0; i < names.size() && i < max; i++) out += (i ? " " : "") + names[i];
    if (names.size() > max) out += " and " + std::to_string(names.size() - max) + " more";
    return out;
}

std::string list_items(const std::vector<std::string> &items, size_t max = 8) {
    std::string out;
    for (size_t i = 0; i < items.size() && i < max; i++) out += (i ? "; " : "") + items[i];
    if (items.size() > max) out += "; and " + std::to_string(items.size() - max) + " more";
    return out;
}

const RValue *attr_of(const Attrs &a, const std::string &name) {
    for (const auto &kv : a)
        if (kv.first == name) return kv.second.get();
    return nullptr;
}

std::vector<std::string> class_list(const Attrs &a) {
    std::vector<std::string> out;
    const RValue *c = attr_of(a, "class");
    if (c && c->type == STRSXP && c->kept())
        for (size_t i = 0; i < c->strs.size(); i++)
            if (!c->na[i]) out.push_back(c->strs[i]);
    return out;
}

bool has_class(const Attrs &a, const std::string &cls) {
    for (const auto &c : class_list(a))
        if (c == cls) return true;
    return false;
}

std::string joined(const std::vector<std::string> &v) {
    std::string out;
    for (const auto &s : v) out += (out.empty() ? "" : " ") + s;
    return out;
}

/* a character attribute's first element */
std::string text_attr(const Attrs &a, const std::string &name) {
    const RValue *v = attr_of(a, name);
    return v ? v->str1() : std::string();
}

/* the numbers of a numeric attribute (NA as NaN) */
std::vector<double> numbers_of(const RValue *v) {
    std::vector<double> out;
    if (!v || !v->kept()) return out;
    if (v->type == INTSXP || v->type == LGLSXP)
        for (int32_t x : v->ints) out.push_back(x == kNaInteger ? std::nan("") : double(x));
    else if (v->type == REALSXP)
        out = v->reals;
    return out;
}

inline int64_t int64_bits(double x) {
    int64_t v;
    std::memcpy(&v, &x, 8);
    return v;
}

/* ---------------------------------------------------- attributes as JSON */

std::string hex_of(const std::string &raw) {
    static const char *d = "0123456789abcdef";
    std::string out;
    for (unsigned char c : raw) {
        out += d[c >> 4];
        out += d[c & 15];
    }
    return out;
}

constexpr uint64_t kJsonMaxElts = 1000;

json value_json(const RValue &v, int depth);

json real_json(double x) {
    if (is_na_real(x)) return json(nullptr);
    if (!std::isfinite(x)) return json(short_double(x));
    return json(x);
}

json element_json(const RValue &v, size_t i, bool int64) {
    switch (v.type) {
    case LGLSXP:
        return v.ints[i] == kNaInteger ? json(nullptr) : json(v.ints[i] != 0);
    case INTSXP:
        return v.ints[i] == kNaInteger ? json(nullptr) : json(v.ints[i]);
    case REALSXP: {
        const double x = v.reals[i];
        if (int64) {
            const int64_t b = int64_bits(x);
            return b == INT64_MIN ? json(nullptr) : json(std::to_string(b));
        }
        if (is_na_real(x)) {
            const char t = na_tag(x);
            return t >= 'a' && t <= 'z' ? json(std::string("NA(") + t + ")") : json(nullptr);
        }
        if (!std::isfinite(x)) return json(short_double(x));
        return json(x);
    }
    case CPLXSXP:
        return json::array({real_json(v.reals[2 * i]), real_json(v.reals[2 * i + 1])});
    case STRSXP:
        return v.na[i] ? json(nullptr) : json(v.strs[i]);
    default:
        return value_json(*v.elts[i], 0);
    }
}

json value_json(const RValue &v, int depth) {
    if (!v.kept()) return json(v.what);
    if (depth > 20) return json("(nested too deeply)");
    json base;
    const bool factor = v.type == INTSXP && v.inherits("factor");
    const bool int64 = v.type == REALSXP && v.inherits("integer64");
    size_t n = 0;
    switch (v.type) {
    case NILSXP: return json(nullptr);
    case SYMSXP: return json(v.sym);
    case LGLSXP: case INTSXP: n = v.ints.size(); break;
    case REALSXP: n = v.reals.size(); break;
    case CPLXSXP: n = v.reals.size() / 2; break;
    case STRSXP: n = v.strs.size(); break;
    case RAWSXP: base = json(hex_of(v.raw)); break;
    case VECSXP: case EXPRSXP: n = v.elts.size(); break;
    case LISTSXP: {
        json o = json::object();
        bool named = true;
        std::set<std::string> seen;
        for (const auto &t : v.tags) named = named && !t.empty() && seen.insert(t).second;
        if (named) {
            for (size_t i = 0; i < v.elts.size(); i++) o[v.tags[i]] = value_json(*v.elts[i], depth + 1);
            return o;
        }
        json a = json::array();
        for (size_t i = 0; i < v.elts.size(); i++)
            a.push_back(json::array({v.tags[i], value_json(*v.elts[i], depth + 1)}));
        return a;
    }
    case OBJSXP: {
        json o = json::object();
        for (const auto &kv : v.attrs) o[kv.first] = value_json(*kv.second, depth + 1);
        return o;
    }
    default:
        return json("(an object of type " + std::to_string(v.type) + ")");
    }
    if (v.type != RAWSXP) {
        if (n > kJsonMaxElts) {
            base = json("(" + std::to_string(n) + " values; not kept)");
        } else {
            const RValue *levels = factor ? v.attr("levels") : nullptr;
            json arr = json::array();
            for (size_t i = 0; i < n; i++) {
                if (levels && levels->type == STRSXP && levels->kept()) {
                    const int32_t code = v.ints[i];
                    arr.push_back(code >= 1 && static_cast<size_t>(code) <= levels->strs.size()
                                      ? json(levels->strs[static_cast<size_t>(code) - 1])
                                      : json(nullptr));
                } else if (v.type == VECSXP || v.type == EXPRSXP) {
                    arr.push_back(value_json(*v.elts[i], depth + 1));
                } else {
                    arr.push_back(element_json(v, i, int64));
                }
            }
            const RValue *names = v.attr("names");
            bool named = names && names->type == STRSXP && names->kept() && names->strs.size() == n;
            if (named) {
                std::set<std::string> seen;
                bool unique = true;
                for (size_t i = 0; i < n && unique; i++)
                    unique = !names->na[i] && !names->strs[i].empty() && seen.insert(names->strs[i]).second;
                json out = unique ? json::object() : json::array();
                for (size_t i = 0; i < n; i++) {
                    if (unique) out[names->strs[i]] = arr[i];
                    else out.push_back(json::array({names->na[i] ? json(nullptr) : json(names->strs[i]), arr[i]}));
                }
                base = out;
            } else {
                base = n == 1 ? arr[0] : arr;
            }
        }
    }
    /* attributes other than names (and a factor's own levels and class) */
    json extra = json::object();
    for (const auto &kv : v.attrs) {
        if (kv.first == "names" || (factor && kv.first == "levels")) continue;
        extra[kv.first] = value_json(*kv.second, depth + 1);
    }
    if (extra.empty()) return base;
    return json{{"value", base}, {"attributes", extra}};
}

/* --------------------------------------------------------------- profile */

enum class Sem { Logical, Integer, Factor, Date, PosixCt, Hms, Difftime, Int64, Double, Character };

Sem semantic_of(const Column &c) {
    const int t = c.vec.type;
    if (t == LGLSXP) return Sem::Logical;
    if (t == STRSXP) return Sem::Character;
    const bool i = t == INTSXP;
    if (i && has_class(c.attrs, "factor")) return Sem::Factor;
    if (has_class(c.attrs, "Date")) return Sem::Date;
    if (has_class(c.attrs, "POSIXct")) return Sem::PosixCt;
    if (has_class(c.attrs, "ITime")) return Sem::Hms;
    if (has_class(c.attrs, "hms")) {
        const std::string u = text_attr(c.attrs, "units");
        return u.empty() || u == "secs" ? Sem::Hms : Sem::Difftime;
    }
    if (has_class(c.attrs, "difftime")) return Sem::Difftime;
    if (!i && has_class(c.attrs, "integer64")) return Sem::Int64;
    return i ? Sem::Integer : Sem::Double;
}

struct Profile {
    bool any = false;           /* a value other than NA was seen */
    int64_t imin = 0, imax = 0; /* integer kinds: their range */
    long long nonfinite = 0;    /* temporal doubles: NaN or ±Inf */
    bool whole_days = true, days_fit = true, us_fit = true, in_day = true;
    long long um_cells = 0;
    std::set<double> in_range;
    bool range_more = false;
    long long tagged = 0, bad_tags = 0;
    uint32_t tag_mask = 0;
    size_t max_bytes = 0;
    long long transcoded = 0;
    long long bad_codes = 0;
    std::vector<char> level_used;
    bool one_to_n = true;       /* integer row names: 1..n */
};

constexpr size_t kRangeKeep = 64;

void see_int(Profile &p, int64_t x) {
    if (!p.any) {
        p.imin = p.imax = x;
        p.any = true;
    } else {
        p.imin = std::min(p.imin, x);
        p.imax = std::max(p.imax, x);
    }
}

void see_user_missing(Profile &p, const ColumnSpec &s, double x) {
    p.um_cells++;
    if (!s.na_range || std::find(s.na_values.begin(), s.na_values.end(), x) != s.na_values.end()) return;
    if (p.in_range.size() < kRangeKeep || x < *p.in_range.rbegin()) {
        p.in_range.insert(x);
        if (p.in_range.size() > kRangeKeep) {
            p.in_range.erase(std::prev(p.in_range.end()));
            p.range_more = true;
        }
    } else if (!p.in_range.count(x)) {
        p.range_more = true;
    }
}

Profile profile_column(const Plan &plan, const ColumnSpec &s, Sem sem, size_t nlevels) {
    Profile p;
    const uint64_t n = s.vec.length;
    Cursor cur(plan.parsed.source, s.vec, plan.parsed.codec, size_t(1) << 20);
    std::string text;
    bool tr = false;
    if (sem == Sem::Factor) p.level_used.assign(nlevels + 1, 0);
    switch (s.vec.type) {
    case STRSXP:
        for (uint64_t i = 0; i < n; i++)
            if (cur.next_string(&text, &tr)) {
                p.max_bytes = std::max(p.max_bytes, text.size());
                if (tr) p.transcoded++;
            }
        break;
    case LGLSXP:
        break; /* nothing to learn */
    case INTSXP:
        for (uint64_t i = 0; i < n; i++) {
            const int32_t x = cur.next_int();
            if (x == kNaInteger) continue;
            if (s.user_missing(x)) {
                see_user_missing(p, s, x);
                continue;
            }
            see_int(p, x);
            if (sem == Sem::Factor) {
                if (x >= 1 && static_cast<size_t>(x) <= nlevels) p.level_used[static_cast<size_t>(x)] = 1;
                else p.bad_codes++;
            }
            if (sem == Sem::Hms && !(x >= 0 && x < 86400)) p.in_day = false;
            if (sem == Sem::Date) {
                int32_t days;
                int64_t us;
                if (!days_to_date(x, &days)) p.days_fit = false;
                if (!days_to_us(x, &us)) p.us_fit = false;
            }
        }
        break;
    case REALSXP:
        for (uint64_t i = 0; i < n; i++) {
            const double x = cur.next_real();
            if (sem == Sem::Int64) {
                const int64_t b = int64_bits(x);
                if (b != INT64_MIN) see_int(p, b);
                continue;
            }
            if (is_na_real(x)) {
                const char t = na_tag(x);
                if (t >= 'a' && t <= 'z') {
                    p.tagged++;
                    p.tag_mask |= uint32_t(1) << (t - 'a');
                } else if (t != 0) {
                    p.bad_tags++;
                }
                continue;
            }
            if (!std::isnan(x) && s.user_missing(x)) {
                see_user_missing(p, s, x);
                continue;
            }
            p.any = true;
            switch (sem) {
            case Sem::Date: {
                if (!std::isfinite(x)) {
                    p.nonfinite++;
                    break;
                }
                int32_t d;
                if (!days_to_date(x, &d)) {
                    if (x != std::trunc(x)) p.whole_days = false;
                    else p.days_fit = false;
                }
                int64_t us;
                if (!days_to_us(x, &us)) p.us_fit = false;
                break;
            }
            case Sem::PosixCt: {
                if (!std::isfinite(x)) {
                    p.nonfinite++;
                    break;
                }
                int64_t us;
                if (!seconds_to_us(x, &us)) p.us_fit = false;
                break;
            }
            case Sem::Hms: {
                int64_t us;
                if (!std::isfinite(x)) p.nonfinite++;
                else if (!seconds_to_time_us(x, &us)) p.in_day = false;
                break;
            }
            default:
                break;
            }
        }
        break;
    default:
        break;
    }
    return p;
}

/* a text in double quotes, an inner quote doubled (SPSS's own rule, which
 * parqit spssencode reads) */
std::string quoted(const std::string &s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\"\"";
        else out += c;
    }
    return out + "\"";
}

/* the SPSS MISSING VALUES of haven's na_values / na_range, R style */
std::string missing_text(const ColumnSpec &s, const RValue *na_values_str) {
    std::string out;
    if (na_values_str && na_values_str->kept() && na_values_str->type == STRSXP) {
        std::string v;
        for (size_t i = 0; i < na_values_str->strs.size(); i++) {
            if (na_values_str->na[i]) continue;
            v += (v.empty() ? "" : ", ") + quoted(na_values_str->strs[i]);
        }
        out = "na_values: " + v;
    } else if (!s.na_values.empty()) {
        std::string v;
        for (size_t i = 0; i < s.na_values.size(); i++) v += (i ? ", " : "") + short_double(s.na_values[i]);
        out = "na_values: " + v;
    }
    if (s.na_range)
        out += (out.empty() ? "" : "; ") + std::string("na_range: ") + short_double(s.na_lo) + " to " +
               short_double(s.na_hi);
    return out;
}

} // namespace

/* ---------------------------------------------------------- conversions */

bool days_to_date(double d, int32_t *days) {
    if (!std::isfinite(d) || d != std::trunc(d)) return false;
    /* DuckDB reserves the two endpoints for infinite dates. */
    if (!(d > -2147483647.0 && d < 2147483647.0)) return false;
    *days = static_cast<int32_t>(d);
    return true;
}

bool days_to_us(double d, int64_t *us) {
    if (!std::isfinite(d) || !(std::fabs(d) < 1.0e8)) return false;
    const double whole = std::floor(d);
    const double frac = d - whole; /* exact */
    *us = static_cast<int64_t>(whole) * 86400000000LL + std::llround(frac * 86400000000.0);
    return true;
}

bool seconds_to_us(double s, int64_t *us) {
    if (!std::isfinite(s) || !(std::fabs(s) < 9.2e12)) return false;
    const double whole = std::floor(s);
    *us = static_cast<int64_t>(whole) * 1000000 + std::llround((s - whole) * 1e6);
    return true;
}

bool seconds_to_time_us(double s, int64_t *us) {
    if (!std::isfinite(s) || s < 0.0 || s >= 86400.0) return false;
    const double whole = std::floor(s);
    *us = static_cast<int64_t>(whole) * 1000000 + std::llround((s - whole) * 1e6);
    return *us < 86400000000LL;
}

bool file_identity(const std::string &path, uint64_t *size, long long *mtime) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path p = fs::u8path(path);
    const auto sz = fs::file_size(p, ec);
    if (ec) return false;
    const auto t = fs::last_write_time(p, ec);
    if (ec) return false;
    *size = static_cast<uint64_t>(sz);
    *mtime = static_cast<long long>(t.time_since_epoch().count());
    return true;
}

bool ColumnSpec::user_missing(double v) const {
    if (std::isnan(v)) return false;
    for (double m : na_values)
        if (m == v) return true;
    return na_range && v >= na_lo && v <= na_hi;
}

int ColumnSpec::code_of(double v) const {
    auto it = std::lower_bound(codes.begin(), codes.end(), v,
                               [](const std::pair<double, int> &a, double x) { return a.first < x; });
    if (it != codes.end() && it->first == v) return it->second;
    return overflow_code;
}

size_t Plan::companions() const {
    size_t n = 0;
    for (const auto &c : cols) n += c.companion ? 1 : 0;
    return n;
}

/* ------------------------------------------------------------------ plan */

Plan make_plan(const std::string &path, const ReadOptions &opt) {
    Plan plan;
    plan.path = path;
    if (!file_identity(path, &plan.file_size, &plan.file_mtime))
        throw RError("cannot open " + path + ": no such file");
    plan.parsed = read_file(path, opt);
    const Frame &fr = plan.parsed.frame;
    plan.nrow = static_cast<int64_t>(fr.nrow);
    for (const auto &n : plan.parsed.notes) plan.notes.push_back(n);

    /* ---- the columns carried, row names first ----------------------------- */
    std::vector<Sem> sems;
    std::vector<std::string> dropped;
    if (fr.rownames == Frame::RowNames::Text) {
        ColumnSpec s;
        s.col = -1;
        s.vec = fr.rownames_vec;
        s.kind = OutKind::Varchar;
        plan.cols.push_back(s);
        sems.push_back(Sem::Character);
    }
    for (size_t i = 0; i < fr.cols.size(); i++) {
        const Column &c = fr.cols[i];
        const std::string shown = c.name_na ? std::string("NA") : c.name.empty() ? "column " + std::to_string(i + 1) : c.name;
        if (!c.unsupported.empty()) {
            dropped.push_back(shown + " (" + c.unsupported + ")");
            continue;
        }
        ColumnSpec s;
        s.col = static_cast<int>(i);
        s.vec = c.vec;
        const Sem sem = semantic_of(c);
        /* haven's SPSS user-missing values apply to plain numbers */
        if (sem == Sem::Integer || sem == Sem::Double) {
            const RValue *nv = attr_of(c.attrs, "na_values");
            if (nv && (nv->type == INTSXP || nv->type == REALSXP))
                for (double x : numbers_of(nv))
                    if (!std::isnan(x)) s.na_values.push_back(x);
            std::sort(s.na_values.begin(), s.na_values.end());
            s.na_values.erase(std::unique(s.na_values.begin(), s.na_values.end()), s.na_values.end());
            const std::vector<double> nr = numbers_of(attr_of(c.attrs, "na_range"));
            if (nr.size() == 2 && !std::isnan(nr[0]) && !std::isnan(nr[1])) {
                s.na_range = true;
                s.na_lo = nr[0];
                s.na_hi = nr[1];
            }
        }
        plan.cols.push_back(s);
        sems.push_back(sem);
    }
    if (!dropped.empty())
        plan.notes.push_back("columns Stata has no type for were left out: " + list_items(dropped));
    plan.dropped = dropped;

    /* ---- names ------------------------------------------------------------ */
    std::vector<std::string> leaves;
    std::vector<std::string> renamed, unnamed;
    {
        std::set<std::string> originals, used;
        for (const auto &s : plan.cols)
            if (s.col >= 0) {
                const Column &c = fr.cols[static_cast<size_t>(s.col)];
                if (!c.name_na && !c.name.empty()) originals.insert(c.name);
            }
        auto fresh = [&](const std::string &base) {
            for (int k = 1;; k++) {
                const std::string t = base + "_" + std::to_string(k);
                if (!used.count(t) && !originals.count(t)) return t;
            }
        };
        for (auto &s : plan.cols) {
            std::string want;
            if (s.col < 0) {
                want = "rowname";
                if (originals.count(want)) want = fresh(want);
            } else {
                const Column &c = fr.cols[static_cast<size_t>(s.col)];
                if (c.name_na || c.name.empty()) {
                    want = "V" + std::to_string(s.col + 1);
                    if (originals.count(want) || used.count(want)) want = fresh(want);
                    unnamed.push_back(std::to_string(s.col + 1) + " → " + want);
                } else {
                    want = c.name;
                    if (used.count(want)) {
                        want = fresh(want);
                        renamed.push_back(c.name + " → " + want);
                    }
                }
            }
            used.insert(want);
            s.name = want;
            leaves.push_back(want);
        }
    }

    /* ---- the profile pass: every column once ------------------------------- */
    std::vector<size_t> became_ts, became_days, date_nonfinite, time_seconds, ts_numbers, factor_text,
        bad_codes, tz_notes, tagged_cols, bad_tag_cols, no_code_cols, deferred_cols;
    std::vector<std::string> long_labels, char_labels, str_missing;
    for (size_t k = 0; k < plan.cols.size(); k++) {
        ColumnSpec &s = plan.cols[k];
        const Sem sem = sems[k];
        const Attrs empty;
        const Attrs &a = s.col >= 0 ? fr.cols[static_cast<size_t>(s.col)].attrs : empty;
        const RValue *levels = sem == Sem::Factor ? attr_of(a, "levels") : nullptr;
        const bool levels_ok = levels && levels->type == STRSXP && levels->kept();
        const size_t nlevels = levels_ok ? levels->strs.size() : 0;
        const Profile p = profile_column(plan, s, sem, nlevels);
        s.transcoded_cells = p.transcoded;
        plan.transcoded_cells += p.transcoded;
        const bool is_int = s.vec.type == INTSXP;
        auto range_type = [&]() {
            return p.any ? sttype_code(integer_type_for_range(double(p.imin), double(p.imax)), 0)
                         : std::string("byte");
        };
        switch (sem) {
        case Sem::Logical:
            s.kind = OutKind::Boolean;
            s.stata_type = "byte";
            break;
        case Sem::Character:
            s.kind = OutKind::Varchar;
            s.max_bytes = p.max_bytes;
            break;
        case Sem::Integer: case Sem::Difftime:
            s.kind = is_int ? OutKind::Integer : OutKind::Double;
            s.stata_type = is_int ? range_type() : "double";
            break;
        case Sem::Factor: {
            bool native = levels_ok && nlevels <= 65536;
            for (size_t j = 0; native && j < nlevels; j++)
                native = levels->strs[j].size() <= 32000;
            if (native) {
                s.kind = OutKind::Integer;
                s.stata_type = range_type();
                if (p.bad_codes) bad_codes.push_back(k);
            } else if (levels_ok) {
                s.kind = OutKind::Varchar;
                s.conv = Conv::FactorText;
                s.levels = levels->strs;
                for (size_t j = 0; j < nlevels; j++)
                    if (levels->na[j]) s.levels[j] = "NA";
                for (size_t j = 1; j <= nlevels; j++)
                    if (p.level_used[j]) s.max_bytes = std::max(s.max_bytes, s.levels[j - 1].size());
                factor_text.push_back(k);
                if (p.bad_codes) bad_codes.push_back(k);
            } else {
                s.kind = OutKind::Integer;
                s.stata_type = range_type();
            }
            break;
        }
        case Sem::Date:
            if (p.whole_days && p.days_fit) {
                s.kind = OutKind::Date;
                s.conv = is_int ? Conv::IntDays : Conv::RealDays;
                s.stata_type = "long";
            } else if (p.us_fit) {
                s.kind = OutKind::Timestamp;
                s.conv = Conv::RealDaysTs;
                s.stata_type = "double";
                became_ts.push_back(k);
            } else {
                s.kind = OutKind::Double;
                s.stata_type = "double";
                became_days.push_back(k);
            }
            if (p.nonfinite && s.kind != OutKind::Double) date_nonfinite.push_back(k);
            break;
        case Sem::PosixCt: {
            if (is_int) {
                s.kind = OutKind::Timestamp;
                s.conv = Conv::IntSecTs;
            } else if (p.us_fit) {
                s.kind = OutKind::Timestamp;
                s.conv = Conv::RealSecTs;
            } else {
                s.kind = OutKind::Double;
                ts_numbers.push_back(k);
            }
            s.stata_type = "double";
            if (p.nonfinite && s.kind != OutKind::Double) date_nonfinite.push_back(k);
            const std::string tz = text_attr(a, "tzone");
            if (s.kind == OutKind::Timestamp && !tz.empty() && tz != "UTC" && tz != "GMT" && tz != "Etc/UTC")
                tz_notes.push_back(k);
            break;
        }
        case Sem::Hms:
            if (p.in_day && p.nonfinite == 0) {
                s.kind = OutKind::Time;
                s.conv = is_int ? Conv::IntSecTime : Conv::RealSecTime;
                s.stata_type = "double";
            } else {
                s.kind = is_int ? OutKind::Integer : OutKind::Double;
                s.stata_type = is_int ? range_type() : "double";
                time_seconds.push_back(k);
            }
            break;
        case Sem::Int64:
            s.kind = OutKind::BigInt;
            s.conv = Conv::Int64Bits;
            s.stata_type = range_type();
            break;
        case Sem::Double:
            s.kind = OutKind::Double;
            s.stata_type = "double";
            if (s.col >= 0 && fr.cols[static_cast<size_t>(s.col)].deferred_numbers) deferred_cols.push_back(k);
            break;
        }
        if (s.kind == OutKind::Varchar)
            s.stata_type = s.max_bytes > static_cast<size_t>(kStataStrMax)
                               ? std::string("strL")
                               : "str" + std::to_string(std::max<size_t>(1, s.max_bytes));

        /* extended-missing codes: tagged NAs by their letter; user-missing
         * values dictionary-first (discrete values, then labelled values in
         * the range, both ascending), then the values seen in the range */
        const bool real_numbers = s.vec.type == REALSXP && sem != Sem::Int64;
        s.tagged = real_numbers && p.tagged > 0;
        if (s.tagged) tagged_cols.push_back(k);
        if (p.bad_tags) bad_tag_cols.push_back(k);
        if (!s.na_values.empty() || s.na_range) {
            /* a tagged NA's letter is its own (the data or a label use it);
             * the user-missing values take the other letters, in order */
            uint32_t reserved = s.tagged ? p.tag_mask : 0;
            const RValue *lk = attr_of(a, "labels");
            if (real_numbers && lk && lk->type == REALSXP && lk->kept())
                for (double key : lk->reals) {
                    const char t = na_tag(key);
                    if (t >= 'a' && t <= 'z') reserved |= uint32_t(1) << (t - 'a');
                }
            std::vector<int> free;
            for (int code = 1; code <= kStataExtMissMax; code++)
                if (!(reserved & (uint32_t(1) << (code - 1)))) free.push_back(code);
            std::vector<double> order;
            auto push = [&](double x) {
                if (std::find(order.begin(), order.end(), x) == order.end()) order.push_back(x);
            };
            for (double x : s.na_values) push(x);
            std::vector<double> lab;
            for (double x : numbers_of(lk))
                if (s.user_missing(x)) lab.push_back(x);
            std::sort(lab.begin(), lab.end());
            for (double x : lab) push(x);
            for (double x : p.in_range) push(x);
            const size_t cap = free.size();
            const size_t total = order.size() + (p.range_more ? 1 : 0);
            for (size_t j = 0; j < order.size() && cap > 0; j++) {
                if (total > cap && j >= cap - 1) {
                    s.overflow_code = free[cap - 1];
                    break;
                }
                s.codes.emplace_back(order[j], free[j]);
            }
            if (p.range_more && cap > 0) s.overflow_code = free[cap - 1];
            if (cap == 0 && p.um_cells > 0) no_code_cols.push_back(k);
            std::sort(s.codes.begin(), s.codes.end());
        }
        s.companion = p.um_cells > 0 || s.tagged;
    }

    /* ---- companions, engine names and Stata names --------------------------- */
    {
        std::set<std::string> taken(leaves.begin(), leaves.end());
        for (auto &s : plan.cols) {
            if (!s.companion) continue;
            std::string want = xmissing_companion_name(s.name);
            for (int k = 1; taken.count(want); k++) want = xmissing_companion_name(s.name) + "_" + std::to_string(k);
            taken.insert(want);
            s.companion_name = want;
        }
    }
    plan.leaf_names = leaves;
    for (const auto &s : plan.cols)
        if (s.companion) plan.leaf_names.push_back(s.companion_name);
    const std::vector<std::string> engine = engine_unique_ci(plan.leaf_names);
    const std::vector<std::string> stata = sanitize_unique(plan.leaf_names);
    {
        size_t k = plan.cols.size();
        for (size_t i = 0; i < plan.cols.size(); i++) {
            ColumnSpec &s = plan.cols[i];
            s.engine_name = engine[i];
            s.stata_name = stata[i];
            if (s.companion) s.companion_engine_name = engine[k++];
        }
    }
    for (size_t i = 0; i < engine.size(); i++)
        if (engine[i] != plan.leaf_names[i]) plan.rename_leaves = true;

    /* ---- metadata ------------------------------------------------------------ */
    json schema;
    schema["version"] = 1;
    json jvars = json::array();
    json vallabs = json::object();
    json chars = json::object();
    json xmiss = json::object();
    bool dta_clash = false;
    std::vector<std::string> big_attrs;
    auto key_json = [](double k) -> json {
        if (is_na_real(k)) {
            const char t = na_tag(k);
            return t >= 'a' && t <= 'z' ? json(std::string(".") + t) : json("NA");
        }
        if (std::isfinite(k) && k == std::trunc(k) && std::fabs(k) < 9007199254740992.0)
            return json(static_cast<long long>(k));
        if (!std::isfinite(k)) return json(short_double(k));
        return json(k);
    };
    for (size_t k = 0; k < plan.cols.size(); k++) {
        ColumnSpec &s = plan.cols[k];
        const Sem sem = sems[k];
        const Attrs empty;
        const Attrs &a = s.col >= 0 ? fr.cols[static_cast<size_t>(s.col)].attrs : empty;
        json scratch = json::object();
        const bool is_dta = s.stata_name == "_dta";
        dta_clash = dta_clash || is_dta;
        json &vc = is_dta ? scratch : chars[s.stata_name];
        vc = json::object();
        std::set<std::string> consumed = {"class"};

        /* formats */
        switch (s.kind) {
        case OutKind::Date: s.stata_format = "%td"; break;
        case OutKind::Timestamp: s.stata_format = "%tc"; break;
        case OutKind::Time: s.stata_format = "%tcHH:MM:SS"; break;
        default: break;
        }
        const bool temporal_fallback = (sem == Sem::Date || sem == Sem::PosixCt || sem == Sem::Hms) &&
                                       s.kind != OutKind::Date && s.kind != OutKind::Timestamp &&
                                       s.kind != OutKind::Time;
        const std::string fstata = text_attr(a, "format.stata");
        if (!fstata.empty()) {
            const FmtClass given = classify_format(fstata);
            const bool compatible = !temporal_fallback &&
                (s.stata_format.empty() || given == FmtClass::None ||
                 given == classify_format(s.stata_format));
            if (compatible) {
                s.stata_format = fstata;
                consumed.insert("format.stata");
            } else {
                plan.notes.push_back(s.stata_name + ": format.stata " + fstata +
                                     " does not match the converted units; kept in char "
                                     "r_attributes, with the output's own display format");
            }
        }

        /* the variable label */
        std::string varlab;
        const RValue *lab = attr_of(a, "label");
        if (lab && lab->type == STRSXP && lab->kept() && lab->strs.size() == 1 && !lab->na[0]) {
            varlab = lab->strs[0];
            consumed.insert("label");
            if (utf8_chars(varlab) > 80) {
                vc["r_label"] = varlab;
                long_labels.push_back(s.stata_name);
            }
        }

        /* value labels: a factor's levels, or haven's labels. What Stata can
         * hold goes native (integer keys of a column holding the R value, and
         * the extended-missing codes of tagged NAs and labelled user-missing
         * values); the complete set goes to char r_value_labels whenever part
         * of it cannot */
        json entries = json::array();
        if (sem == Sem::Factor && s.kind == OutKind::Integer) {
            const RValue *levels = attr_of(a, "levels");
            if (levels && levels->type == STRSXP && levels->kept()) {
                for (size_t j = 0; j < levels->strs.size(); j++)
                    entries.push_back(json::array({std::to_string(j + 1), levels->na[j] ? std::string("NA") : levels->strs[j]}));
                consumed.insert("levels");
            }
        } else if (sem == Sem::Factor) {
            consumed.insert("levels");
        }
        const RValue *labels = attr_of(a, "labels");
        if (labels && labels->kept()) {
            consumed.insert("labels");
            const RValue *names = labels->attr("names");
            const bool named = names && names->type == STRSXP && names->kept();
            const bool raw_numbers = s.kind == OutKind::Integer || s.kind == OutKind::Double;
            bool unrepresentable = !named;
            json full = json::array();
            if (named && (labels->type == INTSXP || labels->type == REALSXP || labels->type == LGLSXP)) {
                const std::vector<double> keys = numbers_of(labels);
                for (size_t j = 0; j < keys.size() && j < names->strs.size(); j++) {
                    const double key = labels->type == REALSXP ? labels->reals[j] : keys[j];
                    const std::string text = names->na[j] ? std::string("NA") : names->strs[j];
                    full.push_back(json::array({key_json(key), text}));
                    if (!raw_numbers || text.size() > 32000) {
                        unrepresentable = true;
                        continue;
                    }
                    if (is_na_real(key) || (labels->type != REALSXP && std::isnan(key))) {
                        const char t = labels->type == REALSXP ? na_tag(key) : 0;
                        if (t >= 'a' && t <= 'z') entries.push_back(json::array({std::string(".") + t, text}));
                        else unrepresentable = true;
                        continue;
                    }
                    const bool integral = std::isfinite(key) && key == std::trunc(key) && std::fabs(key) <= 2147483647.0;
                    if (integral) entries.push_back(json::array({std::to_string(static_cast<long long>(key)), text}));
                    else unrepresentable = true;
                    if (s.user_missing(key)) {
                        const int code = s.code_of(key);
                        if (code > 0 && (code != s.overflow_code || s.overflow_code == 0))
                            entries.push_back(json::array({std::string(".") + static_cast<char>('a' + code - 1), text}));
                    }
                }
            } else if (named && labels->type == STRSXP) {
                /* a label of NA_character_ has no text to match in Stata */
                for (size_t j = 0; j < labels->strs.size() && j < names->strs.size(); j++)
                    if (!labels->na[j])
                        full.push_back(json::array({labels->strs[j], names->na[j] ? std::string("NA") : names->strs[j]}));
                unrepresentable = true;
            } else {
                unrepresentable = true;
            }
            if (entries.size() > 65536) {
                entries = json::array();
                unrepresentable = true;
            }
            if (unrepresentable) {
                vc["r_value_labels"] = jdump(full);
                char_labels.push_back(s.stata_name);
            }
        }
        std::string vallab;
        if (!entries.empty()) {
            vallab = s.stata_name;
            vallabs[vallab] = {{"entries", entries}};
        }

        json jv;
        jv["name"] = s.stata_name;
        jv["src"] = s.name;
        jv["type"] = s.stata_type;
        jv["fmt"] = s.stata_format;
        jv["varlab"] = varlab;
        jv["vallab"] = vallab;
        jvars.push_back(jv);

        /* what Stata has no slot for */
        const std::vector<std::string> cls = class_list(a);
        if (!cls.empty()) vc["r_class"] = joined(cls);
        if (s.col >= 0 && fr.cols[static_cast<size_t>(s.col)].deferred_numbers)
            vc["r_deferred"] = "character in R: as.character() of these numbers, which R had not "
                               "yet carried out when the file was saved";
        if (s.col >= 0) {
            const Column &c = fr.cols[static_cast<size_t>(s.col)];
            if (!c.name_na && !c.name.empty() && c.name != s.name) vc["r_name"] = c.name;
        }
        if (sem == Sem::PosixCt) {
            const std::string tz = text_attr(a, "tzone");
            consumed.insert("tzone");
            vc["r_tzone"] = tz.empty() ? std::string("(none: the session's time zone)") : tz;
        }
        if (sem == Sem::Difftime || sem == Sem::Hms) {
            const std::string u = text_attr(a, "units");
            consumed.insert("units");
            if (!u.empty()) vc["r_units"] = u;
            else if (sem == Sem::Hms) vc["r_units"] = "secs";
        }
        if (!s.na_values.empty() || s.na_range || (s.vec.type == STRSXP && attr_of(a, "na_values"))) {
            consumed.insert("na_values");
            consumed.insert("na_range");
            const RValue *nv = attr_of(a, "na_values");
            vc["r_missing"] = missing_text(s, nv && nv->type == STRSXP ? nv : nullptr);
            if (s.vec.type == STRSXP) str_missing.push_back(s.stata_name);
            if (!s.codes.empty() || s.overflow_code) {
                std::string map;
                for (int code = 1; code <= kStataExtMissMax; code++) {
                    std::string vals;
                    for (const auto &pr : s.codes)
                        if (pr.second == code) vals += (vals.empty() ? "" : ",") + short_double(pr.first);
                    if (code == s.overflow_code)
                        vals += (vals.empty() ? "" : ",") + std::string("other values in the range");
                    if (!vals.empty())
                        map += (map.empty() ? "" : " ") + std::string(".") + static_cast<char>('a' + code - 1) + "=" + vals;
                }
                vc["r_missing_map"] = map;
            }
        }
        /* comment → notes */
        const RValue *cm = attr_of(a, "comment");
        if (cm && cm->type == STRSXP && cm->kept()) {
            consumed.insert("comment");
            int nn = 0;
            for (size_t j = 0; j < cm->strs.size(); j++)
                if (!cm->na[j] && !cm->strs[j].empty()) vc["note" + std::to_string(++nn)] = cm->strs[j];
            if (nn) vc["note0"] = std::to_string(nn);
        }
        json rest = json::object();
        for (const auto &kv : a)
            if (!consumed.count(kv.first)) rest[kv.first] = value_json(*kv.second, 0);
        if (!rest.empty()) {
            std::string dump = jdump(rest);
            if (dump.size() > 60000) {
                json names_only = json::array();
                for (const auto &it : rest.items()) names_only.push_back(it.key());
                dump = jdump(json{{"(too large to keep; attribute names)", names_only}});
                big_attrs.push_back(s.stata_name);
            }
            vc["r_attributes"] = dump;
        }
        if (!is_dta && vc.empty()) chars.erase(s.stata_name);
        if (s.companion) {
            xmiss[s.name] = s.companion_name;
            plan.xmissing_stata.push_back(s.stata_name);
        }
    }
    schema["vars"] = jvars;
    schema["sortedby"] = json::array();

    /* the data frame itself */
    json dta = json::object();
    std::string dtalabel;
    {
        std::set<std::string> consumed = {"class", "row.names", "names"};
        const RValue *lab = attr_of(fr.attrs, "label");
        if (lab && lab->type == STRSXP && lab->kept() && lab->strs.size() == 1 && !lab->na[0]) {
            dtalabel = lab->strs[0];
            consumed.insert("label");
            if (utf8_chars(dtalabel) > 80) dta["r_label"] = dtalabel;
        }
        const RValue *cm = attr_of(fr.attrs, "comment");
        if (cm && cm->type == STRSXP && cm->kept()) {
            consumed.insert("comment");
            int nn = 0;
            for (size_t j = 0; j < cm->strs.size(); j++)
                if (!cm->na[j] && !cm->strs[j].empty()) dta["note" + std::to_string(++nn)] = cm->strs[j];
            if (nn) dta["note0"] = std::to_string(nn);
        }
        if (!fr.object.empty()) dta["r_object"] = fr.object;
        const FileInfo &fi = plan.parsed.info;
        const std::string how = fi.compression == Compression::Gzip   ? "gzip"
                                : fi.compression == Compression::Zstd ? "zstd"
                                                                      : "uncompressed";
        const std::string by = r_version_string(fi.writer_version);
        dta["r_written_by"] = (by.empty() ? std::string("R") : "R " + by) + " (" +
                              std::string(fi.rdata ? "save(), " : "saveRDS(), ") + "serialization version " +
                              std::to_string(fi.version) + ", " + how + ")";
        dta["r_encoding"] = plan.parsed.codec->native_used();
        const std::vector<std::string> cls = class_list(fr.attrs);
        if (!cls.empty()) dta["r_class"] = joined(cls);
        json rest = json::object();
        for (const auto &kv : fr.attrs)
            if (!consumed.count(kv.first)) rest[kv.first] = value_json(*kv.second, 0);
        if (!rest.empty()) {
            std::string dump = jdump(rest);
            if (dump.size() > 60000) {
                json names_only = json::array();
                for (const auto &it : rest.items()) names_only.push_back(it.key());
                dump = jdump(json{{"(too large to keep; attribute names)", names_only}});
                big_attrs.push_back("_dta");
            }
            dta["r_attributes"] = dump;
        }
    }
    chars["_dta"] = dta;

    plan.kv_metadata_sql = "KV_METADATA {'parqit.schema': " + quote_literal(jdump(schema)) +
                           ", 'parqit.vallabs': " + quote_literal(jdump(vallabs)) +
                           ", 'parqit.chars': " + quote_literal(jdump(chars)) +
                           ", 'parqit.dtalabel': " + quote_literal(jdump(json(dtalabel)));
    if (!xmiss.empty())
        plan.kv_metadata_sql += ", '" + std::string(kXmissingMetaKey) + "': " + quote_literal(jdump(xmiss));
    plan.kv_metadata_sql += "}";

    /* ---- notes: every departure from a plain copy is said ------------------ */
    auto names_of = [&](const std::vector<size_t> &idx) {
        std::vector<std::string> out;
        for (size_t i : idx) out.push_back(plan.cols[i].stata_name);
        return list_names(out);
    };
    plan.transcoded_meta = plan.parsed.transcoded_meta;
    if (fr.rownames == Frame::RowNames::Text)
        plan.notes.push_back("the data frame's character row names were kept as the first variable, " +
                             plan.cols[0].stata_name);
    if (fr.rownames == Frame::RowNames::Integer) {
        Cursor rc(plan.parsed.source, fr.rownames_vec, plan.parsed.codec, size_t(1) << 16);
        bool plain = true;
        for (uint64_t i = 0; i < fr.rownames_vec.length && plain; i++) {
            if (fr.rownames_vec.type == INTSXP) plain = rc.next_int() == static_cast<int64_t>(i + 1);
            else plain = rc.next_real() == static_cast<double>(i + 1);
        }
        if (!plain)
            plan.notes.push_back("the data frame's integer row names (positions kept from a subset, "
                                 "not 1 to N) were not kept");
    }
    if (!unnamed.empty())
        plan.notes.push_back("columns without a name were named after their position: " + list_items(unnamed));
    if (!renamed.empty())
        plan.notes.push_back("columns that repeat a name were renamed (the R name is in char "
                             "var[r_name]): " + list_items(renamed));
    if (!tagged_cols.empty())
        plan.notes.push_back("haven tagged missing values became extended missing values (.a-.z) of "
                             "the same letter in " + names_of(tagged_cols) +
                             " (parqit use restores them; a lazy view reads them as .)");
    if (!bad_tag_cols.empty())
        plan.notes.push_back("tagged missing values whose tag is not a letter a-z became plain missing "
                             "values in " + names_of(bad_tag_cols));
    std::vector<std::string> um_cols;
    for (const auto &s : plan.cols)
        if (s.companion && (!s.codes.empty() || s.overflow_code)) um_cols.push_back(s.stata_name);
    if (!um_cols.empty())
        plan.notes.push_back("SPSS user-missing values (haven na_values/na_range) became extended "
                             "missing values (.a-.z) in " + list_names(um_cols) +
                             "; the codes are in char var[r_missing_map]");
    for (const auto &s : plan.cols)
        if (s.overflow_code)
            plan.notes.push_back(s.stata_name + ": more user-missing values than free extended missing "
                                 "codes; the last ones share ." + std::string(1, static_cast<char>('a' + s.overflow_code - 1)) +
                                 " (see char " + s.stata_name + "[r_missing_map])");
    if (!no_code_cols.empty())
        plan.notes.push_back("tagged missing values use all 26 letters, so the user-missing values of " +
                             names_of(no_code_cols) + " became plain missing values");
    if (!str_missing.empty())
        plan.notes.push_back("character variables keep their SPSS user-missing values as text (Stata "
                             "has no missing strings): " + list_names(str_missing) +
                             "; the definition is in char var[r_missing]");
    if (!became_ts.empty())
        plan.notes.push_back("Date variables holding fractions of a day were stored as date-times (%tc): " +
                             names_of(became_ts));
    if (!became_days.empty())
        plan.notes.push_back("Date variables holding values no date type can represent were stored as "
                             "numbers of days since 1970-01-01: " + names_of(became_days));
    if (!ts_numbers.empty())
        plan.notes.push_back("date-time variables holding values no date-time type can represent were "
                             "stored as numbers of seconds since 1970-01-01 UTC: " + names_of(ts_numbers));
    if (!date_nonfinite.empty())
        plan.notes.push_back("infinite or NaN dates and date-times became missing values in " +
                             names_of(date_nonfinite));
    if (!tz_notes.empty())
        plan.notes.push_back("date-times are stored as UTC clock times; R showed them in another time "
                             "zone (char var[r_tzone]): " + names_of(tz_notes));
    if (!time_seconds.empty())
        plan.notes.push_back("time-of-day variables outside 00:00-24:00 after rounding to microseconds were stored as "
                             "seconds: " + names_of(time_seconds));
    if (!deferred_cols.empty())
        plan.notes.push_back("character columns R had not yet converted from numbers (a deferred "
                             "as.character()) were stored as those numbers: " + names_of(deferred_cols) +
                             " (to keep R's text, run x <- paste0(x) on the column in R before saving)");
    if (!factor_text.empty())
        plan.notes.push_back("factors whose levels one Stata value label cannot hold (more than 65,536 "
                             "levels, or a level longer than 32,000 bytes) were stored as text: " +
                             names_of(factor_text));
    if (!bad_codes.empty())
        plan.notes.push_back("factors holding codes outside their levels (a damaged factor): " +
                             names_of(bad_codes) + "; those codes have no label (or no text)");
    if (dta_clash)
        plan.notes.push_back("a variable whose name becomes _dta in Stata cannot carry characteristics "
                             "(_dta names the dataset's own); its R attributes were not recorded");
    if (!char_labels.empty())
        plan.notes.push_back("value labels Stata cannot hold (on text, dates or non-integer values) are "
                             "kept in full in char var[r_value_labels]: " + list_names(char_labels));
    if (!long_labels.empty())
        plan.notes.push_back("variable labels longer than 80 characters (Stata's limit) are kept in full "
                             "in char var[r_label]: " + list_names(long_labels));
    if (!big_attrs.empty())
        plan.notes.push_back("R attributes too large for a Stata characteristic were reduced to their "
                             "names in char r_attributes of " + list_names(big_attrs));
    if (plan.transcoded_cells > 0 || plan.transcoded_meta > 0)
        plan.notes.push_back("text that was not what its encoding declares was read as " +
                             std::string(plan.parsed.codec->fallback_name()) + " (or, for bytes a "
                             "declared legacy encoding does not define, kept as U+FFFD): " +
                             std::to_string(plan.transcoded_cells) + " string cell(s), " +
                             std::to_string(plan.transcoded_meta) + " name or attribute text(s)");
    return plan;
}

} // namespace rdata
} // namespace parqit
