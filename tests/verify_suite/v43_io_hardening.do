* V43 — GLOB-2/SAN-SE-1/HEAD-1/RESHAPE-MISSJ-1/LIST-IN-1/DUP-NORM-1/MERGE-NULLSTR-1:
* I/O and verb hardening from the 2026-07-02 adversarial audit.
*   (a) a filename with glob metacharacters that EXISTS is that literal file,
*       never a pattern (`data[1].parquet` used to silently read data1.parquet);
*   (b) `_se` sanitises to `__se` (summarize _se on a loaded `_se` is a silent
*       no-op against the system variable);
*   (c) `parqit head -3` is r(198), not a full-view materialisation;
*   (d) `reshape wide` with missing j errors like native r(498), never silently
*       destroying the missing-j rows;
*   (e) `parqit list ... in f/l` beyond the row count errors like native;
*   (f) `duplicates drop` (no varlist) treats '' and NULL strings as equal;
*   (g) a lazy save after merge/append never writes SQL NULL strings (pyarrow
*       null_count oracle) — the boundary contract holds on every path.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

local fails 0
local dir "`c(tmpdir)'/parqit_v43"
capture mkdir "`dir'"

* ---- (a) glob metacharacters in existing filenames ---------------------------
clear
set obs 2
gen double a = _n
parqit save "`dir'/data1.parquet", replace data
* no file literally named data[1].parquet exists -> must FAIL, not read data1
capture parqit use using "`dir'/data[1].parquet", clear
if (_rc == 0) {
    di as err "FAIL a1: data[1].parquet read a DIFFERENT file (glob leak)"
    local fails = `fails' + 1
}
* now create the literal bracket file with distinct content -> must read IT
clear
set obs 5
gen double a = 99
parqit save "`dir'/databr.parquet", replace data
python:
import shutil
from sfi import Macro
d = Macro.getLocal("dir")
shutil.copy(d + "/databr.parquet", d + "/data[1].parquet")
end
parqit use using "`dir'/data[1].parquet", clear
capture assert _N == 5 & a[1] == 99
if (_rc) {
    di as err "FAIL a2: literal bracket file not read as itself"
    local fails = `fails' + 1
}

* ---- (b) _se sanitised --------------------------------------------------------
python:
import pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
pq.write_table(pa.table({"_se": [1.0, 2.0], "_b": [3.0, 4.0]}),
               Macro.getLocal("dir") + "/sysnames.parquet")
end
parqit use using "`dir'/sysnames.parquet", clear
capture confirm variable __se
if (_rc) {
    di as err "FAIL b1: _se column did not sanitise to __se"
    local fails = `fails' + 1
}
capture confirm variable __b
if (_rc) {
    di as err "FAIL b2: _b column did not sanitise to __b"
    local fails = `fails' + 1
}

* ---- (c) head must be positive ------------------------------------------------
parqit use using "`dir'/databr.parquet"
capture parqit head -3
if (_rc != 198) {
    di as err "FAIL c: parqit head -3 rc=`=_rc' (want 198)"
    local fails = `fails' + 1
}
parqit close

* ---- (d) reshape wide with missing j is loud ----------------------------------
clear
input long id double j double inc
1 1 10
1 2 20
2 . 99
end
parqit save "`dir'/missj.parquet", replace data
parqit use using "`dir'/missj.parquet"
capture noisily parqit reshape wide inc, i(id) j(j)
local rcw = _rc
if (`rcw' == 0) {
    capture parqit collect, clear
    if (_rc == 0) {
        di as err "FAIL d1: reshape wide with missing j succeeded silently"
        local fails = `fails' + 1
    }
}
parqit close
* string j: empty string is missing too
clear
input long id str2 j double inc
1 "a" 10
2 ""  99
end
parqit save "`dir'/missjs.parquet", replace data
parqit use using "`dir'/missjs.parquet"
capture parqit reshape wide inc, i(id) j(j) string
local rcs = _rc
if (`rcs' == 0) {
    capture parqit collect, clear
    if (_rc == 0) {
        di as err "FAIL d2: reshape wide with empty string j succeeded silently"
        local fails = `fails' + 1
    }
}
parqit close

* ---- (e) list in out-of-range is loud ------------------------------------------
parqit use using "`dir'/databr.parquet"
capture parqit list a in 10/20
if (_rc == 0) {
    di as err "FAIL e: parqit list in 10/20 over 5 rows was silent"
    local fails = `fails' + 1
}
parqit close

* ---- (f)+(g) merge/append NULL strings: dedupe + on-disk honesty ---------------
clear
set obs 2
gen long k = _n
gen str4 t = ""
parqit save "`dir'/m_master.parquet", replace data
clear
set obs 1
gen long k = 1
parqit save "`dir'/m_using_nostr.parquet", replace data

* append a file that lacks t -> appended rows carry NULL t in SQL terms
parqit use using "`dir'/m_master.parquet"
parqit append using "`dir'/m_using_nostr.parquet"
parqit duplicates drop
parqit collect, clear
* rows: (1,""), (2,""), (1,NULL~"") -> dedupe must see (1,NULL)==(1,"") -> 2 rows
capture assert _N == 2
if (_rc) {
    di as err "FAIL f: duplicates drop kept ('',NULL) apart (N=`=_N')"
    local fails = `fails' + 1
}

* save after append must not write NULL strings (pyarrow oracle)
parqit use using "`dir'/m_master.parquet"
parqit append using "`dir'/m_using_nostr.parquet"
parqit save "`dir'/m_out.parquet", replace
parqit close
local nnull -1
python:
import pyarrow.parquet as pq
from sfi import Macro
t = pq.read_table(Macro.getLocal("dir") + "/m_out.parquet")
Macro.setLocal("nnull", str(t.column("t").null_count))
end
capture assert `nnull' == 0
if (_rc) {
    di as err "FAIL g1: lazy save after append wrote `nnull' NULL string(s)"
    local fails = `fails' + 1
}

* merge with unmatched master rows -> using-side strings NULL -> save honest
clear
set obs 1
gen long k = 1
gen str4 u = "uu"
parqit save "`dir'/m_using_str.parquet", replace data
parqit use using "`dir'/m_master.parquet"
parqit merge 1:1 k using "`dir'/m_using_str.parquet"
parqit save "`dir'/m_out2.parquet", replace
parqit close
local nnull2 -1
python:
import pyarrow.parquet as pq
from sfi import Macro
t = pq.read_table(Macro.getLocal("dir") + "/m_out2.parquet")
Macro.setLocal("nnull2", str(t.column("u").null_count))
end
capture assert `nnull2' == 0
if (_rc) {
    di as err "FAIL g2: lazy save after merge wrote `nnull2' NULL string(s)"
    local fails = `fails' + 1
}

* cleanup of the bracket file (avoid confusing later runs)
python:
import os
from sfi import Macro
d = Macro.getLocal("dir")
for f in os.listdir(d):
    os.remove(os.path.join(d, f))
os.rmdir(d)
end

if (`fails' == 0) di as res "VERDICT(V43_IO_HARDENING): PASS - glob-literal, _se, head, reshape-j, list-in, dedupe, no NULL strings on disk"
else di as err "VERDICT(V43_IO_HARDENING): FAIL - `fails' case(s)"
