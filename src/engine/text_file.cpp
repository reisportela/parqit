/* parqit — delimited text in any encoding → UTF-8 (CSV-ENC-1). See text_file.hpp. */
#include "engine/text_file.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace parqit {
namespace {

constexpr size_t kSniffBytes = size_t(64) << 10;

bool has_non_ascii(const char *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (static_cast<unsigned char>(p[i]) >= 0x80) return true;
    return false;
}

bool is_eol(char c) { return c == '\n' || c == '\r'; }

bool open_in(const std::string &path, size_t skip, std::ifstream *f, std::string *err) {
    f->open(fs::u8path(path), std::ios::in | std::ios::binary);
    if (!*f) {
        *err = "cannot open " + path;
        return false;
    }
    if (skip > 0) f->seekg(static_cast<std::streamoff>(skip));
    if (!*f) {
        *err = "cannot read " + path;
        return false;
    }
    return true;
}

/* appends up to `chunk` bytes; returns how many were read (0 = end of file) */
bool fill(std::ifstream &f, std::string *buf, size_t chunk, size_t *got,
          const std::string &path, std::string *err) {
    const size_t old = buf->size();
    buf->resize(old + chunk);
    f.read(&(*buf)[old], static_cast<std::streamsize>(chunk));
    *got = static_cast<size_t>(f.gcount());
    buf->resize(old + *got);
    if (f.bad()) {
        *err = "cannot read " + path;
        return false;
    }
    return true;
}

/* where the first max_line bytes of a line too long to keep whole are cut,
 * without splitting a character: anywhere in a single-byte code page; before
 * the last character in UTF-8; after the last byte below 0x30 in a multibyte
 * code page (no lead, trail or GB18030 digit byte is below 0x30). 0 when a
 * multibyte window has no such byte. The cut depends on the text alone, never
 * on how it was read, so the pieces are the same for every chunk size. */
size_t safe_cut(const unsigned char *u, size_t n, TextEncoding enc) {
    switch (enc.kind()) {
    case TextEncoding::Kind::SingleByte: return n;
    case TextEncoding::Kind::Utf8: {
        size_t p = n;
        const size_t lim = n >= 4 ? n - 4 : 0;
        while (p > lim && (u[p - 1] & 0xC0) == 0x80) p--;
        if (p > 0 && u[p - 1] >= 0xC0) p--; /* a lead byte starts the next piece */
        return p > 0 ? p : n;
    }
    default:
        for (size_t i = n; i > 0; i--)
            if (u[i - 1] < 0x30) return i;
        return 0;
    }
}

class Writer {
public:
    Writer(const std::string &path, TextDecodeStats *st) : path_(path), st_(st) {
        f_.open(fs::u8path(path), std::ios::out | std::ios::binary | std::ios::trunc);
    }
    bool ok() const { return static_cast<bool>(f_); }
    void put(const char *p, size_t n) {
        if (n == 0) return;
        for (size_t i = 0; i < n; i++)
            if (p[i] == '\n') st_->lines++;
        last_ = p[n - 1];
        st_->bytes_out += static_cast<long long>(n);
        f_.write(p, static_cast<std::streamsize>(n));
    }
    bool finish(std::string *err) {
        if (st_->bytes_out > 0 && last_ != '\n') st_->lines++; /* a last line without LF */
        f_.flush();
        if (!f_) {
            *err = "cannot write " + path_;
            return false;
        }
        f_.close();
        if (f_.fail()) {
            *err = "cannot write " + path_;
            return false;
        }
        return true;
    }

private:
    std::ofstream f_;
    std::string path_;
    TextDecodeStats *st_;
    char last_ = '\n';
};

/* The lines of a stream, each with its CR or LF, cut into pieces of at most
 * max_line bytes; `emit` sees each piece once. Returns false when a piece
 * cannot be cut safely (only a multibyte code page), with *cut_failed. */
template <class Emit>
bool split_lines(const std::string &buf, size_t *pos, size_t *searched, bool eof, size_t max_line,
                 TextEncoding enc, bool *cut_failed, Emit emit) {
    const auto u = reinterpret_cast<const unsigned char *>(buf.data());
    size_t i = *pos;
    for (;;) {
        size_t e = std::max(i, *searched);
        const size_t stop = std::min(buf.size(), i + max_line);
        while (e < stop && !is_eol(buf[e])) e++;
        if (e < stop) { /* a line, whole */
            emit(i, e + 1);
            i = e + 1;
            *searched = i;
            continue;
        }
        if (stop - i < max_line) { /* the rest of a line: wait for more, or end */
            *searched = e;
            if (eof && i < buf.size()) {
                emit(i, buf.size());
                i = buf.size();
            }
            break;
        }
        const size_t cut = safe_cut(u + i, max_line, enc);
        if (cut == 0) {
            *cut_failed = true;
            *pos = i;
            return false;
        }
        emit(i, i + cut);
        i += cut;
        *searched = i;
    }
    *pos = i;
    return true;
}

/* one line (with its CR or LF) of text in a byte-oriented encoding */
void decode_line(const char *p, size_t n, TextEncoding enc, bool all, std::string *piece,
                 Writer *w, TextDecodeStats *st) {
    if (text_ascii_unchanged(p, n, enc)) {
        w->put(p, n);
        return;
    }
    const bool valid = utf8_valid(reinterpret_cast<const unsigned char *>(p), n);
    const TextEncoding::Kind k = enc.kind();
    if (valid && (k == TextEncoding::Kind::Utf8 || (k == TextEncoding::Kind::SingleByte && !all))) {
        if (k == TextEncoding::Kind::SingleByte) st->lines_kept++;
        w->put(p, n);
        return;
    }
    if (valid) st->lines_revalid++; /* valid UTF-8, decoded as the file's text (multibyte, all) */
    st->undecodable += static_cast<long long>(legacy_decode(p, n, enc, piece));
    st->lines_decoded++;
    w->put(piece->data(), piece->size());
}

bool decode_bytes(std::ifstream &f, const std::string &src, TextEncoding enc, bool all,
                  size_t chunk, size_t max_line, Writer *w, TextDecodeStats *st,
                  std::string *err) {
    std::string buf, piece;
    size_t searched = 0; /* bytes of the current line known to hold no CR or LF */
    for (;;) {
        size_t got = 0;
        if (!fill(f, &buf, chunk, &got, src, err)) return false;
        st->bytes_in += static_cast<long long>(got);
        const bool eof = got == 0;
        size_t pos = 0;
        bool cut_failed = false;
        const bool ok = split_lines(buf, &pos, &searched, eof, max_line, enc, &cut_failed,
                                    [&](size_t a, size_t b) {
                                        decode_line(buf.data() + a, b - a, enc, all, &piece, w, st);
                                    });
        if (!ok) {
            *err = src + " has a line of more than " + std::to_string(max_line >> 20) +
                   " MiB with no byte at which " + std::string(enc.name()) +
                   " text can be cut safely; is it delimited text?";
            return false;
        }
        buf.erase(0, pos);
        searched -= pos;
        if (eof) return true;
    }
}

bool decode_utf16(std::ifstream &f, const std::string &src, TextEncoding enc, size_t chunk,
                  Writer *w, TextDecodeStats *st, std::string *err) {
    const bool big = enc.kind() == TextEncoding::Kind::Utf16Be;
    std::string buf, piece;
    for (;;) {
        size_t got = 0;
        if (!fill(f, &buf, chunk, &got, src, err)) return false;
        st->bytes_in += static_cast<long long>(got);
        const bool eof = got == 0;
        size_t end = eof ? buf.size() : buf.size() & ~size_t(1);
        if (!eof && end >= 2) {
            /* a high surrogate waits for its pair in the next chunk */
            const auto u = reinterpret_cast<const unsigned char *>(buf.data()) + end - 2;
            const uint32_t unit = big ? (uint32_t(u[0]) << 8) | u[1] : (uint32_t(u[1]) << 8) | u[0];
            if (unit >= 0xD800 && unit <= 0xDBFF) end -= 2;
        }
        if (end > 0) {
            st->undecodable += static_cast<long long>(legacy_decode(buf.data(), end, enc, &piece));
            w->put(piece.data(), piece.size());
            buf.erase(0, end);
        }
        if (eof) return true;
    }
}

/* the sample read as UTF-16 is text: no unpaired surrogate (a pair cut by
 * the end of the sample aside), no control character but tab, LF and CR, no
 * noncharacter U+FFFE/U+FFFF. Bytes that are not UTF-16 text fail at once:
 * one unit in 32 of random bytes is a surrogate, one in 2,000 a control. */
bool utf16_plausible(const unsigned char *u, size_t n, bool big) {
    const size_t units = n / 2;
    auto unit = [&](size_t i) -> uint32_t {
        return big ? (uint32_t(u[2 * i]) << 8) | u[2 * i + 1] : (uint32_t(u[2 * i + 1]) << 8) | u[2 * i];
    };
    for (size_t i = 0; i < units; i++) {
        const uint32_t w = unit(i);
        if (w >= 0xD800 && w <= 0xDBFF) {
            if (i + 1 == units) break; /* its pair lies past the sample */
            const uint32_t w2 = unit(i + 1);
            if (w2 < 0xDC00 || w2 > 0xDFFF) return false;
            i++;
            continue;
        }
        if (w >= 0xDC00 && w <= 0xDFFF) return false;
        if (w < 0x20 && w != 0x09 && w != 0x0A && w != 0x0D) return false;
        if (w == 0xFFFE || w == 0xFFFF) return false;
    }
    return true;
}

/* the sample up to its last ASCII byte (no UTF-8 sequence is cut there) is valid UTF-8 */
bool sample_utf8_valid(const unsigned char *u, size_t n) {
    size_t end = n;
    while (end > 0 && u[end - 1] >= 0x80) end--;
    return utf8_valid(u, end);
}

} // namespace

bool text_file_sniff(const std::string &path, TextSniffResult *out, std::string *err) {
    *out = TextSniffResult();
    std::ifstream f;
    if (!open_in(path, 0, &f, err)) return false;
    std::string b(kSniffBytes, '\0');
    f.read(&b[0], static_cast<std::streamsize>(b.size()));
    if (f.bad()) {
        *err = "cannot read " + path;
        return false;
    }
    b.resize(static_cast<size_t>(f.gcount()));
    const auto u = reinterpret_cast<const unsigned char *>(b.data());
    const size_t n = b.size();
    if (n >= 4 && ((u[0] == 0xFF && u[1] == 0xFE && u[2] == 0 && u[3] == 0) ||
                   (u[0] == 0 && u[1] == 0 && u[2] == 0xFE && u[3] == 0xFF))) {
        out->kind = TextSniff::Utf32Bom;
        out->bom_bytes = 4;
        return true;
    }
    if (n >= 3 && u[0] == 0xEF && u[1] == 0xBB && u[2] == 0xBF) {
        out->kind = TextSniff::Utf8Bom;
        out->bom_bytes = 3;
        return true;
    }
    if (n >= 2 && u[0] == 0xFF && u[1] == 0xFE) {
        out->kind = TextSniff::Utf16LeBom;
        out->bom_bytes = 2;
        return true;
    }
    if (n >= 2 && u[0] == 0xFE && u[1] == 0xFF) {
        out->kind = TextSniff::Utf16BeBom;
        out->bom_bytes = 2;
        return true;
    }
    size_t even = 0, odd = 0, res[4] = {0, 0, 0, 0}, slots[4] = {0, 0, 0, 0};
    size_t le_even = 0, le_odd = 0, be_even = 0, be_odd = 0;
    for (size_t i = 0; i < n; i++) {
        slots[i & 3]++;
        if (u[i] == 0) {
            (i & 1 ? odd : even)++;
            res[i & 3]++;
        }
        if (i + 1 < n) {
            if (u[i] == 0x0A && u[i + 1] == 0) (i & 1 ? le_odd : le_even)++;
            if (u[i] == 0 && u[i + 1] == 0x0A) (i & 1 ? be_odd : be_even)++;
        }
    }
    const size_t nul = even + odd, pairs = n / 2 > 0 ? n / 2 : 1;
    out->nul_bytes = nul;
    if (nul * 10 >= pairs) {
        if (odd * 20 >= nul * 19) out->kind = TextSniff::Utf16Le;
        else if (even * 20 >= nul * 19) out->kind = TextSniff::Utf16Be;
        else if (n >= 8) {
            auto most = [&](int k) { return res[k] * 20 >= slots[k] * 19; };
            auto few = [&](int k) { return res[k] * 20 <= slots[k]; };
            if (most(2) && most(3) && few(0)) out->kind = TextSniff::Utf32Le;
            else if (most(0) && most(1) && few(3)) out->kind = TextSniff::Utf32Be;
        }
        return true;
    }
    /* few NUL bytes: UTF-16 text in scripts other than Latin, by its line
     * feeds — and only if the sample, read as UTF-16, is text */
    if (!sample_utf8_valid(u, n)) {
        if (le_even >= 2 && le_odd * 10 <= le_even && be_even == 0 && utf16_plausible(u, n, false)) {
            out->kind = TextSniff::Utf16Le;
            out->by_line_ends = true;
        } else if (be_even >= 2 && be_odd * 10 <= be_even && le_even == 0 &&
                   utf16_plausible(u, n, true)) {
            out->kind = TextSniff::Utf16Be;
            out->by_line_ends = true;
        }
    }
    return true;
}

bool text_file_utf8_scan(const std::string &path, size_t skip, bool *valid, bool *non_ascii,
                         std::string *err, size_t chunk, size_t max_line) {
    std::ifstream f;
    if (!open_in(path, skip, &f, err)) return false;
    *valid = true;
    *non_ascii = false;
    std::string buf;
    size_t searched = 0;
    for (;;) {
        size_t got = 0;
        if (!fill(f, &buf, chunk, &got, path, err)) return false;
        const bool eof = got == 0;
        size_t pos = 0;
        bool cut_failed = false;
        split_lines(buf, &pos, &searched, eof, max_line, TextEncoding::Utf8, &cut_failed,
                    [&](size_t a, size_t b) {
                        const auto u = reinterpret_cast<const unsigned char *>(buf.data()) + a;
                        if (*valid && has_non_ascii(buf.data() + a, b - a)) {
                            *non_ascii = true;
                            if (!utf8_valid(u, b - a)) *valid = false;
                        }
                    });
        if (!*valid) return true;
        buf.erase(0, pos);
        searched -= pos;
        if (eof) return true;
    }
}

bool text_file_decode(const std::string &src, const std::string &dest, TextEncoding enc,
                      bool all, size_t skip, TextDecodeStats *st, std::string *err,
                      size_t chunk, size_t max_line) {
    *st = TextDecodeStats();
    if (chunk < 2) chunk = 2;
    std::ifstream f;
    if (!open_in(src, skip, &f, err)) return false;
    Writer w(dest, st);
    if (!w.ok()) {
        *err = "cannot write " + dest;
        return false;
    }
    /* past its byte-order mark, UTF-16 without a stated byte order is
     * little-endian (Windows) */
    if (enc.kind() == TextEncoding::Kind::Utf16) legacy_encoding_parse("utf-16le", &enc);
    const bool ok = enc.is_utf16() ? decode_utf16(f, src, enc, chunk, &w, st, err)
                                   : decode_bytes(f, src, enc, all, chunk, max_line, &w, st, err);
    if (!ok || !w.finish(err)) return false;
    if (enc.is_utf16()) st->lines_decoded = st->lines; /* all of it */
    return true;
}

bool text_file_plan(const TextPlanRequest &req, TextPlan *plan, std::string *err) {
    *plan = TextPlan();
    TextEncoding given;
    const bool has_given = !req.encoding.empty();
    if (has_given) {
        if (!legacy_encoding_parse(req.encoding, &given)) {
            *err = "encoding(" + req.encoding + ") is not an encoding parqit reads; it reads " +
                   legacy_encoding_families() + ", and utf-16le/utf-16be";
            plan->usage_error = true;
            return false;
        }
        plan->given = given.name();
    }
    TextEncoding le, be, utf8 = TextEncoding::Utf8;
    legacy_encoding_parse("utf-16le", &le);
    legacy_encoding_parse("utf-16be", &be);

    TextSniffResult sniff;
    if (!text_file_sniff(req.path, &sniff, err)) return false;
    plan->has_nul = sniff.nul_bytes > 0;
    const char *pattern = nullptr; /* a pattern that encoding() overrules */
    switch (sniff.kind) {
    case TextSniff::Utf32Bom:
        *err = req.path + " starts with a UTF-32 byte-order mark; parqit reads delimited "
                          "text in UTF-8, UTF-16 and the legacy code pages — save it as UTF-8";
        return false;
    case TextSniff::Utf16LeBom:
    case TextSniff::Utf16BeBom:
        plan->decode = true;
        plan->enc = sniff.kind == TextSniff::Utf16LeBom ? le : be;
        plan->skip = sniff.bom_bytes;
        plan->found = sniff.kind == TextSniff::Utf16LeBom ? "utf-16le bom" : "utf-16be bom";
        plan->bom_overrode = has_given && given != plan->enc &&
                             given.kind() != TextEncoding::Kind::Utf16;
        return true;
    case TextSniff::Utf16Le:
    case TextSniff::Utf16Be: {
        const bool is_be = sniff.kind == TextSniff::Utf16Be;
        if (has_given && given.is_utf16()) {
            plan->decode = true;
            plan->enc = given.kind() != TextEncoding::Kind::Utf16 ? given : is_be ? be : le;
            if (plan->enc != (is_be ? be : le)) /* the byte order given is followed, and said */
                plan->pattern_overruled = is_be ? "utf-16be" : "utf-16le";
            return true;
        }
        if (has_given) {
            pattern = is_be ? "utf-16be" : "utf-16le"; /* the declaration is followed */
            break;
        }
        plan->decode = true;
        plan->enc = is_be ? be : le;
        plan->found = std::string(is_be ? "utf-16be" : "utf-16le") +
                      (sniff.by_line_ends ? " lines" : " nul");
        return true;
    }
    case TextSniff::Utf32Le:
    case TextSniff::Utf32Be: {
        const char *name = sniff.kind == TextSniff::Utf32Le ? "UTF-32LE" : "UTF-32BE";
        if (has_given) {
            pattern = sniff.kind == TextSniff::Utf32Le ? "utf-32le" : "utf-32be";
            break;
        }
        *err = req.path + " looks like " + name + " text without a byte-order mark (three NUL "
               "bytes in every four), which parqit does not read; save it as UTF-8, or give "
               "encoding() if it is text in another encoding";
        return false;
    }
    case TextSniff::Utf8Bom:
    case TextSniff::Plain:
        break;
    }
    if (pattern) plan->pattern_overruled = pattern;

    plan->skip = sniff.bom_bytes;
    if (sniff.kind == TextSniff::Utf8Bom) {
        plan->found = "utf-8 bom";
        plan->bom_overrode = has_given && !given.is_utf8();
    }
    const bool utf8_declared = (has_given && given.is_utf8()) || sniff.kind == TextSniff::Utf8Bom;
    if (has_given && given.is_utf16() && sniff.kind != TextSniff::Utf8Bom) {
        /* UTF-16 as declared: no byte-order mark means little-endian (Windows) */
        plan->decode = true;
        plan->enc = given.kind() == TextEncoding::Kind::Utf16 ? le : given;
        return true;
    }
    if (utf8_declared || !has_given) {
        /* UTF-8: a file scanned in place is left to the engine (it refuses
         * invalid text, naming encoding()); otherwise invalid text is found
         * here and decoded — as UTF-8 with U+FFFD when UTF-8 is declared, from
         * the session default when nothing is */
        if (!has_given && !req.check_undeclared) return true;
        bool valid = true, non_ascii = false;
        if (!text_file_utf8_scan(req.path, plan->skip, &valid, &non_ascii, err)) return false;
        if (valid) return true;
        plan->decode = true;
        if (utf8_declared) plan->enc = utf8;
        else {
            plan->enc = req.session_default;
            plan->defaulted = true;
        }
        return true;
    }
    /* a legacy code page, as declared */
    plan->enc = given;
    if (!req.all) {
        /* A wholly UTF-8 source stays in place, including filename(). A
         * multibyte legacy file is otherwise decoded as a whole. */
        bool valid = true, non_ascii = false;
        if (!text_file_utf8_scan(req.path, plan->skip, &valid, &non_ascii, err)) return false;
        if (valid) {
            plan->kept_valid = non_ascii;
            return true;
        }
    }
    plan->decode = true;
    return true;
}

} // namespace parqit
