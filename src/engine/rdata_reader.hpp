/* parqit — native reader of R data files (.rds, .rda/.RData), R-READ-1.
 *
 * An R data file is R's own serialization of objects (src/main/serialize.c of
 * R 4.6.0 is the reference this reader follows; ASSUMPTIONS.md records the
 * source commit): `saveRDS` writes one object, `save` a pairlist of named
 * objects behind the "RDX2\n"/"RDX3\n" magic. parqit reads the XDR form (what
 * R writes unless asked for ascii = TRUE), serialization versions 2 and 3,
 * uncompressed or compressed with gzip (R's default) or zstd; bzip2, xz, the
 * ASCII and the native-binary forms are refused, saying how to re-save.
 *
 * A compressed file is inflated once into a temporary file (an uncompressed
 * file is read in place). A first pass parses the structure: attributes are
 * loaded (they are small), while every vector that may be a data frame column
 * is only located (its length and the offset of its first element), so the
 * data are read later, column by column, through a Cursor. Nothing is
 * evaluated: environments, functions (byte-compiled or not) and external
 * pointers are passed over without recursion, so any workspace parses.
 * No Stata API and no DuckDB here.
 */
#pragma once

#include <cstdint>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "engine/legacy_encoding.hpp"

namespace parqit {
namespace rdata {

struct RError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/* the file is readable, but which of its objects to read must be named (or
 * the name given is not a data frame in it) */
struct RChoiceError : RError {
    using RError::RError;
};

/* SEXPTYPEs (Rinternals.h) and the serialization codes (serialize.c) */
enum : int {
    NILSXP = 0, SYMSXP = 1, LISTSXP = 2, CLOSXP = 3, ENVSXP = 4, PROMSXP = 5,
    LANGSXP = 6, SPECIALSXP = 7, BUILTINSXP = 8, CHARSXP = 9, LGLSXP = 10,
    INTSXP = 13, REALSXP = 14, CPLXSXP = 15, STRSXP = 16, DOTSXP = 17,
    VECSXP = 19, EXPRSXP = 20, BCODESXP = 21, EXTPTRSXP = 22, WEAKREFSXP = 23,
    RAWSXP = 24, OBJSXP = 25,
    ALTREP_SXP = 238, ATTRLISTSXP = 239, ATTRLANGSXP = 240, BASEENV_SXP = 241,
    EMPTYENV_SXP = 242, BCREPREF = 243, BCREPDEF = 244, GENERICREFSXP = 245,
    CLASSREFSXP = 246, PERSISTSXP = 247, PACKAGESXP = 248, NAMESPACESXP = 249,
    BASENAMESPACE_SXP = 250, MISSINGARG_SXP = 251, UNBOUNDVALUE_SXP = 252,
    GLOBALENV_SXP = 253, NILVALUE_SXP = 254, REFSXP = 255
};

/* the CHARSXP encoding bits in an item's levels (Defn.h) */
enum : int { kBytesMask = 1 << 1, kLatin1Mask = 1 << 2, kUtf8Mask = 1 << 3, kAsciiMask = 1 << 6 };

/* R's missing integer (and logical) */
constexpr int32_t kNaInteger = INT32_MIN;

enum class Compression { None, Gzip, Zstd };

/* The serialization stream, readable at any offset. An uncompressed file is
 * used in place; a compressed one is inflated into a temporary file under
 * `tmpdir` (the system's when empty), removed with the Source. */
class Source {
public:
    Source(const std::string &path, const std::string &tmpdir);
    ~Source();
    Source(const Source &) = delete;
    Source &operator=(const Source &) = delete;
    const std::string &path() const { return path_; }
    const std::string &data_path() const { return data_path_; }
    Compression compression() const { return compression_; }
    uint64_t size() const { return size_; }
    /* up to n bytes at `offset`; fewer only at the end of the stream */
    size_t read(uint64_t offset, char *dst, size_t n);

private:
    std::string path_, data_path_;
    bool temp_ = false;
    Compression compression_ = Compression::None;
    uint64_t size_ = 0;
    std::ifstream in_;
};

/* How the bytes of a string become UTF-8: by the CHARSXP's own encoding flag
 * (UTF-8, latin1, bytes, ASCII) or, for an unflagged string, by the writer's
 * native encoding from the version-3 header (replaced by encoding() when the
 * user gives one). */
class TextCodec {
public:
    /* throws RError when `override_name` or `default_name` names an encoding
     * parqit cannot decode. `default_name` ("" = windows-1252) is the code
     * page of text that declares none (a version-2 file's unmarked strings
     * that are not UTF-8) and of text that is not what it declares; an
     * encoding() override takes that role too (ENC-3). */
    TextCodec(const std::string &native_name, const std::string &override_name,
              const std::string &default_name = std::string());
    /* Decodes one string. Returns true when the bytes did not match what the
     * file declared — transcoded from the fallback code page, or holding bytes
     * the declared encoding does not define (now U+FFFD). Throws RError for a
     * non-ASCII unflagged string in a native encoding parqit cannot decode
     * (and that is not valid UTF-8 either). */
    bool decode(const char *p, size_t n, int levels, std::string *out) const;
    /* what unflagged strings were read as ("UTF-8", "latin1", ...) */
    std::string native_used() const;
    /* the code page of text that is not what it declares */
    const char *fallback_name() const { return legacy_encoding_name(fallback_); }

private:
    enum class Kind { Utf8, Legacy, Ascii, Undeclared, Unknown };
    Kind kind_ = Kind::Undeclared;
    LegacyEncoding legacy_ = LegacyEncoding::Windows1252;
    LegacyEncoding fallback_ = LegacyEncoding::Windows1252;
    std::string name_;
};

/* where a vector's elements are, without loading them */
struct VecRef {
    int type = NILSXP;       /* LGLSXP, INTSXP, REALSXP or STRSXP */
    uint64_t length = 0;
    uint64_t offset = 0;     /* the first element; for STRSXP its CHARSXP item */
    enum class Alt { None, IntSeq, RealSeq, DeferredInt } alt = Alt::None;
    double seq_first = 0.0, seq_incr = 0.0;
    std::shared_ptr<VecRef> arg; /* DeferredInt: the INTSXP turned into text */
};

struct RValue;
using Attrs = std::vector<std::pair<std::string, std::shared_ptr<RValue>>>;

/* a value loaded in full: attributes and the small objects around the data */
struct RValue {
    int type = NILSXP;
    std::vector<int32_t> ints;       /* LGLSXP, INTSXP */
    std::vector<double> reals;       /* REALSXP; CPLXSXP as re, im pairs */
    std::vector<std::string> strs;   /* STRSXP, decoded to UTF-8 */
    std::vector<char> na;            /* STRSXP: 1 = NA_character_ */
    std::string raw;                 /* RAWSXP bytes */
    std::vector<std::shared_ptr<RValue>> elts; /* VECSXP, EXPRSXP, pairlist values */
    std::vector<std::string> tags;   /* pairlist tags ("" = none) */
    std::string sym;                 /* SYMSXP print name */
    std::string what;                /* what an object not kept is ("an environment") */
    uint64_t length = 0;             /* the vector's length, also when not kept */
    Attrs attrs;
    const RValue *attr(const std::string &name) const;
    /* the class attribute's strings, empty when absent */
    std::vector<std::string> classes() const;
    bool inherits(const std::string &cls) const;
    /* a character vector's first element ("" when not one, or NA) */
    std::string str1() const;
    bool kept() const { return what.empty(); }
};

struct Column {
    std::string name;         /* the R name ("" when the name is empty or NA) */
    bool name_na = false;
    VecRef vec;               /* type NILSXP when the column cannot be carried */
    Attrs attrs;
    std::string unsupported;  /* why it cannot be carried ("" = it can) */
    /* character in R, but still the numbers of a deferred as.character():
     * `vec` is the REALSXP R would have turned into text on access */
    bool deferred_numbers = false;
};

struct Frame {
    std::string object;       /* the object read: its name in an .RData, or its
                                 name in the list an .rds holds; "" otherwise */
    std::vector<Column> cols;
    uint64_t nrow = 0;
    Attrs attrs;              /* the data frame's attributes, row.names excluded */
    enum class RowNames { Automatic, Integer, Text } rownames = RowNames::Automatic;
    VecRef rownames_vec;      /* Integer (not compact) and Text row names */
};

struct FileInfo {
    Compression compression = Compression::None;
    bool rdata = false;       /* an .RData (save) rather than an .rds (saveRDS) */
    int version = 0;          /* serialization version: 2 or 3 */
    int writer_version = 0, min_reader_version = 0;
    std::string native_encoding; /* version 3: the writer's native encoding */
    /* the objects of an .RData, or of the list an .rds holds, and what each is */
    std::vector<std::pair<std::string, std::string>> objects;
    std::string root_what;    /* what an .rds holds ("a data frame", "a list") */
};

struct ReadOptions {
    std::string object;   /* which object to read ("" = the only data frame) */
    std::string encoding; /* replaces the native encoding of unflagged strings */
    std::string default_encoding; /* the session's code page for undeclared text */
    std::string tmpdir;   /* where a compressed file is inflated */
};

struct Parsed {
    FileInfo info;
    Frame frame;
    std::shared_ptr<Source> source;
    std::shared_ptr<const TextCodec> codec;
    /* attribute and name strings transcoded because they were not what the
     * file declared */
    long long transcoded_meta = 0;
    std::vector<std::string> notes;
};

/* Parses the file and returns the data frame it holds (the chosen object).
 * Throws RError with a message the user can act on. */
Parsed read_file(const std::string &path, const ReadOptions &opt);

/* "4.6.0" from R's packed version integer */
std::string r_version_string(int packed);

/* NA_real_ (haven's tagged NA included) versus a plain NaN: R's R_IsNA */
bool is_na_real(double x);
/* haven's tag of a tagged NA (the byte it stores in the high word), 0 for a
 * plain NA or any other value */
char na_tag(double x);

/* Reads a located vector element by element (the data passes). One cursor
 * per column; the cursors share the Source and keep their own position and
 * buffer. */
class Cursor {
public:
    Cursor(std::shared_ptr<Source> src, const VecRef &v, std::shared_ptr<const TextCodec> codec,
           size_t buffer_bytes = 1 << 16);
    uint64_t index() const { return index_; }
    /* INTSXP / LGLSXP element (kNaInteger for NA) */
    int32_t next_int();
    /* REALSXP element, bit pattern preserved (the caller tests NA and NaN) */
    double next_real();
    /* STRSXP element: false for NA_character_; *transcoded when the bytes
     * did not match their declaration */
    bool next_string(std::string *utf8, bool *transcoded);

private:
    void fill(size_t need);
    uint32_t u32();
    std::shared_ptr<Source> src_;
    VecRef v_;
    std::shared_ptr<const TextCodec> codec_;
    uint64_t pos_ = 0; /* stream offset of buf_[0] */
    std::vector<char> buf_;
    size_t at_ = 0, len_ = 0;
    uint64_t index_ = 0;
    std::unique_ptr<Cursor> inner_; /* DeferredInt: the argument's cursor */
};

} // namespace rdata
} // namespace parqit
