* N10/N9: append preserves values through collect and the independent disk path.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile out

set obs 1
gen long x = 16777217
parqit open _data, name(using_long)
clear
set obs 1
gen float x = 1
parqit open _data, name(master)
parqit append using view:using_long
parqit save "`out'.parquet"
parqit collect, clear
assert x[2]==16777217
local type : type x
assert "`type'" == "double"
python:
from sfi import Macro
import pyarrow.parquet as pq
t = pq.read_table(Macro.getLocal('out')+'.parquet')
assert str(t.schema.field('x').type) == 'double'
assert t.column('x').to_pylist() == [1.,16777217.]
end
parqit close _all

clear
set obs 1
gen double x = .1
parqit open _data, name(using_double)
clear
set obs 1
gen float x = 1
parqit open _data, name(master)
parqit append using view:using_double
parqit save "`out'.parquet", replace
parqit collect, clear
assert x[2]==.1
python:
t = pq.read_table(Macro.getLocal('out')+'.parquet')
assert str(t.schema.field('x').type) == 'double'
assert t.column('x').to_pylist() == [1.,.1]
end
parqit close _all

clear
set obs 1
gen double x = 1
parqit open _data, name(master)
clear
set obs 4
gen double x = 8e307
parqit open _data, name(overflow)
parqit collapse (sum) x
parqit view master
parqit append using view:overflow
parqit gen byte m = missing(x)
parqit save "`out'.parquet", replace
parqit collect, clear
assert m == missing(x)
python:
t = pq.read_table(Macro.getLocal('out')+'.parquet')
assert t.column('x').to_pylist() == [1.,None]
assert t.column('m').to_pylist() == [0,1]
end
parqit close _all

* An incompatible mixed type must not silently round a wide foreign key.
parqit sql "SELECT 9007199254740993::BIGINT AS x", name(wide)
clear
set obs 1
gen double x = 1
parqit open _data, name(master)
parqit append using view:wide
capture noisily parqit collect, clear
assert _rc==920
assert _N==1 & x[1]==1
parqit close _all
di "VERDICT(V96_APPEND_NUMERIC_FIDELITY): PASS"
