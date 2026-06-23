* CHARTER 5 (pq finding 5): a %tc variable whose display format contains
* hh:mm tokens (e.g. %tcHH:MM:SS) must be written as a real TIMESTAMP with
* its values — pq's signature was an all-null time64 column (total loss).
* Storage class comes from the format PREFIX, never from display tokens.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

set obs 4
gen double t = tc(01jan2026 08:30:00) + (_n - 1) * 3600000
format t %tcHH:MM:SS
gen double t2 = t
format t2 %tc_DD/NN/CCYY_HH:MM

tempfile obase
local out `"`obase'.parquet"'
parqit save `"`out'"', replace

python:
from sfi import Macro
import pyarrow.parquet as pq
import datetime as dt
t = pq.read_table(Macro.getLocal("out"))
ok = True
def chk(c, w):
    global ok
    if not c:
        ok = False
        print("ORACLE FAIL:", w)
for c in ("t", "t2"):
    chk(str(t.schema.field(c).type).startswith("timestamp"),
        c + " must be timestamp, got " + str(t.schema.field(c).type))
cols = t.to_pydict()
chk(all(v is not None for v in cols["t"]), "the all-null bug signature")
chk(cols["t"][0] == dt.datetime(2026, 1, 1, 8, 30), "value: " + str(cols["t"][0]))
chk(cols["t"][3] == dt.datetime(2026, 1, 1, 11, 30), "value+3h: " + str(cols["t"][3]))
Macro.setLocal("oracle_ok", "1" if ok else "0")
end
assert "`oracle_ok'" == "1"

parqit use using `"`out'"', clear
assert t[1] == tc(01jan2026 08:30:00)
assert !missing(t[4])
local f1 : format t
assert "`f1'" == "%tcHH:MM:SS"

di "VERDICT(V05_HHMM): PASS - hh:mm display tokens never corrupt %tc storage; values intact on disk"
