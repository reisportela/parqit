* V137 — CSV-ENC-1: delimited text in any encoding. Delimited text files in
* 22 encodings (Polish, Russian, Greek, Turkish, Hebrew, Arabic, Lithuanian,
* Vietnamese, Thai, Ukrainian, DOS Russian, Japanese in Shift_JIS and EUC-JP,
* Chinese in GBK, GB18030 and Big5, Korean, UTF-16 with and without a
* byte-order mark, UTF-8 with a mark and with a broken byte) are read with
* encoding() — into Stata (then saved) and as a lazy view (saved by the
* engine, never through Stata) — and pyarrow on the Parquet files must give
* the Unicode text the files were written from. Also: a 9 MB windows-1251 file
* across several 4 MiB chunks; a UTF-16 file whose surrogate pair is cut by
* the first chunk boundary; GBK text that is valid UTF-8 by accident; UTF-32
* refused; encoding() contradicting the UTF-16 NUL pattern refused; undeclared
* legacy text scanned in place refused with parqit's encoding() advice;
* lookups (merge, joinby, append, mergein, appendin) with and without
* encoding(), the session default (parqit set encoding) and the locale hint.
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile stem
local fails 0
python script `"`repo'/tests/fixtures/encoding/make_text_fixtures.py"', args(`"`stem'"')

* ---------- each file, eager and lazy ----------------------------------------
local list pl_1250:windows-1250 ru_1251:cp1251 el_1253:windows-1253 tr_1254:windows-1254 ///
    he_1255:windows-1255 ar_1256:windows-1256 lt_1257:windows-1257 vi_1258:windows-1258 ///
    th_874:windows-874 uk_koi8u:koi8-u ru_866:ibm866 ja_sjis:shift_jis ja_eucjp:euc-jp ///
    zh_gbk:gbk zh_gb18030:gb18030 tw_big5:big5 ko_949:euc-kr u16le_bom: u16be_bom: ///
    u16le_nobom: u8_bom: u8_bad:utf-8 zh_gbk_valid:gbk,all big_1251:cp1251 big_u16:
local done ""
capture erase v137_notes.log
log using v137_notes.log, text replace name(v137)
foreach p of local list {
    gettoken name enc : p, parse(":")
    local enc = subinstr(substr("`enc'", 2, .), ",", ", ", .)
    local encopt ""
    if ("`enc'" != "") local encopt `"encoding(`enc')"'
    capture noisily parqit use `"`stem'_`name'.csv"', clear `encopt'
    if (_rc) {
        di as err "`name': parqit use failed, rc " _rc
        local ++fails
        continue
    }
    local r_enc_`name' "`r(encoding)'"
    local r_lines_`name' = cond(r(transcoded_lines) < ., r(transcoded_lines), 0)
    local r_bad_`name' = cond(r(undecodable) < ., r(undecodable), 0)
    quietly parqit save `"`stem'_`name'_eager.parquet"', data replace
    capture noisily {
        parqit use `"`stem'_`name'.csv"', `encopt' name(v137)
        parqit save `"`stem'_`name'_lazy.parquet"', replace
        parqit close v137
    }
    if (_rc) {
        di as err "`name': the lazy view failed, rc " _rc
        local ++fails
        continue
    }
    local done "`done' `name'"
}
* the GBK file that is valid UTF-8 throughout, without all: read as UTF-8
parqit use `"`stem'_zh_gbk_valid.csv"', clear encoding(gbk)
quietly parqit save `"`stem'_zh_gbk_valid_as_utf8_eager.parquet"', data replace
log close v137
if ("`r_bad_u8_bad'" != "1" | "`r_enc_u8_bad'" != "utf-8") {
    di as err "u8_bad: r(undecodable) `r_bad_u8_bad', r(encoding) `r_enc_u8_bad'"
    local ++fails
}
if ("`r_enc_ru_1251'" != "windows-1251" | "`r_lines_ru_1251'" != "2") {
    di as err "ru_1251: r(encoding) `r_enc_ru_1251', r(transcoded_lines) `r_lines_ru_1251'"
    local ++fails
}
if ("`r_enc_u16le_nobom'" != "utf-16le" | "`r_enc_u16be_bom'" != "utf-16be") {
    di as err "UTF-16: r(encoding) `r_enc_u16le_nobom' / `r_enc_u16be_bom'"
    local ++fails
}
global V137_STEM `"`stem'"'
global V137_DONE "`done'"

python:
from sfi import Macro
import json
import pyarrow.parquet as pq

stem = Macro.getGlobal("V137_STEM")
done = Macro.getGlobal("V137_DONE").split()
truth = json.load(open(stem + "_truth.json", encoding="utf-8"))
fails = 0
def bad(msg):
    global fails
    fails += 1
    print("FAIL:", msg)

def rows_of(path):
    t = pq.read_table(path).to_pydict()
    return [[int(i), n, p] for i, n, p in zip(t["id"], t["name"], t["place"])]

for name in done + ["zh_gbk_valid_as_utf8"]:
    for how in ("eager", "lazy"):
        if name == "zh_gbk_valid_as_utf8" and how == "lazy":
            continue
        got = rows_of(f"{stem}_{name}_{how}.parquet")
        want = truth[name]
        if isinstance(want, dict):
            if len(got) != want["n"]:
                bad(f"{name} {how}: {len(got)} rows, not {want['n']}")
            if sum(len(r[1]) + len(r[2]) for r in got) != want["chars"]:
                bad(f"{name} {how}: the text differs in length")
            probe = want.get("last") or want.get("straddle")
            if probe not in got:
                bad(f"{name} {how}: row {probe[0]} is not {probe[1][-12:]!r}, {probe[2]!r}")
        elif got != want:
            bad(f"{name} {how}: {got!r} != {want!r}")

notes = open("v137_notes.log", encoding="utf-8", errors="replace").read().replace("\n> ", "")
for phrase in ("has no byte-order mark but a NUL byte in every other byte",
               "is UTF-16LE text (by its byte-order mark)",
               "is valid UTF-8 throughout, so it was read as UTF-8, not decoded from windows-936",
               "held bytes that are not UTF-8",
               "line(s) were decoded from windows-1251 to UTF-8"):
    if phrase not in notes:
        bad(f"no note: {phrase}")
Macro.setLocal("pyfails", str(fails))
end
local fails = `fails' + `pyfails'

* ---------- what is refused, loudly --------------------------------------------
capture parqit use `"`stem'_u32.csv"', clear
if (_rc != 610) {
    di as err "UTF-32: rc " _rc ", not 610"
    local ++fails
}
* an encoding() given is followed over the UTF-16 pattern, and says so
capture erase v137_over.log
log using v137_over.log, text replace name(v137o)
capture noisily parqit use `"`stem'_u16le_nobom.csv"', clear encoding(latin1)
local rc_over = _rc
log close v137o
python:
from sfi import Macro
txt = open("v137_over.log", encoding="utf-8", errors="replace").read().replace("\n> ", "")
Macro.setLocal("okover", "1" if "looks like UTF-16LE text without a byte-order mark, but it was read as latin1" in txt else "0")
end
* (the text then read as latin1 holds NUL bytes, which the engine may refuse
* to parse: loudly, after the note that names encoding(utf-16le))
if (`rc_over' == 198 | "`okover'" != "1") {
    di as err "latin1 over UTF-16 without a mark: rc `rc_over', note `okover'"
    local ++fails
}
capture parqit use `"`stem'_ru_1251.csv"', clear encoding(klingon)
if (_rc != 198) {
    di as err "encoding(klingon): rc " _rc
    local ++fails
}
capture erase v137_refuse.log
log using v137_refuse.log, text replace name(v137r)
capture noisily parqit use `"`stem'_ru_1251.csv"', clear
local rc_undeclared = _rc
log close v137r
python:
from sfi import Macro
txt = open("v137_refuse.log", encoding="utf-8", errors="replace").read().replace("\n> ", "")
ok = "parqit's encoding() option" in txt and "encoding='UTF-16'" not in txt
Macro.setLocal("okref", "1" if ok else "0")
end
if (`rc_undeclared' == 0 | "`okref'" != "1") {
    di as err "undeclared windows-1251 scanned in place: rc `rc_undeclared', advice `okref'"
    local ++fails
}

* ---------- a glob: decoded file by file; without encoding() read as before ----------
global V137_GLOB `"`stem'_glob"'
python:
from sfi import Macro
import os
d = Macro.getGlobal("V137_GLOB")
os.makedirs(d, exist_ok=True)
open(d + "/a1.csv", "wb").write("id,place\n1,Москва\n".encode("cp1251"))
open(d + "/a2.csv", "wb").write("id,place\n2,Омск\n3,Москва\n".encode("cp1251"))
open(d + "/u1.csv", "wb").write("id,place\n1,Zürich\n".encode("utf-8"))
open(d + "/u2.csv", "wb").write("id,place\n2,東京\n".encode("utf-8"))
end
parqit use `"${V137_GLOB}/a*.csv"', clear encoding(cp1251)
quietly count if place == "Москва"
if (_N != 3 | r(N) != 2) {
    di as err "glob with encoding(): " _N " rows, " r(N) " Москва"
    local ++fails
}
parqit use `"${V137_GLOB}/u*.csv"', clear
sort id
if (_N != 2 | place[2] != "東京" | `"`r(bridge)'"' != "") {
    di as err "UTF-8 glob: " _N " rows, " place[2]
    local ++fails
}
capture parqit use `"${V137_GLOB}/missing.csv"', clear
if (_rc == 0 | _rc == 610) {
    di as err "a missing file: rc " _rc
    local ++fails
}

* a byte-order mark wins over a contradicting encoding(), and says so
capture erase v137_bom.log
log using v137_bom.log, text replace name(v137b)
parqit use `"`stem'_u8_bom.csv"', clear encoding(windows-1251)
log close v137b
python:
from sfi import Macro
txt = open("v137_bom.log", encoding="utf-8", errors="replace").read().replace("\n> ", "")
Macro.setLocal("okbom", "1" if "starts with a UTF-8 byte-order mark, so it was read as UTF-8, not windows-1251" in txt else "0")
end
if ("`okbom'" != "1" | name[1] != "Москва") {
    di as err "UTF-8 mark over encoding(windows-1251): note `okbom', " name[1]
    local ++fails
}

* ---------- the audit's cases (F1-F7) ----------------------------------------------
* F1: Hive partitions survive decoding, in a glob and in a single file's path
parqit use `"`stem'_hive/*/*.csv"', clear encoding(cp1251)
capture confirm variable year
local hive_ok = !_rc & _N == 2
if (`hive_ok') {
    sort year
    local hive_ok = year[1] == 2020 & place[1] == "Москва" & place[2] == "Омск"
}
parqit use `"`stem'_hive/**/*.csv"', clear encoding(cp1251)
capture confirm variable year
if (_rc | _N != 2) local hive_ok 0
parqit use `"`stem'_hive/year=2021/part.csv"', clear encoding(cp1251)
capture confirm variable year
if (_rc) local hive_ok 0
else if (year[1] != 2021 | place[1] != "Омск") local hive_ok 0
parqit use `"`stem'_hive/*/*.csv"', encoding(cp1251) name(hv)
parqit collect, clear
capture confirm variable year
if (_rc) local hive_ok 0
parqit close hv
if (!`hive_ok') {
    di as err "F1: the Hive partition column did not survive decoding"
    local ++fails
}
* F2: UTF-8 with NUL-padded fields reads as before, declared or not; UTF-32
* without a mark is refused unless encoding() says otherwise
foreach opt in "" "encoding(utf-8)" {
    capture noisily parqit use `"`stem'_nul_padded.csv"', clear `opt'
    if (_rc | _N != 3) {
        di as err "F2: NUL-padded UTF-8 with [`opt']: rc " _rc
        local ++fails
    }
    else if (name[3] != "Zürich") {
        di as err "F2: NUL-padded UTF-8 with [`opt']: " name[3]
        local ++fails
    }
}
capture noisily parqit use `"`stem'_u32_nobom.csv"', clear
if (_rc != 610) {
    di as err "F2: UTF-32 without a mark: rc " _rc
    local ++fails
}
* F4: in a glob, the note names the byte-order mark of the file that has it
capture erase v137_mixed.log
log using v137_mixed.log, text replace name(v137m)
parqit use `"`stem'_mixed/*.csv"', clear encoding(cp1251)
log close v137m
sort id
python:
from sfi import Macro
txt = open("v137_mixed.log", encoding="utf-8", errors="replace").read().replace("\n> ", "")
ok = ("b.csv starts with a UTF-16LE byte-order mark, so it was read as UTF-16LE, not windows-1251" in txt
      and "WINDOWS-1251 byte-order mark" not in txt)
Macro.setLocal("okmixed", "1" if ok else "0")
end
if ("`okmixed'" != "1" | place[1] != "Москва" | place[2] != "東京") {
    di as err "F4: mixed glob note `okmixed', " place[1] " " place[2]
    local ++fails
}
* F5: a UTF-8 line decoded with a GBK file is counted and said
capture erase v137_rv.log
log using v137_rv.log, text replace name(v137r5)
parqit use `"`stem'_mixed_gbk.csv"', clear encoding(gbk)
local rvlines = r(transcoded_revalid_lines)
log close v137r5
python:
from sfi import Macro
txt = open("v137_rv.log", encoding="utf-8", errors="replace").read().replace("\n> ", "")
Macro.setLocal("okrv", "1" if "1 of the decoded lines were valid UTF-8" in txt else "0")
end
if ("`okrv'" != "1" | "`rvlines'" != "1" | place[2] != "女") {
    di as err "F5: revalid note `okrv', r(transcoded_revalid_lines) `rvlines', " place[2]
    local ++fails
}
* F6: classic Mac line ends
parqit use `"`stem'_cr_1251.csv"', clear encoding(cp1251)
if (_N != 2 | place[1] != "Москва" | place[2] != "Омск") {
    di as err "F6: CR-only file: " _N " rows"
    local ++fails
}
* F7: UTF-16LE Chinese without a mark, found by its line ends
parqit use `"`stem'_u16le_cjk_nobom.csv"', clear
if (_N != 2 | substr(a[1], 1, 6) != "北京" | substr(b[2], 1, 6) != "广州") {
    di as err "F7: UTF-16LE Chinese without a mark: " _N " rows"
    local ++fails
}

* ---------- the re-audit's cases ---------------------------------------------------
* F18: a link followed by .. is resolved by the system, decoded or not
python:
from sfi import Macro
import json
t = json.load(open(Macro.getGlobal("V137_STEM") + "_truth.json", encoding="utf-8"))
Macro.setLocal("has_symlink", str(t.get("symlink", 0)))
end
if ("`has_symlink'" == "1") {
    parqit use `"`stem'_symtest/sym/lnk/../hv2u/*.csv"', clear
    local inplace = place[1]
    parqit use `"`stem'_symtest/sym/lnk/../hv2/*.csv"', clear encoding(cp1251)
    if ("`inplace'" != "Москва" | place[1] != "Москва") {
        di as err "F18: link/.. read `inplace' in place and " place[1] " decoded"
        local ++fails
    }
}
else di as txt "(no symbolic links here: F18 not checked)"
* F22: a UTF-8 file read with a single-byte code page says so truthfully
capture erase v137_f22.log
log using v137_f22.log, text replace name(v137f22)
parqit use `"`stem'_u8_plain.csv"', clear encoding(cp1251)
log close v137f22
python:
from sfi import Macro
txt = open("v137_f22.log", encoding="utf-8", errors="replace").read().replace("\n> ", "")
ok = "is valid UTF-8 throughout, so it was read as UTF-8, not decoded from windows-1251" in txt and "0 line(s) were decoded" not in txt
Macro.setLocal("okf22", "1" if ok else "0")
end
if ("`okf22'" != "1" | name[1] != "Zürich") {
    di as err "F22: note `okf22', " name[1]
    local ++fails
}
* F23: the other UTF-16 byte order, followed and said
capture erase v137_f23.log
log using v137_f23.log, text replace name(v137f23)
capture noisily parqit use `"`stem'_u16le_nobom.csv"', clear encoding(utf-16be)
log close v137f23
python:
from sfi import Macro
txt = open("v137_f23.log", encoding="utf-8", errors="replace").read().replace("\n> ", "")
Macro.setLocal("okf23", "1" if "looks like UTF-16LE text without a byte-order mark, but it was read as utf-16be" in txt else "0")
end
if ("`okf23'" != "1") {
    di as err "F23: no note for utf-16be over a UTF-16LE pattern"
    local ++fails
}
* F17: lookup is not an option of parqit use
capture parqit use `"`stem'_u8_plain.csv"', clear lookup
if (_rc != 198) {
    di as err "F17: parqit use ..., lookup: rc " _rc
    local ++fails
}
* F15: random bytes named .csv are not taken for UTF-16 text
capture parqit use `"`stem'_random_bin.csv"', clear
if (_rc == 0) {
    di as err "F15: a random binary loaded as delimited text"
    local ++fails
}

* ---------- lookups ---------------------------------------------------------------
clear
set obs 2
gen long id = _n
tempfile master
quietly parqit save `"`master'.parquet"', data replace
* merge with encoding(); without it the session default, said, with the locale hint
parqit use `"`master'.parquet"', name(m)
parqit merge 1:1 id using `"`stem'_lk_1251.csv"', encoding(cp1251)
parqit collect, clear
if (place[1] != "Москва" | place[2] != "Омск") {
    di as err "merge with encoding(cp1251): " place[1]
    local ++fails
}
set locale_functions ru_RU
capture erase v137_hint.log
log using v137_hint.log, text replace name(v137h)
parqit use `"`master'.parquet"', name(m)
parqit merge 1:1 id using `"`stem'_lk_1251.csv"'
local deflt = r(encoding_default)
local denc "`r(encoding)'"
log close v137h
set locale_functions default
parqit collect, clear
python:
from sfi import Macro
txt = open("v137_hint.log", encoding="utf-8", errors="replace").read().replace("\n> ", "")
ok = ("declares no encoding and is not UTF-8" in txt and "the default" in txt
      and "encoding(windows-1251)" in txt and "parqit set encoding windows-1251" in txt)
Macro.setLocal("okhint", "1" if ok else "0")
end
if ("`okhint'" != "1" | "`deflt'" != "1" | "`denc'" != "windows-1252" | place[1] != "Ìîñêâà") {
    di as err "undeclared lookup: hint `okhint', default `deflt', encoding `denc', " place[1]
    local ++fails
}
* the Western default is unchanged
parqit use `"`master'.parquet"', name(m)
parqit merge 1:1 id using `"`stem'_lk_1252.csv"'
parqit collect, clear
if (place[1] != "São João" | place[2] != "Açores") {
    di as err "windows-1252 lookup: " place[1]
    local ++fails
}
* the session default
parqit set encoding windows-1251
parqit use `"`master'.parquet"', name(m)
parqit merge 1:1 id using `"`stem'_lk_1251.csv"'
parqit collect, clear
parqit set encoding windows-1252
if (place[1] != "Москва") {
    di as err "parqit set encoding windows-1251: " place[1]
    local ++fails
}
* joinby and append
parqit use `"`master'.parquet"', name(m)
parqit joinby id using `"`stem'_lk_1251.csv"', encoding(windows-1251)
parqit collect, clear
sort id
if (place[2] != "Омск") {
    di as err "joinby: " place[2]
    local ++fails
}
parqit use `"`stem'_lk_1251.csv"', name(a) encoding(cp1251)
parqit append using `"`stem'_lk_1251.csv"', encoding(cp1251)
parqit collect, clear
quietly count if place == "Москва"
if (r(N) != 2) {
    di as err "append: " r(N) " rows of Москва"
    local ++fails
}
parqit close _all
* mergein / appendin
clear
set obs 2
gen long id = _n
parqit mergein 1:1 id using `"`stem'_lk_1251.csv"', encoding(cp1251) nogenerate
if (place[1] != "Москва") {
    di as err "mergein: " place[1]
    local ++fails
}
* F3: without encoding(), a mergein/appendin lookup is decoded like a merge's
drop place
parqit mergein 1:1 id using `"`stem'_lk_1251.csv"', nogenerate
if (place[1] != "Ìîñêâà") {
    di as err "F3: mergein without encoding(): " place[1]
    local ++fails
}
drop place
parqit set encoding windows-1251
parqit mergein 1:1 id using `"`stem'_lk_1251.csv"', nogenerate
parqit set encoding windows-1252
if (place[1] != "Москва") {
    di as err "F3: mergein with parqit set encoding: " place[1]
    local ++fails
}
parqit appendin using `"`stem'_lk_1251.csv"', encoding(cp1251)
quietly count if place == "Омск"
if (r(N) != 2) {
    di as err "appendin: " r(N)
    local ++fails
}
* F14: a glob as the disk side of mergein/appendin, with and without encoding()
clear
set obs 2
gen long id = _n
parqit mergein 1:1 id using `"`stem'_lk_glob/*.csv"', encoding(cp1251) nogenerate
if (place[1] != "Москва" | place[2] != "Омск") {
    di as err "F14: mergein on a glob with encoding(): " place[1] " " place[2]
    local ++fails
}
drop place
parqit mergein 1:1 id using `"`stem'_lk_glob/*.csv"', nogenerate
if (place[1] != "Ìîñêâà") {
    di as err "F14: mergein on a glob without encoding(): " place[1]
    local ++fails
}
parqit appendin using `"`stem'_lk_glob/*.csv"', encoding(cp1251)
quietly count if place == "Омск"
if (r(N) != 1 | _N != 4) {
    di as err "F14: appendin on a glob: " _N " rows"
    local ++fails
}

if (`fails' == 0) di "VERDICT(V137_ENCODING_CSV): PASS"
else di "VERDICT(V137_ENCODING_CSV): FAIL - `fails' check(s) failed"
