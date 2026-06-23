/* parqit — source-column-name → Stata-variable-name sanitiser.
 *
 * The documented, reversible scheme of charter §6.2/§6.10/§6.14:
 *   1. every byte outside [A-Za-z0-9_] and outside UTF-8 continuation
 *      territory becomes "_" (Unicode letters pass through untouched);
 *   2. a leading digit or an exact reserved word gains a "_" prefix;
 *   3. names are truncated to 32 bytes on a UTF-8 character boundary;
 *   4. collisions (including with names already taken) get a numbered
 *      suffix, deterministically, case-sensitively;
 *   5. an empty result becomes v<position>.
 * The original name always travels in the manifest and in parqit.* metadata —
 * sanitisation never loses it.
 */
#pragma once

#include <string>
#include <vector>

namespace parqit {

bool is_reserved_stata_name(const std::string &name);

/* Sanitise one candidate (steps 1–3 + 5); no uniqueness handling. */
std::string sanitize_stata_name(const std::string &source, size_t position_1based);

/* Sanitise a whole column list with deterministic dedup (step 4).
 * renamed[i] is true when out[i] != sources[i]. */
std::vector<std::string> sanitize_unique(const std::vector<std::string> &sources,
                                         std::vector<bool> *renamed = nullptr);

} // namespace parqit
