* ============================================================================
* repro_char_projection.do
* Minimal, portable reproduction of PARQIT-CHAR-01:
*   restoring a characteristic/note of a projected-away variable aborts
*   materialisation with:  st_global(): 3300 argument out of range
*
* Run (parqit already on the adopath):
*     stata-mp -b do repro_char_projection.do
* Run (point at a build tree, upstream verify-suite style):
*     stata-mp -b do repro_char_projection.do "/path/to/parqit" "/path/to/parqit_plugin.plugin"
*
* Expected BEFORE the fix: cases 1-3 abort with rc 3300 -> VERDICT FAIL.
* Expected AFTER  the fix: all cases rc 0, survivors keep their metadata
*                          -> VERDICT PASS.
* ============================================================================
version 16.0
clear all
set more off
set varabbrev off

args repo plugin
if `"`repo'"' != "" {
    adopath ++ `"`repo'/src/ado/p"'
}
if `"`plugin'"' != "" {
    global PARQIT_PLUGIN_PATH `"`plugin'"'
}

capture which parqit
if _rc {
    di as error "parqit not found on the adopath. Pass the repo path as the first"
    di as error "argument (e.g. .../parqit), or net install parqit first."
    exit 111
}
parqit version
di as txt "tested parqit: `r(parqit_version)'"

local fails 0
tempfile f

* fixture: char on a survivor, char on a to-be-dropped column, a variable note,
* and a dataset note
clear
set obs 6
gen keepme = _n
gen gone   = _n * 10
gen grp    = mod(_n, 2)
gen noted  = _n
char keepme[source] "survivor"
char gone[source]   "to-be-dropped"
note noted: a variable note
note: a dataset note
parqit save `"`f'.parquet"', replace data

* ---- Case 1 (headline): plain column-subset use excludes the char/note cols ----
clear
capture noisily parqit use keepme grp using `"`f'.parquet"', clear
di as txt "CASE1 rc (subset use, excludes char/note cols) = " _rc
if (_rc) local ++fails
else {
    capture assert `"`: char keepme[source]'"' == "survivor"
    if (_rc) { di as error "CASE1: survivor char lost"; local ++fails }
}

* ---- Case 2: contract drops the char-bearing column ----
clear
parqit use using `"`f'.parquet"'
parqit contract grp, freq(freq)
capture noisily parqit collect, clear
di as txt "CASE2 rc (contract + collect) = " _rc
if (_rc) local ++fails
capture parqit close _all

* ---- Case 3: collapse drops the char-bearing column ----
clear
parqit use using `"`f'.parquet"'
parqit collapse (mean) mk=keepme, by(grp)
capture noisily parqit collect, clear
di as txt "CASE3 rc (collapse + collect) = " _rc
if (_rc) local ++fails
capture parqit close _all

* ---- Case 4: notes still round-trip on a full read (control) ----
clear
parqit use using `"`f'.parquet"', clear
capture assert `"`: char _dta[note1]'"' != "" & `"`: char noted[note1]'"' != ""
di as txt "CASE4 rc (notes round-trip on full use) = " _rc
if (_rc) local ++fails

* ---- Case 5: rename must NOT regress (char follows the renamed survivor) ----
clear
parqit use using `"`f'.parquet"'
parqit rename keepme kept
capture noisily parqit collect, clear
di as txt "CASE5 rc (rename + collect) = " _rc
if (_rc) local ++fails
else {
    capture assert `"`: char kept[source]'"' == "survivor"
    if (_rc) { di as error "CASE5: char did not follow rename"; local ++fails }
}
capture parqit close _all

di as txt _n "{hline 60}"
if (`fails' == 0) di as result "VERDICT(repro_char_projection): PASS (bug fixed)"
else              di as error  "VERDICT(repro_char_projection): FAIL (`fails') -> PARQIT-CHAR-01 reproduced"
di as txt "{hline 60}"
exit cond(`fails' == 0, 0, 459)
