/* parqit — hex codec for the ado↔plugin protocol.
 *
 * Every user-originated string (paths, expressions, labels, column names)
 * crosses the Stata boundary as lowercase hex of its UTF-8 bytes, so no
 * quoting/escaping bug class can exist in either direction. The Mata twin of
 * this codec lives in parqit.ado; the two are covered by the same test vectors.
 */
#pragma once

#include <string>

namespace parqit {

std::string hex_encode(const std::string &raw);

/* Decodes lowercase/uppercase hex; returns false on odd length or any
 * non-hex character (the caller treats that as a malformed request). */
bool hex_decode(const std::string &hex, std::string &out);

} // namespace parqit
