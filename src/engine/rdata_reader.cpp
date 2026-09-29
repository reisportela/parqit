#include "engine/rdata_reader.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <random>
#include <set>
#include <sstream>

#include "miniz.hpp" /* DuckDB's bundled zlib subset (duckdb_miniz): gzip members */
#include "zstd.h"    /* DuckDB's bundled zstd (duckdb_zstd) */

namespace parqit {
namespace rdata {

namespace {

namespace fs = std::filesystem;

constexpr int32_t kObjBit = 1 << 8, kAttrBit = 1 << 9, kTagBit = 1 << 10;

/* how much of an attribute is loaded; larger values are described, not kept */
constexpr uint64_t kLoadMaxAtoms = uint64_t(1) << 24;
constexpr uint64_t kLoadMaxList = uint64_t(1) << 20;
constexpr int kMaxLoadDepth = 200;
/* a list inside the top object is searched for columns only when it is this
 * small (a list column of a data frame has one element per row) */
constexpr uint64_t kLocateMaxElts = 100000;
constexpr double kMaxRLength = 4503599627370496.0; /* R_XLEN_T_MAX, Rinternals.h */

const char *kResave = "re-save it in R with saveRDS(x, file) or save(x, file = ...), whose "
                      "default (gzip-compressed XDR) parqit reads";

std::string offset_text(uint64_t off) { return "byte offset " + std::to_string(off); }

inline uint32_t be32(const unsigned char *p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline uint64_t be64(const unsigned char *p) { return (uint64_t(be32(p)) << 32) | be32(p + 4); }
inline double real_of(uint64_t bits) {
    double d;
    std::memcpy(&d, &bits, 8);
    return d;
}
inline uint64_t bits_of(double d) {
    uint64_t b;
    std::memcpy(&b, &d, 8);
    return b;
}
inline int type_of(int32_t flags) { return flags & 0xFF; }
inline int levels_of(int32_t flags) { return static_cast<int>(static_cast<uint32_t>(flags) >> 12); }

/* ------------------------------------------------------- decompression */

std::string temp_path(const std::string &tmpdir) {
    static std::atomic<unsigned long long> seq{0};
    unsigned long long r = 0;
    try {
        std::random_device rd;
        r = (static_cast<unsigned long long>(rd()) << 32) ^ rd();
    } catch (...) {
        r = static_cast<unsigned long long>(
            std::chrono::steady_clock::now().time_since_epoch().count());
    }
    std::ostringstream os;
    os << "parqit_rdata_" << std::hex << r << "_" << std::dec << ++seq << ".tmp";
    std::error_code ec;
    const fs::path dir = tmpdir.empty() ? fs::temp_directory_path(ec) : fs::u8path(tmpdir);
    return (dir / os.str()).u8string();
}

/* the compressed file, read front to back */
class ByteIn {
  public:
    ByteIn(std::ifstream &f, const std::string &path) : f_(f), path_(path), buf_(size_t(1) << 20) {}
    /* at least one byte buffered; false at the end of the file */
    bool fill() {
        if (at_ < len_) return true;
        f_.read(buf_.data(), static_cast<std::streamsize>(buf_.size()));
        len_ = static_cast<size_t>(f_.gcount());
        at_ = 0;
        if (len_ == 0 && f_.bad()) throw RError("read error in " + path_);
        return len_ > 0;
    }
    int byte() { return fill() ? static_cast<unsigned char>(buf_[at_++]) : -1; }
    int need_byte(const char *what) {
        const int b = byte();
        if (b < 0) throw RError("the " + std::string(what) + " data of " + path_ +
                                " ends early (the file is truncated)");
        return b;
    }
    const unsigned char *data() const { return reinterpret_cast<const unsigned char *>(buf_.data()) + at_; }
    size_t avail() const { return len_ - at_; }
    void consume(size_t n) { at_ += n; }

  private:
    std::ifstream &f_;
    std::string path_;
    std::vector<char> buf_;
    size_t at_ = 0, len_ = 0;
};

void write_out(std::ofstream &out, const void *p, size_t n, const std::string &path) {
    out.write(static_cast<const char *>(p), static_cast<std::streamsize>(n));
    if (!out) throw RError("could not write the decompressed copy of " + path +
                           " to the temporary directory (is the disk full?)");
}

/* gzip (RFC 1952), one member or several concatenated; each member's CRC-32
 * and length are checked */
uint64_t gunzip(ByteIn &in, std::ofstream &out, const std::string &path) {
    namespace mz = duckdb_miniz;
    std::vector<unsigned char> obuf(size_t(1) << 20);
    uint64_t total = 0;
    for (int member = 0;; member++) {
        const int id1 = in.byte();
        if (id1 < 0) {
            if (member == 0) throw RError(path + " is empty");
            break;
        }
        if (member > 0 && id1 == 0) { /* zero padding after the last member */
            int b;
            while ((b = in.byte()) == 0) {
            }
            if (b < 0) break;
            throw RError("the gzip data of " + path + " is followed by bytes that are not gzip data");
        }
        const int id2 = in.need_byte("gzip");
        if (id1 != 0x1f || id2 != 0x8b)
            throw RError("the gzip data of " + path + " is followed by bytes that are not gzip data");
        const int cm = in.need_byte("gzip"), flg = in.need_byte("gzip");
        if (cm != 8)
            throw RError("the gzip data of " + path + " uses compression method " +
                         std::to_string(cm) + ", not deflate");
        if (flg & 0xE0) throw RError("malformed gzip header in " + path);
        for (int i = 0; i < 6; i++) in.need_byte("gzip"); /* mtime, xfl, os */
        if (flg & 4) {
            const int lo = in.need_byte("gzip"), hi = in.need_byte("gzip");
            for (int i = 0; i < (lo | (hi << 8)); i++) in.need_byte("gzip");
        }
        if (flg & 8)
            while (in.need_byte("gzip") != 0) {
            }
        if (flg & 16)
            while (in.need_byte("gzip") != 0) {
            }
        if (flg & 2) {
            in.need_byte("gzip");
            in.need_byte("gzip");
        }
        mz::mz_stream z;
        std::memset(&z, 0, sizeof z);
        if (mz::mz_inflateInit2(&z, -MZ_DEFAULT_WINDOW_BITS) != mz::MZ_OK)
            throw RError("could not start the gzip decoder");
        struct End {
            mz::mz_stream *z;
            ~End() { mz::mz_inflateEnd(z); }
        } end{&z};
        mz::mz_ulong crc = 0;
        uint64_t isize = 0;
        for (;;) {
            const bool have = in.fill();
            z.next_in = in.data();
            z.avail_in = static_cast<unsigned int>(in.avail());
            z.next_out = obuf.data();
            z.avail_out = static_cast<unsigned int>(obuf.size());
            const unsigned int before = z.avail_in;
            const int st = mz::mz_inflate(&z, mz::MZ_NO_FLUSH);
            const size_t used = before - z.avail_in;
            in.consume(used);
            const size_t produced = obuf.size() - z.avail_out;
            if (produced) {
                crc = mz::mz_crc32(crc, obuf.data(), produced);
                write_out(out, obuf.data(), produced, path);
                isize += produced;
            }
            if (st == mz::MZ_STREAM_END) break;
            if (st != mz::MZ_OK && st != mz::MZ_BUF_ERROR)
                throw RError("the gzip data of " + path + " is corrupt");
            if (used == 0 && produced == 0) {
                if (!have) throw RError("the gzip data of " + path + " ends early (the file is truncated)");
                throw RError("the gzip data of " + path + " is corrupt");
            }
        }
        uint32_t tcrc = 0, tsize = 0;
        for (int i = 0; i < 4; i++) tcrc |= uint32_t(in.need_byte("gzip")) << (8 * i);
        for (int i = 0; i < 4; i++) tsize |= uint32_t(in.need_byte("gzip")) << (8 * i);
        if (tcrc != static_cast<uint32_t>(crc))
            throw RError("the gzip data of " + path + " fails its CRC-32 check (the file is corrupt)");
        if (tsize != static_cast<uint32_t>(isize))
            throw RError("the gzip data of " + path + " fails its length check (the file is corrupt)");
        total += isize;
    }
    return total;
}

uint64_t unzstd(ByteIn &in, std::ofstream &out, const std::string &path) {
    namespace zs = duckdb_zstd;
    zs::ZSTD_DStream *ds = zs::ZSTD_createDStream();
    if (!ds) throw RError("could not start the zstd decoder");
    struct Free {
        zs::ZSTD_DStream *ds;
        ~Free() { zs::ZSTD_freeDStream(ds); }
    } free_ds{ds};
    size_t r = zs::ZSTD_initDStream(ds);
    if (zs::ZSTD_isError(r)) throw RError("could not start the zstd decoder");
    std::vector<char> obuf(zs::ZSTD_DStreamOutSize());
    uint64_t total = 0;
    size_t last = 1;
    /* each chunk is decoded until its input is used up and the output is
     * flushed, so nothing is pending at the end of the file: `last` is then
     * the answer for the whole stream (0 = its last frame is complete) */
    while (in.fill()) {
        zs::ZSTD_inBuffer ib{in.data(), in.avail(), 0};
        bool full = false;
        do {
            zs::ZSTD_outBuffer ob{obuf.data(), obuf.size(), 0};
            last = zs::ZSTD_decompressStream(ds, &ob, &ib);
            if (zs::ZSTD_isError(last))
                throw RError("the zstd data of " + path + " is corrupt (" +
                             zs::ZSTD_getErrorName(last) + ")");
            if (ob.pos) {
                write_out(out, obuf.data(), ob.pos, path);
                total += ob.pos;
            }
            full = ob.pos == ob.size;
        } while (ib.pos < ib.size || full);
        in.consume(ib.pos);
    }
    if (last != 0) throw RError("the zstd data of " + path + " ends early (the file is truncated)");
    return total;
}

/* ------------------------------------------------------------- encodings */

/* 0 unknown, 1 UTF-8, 2 a legacy encoding (*enc), 3 ASCII (ENC-3: every
 * encoding of engine/legacy_encoding; UTF-16 is not a string encoding) */
int classify_encoding(const std::string &name, LegacyEncoding *enc) {
    std::string k;
    for (char c : name) {
        if (c == '-' || c == '_' || c == ' ' || c == '.') continue;
        k += (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
    }
    if (k.empty()) return 0;
    if (k == "ANSIX341968" || k == "USASCII" || k == "ASCII" || k == "646" || k == "C" ||
        k == "POSIX" || k == "ISO646US")
        return 3;
    LegacyEncoding e;
    if (!legacy_encoding_parse(name, &e) || e.is_utf16()) return 0;
    if (e.is_utf8()) return 1;
    *enc = e;
    return 2;
}

std::string type_name(int type) {
    switch (type) {
    case LGLSXP: return "logical";
    case INTSXP: return "integer";
    case REALSXP: return "numeric";
    case CPLXSXP: return "complex";
    case STRSXP: return "character";
    case RAWSXP: return "raw";
    default: return "type " + std::to_string(type);
    }
}

const RValue *attr_in(const Attrs &a, const std::string &name) {
    for (const auto &kv : a)
        if (kv.first == name) return kv.second.get();
    return nullptr;
}

std::vector<std::string> classes_in(const Attrs &a) {
    std::vector<std::string> out;
    const RValue *c = attr_in(a, "class");
    if (c && c->type == STRSXP && c->kept())
        for (size_t i = 0; i < c->strs.size(); i++)
            if (!c->na[i]) out.push_back(c->strs[i]);
    return out;
}

bool inherits_in(const Attrs &a, const std::string &cls) {
    for (const auto &c : classes_in(a))
        if (c == cls) return true;
    return false;
}

/* ---------------------------------------------------------------- parse */

/* one located item of the first pass */
struct Located {
    int type = NILSXP;
    VecRef vec;                 /* LGLSXP/INTSXP/REALSXP/STRSXP: where the data are */
    uint64_t length = 0;
    std::vector<Located> elts;  /* VECSXP: its elements, when located */
    bool elts_located = false;
    Attrs attrs;
    bool has_rownames = false;  /* VECSXP: its row.names attribute, located */
    int rn_type = NILSXP;
    VecRef rn_vec;
    std::string rn_unsupported;
    std::string unsupported;    /* a vector parqit cannot read, and why */
    std::string what;           /* what a non-vector object is */
    bool deferred = false;      /* a deferred as.character() of doubles (vec is them) */
};

enum class RefKind { Symbol, Env, ExtPtr, WeakRef, Persist, Package, Namespace };
struct Ref {
    RefKind kind;
    std::string name;
};

enum class FK : uint8_t { Items, BCConsts, BCLang, BCConstsHeader };
struct SkipFrame {
    FK kind;
    uint64_t n;
};

class Parser {
  public:
    explicit Parser(std::shared_ptr<Source> src) : src_(std::move(src)), buf_(size_t(1) << 20) {}
    void set_codec(std::shared_ptr<const TextCodec> c) { codec_ = std::move(c); }
    long long transcoded() const { return transcoded_; }
    uint64_t pos() const { return base_ + at_; }

    void header(FileInfo *info);
    int32_t i32() {
        need(4);
        const int32_t v = static_cast<int32_t>(be32(ptr()));
        at_ += 4;
        return v;
    }
    Located locate(int32_t flags, int level, int deep);
    std::string tag_name();
    void skip_items(uint64_t count) {
        std::vector<SkipFrame> st;
        st.push_back({FK::Items, count});
        run(st);
    }
    void skip_rest(int32_t flags) {
        std::vector<SkipFrame> st;
        skip_body(flags, st);
        run(st);
    }
    std::shared_ptr<RValue> load_item(int depth) { return load_body(i32(), depth); }
    std::shared_ptr<RValue> load_body(int32_t flags, int depth);
    RError malformed(const std::string &what) const {
        return RError("the R data file " + src_->path() + " is malformed (" + what + " at " +
                      offset_text(pos()) + ")");
    }

  private:
    const unsigned char *ptr() const { return reinterpret_cast<const unsigned char *>(buf_.data()) + at_; }
    void need(size_t n) {
        if (len_ - at_ >= n) return;
        if (at_ > 0) {
            std::memmove(buf_.data(), buf_.data() + at_, len_ - at_);
            base_ += at_;
            len_ -= at_;
            at_ = 0;
        }
        if (n > buf_.size()) buf_.resize(n);
        while (len_ < n) {
            const size_t got = src_->read(base_ + len_, buf_.data() + len_, buf_.size() - len_);
            if (got == 0) throw truncated();
            len_ += got;
        }
    }
    RError truncated() const {
        return RError("the R data file " + src_->path() +
                      " is truncated: it ends inside an object (at " + offset_text(pos()) + ")");
    }
    void skip(uint64_t n) {
        const uint64_t have = len_ - at_;
        if (n <= have) {
            at_ += static_cast<size_t>(n);
            return;
        }
        const uint64_t target = pos() + n;
        if (target < pos() || target > src_->size()) throw truncated();
        base_ = target;
        at_ = len_ = 0;
    }
    /* n elements of `size` bytes each fit in what is left of the stream */
    uint64_t span(uint64_t n, uint64_t size) {
        const uint64_t left = src_->size() > pos() ? src_->size() - pos() : 0;
        if (n > left / size) throw truncated();
        return n * size;
    }
    uint64_t length() {
        const int32_t len = i32();
        if (len < -1) throw malformed("a negative vector length");
        if (len == -1) {
            const uint32_t hi = static_cast<uint32_t>(i32()), lo = static_cast<uint32_t>(i32());
            if (hi > 65536) throw malformed("an invalid long vector length");
            return (uint64_t(hi) << 32) + lo;
        }
        return static_cast<uint64_t>(len);
    }
    size_t ref_index(int32_t flags) {
        uint32_t i = static_cast<uint32_t>(flags) >> 8;
        if (i == 0) i = static_cast<uint32_t>(i32());
        if (i == 0 || i > refs_.size()) throw malformed("a reference to an unknown object");
        return i - 1;
    }
    void add_ref(RefKind k, const std::string &name = std::string()) { refs_.push_back({k, name}); }
    /* one CHARSXP item; false for NA_character_ */
    bool charsxp(std::string *out) {
        const int32_t flags = i32();
        if (type_of(flags) != CHARSXP) throw malformed("a string item of type " + std::to_string(type_of(flags)));
        const int32_t len = i32();
        if (len < -1) throw malformed("a negative string length");
        const bool na = len == -1;
        out->clear();
        if (len > 0) {
            span(static_cast<uint64_t>(len), 1);
            need(static_cast<size_t>(len));
            if (codec_->decode(reinterpret_cast<const char *>(ptr()), static_cast<size_t>(len),
                               levels_of(flags), out))
                transcoded_++;
            at_ += static_cast<size_t>(len);
        }
        if (flags & kAttrBit) skip_items(1);
        return !na;
    }
    void skip_charsxp() {
        const int32_t flags = i32();
        if (type_of(flags) != CHARSXP) throw malformed("a string item of type " + std::to_string(type_of(flags)));
        const int32_t len = i32();
        if (len < -1) throw malformed("a negative string length");
        if (len > 0) skip(static_cast<uint64_t>(len));
        if (flags & kAttrBit) skip_items(1);
    }
    std::string string_vec() {
        if (i32() != 0) throw malformed("a persistent name with names");
        const int32_t n = i32();
        if (n < 0) throw malformed("a negative length");
        std::string all, s;
        for (int32_t i = 0; i < n; i++) {
            charsxp(&s);
            all += (i ? ":" : "") + s;
        }
        return all;
    }
    void run(std::vector<SkipFrame> &st);
    void skip_body(int32_t flags, std::vector<SkipFrame> &st);
    void push_bc1(std::vector<SkipFrame> &st) {
        st.push_back({FK::BCConstsHeader, 1});
        st.push_back({FK::Items, 1});
    }
    void bc_lang(int32_t type, std::vector<SkipFrame> &st);
    std::shared_ptr<RValue> load_altrep(int depth);
    Attrs attrs_of(const std::shared_ptr<RValue> &list);
    Located locate_altrep(int level, int deep);
    void frame_attrs(Located *L);

    std::shared_ptr<Source> src_;
    std::shared_ptr<const TextCodec> codec_;
    std::vector<char> buf_;
    uint64_t base_ = 0; /* stream offset of buf_[0] */
    size_t at_ = 0, len_ = 0;
    std::vector<Ref> refs_;
    long long transcoded_ = 0;
};

void Parser::header(FileInfo *info) {
    char m[5] = {0, 0, 0, 0, 0};
    const size_t got = src_->read(0, m, 5);
    const std::string &path = src_->path();
    const std::string head(m, got);
    if (got == 5 && head.compare(0, 3, "RDX") == 0 && (m[3] == '2' || m[3] == '3') && m[4] == '\n') {
        info->rdata = true;
        skip(5);
    } else if (got == 5 && m[0] == 'R' && m[1] == 'D' && (m[2] == 'A' || m[2] == 'B') &&
               (m[3] == '2' || m[3] == '3') && m[4] == '\n') {
        throw RError(path + " was saved in R's " +
                     std::string(m[2] == 'A' ? "ASCII (save(..., ascii = TRUE))" : "native binary") +
                     " format, which parqit does not read; " + kResave);
    } else if (got >= 4 && m[0] == 'R' && m[1] == 'D' && m[3] == '1') {
        throw RError(path + " was saved by R before version 1.4.0 (format version 1), which "
                            "parqit does not read; " + kResave);
    }
    if (src_->size() < pos() + 2)
        throw RError(path + " is not an R data file (it is too short)");
    need(2);
    const std::string fmt(reinterpret_cast<const char *>(ptr()), 2);
    if (fmt == "A\n")
        throw RError(path + " was saved in R's ASCII format (ascii = TRUE), which parqit does not "
                            "read; " + kResave);
    if (fmt == "B\n")
        throw RError(path + " was saved in R's native binary format (xdr = FALSE), which parqit "
                            "does not read; " + kResave);
    if (fmt != "X\n")
        throw RError(path + " is not an R data file: it does not begin with R's serialization "
                            "header (an .rds, .rda or .RData file written by saveRDS() or save())");
    at_ += 2;
    info->version = i32();
    info->writer_version = i32();
    info->min_reader_version = i32();
    if (info->version == 3) {
        const int32_t n = i32();
        if (n < 0 || n > 64) throw malformed("an invalid native encoding name");
        need(static_cast<size_t>(n));
        info->native_encoding.assign(reinterpret_cast<const char *>(ptr()), static_cast<size_t>(n));
        at_ += static_cast<size_t>(n);
    } else if (info->version != 2) {
        throw RError(path + " uses R serialization version " + std::to_string(info->version) +
                     "; parqit reads versions 2 and 3 (what R has written since 1.4.0); " + kResave);
    }
}

std::string Parser::tag_name() {
    const int32_t f = i32();
    const int t = type_of(f);
    if (t == SYMSXP) {
        std::string s;
        charsxp(&s);
        add_ref(RefKind::Symbol, s);
        return s;
    }
    if (t == REFSXP) {
        const Ref &r = refs_[ref_index(f)];
        return r.kind == RefKind::Symbol ? r.name : std::string();
    }
    skip_rest(f); /* a tag that is not a symbol: R never writes one */
    return std::string();
}

/* The skipper: an explicit stack instead of recursion, so a function body or
 * an environment of any depth passes. A frame holds how many items (or byte
 * code constants / language cells) remain at its level; the frame whose last
 * item is being read is popped first, so a pairlist of any length runs in
 * constant stack. */
void Parser::run(std::vector<SkipFrame> &st) {
    while (!st.empty()) {
        SkipFrame &f = st.back();
        if (f.n == 0) {
            st.pop_back();
            continue;
        }
        const FK kind = f.kind;
        if (--f.n == 0) st.pop_back(); /* f is not used below */
        switch (kind) {
        case FK::Items:
            skip_body(i32(), st);
            break;
        case FK::BCConstsHeader: {
            const int32_t n = i32();
            if (n < 0) throw malformed("a negative byte code constant count");
            st.push_back({FK::BCConsts, static_cast<uint64_t>(n)});
            break;
        }
        case FK::BCConsts: {
            const int32_t type = i32();
            switch (type) {
            case BCODESXP: push_bc1(st); break;
            case LANGSXP: case LISTSXP: case BCREPDEF: case BCREPREF: case ATTRLANGSXP:
            case ATTRLISTSXP:
                bc_lang(type, st);
                break;
            default: st.push_back({FK::Items, 1});
            }
            break;
        }
        case FK::BCLang:
            bc_lang(i32(), st);
            break;
        }
    }
}

void Parser::bc_lang(int32_t type, std::vector<SkipFrame> &st) {
    switch (type) {
    case BCREPREF:
        i32();
        return;
    case BCREPDEF: case LANGSXP: case LISTSXP: case ATTRLANGSXP: case ATTRLISTSXP: {
        if (type == BCREPDEF) {
            i32();
            type = i32();
        }
        const bool attr = type == ATTRLANGSXP || type == ATTRLISTSXP;
        st.push_back({FK::BCLang, 2});               /* car, cdr */
        st.push_back({FK::Items, attr ? 2u : 1u});   /* [attributes], tag */
        return;
    }
    default: /* a pad integer (already read) and a plain item */
        st.push_back({FK::Items, 1});
    }
}

void Parser::skip_body(int32_t flags, std::vector<SkipFrame> &st) {
    const int type = type_of(flags);
    const uint64_t attr = (flags & kAttrBit) ? 1 : 0, tag = (flags & kTagBit) ? 1 : 0;
    switch (type) {
    case NILVALUE_SXP: case EMPTYENV_SXP: case BASEENV_SXP: case GLOBALENV_SXP:
    case UNBOUNDVALUE_SXP: case MISSINGARG_SXP: case BASENAMESPACE_SXP:
        return;
    case REFSXP:
        ref_index(flags);
        return;
    case PERSISTSXP: add_ref(RefKind::Persist, string_vec()); return;
    case PACKAGESXP: add_ref(RefKind::Package, string_vec()); return;
    case NAMESPACESXP: add_ref(RefKind::Namespace, string_vec()); return;
    case ALTREP_SXP: /* class info, state, attributes */
        st.push_back({FK::Items, 3});
        return;
    case SYMSXP: {
        std::string s;
        charsxp(&s);
        add_ref(RefKind::Symbol, s);
        return;
    }
    case ENVSXP: /* registered before its enclosure, frame, hash table, attributes */
        i32();
        add_ref(RefKind::Env);
        st.push_back({FK::Items, 4});
        return;
    case LISTSXP: case LANGSXP: case CLOSXP: case PROMSXP: case DOTSXP:
        /* [attributes], [tag], car, cdr — the cdr is the next cell */
        st.push_back({FK::Items, attr + tag + 2});
        return;
    case EXTPTRSXP:
        add_ref(RefKind::ExtPtr);
        st.push_back({FK::Items, 2 + attr}); /* protected value, tag */
        return;
    case WEAKREFSXP:
        add_ref(RefKind::WeakRef);
        break;
    case SPECIALSXP: case BUILTINSXP: {
        const int32_t n = i32();
        if (n < 0) throw malformed("a negative name length");
        skip(static_cast<uint64_t>(n));
        break;
    }
    case CHARSXP: {
        const int32_t n = i32();
        if (n < -1) throw malformed("a negative string length");
        if (n > 0) skip(static_cast<uint64_t>(n));
        break;
    }
    case LGLSXP: case INTSXP: skip(span(length(), 4)); break;
    case REALSXP: skip(span(length(), 8)); break;
    case CPLXSXP: skip(span(length(), 16)); break;
    case RAWSXP: skip(span(length(), 1)); break;
    case STRSXP: {
        const uint64_t n = length();
        span(n, 8);
        for (uint64_t i = 0; i < n; i++) skip_charsxp();
        break;
    }
    case VECSXP: case EXPRSXP: {
        const uint64_t n = length();
        span(n, 4);
        st.push_back({FK::Items, n + attr}); /* elements, then attributes */
        return;
    }
    case BCODESXP:
        i32(); /* the size of its own reference table */
        if (attr) st.push_back({FK::Items, 1});
        push_bc1(st);
        return;
    case OBJSXP:
        break;
    case CLASSREFSXP: case GENERICREFSXP:
        throw RError("the R data file " + src_->path() +
                     " holds class or generic-function references, which R itself no longer reads");
    default:
        throw malformed("an item of unknown type " + std::to_string(type) +
                        " (perhaps written by a newer R)");
    }
    if (attr) st.push_back({FK::Items, 1});
}

Attrs Parser::attrs_of(const std::shared_ptr<RValue> &list) {
    Attrs a;
    if (!list || list->type != LISTSXP) return a;
    for (size_t i = 0; i < list->elts.size(); i++) a.emplace_back(list->tags[i], list->elts[i]);
    return a;
}

std::shared_ptr<RValue> Parser::load_body(int32_t flags, int depth) {
    auto v = std::make_shared<RValue>();
    const int type = type_of(flags);
    if (depth > kMaxLoadDepth) {
        skip_rest(flags);
        v->what = "(nested too deeply; not kept)";
        return v;
    }
    switch (type) {
    case NILVALUE_SXP:
        return v;
    case EMPTYENV_SXP: case BASEENV_SXP: case GLOBALENV_SXP:
        v->type = ENVSXP;
        v->what = "an environment";
        return v;
    case BASENAMESPACE_SXP:
        v->type = ENVSXP;
        v->what = "the namespace base";
        return v;
    case UNBOUNDVALUE_SXP: case MISSINGARG_SXP:
        v->what = "a missing argument";
        return v;
    case REFSXP: {
        const Ref &r = refs_[ref_index(flags)];
        if (r.kind == RefKind::Symbol) {
            v->type = SYMSXP;
            v->sym = r.name;
        } else {
            v->type = ENVSXP;
            v->what = r.kind == RefKind::Namespace ? "the namespace " + r.name
                      : r.kind == RefKind::Package ? "the package environment " + r.name
                      : r.kind == RefKind::ExtPtr  ? std::string("an external pointer")
                      : r.kind == RefKind::WeakRef ? std::string("a weak reference")
                                                   : std::string("an environment");
        }
        return v;
    }
    case PERSISTSXP: case PACKAGESXP: case NAMESPACESXP: {
        const std::string name = string_vec();
        const RefKind k = type == PERSISTSXP   ? RefKind::Persist
                          : type == PACKAGESXP ? RefKind::Package
                                               : RefKind::Namespace;
        add_ref(k, name);
        v->type = ENVSXP;
        v->what = type == NAMESPACESXP ? "the namespace " + name
                  : type == PACKAGESXP ? "the package environment " + name
                                       : std::string("a persistent reference");
        return v;
    }
    case SYMSXP:
        charsxp(&v->sym);
        add_ref(RefKind::Symbol, v->sym);
        v->type = SYMSXP;
        return v;
    case ENVSXP:
        i32();
        add_ref(RefKind::Env);
        skip_items(4);
        v->type = ENVSXP;
        v->what = "an environment";
        return v;
    case LISTSXP: {
        v->type = LISTSXP;
        int32_t f = flags;
        while (type_of(f) == LISTSXP) {
            if (f & kAttrBit) skip_items(1);
            v->tags.push_back((f & kTagBit) ? tag_name() : std::string());
            v->elts.push_back(load_item(depth + 1));
            f = i32();
        }
        if (type_of(f) != NILVALUE_SXP) skip_rest(f); /* a dotted pair's last cdr */
        v->length = v->elts.size();
        return v;
    }
    case LANGSXP: case CLOSXP: case PROMSXP: case DOTSXP:
        skip_rest(flags);
        v->type = type;
        v->what = type == CLOSXP ? "a function" : type == LANGSXP ? "a call" : "a promise";
        return v;
    case EXTPTRSXP:
        add_ref(RefKind::ExtPtr);
        skip_items(2 + ((flags & kAttrBit) ? 1 : 0));
        v->type = EXTPTRSXP;
        v->what = "an external pointer";
        return v;
    case WEAKREFSXP:
        add_ref(RefKind::WeakRef);
        if (flags & kAttrBit) skip_items(1);
        v->type = WEAKREFSXP;
        v->what = "a weak reference";
        return v;
    case SPECIALSXP: case BUILTINSXP: case CHARSXP: case BCODESXP:
        skip_rest(flags);
        v->type = type;
        v->what = type == BCODESXP ? "byte code" : type == CHARSXP ? "a string" : "a built-in function";
        return v;
    case ALTREP_SXP:
        return load_altrep(depth);
    case LGLSXP: case INTSXP: {
        const uint64_t n = length();
        v->length = n;
        if (n > kLoadMaxAtoms) {
            skip(span(n, 4));
            v->what = "(" + type_name(type) + " vector of length " + std::to_string(n) + "; not kept)";
        } else {
            span(n, 4);
            v->ints.resize(static_cast<size_t>(n));
            for (auto &x : v->ints) x = i32();
        }
        break;
    }
    case REALSXP: case CPLXSXP: {
        const uint64_t n = length();
        const uint64_t per = type == CPLXSXP ? 2 : 1;
        v->length = n;
        if (n > kLoadMaxAtoms) {
            skip(span(n, 8 * per));
            v->what = "(" + type_name(type) + " vector of length " + std::to_string(n) + "; not kept)";
        } else {
            span(n, 8 * per);
            v->reals.resize(static_cast<size_t>(n * per));
            for (auto &x : v->reals) {
                need(8);
                x = real_of(be64(ptr()));
                at_ += 8;
            }
        }
        break;
    }
    case RAWSXP: {
        const uint64_t n = length();
        v->length = n;
        if (n > kLoadMaxAtoms) {
            skip(span(n, 1));
            v->what = "(raw vector of length " + std::to_string(n) + "; not kept)";
        } else {
            span(n, 1);
            need(static_cast<size_t>(n));
            v->raw.assign(reinterpret_cast<const char *>(ptr()), static_cast<size_t>(n));
            at_ += static_cast<size_t>(n);
        }
        break;
    }
    case STRSXP: {
        const uint64_t n = length();
        span(n, 8);
        v->length = n;
        if (n > kLoadMaxAtoms) {
            for (uint64_t i = 0; i < n; i++) skip_charsxp();
            v->what = "(character vector of length " + std::to_string(n) + "; not kept)";
        } else {
            v->strs.resize(static_cast<size_t>(n));
            v->na.resize(static_cast<size_t>(n));
            for (size_t i = 0; i < v->strs.size(); i++) v->na[i] = charsxp(&v->strs[i]) ? 0 : 1;
        }
        break;
    }
    case VECSXP: case EXPRSXP: {
        const uint64_t n = length();
        span(n, 4);
        v->length = n;
        if (n > kLoadMaxList) {
            skip_items(n);
            v->what = "(list of length " + std::to_string(n) + "; not kept)";
        } else {
            v->elts.reserve(static_cast<size_t>(n));
            for (uint64_t i = 0; i < n; i++) v->elts.push_back(load_item(depth + 1));
        }
        break;
    }
    case OBJSXP:
        break;
    case CLASSREFSXP: case GENERICREFSXP:
        throw RError("the R data file " + src_->path() +
                     " holds class or generic-function references, which R itself no longer reads");
    default:
        throw malformed("an item of unknown type " + std::to_string(type) +
                        " (perhaps written by a newer R)");
    }
    v->type = type;
    if (flags & kAttrBit) v->attrs = attrs_of(load_item(depth + 1));
    return v;
}

/* ALTREP class info: pairlist(class symbol, package symbol, type integer) */
void altrep_class(const RValue &info, std::string *cls, std::string *pkg, int *type) {
    cls->clear();
    pkg->clear();
    *type = NILSXP;
    if (info.type != LISTSXP) return;
    if (info.elts.size() > 0 && info.elts[0]->type == SYMSXP) *cls = info.elts[0]->sym;
    if (info.elts.size() > 1 && info.elts[1]->type == SYMSXP) *pkg = info.elts[1]->sym;
    if (info.elts.size() > 2 && info.elts[2]->type == INTSXP && !info.elts[2]->ints.empty())
        *type = info.elts[2]->ints[0];
}

/* compact sequence state: (length, first, increment) as REALSXP (R >= 3.5.1)
 * or INTSXP */
bool seq_state(const RValue &s, bool integer, double *n, double *first, double *incr) {
    if (s.type == REALSXP && s.reals.size() == 3) {
        *n = s.reals[0];
        *first = s.reals[1];
        *incr = s.reals[2];
    } else if (s.type == INTSXP && s.ints.size() == 3) {
        *n = s.ints[0];
        *first = s.ints[1];
        *incr = s.ints[2];
    } else {
        return false;
    }
    if (!(*n >= 0 && *n <= kMaxRLength) || *n != std::trunc(*n) ||
        !std::isfinite(*first) || !(*incr == 1 || *incr == -1)) return false;
    const double last = *first + *incr * (*n > 0 ? *n - 1 : 0);
    if (!std::isfinite(last)) return false;
    return !integer || (*first == std::trunc(*first) &&
                        *first >= -2147483647.0 && *first <= 2147483647.0 &&
                        last >= -2147483647.0 && last <= 2147483647.0);
}

std::shared_ptr<RValue> Parser::load_altrep(int depth) {
    auto info = load_item(depth + 1);
    std::string cls, pkg;
    int type = NILSXP;
    altrep_class(*info, &cls, &pkg, &type);
    if (cls == "compact_intseq" || cls == "compact_realseq") {
        auto state = load_item(depth + 1);
        Attrs a = attrs_of(load_item(depth + 1));
        auto v = std::make_shared<RValue>();
        double n = 0, first = 0, incr = 0;
        v->type = cls == "compact_intseq" ? INTSXP : REALSXP;
        v->attrs = a;
        if (!seq_state(*state, v->type == INTSXP, &n, &first, &incr))
            throw malformed("an invalid compact sequence");
        v->length = static_cast<uint64_t>(n);
        if (v->length > kLoadMaxAtoms) {
            v->what = "(compact sequence of length " + std::to_string(v->length) + "; not kept)";
            return v;
        }
        for (uint64_t i = 0; i < v->length; i++) {
            if (v->type == INTSXP)
                v->ints.push_back(static_cast<int32_t>(static_cast<int64_t>(first) +
                                                       static_cast<int64_t>(incr) * static_cast<int64_t>(i)));
            else
                v->reals.push_back(first + incr * static_cast<double>(i));
        }
        return v;
    }
    if (cls.rfind("wrap_", 0) == 0 || cls == "deferred_string") {
        const int32_t sf = i32();
        if (type_of(sf) != LISTSXP) {
            skip_rest(sf);
            skip_items(1);
            auto v = std::make_shared<RValue>();
            v->what = "(an ALTREP object of class " + cls + "; not kept)";
            return v;
        }
        if (sf & kAttrBit) skip_items(1);
        if (sf & kTagBit) skip_items(1);
        auto inner = load_item(depth + 1);
        skip_items(1); /* the wrapper's metadata, or the conversion's settings */
        Attrs a = attrs_of(load_item(depth + 1));
        if (cls == "deferred_string") {
            auto v = std::make_shared<RValue>();
            v->type = STRSXP;
            v->attrs = a;
            v->length = inner->length;
            if (inner->type == INTSXP && inner->kept()) {
                for (int32_t x : inner->ints) {
                    v->strs.push_back(x == kNaInteger ? std::string() : std::to_string(x));
                    v->na.push_back(x == kNaInteger ? 1 : 0);
                }
            } else {
                v->what = "(numbers R had not yet converted to text; not kept)";
            }
            return v;
        }
        if (!a.empty()) inner->attrs = a;
        return inner;
    }
    skip_items(2); /* state, attributes */
    auto v = std::make_shared<RValue>();
    v->type = type;
    v->what = "(an ALTREP object of class " + cls + " from package " + pkg + "; not kept)";
    return v;
}

void Parser::frame_attrs(Located *L) {
    int32_t f = i32();
    if (type_of(f) != LISTSXP) {
        load_body(f, 1); /* not an attribute list: nothing to keep */
        return;
    }
    while (type_of(f) == LISTSXP) {
        if (f & kAttrBit) skip_items(1);
        const std::string name = (f & kTagBit) ? tag_name() : std::string();
        if (name == "row.names") {
            Located rn = locate(i32(), 2, 0);
            L->has_rownames = true;
            L->rn_type = rn.type;
            L->rn_vec = rn.vec;
            L->rn_unsupported = rn.unsupported;
        } else {
            L->attrs.emplace_back(name, load_item(1));
        }
        f = i32();
    }
    if (type_of(f) != NILVALUE_SXP) skip_rest(f);
}

Located Parser::locate_altrep(int level, int deep) {
    Located L;
    auto info = load_item(1);
    std::string cls, pkg;
    int type = NILSXP;
    altrep_class(*info, &cls, &pkg, &type);
    if (cls == "compact_intseq" || cls == "compact_realseq") {
        auto state = load_item(1);
        L.attrs = attrs_of(load_item(1));
        double n = 0, first = 0, incr = 0;
        if (!seq_state(*state, cls == "compact_intseq", &n, &first, &incr))
            throw malformed("an invalid compact sequence");
        L.type = cls == "compact_intseq" ? INTSXP : REALSXP;
        L.vec.type = L.type;
        L.vec.length = L.length = static_cast<uint64_t>(n);
        L.vec.alt = cls == "compact_intseq" ? VecRef::Alt::IntSeq : VecRef::Alt::RealSeq;
        L.vec.seq_first = first;
        L.vec.seq_incr = incr;
        return L;
    }
    if (cls.rfind("wrap_", 0) == 0 || cls == "deferred_string") {
        const int32_t sf = i32();
        if (type_of(sf) != LISTSXP) {
            skip_rest(sf);
            L.attrs = attrs_of(load_item(1));
            L.type = type;
            L.unsupported = "an ALTREP vector of class " + cls + " in a layout parqit does not know";
            return L;
        }
        if (sf & kAttrBit) skip_items(1);
        if (sf & kTagBit) skip_items(1);
        Located inner = locate(i32(), cls == "deferred_string" ? 2 : level, deep);
        skip_items(1); /* the wrapper's metadata, or the conversion's settings */
        Attrs a = attrs_of(load_item(1));
        if (cls == "deferred_string") {
            L.type = STRSXP;
            L.attrs = a;
            L.length = inner.length;
            if (inner.type == INTSXP && inner.unsupported.empty()) {
                L.vec.type = STRSXP;
                L.vec.length = inner.vec.length;
                L.vec.alt = VecRef::Alt::DeferredInt;
                L.vec.arg = std::make_shared<VecRef>(inner.vec);
            } else if (inner.type == REALSXP && inner.unsupported.empty()) {
                /* R writes these doubles as text only when they are used, with
                 * the reading machine's formatting: the numbers are what the
                 * file holds, so they are what is carried */
                L.type = REALSXP;
                L.vec = inner.vec;
                L.deferred = true;
            } else {
                L.unsupported = "text R had not yet converted from numbers (a deferred "
                                "as.character()); in R, re-create the column with paste0() and "
                                "save the data again";
            }
            return L;
        }
        if (!a.empty()) inner.attrs = a;
        return inner;
    }
    skip_items(1); /* the state only that class can read */
    L.attrs = attrs_of(load_item(1));
    L.type = type;
    L.unsupported = "an ALTREP vector of class " + cls + " (package " + pkg +
                    ") that only its package can expand; in R, materialise the column (for "
                    "example x[] <- x[]) and save the data again";
    return L;
}

/* level 0 is the object read (an .rds root, an .RData object); a list's
 * elements are located while level < deep, so an .rds holding a list of data
 * frames is searched one level down; deeper lists are passed over */
Located Parser::locate(int32_t flags, int level, int deep) {
    Located L;
    const int type = type_of(flags);
    switch (type) {
    case LGLSXP: case INTSXP: case REALSXP: case STRSXP: {
        L.type = type;
        L.vec.type = type;
        L.vec.length = L.length = length();
        L.vec.offset = pos();
        if (type == STRSXP) {
            span(L.length, 8);
            for (uint64_t i = 0; i < L.length; i++) {
                const int32_t cf = i32();
                if (type_of(cf) != CHARSXP)
                    throw malformed("a string item of type " + std::to_string(type_of(cf)));
                const int32_t len = i32();
                if (len < -1) throw malformed("a negative string length");
                if (len > 0) skip(static_cast<uint64_t>(len));
                if (cf & kAttrBit) {
                    skip_items(1);
                    L.unsupported = "strings that carry attributes (written by a very old R)";
                }
            }
        } else {
            skip(span(L.length, type == REALSXP ? 8 : 4));
        }
        if (flags & kAttrBit) L.attrs = attrs_of(load_item(1));
        return L;
    }
    case VECSXP: {
        L.type = VECSXP;
        L.length = length();
        span(L.length, 4);
        if (level < deep && (level == 0 || L.length <= kLocateMaxElts)) {
            L.elts_located = true;
            L.elts.reserve(static_cast<size_t>(L.length));
            for (uint64_t i = 0; i < L.length; i++) L.elts.push_back(locate(i32(), level + 1, deep));
        } else {
            skip_items(L.length);
        }
        if (flags & kAttrBit) frame_attrs(&L);
        return L;
    }
    case ALTREP_SXP:
        return locate_altrep(level, deep);
    case CPLXSXP: case RAWSXP: case EXPRSXP: case OBJSXP: {
        L.type = type;
        if (type != OBJSXP) {
            L.length = length();
            if (type == EXPRSXP) skip_items(span(L.length, 4) / 4);
            else skip(span(L.length, type == CPLXSXP ? 16 : 1));
        }
        if (flags & kAttrBit) L.attrs = attrs_of(load_item(1));
        return L;
    }
    case NILVALUE_SXP:
        L.type = NILSXP;
        L.what = "NULL";
        return L;
    default:
        L.type = type;
        L.what = type == CLOSXP                                          ? "a function"
                 : type == ENVSXP || type == GLOBALENV_SXP || type == REFSXP ? "an environment"
                 : type == LANGSXP                                       ? "a call"
                 : type == SYMSXP                                        ? "a symbol"
                 : type == LISTSXP                                       ? "a pairlist"
                 : type == SPECIALSXP || type == BUILTINSXP              ? "a built-in function"
                 : type == EXTPTRSXP                                     ? "an external pointer"
                 : type == NAMESPACESXP                                  ? "a namespace"
                                                                         : "an object of type " + std::to_string(type);
        skip_rest(flags);
        return L;
    }
}

/* ------------------------------------------------------------- frames */

bool is_frame(const Located &L) {
    return L.type == VECSXP && L.elts_located && inherits_in(L.attrs, "data.frame");
}

std::string plural(uint64_t n, const char *one, const char *many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

uint64_t frame_rows(const std::shared_ptr<Source> &src, const std::shared_ptr<const TextCodec> &codec,
                    const Located &L, Frame::RowNames *kind, VecRef *rn) {
    *kind = Frame::RowNames::Automatic;
    if (L.has_rownames && L.rn_unsupported.empty()) {
        if ((L.rn_type == INTSXP || L.rn_type == REALSXP) && L.rn_vec.length == 2) {
            Cursor c(src, L.rn_vec, codec, 64);
            if (L.rn_type == INTSXP) {
                const int32_t a = c.next_int(), b = c.next_int();
                if (a == kNaInteger) {
                    if (b == kNaInteger) throw RError("invalid compact row names in " + src->path());
                    return static_cast<uint64_t>(b < 0 ? -static_cast<int64_t>(b) : b);
                }
            } else {
                const double a = c.next_real(), b = c.next_real();
                if (std::isnan(a)) {
                    if (!std::isfinite(b) || std::fabs(b) > kMaxRLength || b != std::trunc(b))
                        throw RError("invalid compact row names in " + src->path());
                    return static_cast<uint64_t>(std::fabs(b));
                }
            }
        }
        if (L.rn_type == INTSXP || L.rn_type == REALSXP || L.rn_type == STRSXP) {
            *kind = L.rn_type == STRSXP ? Frame::RowNames::Text : Frame::RowNames::Integer;
            *rn = L.rn_vec;
            return L.rn_vec.length;
        }
    }
    return L.elts.empty() ? 0 : L.elts[0].length;
}

std::string describe(const std::shared_ptr<Source> &src, const std::shared_ptr<const TextCodec> &codec,
                     const Located &L) {
    if (!L.what.empty()) return L.what;
    const auto cls = classes_in(L.attrs);
    std::string cl;
    for (const auto &c : cls) cl += (cl.empty() ? "" : " ") + c;
    if (is_frame(L)) {
        Frame::RowNames k;
        VecRef rn;
        const uint64_t n = frame_rows(src, codec, L, &k, &rn);
        return "a data frame (" + plural(n, "row", "rows") + ", " +
               plural(L.elts.size(), "column", "columns") + ")";
    }
    if (L.type == VECSXP) {
        if (inherits_in(L.attrs, "data.frame"))
            return "a data frame parqit does not search this deep (inside a list, or with more "
                   "than 100,000 columns there); save it on its own with saveRDS()";
        if (inherits_in(L.attrs, "POSIXlt")) return "a POSIXlt date-time vector";
        return "a list of " + plural(L.length, "element", "elements") +
               (cl.empty() ? std::string() : " of class " + cl);
    }
    if (L.type == OBJSXP) return "an S4 object" + (cl.empty() ? std::string() : " of class " + cl);
    if (inherits_in(L.attrs, "factor")) return "a factor of length " + std::to_string(L.length);
    if (attr_in(L.attrs, "dim")) return "a " + type_name(L.type) + " matrix or array";
    return "a " + type_name(L.type) + " vector of length " + std::to_string(L.length) +
           (cl.empty() ? std::string() : " of class " + cl);
}

std::string column_problem(const Located &e, uint64_t nrow) {
    if (!e.unsupported.empty()) return e.unsupported;
    switch (e.type) {
    case LGLSXP: case INTSXP: case REALSXP: case STRSXP:
        if (attr_in(e.attrs, "dim")) return "a matrix column";
        if (e.length != nrow)
            return "its length (" + std::to_string(e.length) + ") differs from the number of rows (" +
                   std::to_string(nrow) + ")";
        return std::string();
    case VECSXP:
        if (inherits_in(e.attrs, "data.frame")) return "a data frame inside the data frame (unnest it in R)";
        if (inherits_in(e.attrs, "POSIXlt"))
            return "POSIXlt date-times (convert them with as.POSIXct() in R)";
        return "a list column";
    case CPLXSXP: return "complex numbers";
    case RAWSXP: return "raw bytes";
    case OBJSXP: return "an S4 object";
    case NILSXP: return "NULL";
    default: return e.what.empty() ? "an object of type " + std::to_string(e.type) : e.what;
    }
}

Frame make_frame(const Parsed &p, const Located &L, const std::string &object) {
    Frame fr;
    fr.object = object;
    fr.nrow = frame_rows(p.source, p.codec, L, &fr.rownames, &fr.rownames_vec);
    const RValue *names = attr_in(L.attrs, "names");
    for (size_t i = 0; i < L.elts.size(); i++) {
        const Located &e = L.elts[i];
        Column c;
        if (names && names->type == STRSXP && names->kept() && i < names->strs.size()) {
            c.name = names->strs[i];
            c.name_na = names->na[i] != 0;
        }
        c.attrs = e.attrs;
        c.unsupported = column_problem(e, fr.nrow);
        c.deferred_numbers = e.deferred;
        if (c.unsupported.empty()) c.vec = e.vec;
        fr.cols.push_back(std::move(c));
    }
    for (const auto &kv : L.attrs)
        if (kv.first != "names") fr.attrs.push_back(kv);
    return fr;
}

} // namespace

/* ------------------------------------------------------------- Source */

Source::Source(const std::string &path, const std::string &tmpdir) : path_(path) {
    const fs::path p = fs::u8path(path);
    std::error_code ec;
    if (fs::is_directory(p, ec)) throw RError(path + " is a directory, not an R data file");
    const uintmax_t fsize = fs::file_size(p, ec);
    if (ec) throw RError("cannot open " + path + ": " + ec.message());
    std::ifstream f(p, std::ios::in | std::ios::binary);
    if (!f.is_open()) throw RError("cannot open " + path + " for reading");
    unsigned char m[6] = {0, 0, 0, 0, 0, 0};
    f.read(reinterpret_cast<char *>(m), 6);
    const size_t got = static_cast<size_t>(f.gcount());
    f.clear();
    f.seekg(0);
    if (got >= 2 && m[0] == 0x1f && m[1] == 0x8b) {
        compression_ = Compression::Gzip;
    } else if (got >= 4 && m[0] == 0x28 && m[1] == 0xb5 && m[2] == 0x2f && m[3] == 0xfd) {
        compression_ = Compression::Zstd;
    } else if (got >= 3 && m[0] == 'B' && m[1] == 'Z' && m[2] == 'h') {
        throw RError(path + " is compressed with bzip2, which parqit does not read; " + kResave);
    } else if (got >= 6 && m[0] == 0xfd && m[1] == '7' && m[2] == 'z' && m[3] == 'X' && m[4] == 'Z' &&
               m[5] == 0) {
        throw RError(path + " is compressed with xz, which parqit does not read; " + kResave);
    }
    if (compression_ == Compression::None) {
        data_path_ = path;
        size_ = static_cast<uint64_t>(fsize);
    } else {
        const std::string tmp = temp_path(tmpdir);
        std::ofstream out(fs::u8path(tmp), std::ios::out | std::ios::binary | std::ios::trunc);
        if (!out.is_open())
            throw RError("cannot create a temporary file to decompress " + path + " (" + tmp + ")");
        try {
            ByteIn in(f, path);
            size_ = compression_ == Compression::Gzip ? gunzip(in, out, path) : unzstd(in, out, path);
            out.close();
            if (!out) throw RError("could not write the decompressed copy of " + path +
                                   " to the temporary directory (is the disk full?)");
        } catch (...) {
            out.close();
            fs::remove(fs::u8path(tmp), ec);
            throw;
        }
        temp_ = true;
        data_path_ = tmp;
    }
    in_.open(fs::u8path(data_path_), std::ios::in | std::ios::binary);
    if (!in_.is_open()) {
        if (temp_) fs::remove(fs::u8path(data_path_), ec);
        throw RError("cannot open " + path + " for reading");
    }
}

Source::~Source() {
    in_.close();
    if (temp_) {
        std::error_code ec;
        fs::remove(fs::u8path(data_path_), ec);
    }
}

size_t Source::read(uint64_t offset, char *dst, size_t n) {
    if (offset >= size_ || n == 0) return 0;
    if (n > size_ - offset) n = static_cast<size_t>(size_ - offset);
    in_.clear();
    in_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    in_.read(dst, static_cast<std::streamsize>(n));
    if (static_cast<size_t>(in_.gcount()) != n)
        throw RError("read error in " + path_ + " at " + offset_text(offset) +
                     " (the file changed or is truncated)");
    return n;
}

/* ---------------------------------------------------------- TextCodec */

TextCodec::TextCodec(const std::string &native_name, const std::string &override_name,
                     const std::string &default_name) {
    if (!default_name.empty()) {
        LegacyEncoding d;
        const int c = classify_encoding(default_name, &d);
        if (c != 1 && c != 2)
            throw RError("the default encoding " + default_name + " is not an encoding parqit "
                         "decodes; it decodes " + std::string(legacy_encoding_families()));
        fallback_ = c == 1 ? LegacyEncoding::Utf8 : d;
    }
    if (!override_name.empty()) {
        const int c = classify_encoding(override_name, &legacy_);
        if (c != 1 && c != 2)
            throw RError("encoding(" + override_name + ") is not an encoding parqit decodes; it "
                         "decodes " + std::string(legacy_encoding_families()));
        kind_ = c == 1 ? Kind::Utf8 : Kind::Legacy;
        name_ = c == 1 ? std::string("UTF-8") : std::string(legacy_encoding_name(legacy_));
        if (c == 2) fallback_ = legacy_;
        return;
    }
    name_ = native_name;
    if (native_name.empty()) {
        kind_ = Kind::Undeclared;
        return;
    }
    switch (classify_encoding(native_name, &legacy_)) {
    case 1: kind_ = Kind::Utf8; break;
    case 2: kind_ = Kind::Legacy; break;
    case 3: kind_ = Kind::Ascii; break;
    default: kind_ = Kind::Unknown;
    }
}

std::string TextCodec::native_used() const {
    switch (kind_) {
    case Kind::Utf8: return "UTF-8";
    case Kind::Legacy: return legacy_encoding_name(legacy_);
    case Kind::Ascii: return "ASCII (" + name_ + ")";
    case Kind::Undeclared: return "UTF-8 (not declared)";
    default: return name_;
    }
}

bool TextCodec::decode(const char *p, size_t n, int levels, std::string *out) const {
    const unsigned char *u = reinterpret_cast<const unsigned char *>(p);
    const LegacyEncoding ascii_enc = kind_ == Kind::Legacy &&
        !(levels & (kLatin1Mask | kUtf8Mask | kBytesMask | kAsciiMask)) ? legacy_ : LegacyEncoding::Utf8;
    if (text_ascii_unchanged(p, n, ascii_enc)) {
        out->assign(p, n);
        return false;
    }
    auto fallback = [&]() {
        legacy_decode(p, n, fallback_, out);
        return true;
    };
    if (levels & kLatin1Mask) {
        *out = legacy_to_utf8(std::string(p, n), LegacyEncoding::Latin1);
        return false;
    }
    if (levels & (kUtf8Mask | kBytesMask | kAsciiMask)) {
        if (!utf8_valid(u, n)) return fallback();
        out->assign(p, n);
        return (levels & kAsciiMask) != 0 && !(levels & (kUtf8Mask | kBytesMask));
    }
    switch (kind_) {
    case Kind::Legacy:
        /* a byte sequence the declared encoding does not define is U+FFFD,
         * counted like a transcoding: never silent */
        return legacy_decode(p, n, legacy_, out) > 0;
    case Kind::Utf8: case Kind::Undeclared:
        if (!utf8_valid(u, n)) return fallback();
        out->assign(p, n);
        return false;
    case Kind::Ascii:
        if (!utf8_valid(u, n)) return fallback();
        out->assign(p, n);
        return true;
    default:
        if (utf8_valid(u, n)) {
            out->assign(p, n);
            return true;
        }
        throw RError("the file's text is in " + name_ + ", which parqit cannot decode (it decodes " +
                     std::string(legacy_encoding_families()) + "); give encoding() if one of those "
                     "is right, otherwise convert the strings in R with enc2utf8() and save the "
                     "data again");
    }
}

/* ------------------------------------------------------------- RValue */

const RValue *RValue::attr(const std::string &name) const { return attr_in(attrs, name); }
std::vector<std::string> RValue::classes() const { return classes_in(attrs); }
bool RValue::inherits(const std::string &cls) const { return inherits_in(attrs, cls); }
std::string RValue::str1() const {
    if (type != STRSXP || strs.empty() || na[0]) return std::string();
    return strs[0];
}

/* ------------------------------------------------------------- values */

bool is_na_real(double x) { return std::isnan(x) && (bits_of(x) & 0xFFFFFFFFull) == 1954; }

char na_tag(double x) {
    if (!is_na_real(x)) return 0;
    return static_cast<char>((bits_of(x) >> 32) & 0xFF);
}

std::string r_version_string(int packed) {
    if (packed <= 0) return std::string();
    return std::to_string(packed / 65536) + "." + std::to_string((packed % 65536) / 256) + "." +
           std::to_string(packed % 256);
}

/* ------------------------------------------------------------- read_file */

Parsed read_file(const std::string &path, const ReadOptions &opt) {
    Parsed out;
    out.source = std::make_shared<Source>(path, opt.tmpdir);
    out.info.compression = out.source->compression();
    Parser p(out.source);
    p.header(&out.info);
    out.codec = std::make_shared<const TextCodec>(out.info.native_encoding, opt.encoding,
                                                  opt.default_encoding);
    p.set_codec(out.codec);

    /* the objects: an .RData's named objects, or the .rds root (and, when it
     * is a plain list, its elements) */
    std::vector<std::pair<std::string, Located>> objects;
    if (out.info.rdata) {
        int32_t f = p.i32();
        if (type_of(f) == NILVALUE_SXP) throw RError(path + " holds no objects");
        if (type_of(f) != LISTSXP) throw p.malformed("an .RData file that is not a list of named objects");
        while (type_of(f) == LISTSXP) {
            if (f & kAttrBit) p.skip_items(1);
            const std::string name = (f & kTagBit) ? p.tag_name() : std::string();
            objects.emplace_back(name, p.locate(p.i32(), 0, 1));
            f = p.i32();
        }
        if (type_of(f) != NILVALUE_SXP) p.skip_rest(f);
    } else {
        Located root = p.locate(p.i32(), 0, 2);
        out.info.root_what = describe(out.source, out.codec, root);
        if (is_frame(root)) {
            if (!opt.object.empty())
                throw RChoiceError("object(" + opt.object + "): " + path + " holds a single data frame, "
                             "not named objects; leave out object()");
            out.frame = make_frame(out, root, std::string());
            out.transcoded_meta = p.transcoded();
            return out;
        }
        if (root.type != VECSXP || !root.elts_located)
            throw RError(path + " holds " + out.info.root_what + ", not a data frame");
        const RValue *names = attr_in(root.attrs, "names");
        std::set<std::string> taken;
        if (names && names->type == STRSXP && names->kept())
            for (size_t i = 0; i < names->strs.size() && i < root.elts.size(); i++)
                if (!names->na[i]) taken.insert(names->strs[i]);
        for (size_t i = 0; i < root.elts.size(); i++) {
            std::string name;
            if (names && names->type == STRSXP && names->kept() && i < names->strs.size() && !names->na[i])
                name = names->strs[i];
            if (name.empty()) {
                const std::string base = "[[" + std::to_string(i + 1) + "]]";
                name = base;
                for (size_t k = 1; taken.count(name); k++) name = base + "_" + std::to_string(k);
                taken.insert(name);
            }
            objects.emplace_back(name, std::move(root.elts[i]));
        }
    }
    for (const auto &o : objects) out.info.objects.emplace_back(o.first, describe(out.source, out.codec, o.second));

    const std::string where = out.info.rdata ? path : path + " (a list)";
    auto listing = [&]() {
        std::string s;
        for (size_t i = 0; i < out.info.objects.size() && i < 12; i++)
            s += (i ? "; " : "") + out.info.objects[i].first + " (" + out.info.objects[i].second + ")";
        if (out.info.objects.size() > 12) s += "; and " + std::to_string(out.info.objects.size() - 12) + " more";
        return s;
    };
    size_t chosen = objects.size();
    if (!opt.object.empty()) {
        for (size_t i = 0; i < objects.size() && chosen == objects.size(); i++)
            if (objects[i].first == opt.object) chosen = i;
        if (chosen == objects.size())
            throw RChoiceError("object(" + opt.object + "): " + where + " holds no object of that name; it holds " + listing());
        if (!is_frame(objects[chosen].second))
            throw RChoiceError("object(" + opt.object + ") is " + out.info.objects[chosen].second +
                         ", not a data frame");
    } else {
        std::vector<size_t> frames;
        for (size_t i = 0; i < objects.size(); i++)
            if (is_frame(objects[i].second)) frames.push_back(i);
        if (frames.empty()) throw RError(where + " holds no data frame; it holds " + listing());
        if (frames.size() > 1) {
            std::string s;
            for (size_t k = 0; k < frames.size(); k++) s += (k ? ", " : "") + objects[frames[k]].first;
            throw RChoiceError(where + " holds " + std::to_string(frames.size()) + " data frames (" + s +
                         "); choose one with object()");
        }
        chosen = frames[0];
        if (objects.size() > 1)
            out.notes.push_back(where + " holds " + std::to_string(objects.size()) +
                                " objects; read its only data frame, " + objects[chosen].first);
    }
    out.frame = make_frame(out, objects[chosen].second, objects[chosen].first);
    out.transcoded_meta = p.transcoded();
    return out;
}

/* ------------------------------------------------------------- Cursor */

Cursor::Cursor(std::shared_ptr<Source> src, const VecRef &v, std::shared_ptr<const TextCodec> codec,
               size_t buffer_bytes)
    : src_(std::move(src)), v_(v), codec_(std::move(codec)), pos_(v.offset) {
    if (v_.alt == VecRef::Alt::DeferredInt) {
        if (!v_.arg) throw RError("internal error: a deferred string without its numbers");
        inner_ = std::make_unique<Cursor>(src_, *v_.arg, codec_, buffer_bytes);
    } else if (v_.alt == VecRef::Alt::None) {
        buf_.resize(std::max<size_t>(buffer_bytes, 64));
    }
}

void Cursor::fill(size_t need) {
    if (len_ - at_ >= need) return;
    if (at_ > 0) {
        std::memmove(buf_.data(), buf_.data() + at_, len_ - at_);
        pos_ += at_;
        len_ -= at_;
        at_ = 0;
    }
    if (need > buf_.size()) buf_.resize(need);
    while (len_ < need) {
        const size_t got = src_->read(pos_ + len_, buf_.data() + len_, buf_.size() - len_);
        if (got == 0)
            throw RError("the R data file " + src_->path() +
                         " ended while a column was being read (it changed or is truncated)");
        len_ += got;
    }
}

uint32_t Cursor::u32() {
    fill(4);
    const uint32_t x = be32(reinterpret_cast<const unsigned char *>(buf_.data()) + at_);
    at_ += 4;
    return x;
}

int32_t Cursor::next_int() {
    if (index_ >= v_.length) throw RError("internal error: read past the end of a column");
    const uint64_t i = index_++;
    if (v_.alt == VecRef::Alt::IntSeq)
        return static_cast<int32_t>(static_cast<int64_t>(v_.seq_first) +
                                    static_cast<int64_t>(v_.seq_incr) * static_cast<int64_t>(i));
    return static_cast<int32_t>(u32());
}

double Cursor::next_real() {
    if (index_ >= v_.length) throw RError("internal error: read past the end of a column");
    const uint64_t i = index_++;
    if (v_.alt == VecRef::Alt::RealSeq) return v_.seq_first + v_.seq_incr * static_cast<double>(i);
    fill(8);
    const uint64_t bits = be64(reinterpret_cast<const unsigned char *>(buf_.data()) + at_);
    at_ += 8;
    return real_of(bits);
}

bool Cursor::next_string(std::string *utf8, bool *transcoded) {
    if (index_ >= v_.length) throw RError("internal error: read past the end of a column");
    index_++;
    *transcoded = false;
    if (v_.alt == VecRef::Alt::DeferredInt) {
        const int32_t x = inner_->next_int();
        if (x == kNaInteger) {
            utf8->clear();
            return false;
        }
        *utf8 = std::to_string(x);
        return true;
    }
    const uint32_t flags = u32();
    const int32_t len = static_cast<int32_t>(u32());
    if ((flags & 0xFF) != CHARSXP || (flags & kAttrBit) || len < -1)
        throw RError("the R data file " + src_->path() +
                     " changed while it was being read (a string is no longer where it was)");
    if (len == -1) {
        utf8->clear();
        return false;
    }
    if (static_cast<uint64_t>(len) > src_->size())
        throw RError("the R data file " + src_->path() +
                     " changed while it was being read (a string is no longer where it was)");
    fill(static_cast<size_t>(len));
    *transcoded = codec_->decode(buf_.data() + at_, static_cast<size_t>(len),
                                 static_cast<int>(flags >> 12), utf8);
    at_ += static_cast<size_t>(len);
    return true;
}

} // namespace rdata
} // namespace parqit
