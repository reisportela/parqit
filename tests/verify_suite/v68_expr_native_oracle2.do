* V68 — second live native-Stata oracle for expression edge semantics plus
* two audit-adjacent plan invariants.  v61 covers the common scalar surface;
* this file pins the hostile boundaries exercised manually on 09aug2026.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/ado/plus/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

local fails 0
tempfile stem oracle
local src `"`stem'_expr.parquet"'

* ---------------------------------------------------------------------------
* Native oracle columns are computed first and travel beside the inputs.  The
* lazy result is therefore compared cell-for-cell in the same Stata process.
clear
set obs 7
gen long id = _n
gen double x = .
replace x = -2.5       in 1
replace x = -0.5       in 2
replace x =  2.5       in 3
replace x =  1e20      in 4
replace x =  1.2345e-5 in 5
replace x =  123456789 in 6
replace x = -123456789 in 7

gen double m = 2
replace m = . in 1
replace m = 3 in 2

gen double rx = 2
gen double rlo = 1
gen double rhi = 5
replace rx  = . in 1
replace rlo = . in 2
replace rhi = . in 3
replace rx  = . in 4
replace rlo = . in 4
replace rhi = . in 4

gen double cmpx = id - 4
replace cmpx = . in 1

gen strL txt = "plain"
replace txt = "café" in 1
replace txt = "Ærø"  in 2
replace txt = "éa"   in 3
gen strL rtxt = "1.5e3"
replace rtxt = "."     in 1
replace rtxt = "1e400" in 2
replace rtxt = "abc"   in 3
gen strL cut = "éa"
gen strL expected_sub = uchar(65533) + "a"

gen double n_rnd    = round(x)
gen double n_rndu   = round(x,.)
gen double n_mdyf   = mdy(2.5,10,2020)
gen double n_mdy0   = mdy(0,1,2020)
gen double n_mdybad = mdy(2,30,2020)
gen double n_dofmf  = dofm(1.5)
gen double n_dofmn  = dofm(-0.5)
gen double n_mofdf  = mofd(59.9)
gen double n_dowf   = dow(-0.5)
gen double n_dayf   = day(-0.5)
gen double n_ir     = inrange(rx,rlo,rhi)
gen double n_cond3  = cond(m,11,22)
gen double n_cond4  = cond(m,11,22,33)
gen double n_min1   = min(m,.)
gen double n_max1   = max(m,.)
gen double n_minall = min(.,.)
gen double n_maxall = max(.,.)
gen double n_inlst  = inlist(m,.,3)
gen double n_real   = real(rtxt)
gen double n_pos    = strpos(txt,"é")
gen strL  n_str     = string(x)
gen strL  n_up      = upper(txt)
gen strL  n_sup     = strupper(txt)
gen strL  n_uup     = ustrupper(txt)
gen strL  n_low     = lower(txt)

* Two documented dialect differences: native values are retained as the live
* oracle, but SQL mode is expected to differ on the missing row.
gen double n_chain = 1 < cmpx < 10
gen double n_if = _n if cmpx > 0

* Native substr() may retain an invalid UTF-8 fragment.  parqit must instead
* publish valid UTF-8 with U+FFFD at the split boundary.
gen strL native_sub = substr(cut,2,2)
capture assert native_sub != expected_sub
if (_rc) {
    di as err "FAIL SUB-NATIVE: native substr unexpectedly equals parqit's documented replacement value"
    local ++fails
}
drop native_sub

capture noisily gen double native_ty = ty(2026)
local native_ty_rc = _rc
if (`native_ty_rc' != 133) {
    di as err "FAIL TY-NATIVE: expected native ty() rc 133, got `native_ty_rc'"
    local ++fails
}

parqit save `"`src'"', replace data

parqit use using `"`src'"'
parqit set statamissing off
parqit sort id
parqit gen double p_rnd    = round(x)
parqit gen double p_rndu   = round(x,.)
parqit gen double p_mdyf   = mdy(2.5,10,2020)
parqit gen double p_mdy0   = mdy(0,1,2020)
parqit gen double p_mdybad = mdy(2,30,2020)
parqit gen double p_dofmf  = dofm(1.5)
parqit gen double p_dofmn  = dofm(-0.5)
parqit gen double p_mofdf  = mofd(59.9)
parqit gen double p_dowf   = dow(-0.5)
parqit gen double p_dayf   = day(-0.5)
parqit gen double p_ir     = inrange(rx,rlo,rhi)
parqit gen double p_cond3  = cond(m,11,22)
parqit gen double p_cond4  = cond(m,11,22,33)
parqit gen double p_min1   = min(m,.)
parqit gen double p_max1   = max(m,.)
parqit gen double p_minall = min(.,.)
parqit gen double p_maxall = max(.,.)
parqit gen double p_inlst  = inlist(m,.,3)
parqit gen double p_real   = real(rtxt)
parqit gen double p_pos    = strpos(txt,"é")
parqit gen strL  p_str     = string(x)
parqit gen strL  p_up      = upper(txt)
parqit gen strL  p_sup     = strupper(txt)
parqit gen strL  p_uup     = ustrupper(txt)
parqit gen strL  p_low     = lower(txt)
parqit gen strL  p_sub     = substr(cut,2,2)
parqit gen double p_ty     = ty(2026)
parqit gen double p_chain  = 1 < cmpx < 10
parqit gen double p_if     = _n if cmpx > 0
parqit collect, clear

foreach c in rnd rndu mdyf mdy0 mdybad dofmf dofmn mofdf dowf dayf ir ///
             cond3 cond4 min1 max1 minall maxall inlst real pos {
    capture assert p_`c' == n_`c'
    if (_rc) {
        di as err "FAIL EXPR-`c': parqit differs from live native Stata"
        local ++fails
    }
}
foreach c in str up sup uup low {
    capture assert p_`c' == n_`c'
    if (_rc) {
        di as err "FAIL STRING-`c': parqit differs from live native Stata"
        local ++fails
    }
}
capture assert p_sub == expected_sub
if (_rc) {
    di as err "FAIL SUB-PARQIT: split UTF-8 codepoint did not yield U+FFFD plus the remainder"
    local ++fails
}
capture assert p_ty == 2026
if (_rc) {
    di as err "FAIL TY-PARQIT: ty(2026) extension did not yield 2026"
    local ++fails
}

capture assert n_chain[1] == 1 & missing(p_chain[1])
if (_rc) {
    di as err "FAIL SQL-DIFF-CHAIN: documented missing comparison divergence changed"
    local ++fails
}
capture assert p_chain == n_chain if !missing(cmpx)
if (_rc) {
    di as err "FAIL SQL-CHAIN: nonmissing chained comparisons differ"
    local ++fails
}
capture assert n_if[1] == 1 & missing(p_if[1])
if (_rc) {
    di as err "FAIL SQL-DIFF-IF: documented gen-if missing divergence changed"
    local ++fails
}
capture assert p_if == n_if if !missing(cmpx)
if (_rc) {
    di as err "FAIL SQL-IF: nonmissing gen-if rows differ"
    local ++fails
}
parqit close _all

* The same two expressions must agree everywhere under Stata-missing mode.
parqit use using `"`src'"'
parqit set statamissing on
parqit sort id
parqit gen double s_chain = 1 < cmpx < 10
parqit gen double s_if = _n if cmpx > 0
parqit collect, clear
capture assert s_chain == n_chain
if (_rc) {
    di as err "FAIL STMISS-CHAIN: statamissing-on comparison differs from native"
    local ++fails
}
capture assert s_if == n_if
if (_rc) {
    di as err "FAIL STMISS-IF: statamissing-on gen-if differs from native"
    local ++fails
}
parqit close _all
parqit set statamissing off

* ---------------------------------------------------------------------------
* Exact leading-zero combinations from the audit, compared with native
* reshape in the same process. v34 covers the general rule; these pin the two
* combinations used to falsify it in the 09aug2026 audit.
clear
input id inc01 inc2
1 100 20
2 200 21
end
preserve
reshape long inc, i(id) j(year)
sort id year
order id year inc inc01
save `"`oracle'"', replace
restore
parqit save `"`stem'_rz1.parquet"', replace data
parqit use using `"`stem'_rz1.parquet"'
parqit reshape long inc, i(id) j(year)
parqit collect, clear
sort id year
order id year inc inc01
capture cf _all using `"`oracle'"'
if (_rc) {
    di as err "FAIL RZ-A: inc01+inc2 differs from native reshape"
    local ++fails
}
parqit close _all

clear
input id inc01 inc1 inc2
1 100 10 20
2 200 11 21
end
preserve
reshape long inc, i(id) j(year)
sort id year
order id year inc inc01
save `"`oracle'"', replace
restore
parqit save `"`stem'_rz2.parquet"', replace data
parqit use using `"`stem'_rz2.parquet"'
parqit reshape long inc, i(id) j(year)
parqit collect, clear
sort id year
order id year inc inc01
capture cf _all using `"`oracle'"'
if (_rc) {
    di as err "FAIL RZ-B: inc01+inc1+inc2 differs from native reshape"
    local ++fails
}
parqit close _all

* ---------------------------------------------------------------------------
* A foreign file may legitimately contain names resembling row-context
* machinery. They survive as data, while the reserved uppercase placeholder
* is unavailable as an expression identifier and generated _n still works.
local hostile `"`stem'_hostile.parquet"'
python:
from sfi import Macro
import pyarrow as pa
import pyarrow.parquet as pq
pq.write_table(pa.table({
    "id": pa.array([1, 2, 3], pa.int32()),
    "__PARQIT_ROW__": pa.array([101, 102, 103], pa.int32()),
    "__parqit_rn_1": pa.array([11, 12, 13], pa.int32()),
}), Macro.getLocal("hostile"))
end

parqit use using `"`hostile'"'
capture noisily parqit gen double bad = __PARQIT_ROW__
local reserved_rc = _rc
if (`reserved_rc' == 0) {
    di as err "FAIL HOSTILE-RESERVED: internal row token was accepted as an expression identifier"
    local ++fails
}
parqit sort id
parqit keep if _n <= 2
parqit gen double z = _n
parqit collect, clear
capture assert _N == 2 & id[1] == 1 & id[2] == 2
if (_rc) {
    di as err "FAIL HOSTILE-ROWS: row-context filtering selected the wrong rows"
    local ++fails
}
capture assert __PARQIT_ROW__[1] == 101 & __PARQIT_ROW__[2] == 102
if (_rc) {
    di as err "FAIL HOSTILE-UPPER: __PARQIT_ROW__ payload was clobbered"
    local ++fails
}
capture assert __parqit_rn_1[1] == 11 & __parqit_rn_1[2] == 12
if (_rc) {
    di as err "FAIL HOSTILE-LOWER: __parqit_rn_1 payload was clobbered"
    local ++fails
}
capture assert z[1] == 1 & z[2] == 2
if (_rc) {
    di as err "FAIL HOSTILE-GEN: generated _n did not coexist with hostile columns"
    local ++fails
}
parqit close _all

if (`fails' == 0) {
    di as result "VERDICT(V68_EXPR_NATIVE_ORACLE2): PASS - live native expression edges, documented divergences, reshape and hostile row-context names are pinned"
}
else {
    di as err "VERDICT(V68_EXPR_NATIVE_ORACLE2): FAIL - `fails' check(s)"
}
