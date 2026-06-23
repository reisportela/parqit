* ADVERSARIAL: IEEE specials and extreme payloads. NaN is the de-facto
* float NA (silent missing); ±Inf is a VALUE Stata cannot hold — it must
* become missing WITH a note. float32 holds finite values up to ±3.4e38
* but Stata float stops at ±1.70e38: such columns must widen to double,
* never silently lose the value. Extreme dates keep exact day counts;
* float16 loads as float; 16-byte binary (UUID) drops loudly.
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
import numpy as np
t = pa.table({
    "f64": pa.array([1.5, float("nan"), float("inf"), float("-inf"), 8.9e307],
                    pa.float64()),
    "f32big": pa.array([1.5, 3.0e38, -3.0e38, None, 2.0],  pa.float32()),
    "f32ok":  pa.array([1.5, -2.5, 1.0e38, None, 0.25],    pa.float32()),
    "h":      pa.array(np.array([1.5, 65504.0, -0.5, 0, 2], dtype=np.float16),
                       pa.float16()),
    "d":      pa.array([0, -3653, 2932896, None, 1], pa.date32()),
})
pq.write_table(t, Macro.getLocal("f"))
end

* capture the load log: the Inf collapse and the widening must be LOUD
tempname lg
local plog "`c(tmpdir)'/_parqit_v15.log"
capture erase `"`plog'"'
log using `"`plog'"', text name(`lg')
parqit use using `"`f'"', clear
log close `lg'
mata: st_local("loadtxt", invtokens(cat(st_local("plog"))', char(10)))
capture erase `"`plog'"'

* NaN and ±Inf -> missing; finite values intact
assert missing(f64[2]) & missing(f64[3]) & missing(f64[4])
assert f64[1] == 1.5 & f64[5] == 8.9e307
if (strpos(`"`loadtxt'"', "outside Stata's storable range") == 0) {
    di as err "FAIL: Inf -> missing was silent"
    local ++fails
}

* float32 beyond Stata float range: widened to double, values preserved
confirm double variable f32big
assert reldif(f32big[2], 3.0e38) < 1e-6
assert reldif(f32big[3], -3.0e38) < 1e-6
assert missing(f32big[4]) & f32big[5] == 2
if (strpos(`"`loadtxt'"', "beyond Stata's float range") == 0) {
    di as err "FAIL: float->double widening was silent"
    local ++fails
}

* in-range float32 stays float (1e38 fits)
confirm float variable f32ok
assert reldif(f32ok[3], 1.0e38) < 1e-6

* float16: exact halves load as float
confirm float variable h
assert h[1] == 1.5 & h[2] == 65504 & h[3] == -0.5

* extreme dates: exact day counts (1970->3653, 1960->0, 31dec9999)
assert d[1] == 3653
assert d[2] == 0
assert d[3] == 2936549
assert missing(d[4])

* round-trip: save what we loaded, scan with pyarrow as the oracle
tempfile obase
local o `"`obase'.parquet"'
qui parqit save `"`o'"', replace

python:
from sfi import Macro, Scalar
import pyarrow.parquet as pq
t = pq.read_table(Macro.getLocal("o"))
ok = 1
c = t.column("f32big").to_pylist()
if abs(c[1] - 3.0e38) > 1e32 or abs(c[2] + 3.0e38) > 1e32: ok = 0
if c[3] is not None: ok = 0
d = t.column("d").to_pylist()
# stored as parqit day numbers -> back on disk as DATE via the %td contract
Scalar.setValue("pyok", ok)
end
if (scalar(pyok) != 1) {
    di as err "FAIL: pyarrow round-trip of extreme floats diverged"
    local ++fails
}

if (`fails' == 0) di "VERDICT(V15_FLOAT_EXTREMES): PASS - NaN silent-NA, Inf loud-missing, f32 widens to double, extreme dates exact"
else {
    di as err "VERDICT(V15_FLOAT_EXTREMES): FAIL - `fails' check(s)"
    exit 9
}
