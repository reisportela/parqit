* CHARTER 7 (pq finding 7): partially labelled variables keep their NUMERIC
* values on disk (labels live in metadata, so unlabelled values can never
* collapse to indistinguishable empty strings), and a labelled extended
* missing stays a null (with its label preserved for parqit readers) rather
* than becoming an ordinary string.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

set obs 5
gen byte y = _n          // 1..5; only 1 is labelled
replace  y = .a in 5     // labelled extended missing
label define yl 1 "one" .a "refused"
label values y yl

tempfile obase
local out `"`obase'.parquet"'
parqit save `"`out'"', replace

python:
from sfi import Macro
import json
import pyarrow.parquet as pq
t = pq.read_table(Macro.getLocal("out"))
ok = True
def chk(c, w):
    global ok
    if not c:
        ok = False
        print("ORACLE FAIL:", w)
cols = t.to_pydict()
chk(str(t.schema.field("y").type) == "int8", "y stays numeric on disk")
chk(cols["y"][:4] == [1, 2, 3, 4], "unlabelled values keep their numbers: " + str(cols["y"]))
chk(cols["y"][4] is None, "labelled .a is null on disk, not a string")
md = {k.decode(): v.decode() for k, v in (t.schema.metadata or {}).items()}
vl = json.loads(md.get("parqit.vallabs", "{}"))
ents = {e[0]: e[1] for e in vl.get("yl", {}).get("entries", [])}
chk(ents.get("1") == "one", "label text for 1 in metadata: " + str(ents))
chk(ents.get(".a") == "refused", "label for .a preserved: " + str(ents))
Macro.setLocal("oracle_ok", "1" if ok else "0")
end
assert "`oracle_ok'" == "1"

* parqit → parqit round-trip restores the label set (values + texts)
parqit use using `"`out'"', clear
assert y[2] == 2 & y[4] == 4
assert `"`: label yl 1'"' == "one"
assert `"`: label yl .a'"' == "refused"
assert `"`: value label y'"' == "yl"

di "VERDICT(V07_LABELS): PASS - values stay numbers on disk; labels (incl. .a) travel in metadata"
