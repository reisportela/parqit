#include "engine/legacy_encoding.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "engine/encoding_tables.hpp"

namespace parqit {

namespace {

using namespace enctab;

/* the registry: the single-byte code pages first (in the tables' order, so
 * windows-1252 is id 0), then these */
enum : int {
    kIdCp932 = static_cast<int>(kSingleCount),
    kIdCp936,
    kIdCp949,
    kIdCp950,
    kIdEucJp,
    kIdGb18030,
    kIdUtf8,
    kIdUtf16Le,
    kIdUtf16Be,
    kIdUtf16,
    kIdCount
};

const char *const kOtherName[] = {"windows-932", "windows-936", "windows-949", "windows-950",
                                  "euc-jp",      "gb18030",     "utf-8",       "utf-16le",
                                  "utf-16be",    "utf-16"};

constexpr uint32_t kReplacement = 0xFFFD;

void put(std::string *out, uint32_t cp) {
    if (cp < 0x80) {
        out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string alias_key(const std::string &name) {
    std::string k;
    size_t b = 0, e = name.size();
    while (b < e && (name[b] == ' ' || name[b] == '\t')) b++;
    while (e > b && (name[e - 1] == ' ' || name[e - 1] == '\t')) e--;
    for (size_t i = b; i < e; i++) {
        const char c = name[i];
        if (c == '-' || c == '_' || c == '.' || c == ' ') continue;
        k.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
    }
    return k;
}

const std::unordered_map<std::string, int> &canonical_ids() {
    static const std::unordered_map<std::string, int> ids = [] {
        std::unordered_map<std::string, int> m;
        for (size_t i = 0; i < kSingleCount; i++) m[kSingleName[i]] = static_cast<int>(i);
        for (int i = kIdCp932; i < kIdCount; i++) m[kOtherName[i - kIdCp932]] = i;
        return m;
    }();
    return ids;
}

struct Dbcs {
    const uint16_t *pairs;
    const uint16_t *single;
    uint8_t trail_lo, trail_hi;
    uint8_t lead_lo;
    int span;
    bool (*is_lead)(unsigned char);
};

bool lead_932(unsigned char b) { return (b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC); }
bool lead_81fe(unsigned char b) { return b >= 0x81 && b <= 0xFE; }

const Dbcs &dbcs(int id) {
    static const Dbcs t932{kCp932Pairs, kCp932Single, kCp932TrailLo, kCp932TrailHi, kCp932LeadLo,
                           kCp932TrailHi - kCp932TrailLo + 1, lead_932};
    static const Dbcs t936{kCp936Pairs, kCp936Single, kCp936TrailLo, kCp936TrailHi, kCp936LeadLo,
                           kCp936TrailHi - kCp936TrailLo + 1, lead_81fe};
    static const Dbcs t949{kCp949Pairs, kCp949Single, kCp949TrailLo, kCp949TrailHi, kCp949LeadLo,
                           kCp949TrailHi - kCp949TrailLo + 1, lead_81fe};
    static const Dbcs t950{kCp950Pairs, kCp950Single, kCp950TrailLo, kCp950TrailHi, kCp950LeadLo,
                           kCp950TrailHi - kCp950TrailLo + 1, lead_81fe};
    switch (id) {
    case kIdCp932: return t932;
    case kIdCp936: return t936;
    case kIdCp949: return t949;
    default: return t950;
    }
}

size_t decode_single(const unsigned char *u, size_t n, int id, std::string *out) {
    const uint16_t *table = kSingleTable[id];
    size_t bad = 0;
    for (size_t i = 0; i < n; i++) {
        const unsigned char b = u[i];
        const uint16_t cp = table[b];
        if (cp == kReplacement) bad++;
        put(out, cp);
    }
    return bad;
}

size_t decode_dbcs(const unsigned char *u, size_t n, const Dbcs &t, std::string *out) {
    size_t bad = 0;
    for (size_t i = 0; i < n;) {
        const unsigned char b = u[i];
        if (b < 0x80) {
            out->push_back(static_cast<char>(b));
            i++;
            continue;
        }
        if (t.is_lead(b)) {
            if (i + 1 < n) {
                const unsigned char tr = u[i + 1];
                if (tr >= t.trail_lo && tr <= t.trail_hi) {
                    const uint16_t cp = t.pairs[(b - t.lead_lo) * t.span + (tr - t.trail_lo)];
                    if (cp != 0xFFFF) {
                        put(out, cp);
                        i += 2;
                        continue;
                    }
                }
                /* an invalid pair: an ASCII second byte is read again on its
                 * own, as the WHATWG decoders do */
                bad++;
                put(out, kReplacement);
                i += tr < 0x80 ? 1 : 2;
                continue;
            }
            bad++; /* a lead byte at the end */
            put(out, kReplacement);
            i++;
            continue;
        }
        const uint16_t cp = t.single[b - 0x80];
        if (cp == 0xFFFF) {
            bad++;
            put(out, kReplacement);
        } else {
            put(out, cp);
        }
        i++;
    }
    return bad;
}

size_t decode_eucjp(const unsigned char *u, size_t n, std::string *out) {
    size_t bad = 0;
    auto in_a1fe = [](unsigned char c) { return c >= 0xA1 && c <= 0xFE; };
    for (size_t i = 0; i < n;) {
        const unsigned char b = u[i];
        if (b < 0x80) {
            out->push_back(static_cast<char>(b));
            i++;
            continue;
        }
        if (b == 0x8E && i + 1 < n && u[i + 1] >= 0xA1 && u[i + 1] <= 0xDF) { /* half-width katakana */
            put(out, 0xFF61 + (u[i + 1] - 0xA1));
            i += 2;
            continue;
        }
        if (b == 0x8F && i + 2 < n && in_a1fe(u[i + 1]) && in_a1fe(u[i + 2])) { /* JIS X 0212 */
            const uint16_t cp = kJis0212[(u[i + 1] - 0xA1) * 94 + (u[i + 2] - 0xA1)];
            if (cp != 0xFFFF) {
                put(out, cp);
                i += 3;
                continue;
            }
            bad++;
            put(out, kReplacement);
            i += 3;
            continue;
        }
        if (in_a1fe(b) && i + 1 < n && in_a1fe(u[i + 1])) { /* JIS X 0208 */
            const uint16_t cp = kJis0208[(b - 0xA1) * 94 + (u[i + 1] - 0xA1)];
            if (cp != 0xFFFF) {
                put(out, cp);
                i += 2;
                continue;
            }
            bad++;
            put(out, kReplacement);
            i += 2;
            continue;
        }
        bad++;
        put(out, kReplacement);
        i++;
    }
    return bad;
}

const std::vector<uint16_t> &gb18030_pairs() {
    static const std::vector<uint16_t> pairs = [] {
        std::vector<uint16_t> p(kCp936Pairs, kCp936Pairs + 126 * 191);
        for (size_t k = 0; k < kGb18030DiffCount; k++) p[kGb18030Diff[k].index] = kGb18030Diff[k].cp;
        return p;
    }();
    return pairs;
}

uint32_t gb18030_bmp(uint32_t pointer) {
    size_t lo = 0, hi = kGb18030RangeCount;
    while (hi - lo > 1) {
        const size_t mid = (lo + hi) / 2;
        if (kGb18030Ranges[mid].pointer <= pointer) lo = mid;
        else hi = mid;
    }
    return kGb18030Ranges[lo].cp + (pointer - kGb18030Ranges[lo].pointer);
}

size_t decode_gb18030(const unsigned char *u, size_t n, std::string *out) {
    const std::vector<uint16_t> &pairs = gb18030_pairs();
    size_t bad = 0;
    auto digit = [](unsigned char c) { return c >= 0x30 && c <= 0x39; };
    auto hi = [](unsigned char c) { return c >= 0x81 && c <= 0xFE; };
    for (size_t i = 0; i < n;) {
        const unsigned char b = u[i];
        if (b < 0x80) {
            out->push_back(static_cast<char>(b));
            i++;
            continue;
        }
        if (hi(b) && i + 1 < n) {
            const unsigned char t = u[i + 1];
            if (digit(t)) {
                if (i + 3 < n && hi(u[i + 2]) && digit(u[i + 3])) {
                    const uint32_t p = ((b - 0x81u) * 10u + (t - 0x30u)) * 1260u +
                                       (u[i + 2] - 0x81u) * 10u + (u[i + 3] - 0x30u);
                    uint32_t cp = 0;
                    if (p <= 39419) cp = gb18030_bmp(p);
                    else if (p >= 189000 && p <= 1237575) cp = 0x10000 + (p - 189000);
                    if (cp) {
                        put(out, cp);
                        i += 4;
                        continue;
                    }
                    bad++;
                    put(out, kReplacement);
                    i += 4;
                    continue;
                }
                bad++;
                put(out, kReplacement);
                i += 1; /* the digit is read again as ASCII */
                continue;
            }
            if ((t >= 0x40 && t <= 0x7E) || (t >= 0x80 && t <= 0xFE)) {
                const uint16_t cp = pairs[(b - 0x81) * 191 + (t - 0x40)];
                if (cp != 0xFFFF) {
                    put(out, cp);
                    i += 2;
                    continue;
                }
            }
            bad++;
            put(out, kReplacement);
            i += t < 0x80 ? 1 : 2;
            continue;
        }
        bad++; /* 0x80, 0xFF, or a lead byte at the end */
        put(out, kReplacement);
        i++;
    }
    return bad;
}

/* UTF-8 with the WHATWG replacement: the maximal valid prefix of a broken
 * sequence becomes one U+FFFD and the offending byte is read again */
size_t decode_utf8(const unsigned char *p, size_t n, std::string *out) {
    size_t bad = 0;
    for (size_t i = 0; i < n;) {
        const unsigned char c = p[i];
        if (c < 0x80) {
            out->push_back(static_cast<char>(c));
            i++;
            continue;
        }
        size_t extra = 0;
        unsigned char low = 0x80, high = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) extra = 1;
        else if (c == 0xE0) { extra = 2; low = 0xA0; }
        else if (c >= 0xE1 && c <= 0xEC) extra = 2;
        else if (c == 0xED) { extra = 2; high = 0x9F; }
        else if (c >= 0xEE && c <= 0xEF) extra = 2;
        else if (c == 0xF0) { extra = 3; low = 0x90; }
        else if (c >= 0xF1 && c <= 0xF3) extra = 3;
        else if (c == 0xF4) { extra = 3; high = 0x8F; }
        if (extra == 0) {
            bad++;
            put(out, kReplacement);
            i++;
            continue;
        }
        size_t k = 1;
        for (; k <= extra && i + k < n; k++) {
            const unsigned char x = p[i + k];
            const unsigned char lo = k == 1 ? low : 0x80, hi = k == 1 ? high : 0xBF;
            if (x < lo || x > hi) break;
        }
        if (k == extra + 1) {
            out->append(reinterpret_cast<const char *>(p + i), extra + 1);
            i += extra + 1;
        } else {
            bad++;
            put(out, kReplacement);
            i += k; /* the valid prefix is consumed; the offending byte is read again */
        }
    }
    return bad;
}

size_t decode_utf16(const unsigned char *u, size_t n, bool big, std::string *out) {
    size_t bad = 0;
    auto unit = [&](size_t i) -> uint32_t {
        return big ? (uint32_t(u[i]) << 8) | u[i + 1] : (uint32_t(u[i + 1]) << 8) | u[i];
    };
    size_t i = 0;
    for (; i + 1 < n; i += 2) {
        const uint32_t w = unit(i);
        if (w >= 0xD800 && w <= 0xDBFF) {
            if (i + 3 < n) {
                const uint32_t w2 = unit(i + 2);
                if (w2 >= 0xDC00 && w2 <= 0xDFFF) {
                    put(out, 0x10000 + ((w - 0xD800) << 10) + (w2 - 0xDC00));
                    i += 2;
                    continue;
                }
            }
            bad++;
            put(out, kReplacement);
            continue;
        }
        if (w >= 0xDC00 && w <= 0xDFFF) {
            bad++;
            put(out, kReplacement);
            continue;
        }
        put(out, w);
    }
    if (i < n) { /* an odd final byte */
        bad++;
        put(out, kReplacement);
    }
    return bad;
}

} // namespace

const TextEncoding TextEncoding::Windows1252{0};
const TextEncoding TextEncoding::Latin1{1};
const TextEncoding TextEncoding::Latin9{2};
const TextEncoding TextEncoding::MacRoman{3};
const TextEncoding TextEncoding::Utf8{kIdUtf8};

const char *TextEncoding::name() const {
    if (id_ >= 0 && id_ < static_cast<int>(kSingleCount)) return kSingleName[id_];
    if (id_ >= kIdCp932 && id_ < kIdCount) return kOtherName[id_ - kIdCp932];
    return "windows-1252";
}

TextEncoding::Kind TextEncoding::kind() const {
    if (id_ >= 0 && id_ < static_cast<int>(kSingleCount)) return Kind::SingleByte;
    switch (id_) {
    case kIdCp932: case kIdCp936: case kIdCp949: case kIdCp950: return Kind::DoubleByte;
    case kIdEucJp: return Kind::EucJp;
    case kIdGb18030: return Kind::Gb18030;
    case kIdUtf8: return Kind::Utf8;
    case kIdUtf16Le: return Kind::Utf16Le;
    case kIdUtf16Be: return Kind::Utf16Be;
    case kIdUtf16: return Kind::Utf16;
    default: return Kind::SingleByte;
    }
}

bool TextEncoding::multibyte() const {
    const Kind k = kind();
    return k == Kind::DoubleByte || k == Kind::EucJp || k == Kind::Gb18030;
}

bool TextEncoding::is_utf16() const {
    const Kind k = kind();
    return k == Kind::Utf16Le || k == Kind::Utf16Be || k == Kind::Utf16;
}

bool TextEncoding::ascii_identity() const {
    return kind() == Kind::SingleByte ? kSingleAsciiIdentity[id_] : !is_utf16();
}

bool text_ascii_unchanged(const char *p, size_t n, TextEncoding enc) {
    if (enc.ascii_identity()) {
        for (size_t i = 0; i < n; i++)
            if (static_cast<unsigned char>(p[i]) >= 0x80) return false;
    } else {
        if (enc.kind() != TextEncoding::Kind::SingleByte) return n == 0;
        const uint16_t *table = kSingleTable[enc.id()];
        for (size_t i = 0; i < n; i++) {
            const unsigned char b = static_cast<unsigned char>(p[i]);
            if (b >= 0x80 || table[b] != b) return false;
        }
    }
    return true;
}

bool legacy_encoding_parse(const std::string &name, LegacyEncoding *out) {
    const std::string key = alias_key(name);
    if (key.empty()) {
        *out = TextEncoding::Windows1252;
        return true;
    }
    const Alias *end = kAlias + kAliasCount;
    const Alias *it = std::lower_bound(kAlias, end, key, [](const Alias &a, const std::string &k) {
        return std::strcmp(a.key, k.c_str()) < 0;
    });
    if (it == end || key != it->key) return false;
    const auto &ids = canonical_ids();
    const auto f = ids.find(it->canonical);
    if (f == ids.end()) return false;
    *out = TextEncoding(f->second);
    return true;
}

const char *legacy_encoding_name(LegacyEncoding enc) { return enc.name(); }

const char *legacy_encoding_families() {
    return "utf-8; windows-1250 to windows-1258 and windows-874; latin1 and iso-8859-2 to "
           "iso-8859-16 (latin9 = iso-8859-15); koi8-r, koi8-u; the DOS code pages ibm437, "
           "ibm850, ibm852, ibm866, ...; macroman and the Mac code pages; shift_jis (cp932), "
           "euc-jp, gbk (cp936, gb2312), gb18030, euc-kr (cp949), big5 (cp950)";
}

std::string with_encoding_hint(const std::string &msg) {
    /* duckdb csv_error.cpp, CSVError::InvalidUTF8 (DuckDB 1.5.3) */
    if (msg.find("Invalid unicode (byte sequence mismatch) detected") == std::string::npos)
        return msg;
    static const std::string kSet = "Possible Solution: Set the correct encoding, if available, "
                                    "to read this CSV File (e.g., encoding='UTF-16')";
    static const std::string kSkip = "Possible Solution: Enable ignore errors (ignore_errors=true) "
                                     "to skip this row\n";
    static const std::string kOurs =
        "Possible Solution: name the encoding of the file with parqit's encoding() option, e.g. "
        "encoding(windows-1251), encoding(shift_jis) or encoding(gbk) (see help parqit); "
        "encoding(utf-8) reads it as UTF-8 and turns each invalid byte sequence into U+FFFD";
    std::string out = msg;
    const size_t skip = out.find(kSkip);
    if (skip != std::string::npos) out.erase(skip, kSkip.size());
    const size_t at = out.find(kSet);
    if (at != std::string::npos) return out.replace(at, kSet.size(), kOurs);
    return out + "\n" + kOurs;
}

bool utf8_valid(const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n;) {
        unsigned char c = p[i];
        if (c < 0x80) { i += 1; continue; }
        size_t extra;
        unsigned char low = 0x80, high = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) { extra = 1; }
        else if (c == 0xE0) { extra = 2; low = 0xA0; }
        else if (c >= 0xE1 && c <= 0xEC) { extra = 2; }
        else if (c == 0xED) { extra = 2; high = 0x9F; }
        else if (c >= 0xEE && c <= 0xEF) { extra = 2; }
        else if (c == 0xF0) { extra = 3; low = 0x90; }
        else if (c >= 0xF1 && c <= 0xF3) { extra = 3; }
        else if (c == 0xF4) { extra = 3; high = 0x8F; }
        else return false; /* 0x80-0xC1, 0xF5-0xFF: not a valid lead byte */
        if (i + extra >= n) return false;
        if (p[i + 1] < low || p[i + 1] > high) return false;
        for (size_t kk = 2; kk <= extra; kk++)
            if (p[i + kk] < 0x80 || p[i + kk] > 0xBF) return false;
        i += extra + 1;
    }
    return true;
}

size_t legacy_decode(const char *p, size_t n, LegacyEncoding enc, std::string *out) {
    out->clear();
    out->reserve(n + n / 2);
    const unsigned char *u = reinterpret_cast<const unsigned char *>(p);
    switch (enc.kind()) {
    case TextEncoding::Kind::SingleByte: return decode_single(u, n, enc.id(), out);
    case TextEncoding::Kind::DoubleByte: return decode_dbcs(u, n, dbcs(enc.id()), out);
    case TextEncoding::Kind::EucJp: return decode_eucjp(u, n, out);
    case TextEncoding::Kind::Gb18030: return decode_gb18030(u, n, out);
    case TextEncoding::Kind::Utf8: return decode_utf8(u, n, out);
    case TextEncoding::Kind::Utf16Le: return decode_utf16(u, n, false, out);
    case TextEncoding::Kind::Utf16Be: return decode_utf16(u, n, true, out);
    case TextEncoding::Kind::Utf16: {
        /* the byte-order mark decides; without one, little-endian (Windows) */
        if (n >= 2 && u[0] == 0xFE && u[1] == 0xFF) return decode_utf16(u + 2, n - 2, true, out);
        if (n >= 2 && u[0] == 0xFF && u[1] == 0xFE) return decode_utf16(u + 2, n - 2, false, out);
        return decode_utf16(u, n, false, out);
    }
    }
    return 0;
}

std::string legacy_to_utf8(const std::string &bytes, LegacyEncoding enc) {
    std::string out;
    legacy_decode(bytes.data(), bytes.size(), enc, &out);
    return out;
}

bool utf8_or_transcode(std::string *s, LegacyEncoding enc) {
    if (utf8_valid(*s)) return false;
    *s = legacy_to_utf8(*s, enc);
    return true;
}

} // namespace parqit
