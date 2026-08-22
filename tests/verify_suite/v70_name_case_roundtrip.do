* NAME-CASE-1 (2026-08-22): Stata variable names are case-sensitive — `nuemp`
* and `NUEMP` are two variables — but DuckDB identifiers are not, and its COPY
* dedups the second to `NUEMP_1` in the written file, while read_parquet dedups
* it in the scan. parqit must keep the names EXACT at both boundaries: a save
* from memory writes `nuemp` and `NUEMP` into the file (footer rename after the
* engine's COPY, pyarrow-confirmed), `parqit use ..., clear`, `parqit collect`
* and a view save restore them, labels/formats follow the right variable, and
* inside a lazy view the clashing column is addressed by a documented alias
* (`NUEMP_1`) that collect/save translate back. Names that would clash
* case-insensitively when CREATED lazily are refused loudly (DuckDB would bind
* later references to the wrong column silently).
* (python: blocks stay at top level — Stata's batch parser breaks on them
* inside foreach — so the two writer passes are unrolled around helper programs.)
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

program define _v70_build
    clear
    set obs 4
    gen long nuemp = _n
    gen long NUEMP = _n * 10
    gen str3 caem1L_EMP_QP = "U" + string(_n)
    gen str3 caem1l_EMP_QP = "l" + string(_n)
    gen double x = _n / 2
    label var nuemp "lower"
    label var NUEMP "Upper"
    format NUEMP %12.0f
    label define up 10 "ten"
    label values NUEMP up
    char NUEMP[src] "qp"
end

* asserts on the dataset read back (eager use or collect)
program define _v70_check_full
    qui ds
    assert "`r(varlist)'" == "nuemp NUEMP caem1L_EMP_QP caem1l_EMP_QP x"
    assert NUEMP[2] == 20 & nuemp[2] == 2 & NUEMP[4] == 40
    assert `"`: var label NUEMP'"' == "Upper" & `"`: var label nuemp'"' == "lower"
    assert "`: format NUEMP'" == "%12.0f"
    assert `"`: label (NUEMP) 10'"' == "ten"
    assert `"`: char NUEMP[src]'"' == "qp"
    assert caem1l_EMP_QP[3] == "l3" & caem1L_EMP_QP[3] == "U3"
end

* one writer pass: save from memory, eager read, lazy read, alias addressing,
* view save; the pyarrow oracles run at top level afterwards on `f' and `gf'
program define _v70_pass
    args f gf
    _v70_build
    parqit save `"`f'"', replace
    assert r(N) == 4 & r(k) == 5
    * eager read
    parqit use using `"`f'"', clear
    _v70_check_full
    * lazy open + collect
    clear
    parqit use using `"`f'"'
    parqit describe
    parqit collect, clear
    _v70_check_full
    * lazy verbs address the clashing column by its alias; expressions too
    parqit use using `"`f'"'
    parqit keep nuemp NUEMP_1
    parqit gen z = NUEMP_1 + nuemp
    parqit collect, clear
    qui ds
    assert "`r(varlist)'" == "nuemp NUEMP z"
    assert z[3] == 33
    * view save writes the exact names too, and reads back
    parqit use using `"`f'"'
    parqit keep NUEMP_1 caem1l_EMP_QP_1 x
    parqit save `"`gf'"', replace
    parqit use using `"`gf'"', clear
    qui ds
    assert "`r(varlist)'" == "NUEMP caem1l_EMP_QP x"
    assert NUEMP[1] == 10 & `"`: var label NUEMP'"' == "Upper"
    parqit close
end

* ---------- A1. Arrow writer -------------------------------------------------
python:
import os
os.environ.pop("PARQIT_SAVE_NOARROW", None)
end
tempfile ta
_v70_pass `"`ta'_a.parquet"' `"`ta'_ga.parquet"'
python:
from sfi import Macro
import pyarrow.parquet as pq, json
t = pq.read_table(Macro.getLocal("ta") + "_a.parquet")
ok = t.schema.names == ["nuemp", "NUEMP", "caem1L_EMP_QP", "caem1l_EMP_QP", "x"]
ok = ok and t.column("NUEMP").to_pylist() == [10, 20, 30, 40]
ok = ok and t.column("caem1l_EMP_QP").to_pylist() == ["l1", "l2", "l3", "l4"]
sch = json.loads(t.schema.metadata[b"parqit.schema"])
ok = ok and [v["name"] for v in sch["vars"]] == t.schema.names
ok = ok and [v["src"] for v in sch["vars"]] == t.schema.names
ok = ok and [v["varlab"] for v in sch["vars"] if v["name"] == "NUEMP"][0] == "Upper"
g = pq.read_table(Macro.getLocal("ta") + "_ga.parquet")
ok = ok and g.schema.names == ["NUEMP", "caem1l_EMP_QP", "x"]
ok = ok and g.column("NUEMP").to_pylist() == [10, 20, 30, 40]
gs = json.loads(g.schema.metadata[b"parqit.schema"])
ok = ok and [v["name"] for v in gs["vars"]] == ["NUEMP", "caem1l_EMP_QP", "x"]
Macro.setLocal("oracle_ok", "1" if ok else "0")
end
assert "`oracle_ok'" == "1"

* ---------- A2. staged writer ------------------------------------------------
python:
import os
os.environ["PARQIT_SAVE_NOARROW"] = "1"
end
tempfile tsd
_v70_pass `"`tsd'_s.parquet"' `"`tsd'_gs.parquet"'
python:
from sfi import Macro
import pyarrow.parquet as pq
t = pq.read_table(Macro.getLocal("tsd") + "_s.parquet")
ok = t.schema.names == ["nuemp", "NUEMP", "caem1L_EMP_QP", "caem1l_EMP_QP", "x"]
ok = ok and t.column("NUEMP").to_pylist() == [10, 20, 30, 40]
g = pq.read_schema(Macro.getLocal("tsd") + "_gs.parquet")
ok = ok and g.names == ["NUEMP", "caem1l_EMP_QP", "x"]
Macro.setLocal("oracle_ok", "1" if ok else "0")
import os
os.environ.pop("PARQIT_SAVE_NOARROW", None)
end
assert "`oracle_ok'" == "1"

* ---------- B. a foreign (pq/Polars-style) file with case-distinct names ------
tempfile pb
local pf `"`pb'.parquet"'
python:
from sfi import Macro
import pyarrow as pa, pyarrow.parquet as pq
t = pa.table({"nuemp": [1, 2, 3], "NUEMP": [10, 20, 30],
              "reg_reforma": ["a", "b", "c"], "REG_REFORMA": ["A", "B", "C"]})
pq.write_table(t, Macro.getLocal("pf"))
end
parqit use using `"`pf'"', clear
qui ds
assert "`r(varlist)'" == "nuemp NUEMP reg_reforma REG_REFORMA"
assert NUEMP[3] == 30 & nuemp[3] == 3 & REG_REFORMA[2] == "B" & reg_reforma[2] == "b"
clear
parqit use using `"`pf'"'
parqit collect, clear
qui ds
assert "`r(varlist)'" == "nuemp NUEMP reg_reforma REG_REFORMA"
assert NUEMP[1] == 10 & REG_REFORMA[1] == "A"
* and a parqit re-save of that foreign file keeps the names exact
tempfile pc
local pcf `"`pc'.parquet"'
parqit save `"`pcf'"', replace
python:
from sfi import Macro
import pyarrow.parquet as pq
Macro.setLocal("names_ok", "1" if pq.read_schema(Macro.getLocal("pcf")).names ==
               ["nuemp", "NUEMP", "reg_reforma", "REG_REFORMA"] else "0")
end
assert "`names_ok'" == "1"

* ---------- C. lazy creation of a case-clashing name is refused loudly -------
parqit use using `"`pf'"'
capture noisily parqit gen Nuemp = 1
assert _rc != 0
capture noisily parqit rename reg_reforma NUEMP
assert _rc != 0
capture noisily parqit egen Reg_reforma = total(nuemp)
assert _rc != 0
* the view is intact after the refusals
parqit collect, clear
qui ds
assert "`r(varlist)'" == "nuemp NUEMP reg_reforma REG_REFORMA"
parqit close

* ---------- D. raw SQL results and merge using-sides with case-distinct names
parqit sql "SELECT 1 AS nuemp, 2 AS NUEMP, 3 AS key"
parqit collect, clear
qui ds
assert "`r(varlist)'" == "nuemp NUEMP key"
assert nuemp[1] == 1 & NUEMP[1] == 2 & key[1] == 3
parqit close

clear
set obs 3
gen long nuemp = _n
gen str1 m = "m"
tempfile mb
local mf `"`mb'.parquet"'
parqit save `"`mf'"', replace
parqit use using `"`mf'"'
parqit merge 1:1 nuemp using `"`pf'"'
parqit collect, clear
assert _N == 3
qui ds
assert strpos("`r(varlist)'", "NUEMP") > 0 & strpos("`r(varlist)'", "REG_REFORMA") > 0
assert NUEMP[2] == 20 & REG_REFORMA[3] == "C" & reg_reforma[1] == "a"
parqit close

* ---------- E. partition_by() is refused for case-clashing names -------------
clear
set obs 2
gen long nuemp = _n
gen long NUEMP = _n * 10
gen int year = 2000 + _n
tempfile hb
capture noisily parqit save `"`hb'_tree"', replace partition_by(year)
assert _rc != 0

* ---------- F. relaxed union (A2-2 / V2.2): exact names recovered; the engine's
* case-insensitive union hazards are loud -------------------------------------
* A union_by_name scan lists the first file's (reader-deduped) columns and then
* every later file's new ones, matched CASE-INSENSITIVELY; parqit predicts that
* union exactly and restores the true names (NUEMP, not NUEMP_1) with their
* metadata; a later file's case-variant unioned into an earlier column is
* noted; a name the union would split across two columns is refused.
tempfile rb
local rd `"`rb'_rel"'
mkdir `"`rd'"'
foreach d in rel rel2 xw soft {
    mkdir `"`rd'/`d'"'
}
python:
from sfi import Macro
import pyarrow as pa, pyarrow.parquet as pq, json
rd = Macro.getLocal("rd")
def w(name, names, base=0, meta=None):
    cols = [pa.array([base + i*10 + j for j in range(2)], pa.int32()) for i in range(len(names))]
    t = pa.Table.from_arrays(cols, names=names)
    if meta: t = t.replace_schema_metadata({k: json.dumps(v) for k, v in meta.items()})
    pq.write_table(t, name)
sch = {"version": 1, "vars": [
    {"name": "nuemp", "src": "nuemp", "type": "long", "fmt": "%12.0g", "varlab": "lower", "vallab": ""},
    {"name": "NUEMP", "src": "NUEMP", "type": "long", "fmt": "%12.0f", "varlab": "Upper", "vallab": ""},
    {"name": "s", "src": "s", "type": "long", "fmt": "%8.0g", "varlab": "", "vallab": ""},
    {"name": "extra", "src": "extra", "type": "long", "fmt": "%8.0g", "varlab": "", "vallab": ""}], "sortedby": []}
w(rd + "/rel/a.parquet", ["nuemp", "NUEMP", "s"], meta={"parqit.schema": sch})
w(rd + "/rel/b.parquet", ["nuemp", "NUEMP", "s", "extra"], base=100, meta={"parqit.schema": sch})
w(rd + "/rel2/a.parquet", ["nuemp", "s"])
w(rd + "/rel2/b.parquet", ["nuemp", "NUEMP", "s"], base=100)
w(rd + "/xw/a.parquet", ["nuemp", "NUEMP"])
w(rd + "/xw/b.parquet", ["NUEMP"], base=100)
w(rd + "/soft/a.parquet", ["nuemp", "s"])
w(rd + "/soft/b.parquet", ["NUEMP", "s"], base=100)
end
* a differing schema (extra column in the 2nd file): exact names + metadata
parqit use using `"`rd'/rel/*.parquet"', clear relaxed
qui ds
assert "`r(varlist)'" == "nuemp NUEMP s extra"
assert `"`: var label NUEMP'"' == "Upper" & "`: format NUEMP'" == "%12.0f"
sort nuemp
assert _N == 4 & NUEMP[1] == 10 & NUEMP[3] == 110 & extra[1] == . & extra[3] == 130
clear
parqit use using `"`rd'/rel/*.parquet"', relaxed
parqit describe
parqit collect, clear
qui ds
assert "`r(varlist)'" == "nuemp NUEMP s extra"
assert `"`: var label NUEMP'"' == "Upper"
parqit use using `"`rd'/rel/*.parquet"', relaxed
parqit save `"`rd'_rel_vs.parquet"', replace
parqit close
python:
from sfi import Macro
import pyarrow.parquet as pq
Macro.setLocal("relvs_ok", "1" if pq.read_schema(Macro.getLocal("rd") + "_rel_vs.parquet").names ==
               ["nuemp", "NUEMP", "s", "extra"] else "0")
end
assert "`relvs_ok'" == "1"
* the clash only in the 2nd file: the new column keeps its exact name
parqit use using `"`rd'/rel2/*.parquet"', clear relaxed
qui ds
assert "`r(varlist)'" == "nuemp s NUEMP"
sort nuemp
assert NUEMP[1] == . & NUEMP[3] == 110
* cross-wiring (a: nuemp NUEMP; b: NUEMP -> the engine would pour b's NUEMP
* into nuemp while a's NUEMP is its own column): refused on both paths
capture noisily parqit use using `"`rd'/xw/*.parquet"', clear relaxed
assert _rc == 198
capture noisily parqit use using `"`rd'/xw/*.parquet"', relaxed
assert _rc == 198
* a plain case-variant across files (a: nuemp; b: NUEMP) is unioned, with a note
tempfile v70log
log using `"`v70log'.log"', replace name(v70soft) text
parqit use using `"`rd'/soft/*.parquet"', clear relaxed
log close v70soft
qui ds
assert "`r(varlist)'" == "nuemp s"
assert _N == 4
python:
from sfi import Macro
txt = open(Macro.getLocal("v70log") + ".log", encoding="utf-8", errors="replace").read()
Macro.setLocal("soft_note", "1" if 'column "NUEMP"' in txt and "unioned into" in txt else "0")
end
assert "`soft_note'" == "1"

* ---------- G. an empty column name becomes v<position> (A2-15(1)) ------------
tempfile eb
local ef `"`eb'_empty.parquet"'
python:
from sfi import Macro
import pyarrow as pa, pyarrow.parquet as pq
t = pa.Table.from_arrays([pa.array([1, 2], pa.int32()), pa.array([3, 4], pa.int32()),
                          pa.array([5, 6], pa.int32())], names=["a", "", "c"])
pq.write_table(t, Macro.getLocal("ef"))
end
parqit use using `"`ef'"', clear
qui ds
assert "`r(varlist)'" == "a v2 c"
assert v2[1] == 3 & v2[2] == 4 & `"`: char v2[src_name]'"' == ""
clear
parqit use using `"`ef'"'
parqit collect, clear
qui ds
assert "`r(varlist)'" == "a v2 c"
assert v2[2] == 4
parqit close

* ---------- H. a Hive key clashing only by case with a file column is refused
* (V2.6); a key that exactly duplicates a file column is noted and read -------
tempfile hb
local hd `"`hb'_h"'
mkdir `"`hd'"'
python:
from sfi import Macro
import pyarrow as pa, pyarrow.parquet as pq, pyarrow.dataset as ds, os
hd = Macro.getLocal("hd")
t2 = pa.table({"G": pa.array([1, 2, 3, 4], pa.int32()), "x": pa.array([10, 20, 30, 40], pa.int32()),
               "g": pa.array([1, 1, 2, 2], pa.int32())})
ds.write_dataset(t2, hd + "/hive_clash", format="parquet",
                 partitioning=ds.partitioning(pa.schema([("g", pa.int32())]), flavor="hive"))
os.makedirs(hd + "/hive_same/g=1"); os.makedirs(hd + "/hive_same/g=2")
pq.write_table(pa.table({"g": pa.array([1, 1], pa.int32()), "x": pa.array([10, 20], pa.int32())}),
               hd + "/hive_same/g=1/p.parquet")
pq.write_table(pa.table({"g": pa.array([2, 2], pa.int32()), "x": pa.array([30, 40], pa.int32())}),
               hd + "/hive_same/g=2/p.parquet")
end
capture noisily parqit use using `"`hd'/hive_clash"', clear
assert _rc == 198
capture noisily parqit use using `"`hd'/hive_clash"'
assert _rc == 198
capture noisily parqit describe `"`hd'/hive_clash"'
assert _rc == 198
capture noisily parqit use using `"`hd'/hive_clash/**/*.parquet"', clear
assert _rc == 198
parqit use using `"`hd'/hive_same"', clear
sort x
assert _N == 4 & g[1] == 1 & g[4] == 2 & x[4] == 40
parqit close _all

di "VERDICT(V70_NAME_CASE_ROUNDTRIP): PASS - case-distinct names exact in the " ///
   "written file (pyarrow), on eager use, collect and view save, both writers; " ///
   "labels/formats/chars follow; lazy alias addressing works; clashing lazy " ///
   "names refused; foreign files, sql and merge using-sides covered; relaxed " ///
   "union recovers exact names (cross-wiring refused, case merge noted); empty " ///
   "name -> v<position>; Hive key case clash refused"
