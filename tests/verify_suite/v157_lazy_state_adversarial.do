* V157: adversarial 0.4.0 regressions. Native Stata is the value/order oracle;
* pyarrow independently checks the saved payload. All inputs are synthetic.
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
parqit set statamissing on
tempfile stem original native

* replace is sequential even when the self-reference appears only in if.
set obs 6
gen double id = ceil(_n/3)
gen double t = mod(_n-1, 3)+1
gen double x = t == 1
sort id t
save "`original'", replace
parqit save "`stem'.parquet", data replace
by id: replace x = 1 if x[_n-1] == 1
assert x == 1
quietly datasignature
local sig "`r(datasignature)'"
parqit use using "`stem'.parquet"
parqit sort id t
capture noisily parqit by id: replace x = 1 if x[_n-1] == 1
assert _rc == 198
parqit xtset id t
capture noisily parqit replace x = 1 if L.x == 1
assert _rc == 198
quietly datasignature
assert "`r(datasignature)'" == "`sig'"
parqit collect, clear
cf _all using "`original'"
* A neighbour of a different variable remains supported.
parqit by id: replace x = 2 if t[_n-1] == 1
parqit collect, clear
assert x == 2 if t == 2
assert x == (t == 1) if t != 2
parqit close _all

* Window placeholders are code, never a part of a literal or an identifier.
use "`original'", clear
gen double __PARQIT_WIN_1__ = 777
gen double __PARQIT_OVER_FULL__ = 17
parqit save "`stem'_tokens.parquet", data replace
gen str80 marker = cond(x[1] == 1, "a'__PARQIT_WIN_1__b", "other")
gen double both = x[1]+__PARQIT_WIN_1__
gen double last = __PARQIT_OVER_FULL__[_N]
save "`native'", replace
parqit use using "`stem'_tokens.parquet"
parqit sort id t
parqit gen str80 marker = cond(x[1] == 1, "a'__PARQIT_WIN_1__b", "other")
parqit gen double both = x[1]+__PARQIT_WIN_1__
parqit gen double last = __PARQIT_OVER_FULL__[_N]
parqit collect, clear
cf _all using "`native'"
parqit keep if x[1] == 1 & marker == "a'__PARQIT_WIN_1__b"
parqit save "`stem'_tokens_out.parquet", replace
local saved "`stem'_tokens_out.parquet"
python:
from sfi import Macro
import pyarrow.parquet as pq
t = pq.read_table(Macro.getLocal("saved"))
assert t.num_rows == 6
assert t.column("marker").to_pylist() == ["a'__PARQIT_WIN_1__b"] * 6
assert t.column("both").to_pylist() == [778.] * 6
assert t.column("last").to_pylist() == [17.] * 6
assert t.column("__PARQIT_WIN_1__").to_pylist() == [777.] * 6
end
parqit close _all

* A simultaneous swap maps sort keys and tsset keys once, by old identity.
use "`original'", clear
xtset id t
rename (id t) (t id)
gen double position = _n
gen double lag = L.x
save "`native'", replace
parqit use using "`stem'.parquet"
parqit sort id t
parqit xtset id t
parqit rename (id t) (t id)
parqit tsset
assert "`r(panelvar)'" == "t" & "`r(timevar)'" == "id"
parqit gen double position = _n
parqit gen double lag = L.x
parqit collect, clear
assert "`: sortedby'" == "t id"
cf _all using "`native'"
parqit close _all

* Also check descending order and a rename chain with an unused final name.
use "`original'", clear
gsort -id t
rename (id t) (t time)
gen double position = _n
save "`native'", replace
parqit use using "`stem'.parquet"
parqit gsort -id t
parqit rename (id t) (t time)
parqit gen double position = _n
parqit collect, clear
cf _all using "`native'"
parqit close _all

* Removed declaration keys must stay removed even if their name is reused.
foreach key in id t {
    parqit use using "`stem'.parquet"
    parqit xtset id t
    parqit drop `key'
    parqit gen double `key' = 1
    capture noisily parqit gen double lag = L.x
    assert _rc == 198
    parqit close _all
}
parqit use using "`stem'.parquet"
parqit xtset id t
parqit keep id x
parqit rename x t
parqit tsset
assert "`r(timevar)'" == ""
parqit close _all

* Overflow in a lag count must not turn 2^64 into a zero-lag identity.
use "`original'", clear
xtset id t
capture noisily gen double bad = L18446744073709551616.x
assert _rc == 198
parqit use using "`stem'.parquet"
parqit sort id t
parqit xtset id t
foreach op in L F D S {
    capture noisily parqit gen double bad = `op'18446744073709551616.x
    assert _rc == 198
}
capture noisily parqit gen double bad = date("2020-01-01", "9999999999999999999999999YMD")
assert _rc == 198
parqit gen double lag = L00000000000000000000000001.x
parqit collect, clear
assert missing(lag) if t == 1
assert lag == 1 if t == 2
assert lag == 0 if t == 3
parqit close _all
display "VERDICT(V157_LAZY_STATE_ADVERSARIAL): PASS - sequential-condition refusals are atomic; window literals and names, simultaneous rename ordering and tsset identity match their oracles; removed declarations stay void; oversized lag counts refuse"
