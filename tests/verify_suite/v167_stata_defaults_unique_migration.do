* V167: native missing defaults, explicit SQL mode, unique counts and migration.
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile source native
input double x double y str2 s byte grp
1 1 "a" 1
1 . "a" 1
. 2 "" 1
. . "" 2
2 2 "b" 2
2 2 "b" 2
end
save `native'
parqit save `"`source'.parquet"', data
parqit use using `"`source'.parquet"'
count if x > 0
local native_n = r(N)
parqit count if x > 0
assert r(N) == `native_n'
parqit gen byte positive = x > 0
parqit collect, clear
assert positive == (x > 0)
parqit drop positive
parqit set statamissing off
parqit count if x > 0
assert r(N) == 4
parqit set statamissing on
parqit count if x > 0
assert r(N) == 6

* A removed command refuses before changing either the view or the data.
foreach old in distinct head glimpse {
    capture noisily parqit `old'
    assert _rc == 198
    parqit count
    assert r(N) == 6
}

* Native egen group is an independent oracle for each joint count.
use `native', clear
egen g_all = group(x y s), missing
egen g_complete = group(x y s)
summarize g_all, meanonly
local joint_all = r(max)
summarize g_complete, meanonly
local joint_complete = r(max)
count if !missing(x,y) & s != ""
local n_complete = r(N)
parqit unique x y s
assert r(unique) == `joint_all' & r(N) == 6 & r(N_used) == 6
parqit unique x y s, nomissing
assert r(unique) == `joint_complete' & r(N) == 6 & r(N_used) == `n_complete'
parqit unique x, nomissing
assert r(unique) == 2 & r(N_used) == 4
parqit unique s, nomissing
assert r(unique) == 2 & r(N_used) == 4
parqit unique x y s, by(grp) nomissing
assert r(unique) == `joint_complete' & r(N_used) == `n_complete'
parqit unique x y s, by(grp s) nomissing
assert r(unique) == `joint_complete'
parqit unique
assert r(unique) == 5 & r(N) == 6
parqit unique x y if grp == 1, nomissing
assert r(unique) == 1 & r(N) == 3 & r(N_used) == 1
parqit unique x y if grp == 9, nomissing
assert r(unique) == 0 & r(N) == 0 & r(N_used) == 0
capture noisily parqit unique x y, joint
assert _rc == 198
parqit close _all
di "VERDICT(V167_STATA_DEFAULTS_UNIQUE_MIGRATION): PASS"
