/* parqit — the SPSS → Parquet conversion plan (SPSS-READ-1).
 *
 * make_plan() reads the dictionary, decodes every case once (the profile
 * pass: it validates the whole data section before anything is written and
 * decides what only the data can decide), and fixes the output: one column
 * per SPSS variable in dictionary order under its SPSS name, then one
 * extended-missing companion column per variable whose user-missing values
 * occur (the parqit.xmissing contract of `parqit save, xmissing`), and the
 * parqit.* key-value metadata a parqit-written file carries (schema, value
 * labels, characteristics, dataset label, xmissing), so every reader of
 * parqit restores the file as it restores its own.
 *
 * Mapping (documented in the help and in ASSUMPTIONS.md):
 *   numeric → DOUBLE; SYSMIS → NULL;
 *   user-missing values → NULL + companion code .a, .b, … (dictionary codes
 *     first — discrete values, then labelled values inside the range, both
 *     ascending — then values observed inside the range, ascending; beyond
 *     26 codes the rest share .z), recorded in char var[spss_missing_map];
 *   date formats → DATE (%td mask) when every value is a whole day, else
 *     TIMESTAMP (%tc mask); DATETIME/YMDHMS → TIMESTAMP; TIME/MTIME → TIME
 *     when every value lies in [0, 24h), else DOUBLE seconds; DTIME →
 *     DOUBLE seconds; values no temporal type holds → DOUBLE SPSS seconds;
 *   strings → VARCHAR, trailing blanks removed, text decoded to UTF-8;
 *   everything Stata has no slot for → `spss_*` characteristics.
 * No Stata API and no DuckDB here.
 */
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "engine/spss_reader.hpp"

namespace parqit {
namespace spss {

enum class OutKind { Double, Date, Timestamp, Time, Varchar };

struct ColumnSpec {
    int var = -1;                /* index into Dictionary::vars */
    std::string name;            /* output column: the SPSS long name */
    std::string stata_name;      /* the name parqit's reader gives it */
    OutKind kind = OutKind::Double;
    std::string stata_type;      /* "double", "long", "str12", "strL" */
    std::string stata_format;    /* display format ("" = the reader's default) */
    /* user-missing value -> extended-missing code 1..26, sorted by value */
    std::vector<std::pair<double, int>> codes;
    int overflow_code = 0;       /* > 0: range values beyond the table share it */
    bool companion = false;      /* a companion column is written */
    std::string companion_name;
    long long user_missing_cells = 0;
    long long transcoded_cells = 0;
    size_t max_bytes = 0;        /* strings: widest UTF-8 value */
    /* the extended-missing code of a user-missing numeric value; 0 = none */
    int code_of(double v) const;
};

struct Plan {
    std::string path;
    Dictionary dict;
    std::vector<ColumnSpec> cols;   /* one per SPSS variable, dictionary order */
    int64_t ncases = 0;             /* counted by the profile pass */
    /* file identity at planning time: the conversion refuses a file that
     * changes before it is done (size and modification time) */
    uint64_t file_size = 0;
    long long file_mtime = 0;
    /* the parqit.* metadata as SQL: KV_METADATA {...} */
    std::string kv_metadata_sql;
    /* what the user is told, one line each (loud notes, never silent) */
    std::vector<std::string> notes;
    std::vector<std::string> xmissing_stata;   /* Stata names with a companion */
    long long transcoded_cells = 0, transcoded_meta = 0;
    size_t companions() const;
};

/* Reads, profiles and plans. Throws SavError. */
Plan make_plan(const std::string &path, const ReadOptions &opt = ReadOptions());

/* SPSS seconds (since 14 Oct 1582) → Unix microseconds / days; false when the
 * value has no such representation. time_us: seconds of a day → micros. */
bool seconds_to_unix_us(double s, int64_t *us);
bool seconds_to_unix_days(double s, int32_t *days);
bool seconds_to_time_us(double s, int64_t *us);

/* the file's current identity (size, modification time); false if unreadable */
bool file_identity(const std::string &path, uint64_t *size, long long *mtime);

/* the Stata display format for an SPSS format and output kind ("" = default) */
std::string stata_format_for(const Format &f, OutKind kind);

} // namespace spss
} // namespace parqit
