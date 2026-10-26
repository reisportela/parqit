* Native standardisation, volatile single-realisation, ordered row expressions
* and dead-column pruning are independent gates for the parallel global plan.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
parqit set threads 4
set obs 100001
gen long id=_n
gen double x=mod(id,997)/8
replace x=. if mod(id,13)==0
gen byte eligible=mod(id,3)!=0
label variable x "Standardisation source"
char x[meaning] "preserved"
tempfile source original expected
save "`original'"
parqit save "`source'.parquet", data
egen double z=std(x), mean(10) sd(2)
egen double zi=std(x) if eligible, mean(10) sd(2)
gen double shifted=x+_n
egen double zn=std(shifted)
drop shifted
egen float zf=std(x)
save "`expected'"

parqit use using "`source'.parquet"
parqit sort id
parqit egen double z=std(x), mean(10) sd(2)
parqit egen double zi=std(x) if eligible, mean(10) sd(2)
parqit egen double zn=std(x+_n)
parqit egen float zf=std(x)
parqit collect, clear
assert id==_n
if "`:variable label x'"!="Standardisation source" error 9
if "`:char x[meaning]'"!="preserved" error 9
if "`:type zf'"!="float" error 9
rename (z zi zn zf) (got_z got_zi got_zn got_zf)
merge 1:1 id using "`expected'", assert(match) nogen
foreach v in z zi zn zf {
    assert missing(got_`v')==missing(`v')
    assert reldif(got_`v',`v')<1e-12 if !missing(`v')
}

* The argument and qualifier must read the same random realisation that is
* returned. Independent source scans would fail the native oracle here.
parqit sql "SELECT i::BIGINT AS id, random()::DOUBLE AS x FROM range(20001) t(i)"
parqit sort id
parqit egen double z=std(x) if x<0.75
parqit collect, clear
assert id==_n-1
egen double want=std(x) if x<0.75
assert missing(z)==missing(want)
assert reldif(z,want)<1e-12 if !missing(want)

foreach specification in "SELECT 1::DOUBLE AS x WHERE false" "SELECT 1::DOUBLE AS x" "SELECT NULL::DOUBLE AS x FROM range(17)" "SELECT 4::DOUBLE AS x FROM range(17)" {
    parqit sql "`specification'"
    parqit egen double z=std(x)
    parqit collect, clear
    if _N>0 assert missing(z)
}
parqit use using "`source'.parquet"
parqit egen double z=std(x) if id<0
parqit collect, clear
if _N!=100001 error 9
assert missing(z)

* Dropping a computed column must still permit the optimiser to remove its
* fallible input expression. A materialised CTE must not force dead work.
* DuckDB 2.0 removes it; 1.5.6, the default engine, evaluates the dropped
* input, as the published 0.5.1 does there, and the collect fails with the
* engine's conversion error (checked 25 Oct 2026 against the 0.5.1 plugin) —
* a known limitation of 1.5.6: loud, with the dataset in memory kept (G11).
quietly parqit version
local engine2 = regexm(`"`r(duckdb_version)'"', "^v?2[.]")
quietly datasignature
local sig_before `"`r(datasignature)'"'
parqit sql "SELECT i::INTEGER AS id, CAST(CASE WHEN i=99 THEN 'bad' ELSE '1' END AS INTEGER) AS x FROM range(100) t(i)"
parqit egen double z=std(x)
parqit keep id
parqit sort id
capture log close _v173
tempfile dead_log
log using `"`dead_log'"', text replace name(_v173)
capture noisily parqit collect, clear
local rc_dead = _rc
log close _v173
if `engine2' {
    if `rc_dead' error `rc_dead'
    assert _N==100 & id==_n-1
}
else {
    if `rc_dead' != 920 error 9
    quietly datasignature
    if `"`r(datasignature)'"' != `"`sig_before'"' error 9
    python: from sfi import Macro; assert "Could not convert string 'bad' to INT32" in open(Macro.getLocal("dead_log"), encoding="utf-8").read()
}

* A sorted declaration survives even when the public key is projected away.
parqit use using "`source'.parquet"
parqit gsort -id
parqit egen double z=std(x)
parqit drop id
parqit gen double position=_n
parqit collect, clear
assert position==_n
gen long id=100002-_n
rename z got_z
merge 1:1 id using "`expected'", assert(match) nogen
* expected z used mean(10) sd(2), whereas this branch uses mean(0) sd(1).
assert reldif(got_z,(z-10)/2)<1e-12 if !missing(z)
parqit close _all
di "VERDICT(V173_GLOBAL_STANDARDISE): PASS - native values/storage, qualifiers, row order, volatile single-realisation, empty/constant groups and " cond(`engine2', "dead-column pruning", "the 1.5.6 limitation on a dropped fallible input (a loud error, the dataset kept)")
