/* parqit — legacy 8-bit text → UTF-8 (ENC-2).
 *
 * Parquet/Arrow/DuckDB strings must be valid UTF-8, but a Stata str#/strL cell,
 * a variable/data label, a value-label text, a note or a characteristic can
 * carry raw Latin-1/Windows-1252/MacRoman bytes: administrative data saved by
 * Stata 13 and earlier, or loaded into a Unicode Stata without `unicode
 * translate`. Instead of refusing such text (or corrupting the file), the save
 * path does here what `unicode translate` would do: text that is already valid
 * UTF-8 is kept byte-exact; text that is not is transcoded from a declared
 * single-byte code page, item by item. Every mapping is TOTAL (each of the 256
 * byte values has one code point), so the output is always valid UTF-8 and
 * the transcoding is reversible. No Stata API here — unit-tested in
 * tests/unit/test_legacy_encoding.cpp.
 */
#pragma once

#include <cstddef>
#include <string>

namespace parqit {

enum class LegacyEncoding { Windows1252, Latin1, Latin9, MacRoman };

/* Parses a user-facing encoding name, case-insensitively: "windows-1252" /
 * "cp1252" (the default, also chosen for an empty name), "latin1" /
 * "iso-8859-1", "latin9" / "iso-8859-15", "macroman" / "mac-roman".
 * Returns false on any other name (the caller refuses loudly). */
bool legacy_encoding_parse(const std::string &name, LegacyEncoding *out);

/* Canonical name for messages and r() results ("windows-1252", ...). */
const char *legacy_encoding_name(LegacyEncoding enc);

/* Strict well-formed UTF-8: rejects overlong forms, UTF-16 surrogates and
 * code points above U+10FFFF — the same boundary as the engine's
 * utf8_lossy walker and DuckDB's own VARCHAR validation. */
bool utf8_valid(const unsigned char *p, size_t n);
inline bool utf8_valid(const std::string &s) {
    return utf8_valid(reinterpret_cast<const unsigned char *>(s.data()), s.size());
}

/* Transcodes EVERY byte of `bytes` through the code page (0x00-0x7F are
 * ASCII in all four). Always returns valid UTF-8. */
std::string legacy_to_utf8(const std::string &bytes, LegacyEncoding enc);

/* The save-path policy, mirroring `unicode translate`'s default of leaving
 * strings that are already valid UTF-8 alone: returns false and leaves *s
 * untouched when it is valid UTF-8; otherwise replaces *s with its
 * transcoding and returns true. */
bool utf8_or_transcode(std::string *s, LegacyEncoding enc);

} // namespace parqit
