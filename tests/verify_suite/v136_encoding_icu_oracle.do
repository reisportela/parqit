* V136 — ENC-3 (independent oracle): every byte sequence of every legacy
* encoding parqit decodes goes through the public save path (parqit save ...,
* data encoding(name, all)), is read back from the Parquet file by pyarrow, and
* is compared with ICU's decoding of the same bytes (Stata's ustrfrom()): the
* bytes 1-255 of each single-byte code page ICU has (NUL is checked in C++), every lead/trail pair
* of Shift_JIS (932), GBK (936), UHC (949) and Big5 (950), EUC-JP's two- and
* three-byte sequences, GB18030's two-byte pairs, all 39,420 four-byte
* sequences of the BMP and a sample of the supplementary planes. parqit's
* tables come from Python's codecs, corrected where Windows reads its code
* pages differently (tools/gen_encoding_tables.py), so ICU is a second,
* independent implementation. The differences that remain are listed below,
* each with its reason; any other difference fails. A sequence both decoders
* reject (a lead byte with an invalid trail) is an error either way and is not
* compared: the two write their U+FFFD differently.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile stem
local fails 0

mata:
// the byte sequences of one family; the Python oracle below rebuilds them
// from the same rule, by row
string colvector _v136_seqs(string scalar kind, real scalar lo, real scalar hi,
                            real scalar tlo, real scalar thi)
{
    string colvector v
    real scalar a, b, n

    if (kind == "single") {
        v = J(hi - lo + 1, 1, "")
        for (a = lo; a <= hi; a++) v[a - lo + 1] = char(a)
    }
    else if (kind == "pair") {
        v = J((hi - lo + 1) * (thi - tlo + 1), 1, "")
        n = 0
        for (a = lo; a <= hi; a++) for (b = tlo; b <= thi; b++) v[++n] = char(a) + char(b)
    }
    else if (kind == "jis0212") {
        v = J(94 * 94, 1, "")
        n = 0
        for (a = 161; a <= 254; a++) for (b = 161; b <= 254; b++) v[++n] = char(143) + char(a) + char(b)
    }
    else {
        // gb4: four-byte GB18030 pointers lo..hi in steps of tlo
        v = J(floor((hi - lo) / tlo) + 1, 1, "")
        n = 0
        for (a = lo; a <= hi; a = a + tlo) {
            v[++n] = char(129 + floor(a / 12600)) + char(48 + floor(mod(a, 12600) / 1260)) +
                     char(129 + floor(mod(a, 1260) / 10)) + char(48 + mod(a, 10))
        }
    }
    return(v)
}
end

program define _v136_one
    args tag ours icu kind lo hi tlo thi
    clear
    mata: _v136_v = _v136_seqs("`kind'", `lo', `hi', 0`tlo', 0`thi')
    mata: st_addobs(rows(_v136_v)); (void) st_addvar("str8", "raw"); st_sstore(., "raw", _v136_v)
    gen long id = _n
    gen strL icu = ustrfrom(raw, "`icu'", 1)
    quietly count if icu == ""
    if (r(N) == _N) {
        di as err "ICU has no converter named `icu'"
        exit 111
    }
    preserve
    keep id icu
    quietly parqit save `"${V136_STEM}_`tag'_icu.parquet"', data replace
    restore
    keep id raw
    quietly parqit save `"${V136_STEM}_`tag'_ours.parquet"', data replace encoding(`ours', all)
    di "`tag': `ours' (ICU `icu'), " _N " sequences"
end

global V136_STEM `"`stem'"'
local singles windows-1252:windows-1252 latin1:ISO-8859-1 latin9:ISO-8859-15 macroman:macintosh ///
    windows-874:windows-874 windows-1250:windows-1250 windows-1251:windows-1251 ///
    windows-1253:windows-1253 windows-1254:windows-1254 windows-1255:windows-1255 ///
    windows-1256:windows-1256 windows-1257:windows-1257 windows-1258:windows-1258 ///
    iso-8859-2:ISO-8859-2 iso-8859-3:ISO-8859-3 iso-8859-4:ISO-8859-4 iso-8859-5:ISO-8859-5 ///
    iso-8859-6:ISO-8859-6 iso-8859-7:ISO-8859-7 iso-8859-8:ISO-8859-8 iso-8859-9:ISO-8859-9 ///
    iso-8859-10:ISO-8859-10 iso-8859-11:ISO-8859-11 iso-8859-13:ISO-8859-13 ///
    iso-8859-14:ISO-8859-14 koi8-r:KOI8-R koi8-u:KOI8-U ibm437:ibm-437 ibm737:ibm-737 ///
    ibm775:ibm-775 ibm850:ibm-850 ibm852:ibm-852 ibm855:ibm-855 ibm857:ibm-857 ///
    ibm858:ibm-858 ibm860:ibm-860 ibm861:ibm-861 ibm862:ibm-862 ibm863:ibm-863 ///
    ibm864:ibm-864 ibm865:ibm-865 ibm866:ibm-866 ibm869:ibm-869 ///
    mac-cyrillic:x-mac-cyrillic mac-centraleurope:x-mac-centraleurroman ///
    mac-greek:x-mac-greek mac-turkish:x-mac-turkish
local runs ""
foreach p of local singles {
    gettoken ours icu : p, parse(":")
    local icu = substr("`icu'", 2, .)
    local tag = subinstr("`ours'", "-", "_", .)
    capture noisily _v136_one `tag' `ours' `icu' single 1 255
    if (_rc) local ++fails
    else local runs "`runs' `tag':`ours':single"
}
* the double-byte code pages, EUC-JP and GB18030 (lead x trail as in the tables)
foreach p in "cp932 windows-932 windows-31j pair 129 252 64 252" ///
             "cp936 windows-936 windows-936 pair 129 254 64 254" ///
             "cp949 windows-949 windows-949 pair 129 254 65 254" ///
             "cp950 windows-950 windows-950 pair 129 254 64 254" ///
             "eucjp2 euc-jp EUC-JP pair 142 254 161 254" ///
             "eucjp3 euc-jp EUC-JP jis0212 0 0" ///
             "gb2 gb18030 gb18030 pair 129 254 64 254" ///
             "gb4 gb18030 gb18030 gb4 0 39419 1" ///
             "gb4sup gb18030 gb18030 gb4 189000 1237575 97" {
    gettoken tag rest : p
    gettoken ours rest : rest
    gettoken icu rest : rest
    gettoken kind rest : rest
    capture noisily _v136_one `tag' `ours' `icu' `kind' `rest'
    if (_rc) local ++fails
    else local runs "`runs' `tag':`ours':`kind'"
}
global V136_RUNS "`runs'"

python:
from sfi import Macro
import pyarrow.parquet as pq

stem = Macro.getGlobal("V136_STEM")
runs = [r.split(":") for r in Macro.getGlobal("V136_RUNS").split()]

def seqs(tag, kind):
    if kind == "single":
        return [bytes([b]) for b in range(1, 256)]
    if kind == "jis0212":
        return [bytes([0x8F, a, b]) for a in range(161, 255) for b in range(161, 255)]
    if kind == "gb4":
        lo, hi, step = {"gb4": (0, 39419, 1), "gb4sup": (189000, 1237575, 97)}[tag]
        out = []
        for p in range(lo, hi + 1, step):
            out.append(bytes([0x81 + p // 12600, 0x30 + p % 12600 // 1260, 0x81 + p % 1260 // 10, 0x30 + p % 10]))
        return out
    lo, hi, tlo, thi = {"cp932": (129, 252, 64, 252), "cp936": (129, 254, 64, 254),
                        "cp949": (129, 254, 65, 254), "cp950": (129, 254, 64, 254),
                        "eucjp2": (142, 254, 161, 254), "gb2": (129, 254, 64, 254)}[tag]
    return [bytes([a, b]) for a in range(lo, hi + 1) for b in range(tlo, thi + 1)]

def err(s):
    return "�" in s or "\x1a" in s

# The differences that remain, each for a reason (ICU is IBM's tables; parqit
# follows Microsoft's, the Unicode Consortium's and WHATWG's):
ALLOW = {
    # Microsoft leaves these bytes undefined (U+FFFD); ICU maps them to IBM's Private Use
    "windows-874": {bytes([b]) for b in (0xDB, 0xDC, 0xDD, 0xDE, 0xFC, 0xFD, 0xFE, 0xFF)},
    # Microsoft leaves 0xAA undefined; ICU maps it to U+00AA
    "windows-1253": {b"\xaa"},
    # CP437.TXT (Unicode/Microsoft), and the DOS code pages derived from it: 0xE6
    # is U+00B5 MICRO SIGN; ICU: U+03BC GREEK MU
    **{cp: {b"\xe6"} for cp in ("ibm437", "ibm860", "ibm861", "ibm862", "ibm863", "ibm865")},
    # Microsoft: 0xAA is U+00AC NOT SIGN; ICU leaves it undefined
    "ibm852": {b"\xaa"},
    # vendor tables differ: undefined bytes (parqit: the C1 control, as WHATWG
    # does for 0x80-0x9F) and three Arabic presentation forms
    "ibm864": {bytes([b]) for b in (0x9B, 0x9C, 0x9F, 0xD7, 0xD8, 0xF1)},
    # undefined bytes (C1 controls), 0x88 (U+00B7 vs its canonical equivalent
    # U+0387) and 0xEF (U+0384 GREEK TONOS vs U+00B4)
    "ibm869": {bytes([b]) for b in (0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x87, 0x88, 0x93, 0x94, 0xEF)},
    # Microsoft's 932 reads the single bytes 0x7F, 0x80 and 0xA0 as U+007F,
    # U+0080 and U+F8F0; ICU substitutes (here after a half-width katakana, and
    # 0xA0 before any byte)
    "windows-932": {bytes([a, t]) for a in range(0xA1, 0xE0) for t in (0x7F, 0x80, 0xA0)} |
                   {bytes([0xA0, t]) for t in range(64, 253)},
    # ICU's EUC-JP is IBM's: extensions after 0x8E and in JIS X 0212 row 83
    "euc-jp": {b"\x8e\xe0", b"\x8e\xe1", b"\x8e\xe2"} |
              {bytes([0x8F, 0xF3, t]) for t in list(range(0xA1, 0xB5)) + [0xB7]},
}

fails = compared = errboth = allowed = 0
for tag, enc, kind in runs:
    ours = pq.read_table(f"{stem}_{tag}_ours.parquet").to_pydict()
    icu = pq.read_table(f"{stem}_{tag}_icu.parquet").to_pydict()
    byid = dict(zip(icu["id"], icu["icu"]))
    sq = seqs(tag, kind)
    if len(ours["id"]) != len(sq) or len(byid) != len(sq):
        fails += 1
        print(f"FAIL {tag}: {len(ours['id'])} rows written, {len(sq)} sequences")
        continue
    bad = 0
    for i, o in zip(ours["id"], ours["raw"]):
        x = byid[i]
        b = sq[i - 1]
        compared += 1
        # IBM's ICU tables cycle SUB/FS/DEL; Microsoft/Python/glibc keep ASCII.
        if enc.startswith("ibm") and b in (b"\x1a", b"\x1c", b"\x7f"):
            expected = b.decode("ascii")
            ibm = {b"\x1a": "\x1c", b"\x1c": "\x7f", b"\x7f": "\x1a"}[b]
            if o != expected or x not in (expected, ibm):
                bad += 1
                print(f"FAIL {enc} {b.hex()}: parqit {o!r}, ICU {x!r}, expected {expected!r}")
            continue
        # CP864.TXT: 0x25 is ARABIC PERCENT SIGN; this ICU variant keeps ASCII %.
        if enc == "ibm864" and b == b"%":
            if o != "٪" or x not in ("%", "٪"):
                bad += 1
                print(f"FAIL ibm864 25: parqit {o!r}, ICU {x!r}, expected U+066A")
            continue
        if o == x:
            continue
        if err(o) and err(x):
            errboth += 1
            continue
        if b in ALLOW.get(enc, ()):
            allowed += 1
            continue
        bad += 1
        if bad <= 8:
            print(f"FAIL {enc} {b.hex()}: parqit {[hex(ord(c)) for c in o]} ICU {[hex(ord(c)) for c in x]}")
    fails += bad
    print(f"{tag} ({enc}): {len(sq)} sequences, {bad} unexpected difference(s)")
print(f"{compared} sequences compared with ICU; {allowed} listed differences; "
      f"{errboth} rejected by both")
Macro.setLocal("pyfails", str(fails))
Macro.setLocal("compared", str(compared))
end
local fails = `fails' + `pyfails'
if (`compared' < 195000) {
    di as err "only `compared' sequences were compared"
    local ++fails
}
if (`fails' == 0) di "VERDICT(V136_ENCODING_ICU_ORACLE): PASS - `compared' byte sequences agree with ICU"
else di "VERDICT(V136_ENCODING_ICU_ORACLE): FAIL - `fails' check(s) failed"
