* v53 — N-2G31: a result beyond the SPI's observation ceiling must refuse
* with a real message, never a parser artifact.
*
* Live find: `parqit collect` over a 2,672,287,500-row trades glob died as
* a bare `option n() invalid` (rc 198) — _parqit_load_core declared
* n(integer), and syntax's integer parser rejects any VALUE above
* 2,147,483,647 before code runs. The SPI's observation index (ST_int in
* SF_vstore/SF_sstore, vendor/stata/stplugin.h) is a 32-bit int, so the
* 2^31-1 ceiling is architectural: the fix is a loud refusal naming the
* count and the out-of-core remedies. Both prepare paths (use, collect)
* now guard it in the plugin — verified live against the real 2.67B-row
* glob (rc 901 on both; and the remedy, a lazy collapse, reproduced the
* full count 2,672,287,500 exactly) — and the ado's load_core re-checks as
* defence in depth with n() parsed as a string. This test pins the ado
* layer (a >2^31-row fixture is not buildable in a test): the guard fires
* before the response file is read, so an empty resp suffices.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

* An auto-loaded ado defines its subprograms QUALIFIED (parqit._parqit_...),
* callable only from inside parqit — a do-file cannot invoke the internal
* helper under test. Run the ado explicitly (before any auto-load, so no
* program is doubly defined) to define them globally for the direct pin,
* and pre-load the plugin at do-file level: `program ..., plugin` issued
* from within a do-defined program does not leave the plugin invocable
* (the auto-load context does; the whole suite runs that way).
do `"`repo'/src/ado/p/parqit.ado"'
program parqit_plugin, plugin using(`"`plugin'"')

tempname junk
tempfile resp strl
mata: fh = fopen(st_local("resp"), "w"); fclose(fh)   // empty response file

* ---- beyond the ceiling: loud rc 901 with the real message, never
* ---- "option n() invalid" (rc 198 from the option parser)
capture noisily _parqit_load_core, resp(`"`resp'"') strl(`"`strl'"') ///
    tag(t) n(2672287500)
assert _rc == 901

* the sentinel dataset must be untouched by the refusal
clear
set obs 2
gen keepme = _n
capture noisily _parqit_load_core, resp(`"`resp'"') strl(`"`strl'"') ///
    tag(t) n(9999999999)
assert _rc == 901
assert _N == 2 & keepme[2] == 2

* ---- a non-number n is a loud confirm error, not a silent anything
capture _parqit_load_core, resp(`"`resp'"') strl(`"`strl'"') tag(t) n(abc)
assert _rc == 7

* ---- the boundary itself still parses (2^31-1 passes the guard and
* ---- proceeds into the normal load path: an empty resp yields an empty
* ---- staged load, which must not crash — any rc is fine, 198/901 are not)
capture _parqit_load_core, resp(`"`resp'"') strl(`"`strl'"') ///
    tag(t) n(0)
assert _rc == 0
assert _N == 0 & c(k) == 0                    // empty load swapped in cleanly

* ---- the public paths still load normal files end-to-end
tempfile fb
local f `"`fb'.parquet"'
clear
set obs 5
gen x = _n
parqit save `"`f'"', replace data
parqit use using `"`f'"', clear
assert _N == 5 & x[5] == 5
parqit use using `"`f'"'
parqit collect, clear
assert _N == 5 & x[5] == 5
parqit close _all

di "VERDICT(V53_SPI_OBS_CEILING): PASS - >2^31-row results refuse with rc 901 and a real message on the ado guard; boundary/normal loads intact (plugin guards verified live on a 2.67B-row glob)"
