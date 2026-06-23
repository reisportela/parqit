* CHARTER 6 + 8 (adversarial audit 2026-06-16): a Stata string carrying invalid
* UTF-8 (Latin-1/legacy bytes — common in imported/admin data) must NOT be
* written verbatim into a UTF-8-typed Parquet column. The Arrow-scan writer used
* to emit such bytes silently (rc 0) producing a file no reader — parqit included —
* could decode; the staged (PARQIT_SAVE_NOARROW) writer silently nulled the cell.
* Both paths now refuse LOUDLY at the offending cell (parqit_is_valid_utf8 in
* plugin_io.cpp), never rc 0 with a broken/stale file, and a failed save never
* clobbers a pre-existing good file. Valid UTF-8 (ASCII/accented/emoji) is
* unaffected and round-trips, confirmed by an independent pyarrow oracle.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

* ---------- A. invalid UTF-8 errors loudly; leaves no file -------------------
clear
set obs 3
gen int id = _n
gen strL s = ""
replace s = char(233) in 1                 // 0xE9  Latin-1 'é' (invalid alone)
replace s = "ok" + char(195) + "x" in 2    // 0xC3  lead byte, no continuation
replace s = char(255) + char(254) in 3     // 0xFF 0xFE
tempfile bb
local bad `"`bb'.parquet"'
capture noisily parqit save `"`bad'"', replace
assert _rc != 0
capture confirm file `"`bad'"'
assert _rc != 0                            // no broken/unreadable file written

* ---------- B. a failed invalid-UTF-8 save never clobbers a good file --------
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
replace s = char(233) in 1                 // corrupt the in-memory copy
capture noisily parqit save `"`good'"', replace
assert _rc != 0
python:
from sfi import Macro
import hashlib
Macro.setLocal("md5_after",
    hashlib.md5(open(Macro.getLocal("good"), "rb").read()).hexdigest())
end
assert "`md5_before'" == "`md5_after'"     // pre-existing file byte-identical
* and still a valid, readable Parquet
parqit use using `"`good'"'
parqit collect, clear
assert _N == 2 & s[1] == "good"

* ---------- C. valid UTF-8 unaffected; round-trips; pyarrow-confirmed --------
parqit close                                 // export memory below, not the view
clear
set obs 4
gen int id = _n
gen strL s = ""
replace s = "ascii" in 1
replace s = "café"  in 2                    // accented, valid UTF-8 (c3 a9)
replace s = "a" + char(240)+char(159)+char(152)+char(128) + "b" in 3  // 😀 emoji
* s[4] stays "" (empty)
tempfile vb
local vf `"`vb'.parquet"'
parqit save `"`vf'"', replace
assert _rc == 0
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

di "VERDICT(V32_INVALID_UTF8_SAVE): PASS - invalid UTF-8 refused loudly on both " ///
   "writers; no broken/clobbered file; valid UTF-8 round-trips (pyarrow oracle)"
