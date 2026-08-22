* V74 — type/value fidelity fixes of the 2026-08-22 audit (A1):
*   A1-3  a float/double/%tc partition key is restored to its recorded type on
*         eager use, lazy collect and view save (it came back as a STRING);
*   A1-8  partition-key columns keep the manifest's variable order on read;
*   A1-5  a zero-observation partition_by() save writes an empty tree that reads
*         back as 0 obs with every variable (not a raw engine error);
*   A1-4  a variable named `str` keeps its name;
*   A1-10 value labels not attached to any variable are written and restored;
*   A1-1  %tc instants beyond year 4253 are exact on disk (us == ms*1000) and
*         round-trip on the Arrow, staged and lazy writers (pyarrow oracle);
*   A1-7  exact integer counts >= 2^52 are neither bumped nor reported fractional;
*   A1-9  nanosecond instants before 1970 floor toward -infinity at the ms;
*   A1-6  int/byte %td restore as int on both eager and lazy paths.
* (python: blocks stay at top level — Stata's batch parser breaks on them inside
* loops/programs.)
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

tempfile stem
local twin `"`stem'_twin.dta"'

* ---------- A/B. partition keys of every type, manifest order, zero rows -----
program define _v74_build
    clear
    set obs 12
    gen long id = _n
    gen float fy = 2019 + mod(_n, 3)
    gen double dk = cond(mod(_n, 2), 0.1, 2.5)
    gen double tck = tc(01jan2020 00:00:00) + mod(_n, 2) * 3600000
    format tck %tc
    gen int dd = td(01jan2020) + mod(_n, 2)
    format dd %td
    gen int mk = tm(2020m1) + mod(_n, 2)
    format mk %tm
    gen byte bk = mod(_n, 2)
    gen str3 sk = cond(mod(_n, 2), "a b", "x=y")
    gen double pay = _n * 1.5
    label var pay "payload"
end
program define _v74_typesig, rclass
    local sig
    foreach v of varlist _all {
        local sig "`sig' `v':`: type `v'':`: format `v''"
    }
    return local sig "`sig'"
end
_v74_build
_v74_typesig
local want `"`r(sig)'"'
qui ds
local wantorder "`r(varlist)'"
qui save `"`twin'"', replace
local fails 0
foreach key in fy dk tck dd mk bk sk {
    tempfile tree
    use `"`twin'"', clear
    parqit save `"`tree'_`key'"', replace data partition_by(`key')
    * eager
    parqit use using `"`tree'_`key'"', clear
    sort id
    qui ds
    if ("`r(varlist)'" != "`wantorder'") {
        di as err "FAIL eager order key=`key': `r(varlist)'"
        local ++fails
    }
    _v74_typesig
    if (`"`r(sig)'"' != `"`want'"') {
        di as err `"FAIL eager types key=`key': `r(sig)'"'
        local ++fails
    }
    capture cf _all using `"`twin'"'
    if (_rc) {
        di as err "FAIL eager values key=`key'"
        local ++fails
    }
    * lazy collect
    clear
    parqit use using `"`tree'_`key'"'
    parqit collect, clear
    sort id
    qui ds
    if ("`r(varlist)'" != "`wantorder'") {
        di as err "FAIL lazy order key=`key': `r(varlist)'"
        local ++fails
    }
    _v74_typesig
    if (`"`r(sig)'"' != `"`want'"') {
        di as err `"FAIL lazy types key=`key': `r(sig)'"'
        local ++fails
    }
    capture cf _all using `"`twin'"'
    if (_rc) {
        di as err "FAIL lazy values key=`key'"
        local ++fails
    }
    * view save -> eager read
    parqit use using `"`tree'_`key'"'
    parqit save `"`tree'_`key'_vs.parquet"', replace
    parqit close
    parqit use using `"`tree'_`key'_vs.parquet"', clear
    sort id
    _v74_typesig
    if (`"`r(sig)'"' != `"`want'"') {
        di as err `"FAIL viewsave types key=`key': `r(sig)'"'
        local ++fails
    }
    capture cf _all using `"`twin'"'
    if (_rc) {
        di as err "FAIL viewsave values key=`key'"
        local ++fails
    }
}
* a missing key partitions into the default partition and restores as missing
use `"`twin'"', clear
replace bk = . in 1
tempfile mtree
parqit save `"`mtree'"', replace data partition_by(bk)
parqit use using `"`mtree'"', clear
sort id
assert bk[1] == . & bk[2] == 0
assert "`: type bk'" == "byte"

* zero observations: an empty tree that reads back with every variable
clear
set obs 0
gen byte g = .
gen double x = .
gen str5 s = ""
label var x "x label"
tempfile ztree
parqit save `"`ztree'"', replace data partition_by(g)
assert r(N) == 0 & r(k) == 3
parqit use using `"`ztree'"', clear
assert _N == 0 & c(k) == 3
qui ds
assert "`r(varlist)'" == "g x s"
assert `"`: var label x'"' == "x label"
clear
parqit use using `"`ztree'"'
parqit collect, clear
assert _N == 0 & c(k) == 3
parqit close _all

* ---------- C/D. `str` survives; unattached value labels travel -------------
clear
set obs 3
gen int str = _n
gen byte g = 1
label define orphan 7 "orphan seven" 8 "orphan eight"
label define used 1 "one"
label values g used
tempfile sf
parqit save `"`sf'.parquet"', replace
parqit use using `"`sf'.parquet"', clear
qui ds
assert "`r(varlist)'" == "str g"
assert str[2] == 2
assert `"`: label orphan 7'"' == "orphan seven"
assert `"`: label orphan 8'"' == "orphan eight"
assert `"`: label used 1'"' == "one"
clear
parqit use using `"`sf'.parquet"'
parqit collect, clear
assert `"`: label orphan 7'"' == "orphan seven"
python:
from sfi import Macro
import pyarrow.parquet as pq, json
m = pq.read_schema(Macro.getLocal("sf") + ".parquet").metadata
vl = json.loads(m[b"parqit.vallabs"])
Macro.setLocal("okvl", "1" if "orphan" in vl and vl["orphan"]["entries"][1][1] == "orphan eight" else "0")
end
assert "`okvl'" == "1"
* VALLAB-ALL-1 (V2.4): a VIEW save writes every definition too — attached or
* not, with or without a verb in the pipeline — and a read restores them
clear
parqit use using `"`sf'.parquet"'
parqit save `"`sf'_vs.parquet"', replace
parqit use using `"`sf'.parquet"'
parqit gen double z = g + 1
parqit save `"`sf'_vs2.parquet"', replace
parqit close _all
parqit use using `"`sf'_vs.parquet"', clear
assert `"`: label orphan 7'"' == "orphan seven" & `"`: label used 1'"' == "one"
parqit use using `"`sf'_vs2.parquet"', clear
assert `"`: label orphan 8'"' == "orphan eight" & `"`: label used 1'"' == "one"
python:
from sfi import Macro
import pyarrow.parquet as pq, json
ok = True
for tag in ("_vs", "_vs2"):
    vl = json.loads(pq.read_schema(Macro.getLocal("sf") + tag + ".parquet").metadata[b"parqit.vallabs"])
    ok = ok and sorted(vl.keys()) == ["orphan", "used"] and vl["orphan"]["entries"][0][1] == "orphan seven"
Macro.setLocal("okvl2", "1" if ok else "0")
end
assert "`okvl2'" == "1"

* ---------- E/F. %tc beyond year 4253 exact on disk; 2^52+1 exact ------------
parqit close _all
clear
set seed 20260822
set obs 20000
gen double tc = tc(01jan4253 00:00:00) + floor(runiform() * (tc(31dec9999 23:59:59) - tc(01jan4253 00:00:00)))
replace tc = tc(31dec9999 23:59:59.999) in 1
replace tc = tc(01jan5000 12:00:00.001) in 2
replace tc = 4503599627370497 in 3                  // 2^52 + 1: an exact odd integer
format tc %tc
gen double td = td(31dec9999)
gen double tC = 4503599627370497                    // 2^52 + 1 as a %tC count (out of the %td window)
format td %td
format tC %tC
gen long i = _n
tempfile tcf
qui save `"`tcf'.dta"', replace
python:
import os
os.environ.pop("PARQIT_SAVE_NOARROW", None)
end
parqit save `"`tcf'_arrow.parquet"', replace data
assert "`r(frac_dates)'" == ""
python:
import os
os.environ["PARQIT_SAVE_NOARROW"] = "1"
end
parqit save `"`tcf'_staged.parquet"', replace data
assert "`r(frac_dates)'" == ""
python:
import os
os.environ.pop("PARQIT_SAVE_NOARROW", None)
end
parqit open _data
parqit save `"`tcf'_lazy.parquet"', replace
assert "`r(frac_dates)'" == ""
parqit close
python:
from sfi import Macro, Data
import pyarrow.parquet as pq, pyarrow.compute as pc, pyarrow as pa
shift = 3653 * 86400000
stata_ms = [int(v) for v in Data.get("tc")]
ok = True
for tag in ("arrow", "staged", "lazy"):
    t = pq.read_table(Macro.getLocal("tcf") + "_" + tag + ".parquet")
    us = pc.cast(t.column("tc"), pa.int64()).to_pylist()       # timestamp[us] -> epoch us
    ok = ok and all(u == (m - shift) * 1000 for u, m in zip(us, stata_ms))
    ok = ok and pc.cast(t.column("tC"), pa.int64()).to_pylist()[2] == 4503599627370497
Macro.setLocal("oktc", "1" if ok else "0")
end
assert "`oktc'" == "1"
foreach tag in arrow staged lazy {
    parqit use using `"`tcf'_`tag'.parquet"', clear
    sort i
    capture cf tc td tC using `"`tcf'.dta"'
    if (_rc) {
        di as err "FAIL %tc round-trip (`tag')"
        local ++fails
    }
}

* ---------- G. nanosecond instants before 1970 floor toward -infinity --------
tempfile nsf
python:
from sfi import Macro
import pyarrow as pa, pyarrow.parquet as pq
t = pa.table({"ns": pa.array([-1, -999, -1000, -1001, 1, -1000000, -1000001], pa.timestamp("ns"))})
pq.write_table(t, Macro.getLocal("nsf") + ".parquet")
end
local shift = 3653 * 86400000
parqit use using `"`nsf'.parquet"', clear
assert ns[1] == `shift' - 1 & ns[2] == `shift' - 1 & ns[3] == `shift' - 1 & ns[4] == `shift' - 1
assert ns[5] == `shift' & ns[6] == `shift' - 1 & ns[7] == `shift' - 2
clear
parqit use using `"`nsf'.parquet"'
parqit collect, clear
assert ns[1] == `shift' - 1 & ns[4] == `shift' - 1 & ns[5] == `shift' & ns[7] == `shift' - 2

* ---------- H. int/byte %td restore as int on both paths (byte keeps int by
* the documented period/date rule); float %tc stays double on both -----------
parqit close _all
clear
set obs 5
gen int di = td(01jan2020) + _n
gen byte db = _n
format di db %td
gen float tf = _n * 1000
format tf %tc
tempfile tdf
parqit save `"`tdf'.parquet"', replace data
parqit use using `"`tdf'.parquet"', clear
local eager "`: type di' `: type db' `: type tf'"
clear
parqit use using `"`tdf'.parquet"'
parqit collect, clear
local lazy "`: type di' `: type db' `: type tf'"
assert "`eager'" == "`lazy'"
assert "`eager'" == "int int float"
* FLOAT-EXACT-1 (V2.3): a float %tc whose ms range exceeds long (range sizing
* alone says double) comes back float on eager, lazy-direct, lazy-with-verb and
* view-save reads because a scan proves every value float32-exact; a foreign
* file whose manifest claims float over values that do NOT fit comes back double
clear
set obs 3
gen float tcb = 1.8e12
replace tcb = 123 in 2
replace tcb = -5e11 in 3
format tcb %tc
gen float chk = tcb
tempfile fb
parqit save `"`fb'.parquet"', replace data
parqit use using `"`fb'.parquet"', clear
assert "`: type tcb'" == "float"
assert tcb == chk
clear
parqit use using `"`fb'.parquet"'
parqit collect, clear
assert "`: type tcb'" == "float"
assert tcb == chk
parqit use using `"`fb'.parquet"'
parqit gen double z = 1
parqit collect, clear
assert "`: type tcb'" == "float"
assert tcb == chk
parqit use using `"`fb'.parquet"'
parqit save `"`fb'_vs.parquet"', replace
parqit close _all
parqit use using `"`fb'_vs.parquet"', clear
assert "`: type tcb'" == "float"
assert tcb == chk
python:
from sfi import Macro
import pyarrow as pa, pyarrow.parquet as pq, json
sch = {"version": 1, "vars": [{"name": "tcf", "src": "tcf", "type": "float", "fmt": "%tc", "varlab": "", "vallab": ""}], "sortedby": []}
t = pa.Table.from_arrays([pa.array([1800000000001000, 1800000000003000], pa.timestamp("us"))], names=["tcf"])
t = t.replace_schema_metadata({"parqit.schema": json.dumps(sch)})
pq.write_table(t, Macro.getLocal("fb") + "_notexact.parquet")
end
parqit use using `"`fb'_notexact.parquet"', clear
assert "`: type tcf'" == "double"
assert tcf[2] - tcf[1] == 2
clear
parqit use using `"`fb'_notexact.parquet"'
parqit collect, clear
assert "`: type tcf'" == "double"
parqit close _all

if (`fails') {
    di as err "VERDICT(V74_TYPE_FIDELITY_PARTITION): FAIL - `fails' check(s) failed"
    exit 9
}
di "VERDICT(V74_TYPE_FIDELITY_PARTITION): PASS - partition keys of every type restore their recorded type and order on eager/lazy/view-save reads, empty trees read back, str survives, orphan value labels travel (memory and view save), %tc exact beyond year 4253 on all writers (pyarrow us == ms*1000), 2^52+1 exact, ns floors, int %td parity, float %tc restored float when every value fits"
