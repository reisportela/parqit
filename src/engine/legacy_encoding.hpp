/* parqit — legacy text → UTF-8 (ENC-2, ENC-3).
 *
 * Parquet/Arrow/DuckDB strings must be valid UTF-8, but a Stata str#/strL cell,
 * a variable/data label, a value-label text, a note or a characteristic can
 * carry the raw bytes of a legacy code page: administrative data saved by
 * Stata 13 and earlier, or loaded into a Unicode Stata without `unicode
 * translate`; SPSS and R files and delimited text declare (or not) the code
 * page of their text too. Instead of refusing such text (or corrupting the
 * file), parqit decodes it from a named encoding, item by item.
 *
 * The encodings (ENC-3): UTF-8; 49 single-byte code pages — Windows 874 and
 * 1250-1258, ISO-8859-1 to -16, KOI8-R/U, the DOS (IBM) code pages, the Mac
 * code pages; the Microsoft double-byte code pages 932 (Shift_JIS), 936
 * (GBK/GB2312), 949 (EUC-KR/UHC) and 950 (Big5); EUC-JP; GB18030; and UTF-16
 * for whole files. The tables are generated from the standard mappings
 * (engine/encoding_tables, tools/gen_encoding_tables.py). Decoding always
 * yields valid UTF-8: a byte sequence the encoding does not define becomes
 * U+FFFD and is counted, so the caller can say so — never silently. No Stata
 * API here — unit-tested in tests/unit/test_legacy_encoding.cpp.
 */
#pragma once

#include <cstddef>
#include <string>

namespace parqit {

/* A text encoding parqit decodes. A small value type; the default is
 * windows-1252 (parqit's default code page for undeclared legacy text). */
class TextEncoding {
public:
    enum class Kind { SingleByte, DoubleByte, EucJp, Gb18030, Utf8, Utf16Le, Utf16Be, Utf16 };
    constexpr TextEncoding() : id_(0) {}
    constexpr explicit TextEncoding(int id) : id_(id) {}
    static const TextEncoding Windows1252, Latin1, Latin9, MacRoman, Utf8;
    const char *name() const;
    Kind kind() const;
    bool is_utf8() const { return kind() == Kind::Utf8; }
    /* a character can take more than one byte (double-byte, EUC-JP, GB18030) */
    bool multibyte() const;
    /* UTF-16 applies to whole files (delimited text) only */
    bool is_utf16() const;
    bool ascii_identity() const; /* false for CP864 and UTF-16 */
    int id() const { return id_; }
    bool operator==(const TextEncoding &o) const { return id_ == o.id_; }
    bool operator!=(const TextEncoding &o) const { return id_ != o.id_; }

private:
    int id_;
};

/* the ENC-2 name, kept for its callers */
using LegacyEncoding = TextEncoding;

/* Parses a user-facing encoding name, case-insensitively and ignoring the
 * characters - _ . and blanks ("Windows-1251" = "cp1251", "Shift_JIS" =
 * "sjis" = "cp932", "GBK" = "gb2312" = "cp936", "EUC-KR" = "cp949",
 * "Big5" = "cp950", "KOI8-R", "ISO-8859-5", "latin2", "ibm866", ...). An
 * empty name is windows-1252, the default. Returns false on a name parqit
 * cannot decode (the caller refuses loudly, listing the families). */
bool legacy_encoding_parse(const std::string &name, LegacyEncoding *out);

/* The canonical name for messages and r() results ("windows-1252", "latin1",
 * "latin9", "macroman", "windows-1251", "windows-936", "euc-jp", ...). */
const char *legacy_encoding_name(LegacyEncoding enc);

/* One line naming the encodings parqit decodes, for error messages. */
const char *legacy_encoding_families();

/* CSV-ENC-1: DuckDB's error for delimited text that is not UTF-8 suggests its
 * own reader options, which parqit does not pass through; the message is
 * returned with that advice replaced by parqit's encoding() (any other
 * message is returned unchanged). */
std::string with_encoding_hint(const std::string &msg);

/* Strict well-formed UTF-8: rejects overlong forms, UTF-16 surrogates and
 * code points above U+10FFFF — the same boundary as the engine's
 * utf8_lossy walker and DuckDB's own VARCHAR validation. */
bool utf8_valid(const unsigned char *p, size_t n);
inline bool utf8_valid(const std::string &s) {
    return utf8_valid(reinterpret_cast<const unsigned char *>(s.data()), s.size());
}

/* Decodes n bytes from `enc` into valid UTF-8 (*out is replaced). Returns the
 * number of byte sequences the encoding does not define; each became U+FFFD.
 * UTF-8 as the encoding validates: an invalid sequence becomes U+FFFD. */
size_t legacy_decode(const char *p, size_t n, LegacyEncoding enc, std::string *out);

/* All bytes are ASCII and have the same meaning in this encoding. CP864's
 * percent byte is not ASCII, even though it is below 128. */
bool text_ascii_unchanged(const char *p, size_t n, TextEncoding enc);

/* Transcodes every byte of `bytes` through the encoding (legacy_decode
 * without the count). Always returns valid UTF-8. */
std::string legacy_to_utf8(const std::string &bytes, LegacyEncoding enc);

/* The save-path policy, mirroring `unicode translate`'s default of leaving
 * strings that are already valid UTF-8 alone: returns false and leaves *s
 * untouched when it is valid UTF-8; otherwise replaces *s with its
 * transcoding and returns true. */
bool utf8_or_transcode(std::string *s, LegacyEncoding enc);

} // namespace parqit
