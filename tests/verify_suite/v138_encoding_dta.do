* V138 — ENC-3 on the Stata side: legacy text in a .dta and in memory, beyond
* the Western code pages. A dataset holds the raw bytes of Chinese (GBK) text
* in its values, variable and value labels, notes, characteristics and data
* label — one column holds only 女 (C5 AE), which is also valid UTF-8 (Ů). Read
* with encoding(gbk): a column with any text that is not UTF-8 is decoded as a
* whole (女 too), a column that is all valid UTF-8 is kept (Ů) unless
* encoding(gbk, all) — and in a .dta of format 117 or older (Stata 13, which
* predates Unicode) all of it is decoded without asking. The same through
* parqit save from memory, checked by pyarrow; Russian, Japanese and Greek
* text with encoding(); parqit set encoding; the locale hint (set
* locale_functions); the locale -> code page map; and texts longer than Stata
* allows cut at a character boundary, never inside one.
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile stem
local fails 0

mata:
string scalar _v138_b(string scalar hex)
{
    string scalar s
    real scalar i

    s = ""
    for (i = 1; i <= strlen(hex); i = i + 2) s = s + char(frombase(16, substr(hex, i, 2)))
    return(s)
}
end

* GBK bytes of the text: 北京 上海 女 城市 男 注释 说明 中国数据
foreach w in beijing:B1B1BEA9 shanghai:C9CFBAA3 nv:C5AE chengshi:B3C7CAD0 nan:C4D0 ///
    zhushi:D7A2CACD shuoming:CBB5C3F7 zhongguo:D6D0B9FACAFDBEDD {
    gettoken k h : w, parse(":")
    mata: st_local("`k'", _v138_b(substr("`h'", 2, .)))
}
program define _v138_build
    args beijing shanghai nv chengshi nan zhushi shuoming zhongguo
    clear
    set obs 3
    gen long id = _n
    gen str8 zh = "`beijing'" in 1
    replace zh = "`nv'" in 2
    replace zh = "`shanghai'" in 3
    gen str8 onlynv = "`nv'"
    gen byte sex = cond(_n == 2, 2, 1)
    label define sexl 1 "`nan'" 2 "`nv'"
    label values sex sexl
    label variable zh "`chengshi'"
    notes zh: `zhushi'
    char zh[about] "`shuoming'"
    label data "`zhongguo'"
end
_v138_build "`beijing'" "`shanghai'" "`nv'" "`chengshi'" "`nan'" "`zhushi'" "`shuoming'" "`zhongguo'"
quietly save `"`stem'_118.dta"', replace
quietly saveold `"`stem'_117.dta"', version(13) replace

program define _v138_check
    args label expect_onlynv
    local f 0
    if (zh[1] != "北京" | zh[2] != "女" | zh[3] != "上海") {
        di as err "`label': zh = " zh[1] " " zh[2] " " zh[3]
        local ++f
    }
    if (onlynv[1] != "`expect_onlynv'") {
        di as err "`label': onlynv = " onlynv[1] ", not `expect_onlynv'"
        local ++f
    }
    local vl : variable label zh
    local s1 : label sexl 1
    local s2 : label sexl 2
    local dl : data label
    local ch : char zh[about]
    local nt : char zh[note1]
    if ("`vl'" != "城市" | "`s1'" != "男" | "`s2'" != "女" | "`dl'" != "中国数据" | ///
        "`ch'" != "说明" | "`nt'" != "注释") {
        di as err "`label': metadata `vl' `s1' `s2' `dl' `ch' `nt'"
        local ++f
    }
    c_local f `f'
end

* ---------- a .dta of format 118: the column rule --------------------------------
parqit use `"`stem'_118.dta"', clear encoding(gbk)
local revalid = r(transcoded_revalid)
local rvars "`r(transcoded_revalid_vars)'"
_v138_check "118" "Ů"
local fails = `fails' + `f'
if (`revalid' != 1 | "`rvars'" != "zh") {
    di as err "118: r(transcoded_revalid) `revalid' (`rvars')"
    local ++fails
}
parqit use `"`stem'_118.dta"', clear encoding(gbk, all)
_v138_check "118 all" "女"
local fails = `fails' + `f'

* ---------- a .dta of format 117: all of it, without asking ------------------------
parqit use `"`stem'_117.dta"', clear encoding(gbk)
_v138_check "117" "女"
local fails = `fails' + `f'
* merge with a lookup .dta of format 117 takes the same rule
clear
set obs 3
gen long id = _n
quietly parqit save `"`stem'_master.parquet"', data replace
parqit use `"`stem'_master.parquet"', name(m)
parqit merge 1:1 id using `"`stem'_117.dta"', encoding(gbk)
parqit collect, clear
if (onlynv[1] != "女") {
    di as err "merge with a 117 lookup: " onlynv[1]
    local ++fails
}
parqit close _all

* ---------- parqit save from memory, read by pyarrow --------------------------------
use `"`stem'_118.dta"', clear
parqit save `"`stem'_gbk.parquet"', data replace encoding(gbk)
if ("`r(encoding)'" != "windows-936" | r(encoding_default) != 0) {
    di as err "save encoding(gbk): r(encoding) `r(encoding)'"
    local ++fails
}
use `"`stem'_118.dta"', clear
parqit save `"`stem'_gbk_all.parquet"', data replace encoding(gbk, all)
global V138_STEM `"`stem'"'
python:
from sfi import Macro
import json
import pyarrow.parquet as pq
stem = Macro.getGlobal("V138_STEM")
fails = 0
for name, onlynv in (("gbk", "Ů"), ("gbk_all", "女")):
    t = pq.read_table(f"{stem}_{name}.parquet")
    d = t.to_pydict()
    md = {k.decode(): json.loads(v.decode()) for k, v in t.schema.metadata.items()}
    want = (d["zh"] == ["北京", "女", "上海"] and d["onlynv"] == [onlynv] * 3
            and md["parqit.vallabs"]["sexl"]["entries"] == [["1", "男"], ["2", "女"]]
            and [v["varlab"] for v in md["parqit.schema"]["vars"] if v["name"] == "zh"] == ["城市"]
            and md["parqit.chars"]["zh"]["about"] == "说明" and md["parqit.chars"]["zh"]["note1"] == "注释"
            and md["parqit.dtalabel"] == "中国数据")
    if not want:
        fails += 1
        print("FAIL", name, d, md)
Macro.setLocal("pyfails", str(fails))
end
local fails = `fails' + `pyfails'

* ---------- other scripts, the session default, the locale hint ---------------------
mata: st_local("moskva", _v138_b("CCEEF1EAE2E0"))
mata: st_local("tokyo", _v138_b("938C8B9E"))
mata: st_local("athina", _v138_b("C1E8DEEDE1"))
clear
set obs 1
gen str12 ru = "`moskva'"
gen str12 ja = "`tokyo'"
gen str12 el = "`athina'"
quietly save `"`stem'_mixed.dta"', replace
foreach p in ru:windows-1251:Москва ja:shift_jis:東京 el:windows-1253:Αθήνα {
    gettoken v rest : p, parse(":")
    gettoken enc want : rest, parse(":")
    gettoken enc want : want, parse(":")
    local want = substr("`want'", 2, .)
    parqit use `v' using `"`stem'_mixed.dta"', clear encoding(`enc')
    if (`v'[1] != "`want'") {
        di as err "`v' with encoding(`enc'): " `v'[1]
        local ++fails
    }
}
parqit set encoding windows-1251
use `"`stem'_mixed.dta"', clear
keep ru
parqit save `"`stem'_ru.parquet"', data replace
local denc "`r(encoding)'"
local deflt = r(encoding_default)
parqit set encoding windows-1252
parqit use `"`stem'_ru.parquet"', clear
if (ru[1] != "Москва" | "`denc'" != "windows-1251" | `deflt' != 1) {
    di as err "parqit set encoding: " ru[1] " `denc' `deflt'"
    local ++fails
}
set locale_functions ja_JP
capture erase v138_hint.log
log using v138_hint.log, text replace name(v138h)
use `"`stem'_mixed.dta"', clear
keep ja
parqit save `"`stem'_ja.parquet"', data replace
log close v138h
set locale_functions default
python:
from sfi import Macro
txt = open("v138_hint.log", encoding="utf-8", errors="replace").read().replace("\n> ", "")
ok = "windows-1252 is the default" in txt and "encoding(windows-932)" in txt
Macro.setLocal("okhint", "1" if ok else "0")
end
if ("`okhint'" != "1") {
    di as err "no locale hint for ja_JP"
    local ++fails
}

* ---------- the locale -> code page map ---------------------------------------------
* (Mata in an ado-file is private to it: the function is taken from the shipped
* parqit.ado as it is and compiled here)
global V138_REPO `"`repo'"'
python:
from sfi import Macro
src = open(Macro.getGlobal("V138_REPO") + "/src/ado/p/parqit.ado", encoding="utf-8").read()
start = src.index("string scalar _parqit_locale_codepage(")
body = src[start:src.index("\n}\n", start) + 3]
with open("v138_locale.do", "w", encoding="utf-8") as f:
    f.write("mata:\n" + body + "end\n")
end
run v138_locale.do
foreach p in pt_PT: en_US: ru_RU:windows-1251 uk_UA:windows-1251 sr_RS:windows-1251 ///
    sr_Latn_RS:windows-1250 bs_BA:windows-1250 pl_PL:windows-1250 cs_CZ:windows-1250 ///
    el_GR:windows-1253 tr_TR:windows-1254 az_AZ:windows-1254 az_Cyrl_AZ:windows-1251 ///
    he_IL:windows-1255 ar_EG:windows-1256 fa_IR:windows-1256 lt_LT:windows-1257 ///
    vi_VN:windows-1258 th_TH:windows-874 ja_JP:windows-932 ko_KR:windows-949 ///
    zh_CN:windows-936 zh_Hans_SG:windows-936 zh_TW:windows-950 zh_Hant_TW:windows-950 ///
    zh_hant_hk:windows-950 zh-Hant-MO:windows-950 hi_IN: ka_GE: {
    gettoken loc cp : p, parse(":")
    local cp = substr("`cp'", 2, .)
    mata: st_local("got", _parqit_locale_codepage("`loc'"))
    if ("`got'" != "`cp'") {
        di as err "locale `loc': `got', not `cp'"
        local ++fails
    }
}

* ---------- texts longer than Stata allows: cut at a character boundary --------------
clear
set obs 2
gen byte g = _n
label define gl 1 "a" 2 "b"
label values g gl
char g[about] "x"
quietly parqit save `"`stem'_long.parquet"', data replace
python:
from sfi import Macro
import json
import pyarrow.parquet as pq
stem = Macro.getGlobal("V138_STEM")
t = pq.read_table(stem + "_long.parquet")
md = dict(t.schema.metadata)
vl = json.loads(md[b"parqit.vallabs"])
vl["gl"]["entries"] = [["1", "字" * 14000], ["2", "é" * 16001 + "😀" * 3]]
md[b"parqit.vallabs"] = json.dumps(vl, ensure_ascii=False).encode()
ch = json.loads(md[b"parqit.chars"])
ch["g"]["about"] = "字" * 23000
md[b"parqit.chars"] = json.dumps(ch, ensure_ascii=False).encode()
pq.write_table(t.replace_schema_metadata(md), stem + "_long.parquet")
end
parqit use `"`stem'_long.parquet"', clear
mata: _v138_l1 = st_vlmap("gl", 1); _v138_l2 = st_vlmap("gl", 2); _v138_c = st_global("g[about]")
mata: st_local("okcut", strofreal(strlen(_v138_l1) == 31998 & _v138_l1 == 10666 * "字" & ///
    strlen(_v138_l2) == 32000 & _v138_l2 == 16000 * "é" & ustrinvalidcnt(_v138_l1 + _v138_l2 + _v138_c) == 0 & ///
    strlen(_v138_c) <= 67783 & _v138_c == (strlen(_v138_c) / 3) * "字"))
if ("`okcut'" != "1") {
    di as err "the long texts were not cut at a character boundary"
    local ++fails
}

* ---------- Excel: Stata decodes it, so encoding(name, all) is not applied -----------
clear
set obs 2
gen str20 city = "北京" in 1
replace city = "女" in 2
quietly export excel using `"`stem'.xlsx"', firstrow(variables) replace
parqit use `"`stem'.xlsx"', clear encoding(gbk, all)
if (city[1] != "北京" | city[2] != "女") {
    di as err "Excel with encoding(gbk, all): " city[1] " " city[2]
    local ++fails
}

if (`fails' == 0) di "VERDICT(V138_ENCODING_DTA): PASS"
else di "VERDICT(V138_ENCODING_DTA): FAIL - `fails' check(s) failed"
