* V26 — performance tips fire only when a faster path genuinely applies, and
* can be muted. Uses `parqit open _data` (the in-memory size drives its tip):
* >=1,000,000 obs prints a one-line tip; fewer is silent; PARQIT_NOTIPS mutes it.
clear all
set more off
set varabbrev off

args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

* scan a captured log for the substring "(tip:" — returns r(hit) 0/1
capture program drop _has_tip
program define _has_tip, rclass
    args logfile
    local hit 0
    tempname fh
    file open `fh' using `"`logfile'"', read text
    file read `fh' line
    while (r(eof) == 0) {
        if (strpos(`"`line'"', "(tip:") > 0) local hit 1
        file read `fh' line
    }
    file close `fh'
    return scalar hit = `hit'
end

local nfail = 0
tempfile lg

* --- 1) large in-memory (>=1M): tip fires ---
clear
quietly set obs 1000001
quietly gen double x = _n
capture parqit close _all
log using `"`lg'1.log"', text replace
parqit open _data
log close
quietly parqit close _all
_has_tip `"`lg'1.log"'
if (r(hit) == 1) di as result "  PASS: open _data on 1,000,001 obs prints a tip"
else {
    di as error "  FAIL: no tip on a large open _data"
    local ++nfail
}

* --- 2) small in-memory (<1M): silent ---
clear
quietly set obs 999999
quietly gen double x = _n
capture parqit close _all
log using `"`lg'2.log"', text replace
parqit open _data
log close
quietly parqit close _all
_has_tip `"`lg'2.log"'
if (r(hit) == 0) di as result "  PASS: open _data on 999,999 obs is silent (no tip)"
else {
    di as error "  FAIL: tip fired on a small open _data"
    local ++nfail
}

* --- 3) large + PARQIT_NOTIPS: muted ---
global PARQIT_NOTIPS 1
clear
quietly set obs 1000001
quietly gen double x = _n
capture parqit close _all
log using `"`lg'3.log"', text replace
parqit open _data
log close
quietly parqit close _all
global PARQIT_NOTIPS
_has_tip `"`lg'3.log"'
if (r(hit) == 0) di as result "  PASS: PARQIT_NOTIPS mutes the tip"
else {
    di as error "  FAIL: tip fired with PARQIT_NOTIPS set"
    local ++nfail
}

if (`nfail' == 0) di as result _n "VERDICT(v26_perf_tips): PASS"
else              di as error  _n "VERDICT(v26_perf_tips): FAIL (`nfail')"
