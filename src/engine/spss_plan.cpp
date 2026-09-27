#include "engine/spss_plan.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <locale>
#include <map>
#include <set>
#include <sstream>

#include "json.hpp"

#include "engine/legacy_encoding.hpp"
#include "engine/sanitize.hpp"
#include "engine/session.hpp" /* quote_literal */
#include "engine/typemap.hpp" /* kXmissingMetaKey, xmissing_companion_name */

namespace parqit {
namespace spss {

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

/* every text reaching the metadata is decoded UTF-8 already; replacing (not
 * throwing on) an invalid sequence is the safety net that keeps a damaged
 * text from turning into the JSON library's own error */
std::string jdump(const json &j) {
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string quoted(const std::string &s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\"\"";
        else out += c;
    }
    return out + "\"";
}

/* the SPSS MISSING VALUES syntax of a definition: 98, 99 / LO THRU -1, 99 */
std::string missing_syntax(const MissingSpec &m) {
    std::string out;
    auto add = [&](const std::string &part) { out += (out.empty() ? "" : ", ") + part; };
    if (m.has_range)
        add((m.lo_open ? std::string("LO") : short_double(m.lo)) + " THRU " +
            (m.hi_open ? std::string("HI") : short_double(m.hi)));
    for (double v : m.values) add(short_double(v));
    for (const auto &s : m.strings) add(quoted(s));
    return out;
}

/* Unicode code points of valid UTF-8 (continuation bytes are not counted) */
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

const char *kMeasure[] = {"", "nominal", "ordinal", "scale"};
const char *kAlign[] = {"left", "right", "center"};
const char *kRole[] = {"input", "target", "both", "none", "partition", "split"};

/* what the profile pass learns about one variable */
struct Profile {
    bool finite = true;          /* no NaN/Inf among the values kept */
    bool whole_days = true;      /* every value kept is a whole day */
    bool days_fit = true;        /* ... and fits a DATE */
    bool us_fit = true;          /* every value kept fits a TIMESTAMP */
    bool in_day = true;          /* every value kept lies in [0, 86400) */
    long long um_cells = 0;      /* user-missing cells */
    std::set<double> in_range;   /* the smallest distinct values seen inside the range */
    bool range_more = false;     /* ... and there were more than kept */
    size_t max_bytes = 0;
    long long transcoded = 0;
};

constexpr size_t kRangeKeep = 64;

std::string date_mask(int type, int width) {
    switch (type) {
    case kFmtDATE: return width >= 11 ? "DD-Mon-CCYY" : "DD-Mon-YY";
    case kFmtADATE: return width >= 10 ? "NN/DD/CCYY" : "NN/DD/YY";
    case kFmtEDATE: return width >= 10 ? "DD.NN.CCYY" : "DD.NN.YY";
    case kFmtSDATE: return width >= 10 ? "CCYY/NN/DD" : "YY/NN/DD";
    case kFmtJDATE: return width >= 7 ? "CCYYJJJ" : "YYJJJ";
    case kFmtQYR: return width >= 8 ? "q_!Q_CCYY" : "q_!Q_YY";
    case kFmtMOYR: return width >= 8 ? "Mon_CCYY" : "Mon_YY";
    case kFmtWKYR: return width >= 10 ? "ww_!W!K_CCYY" : "ww_!W!K_YY";
    default: return "DD-Mon-CCYY";
    }
}

std::string fraction_mask(int decimals) {
    if (decimals <= 0) return "";
    return "." + std::string(static_cast<size_t>(std::min(decimals, 3)), 's');
}

} // namespace

/* ---------------------------------------------------------- conversions */

bool seconds_to_unix_us(double s, int64_t *us) {
    if (!std::isfinite(s)) return false;
    const double whole = std::floor(s);
    const double frac = s - whole; /* exact */
    const double secs = whole - static_cast<double>(kSpssEpochToUnixSeconds);
    if (!(secs > -9.2e12 && secs < 9.2e12)) return false;
    int64_t sub = std::llround(frac * 1e6);
    int64_t base = static_cast<int64_t>(secs) * 1000000;
    *us = base + sub;
    return true;
}

bool seconds_to_unix_days(double s, int32_t *days) {
    if (!std::isfinite(s) || std::fmod(s, 86400.0) != 0.0) return false;
    const double d = s / 86400.0 - static_cast<double>(kSpssEpochToUnixDays);
    if (!(d >= -2147483647.0 && d <= 2147483647.0)) return false;
    *days = static_cast<int32_t>(d);
    return true;
}

bool seconds_to_time_us(double s, int64_t *us) {
    if (!std::isfinite(s) || s < 0.0 || s >= 86400.0) return false;
    const double whole = std::floor(s);
    *us = static_cast<int64_t>(whole) * 1000000 + std::llround((s - whole) * 1e6);
    return true;
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

std::string stata_format_for(const Format &f, OutKind kind) {
    if (kind == OutKind::Varchar) return "";
    if (kind == OutKind::Date) return "%td" + date_mask(f.type, f.width);
    if (kind == OutKind::Time) {
        if (f.type == kFmtMTIME) return "%tcMM:SS" + fraction_mask(f.decimals);
        return (f.width > 0 && f.width < 8 ? "%tcHH:MM" : "%tcHH:MM:SS") + fraction_mask(f.decimals);
    }
    if (kind == OutKind::Timestamp) {
        if (f.type == kFmtDATETIME)
            return (f.width > 0 && f.width < 20 ? "%tcDD-Mon-CCYY_HH:MM" : "%tcDD-Mon-CCYY_HH:MM:SS") +
                   fraction_mask(f.decimals);
        if (f.type == kFmtYMDHMS)
            return (f.width > 0 && f.width < 19 ? "%tcCCYY-NN-DD_HH:MM" : "%tcCCYY-NN-DD_HH:MM:SS") +
                   fraction_mask(f.decimals);
        return "%tc" + date_mask(f.type, f.width); /* a date holding times of day */
    }
    /* numeric display formats; the SPSS width counts sign and decimals too */
    int w = f.width, d = f.decimals;
    if (w <= 0) return "";
    if (d >= w) d = w > 1 ? w - 1 : 0;
    const std::string wd = std::to_string(w) + "." + std::to_string(d);
    switch (f.type) {
    case kFmtF: case kFmtZ: case kFmtPCT: case kFmtIB: case kFmtPIB: case kFmtP: case kFmtPK:
    case kFmtRB:
        return "%" + wd + "f";
    case kFmtCOMMA: case kFmtDOLLAR: case kFmtCCA: case kFmtCCB: case kFmtCCC: case kFmtCCD:
    case kFmtCCE:
        return "%" + wd + "fc";
    case kFmtDOT: /* 1.234,5: period grouping, comma decimal */
        return "%" + std::to_string(w) + "," + std::to_string(d) + "fc";
    case kFmtE:
        return "%" + wd + "e";
    case kFmtN: /* leading zeros */
        return "%0" + std::to_string(w) + ".0f";
    case kFmtWKDAY: case kFmtMONTH:
        return "%" + std::to_string(w) + ".0f";
    default:
        return ""; /* hex/binary input formats, durations: the numeric default */
    }
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

/* ---------------------------------------------------------------- plan */

Plan make_plan(const std::string &path, const ReadOptions &opt) {
    Plan plan;
    plan.path = path;
    if (!file_identity(path, &plan.file_size, &plan.file_mtime))
        throw SavError("cannot open " + path + ": no such file");
    plan.dict = read_dictionary(path, opt);
    const Dictionary &d = plan.dict;
    const size_t nv = d.vars.size();

    /* ---- the profile pass: every case, every variable ------------------ */
    std::vector<Profile> prof(nv);
    {
        CaseReader r(path, d);
        std::string raw, utf8;
        while (r.next()) {
            for (size_t i = 0; i < nv; i++) {
                const Variable &v = d.vars[i];
                Profile &p = prof[i];
                if (v.is_string()) {
                    r.raw_string(v, &raw);
                    if (decode_text(d, raw.data(), raw.size(), &utf8)) p.transcoded++;
                    p.max_bytes = std::max(p.max_bytes, utf8.size());
                    continue;
                }
                if (r.is_sysmis(v)) continue;
                const double x = r.number(v);
                if (!v.missing.empty() && v.missing.matches(x)) {
                    p.um_cells++;
                    if (v.missing.has_range &&
                        std::find(v.missing.values.begin(), v.missing.values.end(), x) ==
                            v.missing.values.end()) {
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
                    continue;
                }
                const Temporal t = temporal_of(v.print.type);
                if (t == Temporal::None || t == Temporal::Duration) {
                    if (!std::isfinite(x)) p.finite = false;
                    continue;
                }
                if (!std::isfinite(x)) {
                    p.finite = false;
                    continue;
                }
                if (t == Temporal::Date) {
                    int32_t dd;
                    if (!seconds_to_unix_days(x, &dd)) {
                        if (std::fmod(x, 86400.0) != 0.0) p.whole_days = false;
                        else p.days_fit = false;
                    }
                }
                if (t == Temporal::Date || t == Temporal::DateTime) {
                    int64_t us;
                    if (!seconds_to_unix_us(x, &us)) p.us_fit = false;
                }
                if (t == Temporal::Time && !(x >= 0.0 && x < 86400.0)) p.in_day = false;
            }
        }
        plan.ncases = r.cases_read();
    }

    /* ---- one column per variable ---------------------------------------- */
    /* column indices for the notes, named by their Stata names once known */
    std::vector<size_t> became_ts, became_seconds, time_seconds, str_missing, overflowed;
    std::vector<std::string> long_labels, char_labels;
    for (size_t i = 0; i < nv; i++) {
        const Variable &v = d.vars[i];
        const Profile &p = prof[i];
        ColumnSpec c;
        c.var = static_cast<int>(i);
        c.name = v.name;
        c.user_missing_cells = p.um_cells;
        c.transcoded_cells = p.transcoded;
        c.max_bytes = p.max_bytes;
        plan.transcoded_cells += p.transcoded;
        if (v.is_string()) {
            c.kind = OutKind::Varchar;
            c.stata_type = p.max_bytes > static_cast<size_t>(kStataStrMax)
                               ? std::string("strL")
                               : "str" + std::to_string(std::max<size_t>(1, p.max_bytes));
            if (!v.missing.strings.empty()) str_missing.push_back(i);
        } else {
            c.stata_type = "double";
            switch (temporal_of(v.print.type)) {
            case Temporal::Date:
                if (p.finite && p.whole_days && p.days_fit) {
                    c.kind = OutKind::Date;
                    c.stata_type = "long";
                } else if (p.finite && p.us_fit) {
                    c.kind = OutKind::Timestamp;
                    became_ts.push_back(i);
                } else {
                    c.kind = OutKind::Double;
                    became_seconds.push_back(i);
                }
                break;
            case Temporal::DateTime:
                if (p.finite && p.us_fit) {
                    c.kind = OutKind::Timestamp;
                } else {
                    c.kind = OutKind::Double;
                    became_seconds.push_back(i);
                }
                break;
            case Temporal::Time:
                if (p.finite && p.in_day) {
                    c.kind = OutKind::Time;
                } else {
                    c.kind = OutKind::Double;
                    time_seconds.push_back(i);
                }
                break;
            default:
                c.kind = OutKind::Double;
            }
        }
        c.stata_format = stata_format_for(v.print, c.kind);

        /* user-missing codes: dictionary-derived first (stable across the
         * files of a series), then the values seen inside the range */
        if (!v.is_string() && !v.missing.empty()) {
            std::vector<double> order;
            auto push = [&](double x) {
                if (std::find(order.begin(), order.end(), x) == order.end()) order.push_back(x);
            };
            std::vector<double> disc = v.missing.values;
            std::sort(disc.begin(), disc.end());
            for (double x : disc) push(x);
            if (v.missing.has_range) {
                std::vector<double> lab;
                for (const auto &vl : v.labels)
                    if (v.missing.matches(vl.number)) lab.push_back(vl.number);
                std::sort(lab.begin(), lab.end());
                for (double x : lab) push(x);
                for (double x : p.in_range) push(x);
            }
            const size_t total = order.size() + (p.range_more ? 1 : 0);
            for (size_t k = 0; k < order.size(); k++) {
                if (total > static_cast<size_t>(kStataExtMissMax) &&
                    k >= static_cast<size_t>(kStataExtMissMax) - 1) {
                    c.overflow_code = kStataExtMissMax;
                    break;
                }
                c.codes.emplace_back(order[k], static_cast<int>(k) + 1);
            }
            if (p.range_more) c.overflow_code = kStataExtMissMax;
            if (c.overflow_code) overflowed.push_back(i);
            std::sort(c.codes.begin(), c.codes.end());
            c.companion = p.um_cells > 0;
        }
        plan.cols.push_back(std::move(c));
    }

    /* ---- names: the reader's sanitiser over the output columns, in order */
    std::vector<std::string> out_names;
    for (auto &c : plan.cols) out_names.push_back(c.name);
    for (auto &c : plan.cols)
        if (c.companion) {
            c.companion_name = xmissing_companion_name(c.name);
            out_names.push_back(c.companion_name);
        }
    const std::vector<std::string> stata = sanitize_unique(out_names);
    for (size_t i = 0; i < plan.cols.size(); i++) plan.cols[i].stata_name = stata[i];

    /* ---- metadata --------------------------------------------------------- */
    json schema;
    schema["version"] = 1;
    json jvars = json::array();
    json vallabs = json::object();
    json chars = json::object();
    json xmiss = json::object();
    bool dta_clash = false;
    for (auto &c : plan.cols) {
        const Variable &v = d.vars[static_cast<size_t>(c.var)];
        /* chars are keyed by the Stata name, and `_dta` names the dataset's
         * own characteristics: a variable that loads as _dta keeps none */
        json scratch = json::object();
        const bool is_dta = c.stata_name == "_dta";
        dta_clash = dta_clash || is_dta;
        json &vc = is_dta ? scratch : chars[c.stata_name];
        vc = json::object();

        /* value labels: what Stata can hold goes native (integer keys of a
         * numeric column holding the SPSS value, plus the extended-missing
         * codes of labelled user-missing values); the complete SPSS set goes
         * to char spss_value_labels whenever part of it cannot */
        json entries = json::array();
        bool unrepresentable = false;
        const bool raw_numbers = c.kind == OutKind::Double; /* the stored value IS the SPSS value */
        for (const auto &vl : v.labels) {
            if (v.is_string()) {
                unrepresentable = true;
                continue;
            }
            const double k = vl.number;
            const bool integral = std::isfinite(k) && k == std::trunc(k) &&
                                  std::fabs(k) <= 2147483647.0;
            if (raw_numbers && integral) {
                entries.push_back(json::array({std::to_string(static_cast<long long>(k)), vl.label}));
            } else {
                unrepresentable = true;
            }
            if (!v.missing.empty() && v.missing.matches(k)) {
                const int code = c.code_of(k);
                if (code > 0 && (code != c.overflow_code || c.overflow_code == 0))
                    entries.push_back(json::array(
                        {std::string(".") + static_cast<char>('a' + code - 1), vl.label}));
            }
        }
        std::string vallab;
        if (!entries.empty()) {
            vallab = c.stata_name;
            vallabs[vallab] = {{"entries", entries}};
        }
        if (unrepresentable) {
            /* numeric keys as JSON numbers, integers without a ".0" */
            auto key_json = [](double k) -> json {
                if (std::isfinite(k) && k == std::trunc(k) && std::fabs(k) < 9007199254740992.0)
                    return json(static_cast<long long>(k));
                if (!std::isfinite(k)) return json(short_double(k)); /* JSON has no NaN/Inf */
                return json(k);
            };
            json full = json::array();
            for (const auto &vl : v.labels)
                full.push_back(v.is_string() ? json::array({vl.text_key, vl.label})
                                             : json::array({key_json(vl.number), vl.label}));
            vc["spss_value_labels"] = jdump(full);
            char_labels.push_back(c.stata_name);
        }

        json jv;
        jv["name"] = c.stata_name;
        jv["src"] = c.name;
        jv["type"] = c.stata_type;
        jv["fmt"] = c.stata_format;
        jv["varlab"] = v.label;
        jv["vallab"] = vallab;
        jvars.push_back(jv);

        const std::string fmt = format_name(v.print);
        if (!fmt.empty()) vc["spss_format"] = fmt;
        const std::string wfmt = format_name(v.write);
        if (!wfmt.empty() && wfmt != fmt) vc["spss_write_format"] = wfmt;
        if (v.measure >= 1 && v.measure <= 3) vc["spss_measure"] = kMeasure[v.measure];
        if (v.display_width >= 0) vc["spss_display_width"] = std::to_string(v.display_width);
        if (v.alignment >= 0 && v.alignment <= 2) vc["spss_alignment"] = kAlign[v.alignment];
        if (v.role >= 0 && v.role <= 5) vc["spss_role"] = kRole[v.role];
        if (!v.attributes.empty()) {
            json a = json::object();
            for (const auto &at : v.attributes) a[at.name] = at.values;
            vc["spss_attributes"] = jdump(a);
        }
        if (!v.missing.empty()) {
            vc["spss_missing"] = missing_syntax(v.missing);
            if (!c.codes.empty() || c.overflow_code) {
                std::string map;
                for (int code = 1; code <= kStataExtMissMax; code++) {
                    std::string vals;
                    for (const auto &pr : c.codes)
                        if (pr.second == code) vals += (vals.empty() ? "" : ",") + short_double(pr.first);
                    if (code == c.overflow_code) {
                        vals += (vals.empty() ? "" : ",") + std::string("other values in ") +
                                missing_syntax(v.missing);
                    }
                    if (!vals.empty())
                        map += (map.empty() ? "" : " ") + std::string(".") +
                               static_cast<char>('a' + code - 1) + "=" + vals;
                }
                vc["spss_missing_map"] = map;
            }
        }
        if (v.has_label && utf8_chars(v.label) > 80) {
            vc["spss_label"] = v.label;
            long_labels.push_back(c.stata_name);
        }
        if (!is_dta && vc.empty()) chars.erase(c.stata_name);
        if (c.companion) {
            xmiss[c.name] = c.companion_name;
            plan.xmissing_stata.push_back(c.stata_name);
        }
    }
    schema["vars"] = jvars;
    schema["sortedby"] = json::array();

    json dta = json::object();
    for (size_t k = 0; k < d.documents.size(); k++)
        dta["note" + std::to_string(k + 1)] = d.documents[k];
    if (!d.documents.empty()) dta["note0"] = std::to_string(d.documents.size());
    if (d.weight_var >= 0) dta["spss_weight"] = plan.cols[static_cast<size_t>(d.weight_var)].stata_name;
    dta["spss_encoding"] = d.declared_encoding.empty() ? std::string("not declared")
                                                       : d.declared_encoding;
    if (!d.product.empty()) dta["spss_product"] = d.product;
    if (!d.product_info.empty()) dta["spss_product_info"] = d.product_info;
    if (!d.creation_date.empty() || !d.creation_time.empty())
        dta["spss_creation"] = d.creation_date + (d.creation_time.empty() ? "" : " " + d.creation_time);
    if (!d.file_attributes.empty()) {
        json a = json::object();
        for (const auto &at : d.file_attributes) a[at.name] = at.values;
        dta["spss_attributes"] = jdump(a);
    }
    if (!d.mrsets.empty()) dta["spss_mrsets"] = d.mrsets;
    if (!d.varsets.empty()) dta["spss_varsets"] = d.varsets;
    chars["_dta"] = dta;

    plan.kv_metadata_sql = "KV_METADATA {'parqit.schema': " + quote_literal(jdump(schema)) +
                           ", 'parqit.vallabs': " + quote_literal(jdump(vallabs)) +
                           ", 'parqit.chars': " + quote_literal(jdump(chars)) +
                           ", 'parqit.dtalabel': " + quote_literal(jdump(json(d.file_label)));
    if (!xmiss.empty())
        plan.kv_metadata_sql += ", '" + std::string(kXmissingMetaKey) + "': " +
                                quote_literal(jdump(xmiss));
    plan.kv_metadata_sql += "}";

    /* ---- notes: every departure from a plain copy is said -------------- */
    auto names_of = [&](const std::vector<size_t> &idx) {
        std::vector<std::string> out;
        for (size_t i : idx) out.push_back(plan.cols[i].stata_name);
        return list_names(out);
    };
    plan.transcoded_meta = d.transcoded_meta;
    if (!plan.xmissing_stata.empty())
        plan.notes.push_back("SPSS user-missing values became extended missing values (.a-.z) in " +
                             list_names(plan.xmissing_stata) +
                             "; the codes are in char var[spss_missing_map] (parqit use restores "
                             "them; a lazy view reads them as .)");
    for (size_t i : overflowed)
        plan.notes.push_back(plan.cols[i].stata_name +
                             ": more user-missing values than the 26 extended missing codes; the "
                             "values beyond the 25th share .z (see char " + plan.cols[i].stata_name +
                             "[spss_missing_map])");
    if (!str_missing.empty())
        plan.notes.push_back("string variables keep their SPSS user-missing values as text (Stata has "
                             "no missing strings): " + names_of(str_missing) +
                             "; the definition is in char var[spss_missing]");
    if (!became_ts.empty())
        plan.notes.push_back("date variables that hold times of day were stored as date-times (%tc): " +
                             names_of(became_ts));
    if (!became_seconds.empty())
        plan.notes.push_back("date variables holding values no date type can represent were stored as "
                             "SPSS seconds since 14 Oct 1582 (char var[spss_format] keeps the format): " +
                             names_of(became_seconds));
    if (!time_seconds.empty())
        plan.notes.push_back("time variables holding durations beyond 00:00-24:00 were stored as "
                             "seconds: " + names_of(time_seconds));
    if (dta_clash)
        plan.notes.push_back("a variable whose name becomes _dta in Stata cannot carry "
                             "characteristics (_dta names the dataset's own); its SPSS "
                             "properties were not recorded");
    if (!char_labels.empty())
        plan.notes.push_back("value labels Stata cannot hold (on strings, dates or non-integer values) "
                             "are kept in full in char var[spss_value_labels]: " + list_names(char_labels));
    if (!long_labels.empty())
        plan.notes.push_back("variable labels longer than 80 characters (Stata's limit) are kept in "
                             "full in char var[spss_label]: " + list_names(long_labels));
    if (plan.transcoded_cells > 0 || plan.transcoded_meta > 0)
        plan.notes.push_back("text that was not valid UTF-8 despite the file's declaration was "
                             "transcoded from " + std::string(legacy_encoding_name(d.legacy)) + ": " +
                             std::to_string(plan.transcoded_cells) + " string cell(s), " +
                             std::to_string(plan.transcoded_meta) + " dictionary text(s)");
    if (!d.ignored.empty()) {
        std::string what;
        for (size_t k = 0; k < d.ignored.size(); k++) what += (k ? "; " : "") + d.ignored[k];
        plan.notes.push_back("SPSS records not carried into the Parquet file: " + what);
    }
    for (const auto &w : d.warnings) plan.notes.push_back(w);
    return plan;
}

} // namespace spss
} // namespace parqit
