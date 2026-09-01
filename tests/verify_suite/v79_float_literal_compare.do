* V79 — FLOAT-LIT-1 / FLOAT-FN-1 (audit 2026-09-01, F2/F11): a float variable
*   compared with a decimal literal is evaluated in DOUBLE, as native Stata (an
*   all-double evaluator) does. The engine used to bind `x == 0.1` by casting
*   the literal DOWN to float, so the comparison was true for a float x holding
*   float(0.1) — native says false — and `x > 0.1` was false where native says
*   true; the same in inrange()/inlist()/cond() and in gen/replace qualifiers.
*   Oracle: native `count if` / `gen` / `replace` on the same data, in both
*   missing-value modes. float() — the native idiom for such comparisons — is
*   implemented and checked against native values.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

tempfile stem
local src `"`stem'_src.parquet"'
clear
set obs 7
gen long id = _n
gen float x = .
replace x = 0.1 in 1
replace x = 0.3 in 2
replace x = 1.1 in 3
replace x = 2.5 in 4
replace x = -0.1 in 5
replace x = 1e-5 in 6
gen double d = x
gen double v = _n
parqit save `"`src'"', replace data

local nfail 0
local ncase 0
foreach mode in off on {
    parqit set statamissing `mode'
    foreach e in "x == 0.1" "x > 0.1" "x >= 0.1" "x < 0.3" "x <= 0.3" "x != 1.1" ///
        "x == 2.5" "x > 1.1" "x < 1.1" "x == 0.3" "x == -0.1" "x > -0.1"          ///
        "x == 1e-5" "x < 1e-5" "x == float(0.1)" "x == float(0.3)"                 ///
        "inrange(x, 0.1, 0.3)" "inrange(x, -0.1, 1.1)" "inlist(x, 0.1, 1.1)"      ///
        "inlist(x, 2.5, 0.3)" "cond(x > 0.1, 1, 0)" "(x > 0.1) + (x > 1.1) == 2"  ///
        "d == 0.1" "d > 0.1" "x*1 == 0.1" "x + 0 > 0.1" "x - 0.3 == 0"             ///
        "abs(x - 0.1) < 1e-9" "round(x, 0.1) == 0.1" "x == 0.1 | x == 1.1"        ///
        "!(x > 0.1)" "missing(x) | x > 0.1" {
        local ++ncase
        * statamissing on: native ordering (missing sorts high) on both sides;
        * off (SQL semantics): the one row with a missing x is excluded on both
        * sides by the same qualifier, so only the float-vs-double question
        * remains — the documented missing-mode divergence is v42's business
        local wrap = cond("`mode'" == "on", "", "& !missing(x)")
        qui count if (`e') `wrap'
        local nat = r(N)
        parqit use using `"`src'"'
        capture noisily quietly parqit count if (`e') `wrap'
        local prc = _rc
        local pq = cond(`prc' == 0, r(N), -1)
        parqit close _all
        if (`pq' != `nat') {
            local ++nfail
            di as err `"MISMATCH (statamissing `mode'): `e' -> native `nat', parqit `pq' (rc `prc')"'
        }
    }
}
parqit set statamissing off
assert `nfail' == 0
assert `ncase' == 64

* ---------- gen / replace with the same literals, materialised -------------------
parqit use using `"`src'"'
parqit gen double g1 = x > 0.1
parqit gen double g2 = x == 0.3
parqit gen double g3 = inrange(x, 0.1, 0.3)
parqit gen double g4 = float(x) == float(0.1)
parqit gen double g5 = float(0.1)
parqit replace v = 99 if x == 0.1
parqit replace v = 77 if x > 1.1
parqit keep if x != 1.1 | missing(x)
parqit collect, clear
parqit close _all
* the row with a missing x follows the documented SQL missing-mode rules (v42);
* the float-vs-double question is the six nonmissing rows, compared exactly
drop if missing(x)
tempfile pq
save `"`pq'"', replace
parqit use `"`src'"', clear
gen double g1 = x > 0.1
gen double g2 = x == 0.3
gen double g3 = inrange(x, 0.1, 0.3)
gen double g4 = float(x) == float(0.1)
gen double g5 = float(0.1)
replace v = 99 if x == 0.1
replace v = 77 if x > 1.1
keep if x != 1.1 | missing(x)
drop if missing(x)
cf _all using `"`pq'"'

* float() values themselves match native
parqit use using `"`src'"'
parqit gen double f1 = float(0.1)
parqit gen double f2 = float(x)
parqit gen double f3 = float(1e39)
parqit collect, clear
assert reldif(f1[1], float(0.1)) < 1e-15
assert f1[1] != 0.1
forvalues i = 1/6 {
    assert f2[`i'] == x[`i']
}
assert f3[1] == .
parqit close _all

di "VERDICT(V79_FLOAT_LITERAL_COMPARE): PASS - 64 float-column filters agree with native in both missing modes, gen/replace qualifiers match native cf, float() values are native"
