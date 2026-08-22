* CHARTER 6 + 8 — ENC-2 (2026-08-22, supersedes the 2026-06-16 refusal): a Stata
* string cell, variable/data label, value-label text, note or characteristic
* carrying legacy 8-bit bytes (Latin-1/Windows-1252 — common in administrative
* data saved by Stata 13 and earlier, or loaded without -unicode translate-)
* must NOT be written verbatim into a UTF-8-typed Parquet column (unreadable
* file), must NOT be refused, and must NOT crash the metadata serialiser
* ("internal error: invalid UTF-8 byte", the live finding). parqit save now
* transcodes every invalid item from the declared legacy code page
* (windows-1252 by default, encoding() to choose), keeps valid UTF-8 byte-exact,
* widens str# for the longer UTF-8 form, prints a loud note, and returns
* r(transcoded_cells/meta/vars/encoding). Both writers (Arrow, staged) are
* covered, the payload is confirmed by an independent pyarrow oracle, and a
* failed save never clobbers a good file.
* (python: blocks stay at top level — Stata's batch parser breaks on them
* inside foreach — so the two writer passes are unrolled around helper programs.)
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

* legacy-byte dataset: cells, varlab, value label, note, char, data label
program define _v32_build
    clear
    set obs 5
    gen int id = _n
    gen strL s = ""
    replace s = char(233) in 1                              // 0xE9 é
    replace s = "ok" + char(195) + "x" in 2                 // 0xC3 lead byte, no continuation -> Ã
    replace s = char(255) + char(254) in 3                  // ÿþ
    replace s = char(128) + char(147) + "q" + char(148) in 4 // cp1252 0x80-0x9F: € “q”
    replace s = "S" + char(227) + "o Jo" + char(227) + "o" in 5  // São João
    local lab = "Regi" + char(227) + "o"
    label var s "`lab'"
    local vt = "N" + char(237) + "vel 1"
    label define lbl 1 "`vt'"
    label values id lbl
    local nt = "Nota " + char(231)
    notes: `nt'
    local ch = "INE " + char(233)
    char _dta[fonte] "`ch'"
    local dl = "Dados " + char(227)
    label data "`dl'"
end

* asserts on the dataset read back from a transcoded save
program define _v32_check_loaded
    assert s[1] == "é" & s[2] == "okÃx" & s[3] == "ÿþ" & s[4] == "€“q”" & s[5] == "São João"
    assert `"`: var label s'"' == "Região"
    assert `"`: label lbl 1'"' == "Nível 1"
    assert `"`: char _dta[note1]'"' == "Nota ç"
    assert `"`: char _dta[fonte]'"' == "INE é"
    assert `"`: data label'"' == "Dados ã"
end

* ---------- A1. Arrow writer (default) ---------------------------------------
python:
import os
os.environ.pop("PARQIT_SAVE_NOARROW", None)
end
_v32_build
tempfile ta
local fa `"`ta'.parquet"'
parqit save `"`fa'"', replace
assert r(transcoded_cells) == 5
assert r(transcoded_meta) >= 5          // varlab, value-label text, note, char, data label
assert "`r(transcoded_vars)'" == "s"
assert "`r(encoding)'" == "windows-1252"
python:
from sfi import Macro
import pyarrow.parquet as pq, json
f = Macro.getLocal("fa")
t = pq.read_table(f)
vals = t.column("s").to_pylist()
ok = vals == ["é", "okÃx", "ÿþ", "€“q”", "São João"]
kv = t.schema.metadata
sch = json.loads(kv[b"parqit.schema"])
varlab = [v["varlab"] for v in sch["vars"] if v["name"] == "s"][0]
ok = ok and varlab == "Região"
vl = json.loads(kv[b"parqit.vallabs"])
ok = ok and vl["lbl"]["entries"][0][1] == "Nível 1"
ch = json.loads(kv[b"parqit.chars"])
ok = ok and ch["_dta"]["note1"] == "Nota ç" and ch["_dta"]["fonte"] == "INE é"
ok = ok and json.loads(kv[b"parqit.dtalabel"]) == "Dados ã"
Macro.setLocal("oracle_ok", "1" if ok else "0")
end
assert "`oracle_ok'" == "1"
parqit use using `"`fa'"', clear
_v32_check_loaded

* ---------- A2. staged writer ------------------------------------------------
python:
import os
os.environ["PARQIT_SAVE_NOARROW"] = "1"
end
_v32_build
tempfile ts
local fs `"`ts'.parquet"'
parqit save `"`fs'"', replace
assert r(transcoded_cells) == 5
assert r(transcoded_meta) >= 5
assert "`r(transcoded_vars)'" == "s"
python:
from sfi import Macro
import pyarrow.parquet as pq, json
t = pq.read_table(Macro.getLocal("fs"))
ok = t.column("s").to_pylist() == ["é", "okÃx", "ÿþ", "€“q”", "São João"]
sch = json.loads(t.schema.metadata[b"parqit.schema"])
ok = ok and [v["varlab"] for v in sch["vars"] if v["name"] == "s"][0] == "Região"
Macro.setLocal("oracle_ok", "1" if ok else "0")
import os
os.environ.pop("PARQIT_SAVE_NOARROW", None)
end
assert "`oracle_ok'" == "1"
parqit use using `"`fs'"', clear
_v32_check_loaded

* ---------- B. str# widens for the longer UTF-8 form (like unicode translate) -
clear
set obs 2
gen str5 w = "a" + char(227) + "b" + char(233) + "c" in 1    // 5 Latin-1 bytes -> 7 UTF-8 bytes
replace w = "plain" in 2
tempfile wb
local wf `"`wb'.parquet"'
parqit save `"`wf'"', replace
assert r(transcoded_cells) == 1
python:
from sfi import Macro
import pyarrow.parquet as pq, json
sch = json.loads(pq.read_schema(Macro.getLocal("wf")).metadata[b"parqit.schema"])
Macro.setLocal("wtype", [v["type"] for v in sch["vars"] if v["name"] == "w"][0])
end
assert "`wtype'" == "str7"
parqit use using `"`wf'"', clear
assert w[1] == "aãbéc" & w[2] == "plain"
assert "`: type w'" == "str7"

* ---------- C. encoding() selects the legacy code page; unknown is refused ----
clear
set obs 1
gen str4 e = char(128) + char(164) + char(142) + char(233)
tempfile eb
local e1 `"`eb'_1.parquet"'
parqit save `"`e1'"', replace encoding(latin1)
assert "`r(encoding)'" == "latin1"
local e9 `"`eb'_9.parquet"'
parqit save `"`e9'"', replace encoding(iso-8859-15)
assert "`r(encoding)'" == "latin9"
local em `"`eb'_m.parquet"'
parqit save `"`em'"', replace encoding(MacRoman)
assert "`r(encoding)'" == "macroman"
local ec `"`eb'_c.parquet"'
parqit save `"`ec'"', replace encoding(cp1252)
assert "`r(encoding)'" == "windows-1252"
python:
from sfi import Macro
import pyarrow.parquet as pq
def col(p): return pq.read_table(p).column("e").to_pylist()[0]
ok = (col(Macro.getLocal("e1")) == "¤é"     # latin1: identity (C1 controls kept)
  and col(Macro.getLocal("e9")) == "€é"     # latin9: 0xA4 -> €
  and col(Macro.getLocal("em")) == "Ä§éÈ"     # macroman: Ä § é È
  and col(Macro.getLocal("ec")) == "€¤Žé")    # cp1252: € ¤ Ž é
Macro.setLocal("enc_ok", "1" if ok else "0")
end
assert "`enc_ok'" == "1"
local ebad `"`eb'_bad.parquet"'
capture noisily parqit save `"`ebad'"', replace encoding(utf-16)
assert _rc == 198
capture confirm file `"`ebad'"'
assert _rc != 0                                  // refused before anything is written

* ---------- D. a failed save never clobbers a good file ---------------------
clear
set obs 2
gen int id = _n
gen str5 s = "good"
tempfile gb
local good `"`gb'.parquet"'
parqit save `"`good'"', replace
python:
from sfi import Macro
import hashlib
Macro.setLocal("md5_before",
    hashlib.md5(open(Macro.getLocal("good"), "rb").read()).hexdigest())
end
capture noisily parqit save `"`good'"', replace encoding(nonsense)
assert _rc != 0
python:
from sfi import Macro
import hashlib
Macro.setLocal("md5_after",
    hashlib.md5(open(Macro.getLocal("good"), "rb").read()).hexdigest())
end
assert "`md5_before'" == "`md5_after'"     // pre-existing file byte-identical
parqit use using `"`good'"'
parqit collect, clear
assert _N == 2 & s[1] == "good"

* ---------- E. valid UTF-8 untouched, nothing transcoded; pyarrow-confirmed --
parqit close                                 // export memory below, not the view
clear
set obs 4
gen int id = _n
gen strL s = ""
replace s = "ascii" in 1
replace s = "café"  in 2                    // accented, valid UTF-8 (c3 a9)
replace s = "a" + char(240)+char(159)+char(152)+char(128) + "b" in 3  // 😀 emoji
* s[4] stays "" (empty)
label var s "Região já em UTF-8"
tempfile vb
local vf `"`vb'.parquet"'
parqit save `"`vf'"', replace
assert r(transcoded_cells) == 0 & r(transcoded_meta) == 0
python:
from sfi import Macro
import pyarrow.parquet as pq
vals = pq.read_table(Macro.getLocal("vf")).column("s").to_pylist()
ok = (vals[0] == "ascii" and vals[1] == "café" and
      vals[2] == "a\U0001F600b" and (vals[3] == "" or vals[3] is None))
Macro.setLocal("oracle_ok", "1" if ok else "0")
end
assert "`oracle_ok'" == "1"
parqit use using `"`vf'"'
parqit collect, clear
assert s[1] == "ascii" & s[2] == "café"
assert `"`: var label s'"' == "Região já em UTF-8"

di "VERDICT(V32_INVALID_UTF8_SAVE): PASS - legacy 8-bit text transcoded on both " ///
   "writers (cells, labels, value labels, notes, chars, data label), str# widened, " ///
   "encoding() honoured, unknown encoding refused, no clobber, valid UTF-8 byte-exact " ///
   "(pyarrow oracle)"
