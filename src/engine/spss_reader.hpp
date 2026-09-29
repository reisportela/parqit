/* parqit — native reader for SPSS system files (.sav and ZLIB-compressed .zsav).
 *
 * Written from the published description of the format (GNU PSPP, "System
 * File Format") and cross-checked against an independent implementation
 * (ReadStat); the Stata tests compare the result with three independent
 * readers (pyreadstat, Stata's import spss, pyarrow on the Parquet written).
 * No Stata API and no DuckDB here, so the parser is unit-tested on synthetic
 * files (tests/unit/test_spss_reader.cpp).
 *
 * The whole dictionary is read — names (long and short), variable labels,
 * value labels (numeric, short and long string), missing-value definitions
 * (discrete, ranges, long-string), print/write formats, measurement level,
 * display width, alignment, role and custom attributes, documents, the file
 * label, data-file attributes, multiple-response and variable sets, the
 * weight variable, the product and creation stamps and the character
 * encoding — and the data in all three layouts (uncompressed, bytecode,
 * ZLIB), both byte orders, very long strings (> 255 bytes) and 64-bit case
 * counts. A malformed or unsupported file is a loud SavError naming the byte
 * offset and the record; nothing is guessed silently (SPSS-READ-1).
 */
#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "engine/legacy_encoding.hpp"

namespace parqit {
namespace spss {

/* A malformed or unsupported file; what() names the offset and the record. */
class SavError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

enum class Compression { None, Bytecode, Zlib };

/* SPSS format type codes of the print/write formats (variable record). */
enum FormatType : int {
    kFmtA = 1, kFmtAHEX = 2, kFmtCOMMA = 3, kFmtDOLLAR = 4, kFmtF = 5,
    kFmtIB = 6, kFmtPIBHEX = 7, kFmtP = 8, kFmtPIB = 9, kFmtPK = 10,
    kFmtRB = 11, kFmtRBHEX = 12, kFmtZ = 15, kFmtN = 16, kFmtE = 17,
    kFmtDATE = 20, kFmtTIME = 21, kFmtDATETIME = 22, kFmtADATE = 23,
    kFmtJDATE = 24, kFmtDTIME = 25, kFmtWKDAY = 26, kFmtMONTH = 27,
    kFmtMOYR = 28, kFmtQYR = 29, kFmtWKYR = 30, kFmtPCT = 31, kFmtDOT = 32,
    kFmtCCA = 33, kFmtCCB = 34, kFmtCCC = 35, kFmtCCD = 36, kFmtCCE = 37,
    kFmtEDATE = 38, kFmtSDATE = 39, kFmtMTIME = 40, kFmtYMDHMS = 41
};

struct Format {
    int type = 0, width = 0, decimals = 0;
};
/* "F8.2", "A20", "DATETIME23.2"; "" for a type code the format does not define */
std::string format_name(const Format &f);

/* How a numeric format reads its value: a date (seconds since 14 Oct 1582,
 * shown as a day, month, quarter or week), a date-time, a time of day
 * (TIME/MTIME) or a duration in days (DTIME); None for every other format. */
enum class Temporal { None, Date, DateTime, Time, Duration };
Temporal temporal_of(int format_type);

/* The missing-value definition of one variable (SPSS MISSING VALUES). */
struct MissingSpec {
    std::vector<double> values;         /* discrete numeric values (0-3)       */
    bool has_range = false;             /* numeric range lo THRU hi             */
    double lo = 0.0, hi = 0.0;
    bool lo_open = false, hi_open = false; /* LO / HI (LOWEST, HIGHEST)       */
    std::vector<std::string> strings;   /* discrete string values (UTF-8, trailing blanks trimmed) */
    bool empty() const { return values.empty() && !has_range && strings.empty(); }
    /* true when the numeric value is user-missing under this definition */
    bool matches(double v) const;
};

struct Attribute {
    std::string name;                   /* as written (UTF-8) */
    std::vector<std::string> values;    /* one per array element */
};

struct ValueLabel {
    double number = 0.0;                /* key of a numeric variable */
    std::string text_key;               /* key of a string variable (UTF-8, trimmed) */
    std::string label;                  /* UTF-8 */
};

struct Variable {
    std::string name;                   /* long name, UTF-8 */
    std::string short_name;             /* 8-byte record name, UTF-8, trimmed */
    int width = 0;                      /* 0 numeric; else string width in bytes */
    /* the case slots holding the value: one per numeric variable; for a
     * string, one segment per physical variable record group (very long
     * strings have several), each with the bytes it actually carries */
    struct Segment {
        size_t slot = 0;                /* first 8-byte slot of the segment */
        size_t nslots = 0;
        size_t used = 0;                /* bytes of this segment that hold data */
    };
    std::vector<Segment> segments;
    Format print, write;
    bool has_label = false;
    std::string label;                  /* UTF-8 */
    MissingSpec missing;
    int measure = 0;                    /* 0 unknown, 1 nominal, 2 ordinal, 3 scale */
    int display_width = -1;             /* -1 = not recorded */
    int alignment = -1;                 /* -1 not recorded, 0 left, 1 right, 2 center */
    int role = -1;                      /* -1 not recorded; SPSS $@Role code 0-5 */
    std::vector<Attribute> attributes;  /* custom attributes (without $@Role) */
    std::vector<ValueLabel> labels;     /* value labels, file order, last key wins */
    bool is_string() const { return width > 0; }
};

struct Dictionary {
    bool big_endian = false;
    Compression compression = Compression::None;
    double bias = 100.0;
    int64_t ncases = -1;                /* -1 = not recorded (read to the end) */
    std::string product;                /* header product string, trimmed */
    std::string creation_date, creation_time;
    std::string file_label;             /* UTF-8, trimmed */
    std::vector<std::string> documents; /* UTF-8 lines, trailing blanks trimmed */
    std::vector<Variable> vars;         /* logical variables, dictionary order */
    size_t nslots = 0;                  /* 8-byte slots per case */
    std::vector<bool> slot_is_string;   /* per slot */
    int weight_var = -1;                /* index into vars, -1 none */
    /* character encoding: what the file declares and what is used */
    std::string declared_encoding;      /* record 7/20 text, or "code page N" */
    bool utf8 = true;                   /* text decoded as UTF-8 ...          */
    LegacyEncoding legacy = LegacyEncoding::Windows1252; /* ... or this code page;
                                         * also the fallback for invalid UTF-8 */
    std::string encoding_used;          /* "UTF-8", "windows-1252", ... */
    uint64_t sysmis_bits = 0, highest_bits = 0, lowest_bits = 0;
    std::string product_info;           /* record 7/10 */
    std::string mrsets;                 /* records 7/7 and 7/19, as written */
    std::string varsets;                /* record 7/5, as written */
    std::vector<Attribute> file_attributes; /* record 7/17 */
    int machine_version[3] = {0, 0, 0}; /* record 7/3 release, when present */
    /* records read but not carried (their type and size), and every tolerated
     * anomaly: the converter reports them in one note, never silently */
    std::vector<std::string> ignored;
    std::vector<std::string> warnings;
    long long transcoded_meta = 0;      /* dictionary texts transcoded (ENC-2) */
    long long undecodable_meta = 0;     /* dictionary texts with bytes the declared
                                         * encoding does not define (ENC-3) */
    /* where the case data begins; the ZLIB block map for .zsav */
    uint64_t data_offset = 0;
    struct ZBlock {
        uint64_t uncompressed_ofs = 0, compressed_ofs = 0;
        uint32_t uncompressed_size = 0, compressed_size = 0;
    };
    std::vector<ZBlock> zblocks;
    uint64_t file_size = 0;
};

struct ReadOptions {
    /* "" = the encoding the file declares; otherwise utf-8 or one of the
     * legacy code pages of engine/legacy_encoding.hpp, replacing it */
    std::string encoding;
    /* the session's code page (parqit set encoding) for a file that declares
     * none, and for bytes of a UTF-8 file that are not UTF-8; "" = windows-1252 */
    std::string default_encoding;
};

/* Reads and validates the dictionary (and, for .zsav, the block map).
 * Throws SavError. */
Dictionary read_dictionary(const std::string &path, const ReadOptions &opt = ReadOptions());

/* Decodes one text item of the file (dictionary or data) to UTF-8 with the
 * dictionary's encoding. Returns true when a UTF-8 file held bytes that are
 * not valid UTF-8, which are then transcoded from the fallback code page
 * (ENC-2); a legacy code page is always decoded, which is not a transcoding.
 * *undecodable (optional) receives the count of byte sequences the declared
 * legacy encoding does not define, each now U+FFFD (ENC-3). */
bool decode_text(const Dictionary &d, const char *raw, size_t n, std::string *out,
                 size_t *undecodable = nullptr);

/* The encoding of an SPSS Windows code-page number (record 7/3), for the
 * code pages parqit decodes; false for any other (ENC-3). */
bool spss_code_page(int code_page, bool *utf8, LegacyEncoding *enc);

/* Sequential reader of the cases. Opens its own handle, so several readers of
 * one file never share a seek position. Throws SavError. */
class CaseReader {
  public:
    CaseReader(const std::string &path, const Dictionary &dict);
    ~CaseReader();
    CaseReader(const CaseReader &) = delete;
    CaseReader &operator=(const CaseReader &) = delete;

    /* Decodes the next case; false at the end of the data. */
    bool next();
    int64_t cases_read() const { return cases_; }

    /* the value of a numeric variable (SYSMIS returned as its bit pattern) */
    double number(const Variable &v) const;
    bool is_sysmis(const Variable &v) const;
    /* the bytes of a string variable, segments joined, trailing blanks and
     * NULs removed (SPSS pads with blanks; some writers with NULs) */
    void raw_string(const Variable &v, std::string *out) const;

    /* the byte source of the case data (the file, or its inflated ZLIB blocks) */
    struct Stream;

  private:
    const Dictionary &d_;
    std::string path_;                  /* for messages */
    std::unique_ptr<Stream> in_;
    std::vector<uint64_t> slots_;       /* numeric slots: native-order bits;
                                         * string slots: the 8 file bytes */
    std::vector<unsigned char> buf_;    /* one uncompressed case */
    int64_t cases_ = 0;
    bool done_ = false;
    unsigned char cmd_[8];
    int cmd_pos_ = 8;
    bool read_case_uncompressed();
    bool read_case_bytecode();
};

/* seconds since 14 Oct 1582 at 1 Jan 1970 (141,428 days) */
constexpr int64_t kSpssEpochToUnixSeconds = 12219379200LL;
constexpr int64_t kSpssEpochToUnixDays = 141428LL;

} // namespace spss
} // namespace parqit
