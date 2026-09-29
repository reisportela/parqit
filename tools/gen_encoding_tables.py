"""Generate src/engine/encoding_tables.{hpp,cpp} (ENC-3).

    python3 tools/gen_encoding_tables.py          (from the repository root)

The tables are taken from Python's own codecs, which follow the Unicode
Consortium and Microsoft mapping files, so parqit decodes legacy text as the
standard tables say rather than as anyone remembers them. Stata's ICU
(`ustrfrom()`) is the independent oracle the verify suite compares against
(tests/verify_suite/v136); where Python's codecs differ from what Windows
itself does with its code pages, Windows wins (below), and ICU agrees.

Single-byte code pages: all 256 bytes, including CP864's 0x25 -> U+066A.
A byte the code page leaves
undefined decodes, as the WHATWG decoders do, to the C1 control of the same
value when it lies in 0x80-0x9F (the mapping stays total and reversible) and
to U+FFFD elsewhere, counted by the decoder.

Double-byte Microsoft code pages (932, 936, 949, 950): every lead/trail pair
of the code page's ranges, plus the meaning of each single upper byte. Windows
maps the end-user-defined character (EUDC) areas of these code pages to the
Private Use Area, linearly over each area's lead/trail grid, and so do ICU's
windows-9xx tables; Python leaves them undefined for 936, 949 and 950 (932's
are there), and reads 950's C6A1-C8FE as the ETEN extensions, which Windows
does not. So: 949 and 950 take the EUDC areas below; 936 takes the pairs it
leaves undefined as Windows reads them: the Private Use code points GB18030
(2000, Python's) gives them, EUDC included — and GBK's reserved cells, which
Windows numbers U+E766-U+E864 in byte order, keep that numbering where GB18030
later gave them real characters (0xA2E3 = U+E76C in 936, the euro in GB18030;
0xFE50 = U+E815, a CJK radical in GB18030): each such run of cells fills
exactly the gap between its neighbours' code points, which the generator
checks.

EUC-JP: JIS X 0208 is read as WHATWG's index-jis0208 is built, through the
windows-31j (932) table by pointer — the NEC row 13 specials, the NEC-selected
IBM extensions and Microsoft's mappings of the few JIS characters whose
Unicode mapping vendors disagree on (0x2141 = U+FF5E, ...), consistently with
Shift_JIS; JIS X 0212 is Python's, except that 0x2237 is U+FF5E, as ICU and
glibc read it (Python: U+007E — an ASCII byte out of a three-byte sequence,
which none of the other tables produce). GB18030: its two-byte table (stored as
changes to 936) and the four-byte BMP ranges, in the GB18030-2005 mapping
that ICU and glibc use (0xA8BC = U+1E3F, 0x8135F437 = U+E7C7; Python keeps
the 2000 mapping, swapped); the supplementary planes are linear.

The generated files are committed; rerun this script only to change the set.
"""
import codecs
import datetime
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_HPP = os.path.join(ROOT, "src", "engine", "encoding_tables.hpp")
OUT_CPP = os.path.join(ROOT, "src", "engine", "encoding_tables.cpp")

UNDEF = 0xFFFF

# canonical name, Python codec, aliases (compared lower-case, without - _ . and spaces)
SINGLE = [
    ("windows-1252", "cp1252", ["cp1252", "windows1252", "ms-ansi", "win1252"]),
    ("latin1", "latin_1", ["iso-8859-1", "iso8859-1", "iso_8859-1", "l1", "cp819", "ibm819", "latin-1",
                           "cp28591", "iso-ir-100", "csisolatin1"]),
    ("latin9", "iso8859_15", ["iso-8859-15", "iso8859-15", "latin-9", "latin0", "l9", "cp28605",
                              "csisolatin9"]),
    ("macroman", "mac_roman", ["mac-roman", "macintosh", "x-mac-roman", "mac", "cp10000", "csmacintosh"]),
    ("windows-874", "cp874", ["cp874", "ms874", "win874"]),
    ("windows-1250", "cp1250", ["cp1250", "win1250", "ms-ee", "x-cp1250"]),
    ("windows-1251", "cp1251", ["cp1251", "win1251", "ms-cyrl", "x-cp1251"]),
    ("windows-1253", "cp1253", ["cp1253", "win1253", "ms-greek"]),
    ("windows-1254", "cp1254", ["cp1254", "win1254", "ms-turk"]),
    ("windows-1255", "cp1255", ["cp1255", "win1255", "ms-hebr"]),
    ("windows-1256", "cp1256", ["cp1256", "win1256", "ms-arab"]),
    ("windows-1257", "cp1257", ["cp1257", "win1257", "winbaltrim"]),
    ("windows-1258", "cp1258", ["cp1258", "win1258"]),
    ("iso-8859-2", "iso8859_2", ["latin2", "l2", "cp28592", "csisolatin2"]),
    ("iso-8859-3", "iso8859_3", ["latin3", "l3", "cp28593"]),
    ("iso-8859-4", "iso8859_4", ["latin4", "l4", "cp28594"]),
    ("iso-8859-5", "iso8859_5", ["cyrillic", "cp28595", "csisolatincyrillic"]),
    ("iso-8859-6", "iso8859_6", ["arabic", "cp28596", "asmo-708"]),
    ("iso-8859-7", "iso8859_7", ["greek", "greek8", "cp28597", "elot_928"]),
    ("iso-8859-8", "iso8859_8", ["hebrew", "cp28598", "iso-8859-8-i"]),
    ("iso-8859-9", "iso8859_9", ["latin5", "l5", "cp28599"]),
    ("iso-8859-10", "iso8859_10", ["latin6", "l6"]),
    ("iso-8859-11", "iso8859_11", ["tis-620", "tis620", "thai"]),
    ("iso-8859-13", "iso8859_13", ["latin7", "l7", "cp28603"]),
    ("iso-8859-14", "iso8859_14", ["latin8", "l8"]),
    ("iso-8859-16", "iso8859_16", ["latin10", "l10"]),
    ("koi8-r", "koi8_r", ["koi8", "cp20866", "cskoi8r"]),
    ("koi8-u", "koi8_u", ["cp21866"]),
    ("ibm437", "cp437", ["cp437", "437", "ibm-437", "cspc8codepage437"]),
    ("ibm737", "cp737", ["cp737", "ibm-737"]),
    ("ibm775", "cp775", ["cp775", "ibm-775"]),
    ("ibm850", "cp850", ["cp850", "850", "ibm-850"]),
    ("ibm852", "cp852", ["cp852", "852", "ibm-852"]),
    ("ibm855", "cp855", ["cp855", "ibm-855"]),
    ("ibm857", "cp857", ["cp857", "ibm-857"]),
    ("ibm858", "cp858", ["cp858", "ibm-858"]),
    ("ibm860", "cp860", ["cp860", "ibm-860"]),
    ("ibm861", "cp861", ["cp861", "ibm-861"]),
    ("ibm862", "cp862", ["cp862", "ibm-862"]),
    ("ibm863", "cp863", ["cp863", "ibm-863"]),
    ("ibm864", "cp864", ["cp864", "ibm-864"]),
    ("ibm865", "cp865", ["cp865", "ibm-865"]),
    ("ibm866", "cp866", ["cp866", "866", "ibm-866", "csibm866"]),
    ("ibm869", "cp869", ["cp869", "ibm-869"]),
    ("mac-cyrillic", "mac_cyrillic", ["maccyrillic", "x-mac-cyrillic", "cp10007"]),
    ("mac-centraleurope", "mac_latin2", ["maccentraleurope", "x-mac-ce", "mac-latin2", "maclatin2",
                                         "x-mac-centraleurroman", "cp10029"]),
    ("mac-greek", "mac_greek", ["macgreek", "x-mac-greek", "cp10006"]),
    ("mac-turkish", "mac_turkish", ["macturkish", "x-mac-turkish", "cp10081"]),
    ("mac-iceland", "mac_iceland", ["maciceland", "x-mac-icelandic", "cp10079"]),
]

# Microsoft double-byte code pages: canonical, codec, lead ranges, trail ranges, aliases
DOUBLE = [
    ("windows-932", "cp932", [(0x81, 0x9F), (0xE0, 0xFC)], [(0x40, 0x7E), (0x80, 0xFC)],
     ["cp932", "shift_jis", "shift-jis", "sjis", "ms932", "ms_kanji", "windows-31j", "x-sjis",
      "csshiftjis", "cswindows31j"]),
    ("windows-936", "cp936", [(0x81, 0xFE)], [(0x40, 0x7E), (0x80, 0xFE)],
     ["cp936", "gbk", "gb2312", "euc-cn", "euccn", "x-gbk", "ms936", "gb_2312-80", "chinese",
      "csgb2312", "iso-ir-58"]),
    ("windows-949", "cp949", [(0x81, 0xFE)], [(0x41, 0x5A), (0x61, 0x7A), (0x81, 0xFE)],
     ["cp949", "euc-kr", "euckr", "uhc", "ks_c_5601-1987", "ks_c_5601-1989", "ksc5601", "ms949",
      "korean", "cseuckr", "iso-ir-149"]),
    ("windows-950", "cp950", [(0x81, 0xFE)], [(0x40, 0x7E), (0xA1, 0xFE)],
     ["cp950", "big5", "big-5", "ms950", "x-big5", "csbig5", "cn-big5"]),
]


def c_list(values, per_line=16, fmt="0x{:04X}", indent="    "):
    lines = []
    for i in range(0, len(values), per_line):
        lines.append(indent + ", ".join(fmt.format(v) for v in values[i:i + per_line]) + ",")
    return "\n".join(lines)


def one_char(b, codec):
    try:
        s = b.decode(codec)
    except UnicodeDecodeError:
        return None
    if len(s) != 1:
        return None
    return ord(s)


def single_table(codec):
    out = []
    for b in range(0x100):
        cp = one_char(bytes([b]), codec)
        if cp is None:
            cp = b if b <= 0x9F else 0xFFFD  # the WHATWG rule: C1 control, else U+FFFD
        assert cp < 0x10000
        out.append(cp)
    return out


def in_ranges(x, ranges):
    return any(lo <= x <= hi for lo, hi in ranges)


def double_tables(codec, leads, trails):
    lead_lo, lead_hi = min(l for l, _ in leads), max(h for _, h in leads)
    trail_lo, trail_hi = min(l for l, _ in trails), max(h for _, h in trails)
    span = trail_hi - trail_lo + 1
    pairs = []
    for lead in range(lead_lo, lead_hi + 1):
        for trail in range(trail_lo, trail_hi + 1):
            cp = None
            if in_ranges(lead, leads) and in_ranges(trail, trails):
                cp = one_char(bytes([lead, trail]), codec)
            pairs.append(UNDEF if cp is None else cp)
    single = []
    for b in range(0x80, 0x100):
        if in_ranges(b, leads):
            single.append(UNDEF)  # a lead byte: never a character on its own
            continue
        cp = one_char(bytes([b]), codec)
        single.append(UNDEF if cp is None else cp)
    return (lead_lo, lead_hi, trail_lo, trail_hi, span), pairs, single


# Windows' EUDC areas: (canonical, [(lead, trail ranges) ...] in order, first
# code point, last code point)
EUDC = [
    ("windows-949", [(0xC9, [(0xA1, 0xFE)])], 0xE000, 0xE05D),
    ("windows-949", [(0xFE, [(0xA1, 0xFE)])], 0xE05E, 0xE0BB),
    ("windows-950", [(l, [(0x40, 0x7E), (0xA1, 0xFE)]) for l in range(0xFA, 0xFF)], 0xE000, 0xE310),
    ("windows-950", [(l, [(0x40, 0x7E), (0xA1, 0xFE)]) for l in range(0x8E, 0xA1)], 0xE311, 0xEEB7),
    ("windows-950", [(l, [(0x40, 0x7E), (0xA1, 0xFE)]) for l in range(0x81, 0x8E)], 0xEEB8, 0xF6B0),
    ("windows-950", [(0xC6, [(0xA1, 0xFE)])] +
     [(l, [(0x40, 0x7E), (0xA1, 0xFE)]) for l in (0xC7, 0xC8)], 0xF6B1, 0xF848),
]


def apply_eudc(canonical, meta, pairs):
    lead_lo, _, trail_lo, _, span = meta
    replaced = 0
    for name, area, first, last in EUDC:
        if name != canonical:
            continue
        cp = first
        for lead, trails in area:
            for lo, hi in trails:
                for trail in range(lo, hi + 1):
                    i = (lead - lead_lo) * span + (trail - trail_lo)
                    replaced += pairs[i] != UNDEF
                    pairs[i] = cp
                    cp += 1
        if cp - 1 != last:
            raise SystemExit(f"{canonical} EUDC area ends at U+{cp - 1:04X}, not U+{last:04X}")
    return replaced


def sjis_bytes(pointer):
    """WHATWG's Shift_JIS pointer -> the two bytes of windows-31j"""
    lead_off, trail_off = divmod(pointer, 188)
    lead = lead_off + (0x81 if lead_off < 0x1F else 0xC1)
    trail = trail_off + (0x40 if trail_off < 0x3F else 0x41)
    return bytes([lead, trail])


def jis_tables():
    t0208, t0212 = [], []
    for row in range(0xA1, 0xFF):
        for col in range(0xA1, 0xFF):
            cp = one_char(sjis_bytes((row - 0xA1) * 94 + (col - 0xA1)), "cp932")
            t0208.append(UNDEF if cp is None else cp)
            cp = one_char(bytes([0x8F, row, col]), "euc_jp")
            if (row, col) == (0xA2, 0xB7):
                cp = 0xFF5E
            t0212.append(UNDEF if cp is None else cp)
    ascii_out = [i for i, cp in enumerate(t0208 + t0212) if cp < 0x80]
    if ascii_out:
        raise SystemExit(f"EUC-JP tables decode {len(ascii_out)} multibyte sequence(s) to ASCII")
    return t0208, t0212


GB18030_2005 = {bytes([0xA8, 0xBC]): 0x1E3F, bytes([0x81, 0x35, 0xF4, 0x37]): 0xE7C7}


def fill_cp936(pairs):
    """The pairs Python's cp936 leaves undefined, as Windows' 936 reads them."""
    seq = []  # (index, Private Use code point or None) of GBK's reserved cells, in byte order
    filled = 0
    for i, v in enumerate(pairs):
        if v != UNDEF:
            continue
        lead, trail = 0x81 + i // 191, 0x40 + i % 191
        g = one_char(bytes([lead, trail]), "gb18030")
        if g is None:
            continue
        if 0xE000 <= g <= 0xF8FF and not 0xE766 <= g <= 0xE864:
            pairs[i] = g  # EUDC
            filled += 1
        elif 0xE766 <= g <= 0xE864:
            seq.append((i, g))
        else:
            seq.append((i, None))
    prev, pending = 0xE765, []
    for i, g in seq + [(None, 0xE865)]:
        if g is None:
            pending.append(i)
            continue
        if g <= prev or g - prev - 1 != len(pending):
            raise SystemExit(f"936 reserved cells: {len(pending)} cell(s) between U+{prev:04X} and U+{g:04X}")
        for k, j in enumerate(pending):
            pairs[j] = prev + 1 + k
        filled += len(pending) + (i is not None)
        if i is not None:
            pairs[i] = g
        prev, pending = g, []
    return filled


def gb18030_char(seq):
    return GB18030_2005.get(seq, one_char(seq, "gb18030"))


def gb18030_pairs():
    pairs = []
    for lead in range(0x81, 0xFF):
        for trail in range(0x40, 0xFF):
            cp = None
            if trail != 0x7F:
                cp = gb18030_char(bytes([lead, trail]))
            pairs.append(UNDEF if cp is None else cp)
    return pairs


def gb18030_tables(cp936_pairs):
    pairs = gb18030_pairs()
    diffs = [(i, v) for i, (v, w) in enumerate(zip(pairs, cp936_pairs)) if v != w]
    # four-byte sequences of the BMP: pointer -> code point, as runs
    ranges = []
    for p in range(0, 39420):
        b1, r = divmod(p, 12600)
        b2, r = divmod(r, 1260)
        b3, b4 = divmod(r, 10)
        seq = bytes([0x81 + b1, 0x30 + b2, 0x81 + b3, 0x30 + b4])
        cp = gb18030_char(seq)
        if cp is None:
            raise SystemExit(f"gb18030 four-byte pointer {p} does not decode")
        if ranges and cp - ranges[-1][1] == p - ranges[-1][0]:
            continue
        ranges.append((p, cp))
    return diffs, ranges


def main():
    ver = sys.version.split()[0]
    today = datetime.date.today().isoformat()
    names = []  # (canonical, kind, index)
    aliases = {}

    def add_alias(alias, canonical):
        key = "".join(ch for ch in alias.lower() if ch not in "-_. ")
        if key in aliases and aliases[key] != canonical:
            raise SystemExit(f"alias {alias} maps to both {aliases[key]} and {canonical}")
        aliases[key] = canonical

    cpp = []
    cpp.append(f"/* Generated by tools/gen_encoding_tables.py from the codecs of Python {ver}\n"
               f" * ({today}) — do not edit by hand; see the generator for the rules. */\n")
    cpp.append('#include "engine/encoding_tables.hpp"\n\nnamespace parqit {\nnamespace enctab {\n')

    single_rows = []
    for canonical, codec, al in SINGLE:
        table = single_table(codec)
        single_rows.append((canonical, table))
        for a in [canonical, codec] + al:
            add_alias(a, canonical)
    cpp.append("const uint16_t kSingleTable[kSingleCount][256] = {")
    for canonical, table in single_rows:
        cpp.append(f"    /* {canonical} */ {{")
        cpp.append(c_list(table, per_line=8, indent="        "))
        cpp.append("    },")
    cpp.append("};\n")
    cpp.append("const bool kSingleAsciiIdentity[kSingleCount] = {")
    cpp.append("    " + ", ".join("true" if t[:128] == list(range(128)) else "false"
                                  for _, t in single_rows) + ",")
    cpp.append("};\n")
    cpp.append("const char *const kSingleName[kSingleCount] = {")
    cpp.append("    " + ", ".join(f'"{c}"' for c, _ in single_rows) + ",")
    cpp.append("};\n")

    dbcs_meta = []
    cp936_pairs = None
    eudc_note = []
    for canonical, codec, leads, trails, al in DOUBLE:
        meta, pairs, single = double_tables(codec, leads, trails)
        ident = canonical.replace("windows-", "Cp")
        if canonical == "windows-936":
            filled = fill_cp936(pairs)
            eudc_note.append(f"936 +{filled} Private Use pairs")
            cp936_pairs = pairs
            # Microsoft's 936 maps the single byte 0x80 to the euro sign (as
            # WHATWG's gbk decoder does); Python's cp936 leaves it undefined
            single[0] = 0x20AC
        replaced = apply_eudc(canonical, meta, pairs)
        if replaced:
            eudc_note.append(f"{canonical}: {replaced} Python mappings replaced by EUDC")
        dbcs_meta.append((canonical, ident, meta, leads, trails))
        cpp.append(f"const uint16_t k{ident}Pairs[{len(pairs)}] = {{")
        cpp.append(c_list(pairs))
        cpp.append("};\n")
        cpp.append(f"const uint16_t k{ident}Single[128] = {{")
        cpp.append(c_list(single, per_line=8))
        cpp.append("};\n")
        for a in [canonical, codec] + al:
            add_alias(a, canonical)

    t0208, t0212 = jis_tables()
    cpp.append(f"const uint16_t kJis0208[94 * 94] = {{\n{c_list(t0208)}\n}};\n")
    cpp.append(f"const uint16_t kJis0212[94 * 94] = {{\n{c_list(t0212)}\n}};\n")
    for a in ["euc-jp", "eucjp", "ujis", "x-euc-jp", "cseucpkdfmtjapanese", "cp51932", "cp20932"]:
        add_alias(a, "euc-jp")

    diffs, ranges = gb18030_tables(cp936_pairs)
    cpp.append(f"const PairDiff kGb18030Diff[kGb18030DiffCount] = {{")
    for i in range(0, len(diffs), 6):
        cpp.append("    " + " ".join(f"{{{i2}, 0x{v:04X}}}," for i2, v in diffs[i:i + 6]))
    cpp.append("};\n")
    cpp.append(f"const Range4 kGb18030Ranges[kGb18030RangeCount] = {{")
    for i in range(0, len(ranges), 6):
        cpp.append("    " + " ".join(f"{{{p}, 0x{cp:04X}}}," for p, cp in ranges[i:i + 6]))
    cpp.append("};\n")
    for a in ["gb18030", "gb-18030", "cp54936", "windows-54936"]:
        add_alias(a, "gb18030")

    for a in ["utf-8", "utf8", "cp65001", "unicode-1-1-utf-8"]:
        add_alias(a, "utf-8")
    for a in ["utf-16le", "utf16le", "cp1200", "ucs-2le"]:
        add_alias(a, "utf-16le")
    for a in ["utf-16be", "utf16be", "cp1201", "ucs-2be"]:
        add_alias(a, "utf-16be")
    for a in ["utf-16", "utf16", "ucs-2", "unicode"]:
        add_alias(a, "utf-16")

    keys = sorted(aliases)
    cpp.append("const Alias kAlias[kAliasCount] = {")
    for k in keys:
        cpp.append(f'    {{"{k}", "{aliases[k]}"}},')
    cpp.append("};\n")
    cpp.append("} // namespace enctab\n} // namespace parqit\n")

    hpp = f"""/* Generated by tools/gen_encoding_tables.py from the codecs of Python {ver}
 * ({today}) — do not edit by hand; see the generator for the rules.
 *
 * The code-page tables behind engine/legacy_encoding (ENC-3). 0xFFFF marks a
 * byte or byte pair the code page does not define. */
#pragma once

#include <cstddef>
#include <cstdint>

namespace parqit {{
namespace enctab {{

constexpr size_t kSingleCount = {len(single_rows)};
extern const uint16_t kSingleTable[kSingleCount][256];
extern const bool kSingleAsciiIdentity[kSingleCount];
extern const char *const kSingleName[kSingleCount];

/* Microsoft double-byte code pages: pairs indexed
 * (lead - lead_lo) * span + (trail - trail_lo); Single = bytes 0x80-0xFF */
"""
    for canonical, ident, (llo, lhi, tlo, thi, span), leads, trails in dbcs_meta:
        n = (lhi - llo + 1) * span
        hpp += (f"constexpr uint8_t k{ident}LeadLo = 0x{llo:02X}, k{ident}LeadHi = 0x{lhi:02X}, "
                f"k{ident}TrailLo = 0x{tlo:02X}, k{ident}TrailHi = 0x{thi:02X};\n"
                f"extern const uint16_t k{ident}Pairs[{n}];\nextern const uint16_t k{ident}Single[128];\n")
    hpp += f"""
/* EUC-JP: JIS X 0208 and JIS X 0212, row-major over 0xA1-0xFE x 0xA1-0xFE */
extern const uint16_t kJis0208[94 * 94];
extern const uint16_t kJis0212[94 * 94];

/* GB18030: the two-byte table is 936's with these changes (index into the
 * 0x81-0xFE x 0x40-0xFE rectangle), and the four-byte BMP pointers map by
 * runs: the last range at or below a pointer gives cp + (pointer - start) */
struct PairDiff {{
    uint32_t index;
    uint16_t cp;
}};
constexpr size_t kGb18030DiffCount = {len(diffs)};
extern const PairDiff kGb18030Diff[kGb18030DiffCount];
struct Range4 {{
    uint32_t pointer;
    uint32_t cp;
}};
constexpr size_t kGb18030RangeCount = {len(ranges)};
extern const Range4 kGb18030Ranges[kGb18030RangeCount];

/* every accepted name, lower-case without - _ . and blanks, sorted */
struct Alias {{
    const char *key;
    const char *canonical;
}};
constexpr size_t kAliasCount = {len(keys)};
extern const Alias kAlias[kAliasCount];

}} // namespace enctab
}} // namespace parqit
"""
    with open(OUT_HPP, "w", encoding="utf-8", newline="\n") as f:
        f.write(hpp)
    with open(OUT_CPP, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(cpp))
    print(f"single-byte {len(single_rows)}, double-byte {len(dbcs_meta)}, gb18030 diffs {len(diffs)}, "
          f"ranges {len(ranges)}, aliases {len(keys)}; " + "; ".join(eudc_note))


if __name__ == "__main__":
    main()
