* ADVERSARIAL: the 2045-byte str#/strL boundary and the strL sidecar.
* Lengths are BYTES (UTF-8): a 2045-byte value stays str2045, 2046 bytes
* must become strL; multibyte characters straddling internal chunk edges
* must reassemble exactly; ~1MB strLs stream through both in-memory writers;
* binary strLs (embedded NUL) are refused on save with a loud error.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

local fails 0
tempfile fbase
local f `"`fbase'.parquet"'

python:
from sfi import Macro
import pyarrow as pa, pyarrow.parquet as pq
s2045 = "a" * 2045
s2046 = "b" * 2046
# 2044 ASCII + one 4-byte emoji = 2048 bytes -> strL, multibyte at the edge
edge  = "c" * 2044 + "\U0001F600"
big   = ("xyz" * 350000)[:1048576]          # 1 MiB exactly
t = pa.table({
    "at_max":  pa.array([s2045, "short", None]),
    "over":    pa.array([s2046, "", "z"]),
    "edge":    pa.array([edge, "e", None]),
    "big":     pa.array([big, "tiny", None]),
})
pq.write_table(t, Macro.getLocal("f"))
end

parqit use using `"`f'"', clear

* ---------- byte-exact typing at the boundary -------------------------------
local t : type at_max
if ("`t'" != "str2045") {
    di as err "FAIL: 2045-byte value typed `t', want str2045"
    local ++fails
}
local t : type over
if ("`t'" != "strL") {
    di as err "FAIL: 2046-byte value typed `t', want strL"
    local ++fails
}
local t : type edge
if ("`t'" != "strL") {
    di as err "FAIL: 2048-byte multibyte value typed `t', want strL"
    local ++fails
}

* ---------- payload integrity ------------------------------------------------
assert strlen(at_max[1]) == 2045
assert strlen(over[1]) == 2046
assert at_max[3] == "" & over[2] == ""
assert strlen(edge[1]) == 2048
assert usubstr(edge[1], 2045, 1) == uchar(128512)
assert strlen(big[1]) == 1048576
* 1048576 = 3*349525 + 1: the repeating "xyz" tail lands as y,z,x
assert substr(big[1], 1048574, 3) == "yzx"
assert big[2] == "tiny"

* ---------- both strL writers round-trip to parquet (pyarrow oracle) --------
* Invalidate the unchanged-source fast path so both saves below must read the
* Stata strL cells through SF_strldata() and return the full reported length.
gen byte path_marker = 1
tempfile obase sbase bbase abase
local o `"`obase'.parquet"'
local staged `"`sbase'.parquet"'
local blocked `"`bbase'.parquet"'
local automatic `"`abase'.parquet"'
python:
import os
os.environ.pop("PARQIT_SAVE_NOARROW", None)
os.environ.pop("PARQIT_TEST_ARROW_OFFSET_LIMIT", None)
os.environ.pop("PARQIT_TEST_FAIL_ARROW_REGISTER", None)
end
qui parqit save `"`o'"', replace
python:
import os
os.environ["PARQIT_SAVE_NOARROW"] = "1"
end
qui parqit save `"`staged'"', replace
python:
import os
os.environ.pop("PARQIT_SAVE_NOARROW", None)
os.environ["PARQIT_TEST_FAIL_ARROW_REGISTER"] = "1"
end
capture parqit save `"`blocked'"', replace
if (_rc == 0) {
    di as err "FAIL: Arrow registration hook did not fail"
    local ++fails
}
capture confirm file `"`blocked'"'
if (_rc == 0) {
    di as err "FAIL: blocked Arrow registration published a destination"
    local ++fails
}
python:
import os
os.environ["PARQIT_TEST_ARROW_OFFSET_LIMIT"] = "4096"
end
qui parqit save `"`automatic'"', replace
python:
import os
os.environ.pop("PARQIT_TEST_ARROW_OFFSET_LIMIT", None)
os.environ.pop("PARQIT_TEST_FAIL_ARROW_REGISTER", None)
end
python:
from sfi import Macro, Scalar
import pyarrow.parquet as pq
ok = 1
for path in (Macro.getLocal("o"), Macro.getLocal("staged"),
             Macro.getLocal("automatic")):
    t = pq.read_table(path)
    if t.column("big").to_pylist()[0] != ("xyz" * 350000)[:1048576]: ok = 0
    if t.column("edge").to_pylist()[0] != "c" * 2044 + "\U0001F600": ok = 0
    if [len(x) if x is not None else None for x in t.column("over").to_pylist()] \
            != [2046, 0, 1]: ok = 0
Scalar.setValue("pyok", ok)
end
if (scalar(pyok) != 1) {
    di as err "FAIL: Arrow, staged or automatic-fallback strL payload diverged (pyarrow)"
    local ++fails
}

* note: NULL≡"" inside Stata (no distinction exists there), so the writer
* canonicalises both to "" on disk — the pyarrow check asserts exactly that.

* ---------- binary strL must be refused loudly on save ----------------------
clear
qui set obs 2
gen strL b = "plain"
mata: st_sstore(1, "b", "bin" + char(0) + "ary")
capture parqit save `"`o'"', replace
if (_rc == 0) {
    di as err "FAIL: binary strL was saved silently"
    local ++fails
}

if (`fails' == 0) di "VERDICT(V19_STRL_BOUNDARY): PASS - boundaries exact; Arrow, staged and automatic fallback preserve strL; binary refused"
else {
    di as err "VERDICT(V19_STRL_BOUNDARY): FAIL - `fails' check(s)"
    exit 9
}
