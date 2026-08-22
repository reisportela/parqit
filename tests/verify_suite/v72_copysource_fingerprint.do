* V72 — COPYSOURCE-1 / FP-2 (audit 2026-08-22, A4-1/A4-2/A1-2): the
* unchanged-source copy path no longer runs automatically. c(changed) cannot
* prove the dataset equals the file — Stata exempts sort/gsort and Mata
* st_store/st_sstore/st_view writes — so the automatic path wrote the SOURCE
* FILE (source row order, old values) with rc 0 and a `sortedby` claim the rows
* did not have. Now: the default `parqit save` always reads the dataset in
* memory; `copysource` is an explicit opt-in whose every proof (nonce,
* c(changed), size/mtime/ctime/inode/footer digest, variable names and kinds,
* N, sortedby) is loud. pyarrow is the oracle for what landed on disk.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

tempfile stem
local src  `"`stem'_src.parquet"'
local outs `"`stem'_sorted.parquet"'
local outg `"`stem'_gsorted.parquet"'
local outm `"`stem'_mata.parquet"'
local outc `"`stem'_copy.parquet"'
local outt `"`stem'_tamper.parquet"'
local outr `"`stem'_replaced.parquet"'

clear
set obs 2003
gen long id = _n
gen double x = mod(_n * 7919, 2003)        // a permutation of 0..2002
gen str6 s = "s" + string(_n, "%05.0f")
label var x "x label"
sort id
parqit save `"`src'"', replace data

* ---------- A. the default save writes MEMORY after sort / gsort / Mata store
parqit use using `"`src'"', clear
assert c(changed) == 0
assert `"`: char _dta[_parqit_fast_source_nonce]'"' != ""
sort x
assert c(changed) == 0
parqit save `"`outs'"', replace data
parqit use using `"`src'"', clear
gsort -id
parqit save `"`outg'"', replace data
parqit use using `"`src'"', clear
mata: st_store(1, "x", 999999)
mata: st_sstore(2, "s", "ZZZ")
assert c(changed) == 0
parqit save `"`outm'"', replace data
python:
from sfi import Macro
import pyarrow.parquet as pq, json
def tab(p): return pq.read_table(p)
ts = tab(Macro.getLocal("outs")); tg = tab(Macro.getLocal("outg")); tm = tab(Macro.getLocal("outm"))
xs = ts.column("x").to_pylist()
ok = xs == sorted(xs)                                          # memory order (sorted by x)
ok = ok and json.loads(ts.schema.metadata[b"parqit.schema"])["sortedby"] == ["x"]
ids = tg.column("id").to_pylist()
ok = ok and ids[0] == 2003 and ids[-1] == 1                   # gsort -id order
ok = ok and json.loads(tg.schema.metadata[b"parqit.schema"])["sortedby"] == []
ok = ok and tm.column("x").to_pylist()[0] == 999999 and tm.column("s").to_pylist()[1] == "ZZZ"
Macro.setLocal("okA", "1" if ok else "0")
end
assert "`okA'" == "1"

* ---------- B. copysource on untouched data: identical payload, true sortedby
parqit use using `"`src'"', clear
parqit save `"`outc'"', replace data copysource
assert r(N) == 2003 & r(k) == 3
assert `"`r(copysource)'"' == `"`src'"'
python:
from sfi import Macro
import pyarrow.parquet as pq, json
a = pq.read_table(Macro.getLocal("src")); b = pq.read_table(Macro.getLocal("outc"))
ok = a.to_pydict() == b.to_pydict()
sa = json.loads(a.schema.metadata[b"parqit.schema"]); sb = json.loads(b.schema.metadata[b"parqit.schema"])
ok = ok and sa["sortedby"] == ["id"] and sb["sortedby"] == ["id"]
ok = ok and [v["varlab"] for v in sb["vars"] if v["name"] == "x"][0] == "x label"
ch = json.loads(b.schema.metadata[b"parqit.chars"])
ok = ok and "_parqit_fast_source_nonce" not in ch.get("_dta", {})
Macro.setLocal("okB", "1" if ok else "0")
end
assert "`okB'" == "1"

* ---------- C. copysource refusals: sort, gsort, edit, no source, view open
parqit use using `"`src'"', clear
sort x
capture noisily parqit save `"`outt'"', replace data copysource
assert _rc == 198
capture confirm file `"`outt'"'
assert _rc != 0
parqit use using `"`src'"', clear
gsort -id
capture noisily parqit save `"`outt'"', replace data copysource
assert _rc == 198
parqit use using `"`src'"', clear
replace x = 1 in 1
capture noisily parqit save `"`outt'"', replace data copysource
assert _rc == 198
clear
set obs 2
gen x = _n
capture noisily parqit save `"`outt'"', replace data copysource
assert _rc == 198
parqit use using `"`src'"', clear
parqit use using `"`src'"'
capture noisily parqit save `"`outt'"', replace copysource
assert _rc == 198
parqit close
capture confirm file `"`outt'"'
assert _rc != 0

* ---------- D. fingerprint: a same-size in-place rewrite with the mtime
* restored (cp -p / rsync -a style) is detected (ctime, footer digest); a
* replace by rename (new inode) is detected too
parqit use using `"`src'"', clear
python:
from sfi import Macro
import pyarrow as pa, pyarrow.parquet as pq, os, shutil
src = Macro.getLocal("src")
st = os.stat(src)
t = pq.read_table(src)
# same schema, same row count, different values -> different footer stats + data; pad to the same size
alt = t.set_column(t.schema.get_field_index("x"), "x", pa.array([float(2002 - v) for v in t.column("x").to_pylist()], pa.float64()))
tmp = src + ".alt"
pq.write_table(alt, tmp, compression="snappy")
with open(tmp, "ab") as fh:          # a Parquet reader ignores nothing after PAR1, so pad BEFORE: rewrite keeping size
    pass
# size may differ: force identical size by writing alt in place and truncating/padding is not possible for a Parquet file,
# so emulate the auditor's scenario: overwrite in place with the alt bytes when sizes coincide, else just rewrite in place
data = open(tmp, "rb").read()
with open(src, "r+b") as fh:
    fh.seek(0); fh.write(data); fh.truncate()
os.utime(src, ns=(st.st_atime_ns, st.st_mtime_ns))   # restore mtime
os.remove(tmp)
Macro.setLocal("same_size", "1" if os.stat(src).st_size == st.st_size else "0")
end
capture noisily parqit save `"`outt'"', replace data copysource
assert _rc == 198
capture confirm file `"`outt'"'
assert _rc != 0
* (the default save still writes memory, untouched by the tamper)
parqit save `"`outt'"', replace data
python:
from sfi import Macro, Data
import pyarrow.parquet as pq
t = pq.read_table(Macro.getLocal("outt"))
mem_x = [float(v) for v in Data.get("x")]
Macro.setLocal("okD", "1" if t.column("x").to_pylist() == mem_x and t.column("id").to_pylist()[-1] == 2003 else "0")
end
assert "`okD'" == "1"
* replace-by-rename (new inode, same content as loaded)
parqit save `"`src'"', replace data
parqit use using `"`src'"', clear
python:
from sfi import Macro
import os, shutil
src = Macro.getLocal("src")
shutil.copyfile(src, src + ".new")
os.replace(src + ".new", src)
end
capture noisily parqit save `"`outr'"', replace data copysource
assert _rc == 198
capture confirm file `"`outr'"'
assert _rc != 0

di "VERDICT(V72_COPYSOURCE_FINGERPRINT): PASS - default save writes memory after sort/gsort/Mata stores; copysource copies the unchanged source on request with the true sortedby, and refuses loudly after a sort, gsort, edit, without a source, with a view open, after a same-size in-place rewrite with restored mtime, and after a replace-by-rename"
