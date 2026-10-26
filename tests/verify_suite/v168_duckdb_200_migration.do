* Engine-upgrade contracts: native SQL dialect, offline operation, row lambdas,
* native precision and atomic refusal of a type without a Stata representation.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
parqit version
local engine `"`r(duckdb_version)'"'
parqit set threads 4

parqit sql "SELECT current_setting('autoload_known_extensions')::BOOLEAN AS loading, current_setting('autoinstall_known_extensions')::BOOLEAN AS installing"
parqit collect, clear
assert _N == 1 & loading == 0 & installing == 0
parqit close _all

* The maintainer selected native 2.0 SQL, with documented differences from 1.5.6.
if strpos(`"`engine'"', "v2.0.0") == 1 {
    parqit sql "SELECT current_setting('error_on_division_by_zero')::BOOLEAN AS native_division, 2*3! AS factorial, ('abc' ~ 'b')::INTEGER AS partial_regex", clear
    assert native_division==1 & factorial==12 & partial_regex==1
    parqit close _all
    foreach expression in "1 // 0" "1 % 0" "list_sum(list_transform([1,2,3], x -> x+1))" {
        clear
        set obs 1
        gen long sentinel=12345
        capture noisily parqit sql "SELECT `expression' AS unsupported", clear
        assert _rc!=0
        assert _N==1 & sentinel==12345
        parqit close _all
    }
}
else {
    parqit sql "SELECT CAST(1 // 0 AS DOUBLE) AS quotient, CAST(1 % 0 AS DOUBLE) AS remainder, list_sum(list_transform([1,2,3], x -> x+1)) AS old_lambda", clear
    assert missing(quotient) & missing(remainder) & old_lambda==9
    parqit close _all
}
parqit sql "SELECT list_sum(list_transform([1,2,3], lambda x : x+1)) AS new_lambda"
parqit collect, clear
assert _N == 1 & new_lambda == 9
parqit close _all

* query shares the same native dialect as sql; parentheses are unambiguous.
parqit sql "SELECT 12 AS v UNION ALL SELECT 720"
parqit query "WHERE v = 2*3!"
parqit collect, clear
assert _N == 1
if strpos(`"`engine'"', "v2.0.0") == 1 assert v == 12
else assert v == 720
parqit close _all
parqit sql "SELECT 2*(3!) AS mathematical, (2*3)! AS legacy", clear
assert mathematical == 12 & legacy == 720
parqit close _all

* Stata expressions retain missing for invalid arithmetic in both engines.
parqit sql "SELECT 0 AS x"
parqit gen double division = 1/x
parqit gen double remainder = mod(1,x)
parqit collect, clear
assert missing(division) & missing(remainder)
parqit close _all

clear
set obs 4101
gen long id = _n
gen double x = cond(mod(id, 7) == 0, ., mod(id, 17) - 8)
gen double y = cond(mod(id, 11) == 0, ., mod(id, 23) - 11)
gen double z = cond(mod(id, 13) == 0, ., 2^40 + mod(id, 3))
egen double expected_sd = rowsd(x y z)
egen double expected_median = rowmedian(x y z)
egen double expected_pct = rowpctile(x y z), p(25)
parqit open _data
parqit egen double actual_sd = rowsd(x y z)
parqit egen double actual_median = rowmedian(x y z)
parqit egen double actual_pct = rowpctile(x y z), p(25)
parqit sort id
parqit collect, clear
assert _N == 4101 & id == _n
assert actual_median == expected_median & actual_pct == expected_pct
assert missing(actual_sd) == missing(expected_sd)
assert reldif(actual_sd, expected_sd) < 1e-15 if !missing(expected_sd)
parqit close _all

if strpos(`"`engine'"', "v2.0.0") == 1 {
    * Native 2.0 huge literals and explicit BIGNUM retain their exact engine
    * type and must not be silently narrowed by either materialiser.
    parqit sql "SELECT typeof(1234567890123456789012345678901234567890) AS literal_type", clear
    assert literal_type=="BIGNUM"
    parqit close _all
    foreach mode in eager lazy {
        clear
        set obs 1
        gen double sentinel = 123456789
        if "`mode'" == "eager" {
            capture noisily parqit sql "SELECT CAST('1234567890123456789012345678901234567890' AS BIGNUM) AS unsupported", clear
        }
        else {
            capture noisily parqit sql "SELECT CAST('1234567890123456789012345678901234567890' AS BIGNUM) AS unsupported"
        }
        local type_rc = _rc
        if `type_rc' == 0 {
            capture noisily parqit collect, clear
            local type_rc = _rc
        }
        assert `type_rc' != 0
        assert _N == 1 & sentinel == 123456789
        parqit close _all
    }
    parqit sql "SELECT 1 AS good, NULL AS typeless", clear
    assert _N == 1 & good == 1
    capture confirm variable typeless
    assert _rc != 0
    parqit close _all
    clear
    set obs 1
    gen double sentinel = 123456789
    capture noisily parqit sql "SELECT 42::VARIANT AS unsupported"
    local type_rc = _rc
    if `type_rc' == 0 {
        capture noisily parqit collect, clear
        local type_rc = _rc
    }
    assert `type_rc' != 0
    assert _N == 1 & sentinel == 123456789
    parqit close _all
}
di "VERDICT(V168_ENGINE_MIGRATION): PASS - SQL, offline settings, row functions and atomic type refusal (`engine')"
