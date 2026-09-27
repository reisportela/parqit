#include "engine/spss_reader.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <locale>
#include <map>
#include <set>
#include <sstream>

#include "miniz.hpp" /* DuckDB's bundled zlib subset (duckdb_miniz), for .zsav */

namespace parqit {
namespace spss {

namespace {

/* ---------------------------------------------------------------- bytes */

bool host_big_endian() {
    const uint16_t one = 1;
    unsigned char first;
    std::memcpy(&first, &one, 1);
    return first == 0;
}

uint32_t bswap32(uint32_t x) {
    return (x >> 24) | ((x >> 8) & 0xFF00u) | ((x << 8) & 0xFF0000u) | (x << 24);
}

uint64_t bswap64(uint64_t x) {
    return (uint64_t(bswap32(uint32_t(x))) << 32) | bswap32(uint32_t(x >> 32));
}

std::string offset_text(uint64_t off) { return "byte offset " + std::to_string(off); }

/* SPSS pads text with blanks; a few writers pad with NULs */
std::string rtrim_blanks(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    return s;
}

std::string ascii_upper(std::string s) {
    for (auto &c : s)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}

/* ------------------------------------------------------------ file input */

/* A buffered, offset-tracking reader over one file handle. std::ifstream on a
 * std::filesystem path opens UTF-8 names on every platform (a wide path on
 * Windows) and seeks with 64-bit offsets, so files beyond 4 GB work. */
class FileIn {
  public:
    explicit FileIn(const std::string &path) : path_(path) {
        namespace fs = std::filesystem;
        const fs::path p = fs::u8path(path);
        std::error_code ec;
        if (fs::is_directory(p, ec)) throw SavError(path + " is a directory, not an SPSS file");
        size_ = static_cast<uint64_t>(fs::file_size(p, ec));
        if (ec) throw SavError("cannot open " + path + ": " + ec.message());
        buf_.resize(size_t(1) << 20);
        f_.rdbuf()->pubsetbuf(buf_.data(), static_cast<std::streamsize>(buf_.size()));
        f_.open(p, std::ios::in | std::ios::binary);
        if (!f_.is_open()) throw SavError("cannot open " + path + " for reading");
    }
    uint64_t size() const { return size_; }
    uint64_t offset() const { return off_; }
    uint64_t remaining() const { return off_ <= size_ ? size_ - off_ : 0; }
    /* up to n bytes; fewer only at the end of the file */
    size_t read(void *dst, size_t n) {
        if (n == 0) return 0;
        f_.read(static_cast<char *>(dst), static_cast<std::streamsize>(n));
        const size_t got = static_cast<size_t>(f_.gcount());
        off_ += got;
        if (got < n) {
            if (f_.bad()) throw SavError("read error in " + path_ + " at " + offset_text(off_));
            f_.clear(); /* EOF: keep the stream usable for a later seek */
        }
        return got;
    }
    void seek(uint64_t off) {
        f_.clear();
        f_.seekg(static_cast<std::streamoff>(off), std::ios::beg);
        if (!f_) throw SavError("cannot seek to " + offset_text(off) + " in " + path_);
        off_ = off;
    }
    const std::string &path() const { return path_; }

  private:
    std::string path_;
    std::vector<char> buf_;
    std::ifstream f_;
    uint64_t size_ = 0, off_ = 0;
};

/* ---------------------------------------------------- dictionary parsing */

struct RawVar {
    uint64_t offset = 0;
    int32_t type = 0;
    int32_t n_missing = 0;
    int32_t print = 0, write = 0;
    std::string name;                  /* raw 8 bytes, trimmed */
    bool has_label = false;
    std::string label;                 /* raw */
    std::vector<std::string> missing;  /* raw 8-byte values */
};

struct RawLabelSet {
    uint64_t offset = 0;
    std::vector<std::pair<std::string, std::string>> entries; /* raw 8-byte key, raw label */
    std::vector<int32_t> dict_indexes;                         /* 1-based */
};

/* one physical variable = a variable record and its continuation records */
struct Phys {
    size_t raw = 0;            /* index into RawVar */
    size_t first_slot = 0;
    size_t nslots = 0;
    int width = 0;             /* record type: 0 numeric, else string width */
    int logical = -1;          /* the logical variable it belongs to */
    bool segment_tail = false; /* a 2nd+ segment of a very long string */
};

Format decode_format(int32_t f) {
    Format out;
    const uint32_t u = static_cast<uint32_t>(f);
    out.decimals = static_cast<int>(u & 0xFFu);
    out.width = static_cast<int>((u >> 8) & 0xFFu);
    out.type = static_cast<int>((u >> 16) & 0xFFu);
    return out;
}

class Parser {
  public:
    Parser(const std::string &path, const ReadOptions &opt) : path_(path), in_(path), opt_(opt) {}
    Dictionary parse();

  private:
    std::string path_;
    FileIn in_;
    ReadOptions opt_;
    Dictionary d_;
    bool swap_ = false; /* file byte order differs from the host's */
    std::vector<RawVar> raw_;
    std::vector<RawLabelSet> label_sets_;
    int32_t weight_index_ = 0;
    int32_t header_ncases_ = -1;
    int64_t ext_ncases_ = -1;
    bool have_ext_ncases_ = false;
    int character_code_ = -1;
    bool have_fp_info_ = false;
    std::string raw_file_label_;
    std::vector<std::string> raw_documents_;
    std::string encoding_record_;
    std::string long_names_, very_long_, long_labels_, long_missing_, var_attrs_, file_attrs_;
    std::string raw_product_info_, raw_mrsets_, raw_varsets_;
    std::vector<int32_t> display_;
    bool have_display_ = false;
    std::vector<Phys> phys_;
    /* per slot: the logical variable that STARTS there, else -1 (a dictionary
     * index names a variable by its first slot) */
    std::vector<int> slot_start_;
    std::string section_ = "file header";

    [[noreturn]] void fail(const std::string &what, uint64_t at) const {
        throw SavError("malformed SPSS file " + path_ + ": " + what + " (" + section_ + ", " +
                       offset_text(at) + ")");
    }
    [[noreturn]] void fail(const std::string &what) const { fail(what, in_.offset()); }
    void warn(const std::string &what) { d_.warnings.push_back(what); }

    void bytes(void *dst, size_t n) {
        const uint64_t at = in_.offset();
        if (in_.read(dst, n) != n) fail("the file ends early (truncated)", at);
    }
    std::string text(uint64_t n) {
        if (n > in_.remaining())
            fail("a length of " + std::to_string(n) + " bytes runs past the end of the file");
        std::string s(static_cast<size_t>(n), '\0');
        if (n) bytes(&s[0], static_cast<size_t>(n));
        return s;
    }
    int32_t i32() {
        uint32_t v;
        bytes(&v, 4);
        if (swap_) v = bswap32(v);
        int32_t out;
        std::memcpy(&out, &v, 4);
        return out;
    }
    uint64_t bits_of(const std::string &raw8) const {
        uint64_t v;
        std::memcpy(&v, raw8.data(), 8);
        return swap_ ? bswap64(v) : v;
    }
    double f64_of(const std::string &raw8) const {
        const uint64_t b = bits_of(raw8);
        double v;
        std::memcpy(&v, &b, 8);
        return v;
    }
    int32_t i32_at(const std::string &buf, size_t pos) const {
        uint32_t v;
        std::memcpy(&v, buf.data() + pos, 4);
        if (swap_) v = bswap32(v);
        int32_t out;
        std::memcpy(&out, &v, 4);
        return out;
    }
    int64_t i64_at(const std::string &buf, size_t pos) const {
        uint64_t v;
        std::memcpy(&v, buf.data() + pos, 8);
        if (swap_) v = bswap64(v);
        int64_t out;
        std::memcpy(&out, &v, 8);
        return out;
    }
    std::string dec(const std::string &raw) {
        std::string out;
        if (decode_text(d_, raw.data(), raw.size(), &out)) d_.transcoded_meta++;
        return out;
    }

    void read_header();
    void read_variable();
    void read_value_labels();
    void read_document();
    void read_extension();
    void resolve_encoding();
    void build_physical();
    void apply_very_long_strings();
    void build_logical();
    void apply_long_names();
    void apply_missing();
    void apply_value_labels();
    void apply_long_string_labels();
    void apply_long_string_missing();
    void apply_display();
    void apply_attributes();
    void apply_rest();
    void read_zlib_map();
    int var_by_name(const std::string &name) const;
    std::vector<Attribute> parse_attributes(const std::string &txt, size_t *pos, bool var_level);
};

void Parser::read_header() {
    char magic[4] = {0, 0, 0, 0};
    const size_t got = in_.read(magic, 4);
    const bool fl = got == 4 && (std::memcmp(magic, "$FL2", 4) == 0 ||
                                 std::memcmp(magic, "$FL3", 4) == 0);
    if (!fl) {
        if (got >= 3 && static_cast<unsigned char>(magic[0]) == 0x5b &&
            static_cast<unsigned char>(magic[1]) == 0xc6 &&
            static_cast<unsigned char>(magic[2]) == 0xd3)
            throw SavError(path_ + " is an EBCDIC SPSS system file, which parqit does "
                                   "not read; re-save it from SPSS on a current platform");
        /* a portable file (.por) carries "SPSSPORT" after its 200-byte splash
         * and 256-byte translation table; an encrypted .sav has its own marker
         * — name both instead of a bare "not an SPSS file" */
        std::string head(static_cast<size_t>(std::min<uint64_t>(in_.size(), 600)), '\0');
        in_.seek(0);
        if (!head.empty()) in_.read(&head[0], head.size());
        if (head.find("SPSSPORT") != std::string::npos)
            throw SavError(path_ + " is an SPSS portable file (.por), not a system file "
                                   "(.sav); open it in SPSS or PSPP and save it as .sav");
        if (head.find("ENCRYPTED") != std::string::npos)
            throw SavError(path_ + " is an encrypted (password-protected) SPSS file; save "
                                   "an unencrypted copy from SPSS and read that");
        throw SavError(path_ + " is not an SPSS system file (.sav/.zsav): it does not start "
                               "with $FL2 or $FL3");
    }
    const bool zsav = std::memcmp(magic, "$FL3", 4) == 0;
    if (in_.size() < 176) fail("the file ends inside the 176-byte header (truncated)", 0);
    char prod[60];
    bytes(prod, 60);
    d_.product = rtrim_blanks(std::string(prod, 60));
    /* layout_code (2 or 3) tells the byte order of every number */
    uint32_t layout;
    bytes(&layout, 4);
    if (layout == 2 || layout == 3) swap_ = false;
    else if (bswap32(layout) == 2 || bswap32(layout) == 3) swap_ = true;
    else fail("the layout code is neither 2 nor 3 in either byte order", 64);
    d_.big_endian = host_big_endian() != swap_;
    (void)i32(); /* nominal_case_size: unreliable by the specification */
    const int32_t compression = i32();
    if (compression == 0 && !zsav) d_.compression = Compression::None;
    else if (compression == 1 && !zsav) d_.compression = Compression::Bytecode;
    else if (compression == 2 && zsav) d_.compression = Compression::Zlib;
    else fail("compression code " + std::to_string(compression) + " does not match the " +
                  std::string(zsav ? "$FL3" : "$FL2") + " file type", 72);
    weight_index_ = i32();
    header_ncases_ = i32();
    const std::string braw = text(8);
    d_.bias = f64_of(braw);
    if (!std::isfinite(d_.bias) || d_.bias < 1.0 || d_.bias > 1e6)
        fail("the compression bias is not a plausible IEEE 754 number (a non-IEEE file?)", 84);
    char date[9], tim[8], label[64], pad[3];
    bytes(date, 9);
    bytes(tim, 8);
    bytes(label, 64);
    bytes(pad, 3);
    d_.creation_date = rtrim_blanks(std::string(date, 9));
    d_.creation_time = rtrim_blanks(std::string(tim, 8));
    raw_file_label_ = std::string(label, 64);
}

void Parser::read_variable() {
    RawVar v;
    v.offset = in_.offset() - 4;
    v.type = i32();
    const int32_t has_label = i32();
    v.n_missing = i32();
    v.print = i32();
    v.write = i32();
    char name[8];
    bytes(name, 8);
    v.name = rtrim_blanks(std::string(name, 8));
    if (v.type < -1 || v.type > 255)
        fail("variable type " + std::to_string(v.type) + " is not -1, 0 or a width 1-255", v.offset);
    if (has_label != 0 && has_label != 1)
        fail("has_var_label is " + std::to_string(has_label) + " (must be 0 or 1)", v.offset);
    if (has_label == 1) {
        const int32_t len = i32();
        if (len < 0) fail("negative variable-label length");
        const uint64_t padded = (static_cast<uint64_t>(len) + 3) / 4 * 4;
        std::string lab = text(padded);
        lab.resize(static_cast<size_t>(len));
        v.has_label = true;
        v.label = lab;
    }
    const int32_t nm = v.n_missing;
    if (!(nm >= 0 && nm <= 3) && nm != -2 && nm != -3)
        fail("n_missing_values is " + std::to_string(nm) + " (must be 0-3, -2 or -3)", v.offset);
    for (int k = 0; k < std::abs(nm); k++) v.missing.push_back(text(8));
    raw_.push_back(std::move(v));
}

void Parser::read_value_labels() {
    RawLabelSet s;
    s.offset = in_.offset() - 4;
    const int32_t count = i32();
    if (count < 0 || static_cast<uint64_t>(count) * 16 > in_.remaining())
        fail("value-label count " + std::to_string(count) + " is impossible here", s.offset);
    for (int32_t k = 0; k < count; k++) {
        std::string key = text(8);
        unsigned char len;
        bytes(&len, 1);
        /* the label and its length byte fill a multiple of 8 bytes */
        const uint64_t padded = (static_cast<uint64_t>(len) + 1 + 7) / 8 * 8 - 1;
        std::string lab = text(padded);
        lab.resize(len);
        s.entries.emplace_back(std::move(key), std::move(lab));
    }
    section_ = "value-label variables record";
    const uint64_t at = in_.offset();
    const int32_t rt = i32();
    if (rt != 4) fail("a value-label record is not followed by its variables record (type 4)", at);
    const int32_t n = i32();
    if (n < 0 || static_cast<uint64_t>(n) * 4 > in_.remaining())
        fail("value-label variable count " + std::to_string(n) + " is impossible here", at);
    for (int32_t k = 0; k < n; k++) s.dict_indexes.push_back(i32());
    label_sets_.push_back(std::move(s));
}

void Parser::read_document() {
    const uint64_t at = in_.offset() - 4;
    const int32_t n = i32();
    if (n < 0 || static_cast<uint64_t>(n) * 80 > in_.remaining())
        fail("document line count " + std::to_string(n) + " is impossible here", at);
    for (int32_t k = 0; k < n; k++) raw_documents_.push_back(text(80));
}

void Parser::read_extension() {
    const uint64_t at = in_.offset() - 4;
    const int32_t subtype = i32();
    const int32_t size = i32();
    const int32_t count = i32();
    section_ = "extension record 7/" + std::to_string(subtype);
    if (size < 0 || count < 0) fail("negative size or count", at);
    const uint64_t n = static_cast<uint64_t>(size) * static_cast<uint64_t>(count);
    if (n > in_.remaining()) fail("the record runs past the end of the file", at);
    std::string data = text(n);
    auto expect = [&](int32_t want_size, int32_t want_count) {
        if (size == want_size && (want_count < 0 || count == want_count)) return true;
        warn("record 7/" + std::to_string(subtype) + " has an unexpected size (" +
             std::to_string(size) + " x " + std::to_string(count) + ") and was ignored");
        return false;
    };
    switch (subtype) {
    case 3: /* machine integer info */
        if (expect(4, 8)) {
            for (int k = 0; k < 3; k++) d_.machine_version[k] = i32_at(data, size_t(k) * 4);
            const int32_t fp = i32_at(data, 16);
            if (fp == 2 || fp == 3)
                throw SavError("SPSS file " + path_ + " stores numbers in the " +
                               std::string(fp == 2 ? "IBM 370" : "DEC VAX") +
                               " floating-point format, which parqit does not read; "
                               "re-save it from SPSS on a current platform");
            if (fp != 1) warn("record 7/3 names floating-point format " + std::to_string(fp) +
                              "; the numbers were read as IEEE 754");
            const int32_t endian = i32_at(data, 24);
            if ((endian == 1 && !d_.big_endian) || (endian == 2 && d_.big_endian))
                warn("record 7/3 declares a byte order that contradicts the file header; "
                     "the header's was used");
            character_code_ = i32_at(data, 28);
        }
        break;
    case 4: /* machine floating-point info: SYSMIS, HIGHEST, LOWEST */
        if (expect(8, 3)) {
            d_.sysmis_bits = bits_of(data.substr(0, 8));
            d_.highest_bits = bits_of(data.substr(8, 8));
            d_.lowest_bits = bits_of(data.substr(16, 8));
            have_fp_info_ = true;
        }
        break;
    case 5: if (expect(1, -1)) raw_varsets_ += data; break;
    case 7:
    case 19: if (expect(1, -1)) raw_mrsets_ += data; break;
    case 10: if (expect(1, -1)) raw_product_info_ += data; break;
    case 11:
        if (expect(4, -1)) {
            display_.clear();
            for (int32_t k = 0; k < count; k++) display_.push_back(i32_at(data, size_t(k) * 4));
            have_display_ = true;
        }
        break;
    case 13: if (expect(1, -1)) long_names_ += data; break;
    case 14: if (expect(1, -1)) very_long_ += data; break;
    case 16:
        if (expect(8, 2)) {
            ext_ncases_ = i64_at(data, 8);
            have_ext_ncases_ = true;
        }
        break;
    case 17: if (expect(1, -1)) file_attrs_ += data; break;
    case 18: if (expect(1, -1)) var_attrs_ += data; break;
    case 20: if (expect(1, -1)) encoding_record_ = data; break;
    case 21: if (expect(1, -1)) long_labels_ += data; break;
    case 22: if (expect(1, -1)) long_missing_ += data; break;
    case 6:
        d_.ignored.push_back("7/6 (obsolete date information, " + std::to_string(n) + " bytes)");
        break;
    case 24:
        d_.ignored.push_back("7/24 (SPSS Data Editor view settings, " + std::to_string(n) +
                             " bytes of XML)");
        break;
    default:
        d_.ignored.push_back("7/" + std::to_string(subtype) + " (" + std::to_string(n) +
                             " bytes, not documented)");
        break;
    }
}

/* ------------------------------------------------------------- encoding */

bool legacy_by_name(std::string name, bool *utf8, LegacyEncoding *enc) {
    name = ascii_upper(name);
    std::string k;
    for (char c : name)
        if (c != '-' && c != '_' && c != ' ') k += c;
    if (k == "UTF8") { *utf8 = true; return true; }
    *utf8 = false;
    if (k == "WINDOWS1252" || k == "CP1252" || k == "MSANSI" || k == "USASCII" || k == "ASCII" ||
        k == "ANSIX3.41968" || k == "ISO646US" || k == "CP20127" || k == "US") {
        *enc = LegacyEncoding::Windows1252; /* ASCII is a subset */
        return true;
    }
    if (k == "ISO88591" || k == "LATIN1" || k == "CP819" || k == "L1" || k == "ISOLATIN1" ||
        k == "CP28591") {
        *enc = LegacyEncoding::Latin1;
        return true;
    }
    if (k == "ISO885915" || k == "LATIN9" || k == "LATIN0" || k == "CP28605") {
        *enc = LegacyEncoding::Latin9;
        return true;
    }
    if (k == "MACINTOSH" || k == "MACROMAN" || k == "XMACROMAN" || k == "CP10000" || k == "MAC") {
        *enc = LegacyEncoding::MacRoman;
        return true;
    }
    return false;
}

void Parser::resolve_encoding() {
    std::string declared = rtrim_blanks(encoding_record_);
    if (!declared.empty()) {
        d_.declared_encoding = declared;
    } else if (character_code_ >= 0) {
        static const std::map<int, const char *> known = {
            {65001, "UTF-8"}, {1252, "windows-1252"}, {28591, "ISO-8859-1"},
            {28605, "ISO-8859-15"}, {10000, "macintosh"}, {2, "7-bit ASCII"},
            {3, "8-bit ASCII"}, {20127, "US-ASCII"}, {1, "EBCDIC"}};
        d_.declared_encoding = "code page " + std::to_string(character_code_);
        auto it = known.find(character_code_);
        if (it != known.end()) d_.declared_encoding += std::string(" (") + it->second + ")";
    }
    bool utf8 = true;
    LegacyEncoding enc = LegacyEncoding::Windows1252;
    if (!opt_.encoding.empty()) {
        if (!legacy_by_name(opt_.encoding, &utf8, &enc))
            throw SavError("encoding(" + opt_.encoding + ") is not one parqit can decode; use "
                           "utf-8, windows-1252, latin1, latin9 or macroman");
    } else if (!declared.empty()) {
        if (!legacy_by_name(declared, &utf8, &enc))
            throw SavError("SPSS file " + path_ + " declares its text as " + declared +
                           ", which parqit cannot decode (it reads UTF-8, windows-1252, "
                           "latin1, latin9 and macroman); if the declaration is wrong give "
                           "encoding(), otherwise re-save the file from SPSS in Unicode mode");
    } else if (character_code_ >= 0) {
        switch (character_code_) {
        case 65001: utf8 = true; break;
        case 1252: case 2: case 3: case 20127: /* 7/8-bit ASCII: often miscoded 1252 */
            utf8 = false; enc = LegacyEncoding::Windows1252; break;
        case 28591: utf8 = false; enc = LegacyEncoding::Latin1; break;
        case 28605: utf8 = false; enc = LegacyEncoding::Latin9; break;
        case 10000: utf8 = false; enc = LegacyEncoding::MacRoman; break;
        case 1:
            throw SavError("SPSS file " + path_ + " declares EBCDIC text, which parqit does not read");
        default:
            throw SavError("SPSS file " + path_ + " declares its text in code page " +
                           std::to_string(character_code_) +
                           ", which parqit cannot decode (it reads UTF-8, windows-1252, latin1, "
                           "latin9 and macroman); if the declaration is wrong give encoding(), "
                           "otherwise re-save the file from SPSS in Unicode mode");
        }
    } else {
        /* no declaration at all (very old writers): the Western code page */
        utf8 = false;
        enc = LegacyEncoding::Windows1252;
        warn("the file does not declare its character encoding; text was read as windows-1252 "
             "(give encoding() if that is wrong)");
    }
    d_.utf8 = utf8;
    d_.legacy = enc;
    d_.encoding_used = utf8 ? "UTF-8" : legacy_encoding_name(enc);
}

/* ------------------------------------------------------- the variables */

void Parser::build_physical() {
    section_ = "variable records";
    size_t slot = 0;
    int pending = 0;
    for (size_t i = 0; i < raw_.size(); i++) {
        const RawVar &v = raw_[i];
        if (v.type == -1) {
            if (pending <= 0)
                fail("a string continuation record (type -1) follows no string variable", v.offset);
            pending--;
            phys_.back().nslots++;
            slot++;
            continue;
        }
        if (pending > 0)
            fail("string variable " + raw_[phys_.back().raw].name + " (width " +
                     std::to_string(phys_.back().width) + ") is missing " +
                     std::to_string(pending) + " continuation record(s)",
                 v.offset);
        Phys p;
        p.raw = i;
        p.first_slot = slot;
        p.nslots = 1;
        p.width = v.type;
        pending = v.type > 0 ? (v.type + 7) / 8 - 1 : 0;
        phys_.push_back(p);
        slot++;
    }
    if (pending > 0) fail("the last string variable is missing continuation records");
    d_.nslots = slot;
}

void Parser::apply_very_long_strings() {
    if (very_long_.empty()) return;
    section_ = "very long string record (7/14)";
    std::map<std::string, int> widths; /* upper-cased short name -> true width */
    size_t pos = 0;
    while (pos < very_long_.size()) {
        size_t end = very_long_.find('\t', pos);
        if (end == std::string::npos) end = very_long_.size();
        std::string item = very_long_.substr(pos, end - pos);
        pos = end + 1;
        item = rtrim_blanks(item);
        while (!item.empty() && item.front() == '\0') item.erase(item.begin());
        if (item.empty()) continue;
        const size_t eq = item.find('=');
        if (eq == std::string::npos || eq == 0) fail("entry \"" + item + "\" is not NAME=WIDTH");
        const std::string key = ascii_upper(rtrim_blanks(item.substr(0, eq)));
        const std::string num = item.substr(eq + 1);
        if (num.empty() || num.size() > 9 ||
            num.find_first_not_of("0123456789") != std::string::npos)
            fail("entry \"" + item + "\" does not give a decimal width");
        widths[key] = std::stoi(num);
    }
    for (size_t i = 0; i < phys_.size(); i++) {
        Phys &p = phys_[i];
        if (p.segment_tail) continue;
        auto it = widths.find(ascii_upper(raw_[p.raw].name));
        if (it == widths.end()) continue;
        const int W = it->second;
        if (W < 256 || W > 32767)
            fail("very long string " + raw_[p.raw].name + " has width " + std::to_string(W) +
                 " (must be 256-32767)");
        const size_t nseg = static_cast<size_t>((W + 251) / 252);
        if (i + nseg > phys_.size())
            fail("very long string " + raw_[p.raw].name + " needs " + std::to_string(nseg) +
                 " segments but the dictionary ends first");
        for (size_t s = 0; s < nseg; s++) {
            const Phys &q = phys_[i + s];
            if (q.width <= 0 || (s > 0 && q.segment_tail))
                fail("segment " + std::to_string(s + 1) + " of very long string " +
                     raw_[p.raw].name + " is not a string variable");
            const long used = std::max(0L, std::min(long(W) - 255L * long(s), 255L));
            if (q.width < used)
                fail("segment " + std::to_string(s + 1) + " of very long string " +
                     raw_[p.raw].name + " is narrower than the data it must hold");
        }
        for (size_t s = 1; s < nseg; s++) phys_[i + s].segment_tail = true;
        p.width = W; /* the logical width; segments are rebuilt in build_logical */
        widths.erase(it);
    }
    for (const auto &w : widths)
        warn("very long string record names " + w.first +
             ", which is not a string variable of the file; entry ignored");
}

void Parser::build_logical() {
    for (size_t i = 0; i < phys_.size(); i++) {
        Phys &p = phys_[i];
        if (p.segment_tail) continue;
        const RawVar &r = raw_[p.raw];
        Variable v;
        v.short_name = r.name;
        v.width = p.width;
        v.print = decode_format(r.print);
        v.write = decode_format(r.write);
        v.has_label = r.has_label;
        const int logical = static_cast<int>(d_.vars.size());
        p.logical = logical;
        if (p.width <= 255) {
            Variable::Segment seg;
            seg.slot = p.first_slot;
            seg.nslots = p.nslots;
            seg.used = p.width == 0 ? 8 : static_cast<size_t>(p.width);
            v.segments.push_back(seg);
        } else {
            /* the format fields hold one byte of width: a very long string's
             * records say A255; its true width is the record 7/14 one */
            v.print.width = v.write.width = p.width;
            const size_t nseg = static_cast<size_t>((p.width + 251) / 252);
            for (size_t s = 0; s < nseg; s++) {
                Phys &q = phys_[i + s];
                q.logical = logical;
                Variable::Segment seg;
                seg.slot = q.first_slot;
                seg.nslots = q.nslots;
                seg.used = static_cast<size_t>(
                    std::max(0L, std::min(long(p.width) - 255L * long(s), 255L)));
                v.segments.push_back(seg);
            }
        }
        d_.vars.push_back(std::move(v));
    }
    d_.slot_is_string.assign(d_.nslots, false);
    slot_start_.assign(d_.nslots, -1);
    for (const auto &p : phys_) {
        if (p.width != 0)
            for (size_t s = 0; s < p.nslots; s++) d_.slot_is_string[p.first_slot + s] = true;
        if (!p.segment_tail) slot_start_[p.first_slot] = p.logical;
    }
}

void Parser::apply_long_names() {
    /* short names first (decoded), then the long-name map: SHORT=Long<TAB>... */
    std::map<std::string, int> by_short;
    for (size_t i = 0; i < phys_.size(); i++) {
        const Phys &p = phys_[i];
        if (p.segment_tail) continue;
        Variable &v = d_.vars[static_cast<size_t>(p.logical)];
        v.short_name = dec(raw_[p.raw].name);
        v.name = v.short_name;
        by_short.emplace(ascii_upper(raw_[p.raw].name), p.logical);
    }
    if (!long_names_.empty()) {
        section_ = "long variable names record (7/13)";
        /* SHORT=Long pairs separated by tabs */
        std::vector<std::string> items;
        size_t pos = 0;
        for (;;) {
            const size_t end = long_names_.find('\t', pos);
            items.push_back(long_names_.substr(pos, end == std::string::npos ? std::string::npos
                                                                            : end - pos));
            if (end == std::string::npos) break;
            pos = end + 1;
        }
        for (const std::string &item : items) {
            if (rtrim_blanks(item).empty()) continue;
            const size_t eq = item.find('=');
            if (eq == std::string::npos || eq == 0 || eq + 1 >= item.size()) {
                warn("long variable names record: entry \"" + dec(item) +
                     "\" is not SHORT=Long and was ignored");
                continue;
            }
            auto it = by_short.find(ascii_upper(rtrim_blanks(item.substr(0, eq))));
            if (it == by_short.end()) {
                warn("long variable names record names " + dec(item.substr(0, eq)) +
                     ", which is not a variable of the file; entry ignored");
                continue;
            }
            d_.vars[static_cast<size_t>(it->second)].name = dec(rtrim_blanks(item.substr(eq + 1)));
        }
    }
    /* names must stay distinct, case-insensitively (SPSS and DuckDB compare
     * names that way), and hold no NUL (the engine takes names as C strings);
     * a damaged file is repaired loudly */
    std::set<std::string> seen;
    for (auto &v : d_.vars) {
        if (v.name.find('\0') != std::string::npos) {
            std::replace(v.name.begin(), v.name.end(), '\0', '_');
            warn("a variable name holds a NUL byte; it is read as " + v.name);
        }
        if (v.name.empty()) v.name = "V" + std::to_string(&v - &d_.vars[0] + 1);
        std::string key = ascii_upper(v.name);
        if (seen.insert(key).second) continue;
        const std::string orig = v.name;
        for (int k = 1;; k++) {
            const std::string cand = orig + "_" + std::to_string(k);
            if (seen.insert(ascii_upper(cand)).second) {
                v.name = cand;
                break;
            }
        }
        warn("variable name " + orig + " occurs twice in the dictionary; the second is " + v.name);
    }
}

void Parser::apply_missing() {
    const uint64_t kNegMax = 0xFFEFFFFFFFFFFFFFull; /* -DBL_MAX: SYSMIS, and LOWEST since SPSS 21 */
    const uint64_t kNegNext = 0xFFEFFFFFFFFFFFFEull; /* LOWEST before SPSS 21 */
    const uint64_t kPosMax = 0x7FEFFFFFFFFFFFFFull;  /* HIGHEST */
    for (const auto &p : phys_) {
        if (p.segment_tail) continue;
        const RawVar &r = raw_[p.raw];
        if (r.missing.empty()) continue;
        Variable &v = d_.vars[static_cast<size_t>(p.logical)];
        MissingSpec &m = v.missing;
        if (v.width == 0) {
            size_t k = 0;
            if (r.n_missing < 0) {
                const uint64_t lo = bits_of(r.missing[0]), hi = bits_of(r.missing[1]);
                m.has_range = true;
                m.lo_open = lo == kNegMax || lo == kNegNext || (have_fp_info_ && lo == d_.lowest_bits);
                m.hi_open = hi == kPosMax || (have_fp_info_ && hi == d_.highest_bits);
                m.lo = f64_of(r.missing[0]);
                m.hi = f64_of(r.missing[1]);
                if (!m.lo_open && !m.hi_open && m.lo > m.hi)
                    warn("variable " + v.name + ": missing-value range " +
                         std::to_string(m.lo) + " THRU " + std::to_string(m.hi) +
                         " is empty (low end above high end)");
                k = 2;
            }
            for (; k < r.missing.size(); k++) m.values.push_back(f64_of(r.missing[k]));
        } else {
            if (r.n_missing < 0) {
                warn("string variable " + v.name +
                     " has a missing-value range, which SPSS does not allow; ignored");
                continue;
            }
            for (const auto &raw : r.missing) {
                std::string key = raw.substr(0, std::min<size_t>(8, static_cast<size_t>(v.width)));
                m.strings.push_back(dec(rtrim_blanks(key)));
            }
        }
    }
}

int Parser::var_by_name(const std::string &name) const {
    const std::string key = ascii_upper(name);
    for (size_t i = 0; i < d_.vars.size(); i++)
        if (ascii_upper(d_.vars[i].name) == key) return static_cast<int>(i);
    for (size_t i = 0; i < d_.vars.size(); i++)
        if (ascii_upper(d_.vars[i].short_name) == key) return static_cast<int>(i);
    return -1;
}

void Parser::apply_value_labels() {
    for (const auto &s : label_sets_) {
        section_ = "value-label record";
        std::vector<int> targets;
        int kind = -1; /* 0 numeric, 1 string */
        bool bad = false;
        for (int32_t idx : s.dict_indexes) {
            if (idx < 1 || static_cast<size_t>(idx) > d_.nslots) {
                warn("a value-label set (" + offset_text(s.offset) + ") names dictionary index " +
                     std::to_string(idx) + ", outside the dictionary; that entry was ignored");
                continue;
            }
            const int found = slot_start_[static_cast<size_t>(idx - 1)];
            if (found < 0) {
                warn("a value-label set (" + offset_text(s.offset) + ") names dictionary index " +
                     std::to_string(idx) + ", which is not the start of a variable; ignored");
                continue;
            }
            const Variable &v = d_.vars[static_cast<size_t>(found)];
            if (v.width > 8) {
                warn("a value-label set (" + offset_text(s.offset) + ") names long string " +
                     v.name + ", which may not carry short-string labels; ignored");
                continue;
            }
            const int k = v.width > 0 ? 1 : 0;
            if (kind >= 0 && kind != k) bad = true;
            kind = k;
            targets.push_back(found);
        }
        if (bad) {
            warn("a value-label set (" + offset_text(s.offset) +
                 ") is attached to both numeric and string variables; it was ignored");
            continue;
        }
        for (int t : targets) {
            Variable &v = d_.vars[static_cast<size_t>(t)];
            for (const auto &e : s.entries) {
                ValueLabel vl;
                vl.label = dec(e.second);
                if (v.width == 0) {
                    vl.number = f64_of(e.first);
                } else {
                    const size_t w = static_cast<size_t>(v.width);
                    /* ReadStat writes keys longer than the variable; PSPP drops
                     * a key whose bytes beyond the width are not blank */
                    bool clean = true;
                    for (size_t b = w; b < 8; b++)
                        if (e.first[b] != ' ' && e.first[b] != '\0') clean = false;
                    if (!clean) {
                        warn("variable " + v.name + ": a value label for a key wider than the "
                             "variable (" + dec(rtrim_blanks(e.first)) + ") was ignored");
                        continue;
                    }
                    vl.text_key = dec(rtrim_blanks(e.first.substr(0, w)));
                }
                v.labels.push_back(std::move(vl));
            }
        }
    }
}

void Parser::apply_long_string_labels() {
    if (long_labels_.empty()) return;
    section_ = "long string value labels record (7/21)";
    const std::string &b = long_labels_;
    size_t pos = 0;
    auto need = [&](size_t n) {
        if (pos + n > b.size()) fail("an entry runs past the end of the record");
    };
    while (pos < b.size()) {
        need(4);
        const int32_t nlen = i32_at(b, pos);
        pos += 4;
        if (nlen < 0) fail("negative variable-name length");
        need(static_cast<size_t>(nlen));
        const std::string name = rtrim_blanks(b.substr(pos, static_cast<size_t>(nlen)));
        pos += static_cast<size_t>(nlen);
        need(8);
        (void)i32_at(b, pos); /* the variable's width */
        const int32_t nlab = i32_at(b, pos + 4);
        pos += 8;
        if (nlab < 0) fail("negative label count");
        const int vi = var_by_name(dec(name));
        for (int32_t k = 0; k < nlab; k++) {
            need(4);
            const int32_t vlen = i32_at(b, pos);
            pos += 4;
            if (vlen < 0) fail("negative value length");
            need(static_cast<size_t>(vlen));
            const std::string key = b.substr(pos, static_cast<size_t>(vlen));
            pos += static_cast<size_t>(vlen);
            need(4);
            const int32_t llen = i32_at(b, pos);
            pos += 4;
            if (llen < 0) fail("negative label length");
            need(static_cast<size_t>(llen));
            const std::string lab = b.substr(pos, static_cast<size_t>(llen));
            pos += static_cast<size_t>(llen);
            if (vi < 0 || !d_.vars[static_cast<size_t>(vi)].is_string()) continue;
            ValueLabel vl;
            vl.text_key = dec(rtrim_blanks(key));
            vl.label = dec(lab);
            d_.vars[static_cast<size_t>(vi)].labels.push_back(std::move(vl));
        }
        if (vi < 0) warn("long string value labels name " + dec(name) +
                         ", which is not a variable of the file; ignored");
        else if (!d_.vars[static_cast<size_t>(vi)].is_string())
            warn("long string value labels name numeric variable " + dec(name) + "; ignored");
    }
}

void Parser::apply_long_string_missing() {
    if (long_missing_.empty()) return;
    section_ = "long string missing values record (7/22)";
    const std::string &b = long_missing_;
    size_t pos = 0;
    auto need = [&](size_t n) {
        if (pos + n > b.size()) fail("an entry runs past the end of the record");
    };
    while (pos < b.size()) {
        need(4);
        const int32_t nlen = i32_at(b, pos);
        pos += 4;
        if (nlen < 0) fail("negative variable-name length");
        need(static_cast<size_t>(nlen));
        const std::string name = rtrim_blanks(b.substr(pos, static_cast<size_t>(nlen)));
        pos += static_cast<size_t>(nlen);
        need(1);
        const int nmiss = static_cast<unsigned char>(b[pos]);
        pos += 1;
        if (nmiss < 1 || nmiss > 3) fail("missing-value count " + std::to_string(nmiss) + " is not 1-3");
        need(4);
        const int32_t vlen = i32_at(b, pos);
        pos += 4;
        if (vlen < 0) fail("negative value length");
        const int vi = var_by_name(dec(name));
        std::vector<std::string> vals;
        for (int k = 0; k < nmiss; k++) {
            need(static_cast<size_t>(vlen));
            vals.push_back(dec(rtrim_blanks(b.substr(pos, static_cast<size_t>(vlen)))));
            pos += static_cast<size_t>(vlen);
        }
        if (vi < 0 || !d_.vars[static_cast<size_t>(vi)].is_string()) {
            warn("long string missing values name " + dec(name) +
                 ", which is not a string variable of the file; ignored");
            continue;
        }
        d_.vars[static_cast<size_t>(vi)].missing.strings = vals;
    }
}

void Parser::apply_display() {
    if (!have_display_) return;
    /* one set per variable record other than continuations — a very long
     * string contributes one per segment; the first segment's set is its own */
    const size_t nrec = phys_.size();
    size_t per = 0;
    if (nrec > 0 && display_.size() == 3 * nrec) per = 3;
    else if (nrec > 0 && display_.size() == 2 * nrec) per = 2;
    if (per == 0) {
        warn("variable display record (7/11) has " + std::to_string(display_.size()) +
             " values for " + std::to_string(nrec) +
             " variable records; measurement levels, widths and alignments were not read");
        return;
    }
    for (size_t i = 0; i < nrec; i++) {
        if (phys_[i].segment_tail) continue;
        Variable &v = d_.vars[static_cast<size_t>(phys_[i].logical)];
        const int32_t measure = display_[per * i];
        const int32_t align = display_[per * i + per - 1];
        if (measure >= 0 && measure <= 3) v.measure = measure;
        if (per == 3 && display_[per * i + 1] >= 0) v.display_width = display_[per * i + 1];
        if (align >= 0 && align <= 2) v.alignment = align;
    }
}

/* attributes: name('value'\n'value'\n)name2('...'\n)  [variable level: a
 * variable name and ':' before each set, sets separated by '/'] */
std::vector<Attribute> Parser::parse_attributes(const std::string &t, size_t *pos, bool var_level) {
    std::vector<Attribute> out;
    size_t &p = *pos;
    while (p < t.size()) {
        while (p < t.size() && (t[p] == '\n' || t[p] == '\r' || t[p] == ' ')) p++;
        if (p >= t.size()) break;
        if (var_level && t[p] == '/') { p++; break; }
        const size_t open = t.find('(', p);
        if (open == std::string::npos) {
            warn("attribute record: text after the last attribute was ignored");
            p = t.size();
            break;
        }
        Attribute a;
        a.name = dec(t.substr(p, open - p));
        p = open + 1;
        for (;;) {
            const size_t nl = t.find('\n', p);
            if (nl == std::string::npos) {
                warn("attribute " + a.name + ": a value is not terminated; the rest was ignored");
                p = t.size();
                break;
            }
            std::string val = t.substr(p, nl - p);
            p = nl + 1;
            if (val.size() >= 2 && val.front() == '\'' && val.back() == '\'')
                val = val.substr(1, val.size() - 2);
            a.values.push_back(dec(val));
            if (p < t.size() && t[p] == ')') {
                p++;
                break;
            }
        }
        out.push_back(std::move(a));
    }
    return out;
}

void Parser::apply_attributes() {
    if (!file_attrs_.empty()) {
        section_ = "data file attributes record (7/17)";
        size_t pos = 0;
        d_.file_attributes = parse_attributes(file_attrs_, &pos, false);
    }
    if (!var_attrs_.empty()) {
        section_ = "variable attributes record (7/18)";
        size_t pos = 0;
        const std::string &t = var_attrs_;
        while (pos < t.size()) {
            const size_t colon = t.find(':', pos);
            if (colon == std::string::npos) {
                warn("variable attributes record: trailing text was ignored");
                break;
            }
            const std::string name = dec(t.substr(pos, colon - pos));
            pos = colon + 1;
            std::vector<Attribute> attrs = parse_attributes(t, &pos, true);
            const int vi = var_by_name(name);
            if (vi < 0) {
                warn("variable attributes name " + name + ", which is not a variable of the file; ignored");
                continue;
            }
            Variable &v = d_.vars[static_cast<size_t>(vi)];
            for (auto &a : attrs) {
                if (a.name == "$@Role" && !a.values.empty()) {
                    const std::string &r = a.values[0];
                    if (r.size() == 1 && r[0] >= '0' && r[0] <= '5') v.role = r[0] - '0';
                    else warn("variable " + v.name + ": unknown role " + r + " ignored");
                    continue;
                }
                v.attributes.push_back(std::move(a));
            }
        }
    }
}

void Parser::apply_rest() {
    for (size_t i = 0; i < phys_.size(); i++) {
        const Phys &p = phys_[i];
        if (p.segment_tail) continue;
        const RawVar &r = raw_[p.raw];
        if (r.has_label) d_.vars[static_cast<size_t>(p.logical)].label = dec(r.label);
    }
    d_.file_label = rtrim_blanks(dec(rtrim_blanks(raw_file_label_)));
    /* the header's own texts are in the file's encoding too (a localized
     * product string, a label in a legacy code page): decoded like the rest */
    d_.product = dec(d_.product);
    d_.creation_date = dec(d_.creation_date);
    d_.creation_time = dec(d_.creation_time);
    d_.declared_encoding = dec(d_.declared_encoding);
    for (const auto &line : raw_documents_) d_.documents.push_back(dec(rtrim_blanks(line)));
    d_.product_info = dec(rtrim_blanks(raw_product_info_));
    d_.mrsets = dec(rtrim_blanks(raw_mrsets_));
    d_.varsets = dec(rtrim_blanks(raw_varsets_));
    /* the weight: a 1-based dictionary index (slot + 1) of a numeric variable */
    if (weight_index_ != 0) {
        const int found = (weight_index_ >= 1 && static_cast<size_t>(weight_index_) <= d_.nslots)
                              ? slot_start_[static_cast<size_t>(weight_index_ - 1)]
                              : -1;
        if (found >= 0 && !d_.vars[static_cast<size_t>(found)].is_string()) d_.weight_var = found;
        else warn("the weight index " + std::to_string(weight_index_) +
                  " does not name a numeric variable; no weight recorded");
    }
    /* the case count: the 64-bit record (7/16) wins over the 32-bit header */
    if (have_ext_ncases_ && ext_ncases_ >= 0) {
        d_.ncases = ext_ncases_;
        if (header_ncases_ >= 0 && header_ncases_ != ext_ncases_)
            warn("the header counts " + std::to_string(header_ncases_) +
                 " cases and record 7/16 counts " + std::to_string(ext_ncases_) +
                 "; the 64-bit count was used");
    } else {
        d_.ncases = header_ncases_ >= 0 ? header_ncases_ : -1;
    }
    /* a key labelled twice keeps its last label, in first-appearance order */
    for (auto &v : d_.vars) {
        if (v.labels.size() < 2) continue;
        std::vector<ValueLabel> kept;
        std::map<std::string, size_t> at; /* key -> index in kept */
        for (const auto &vl : v.labels) {
            std::string key;
            if (v.is_string()) {
                key = vl.text_key;
            } else {
                const double k = vl.number == 0.0 ? 0.0 : vl.number; /* -0 == 0 */
                key.assign(reinterpret_cast<const char *>(&k), sizeof k);
            }
            auto it = at.find(key);
            if (it != at.end()) {
                kept[it->second].label = vl.label;
                continue;
            }
            at.emplace(key, kept.size());
            kept.push_back(vl);
        }
        v.labels.swap(kept);
    }
}

void Parser::read_zlib_map() {
    section_ = "ZLIB data header";
    in_.seek(d_.data_offset);
    if (in_.remaining() < 24) fail("the file ends before the ZLIB header (truncated)");
    const int64_t zheader_ofs = static_cast<int64_t>(bits_of(text(8)));
    const int64_t ztrailer_ofs = static_cast<int64_t>(bits_of(text(8)));
    const int64_t ztrailer_len = static_cast<int64_t>(bits_of(text(8)));
    const int64_t fsize = static_cast<int64_t>(in_.size());
    if (zheader_ofs != static_cast<int64_t>(d_.data_offset))
        fail("the ZLIB header gives its own offset as " + std::to_string(zheader_ofs), d_.data_offset);
    if (ztrailer_len < 24 || ztrailer_len % 24 != 0 || ztrailer_ofs < zheader_ofs + 24 ||
        ztrailer_ofs > fsize || ztrailer_len > fsize - ztrailer_ofs)
        fail("the ZLIB trailer position or length is impossible (truncated file?)", d_.data_offset);
    section_ = "ZLIB trailer";
    in_.seek(static_cast<uint64_t>(ztrailer_ofs));
    (void)text(8); /* int_bias */
    (void)text(8); /* zero */
    const int32_t block_size = i32();
    const int32_t n_blocks = i32();
    if (n_blocks < 0 || static_cast<int64_t>(n_blocks) != (ztrailer_len - 24) / 24)
        fail("the ZLIB trailer counts " + std::to_string(n_blocks) + " blocks for " +
             std::to_string(ztrailer_len) + " bytes");
    if (block_size <= 0) fail("the ZLIB block size is not positive");
    int64_t expect_u = zheader_ofs, expect_c = zheader_ofs + 24;
    for (int32_t k = 0; k < n_blocks; k++) {
        Dictionary::ZBlock zb;
        zb.uncompressed_ofs = bits_of(text(8));
        zb.compressed_ofs = bits_of(text(8));
        zb.uncompressed_size = static_cast<uint32_t>(i32());
        zb.compressed_size = static_cast<uint32_t>(i32());
        if (static_cast<int64_t>(zb.uncompressed_ofs) != expect_u ||
            static_cast<int64_t>(zb.compressed_ofs) != expect_c ||
            zb.uncompressed_size == 0 || zb.compressed_size == 0 ||
            zb.uncompressed_size > static_cast<uint32_t>(block_size))
            fail("ZLIB block " + std::to_string(k + 1) + " of " + std::to_string(n_blocks) +
                 " is not where the previous blocks end");
        expect_u += zb.uncompressed_size;
        expect_c += zb.compressed_size;
        d_.zblocks.push_back(zb);
    }
    if (expect_c != ztrailer_ofs)
        fail("the ZLIB blocks end at " + offset_text(static_cast<uint64_t>(expect_c)) +
             " but the trailer starts at " + offset_text(static_cast<uint64_t>(ztrailer_ofs)));
}

Dictionary Parser::parse() {
    d_.file_size = in_.size();
    d_.sysmis_bits = 0xFFEFFFFFFFFFFFFFull;
    d_.highest_bits = 0x7FEFFFFFFFFFFFFFull;
    d_.lowest_bits = 0xFFEFFFFFFFFFFFFEull;
    read_header();
    for (;;) {
        section_ = "dictionary";
        const uint64_t at = in_.offset();
        const int32_t rt = i32();
        if (rt == 2) {
            section_ = "variable record " + std::to_string(raw_.size() + 1);
            read_variable();
        } else if (rt == 3) {
            section_ = "value-label record";
            read_value_labels();
        } else if (rt == 4) {
            fail("a value-label variables record (type 4) with no value-label record before it", at);
        } else if (rt == 6) {
            section_ = "document record";
            read_document();
        } else if (rt == 7) {
            read_extension();
        } else if (rt == 999) {
            section_ = "dictionary termination record";
            (void)i32();
            break;
        } else {
            fail("unknown record type " + std::to_string(rt), at);
        }
    }
    d_.data_offset = in_.offset();
    resolve_encoding();
    build_physical();
    if (phys_.empty())
        throw SavError("SPSS file " + path_ + " defines no variables; there is nothing to read");
    apply_very_long_strings();
    build_logical();
    apply_long_names();
    apply_missing();
    apply_value_labels();
    apply_long_string_labels();
    apply_long_string_missing();
    apply_display();
    apply_attributes();
    apply_rest();
    if (d_.compression == Compression::Zlib) read_zlib_map();
    return std::move(d_);
}

} // namespace

/* ---------------------------------------------------------------- public */

std::string format_name(const Format &f) {
    static const std::map<int, const char *> names = {
        {kFmtA, "A"}, {kFmtAHEX, "AHEX"}, {kFmtCOMMA, "COMMA"}, {kFmtDOLLAR, "DOLLAR"},
        {kFmtF, "F"}, {kFmtIB, "IB"}, {kFmtPIBHEX, "PIBHEX"}, {kFmtP, "P"}, {kFmtPIB, "PIB"},
        {kFmtPK, "PK"}, {kFmtRB, "RB"}, {kFmtRBHEX, "RBHEX"}, {kFmtZ, "Z"}, {kFmtN, "N"},
        {kFmtE, "E"}, {kFmtDATE, "DATE"}, {kFmtTIME, "TIME"}, {kFmtDATETIME, "DATETIME"},
        {kFmtADATE, "ADATE"}, {kFmtJDATE, "JDATE"}, {kFmtDTIME, "DTIME"}, {kFmtWKDAY, "WKDAY"},
        {kFmtMONTH, "MONTH"}, {kFmtMOYR, "MOYR"}, {kFmtQYR, "QYR"}, {kFmtWKYR, "WKYR"},
        {kFmtPCT, "PCT"}, {kFmtDOT, "DOT"}, {kFmtCCA, "CCA"}, {kFmtCCB, "CCB"},
        {kFmtCCC, "CCC"}, {kFmtCCD, "CCD"}, {kFmtCCE, "CCE"}, {kFmtEDATE, "EDATE"},
        {kFmtSDATE, "SDATE"}, {kFmtMTIME, "MTIME"}, {kFmtYMDHMS, "YMDHMS"}};
    auto it = names.find(f.type);
    if (it == names.end()) return std::string();
    std::string s = std::string(it->second) + std::to_string(f.width);
    /* numeric formats that take decimals always show them (F8.0), like SPSS */
    static const std::set<int> with_decimals = {kFmtCOMMA, kFmtDOLLAR, kFmtF, kFmtIB,
                                                kFmtP, kFmtPIB, kFmtPK, kFmtRB, kFmtZ,
                                                kFmtE, kFmtPCT, kFmtDOT, kFmtCCA, kFmtCCB,
                                                kFmtCCC, kFmtCCD, kFmtCCE};
    if (with_decimals.count(f.type) || f.decimals > 0) s += "." + std::to_string(f.decimals);
    return s;
}

Temporal temporal_of(int t) {
    switch (t) {
    case kFmtDATE: case kFmtADATE: case kFmtEDATE: case kFmtJDATE: case kFmtSDATE:
    case kFmtMOYR: case kFmtQYR: case kFmtWKYR:
        return Temporal::Date;
    case kFmtDATETIME: case kFmtYMDHMS:
        return Temporal::DateTime;
    case kFmtTIME: case kFmtMTIME:
        return Temporal::Time;
    case kFmtDTIME:
        return Temporal::Duration;
    default:
        return Temporal::None;
    }
}

bool MissingSpec::matches(double v) const {
    if (std::isnan(v)) return false;
    for (double x : values)
        if (v == x) return true;
    if (has_range) {
        const bool above = lo_open || v >= lo;
        const bool below = hi_open || v <= hi;
        if (above && below) return true;
    }
    return false;
}

bool decode_text(const Dictionary &d, const char *raw, size_t n, std::string *out) {
    const unsigned char *p = reinterpret_cast<const unsigned char *>(raw);
    bool ascii = true;
    for (size_t i = 0; i < n && ascii; i++) ascii = p[i] < 0x80;
    if (ascii) {
        out->assign(raw, n);
        return false;
    }
    if (d.utf8) {
        if (utf8_valid(p, n)) {
            out->assign(raw, n);
            return false;
        }
        *out = legacy_to_utf8(std::string(raw, n), d.legacy);
        return true;
    }
    *out = legacy_to_utf8(std::string(raw, n), d.legacy);
    return false;
}

Dictionary read_dictionary(const std::string &path, const ReadOptions &opt) {
    Parser p(path, opt);
    return p.parse();
}

/* ------------------------------------------------------ CaseReader streams */

struct CaseReader::Stream {
    virtual ~Stream() = default;
    /* up to n bytes; fewer only at the end of the data */
    virtual size_t read(unsigned char *dst, size_t n) = 0;
    /* where the next byte comes from, for messages */
    virtual std::string where() const = 0;
};

namespace {

class PlainStream : public CaseReader::Stream {
  public:
    PlainStream(const std::string &path, uint64_t start) : in_(path) { in_.seek(start); }
    size_t read(unsigned char *dst, size_t n) override { return in_.read(dst, n); }
    std::string where() const override { return offset_text(in_.offset()); }

  private:
    FileIn in_;
};

/* the bytecode stream of a .zsav: its ZLIB blocks inflated one at a time */
class ZlibStream : public CaseReader::Stream {
  public:
    ZlibStream(const std::string &path, const Dictionary &d) : in_(path), blocks_(d.zblocks) {}
    size_t read(unsigned char *dst, size_t n) override {
        size_t done = 0;
        while (done < n) {
            if (pos_ == out_.size()) {
                if (next_ == blocks_.size()) break;
                inflate_block();
            }
            const size_t take = std::min(n - done, out_.size() - pos_);
            std::memcpy(dst + done, out_.data() + pos_, take);
            pos_ += take;
            done += take;
        }
        return done;
    }
    std::string where() const override {
        return "ZLIB block " + std::to_string(next_) + " of " + std::to_string(blocks_.size()) +
               ", byte " + std::to_string(pos_) + " of its data";
    }

  private:
    void inflate_block() {
        const Dictionary::ZBlock &b = blocks_[next_];
        in_.seek(b.compressed_ofs);
        in_buf_.resize(b.compressed_size);
        if (in_.read(in_buf_.data(), b.compressed_size) != b.compressed_size)
            throw SavError("malformed SPSS file " + in_.path() + ": ZLIB block " +
                           std::to_string(next_ + 1) + " runs past the end of the file (truncated)");
        out_.resize(b.uncompressed_size);
        duckdb_miniz::mz_ulong got = b.uncompressed_size;
        const int st = duckdb_miniz::mz_uncompress(out_.data(), &got, in_buf_.data(),
                                                   static_cast<duckdb_miniz::mz_ulong>(b.compressed_size));
        if (st != duckdb_miniz::MZ_OK || got != b.uncompressed_size)
            throw SavError("malformed SPSS file " + in_.path() + ": ZLIB block " +
                           std::to_string(next_ + 1) + " of " +
                           std::to_string(blocks_.size()) + " at " + offset_text(b.compressed_ofs) +
                           " does not inflate to its declared " +
                           std::to_string(b.uncompressed_size) + " bytes (corrupt data)");
        pos_ = 0;
        next_++;
    }
    FileIn in_;
    std::vector<Dictionary::ZBlock> blocks_;
    std::vector<unsigned char> in_buf_, out_;
    size_t pos_ = 0, next_ = 0;
};

} // namespace

CaseReader::CaseReader(const std::string &path, const Dictionary &dict) : d_(dict), path_(path) {
    if (d_.compression == Compression::Zlib) in_ = std::make_unique<ZlibStream>(path, d_);
    else in_ = std::make_unique<PlainStream>(path, d_.data_offset);
    slots_.assign(d_.nslots, 0);
}

CaseReader::~CaseReader() = default;

bool CaseReader::next() {
    if (done_) return false;
    if (d_.ncases >= 0 && cases_ >= d_.ncases) {
        done_ = true;
        return false;
    }
    const bool got = d_.compression == Compression::None ? read_case_uncompressed()
                                                          : read_case_bytecode();
    if (!got) {
        done_ = true;
        if (d_.ncases >= 0 && cases_ < d_.ncases)
            throw SavError("malformed SPSS file " + path_ + ": the data ends after " +
                           std::to_string(cases_) + " cases but the file declares " +
                           std::to_string(d_.ncases) + " (truncated file)");
        return false;
    }
    cases_++;
    return true;
}

bool CaseReader::read_case_uncompressed() {
    const size_t bytes = d_.nslots * 8;
    std::vector<unsigned char> &buf = buf_;
    buf.resize(bytes);
    const size_t got = in_->read(buf.data(), bytes);
    if (got == 0) return false;
    if (got < bytes)
        throw SavError("malformed SPSS file " + path_ + ": case " + std::to_string(cases_ + 1) +
                       " is cut short at the end of the file (truncated)");
    const bool swap = d_.big_endian != host_big_endian();
    for (size_t i = 0; i < d_.nslots; i++) {
        uint64_t v;
        std::memcpy(&v, buf.data() + i * 8, 8);
        slots_[i] = (!d_.slot_is_string[i] && swap) ? bswap64(v) : v;
    }
    return true;
}

bool CaseReader::read_case_bytecode() {
    const bool swap = d_.big_endian != host_big_endian();
    for (size_t i = 0; i < d_.nslots; i++) {
        for (;;) {
            if (cmd_pos_ == 8) {
                const size_t got = in_->read(cmd_, 8);
                if (got == 0 && i == 0) return false;
                if (got < 8)
                    throw SavError("malformed SPSS file " + path_ +
                                   ": the compressed data ends inside case " +
                                   std::to_string(cases_ + 1) + " (truncated file)");
                cmd_pos_ = 0;
            }
            const unsigned char c = cmd_[cmd_pos_++];
            if (c == 0) continue; /* padding */
            const bool str = d_.slot_is_string[i];
            auto corrupt = [&](const std::string &what) {
                return SavError("malformed SPSS file " + path_ + ": " + what + " in case " +
                                std::to_string(cases_ + 1) + " (" + in_->where() +
                                "; corrupt compressed data)");
            };
            if (c == 252) {
                if (i == 0) return false;
                throw corrupt("the end-of-data code 252 appears in the middle of a case");
            }
            if (c == 253) {
                unsigned char b[8];
                if (in_->read(b, 8) != 8)
                    throw SavError("malformed SPSS file " + path_ +
                                   ": the compressed data ends inside case " +
                                   std::to_string(cases_ + 1) + " (truncated file)");
                uint64_t v;
                std::memcpy(&v, b, 8);
                slots_[i] = (!str && swap) ? bswap64(v) : v;
                break;
            }
            if (c == 254) {
                if (!str) throw corrupt("code 254 (eight blanks) stands for a numeric value");
                std::memset(&slots_[i], ' ', 8);
                break;
            }
            if (c == 255) {
                if (str) throw corrupt("code 255 (system-missing) stands for a string value");
                slots_[i] = d_.sysmis_bits;
                break;
            }
            if (str)
                throw corrupt("code " + std::to_string(int(c)) +
                              " (a compressed number) stands for a string value");
            const double v = double(c) - d_.bias;
            std::memcpy(&slots_[i], &v, 8);
            break;
        }
    }
    return true;
}

double CaseReader::number(const Variable &v) const {
    double out;
    std::memcpy(&out, &slots_[v.segments[0].slot], 8);
    return out;
}

bool CaseReader::is_sysmis(const Variable &v) const {
    return slots_[v.segments[0].slot] == d_.sysmis_bits;
}

void CaseReader::raw_string(const Variable &v, std::string *out) const {
    out->clear();
    for (const auto &s : v.segments) {
        const char *base = reinterpret_cast<const char *>(&slots_[s.slot]);
        out->append(base, std::min(s.used, s.nslots * 8));
    }
    while (!out->empty() && (out->back() == ' ' || out->back() == '\0')) out->pop_back();
}

} // namespace spss
} // namespace parqit
