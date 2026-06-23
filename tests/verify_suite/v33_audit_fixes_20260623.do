* V33 — adversarial-audit fixes (2026-06-23, multi-agent residual round). Each
* invariant has an independent oracle (native Stata twin or pyarrow), never a
* parqit-only round-trip.
*   EXPR-1   gen byte/int/long truncates toward zero and maps out-of-range to
*            missing (gen byte=3.9->3, =200->. ); gen float rounds to float32
*   EXPR-4   chained relational comparisons (1<x<3000) parse left-associatively
*   TT-A2    append generate() name colliding with a using column errors loudly
*   TT-A1    m:m master pairing is reproducible for fixed inputs (no engine drift)
*   COLLAPSE-WEIGHTS  collapse with a weight expression is a clear, loud error
*   COLLAPSE-3        collapse (first) without a sort is deterministic/reproducible
*   RESHAPE-WIDE-COLORDER  numeric j orders columns numerically (inc2 before inc10)
*   TAB-FLOAT-1       tabulate of an integer-valued float prints 11, not 11.0
*   GLOB-1a   save verify-read escapes glob metachars in the staged path
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

local fails 0
tempfile t

* ---- EXPR-1 : narrow storage type truncates + range->missing --------------
clear
input double base
0
end
parqit save `"`t'_e1.parquet"', replace data
parqit use using `"`t'_e1.parquet"'
parqit gen byte  b1   = 3.9
parqit gen byte  bneg = -2.5
parqit gen int   i1   = 2.5
parqit gen byte  b2   = 200
parqit gen byte  b101 = 101
parqit gen long  l1   = 2.9
parqit gen float f1   = 1/3
parqit collect, clear
local tb : type b1
local ti : type i1
local tl : type l1
local tf : type f1
* values match native Stata's gen <type> coercion (verified against Stata)
capture assert b1[1]==3 & bneg[1]==-2 & i1[1]==2 & mi(b2[1]) & mi(b101[1]) & l1[1]==2
if (_rc) di as err "FAIL EXPR-1 values: b1=`=b1[1]' bneg=`=bneg[1]' i1=`=i1[1]' b2=`=b2[1]' b101=`=b101[1]' l1=`=l1[1]'"
local fails = `fails' + (_rc!=0)
* storage type honours the request (byte/int/long/float), not silent double
capture assert "`tb'"=="byte" & "`ti'"=="int" & "`tl'"=="long" & "`tf'"=="float"
if (_rc) di as err "FAIL EXPR-1 types: b1=`tb' i1=`ti' l1=`tl' f1=`tf'"
local fails = `fails' + (_rc!=0)
capture assert reldif(f1[1], float(1/3)) < 1e-6
if (_rc) di as err "FAIL EXPR-1 float: f1=`=f1[1]'"
local fails = `fails' + (_rc!=0)

* ---- EXPR-4 : chained relational comparisons parse (left-associative) ------
clear
input double x
1500
500
end
parqit save `"`t'_e4.parquet"', replace data
parqit use using `"`t'_e4.parquet"'
* (1 < x) is 0/1, then ( ) < 3000 is 1 for both rows; never a parse error
capture noisily parqit gen byte z = (1 < x) < 3000
local rc4 = _rc
parqit collect, clear
capture assert `rc4'==0 & z[1]==1 & z[2]==1
if (_rc) di as err "FAIL EXPR-4: chained comparison rc=`rc4' z1=`=z[1]' z2=`=z[2]'"
local fails = `fails' + (_rc!=0)

* ---- TT-A2 : append generate() colliding with a using column is loud -------
python:
import pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
b = Macro.getLocal("t")
pq.write_table(pa.table({"id":[1,2], "src":[10,20]}), b+"_ap.parquet")
end
clear
input double id
1
end
parqit open _data
capture noisily parqit append using `"`t'_ap.parquet"', generate(src)
if (_rc==0) di as err "FAIL TT-A2: append generate(src) colliding with using col not refused"
local fails = `fails' + (_rc==0)
parqit close _all

* ---- TT-A1 : m:m master pairing reproducible for fixed inputs --------------
python:
import pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
b = Macro.getLocal("t")
pq.write_table(pa.table({"k":[1,1,1], "mv":[10,20,30]}), b+"_mm.parquet")
pq.write_table(pa.table({"k":[1,1],    "uv":[100,200]}), b+"_mu.parquet")
end
parqit use using `"`t'_mm.parquet"'
parqit merge m:m k using `"`t'_mu.parquet"', keepusing(uv)
parqit collect, clear
sort mv
tempfile firstrun
save `"`firstrun'"', replace
parqit use using `"`t'_mm.parquet"'
parqit merge m:m k using `"`t'_mu.parquet"', keepusing(uv)
parqit collect, clear
sort mv
capture cf _all using `"`firstrun'"'
if (_rc) di as err "FAIL TT-A1: m:m pairing not reproducible across runs"
local fails = `fails' + (_rc!=0)

* ---- COLLAPSE-WEIGHTS : weights are a clear, loud error --------------------
clear
input double(g x n)
1 10 2
1 20 3
end
parqit save `"`t'_cw.parquet"', replace data
parqit use using `"`t'_cw.parquet"'
capture noisily parqit collapse (mean) x [fweight=n], by(g)
if (_rc==0) di as err "FAIL COLLAPSE-WEIGHTS: weight expression not rejected"
local fails = `fails' + (_rc==0)

* ---- COLLAPSE-3 : (first) without a sort is deterministic ------------------
clear
input double(id v)
1 5
1 3
1 7
2 9
2 1
end
parqit save `"`t'_c3.parquet"', replace data
parqit use using `"`t'_c3.parquet"'
parqit collapse (first) fv = v, by(id)
parqit collect, clear
sort id
tempfile c3a
save `"`c3a'"', replace
parqit use using `"`t'_c3.parquet"'
parqit collapse (first) fv = v, by(id)
parqit collect, clear
sort id
capture cf _all using `"`c3a'"'
if (_rc) di as err "FAIL COLLAPSE-3: (first) without sort not reproducible"
local fails = `fails' + (_rc!=0)

* ---- RESHAPE-WIDE-COLORDER : numeric j orders columns numerically ----------
clear
input double(id n inc)
1 2  100
1 10 200
end
parqit save `"`t'_rw.parquet"', replace data
parqit use using `"`t'_rw.parquet"'
parqit reshape wide inc, i(id) j(n)
parqit collect, clear
qui ds
local vl `r(varlist)'
local p2  : list posof "inc2"  in vl
local p10 : list posof "inc10" in vl
capture assert `p2' > 0 & `p10' > 0 & `p2' < `p10' & inc2[1]==100 & inc10[1]==200
if (_rc) di as err "FAIL RESHAPE-WIDE-COLORDER: order [`vl'] (inc2 should precede inc10)"
local fails = `fails' + (_rc!=0)

* ---- TAB-FLOAT-1 : tabulate of an integer-valued float prints 11, not 11.0 -
python:
import pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
b = Macro.getLocal("t")
pq.write_table(pa.table({"cat":pa.array([11.0,11.0,12.0,13.0], type=pa.float64())}), b+"_tf.parquet")
end
parqit use using `"`t'_tf.parquet"'
tempname lg
local plog "`c(tmpdir)'/_parqit_v33_tab.log"
capture erase `"`plog'"'
log using `"`plog'"', text name(`lg')
parqit tabulate cat
log close `lg'
mata: st_local("tabtxt", invtokens(cat(st_local("plog"))', char(10)))
if (strpos(`"`tabtxt'"', "11.0") > 0 | strpos(`"`tabtxt'"', "12.0") > 0) {
    di as err "FAIL TAB-FLOAT-1: tabulate printed a trailing .0"
    local fails = `fails' + 1
}
capture erase `"`plog'"'

* ---- GLOB-1a : save verify escapes glob metachars in the staged path -------
* A sibling whose name matches the staged-tmp bracket class would, unescaped,
* be scanned in place of the real staged file and fail (or falsely pass) the
* verify. With escaping, the literal staged file is read and the save succeeds.
clear
set obs 5
gen double v = _n
* plant a decoy that matches the glob class of dest+".parqit_tmp"
python:
import pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
b = Macro.getLocal("t")
pq.write_table(pa.table({"v":list(range(99))}), b+"_g1.parquet.parqit_tmp")
end
capture noisily parqit save `"`t'_g[12].parquet"', replace data
local rcg = _rc
if (`rcg') di as err "FAIL GLOB-1a: save to a bracketed path failed (rc `rcg') — verify globbed a sibling"
local fails = `fails' + (`rcg'!=0)
local gn -1
python:
import os, pyarrow.parquet as pq
from sfi import Macro
p = Macro.getLocal("t")+"_g[12].parquet"
Macro.setLocal("gn", str(pq.read_table(p).num_rows) if os.path.exists(p) else "-1")
end
if (`rcg'==0 & "`gn'"!="5") di as err "FAIL GLOB-1a: literal bracketed file has `gn' rows (expected 5)"
local fails = `fails' + (`rcg'==0 & "`gn'"!="5")

di as txt "VERDICT(V33_AUDIT_FIXES_20260623): " cond(`fails'==0, "PASS", "FAIL — `fails' failures") ///
    " - gen-coerce/chained-cmp/append-gen-collide/m:m-determinism/collapse-weights/collapse-first/reshape-colorder/tab-float/glob-escape"
