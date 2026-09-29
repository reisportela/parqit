* V139 — text in any language, UTF-8 throughout: variable names, values,
* variable/value/data labels, notes and characteristics in Cyrillic, Greek,
* Chinese, Japanese, Korean, Arabic, Hebrew, Devanagari, Georgian, Armenian,
* Ethiopic, Thai and Vietnamese, with emoji (ZWJ sequences, flags, skin
* tones), combining marks, right-to-left and zero-width marks, a BOM
* character, a line separator, characters beyond the BMP and a long strL. parqit
* save -> pyarrow must give the text byte for byte (no normalisation: é and
* e + U+0301 stay distinct); parqit use must give back the dataset (cf) and its
* metadata; and a lazy view must filter, measure (strlen bytes, ustrlen
* characters), sort, group and merge on such text exactly as native Stata does.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile stem
local fails 0

clear
input str80 город str80 πόλη str80 城市 str80 도시 str80 مدينة str80 עיר
"Москва" "Αθήνα" "北京" "서울" "القاهرة" "ירושלים"
"Санкт-Петербург" "Θεσσαλονίκη" "上海" "부산" "دمشق" "תל אביב"
"Ёлки" "Ψ" "𠀀𠀁" "똠방각하" "بغداد" "חיפה"
"Москва" "Αθήνα" "北京" "서울" "القاهرة" "ירושלים"
end
gen str80 शहर = "नमस्ते दुनिया" in 1
replace शहर = "मुंबई" in 2
replace शहर = "दिल्ली" in 3
replace शहर = "नमस्ते दुनिया" in 4
gen str80 ქალაქი = "თბილისი"
gen str80 քաղաք = "Երևան"
gen str80 ከተማ = "አዲስ አበባ"
gen str80 thai = "กรุงเทพมหานคร"
* combining marks and invisible characters from their code points (uchar();
* Stata's ustrunescape() re-encodes characters that are already UTF-8:
* ustrunescape("é") is C3 83 C2 A9)
foreach u in 769 803 770 8207 8206 8205 65279 8232 {
    mata: st_local("u`u'", uchar(`u'))
}
gen str80 viet = "Việt Nam" in 1
replace viet = "Vie`u803'`u770't Nam" in 2
replace viet = "Hà Nội" in 3
replace viet = "Việt Nam" in 4
gen str80 emoji = "👩‍👩‍👧 🇵🇹 👍🏽 😀"
replace emoji = "é" in 2
replace emoji = "e`u769'" in 3
replace emoji = "`u8207'שלום`u8206' `u8205' `u65279' `u8232' x" in 4
gen strL longtext = 1000 * "中文テキスト한국어"
gen byte grupo = mod(_n, 2) + 1
label define grupol 1 "Первый 第一" 2 "ثاني ✓"
label values grupo grupol
label variable город "Город (город)"
label variable 城市 "城市名称"
label variable مدينة "اسم المدينة"
label variable emoji "絵文字 😀"
label data "Многоязычные данные 多语言数据"
notes город: Заметка о городе
notes: 数据说明 🇯🇵
char 城市[来源] "国家统计局"
char _dta[נתונים] "מקור"
quietly save `"`stem'_native.dta"', replace

* ---------- save -> pyarrow ------------------------------------------------------
parqit save `"`stem'.parquet"', data replace
if (r(transcoded_cells) > 0 | r(transcoded_meta) > 0) {
    di as err "UTF-8 text was transcoded"
    local ++fails
}
global V139_STEM `"`stem'"'
python:
from sfi import Macro, Data
import json
import pyarrow.parquet as pq
stem = Macro.getGlobal("V139_STEM")
fails = 0
t = pq.read_table(stem + ".parquet")
names = [Data.getVarName(i) for i in range(Data.getVarCount())]
if t.schema.names != names:
    fails += 1
    print("FAIL names", t.schema.names, names)
for j, n in enumerate(names):
    if n == "grupo":
        continue
    want = [Data.getAt(j, i) for i in range(Data.getObsTotal())]
    got = t.column(n).to_pylist()
    if got != want:
        fails += 1
        print("FAIL values", n, got[:2], want[:2])
md = {k.decode(): json.loads(v.decode()) for k, v in t.schema.metadata.items()}
labs = {v["name"]: v.get("varlab", "") for v in md["parqit.schema"]["vars"]}
if (labs["город"] != "Город (город)" or labs["城市"] != "城市名称" or labs["مدينة"] != "اسم المدينة"
        or labs["emoji"] != "絵文字 😀" or md["parqit.dtalabel"] != "Многоязычные данные 多语言数据"
        or md["parqit.vallabs"]["grupol"]["entries"] != [["1", "Первый 第一"], ["2", "ثاني ✓"]]
        or md["parqit.chars"]["城市"]["来源"] != "国家统计局" or md["parqit.chars"]["_dta"]["נתונים"] != "מקור"
        or md["parqit.chars"]["город"]["note1"] != "Заметка о городе"
        or md["parqit.chars"]["_dta"]["note1"] != "数据说明 🇯🇵"):
    fails += 1
    print("FAIL metadata", labs, md)
nfc, nfd = t.column("emoji").to_pylist()[1:3]
if nfc == nfd or nfc != "é" or nfd != "é":
    fails += 1
    print("FAIL normalisation", repr(nfc), repr(nfd))
Macro.setLocal("pyfails", str(fails))
end
local fails = `fails' + `pyfails'

* ---------- parqit use gives the dataset back -------------------------------------------
parqit use `"`stem'.parquet"', clear
capture noisily cf _all using `"`stem'_native.dta"', all
if (_rc) local ++fails
local a : variable label город
local b : variable label emoji
local c : data label
local d : label grupol 2
local e : char 城市[来源]
local f : char _dta[נתונים]
local g : char город[note1]
if ("`a'" != "Город (город)" | "`b'" != "絵文字 😀" | "`c'" != "Многоязычные данные 多语言数据" | ///
    "`d'" != "ثاني ✓" | "`e'" != "国家统计局" | "`f'" != "מקור" | "`g'" != "Заметка о городе") {
    di as err "metadata after parqit use: `a' | `b' | `c' | `d' | `e' | `f' | `g'"
    local ++fails
}

* ---------- a lazy view against native Stata --------------------------------------------
use `"`stem'_native.dta"', clear
gen long bytes = strlen(城市)
gen long chars = ustrlen(城市)
keep if город == "Москва" | 城市 == "𠀀𠀁"
sort emoji viet
keep город 城市 emoji viet bytes chars
tempfile want
quietly save `"`want'"'
parqit use `"`stem'.parquet"', name(v)
parqit gen long bytes = strlen(城市)
parqit gen long chars = ustrlen(城市)
parqit keep if город == "Москва" | 城市 == "𠀀𠀁"
parqit sort emoji viet
parqit keep город 城市 emoji viet bytes chars
parqit collect, clear
capture noisily cf _all using `"`want'"', all
if (_rc) {
    di as err "filter / strlen / ustrlen / sort differ from native Stata"
    local ++fails
}
* groups: é and e + U+0301 are different text
use `"`stem'_native.dta"', clear
collapse (count) n = grupo, by(emoji)
sort emoji
quietly save `"`want'"', replace
parqit use `"`stem'.parquet"', name(v)
parqit collapse (count) n = grupo, by(emoji)
parqit sort emoji
parqit collect, clear
capture noisily cf _all using `"`want'"', all
if (_rc | _N != 4) {
    di as err "collapse by emoji: " _N " groups"
    local ++fails
}
* merge on text keys in two scripts
use `"`stem'_native.dta"', clear
keep in 1/3
keep город 城市
gen int k = _n
quietly parqit save `"`stem'_right.parquet"', data replace
tempfile right
quietly save `"`right'"'
use `"`stem'_native.dta"', clear
keep город 城市 viet
merge m:1 город 城市 using `"`right'"', keep(master match)
sort город 城市 viet
quietly save `"`want'"', replace
parqit use `"`stem'.parquet"', name(v)
parqit keep город 城市 viet
parqit merge m:1 город 城市 using `"`stem'_right.parquet"', keep(master match)
parqit collect, clear
sort город 城市 viet
capture noisily cf город 城市 viet k using `"`want'"', all
if (_rc) {
    di as err "merge on Cyrillic and Chinese keys differs from native Stata"
    local ++fails
}
parqit close _all

if (`fails' == 0) di "VERDICT(V139_MULTILINGUAL_ROUNDTRIP): PASS"
else di "VERDICT(V139_MULTILINGUAL_ROUNDTRIP): FAIL - `fails' check(s) failed"
