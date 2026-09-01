* V78 — PART-STRKEY-1 (audit 2026-09-01, F1): string partition keys.
*   The engine's Hive writer names the directory of the string value "NULL"
*   (or "__HIVE_DEFAULT_PARTITION__") exactly like the directory of a MISSING
*   partition, and its reader maps both back to a missing key — so the value
*   silently loaded as "". A save whose staged tree carries such a directory
*   under a string key is now refused before it is published (memory and view
*   save alike, nothing left behind), while every other value — the empty
*   string, '=', '/', space, '%', '.', Unicode, case-distinct and
*   numeric-looking text — and a MISSING numeric/date key round-trip exactly
*   on the eager, lazy and view-save paths (oracles: cf against the source,
*   pyarrow for the directory names). A foreign tree that carries such a
*   directory loads with a note on both read paths.
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

tempfile stem
local dir `"`stem'_d"'
mkdir `"`dir'"'

* ---------- values the engine round-trips ------------------------------------
clear
set obs 12
gen long id = _n
gen str12 k = ""
replace k = "a" in 1
replace k = "" in 2
replace k = "a=b" in 3
replace k = "a/b" in 4
replace k = "x y" in 5
replace k = "é" in 6
replace k = "A" in 7
replace k = "a" in 8
replace k = "01" in 9
replace k = "a%20b" in 10
replace k = "%" in 11
replace k = "." in 12
gen long n = cond(mod(_n, 4) == 0, ., -5 * _n)
gen double d = td(15jan2020) + _n
replace d = . in 3
format d %td
gen double v = _n * 1.5
label var k "key label"
sort id
tempfile ref
save `"`ref'"', replace
parqit save `"`dir'/src.parquet"', replace data

foreach key in k n d {
    parqit save `"`dir'/by_`key'"', replace data partition_by(`key')
    assert r(N) == 12
    parqit use `"`dir'/by_`key'"', clear
    sort id
    cf _all using `"`ref'"'
    assert "`: type k'" == "str12"
    assert `"`: variable label k'"' == "key label"
    parqit use using `"`dir'/by_`key'"'
    parqit collect, clear
    sort id
    cf _all using `"`ref'"'
    parqit close _all
    * the same tree written by a lazy view save
    parqit use using `"`dir'/src.parquet"'
    parqit save `"`dir'/vby_`key'"', replace partition_by(`key')
    assert r(N) == 12
    parqit close _all
    parqit use `"`dir'/vby_`key'"', clear
    sort id
    cf _all using `"`ref'"'
}
python:
from sfi import Macro
import os
d = Macro.getLocal("dir")
dirs = sorted(os.listdir(os.path.join(d, "by_k")))
want = {"k=a", "k=", "k=a%3Db", "k=a%2Fb", "k=x%20y", "k=%C3%A9", "k=A", "k=01",
        "k=a%2520b", "k=%25", "k=."}
Macro.setLocal("dirs_ok", "1" if set(dirs) == want else "0")
Macro.setLocal("dirs_seen", " ".join(dirs))
ndirs = sorted(os.listdir(os.path.join(d, "by_n")))
# a MISSING numeric key is a legitimate missing partition: the engine names its
# directory NULL or __HIVE_DEFAULT_PARTITION__ (1.5.3 writes the latter)
Macro.setLocal("nnull_ok", "1" if ("n=NULL" in ndirs or "n=__HIVE_DEFAULT_PARTITION__" in ndirs) else "0")
end
assert "`dirs_ok'" == "1"
assert "`nnull_ok'" == "1"

* ---------- the two values the engine cannot read back are refused -------------
clear
set obs 3
gen long id = _n
gen str30 k = cond(_n == 2, "NULL", "a")
gen double v = _n
tempfile srcnull
parqit save `"`srcnull'.parquet"', replace data
capture log close v78
log using `"`dir'/v78.log"', replace name(v78) text
capture noisily parqit save `"`dir'/by_null"', replace data partition_by(k)
local rc1 = _rc
parqit use using `"`srcnull'.parquet"'
capture noisily parqit save `"`dir'/by_null_view"', replace partition_by(k)
local rc2 = _rc
parqit close _all
replace k = "__HIVE_DEFAULT_PARTITION__" in 2
capture noisily parqit save `"`dir'/by_hdp"', replace data partition_by(k)
local rc3 = _rc
* a numeric key that is MISSING is a legitimate missing partition, not refused
gen long m = cond(_n == 2, ., _n)
parqit save `"`dir'/by_m"', replace data partition_by(m)
local rc4 = _rc
log close v78
assert `rc1' == 198 & `rc2' == 198 & `rc3' == 198 & `rc4' == 0
python:
from sfi import Macro
import os
d = Macro.getLocal("dir")
gone = not any(os.path.exists(os.path.join(d, p)) for p in ("by_null", "by_null_view", "by_hdp"))
leak = [f for f in os.listdir(d) if ".parqit_" in f]
txt = open(os.path.join(d, "v78.log"), encoding="utf-8", errors="replace").read().replace("\n> ", "")
msgs = txt.count("cannot be written as a partition directory")
Macro.setLocal("refuse_ok", "1" if gone and not leak and msgs >= 3 else "0")
end
assert "`refuse_ok'" == "1"
parqit use `"`dir'/by_m"', clear
sort id
assert m[2] == . & m[1] == 1 & m[3] == 3

* ---------- a foreign tree with such directories loads with a note --------------
python:
from sfi import Macro
import os, shutil, pyarrow as pa, pyarrow.parquet as pq
d = os.path.join(Macro.getLocal("dir"), "pa_tree")
shutil.rmtree(d, ignore_errors=True)
t = pa.table({"id": [1, 2, 3, 4], "k": ["a", None, "NULL", ""], "v": [1.5, 2.5, 3.5, 4.5]})
pq.write_to_dataset(t, root_path=d, partition_cols=["k"])
Macro.setLocal("pa_dirs", " ".join(sorted(os.listdir(d))))
end
assert strpos("`pa_dirs'", "k=__HIVE_DEFAULT_PARTITION__") > 0 & strpos("`pa_dirs'", "k=NULL") > 0
log using `"`dir'/v78b.log"', replace name(v78b) text
parqit use `"`dir'/pa_tree"', clear
sort id
assert k[1] == "a" & k[2] == "" & k[3] == "" & k[4] == ""
parqit use using `"`dir'/pa_tree"'
parqit collect, clear
sort id
assert k[1] == "a" & k[2] == "" & k[3] == "" & k[4] == ""
parqit close _all
log close v78b
python:
from sfi import Macro
import os
txt = open(os.path.join(Macro.getLocal("dir"), "v78b.log"), encoding="utf-8", errors="replace").read().replace("\n> ", "")
ok = txt.count('directory value "NULL"') >= 2 and txt.count('directory value "__HIVE_DEFAULT_PARTITION__"') >= 2
Macro.setLocal("note_ok", "1" if ok else "0")
end
assert "`note_ok'" == "1"

di "VERDICT(V78_PARTITION_STRING_KEYS): PASS - string keys round-trip exactly (empty, =, /, space, %, ., Unicode, case-distinct, numeric-looking) on eager/lazy/view save, NULL and __HIVE_DEFAULT_PARTITION__ string values are refused before publishing, missing numeric keys stay legitimate, foreign trees with such directories load with a note on both paths"
