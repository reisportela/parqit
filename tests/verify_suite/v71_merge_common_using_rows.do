* V71 — MERGE-COMMON-1 (audit 2026-08-22, A3-1): a non-key variable present on
* BOTH sides of a lazy merge keeps the master value on matched and master-only
* rows, but on a using-only row (_merge == 2) it must carry the USING value, as
* native merge does; parqit left it missing. Native Stata on the same data is
* the oracle (cf _all) for every keep()/keepusing() variant, both directions,
* numeric and string common variables, a master missing kept on a match, and
* the lazy save of the merged view. A common variable that is string on one
* side and numeric on the other is refused loudly (native r(106)).
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

global V71_FAILS 0
tempfile stem
local mq  `"`stem'_m.parquet"'
local uq  `"`stem'_u.parquet"'
local md  `"`stem'_m.dta"'
local ud  `"`stem'_u.dta"'

* master: k c sc a — k=2 has a missing c and "" sc (must stay missing on match)
clear
input double k double c str4 sc double a
1 11 "m1" 1
2  . ""   2
3 33 "m3" 3
5 55 "m5" 5
end
qui save `"`md'"'
parqit save `"`mq'"', replace
* using: k c sc b — k=4, 6 exist only here (and 6 carries a missing c)
clear
input double k double c str4 sc double b
2 222 "u2" 20
3 333 "u3" 30
4 444 "u4" 40
6   . ""   60
end
qui save `"`ud'"'
parqit save `"`uq'"', replace

* one variant: native result vs lazy result (collect) and vs a lazy save
program define _v71_case
    args label kind mfile ufile mparq uparq opts
    use `"`mfile'"', clear
    qui merge `kind' k using `"`ufile'"', `opts'
    sort k c
    tempfile oracle
    qui save `"`oracle'"'
    parqit use using `"`mparq'"'
    parqit merge `kind' k using `"`uparq'"', `opts'
    parqit sort k
    tempfile lazyout
    parqit save `"`lazyout'.parquet"', replace
    parqit collect, clear
    sort k c
    capture cf _all using `"`oracle'"', verbose
    if (_rc) {
        di as err "FAIL: `label' — collect differs from native merge `kind' (`opts')"
        global V71_FAILS = $V71_FAILS + 1
    }
    parqit close _all
    parqit use using `"`lazyout'.parquet"', clear
    sort k c
    capture cf _all using `"`oracle'"', verbose
    if (_rc) {
        di as err "FAIL: `label' — lazy save differs from native merge `kind' (`opts')"
        global V71_FAILS = $V71_FAILS + 1
    }
end

_v71_case default    1:1 `"`md'"' `"`ud'"' `"`mq'"' `"`uq'"' ""
_v71_case keepusing  1:1 `"`md'"' `"`ud'"' `"`mq'"' `"`uq'"' "keep(using)"
_v71_case keepmatchu 1:1 `"`md'"' `"`ud'"' `"`mq'"' `"`uq'"' "keep(match using)"
_v71_case keepus_csc 1:1 `"`md'"' `"`ud'"' `"`mq'"' `"`uq'"' "keepusing(c sc)"
_v71_case keepus_b   1:1 `"`md'"' `"`ud'"' `"`mq'"' `"`uq'"' "keepusing(b)"
_v71_case keep13     1:1 `"`md'"' `"`ud'"' `"`mq'"' `"`uq'"' "keep(1 3)"
_v71_case keepu_c    1:1 `"`md'"' `"`ud'"' `"`mq'"' `"`uq'"' "keep(using) keepusing(c)"
_v71_case gen        1:1 `"`md'"' `"`ud'"' `"`mq'"' `"`uq'"' "generate(mg)"
_v71_case m1         m:1 `"`md'"' `"`ud'"' `"`mq'"' `"`uq'"' ""
_v71_case rev_1m     1:m `"`ud'"' `"`md'"' `"`uq'"' `"`mq'"' ""
_v71_case rev_keepu  1:m `"`ud'"' `"`md'"' `"`uq'"' `"`mq'"' "keep(using)"

* m:1 with duplicate master keys and a using-only row
clear
input double k double c str4 sc double a
1 11 "m1" 1
1 12 "m1b" 2
3 .  ""   3
end
tempfile md2 mq2
qui save `"`md2'"'
parqit save `"`mq2'.parquet"', replace
_v71_case m1_dupkeys m:1 `"`md2'"' `"`ud'"' `"`mq2'.parquet"' `"`uq'"' ""

* string on one side, numeric on the other: refused loudly (native r(106))
clear
input double k str4 c
1 "x"
end
tempfile us
parqit save `"`us'.parquet"', replace
parqit use using `"`mq'"'
capture noisily parqit merge 1:1 k using `"`us'.parquet"'
if (_rc == 0) {
    di as err "FAIL: common variable with mismatched kinds was not refused"
    global V71_FAILS = $V71_FAILS + 1
}
parqit close _all

if ($V71_FAILS) {
    di as err "VERDICT(V71_MERGE_COMMON_USING_ROWS): FAIL - $V71_FAILS case(s) differ from native"
    exit 9
}
di "VERDICT(V71_MERGE_COMMON_USING_ROWS): PASS - common non-key variables carry the using value on using-only rows, master values on matches (missing included), every keep()/keepusing() variant, both directions, collect and lazy save agree with native merge; mismatched kinds refused"
