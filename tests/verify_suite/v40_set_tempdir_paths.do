* V40 — SET-TEMPDIR-2: `parqit set tempdir` must accept a quoted value, an
* absolute Unix path, and a path containing spaces. The value arrives quoted
* (mandatory for spaces; usual for a path), and a regular-quoted reference of a
* still-quoted value built ""/abs/path"", which Stata parsed as arithmetic (a
* leading "/" divides by the first path component) and aborted with
* "<component> not found", rc 111 — so spill-to-scratch was unusable on every
* Unix absolute path. Oracle: behavioural — rc 0 on each form, the other `set`
* values still work, and a collect runs after the tempdir is set.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

local fails 0
tempfile f

* a writable absolute dir and an absolute dir whose name contains spaces
local base `"`c(tmpdir)'/_parqit_v40"'
local spaced `"`base'/dir with spaces"'
capture mkdir `"`base'"'
capture mkdir `"`spaced'"'

clear
set obs 5
gen long id = _n
gen double x = _n
parqit save `"`f'.parquet"', replace data

* ---- absolute path (the historical rc-111 trigger) --------------------------
parqit use using `"`f'.parquet"'
parqit set threads 1
parqit set memory_limit 512MB
capture noisily parqit set tempdir `"`base'"'
if (_rc) di as err "FAIL t1: set tempdir <absolute> rc=`=_rc' (the rc-111 regression)"
local fails = `fails' + (_rc!=0)

* ---- absolute path containing spaces ----------------------------------------
capture noisily parqit set tempdir `"`spaced'"'
if (_rc) di as err "FAIL t2: set tempdir <abs path with spaces> rc=`=_rc'"
local fails = `fails' + (_rc!=0)

* ---- the tempdir actually took effect: a collect still materialises ----------
capture noisily parqit collect, clear
if (_rc) di as err "FAIL t3: collect after set tempdir aborted rc=`=_rc'"
local fails = `fails' + (_rc!=0)
if (_rc==0) {
    capture assert _N == 5 & c(k) == 2
    if (_rc) di as err "FAIL t3: wrong result shape after set tempdir"
    local fails = `fails' + (_rc!=0)
}
capture parqit close _all

* ---- non-regression: the scalar settings still parse ------------------------
clear
parqit use using `"`f'.parquet"'
capture noisily parqit set statamissing on
local r1 = _rc
capture noisily parqit set statamissing off
local r2 = _rc
capture noisily parqit set threads 2
local r3 = _rc
capture assert `r1'==0 & `r2'==0 & `r3'==0
if (_rc) di as err "FAIL t4: scalar set values regressed (statamissing=`r1'/`r2' threads=`r3')"
local fails = `fails' + (_rc!=0)

* ---- a bad setting name is still a loud, clean error (not an abort) ----------
capture noisily parqit set notasetting 1
if (_rc==0) di as err "FAIL t5: unknown set name not rejected"
local fails = `fails' + (_rc==0)
capture parqit close _all

di as txt "VERDICT(V40_SET_TEMPDIR_PATHS): " cond(`fails'==0, "PASS", "FAIL — `fails' failures") ///
    " - set tempdir accepts absolute / spaced / quoted paths (no rc-111); scalar sets + bad-name error intact"
