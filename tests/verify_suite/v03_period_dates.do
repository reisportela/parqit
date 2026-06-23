* CHARTER 3 (pq finding 3): %tm/%tq/%th/%ty/%tw/%tb/%tC variables are never
* written as calendar dates — they stay INTEGER period counts on disk with
* their true format in parqit.* metadata, and round-trip with format intact.
* pq's signature: 2026m1 (=792) written as date32 1962-03-03.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

set obs 1
gen int    m  = tm(2026m1)
format m %tm
gen int    q  = tq(2026q2)
format q %tq
gen int    h  = th(2026h1)
format h %th
gen int    y  = 2026
format y %ty
gen int    w  = tw(2026w7)
format w %tw
gen double tc2 = tC(01jan2026 12:00:00)
format tc2 %tC

tempfile obase
local out `"`obase'.parquet"'
parqit save `"`out'"', replace

python:
from sfi import Macro
import pyarrow.parquet as pq
import datetime as dt
t = pq.read_table(Macro.getLocal("out"))
s = {f.name: str(f.type) for f in t.schema}
ok = True
def chk(c, w):
    global ok
    if not c:
        ok = False
        print("ORACLE FAIL:", w)
for c in ("m", "q", "h", "y", "w"):
    chk(s[c] == "int32", c + " must be int32 on disk, got " + s[c])
chk(s["tc2"] == "int64", "tc2 (%tC ms count) must be int64 on disk, got " + s["tc2"])
cols = t.to_pydict()
chk(cols["m"][0] == 792, "m: raw month count 792, got " + str(cols["m"][0]))
chk(cols["q"][0] == 265, "q: raw quarter count 265, got " + str(cols["q"][0]))
chk(cols["y"][0] == 2026, "y: raw year, got " + str(cols["y"][0]))
# the buggy signature: any of these as a date32 like 1962-03-03
chk(not any(isinstance(v, dt.date) for v in (cols["m"][0], cols["q"][0])),
    "period counts must not be calendar dates")
Macro.setLocal("oracle_ok", "1" if ok else "0")
end
assert "`oracle_ok'" == "1"

parqit use using `"`out'"', clear
assert m[1] == tm(2026m1)
assert q[1] == tq(2026q2)
assert y[1] == 2026
assert w[1] == tw(2026w7)
local fm : format m
local fq : format q
local fy : format y
local fw : format w
local ftc : format tc2
assert "`fm'" == "%tm" & "`fq'" == "%tq" & "`fy'" == "%ty" & "`fw'" == "%tw"
assert "`ftc'" == "%tC"
assert tc2[1] == tC(01jan2026 12:00:00)

di "VERDICT(V03_PERIOD_DATES): PASS - period counts stay integers on disk; formats and values round-trip"
