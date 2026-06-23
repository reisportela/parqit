/* parqit — ado↔plugin protocol: request file parsing and response file
 * writing. Requests are JSON whose every user-originated string value is
 * lowercase hex of UTF-8 bytes (written by the Mata helpers in parqit.ado);
 * responses are line records `kind|field|field|...` with all text fields hex
 * encoded (hex never contains '|'), applied by Mata. No Stata API here —
 * unit-testable.
 */
#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "json.hpp"

namespace parqit {

using json = nlohmann::json;

/* Reads and parses a request file. Returns false with a message suitable
 * for SF_error on any malformed input (missing file, bad JSON). */
bool load_request(const std::string &path, json *out, std::string *err);

/* Hex-decoded accessors; *err set on malformed or missing-but-required. */
bool req_text(const json &j, const char *key, std::string *out, std::string *err,
              bool required = true);
bool req_text_list(const json &j, const char *key, std::vector<std::string> *out,
                   std::string *err, bool required = true);

/* Line-record response writer. Text fields are hex-encoded by rec(). */
class ResponseWriter {
  public:
    ~ResponseWriter();
    bool open(const std::string &path, std::string *err);
    /* writes: kind|plain1|...|hex(text1)|...  (plain fields first) */
    void rec(const std::string &kind, const std::vector<std::string> &plain,
             const std::vector<std::string> &texts);
    bool close(std::string *err);

  private:
    std::FILE *f_ = nullptr;
    bool failed_ = false;
};

} // namespace parqit
