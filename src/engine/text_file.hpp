/* parqit — delimited text in any encoding → UTF-8 (CSV-ENC-1).
 *
 * DuckDB's CSV reader reads UTF-8 only, and reads UTF-16 text that has no
 * byte-order mark as one column of garbage without an error; Stata's import
 * delimited guesses the encoding of a file that does not declare it, and for
 * Cyrillic, Chinese or Japanese text guesses wrong in silence (no
 * observations, or mojibake). So parqit reads delimited text itself first:
 * the byte-order mark and the patterns of UTF-16 and UTF-32 are recognised,
 * the text is checked for UTF-8, and text in another encoding is decoded,
 * streaming, into a UTF-8 copy that the engine or Stata then reads instead.
 * No Stata API here — unit-tested in tests/unit/test_text_file.cpp.
 */
#pragma once

#include "engine/legacy_encoding.hpp"

#include <cstddef>
#include <string>

namespace parqit {

/* bytes read at a time */
constexpr size_t kTextChunk = size_t(4) << 20;
/* a line longer than this (no CR or LF in it) is cut at a byte that no
 * supported encoding uses inside a character */
constexpr size_t kTextMaxLine = size_t(64) << 20;

/* What the first bytes of a text file say about its encoding. */
enum class TextSniff {
    Plain,      /* no byte-order mark and no sign of UTF-16 or UTF-32 */
    Utf8Bom,    /* EF BB BF */
    Utf16LeBom, /* FF FE */
    Utf16BeBom, /* FE FF */
    Utf32Bom,   /* FF FE 00 00 or 00 00 FE FF: not read */
    Utf16Le,    /* no mark: NUL bytes at the odd offsets, or UTF-16LE line ends */
    Utf16Be,    /* no mark: NUL bytes at the even offsets, or UTF-16BE line ends */
    Utf32Le,    /* no mark: the NUL bytes of UTF-32LE text (not read) */
    Utf32Be     /* no mark: the NUL bytes of UTF-32BE text (not read) */
};

struct TextSniffResult {
    TextSniff kind = TextSniff::Plain;
    size_t bom_bytes = 0;      /* length of the byte-order mark */
    size_t nul_bytes = 0;      /* NUL bytes in the first 64 KiB */
    bool by_line_ends = false; /* UTF-16 recognised by its line ends, not its NUL bytes */
};

/* Reads the first 64 KiB of a file. A NUL byte is valid UTF-8 but is not
 * text; ASCII characters in UTF-16 have one in every other byte, and those in
 * UTF-32 three in every four. UTF-16 without a byte-order mark: NUL bytes fill
 * at least a tenth of the byte pairs and at least 95% of them sit at one
 * parity; or, with fewer NUL bytes (text in scripts other than Latin), the
 * sample is not UTF-8, its line feeds are UTF-16 line feeds (0A 00, or 00 0A,
 * at even offsets; at the opposite parity at most one in ten) and, read as
 * UTF-16, it is text (no unpaired surrogate, no control but tab/LF/CR). UTF-32
 * without a byte-order mark: the two high bytes of every four are NUL (at least
 * 95%) and the low byte is not (at most 5%). Any other NUL bytes — the padding
 * of fixed-width exports — are left to the reader (Plain). */
bool text_file_sniff(const std::string &path, TextSniffResult *out, std::string *err);

/* Scans the file from byte `skip` for text that is not valid UTF-8, stopping
 * at the first such line: *valid says whether all of it is valid UTF-8,
 * *non_ascii whether any byte read is not ASCII. */
bool text_file_utf8_scan(const std::string &path, size_t skip, bool *valid, bool *non_ascii,
                         std::string *err, size_t chunk = kTextChunk,
                         size_t max_line = kTextMaxLine);

struct TextDecodeStats {
    long long bytes_in = 0;      /* bytes read (after the byte-order mark) */
    long long bytes_out = 0;     /* UTF-8 bytes written */
    long long lines = 0;         /* lines written (line feeds, and a last line without one) */
    long long lines_decoded = 0; /* lines decoded from the encoding */
    long long lines_kept = 0;    /* lines with non-ASCII text kept because they were valid UTF-8 */
    long long lines_revalid = 0; /* decoded lines that were valid UTF-8 (multibyte or `all`) */
    long long undecodable = 0;   /* byte sequences the encoding does not define (each became U+FFFD) */
};

/* Decodes the file from byte `skip` into a UTF-8 copy at `dest` (without a
 * byte-order mark), streaming. A line ends at CR or LF, bytes that no
 * supported encoding uses inside a character, so the text is decoded line by
 * line (a line longer than max_line is cut at a byte that is safe for the
 * encoding — any byte in a single-byte code page, a character boundary in
 * UTF-8, a byte below 0x30 in a multibyte code page — and refused when a
 * multibyte one has none):
 *  - a line of ASCII text is copied;
 *  - UTF-8: in a line that is not valid UTF-8, each invalid sequence becomes
 *    U+FFFD;
 *  - a single-byte code page: a line that is valid UTF-8 is kept as it is
 *    (unless `all`), any other line is decoded;
 *  - a multibyte code page (Shift_JIS, GBK, UHC, Big5, EUC-JP, GB18030):
 *    every line with non-ASCII text is decoded;
 *  - UTF-16LE/BE: the whole file is decoded (`enc` names the byte order).
 * `dest` is replaced. */
bool text_file_decode(const std::string &src, const std::string &dest, TextEncoding enc,
                      bool all, size_t skip, TextDecodeStats *st, std::string *err,
                      size_t chunk = kTextChunk, size_t max_line = kTextMaxLine);

/* How to read one delimited text file (the policy, CSV-ENC-1). */
struct TextPlanRequest {
    std::string path;
    std::string encoding;       /* the encoding() given; "" = none */
    bool all = false;           /* encoding(name, all) */
    /* an undeclared file that is not valid UTF-8 is decoded from the session
     * default (a lookup file read into Stata) instead of being left for the
     * engine to refuse (a file scanned in place, which is not read twice) */
    bool check_undeclared = false;
    TextEncoding session_default;
};

struct TextPlan {
    bool decode = false;        /* false: the file itself is read, as UTF-8 */
    TextEncoding enc;           /* what it is decoded from */
    size_t skip = 0;            /* byte-order mark bytes */
    std::string found;          /* how UTF-8/16 was found without encoding(): "", "utf-8 bom",
                                   "utf-16le bom", "utf-16be bom", "utf-16le nul",
                                   "utf-16be nul", "utf-16le lines", "utf-16be lines" */
    std::string given;          /* the canonical name of encoding(); "" = none */
    bool bom_overrode = false;  /* a byte-order mark overrode encoding() */
    bool defaulted = false;     /* undeclared text that is not UTF-8: the session default */
    bool kept_valid = false;    /* a multibyte encoding() for text that is valid UTF-8 throughout */
    /* the bytes looked like UTF-16/32 without a byte-order mark ("utf-16le",
     * ...), but encoding() said otherwise and was followed */
    std::string pattern_overruled;
    bool has_nul = false;       /* the first 64 KiB hold NUL bytes */
    bool usage_error = false;   /* on failure: the options are wrong (not the file) */
};

/* Decides how the file is read; may scan it for UTF-8 (stopping at the
 * first line that is not). An encoding() given is followed over the patterns
 * of UTF-16 and UTF-32 (a byte-order mark, which the file itself declares,
 * overrides it). Returns false with *err on a file parqit cannot read as text
 * (UTF-32) or an encoding it does not know. */
bool text_file_plan(const TextPlanRequest &req, TextPlan *plan, std::string *err);

} // namespace parqit
