* V39 — PARQIT-CHAR-01: restoring a characteristic/note of a projected-away
* variable must NOT abort materialisation. A column-subset `use`, or a
* contract/collapse/keep/drop that removes a char- or note-bearing column before
* `collect`, used to emit the orphan char record and abort the ado's st_global
* with `rc 3300 argument out of range`. Fixed at two layers (defence in depth):
* the plugin emitter filters char/note records to `_dta` + live result columns
* (mirroring the `save` path), and the ado `char` branch existence-gates with the
* non-aborting `_st_varindex`. rename (META-2) and full-read round-trip must NOT
* regress.
*
* Independent oracle: pyarrow confirms the on-disk file really carries a char on
* the column that gets dropped — the char-stripped fixtures elsewhere in the
* suite mask this bug, so the fixture here must retain it. Native Stata `: char`
* reads verify the survivors.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

local fails 0
tempfile f

* fixture: char on a survivor, char on a to-be-dropped col, a variable note, a
* dataset note
clear
set obs 6
gen keepme = _n
gen gone   = _n * 10
gen grp    = mod(_n, 2)
gen noted  = _n
char keepme[source] "survivor"
char gone[source]   "to-be-dropped"
note noted: a variable note
note: a dataset note
parqit save `"`f'.parquet"', replace data

* ---- ORACLE : the on-disk file must actually carry the char on the dropped col,
*               else this test would mask the bug like char-stripped fixtures do.
local has_gone 0
python:
import json, pyarrow.parquet as pq
from sfi import Macro
md = pq.read_metadata(Macro.getLocal("f") + ".parquet").metadata or {}
chars = json.loads((md.get(b"parqit.chars") or b"{}").decode())
ok = isinstance(chars.get("gone"), dict) and "source" in chars["gone"]
Macro.setLocal("has_gone", "1" if ok else "0")
end
capture assert "`has_gone'" == "1"
if (_rc) di as err "FAIL setup: on-disk file lacks a char on dropped col 'gone' — test would mask the bug"
local fails = `fails' + (_rc!=0)

* ---- CASE 1 (headline) : column-subset use excludes the char/note cols --------
clear
capture noisily parqit use keepme grp using `"`f'.parquet"', clear
if (_rc) di as err "FAIL c1: subset use aborted (rc=`=_rc')"
local fails = `fails' + (_rc!=0)
if (_rc==0) {
    capture assert `"`: char keepme[source]'"' == "survivor"
    if (_rc) di as err "FAIL c1: survivor char lost"
    local fails = `fails' + (_rc!=0)
}

* ---- CASE 2 : contract drops the char-bearing column --------------------------
clear
parqit use using `"`f'.parquet"'
parqit contract grp, freq(freq)
capture noisily parqit collect, clear
if (_rc) di as err "FAIL c2: contract+collect aborted (rc=`=_rc')"
local fails = `fails' + (_rc!=0)
if (_rc==0) {
    capture assert _N == 2 & c(k) == 2
    if (_rc) di as err "FAIL c2: wrong shape (_N=`=_N' k=`=c(k)')"
    local fails = `fails' + (_rc!=0)
}
capture parqit close _all

* ---- CASE 3 : collapse drops the char-bearing column -------------------------
clear
parqit use using `"`f'.parquet"'
parqit collapse (mean) mk=keepme, by(grp)
capture noisily parqit collect, clear
if (_rc) di as err "FAIL c3: collapse+collect aborted (rc=`=_rc')"
local fails = `fails' + (_rc!=0)
capture parqit close _all

* ---- CASE 4 : var + dataset notes still round-trip on a full read (control) ---
clear
parqit use using `"`f'.parquet"', clear
capture assert `"`: char _dta[note1]'"' != ""
if (_rc) di as err "FAIL c4: _dta note lost on full read"
local fails = `fails' + (_rc!=0)
capture assert `"`: char noted[note1]'"' != ""
if (_rc) di as err "FAIL c4: variable note lost on full read"
local fails = `fails' + (_rc!=0)

* ---- CASE 5 : rename must NOT regress (char follows the renamed survivor) -----
clear
parqit use using `"`f'.parquet"'
parqit rename keepme kept
capture noisily parqit collect, clear
if (_rc) di as err "FAIL c5: rename+collect aborted (rc=`=_rc')"
local fails = `fails' + (_rc!=0)
if (_rc==0) {
    capture assert `"`: char kept[source]'"' == "survivor"
    if (_rc) di as err "FAIL c5: char did not follow rename"
    local fails = `fails' + (_rc!=0)
}
capture parqit close _all

di as txt "VERDICT(V39_CHAR_PROJECTION): " cond(`fails'==0, "PASS", "FAIL — `fails' failures") ///
    " - subset-use/contract/collapse drop char+note cols without rc3300; full-read notes + rename char preserved"
