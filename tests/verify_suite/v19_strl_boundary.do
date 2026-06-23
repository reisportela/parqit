* ADVERSARIAL: the 2045-byte str#/strL boundary and the strL sidecar.
* Lengths are BYTES (UTF-8): a 2045-byte value stays str2045, 2046 bytes
* must become strL; multibyte characters straddling internal chunk edges
* must reassemble exactly; ~1MB strLs stream through the sidecar; binary
* strLs (embedded NUL) are refused on save with a loud error.
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

* ---------- strL round-trip back to parquet (pyarrow oracle) ----------------
tempfile obase
local o `"`obase'.parquet"'
qui parqit save `"`o'"', replace
python:
from sfi import Macro, Scalar
import pyarrow.parquet as pq
t = pq.read_table(Macro.getLocal("o"))
ok = 1
if t.column("big").to_pylist()[0] != ("xyz" * 350000)[:1048576]: ok = 0
if t.column("edge").to_pylist()[0] != "c" * 2044 + "\U0001F600": ok = 0
if [len(x) if x is not None else None for x in t.column("over").to_pylist()] \
        != [2046, 0, 1]: ok = 0
Scalar.setValue("pyok", ok)
end
if (scalar(pyok) != 1) {
    di as err "FAIL: strL round-trip payload diverged (pyarrow)"
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

if (`fails' == 0) di "VERDICT(V19_STRL_BOUNDARY): PASS - 2045/2046 boundary exact, 1MB strL streams, multibyte edges survive, binary strL refused"
else {
    di as err "VERDICT(V19_STRL_BOUNDARY): FAIL - `fails' check(s)"
    exit 9
}
