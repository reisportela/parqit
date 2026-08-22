#include "engine/parquet_footer.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <system_error>

namespace parqit {

namespace {

/* Thrift compact-protocol field types */
enum : uint8_t {
    CT_STOP = 0,
    CT_BOOL_TRUE = 1,
    CT_BOOL_FALSE = 2,
    CT_BYTE = 3,
    CT_I16 = 4,
    CT_I32 = 5,
    CT_I64 = 6,
    CT_DOUBLE = 7,
    CT_BINARY = 8,
    CT_LIST = 9,
    CT_SET = 10,
    CT_MAP = 11,
    CT_STRUCT = 12,
};

/* Structural position of a scalar inside FileMetaData: the chain of Thrift
 * field ids from the root struct (list/map levels add no id) and, for every
 * list level crossed, the element index. */
struct Where {
    std::vector<int> fields;
    std::vector<long long> list_idx;
};

/* Visitor: may replace a BINARY value (return true and fill *out) and sees
 * every integer (for num_children). */
struct Visitor {
    virtual ~Visitor() = default;
    virtual bool binary(const Where &, const std::string &, std::string *) { return false; }
    virtual void integer(const Where &, long long) {}
};

/* Parses `in` and re-emits it into `out`, applying the visitor. Any structural
 * surprise sets ok=false with a reason. */
class Rewriter {
  public:
    Rewriter(const std::string &in, Visitor &v) : in_(in), vis_(v) {}

    bool run(std::string *out, std::string *why) {
        Where w;
        walk_struct(w);
        if (ok_ && pos_ != in_.size()) fail("trailing bytes after the footer struct");
        if (!ok_) {
            *why = why_;
            return false;
        }
        *out = out_;
        return true;
    }

  private:
    const std::string &in_;
    Visitor &vis_;
    std::string out_;
    size_t pos_ = 0;
    bool ok_ = true;
    std::string why_;

    void fail(const std::string &m) {
        if (ok_) why_ = m;
        ok_ = false;
    }
    uint8_t byte() {
        if (pos_ >= in_.size()) {
            fail("truncated footer");
            return 0;
        }
        return static_cast<uint8_t>(in_[pos_++]);
    }
    /* reads a varint, echoing its bytes */
    uint64_t varint_echo() {
        uint64_t v = 0;
        int shift = 0;
        for (;;) {
            uint8_t b = byte();
            if (!ok_) return 0;
            out_.push_back(static_cast<char>(b));
            v |= static_cast<uint64_t>(b & 0x7F) << shift;
            if (!(b & 0x80)) break;
            shift += 7;
            if (shift > 63) {
                fail("varint too long");
                return 0;
            }
        }
        return v;
    }
    /* reads a varint WITHOUT echoing (the caller re-emits a possibly new one) */
    uint64_t varint_raw() {
        uint64_t v = 0;
        int shift = 0;
        for (;;) {
            uint8_t b = byte();
            if (!ok_) return 0;
            v |= static_cast<uint64_t>(b & 0x7F) << shift;
            if (!(b & 0x80)) break;
            shift += 7;
            if (shift > 63) {
                fail("varint too long");
                return 0;
            }
        }
        return v;
    }
    void put_varint(uint64_t v) {
        while (v >= 0x80) {
            out_.push_back(static_cast<char>((v & 0x7F) | 0x80));
            v >>= 7;
        }
        out_.push_back(static_cast<char>(v));
    }
    static long long unzigzag(uint64_t z) {
        return static_cast<long long>((z >> 1) ^ (~(z & 1) + 1));
    }
    void copy_bytes(size_t n) {
        if (pos_ + n > in_.size()) {
            fail("truncated footer");
            return;
        }
        out_.append(in_, pos_, n);
        pos_ += n;
    }

    void walk_struct(Where &w) {
        int last_id = 0;
        for (;;) {
            if (!ok_) return;
            const uint8_t hdr = byte();
            if (!ok_) return;
            out_.push_back(static_cast<char>(hdr));
            if (hdr == CT_STOP) return;
            const uint8_t type = hdr & 0x0F;
            const int delta = hdr >> 4;
            int id;
            if (delta != 0) {
                id = last_id + delta;
            } else {
                /* long form: zigzag i16 field id follows */
                id = static_cast<int>(unzigzag(varint_echo()));
                if (!ok_) return;
            }
            last_id = id;
            w.fields.push_back(id);
            walk_value(type, w, /*in_field=*/true);
            w.fields.pop_back();
        }
    }

    void walk_value(uint8_t type, Where &w, bool in_field) {
        if (!ok_) return;
        switch (type) {
        case CT_BOOL_TRUE:
        case CT_BOOL_FALSE:
            /* as a field the value lives in the header; as a container
             * element it is one byte */
            if (!in_field) copy_bytes(1);
            return;
        case CT_BYTE:
            copy_bytes(1);
            return;
        case CT_I16:
        case CT_I32:
        case CT_I64: {
            const uint64_t z = varint_echo();
            if (ok_) vis_.integer(w, unzigzag(z));
            return;
        }
        case CT_DOUBLE:
            copy_bytes(8);
            return;
        case CT_BINARY: {
            const uint64_t len = varint_raw();
            if (!ok_) return;
            if (pos_ + len > in_.size()) {
                fail("truncated binary field");
                return;
            }
            std::string val(in_, pos_, static_cast<size_t>(len));
            pos_ += static_cast<size_t>(len);
            std::string rep;
            if (vis_.binary(w, val, &rep)) {
                put_varint(rep.size());
                out_ += rep;
            } else {
                put_varint(len);
                out_ += val;
            }
            return;
        }
        case CT_LIST:
        case CT_SET: {
            const uint8_t lh = byte();
            if (!ok_) return;
            out_.push_back(static_cast<char>(lh));
            uint64_t size = lh >> 4;
            const uint8_t etype = lh & 0x0F;
            if (size == 15) {
                size = varint_echo();
                if (!ok_) return;
            }
            for (uint64_t e = 0; e < size && ok_; e++) {
                w.list_idx.push_back(static_cast<long long>(e));
                walk_value(etype, w, /*in_field=*/false);
                w.list_idx.pop_back();
            }
            return;
        }
        case CT_MAP: {
            const uint64_t size = varint_echo();
            if (!ok_) return;
            if (size == 0) return;
            const uint8_t kv = byte();
            if (!ok_) return;
            out_.push_back(static_cast<char>(kv));
            const uint8_t ktype = kv >> 4, vtype = kv & 0x0F;
            for (uint64_t e = 0; e < size && ok_; e++) {
                w.list_idx.push_back(static_cast<long long>(e));
                walk_value(ktype, w, false);
                walk_value(vtype, w, false);
                w.list_idx.pop_back();
            }
            return;
        }
        case CT_STRUCT:
            walk_struct(w);
            return;
        default:
            fail("unknown thrift compact type " + std::to_string(type));
            return;
        }
    }
};

/* FileMetaData field ids we rely on (parquet.thrift) */
constexpr int kFmSchema = 2;       /* list<SchemaElement> */
constexpr int kFmRowGroups = 4;    /* list<RowGroup> */
constexpr int kSeName = 4;         /* SchemaElement.name */
constexpr int kSeNumChildren = 5;  /* SchemaElement.num_children */
constexpr int kRgColumns = 1;      /* RowGroup.columns: list<ColumnChunk> */
constexpr int kCcMetaData = 3;     /* ColumnChunk.meta_data */
constexpr int kCmPathInSchema = 3; /* ColumnMetaData.path_in_schema: list<string> */

/* pass 1: schema shape */
struct SchemaCollector : Visitor {
    std::map<long long, std::string> names;       /* element index → name */
    std::map<long long, long long> num_children;  /* element index → count */
    bool binary(const Where &w, const std::string &v, std::string *) override {
        if (w.fields.size() == 2 && w.fields[0] == kFmSchema && w.fields[1] == kSeName &&
            w.list_idx.size() == 1)
            names[w.list_idx[0]] = v;
        return false;
    }
    void integer(const Where &w, long long v) override {
        if (w.fields.size() == 2 && w.fields[0] == kFmSchema &&
            w.fields[1] == kSeNumChildren && w.list_idx.size() == 1)
            num_children[w.list_idx[0]] = v;
    }
};

/* pass 2: positional rename */
struct Renamer : Visitor {
    const std::vector<std::string> &names;
    std::string why;
    explicit Renamer(const std::vector<std::string> &n) : names(n) {}
    bool binary(const Where &w, const std::string &, std::string *out) override {
        if (w.fields.size() == 2 && w.fields[0] == kFmSchema && w.fields[1] == kSeName &&
            w.list_idx.size() == 1) {
            const long long k = w.list_idx[0];
            if (k == 0) return false; /* root element keeps its name */
            if (k - 1 >= static_cast<long long>(names.size())) {
                why = "schema has more leaves than names";
                return false;
            }
            *out = names[static_cast<size_t>(k - 1)];
            return true;
        }
        if (w.fields.size() == 4 && w.fields[0] == kFmRowGroups && w.fields[1] == kRgColumns &&
            w.fields[2] == kCcMetaData && w.fields[3] == kCmPathInSchema &&
            w.list_idx.size() == 3) {
            const long long c = w.list_idx[1], e = w.list_idx[2];
            if (e != 0) {
                why = "nested path_in_schema";
                return false;
            }
            if (c >= static_cast<long long>(names.size())) {
                why = "row group has more columns than names";
                return false;
            }
            *out = names[static_cast<size_t>(c)];
            return true;
        }
        return false;
    }
};

struct Footer {
    std::string bytes;
    uint64_t start = 0; /* offset of the footer struct in the file */
    uint64_t file_size = 0;
};

std::string read_footer(const std::string &path, Footer *f) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) return "cannot stat " + path + ": " + ec.message();
    if (size < 12) return path + " is not a Parquet file (too small)";
    std::ifstream in(path, std::ios::binary);
    if (!in) return "cannot open " + path;
    char head[4];
    in.read(head, 4);
    if (!in || std::memcmp(head, "PAR1", 4) != 0)
        return path + " is not a Parquet file (bad header magic)";
    in.seekg(static_cast<std::streamoff>(size - 8));
    unsigned char tail[8];
    in.read(reinterpret_cast<char *>(tail), 8);
    if (!in || std::memcmp(tail + 4, "PAR1", 4) != 0)
        return path + " is not a Parquet file (bad footer magic)";
    const uint64_t flen = static_cast<uint64_t>(tail[0]) | (static_cast<uint64_t>(tail[1]) << 8) |
                          (static_cast<uint64_t>(tail[2]) << 16) |
                          (static_cast<uint64_t>(tail[3]) << 24);
    if (flen + 12 > size) return path + ": footer length exceeds the file";
    f->start = size - 8 - flen;
    f->file_size = size;
    f->bytes.resize(static_cast<size_t>(flen));
    in.seekg(static_cast<std::streamoff>(f->start));
    in.read(&f->bytes[0], static_cast<std::streamsize>(flen));
    if (!in) return "cannot read the footer of " + path;
    return "";
}

std::string collect_schema(const Footer &f, SchemaCollector *sc) {
    Rewriter rw(f.bytes, *sc);
    std::string out, why;
    if (!rw.run(&out, &why)) return "unrecognised Parquet footer: " + why;
    if (out != f.bytes) return "Parquet footer could not be reproduced byte-for-byte";
    if (sc->names.empty()) return "Parquet footer carries no schema";
    return "";
}

} // namespace

std::string parquet_leaf_names(const std::string &path, std::vector<std::string> *names) {
    Footer f;
    std::string e = read_footer(path, &f);
    if (!e.empty()) return e;
    SchemaCollector sc;
    e = collect_schema(f, &sc);
    if (!e.empty()) return e;
    names->clear();
    for (const auto &kv : sc.names) {
        if (kv.first == 0) continue; /* root */
        auto nc = sc.num_children.find(kv.first);
        if (nc != sc.num_children.end() && nc->second > 0) continue; /* group node */
        names->push_back(kv.second);
    }
    return "";
}

std::string parquet_rename_leaf_columns(const std::string &path,
                                        const std::vector<std::string> &new_names) {
    Footer f;
    std::string e = read_footer(path, &f);
    if (!e.empty()) return e;
    SchemaCollector sc;
    e = collect_schema(f, &sc);
    if (!e.empty()) return e;
    /* flat: root with N children, every other element a childless leaf */
    size_t leaves = 0;
    for (const auto &kv : sc.names) {
        if (kv.first == 0) continue;
        auto nc = sc.num_children.find(kv.first);
        if (nc != sc.num_children.end() && nc->second > 0)
            return "column rename needs a flat Parquet schema (" + kv.second +
                   " is a nested group)";
        leaves++;
    }
    if (leaves != new_names.size())
        return "column rename: the file has " + std::to_string(leaves) +
               " leaf columns but " + std::to_string(new_names.size()) +
               " names were supplied";
    auto root_nc = sc.num_children.find(0);
    if (root_nc == sc.num_children.end() || root_nc->second != static_cast<long long>(leaves))
        return "column rename needs a flat Parquet schema (root child count mismatch)";
    /* no-op? */
    bool same = true;
    for (const auto &kv : sc.names) {
        if (kv.first == 0) continue;
        if (kv.second != new_names[static_cast<size_t>(kv.first - 1)]) { same = false; break; }
    }
    if (same) return "";

    Renamer rn(new_names);
    Rewriter rw(f.bytes, rn);
    std::string out, why;
    if (!rw.run(&out, &why)) return "unrecognised Parquet footer: " + why;
    if (!rn.why.empty()) return "column rename: " + rn.why;
    if (out.size() > 0xFFFFFFFFull) return "rewritten footer too large";

    /* write the new tail: footer, its length, magic; then fit the file size */
    {
        std::fstream io(path, std::ios::in | std::ios::out | std::ios::binary);
        if (!io) return "cannot open " + path + " for writing";
        io.seekp(static_cast<std::streamoff>(f.start));
        io.write(out.data(), static_cast<std::streamsize>(out.size()));
        const uint32_t len = static_cast<uint32_t>(out.size());
        unsigned char tail[8] = {static_cast<unsigned char>(len & 0xFF),
                                 static_cast<unsigned char>((len >> 8) & 0xFF),
                                 static_cast<unsigned char>((len >> 16) & 0xFF),
                                 static_cast<unsigned char>((len >> 24) & 0xFF),
                                 'P', 'A', 'R', '1'};
        io.write(reinterpret_cast<const char *>(tail), 8);
        io.flush();
        if (!io) return "write failed while rewriting the footer of " + path;
    }
    std::error_code ec;
    std::filesystem::resize_file(path, f.start + out.size() + 8, ec);
    if (ec) return "could not resize " + path + ": " + ec.message();
    /* verify: the rewritten footer must parse and carry exactly the new names */
    std::vector<std::string> check;
    e = parquet_leaf_names(path, &check);
    if (!e.empty()) return "post-rename verification failed: " + e;
    if (check != new_names) return "post-rename verification failed: names differ";
    return "";
}

} // namespace parqit
