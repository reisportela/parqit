* V151 — BPLIM feedback, third round (2026-10-13): parqit unique (UNIQUE-1)
*   as the community-contributed unique (Brady/Hills, SSC): the varlist counted
*   jointly, a missing value counting as a value, the two "Number of" lines,
*   r(unique) = r(sum) and r(N); parqit's additions — the table per variable
*   with several variables and the table per group with by(), without the
*   generated variable; the refusals of generate() and detail; and the
*   partitions() refusal that names the keys of the existing tree
*   (PART-KEYS-1).
* Oracles: native count of distinct values via egen group(), missing; the
*   SSC unique when it is installed; the files on disk.
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile stem nat
parqit set statamissing off

sysuse auto, clear
gen int id = _n
parqit save `"`stem'_auto.parquet"', data replace

* native counts: distinct tuples with missing as a value
egen g1 = group(foreign), missing
egen g2 = group(make foreign), missing
egen g3 = group(foreign rep78), missing
egen g4 = group(rep78), missing
summarize g1, meanonly
local n1 = r(max)
summarize g2, meanonly
local n2 = r(max)
summarize g3, meanonly
local n3 = r(max)
summarize g4, meanonly
local n4 = r(max)
egen g5 = group(rep78) if foreign == 0, missing
summarize g5, meanonly
local n5 = r(max)
egen g6 = group(rep78) if foreign == 1, missing
summarize g6, meanonly
local n6 = r(max)
assert `n1' == 2 & `n2' == 74 & `n3' == 10 & `n4' == 6 & `n5' == 6 & `n6' == 4

parqit use using `"`stem'_auto.parquet"'
parqit unique foreign
assert r(unique) == `n1' & r(sum) == `n1' & r(N) == 74
parqit unique make foreign
assert r(unique) == `n2' & r(N) == 74
parqit unique foreign rep78
assert r(unique) == `n3'
parqit unique rep78
assert r(unique) == `n4'
parqit unique rep78, by(foreign)
assert r(unique) == `n4' & r(N) == 74
parqit unique rep78 if foreign == 0
assert r(unique) == `n5' & r(N) == 52
* the printed lines, the per-variable table and the by() table
tempfile ulog
log using `"`ulog'"', text name(v151)
parqit unique foreign rep78, by(foreign)
log close v151
capture program drop _v151_grep
program define _v151_grep, rclass
    version 16.0
    args logfile needle
    tempname fh
    local found 0
    file open `fh' using `"`logfile'"', read text
    file read `fh' line
    while (!r(eof)) {
        if (strpos(`"`macval(line)'"', `"`needle'"')) local found 1
        file read `fh' line
    }
    file close `fh'
    return scalar found = `found'
end
_v151_grep `"`ulog'"' "Number of unique values of foreign rep78 is  10"
assert r(found) == 1
_v151_grep `"`ulog'"' "Number of records is  74"
assert r(found) == 1
_v151_grep `"`ulog'"' "(jointly)"
assert r(found) == 1
_v151_grep `"`ulog'"' "by foreign"
assert r(found) == 1
* the SSC unique, when installed, agrees
capture which unique
if (_rc == 0) {
    sysuse auto, clear
    unique foreign rep78
    assert r(unique) == `n3' & r(N) == 74
    unique rep78 if foreign == 0
    assert r(unique) == `n5'
}
* refusals
capture noisily parqit unique
assert _rc == 198
capture noisily parqit unique rep78, generate(u)
assert _rc == 198
capture noisily parqit unique rep78, detail
assert _rc == 198
capture noisily parqit unique nosuchvar
assert _rc == 111
capture noisily parqit unique rep78 if
assert _rc == 198
parqit close _all

* ---------- partitions() without partition_by(): the keys are named --------
sysuse auto, clear
parqit save `"`stem'_tree"', data replace partition_by(foreign rep78)
tempfile plog
log using `"`plog'"', text name(v151p)
capture noisily parqit save `"`stem'_tree"', data partitions(replace)
local rc = _rc
log close v151p
assert `rc' == 198
_v151_grep `"`plog'"' "partition_by(foreign rep78)"
assert r(found) == 1
* with the keys named, the per-partition update still works as before
keep if foreign == 1 & rep78 == 5
replace price = price + 1
parqit save `"`stem'_tree"', data partitions(replace) partition_by(foreign rep78)
parqit use using `"`stem'_tree"', clear
assert _N == 74
summarize price if foreign == 1 & rep78 == 5, meanonly
sysuse auto, clear
summarize price if foreign == 1 & rep78 == 5, meanonly
local nat_mean = r(mean)
parqit use using `"`stem'_tree"', clear
summarize price if foreign == 1 & rep78 == 5, meanonly
assert reldif(r(mean), `nat_mean' + 1) < 1e-12

di "VERDICT(V151_UNIQUE_PARTITIONS_KEYS): PASS - parqit unique counts the varlist jointly with missing as a value (the SSC unique's numbers and lines), prints the per-variable and per-group tables, returns r(unique) r(sum) r(N) and refuses generate() and detail; partitions() without partition_by() names the keys of the existing tree"
