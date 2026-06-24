* ============================================================================
* verify_mata_primitives.do
* Proves why the fix must use _st_varindex (NOT st_varindex):
*   - st_global("absent[char]", val) aborts rc 3300   (the bug's mechanism)
*   - st_varindex("absent")  aborts rc 3500            (so it CANNOT be the guard)
*   - _st_varindex("absent") returns .  (missing)      (correct, non-aborting)
*   - _st_varindex does NOT abbreviate                 (safe under set varabbrev)
*   - st_varindex("_dta") / _st_varindex("_dta") is .  -> keep the _dta branch
* No parqit needed; pure Stata/Mata. Verified live on StataNow 19.5.
* ============================================================================
version 16.0
clear all
set more off
set varabbrev off

clear
set obs 2
gen wage = _n

capture mata: st_global("wage[c]", "ok")
di as txt "st_global on EXISTING var  -> rc " _rc "   (expect 0)"
capture mata: st_global("absent[c]", "boom")
di as txt "st_global on ABSENT  var   -> rc " _rc "   (expect 3300 == the bug)"
capture mata: st_global("_dta[c]", "ok")
di as txt "st_global on _dta          -> rc " _rc "   (expect 0)"

capture mata: st_numscalar("r_a", st_varindex("absent"))
di as txt "st_varindex(absent)        -> rc " _rc "   (expect 3500 ABORT; unusable as guard)"
capture mata: st_numscalar("r_b", _st_varindex("absent"))
di as txt "_st_varindex(absent)       -> rc " _rc ", value " r_b "   (expect rc 0, value .)"
capture mata: st_numscalar("r_c", _st_varindex("wag"))
di as txt "_st_varindex(wag) [abbrev] -> value " r_c "   (expect . ; no abbreviation)"
capture mata: st_numscalar("r_d", _st_varindex("_dta"))
di as txt "_st_varindex(_dta)         -> value " r_d "   (expect . ; keep _dta branch)"

di as txt _n "Corrected guard  (tgt==_dta | _st_varindex(tgt) < .):"
clear
set obs 2
gen g = _n
foreach tgt in wage _dta g {
    capture mata: { ///
        if ("`tgt'" == "_dta" | _st_varindex("`tgt'") < .) st_global("`tgt'[c]", "v") ; ///
        else printf("  skip char for absent target %s\n", "`tgt'") ; ///
    }
    di as txt "  guard tgt=`tgt' -> rc " _rc "   (expect 0, never aborts)"
}
di as txt _n "VERDICT(verify_mata_primitives): inspect rc values above"
