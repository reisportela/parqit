/* parqit — the R data frame → Parquet conversion plan (R-READ-1).
 *
 * make_plan() parses the file (rdata_reader.hpp), reads every column once
 * (the profile pass: it decides what only the data can decide and proves the
 * file whole before anything is written), and fixes the output: the character
 * row names first (as `rowname`), then one column per data frame column under
 * its R name, then one extended-missing companion column per column whose
 * haven tagged NAs or SPSS user-missing values occur (the parqit.xmissing
 * contract of `parqit save, xmissing`), and the parqit.* key-value metadata a
 * parqit-written file carries (schema, value labels, characteristics, dataset
 * label, xmissing), so every reader of parqit restores the file as it
 * restores its own.
 *
 * Mapping (documented in the help and in ASSUMPTIONS.md):
 *   logical → BOOLEAN; integer → INTEGER; double → DOUBLE (NA → NULL, NaN
 *     and ±Inf kept); integer64 (bit64) → BIGINT;
 *   factor → INTEGER codes + a value label of its levels (text when Stata
 *     cannot hold the levels in one value label);
 *   Date → DATE (TIMESTAMP when a value holds a fraction of a day);
 *   POSIXct → TIMESTAMP in UTC (char r_tzone keeps the time zone);
 *   hms / ITime → TIME when every value lies in [0, 24h), else seconds;
 *     difftime → its number (char r_units keeps the unit);
 *   character → VARCHAR, decoded to UTF-8;
 *   haven: label → variable label, labels → value labels, format.stata →
 *     display format, na_values/na_range → .a-.z (the SPSS rule), tagged
 *     NAs → .a-.z by their letter;
 *   comment → notes; every other attribute → char r_attributes (JSON).
 * Columns Stata has no type for (lists, nested data frames, complex, raw,
 * matrices, POSIXlt, S4 objects) are left out, each named in a note.
 * No Stata API and no DuckDB here.
 */
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "engine/rdata_reader.hpp"

namespace parqit {
namespace rdata {

enum class OutKind { Boolean, Integer, BigInt, Double, Date, Timestamp, Time, Varchar };

/* how the scan turns an R element into the output value */
enum class Conv {
    Plain,       /* logical, integer, double, character as they are */
    FactorText,  /* integer codes → the levels' text */
    IntDays,     /* integer days since 1970 → DATE */
    RealDays,    /* double days since 1970 → DATE */
    RealDaysTs,  /* double days since 1970 → TIMESTAMP */
    IntSecTs,    /* integer seconds since 1970 (UTC) → TIMESTAMP */
    RealSecTs,   /* double seconds since 1970 (UTC) → TIMESTAMP */
    IntSecTime,  /* integer seconds of a day → TIME */
    RealSecTime, /* double seconds of a day → TIME */
    Int64Bits    /* bit64::integer64 (int64 in a double's bits) → BIGINT */
};

struct ColumnSpec {
    int col = -1;              /* index into Frame::cols; -1 = the character row names */
    VecRef vec;
    std::string name;          /* the Parquet column name */
    std::string engine_name;   /* its name inside the engine (unique ignoring case) */
    std::string stata_name;    /* the name parqit's reader gives it */
    OutKind kind = OutKind::Double;
    Conv conv = Conv::Plain;
    std::vector<std::string> levels; /* FactorText */
    std::string stata_type;    /* "double", "long", "str12", "strL" */
    std::string stata_format;  /* display format ("" = the reader's default) */
    /* SPSS user-missing values carried by haven (na_values / na_range) */
    std::vector<double> na_values;
    bool na_range = false;
    double na_lo = 0.0, na_hi = 0.0;
    /* user-missing value → extended-missing code 1..26, sorted by value */
    std::vector<std::pair<double, int>> codes;
    int overflow_code = 0;
    bool tagged = false;       /* haven tagged NAs keep their letter as the code */
    bool companion = false;    /* a companion column is written */
    std::string companion_name, companion_engine_name;
    long long transcoded_cells = 0;
    size_t max_bytes = 0;
    bool user_missing(double v) const;
    /* the extended-missing code of a user-missing value; 0 = none */
    int code_of(double v) const;
};

struct Plan {
    std::string path;
    Parsed parsed;
    std::vector<ColumnSpec> cols;   /* output order: row names, then the columns */
    int64_t nrow = 0;
    /* file identity at planning time: the conversion refuses a file that
     * changes before it is done (size and modification time) */
    uint64_t file_size = 0;
    long long file_mtime = 0;
    /* the parqit.* metadata as SQL: KV_METADATA {...} */
    std::string kv_metadata_sql;
    /* the exact Parquet names of every output column (companions last), and
     * whether they differ from the engine's names (then the footer is renamed) */
    std::vector<std::string> leaf_names;
    bool rename_leaves = false;
    /* what the user is told, one line each (loud notes, never silent) */
    std::vector<std::string> notes;
    std::vector<std::string> xmissing_stata;   /* Stata names with a companion */
    std::vector<std::string> dropped;          /* R columns left out */
    long long transcoded_cells = 0, transcoded_meta = 0;
    size_t companions() const;
};

/* Reads, profiles and plans. Throws RError. */
Plan make_plan(const std::string &path, const ReadOptions &opt = ReadOptions());

/* the conversions of the scan, shared with the profile so both agree; false
 * when the value has no such representation */
bool days_to_date(double d, int32_t *days);
bool days_to_us(double d, int64_t *us);
bool seconds_to_us(double s, int64_t *us);
bool seconds_to_time_us(double s, int64_t *us);

/* the file's current identity (size, modification time); false if unreadable */
bool file_identity(const std::string &path, uint64_t *size, long long *mtime);

} // namespace rdata
} // namespace parqit
