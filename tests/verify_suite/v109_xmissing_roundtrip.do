* XMISS-1 (2026-09-20): extended missing values (.a-.z) preserved on disk.
* Parquet has one null, so `parqit save ..., xmissing` writes each such cell's
* code (1-26) into a TINYINT companion column `_parqit_xm_<var>` (0 = not an
* extended missing; the primary cell is null either way) and records the
* pairs under the parqit.xmissing key-value entry. The eager readers (`parqit
* use`, `describe`, `mergein`/`appendin`) hide the companions and `use`
* restores the cells, for every numeric storage type, from the codes. The
* lazy view (phase 1) does NOT restore them: it hides the companions and says
* so. Every payload claim below is checked with pyarrow, never parqit alone.
*
*   A  a THIRD-PARTY (pyarrow-written) file with companions and the key:
*      use restores .a/.z/.m in byte/int/long/float/double, keeps . and the
*      values, keeps the storage types, hides the companions, prints the note
*   B  a varlist projection restores the same cells; describe hides the
*      companions; appendin (native append of the disk side) carries them
*   C  the lazy view hides the companions, folds the cells to . and says so
*   D  corrupt companions are refused loudly, memory untouched: a code
*      outside 1-26, and a code on a cell that holds a value; a companion
*      paired with a string column is hidden and ignored, with a note
*   E  save round trip: every numeric type with ., .a, .z, .m and values,
*      plus a variable with only plain . (no companion written) and a
*      string; pyarrow sees exactly the expected columns, codes, nulls and
*      metadata; parqit use gives the original back cell for cell (a
*      bit-exact comparison that distinguishes . from .a); the staged writer
*      (PARQIT_SAVE_NOARROW) writes the identical file; without the option
*      nothing changes on disk and the note names the option
*   F  a partitioned save carries the companions and the directory read
*      restores them; 0 rows; a dataset holding a variable named like a
*      companion is refused; the save from a lazy view loses .a-.z and the
*      open says so
clear all
set more off
set varabbrev off
set linesize 244
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
local fails 0

program define check, rclass
    args cond what
    capture assert `cond'
    if (_rc) di as err "  [FAIL] `what'"
    else di as txt "  [ok] `what'"
    return scalar bad = (_rc != 0)
end

capture program drop _v109_loghas
program define _v109_loghas, rclass
    version 16.0
    gettoken lf 0 : 0
    local pat = strtrim(`"`0'"')
    tempname fh
    local found 0
    file open `fh' using `"`lf'"', read text
    file read `fh' line
    while (r(eof)==0) {
        if (strpos(`"`line'"', `"`pat'"')) local found 1
        file read `fh' line
    }
    file close `fh'
    return scalar found = `found'
end

tempfile base
local third `"`base'_third.parquet"'
local bad27 `"`base'_bad27.parquet"'
local badval `"`base'_badval.parquet"'
local strcomp `"`base'_strcomp.parquet"'

* =====================================================================
* A — a third-party file: rows 1 value, 2 ., 3 .a, 4 .z, 5 .m, 6 value, 7 .
* =====================================================================
python:
import json, pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
def prim(v1, v6, t):
    return pa.array([v1, None, None, None, None, v6, None], t)
codes = [0, 0, 1, 26, 13, 0, 0]
def schema_json(types):
    return json.dumps({"version": 1, "sortedby": [], "vars": [
        {"name": n, "src": n, "type": t, "fmt": "", "varlab": "", "vallab": ""}
        for n, t in types]})
cols = {"b": prim(1, 5, pa.int8()), "i": prim(100, 500, pa.int16()),
        "l": prim(1000, 5000, pa.int32()), "f": prim(1.5, 5.5, pa.float32()),
        "d": prim(1.25, 5.25, pa.float64()),
        "s": pa.array(["a", "", "c", "d", "e", "f", ""], pa.string())}
for v in "bilfd":
    cols["_parqit_xm_" + v] = pa.array(codes, pa.int8())
types = [("b", "byte"), ("i", "int"), ("l", "long"), ("f", "float"),
         ("d", "double"), ("s", "str1")]
xm = {v: "_parqit_xm_" + v for v in "bilfd"}
def write(path, cols, xm, types):
    t = pa.table(cols)
    t = t.replace_schema_metadata({b"parqit.schema": schema_json(types).encode(),
                                   b"parqit.xmissing": json.dumps(xm).encode()})
    pq.write_table(t, path)
write(Macro.getLocal("third"), cols, xm, types)
# D: a code outside 1-26 on row 3 of b
bad = dict(cols); bad["_parqit_xm_b"] = pa.array([0, 0, 27, 0, 0, 0, 0], pa.int8())
write(Macro.getLocal("bad27"), bad, xm, types)
# D: a code on a cell that holds a value (row 1 of d)
bad = dict(cols); bad["_parqit_xm_d"] = pa.array([2, 0, 1, 26, 13, 0, 0], pa.int8())
write(Macro.getLocal("badval"), bad, xm, types)
# D: a companion paired with the STRING column s
sc = dict(cols); sc["_parqit_xm_s"] = pa.array(codes, pa.int8())
xs = dict(xm); xs["s"] = "_parqit_xm_s"
write(Macro.getLocal("strcomp"), sc, xs, types)
end

log using `"`base'_A.log"', replace text name(v109A)
parqit use `"`third'"', clear
log close v109A
check "_N == 7 & c(k) == 6" "A: 7 obs, 6 variables (5 companions hidden)"
local fails = `fails' + r(bad)
check "b[3] == .a & b[4] == .z & b[5] == .m & b[2] == . & b[7] == . & b[1] == 1 & b[6] == 5" ///
    "A: byte .a/.z/.m restored, . kept, values kept"
local fails = `fails' + r(bad)
check "i[3] == .a & i[4] == .z & i[5] == .m & i[2] == . & i[1] == 100 & i[6] == 500" "A: int"
local fails = `fails' + r(bad)
check "l[3] == .a & l[4] == .z & l[5] == .m & l[7] == . & l[1] == 1000 & l[6] == 5000" "A: long"
local fails = `fails' + r(bad)
check "f[3] == .a & f[4] == .z & f[5] == .m & f[2] == . & f[1] == 1.5 & f[6] == 5.5" "A: float"
local fails = `fails' + r(bad)
check "d[3] == .a & d[4] == .z & d[5] == .m & d[2] == . & d[1] == 1.25 & d[6] == 5.25" "A: double"
local fails = `fails' + r(bad)
check `"("`: type b'" == "byte") & ("`: type i'" == "int") & ("`: type l'" == "long") & ("`: type f'" == "float") & ("`: type d'" == "double")"' ///
    "A: storage types are the file's (`: type b' `: type i' `: type l' `: type f' `: type d')"
local fails = `fails' + r(bad)
check `"s[3] == "c" & s[2] == "" & s[7] == """' "A: the string column is untouched"
local fails = `fails' + r(bad)
capture confirm variable _parqit_xm_b
check "_rc == 111" "A: no companion variable in memory"
local fails = `fails' + r(bad)
_v109_loghas `"`base'_A.log"' are preserved in companion columns for:
check "r(found) == 1" "A: the load says which variables carry companions"
local fails = `fails' + r(bad)

* =====================================================================
* B — projection, describe, appendin
* =====================================================================
parqit use d b using `"`third'"', clear
check "c(k) == 2 & d[4] == .z & b[5] == .m & d[6] == 5.25" "B: varlist projection restores the same cells"
local fails = `fails' + r(bad)
capture noisily parqit use _parqit_xm_b using `"`third'"', clear
check "_rc == 111" "B: a companion cannot be named in the varlist (rc `=_rc')"
local fails = `fails' + r(bad)
parqit describe `"`third'"'
check "r(n_cols) == 6 & r(n_rows) == 7" "B: describe counts 6 columns (r(n_cols)=`r(n_cols)')"
local fails = `fails' + r(bad)
clear
input byte b int i long l float f double d str1 s
9 9 9 9 9 "z"
end
parqit appendin using `"`third'"'
check "_N == 8 & b[1] == 9 & b[4] == .a & d[5] == .z & i[6] == .m & f[2] == 1.5" ///
    "B: appendin (native append) carries the restored cells"
local fails = `fails' + r(bad)

* =====================================================================
* C — the lazy view: hidden companions, . in the cells, the note
* =====================================================================
capture quietly parqit close _all
log using `"`base'_C.log"', replace text name(v109C)
parqit use using `"`third'"'
log close v109C
parqit collect, clear
capture quietly parqit close _all
capture confirm variable _parqit_xm_d
local no_comp = (_rc == 111)
check "c(k) == 6 & `no_comp' & d[3] == . & d[4] == . & b[5] == . & d[1] == 1.25 & d[6] == 5.25" ///
    "C: lazy collect: 6 variables, no companion, the extended cells are plain ."
local fails = `fails' + r(bad)
_v109_loghas `"`base'_C.log"' are not carried by a lazy view
check "r(found) == 1" "C: the lazy open says the companions are not carried"
local fails = `fails' + r(bad)

* =====================================================================
* D — corrupt or mismatched companions
* =====================================================================
clear
set obs 2
gen x = _n
capture noisily parqit use `"`bad27'"', clear
local rc27 = _rc
check "`rc27' != 0 & _N == 2 & x[2] == 2" "D: a code of 27 is refused (rc `rc27'), memory untouched"
local fails = `fails' + r(bad)
capture noisily parqit use `"`badval'"', clear
local rcval = _rc
check "`rcval' != 0 & _N == 2 & x[2] == 2" "D: a code on a non-missing cell is refused (rc `rcval'), memory untouched"
local fails = `fails' + r(bad)
log using `"`base'_D.log"', replace text name(v109D)
parqit use `"`strcomp'"', clear
log close v109D
capture confirm variable _parqit_xm_s
local no_scomp = (_rc == 111)
check `"c(k) == 6 & `no_scomp' & s[3] == "c" & b[3] == .a"' "D: a companion paired with a string column is hidden and ignored"
local fails = `fails' + r(bad)
_v109_loghas `"`base'_D.log"' is hidden and ignored
check "r(found) == 1" "D: ... and the load says so"
local fails = `fails' + r(bad)

* =====================================================================
* E — the save round trip, checked by pyarrow and bit-exact in Stata
* =====================================================================
local out `"`base'_out.parquet"'
local out2 `"`base'_out_staged.parquet"'
local out3 `"`base'_out_plain.parquet"'
clear
set obs 7
gen long id = _n
gen byte b = cond(_n == 1, 1, cond(_n == 6, 5, .))
gen int i = cond(_n == 1, 100, cond(_n == 6, 500, .))
gen long l = cond(_n == 1, 1000, cond(_n == 6, 5000, .))
gen float f = cond(_n == 1, 1.5, cond(_n == 6, 5.5, .))
gen double d = cond(_n == 1, 1.25, cond(_n == 6, 5.25, .))
gen double only_dot = cond(_n == 1, 7, .)
gen str3 s = cond(_n == 2, "", "s" + string(_n))
foreach v in b i l f d {
    quietly replace `v' = .a in 3
    quietly replace `v' = .z in 4
    quietly replace `v' = .m in 5
}
label variable d "double with codes"
quietly datasignature
local sig_orig "`r(datasignature)'"
tempfile orig
quietly save `"`orig'"'

log using `"`base'_E.log"', replace text name(v109E)
parqit save `"`out'"', replace data xmissing
local E_xm `"`r(xmissing_vars)'"'
log close v109E
check `""`E_xm'" == "b d f i l""' "E: r(xmissing_vars) lists the five preserved variables (got `E_xm')"
local fails = `fails' + r(bad)
_v109_loghas `"`base'_E.log"' were preserved in companion columns (xmissing)
check "r(found) == 1" "E: the save says the codes were preserved"
local fails = `fails' + r(bad)
_v109_loghas `"`base'_E.log"' were written as nulls
check "r(found) == 0" "E: ... and does not also call them lost"
local fails = `fails' + r(bad)

python:
import os, json, pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
def oracle(path):
    t = pq.read_table(path)
    md = t.schema.metadata or {}
    want = ["id", "b", "i", "l", "f", "d", "only_dot", "s"] + ["_parqit_xm_" + v for v in "bilfd"]
    ok_names = t.column_names == want
    ok_codes = all(t.column("_parqit_xm_" + v).to_pylist() == [0, 0, 1, 26, 13, 0, 0]
                   and str(t.schema.field("_parqit_xm_" + v).type) == "int8" for v in "bilfd")
    ok_prim = (t.column("b").to_pylist() == [1, None, None, None, None, 5, None]
               and t.column("d").to_pylist() == [1.25, None, None, None, None, 5.25, None]
               and t.column("only_dot").to_pylist() == [7.0] + [None] * 6
               and t.column("s").to_pylist() == ["s1", "", "s3", "s4", "s5", "s6", "s7"])
    xm = json.loads(md.get(b"parqit.xmissing", b"{}"))
    ok_meta = xm == {v: "_parqit_xm_" + v for v in "bilfd"}
    schema = json.loads(md.get(b"parqit.schema", b"{}"))
    ok_schema = [v["name"] for v in schema.get("vars", [])] == ["id", "b", "i", "l", "f", "d", "only_dot", "s"]
    return ok_names, ok_codes, ok_prim, ok_meta, ok_schema, t
r = oracle(Macro.getLocal("out"))
for k, v in zip(["E_names", "E_codes", "E_prim", "E_meta", "E_schema"], r[:5]):
    Macro.setLocal(k, "1" if v else "0")
end
check "`E_names' == 1" "E: pyarrow sees the 8 variables then exactly five companions, in variable order"
local fails = `fails' + r(bad)
check "`E_codes' == 1" "E: every companion is int8 with codes 0 0 1 26 13 0 0"
local fails = `fails' + r(bad)
check "`E_prim' == 1" "E: the primaries hold values and nulls only; only_dot and s untouched"
local fails = `fails' + r(bad)
check "`E_meta' == 1" "E: parqit.xmissing maps the five variables to their companions"
local fails = `fails' + r(bad)
check "`E_schema' == 1" "E: parqit.schema describes the 8 variables and no companion"
local fails = `fails' + r(bad)

parqit use `"`out'"', clear
quietly datasignature
local sig_back "`r(datasignature)'"
check `"("`sig_back'" == "`sig_orig'") & ("`sig_orig'" != "")"' "E: parqit use gives the original back (datasignature `sig_orig')"
local fails = `fails' + r(bad)
check "b[3] == .a & i[4] == .z & l[5] == .m & f[3] == .a & d[5] == .m & d[2] == . & only_dot[2] == . & d[6] == 5.25" ///
    "E: cells bit-exact, . stays ."
local fails = `fails' + r(bad)
check `"("`: variable label d'" == "double with codes") & ("`: type b'" == "byte") & ("`: type f'" == "float")"' ///
    "E: labels and storage types round-trip"
local fails = `fails' + r(bad)

* the staged writer writes the identical file
quietly use `"`orig'"', clear
python:
import os
os.environ["PARQIT_SAVE_NOARROW"] = "1"
end
parqit save `"`out2'"', replace data xmissing
python:
import os, pyarrow.parquet as pq
from sfi import Macro
os.environ.pop("PARQIT_SAVE_NOARROW", None)
a = pq.read_table(Macro.getLocal("out"))
b = pq.read_table(Macro.getLocal("out2"))
same = a.equals(b) and (a.schema.metadata or {}).get(b"parqit.xmissing") == (b.schema.metadata or {}).get(b"parqit.xmissing")
Macro.setLocal("E_staged", "1" if same else "0")
end
check "`E_staged' == 1" "E: the staged writer (PARQIT_SAVE_NOARROW) writes the same columns, codes and map"
local fails = `fails' + r(bad)

* without the option nothing changes on disk, and the note names the option
quietly use `"`orig'"', clear
log using `"`base'_E3.log"', replace text name(v109E3)
parqit save `"`out3'"', replace data
log close v109E3
python:
import pyarrow.parquet as pq
from sfi import Macro
t = pq.read_table(Macro.getLocal("out3"))
ok = (not any(c.startswith("_parqit_xm_") for c in t.column_names)
      and b"parqit.xmissing" not in (t.schema.metadata or {})
      and t.column("b").to_pylist() == [1, None, None, None, None, 5, None])
Macro.setLocal("E_plain", "1" if ok else "0")
end
check "`E_plain' == 1" "E: without xmissing: no companion, no key, nulls as before"
local fails = `fails' + r(bad)
_v109_loghas `"`base'_E3.log"' were written as nulls
check "r(found) == 1" "E: ... the loss is still announced"
local fails = `fails' + r(bad)
_v109_loghas `"`base'_E3.log"' xmissing
check "r(found) == 1" "E: ... and the note names the option"
local fails = `fails' + r(bad)

* =====================================================================
* F — partitioned tree, 0 rows, a reserved name, copysource, the view
* =====================================================================
local tree `"`base'_tree"'
quietly use `"`orig'"', clear
gen byte g = cond(_n <= 3, 1, 2)
parqit save `"`tree'"', replace data xmissing partition_by(g)
parqit use `"`tree'"', clear
sort id
check "_N == 7 & b[3] == .a & d[4] == .z & i[5] == .m & f[1] == 1.5 & d[6] == 5.25" ///
    "F: a partitioned tree carries the companions and the directory read restores them"
local fails = `fails' + r(bad)

clear
set obs 0
gen double d = .
gen byte b = .
parqit save `"`base'_zero.parquet"', replace data xmissing
parqit use `"`base'_zero.parquet"', clear
check "_N == 0 & c(k) == 2" "F: a 0-row save with xmissing round-trips"
local fails = `fails' + r(bad)

quietly use `"`orig'"', clear
gen double _parqit_xm_d = 1
capture noisily parqit save `"`base'_clash.parquet"', replace data xmissing
check "_rc == 198" "F: a variable named like a companion is refused (rc `=_rc')"
local fails = `fails' + r(bad)
capture confirm file `"`base'_clash.parquet"'
check "_rc != 0" "F: ... and nothing was written"
local fails = `fails' + r(bad)

parqit use `"`out'"', clear
capture noisily parqit save `"`base'_copy.parquet"', replace copysource
check "_rc == 198" "F: copysource of a file with companions is refused (rc `=_rc')"
local fails = `fails' + r(bad)
capture noisily parqit save `"`base'_copy.parquet"', replace copysource xmissing
check "_rc == 198" "F: xmissing with copysource is refused (rc `=_rc')"
local fails = `fails' + r(bad)
capture noisily parqit save `"`base'_copy.parquet"', replace data xmissing partition_by(id) partitions(append)
check "_rc == 198" "F: xmissing with partitions(append) is refused (rc `=_rc')"
local fails = `fails' + r(bad)

capture quietly parqit close _all
parqit use using `"`out'"'
capture noisily parqit save `"`base'_viewsave.parquet"', replace xmissing
check "_rc == 198" "F: xmissing on a view save is refused (rc `=_rc')"
local fails = `fails' + r(bad)
parqit save `"`base'_viewsave.parquet"', replace
capture quietly parqit close _all
python:
import pyarrow.parquet as pq
from sfi import Macro
t = pq.read_table(Macro.getLocal("base") + "_viewsave.parquet")
ok = (not any(c.startswith("_parqit_xm_") for c in t.column_names)
      and t.column("b").to_pylist() == [1, None, None, None, None, 5, None])
Macro.setLocal("F_view", "1" if ok else "0")
end
check "`F_view' == 1" "F: a save from the lazy view writes no companion and plain nulls (announced at open)"
local fails = `fails' + r(bad)

if (`fails' == 0) di "VERDICT(V109_XMISSING_ROUNDTRIP): PASS - extended missings preserved by save (both writers, pyarrow-checked) and restored by use/appendin for every numeric type; lazy view folds and says so; corrupt companions, reserved names, copysource and partial trees refused"
else di "VERDICT(V109_XMISSING_ROUNDTRIP): FAIL - `fails' check(s)"
