* V45 — SAVE-SELFGLOB-2: the save self-overwrite guard must match the source
* GLOB, not its base directory, and must compare ABSOLUTE paths.
* Old behaviour had a false positive and a real hole, both found by executing
* the help-file examples: (a) with a view over qp_*.parquet, saving to ANY
* existing destination in the same folder was refused (base-dir containment) —
* blocking the documented filter-then-save workflow on its second run, once
* the partitioned destination existed; (b) a RELATIVE destination that did not
* exist yet was never made absolute, so the guard let the FIRST overwrite of
* the view's own source through. Now: dest is matched against the decoded
* pattern (parqit dialect: * ? per segment, ** deep, [ literal), a directory
* dest is refused when the pattern's base lies inside it, and an exact-file
* source is refused for the file itself or any dest directory containing it.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

local fails 0
local dir "`c(tmpdir)'/parqit_v45"
capture mkdir "`dir'"
quietly cd "`dir'"

clear
set obs 6
gen long id = _n
gen int year = 2019 + mod(_n, 2)
gen double wage = _n * 1.5
parqit save "qp_1.parquet", replace data
parqit save "qp_2.parquet", replace data

* ---- CASE 1: dest in the same folder as the glob source, NOT matching -------
parqit use using "qp_*.parquet"
capture parqit save "firm_panel.parquet", replace
if (_rc) {
    di as err "FAIL c1: non-matching dest next to a glob source refused (rc=`=_rc')"
    local fails = `fails' + 1
}
parqit close

* ---- CASE 2: same save AGAIN with the destination now existing --------------
parqit use using "qp_*.parquet"
capture parqit save "firm_panel.parquet", replace
if (_rc) {
    di as err "FAIL c2: second save over the existing dest refused (rc=`=_rc')"
    local fails = `fails' + 1
}
capture parqit save "firm_tree.parquet", replace partition_by(year)
if (_rc) {
    di as err "FAIL c2b: partitioned save next to glob source refused (rc=`=_rc')"
    local fails = `fails' + 1
}
capture parqit save "firm_tree.parquet", replace partition_by(year)
if (_rc) {
    di as err "FAIL c2c: partitioned re-save over its own tree refused (rc=`=_rc')"
    local fails = `fails' + 1
}
parqit close

* ---- CASE 3: dest MATCHES the glob source -> refused -------------------------
parqit use using "qp_*.parquet"
capture parqit save "qp_1.parquet", replace
if (_rc == 0) {
    di as err "FAIL c3: overwrote a file the open view's glob reads"
    local fails = `fails' + 1
}
capture parqit save "qp_9.parquet", replace
if (_rc == 0) {
    di as err "FAIL c3b: wrote a NEW file the open view's glob would re-read"
    local fails = `fails' + 1
}
parqit close

* ---- CASE 4: relative not-yet-existing dest equal to an exact source --------
parqit use using "qp_1.parquet"
capture parqit save "qp_1.parquet", replace
if (_rc == 0) {
    di as err "FAIL c4: relative self-overwrite of an exact source allowed"
    local fails = `fails' + 1
}
parqit close

* ---- verdict ------------------------------------------------------------------
if (`fails' == 0) di as res "VERDICT(V45_SAVE_SELFOVERLAP): PASS - glob matched, not base-dir; absolute paths"
else di as err "VERDICT(V45_SAVE_SELFOVERLAP): FAIL - `fails' case(s)"
