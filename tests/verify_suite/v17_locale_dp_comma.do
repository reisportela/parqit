* ADVERSARIAL: locale. This machine runs a pt_PT locale (decimal comma in
* system tools) and Stata users may `set dp comma`. Fractional literals in
* expressions, generated SQL, returned r() scalars and the on-disk payload
* must all stay period-decimal internally — a comma leaking into SQL or a
* response file would silently corrupt numbers.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

local fails 0
set dp comma

* ---------- expression literals + computed columns under dp comma ----------
clear
set obs 6
gen double x = _n / 4
parqit open _data
parqit keep if x > 0.5 & x < 1.3
parqit gen double y = x * 2.5
parqit collect, clear
if (_N != 3) {
    di as err "FAIL: fractional filter bounds broke under dp comma"
    local ++fails
}
if (reldif(y[1], 1.875) > 1e-12) {
    di as err "FAIL: fractional gen constant broke under dp comma"
    local ++fails
}
parqit close _all

* ---------- pushdown stats return exact scalars under dp comma -------------
clear
set obs 100
gen double w = _n / 7
qui summarize w
local omean = r(mean)
local osd   = r(sd)
parqit open _data
parqit summarize w
if (reldif(r(mean), `omean') > 1e-12 | reldif(r(sd), `osd') > 1e-9) {
    di as err "FAIL: summarize scalars diverge under dp comma"
    local ++fails
}
parqit histogram w, bins(7)
if (r(bins) != 7 | reldif(r(width), (100/7 - 1/7)/7) > 1e-6) {
    di as err "FAIL: histogram bin scalars diverge under dp comma"
    local ++fails
}
parqit close _all

* ---------- on-disk payload: pyarrow is the oracle --------------------------
clear
set obs 4
gen double frac = _n + 0.25
tempfile obase
local o `"`obase'.parquet"'
qui parqit save `"`o'"', replace
python:
from sfi import Macro, Scalar
import pyarrow.parquet as pq
vals = pq.read_table(Macro.getLocal("o")).column("frac").to_pylist()
Scalar.setValue("pyok", 1 if vals == [1.25, 2.25, 3.25, 4.25] else 0)
end
if (scalar(pyok) != 1) {
    di as err "FAIL: fractional payload corrupted on disk under dp comma"
    local ++fails
}

set dp period
if (`fails' == 0) di "VERDICT(V17_LOCALE_DP_COMMA): PASS - dp comma never leaks into SQL, scalars or payloads"
else {
    di as err "VERDICT(V17_LOCALE_DP_COMMA): FAIL - `fails' check(s)"
    exit 9
}
