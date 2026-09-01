* Independent repro (audit 2026-09-01, F2 / FLOAT-LIT-1): a float variable
* compared with a decimal literal was evaluated in single precision by the
* engine (`x == 0.1` true, `x > 0.1` false for a float x holding float(0.1)),
* the opposite of native Stata's all-double evaluator.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

tempfile src
clear
set obs 3
gen float x = 0.1
replace x = 0.3 in 2
replace x = 2.5 in 3
parqit save `"`src'.parquet"', replace data
qui count if x == 0.1
local n_eq = r(N)           // 0 natively: float(0.1) != 0.1
qui count if x > 0.1
local n_gt = r(N)           // 3 natively
parqit use using `"`src'.parquet"'
parqit count if x == 0.1
assert r(N) == `n_eq'
parqit count if x > 0.1
assert r(N) == `n_gt'
parqit count if x == float(0.1)
assert r(N) == 1
parqit close _all

di as result "VERDICT(REPRO_FLOAT_LITERAL_COMPARE): PASS - float-column comparisons match native double semantics; float() available"
