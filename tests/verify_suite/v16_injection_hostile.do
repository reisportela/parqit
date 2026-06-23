* ADVERSARIAL: injection surface. Column names that read as SQL, quote
* and pipe characters in names and data, embedded NUL bytes, quote
* literals inside parqit expressions, and targets with spaces/accents.
* Nothing may execute as SQL, nothing may corrupt the response protocol,
* and any unavoidable loss (NUL in a C-string world) must be LOUD.
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
names = ['x"; DROP TABLE t; --', "a'b", 'pipe|name', 'select', 'nul_vals', 'k']
t = pa.Table.from_arrays([
    pa.array([1.0, 2.0]),
    pa.array([3.0, 4.0]),
    pa.array(["p|q", "r's"]),
    pa.array([7.0, 8.0]),
    pa.array(["ab\x00cd", "ok"]),
    pa.array([1, 2], pa.int32()),
], names=names)
pq.write_table(t, Macro.getLocal("f"))
end

* ---------- load: sanitised loudly, payloads intact, NUL truncation loud ---
tempname lg
local plog "`c(tmpdir)'/_parqit_v16.log"
capture erase `"`plog'"'
log using `"`plog'"', text name(`lg')
parqit use using `"`f'"', clear
log close `lg'
mata: st_local("loadtxt", invtokens(cat(st_local("plog"))', char(10)))
capture erase `"`plog'"'

assert _N == 2
confirm variable a_b
confirm variable pipe_name
confirm variable select
* SQL in a column name is data, not code: payload columns intact
assert x___DROP_TABLE_t____[1] == 1 & a_b[2] == 4
* pipe is the response-field separator: hex transport must protect it
assert pipe_name[1] == "p|q"
assert pipe_name[2] == "r's"
* NUL truncation must be loud (str# is a C string)
assert nul_vals[1] == "ab" & nul_vals[2] == "ok"
if (strpos(`"`loadtxt'"', "NUL") == 0) {
    di as err "FAIL: NUL truncation was silent"
    local ++fails
}
* original names preserved in chars for reversibility
local src : char pipe_name[src_name]
if (`"`src'"' != "pipe|name") {
    di as err "FAIL: src_name char lost the original column name"
    local ++fails
}

* ---------- expressions: quote literals never break the translator ---------
clear
input str10 s double x
"a'b" 1
"plain" 2
end
parqit open _data
parqit keep if s == "a'b"
parqit collect, clear
if (_N != 1 | x[1] != 1) {
    di as err "FAIL: quote literal in filter mis-translated"
    local ++fails
}
parqit close _all

* ---------- merge keyed on a sanitised hostile column -----------------------
tempfile g1 g2
local m1 `"`g1'.parquet"'
local m2 `"`g2'.parquet"'
python:
from sfi import Macro
import pyarrow as pa, pyarrow.parquet as pq
pq.write_table(pa.Table.from_arrays(
    [pa.array([1, 2, 3], pa.int32()), pa.array([10.0, 20.0, 30.0])],
    names=["group key", "xm"]), Macro.getLocal("m1"))
pq.write_table(pa.Table.from_arrays(
    [pa.array([1, 3], pa.int32()), pa.array([100.0, 300.0])],
    names=["group key", "w"]), Macro.getLocal("m2"))
end
parqit use using `"`m1'"'
parqit merge m:1 group_key using `"`m2'"'
parqit sort group_key
parqit collect, clear
qui count if _merge == 3
if (r(N) != 2 | _N != 3) {
    di as err "FAIL: merge on sanitised space-named key broke"
    local ++fails
}
parqit close _all

* ---------- save/load through a path with spaces and accents ----------------
clear
set obs 3
gen double v = _n
local dir "`c(tmpdir)'/sl ab áé ñ"
capture mkdir `"`dir'"'
parqit save `"`dir'/ficheiro raro ç.parquet"', replace
parqit use using `"`dir'/ficheiro raro ç.parquet"', clear
if (_N != 3) {
    di as err "FAIL: accented/space path round-trip broke"
    local ++fails
}
capture erase `"`dir'/ficheiro raro ç.parquet"'

if (`fails' == 0) di "VERDICT(V16_INJECTION_HOSTILE): PASS - hostile names are data not code; pipes/quotes/NULs survive or fail loudly"
else {
    di as err "VERDICT(V16_INJECTION_HOSTILE): FAIL - `fails' check(s)"
    exit 9
}
