#include "engine/request.hpp"

#include "engine/hexcodec.hpp"

namespace parqit {

bool load_request(const std::string &path, json *out, std::string *err) {
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) {
        *err = "parqit: could not open request file";
        return false;
    }
    std::string content;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) content.append(buf, n);
    std::fclose(f);

    *out = json::parse(content, nullptr, false);
    if (out->is_discarded()) {
        *err = "parqit: malformed request (JSON parse failed)";
        return false;
    }
    return true;
}

bool req_text(const json &j, const char *key, std::string *out, std::string *err,
              bool required) {
    if (!j.contains(key)) {
        if (!required) {
            out->clear();
            return true;
        }
        *err = std::string("parqit: request missing field '") + key + "'";
        return false;
    }
    if (!j[key].is_string() || !hex_decode(j[key].get<std::string>(), *out)) {
        *err = std::string("parqit: request field '") + key + "' is not valid hex";
        return false;
    }
    return true;
}

bool req_text_list(const json &j, const char *key, std::vector<std::string> *out,
                   std::string *err, bool required) {
    out->clear();
    if (!j.contains(key)) {
        if (!required) return true;
        *err = std::string("parqit: request missing field '") + key + "'";
        return false;
    }
    if (!j[key].is_array()) {
        *err = std::string("parqit: request field '") + key + "' is not a list";
        return false;
    }
    for (const auto &item : j[key]) {
        std::string s;
        if (!item.is_string() || !hex_decode(item.get<std::string>(), s)) {
            *err = std::string("parqit: request field '") + key + "' has a non-hex entry";
            return false;
        }
        out->push_back(s);
    }
    return true;
}

ResponseWriter::~ResponseWriter() {
    if (f_) std::fclose(f_);
}

bool ResponseWriter::open(const std::string &path, std::string *err) {
    f_ = std::fopen(path.c_str(), "wb");
    if (!f_) {
        *err = "parqit: could not create response file";
        return false;
    }
    return true;
}

void ResponseWriter::rec(const std::string &kind, const std::vector<std::string> &plain,
                         const std::vector<std::string> &texts) {
    if (!f_) {
        failed_ = true;
        return;
    }
    std::string line = kind;
    for (const auto &p : plain) {
        line.push_back('|');
        line += p;
    }
    for (const auto &t : texts) {
        line.push_back('|');
        line += hex_encode(t);
    }
    line.push_back('\n');
    if (std::fwrite(line.data(), 1, line.size(), f_) != line.size()) failed_ = true;
}

bool ResponseWriter::close(std::string *err) {
    if (!f_) return !failed_;
    bool ok = (std::fflush(f_) == 0) && !failed_;
    ok = (std::fclose(f_) == 0) && ok;
    f_ = nullptr;
    if (!ok && err) *err = "parqit: failed writing response file";
    return ok;
}

} // namespace parqit
