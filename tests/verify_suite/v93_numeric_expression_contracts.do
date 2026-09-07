* N5/N9: storage-aware expressions, including nested missing results.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

program define compare_expression
    args expression
    preserve
    gen double expected = `expression'
    parqit open _data
    parqit gen double actual = `expression'
    parqit collect, clear
    assert actual == expected
    parqit close _all
    restore
end

set obs 4
gen float x = 16777216+2*(_n-1)
gen long y = 16777217+2*(_n-1)
foreach mode in off on {
    parqit set statamissing `mode'
    compare_expression "x==y"
    compare_expression "x<y"
    compare_expression "x==16777217"
    compare_expression "inrange(x,y,y)"
    compare_expression "inlist(x,y)"
    compare_expression "max(x,y)"
    compare_expression "cond(x<y,y,x)"
    compare_expression "x==abs(y)"
}
replace x = _n/10
compare_expression "min(x,.1)"
compare_expression "max(x,.15)"
compare_expression "cond(x>.2,x,.1)"
compare_expression "cond(.,x,.1,.2)"

clear
set obs 1
gen double x = 1e300
foreach mode in off on {
    parqit set statamissing `mode'
    parqit open _data
    parqit gen byte valid_round = round(x,1e-100)<.
    parqit gen byte valid_mod = mod(x,1e-100)<.
    parqit gen byte missing_round = missing(round(x,1e-100))
    parqit collect, clear
    assert valid_round==1 & valid_mod==1 & missing_round==0
    drop valid_round valid_mod missing_round
    parqit close _all
}

* Integer-only filters retain exact keys above binary64's integer limit.
parqit sql "SELECT 9007199254740993::BIGINT AS id"
parqit keep if id==9007199254740992
parqit count
assert r(N)==0
parqit close _all
di "VERDICT(V93_NUMERIC_EXPRESSION_CONTRACTS): PASS"
