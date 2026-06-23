* Repro: parqit open _data reuses one bridge file per Stata PID, so a later
* open overwrites the backing file of an earlier named view.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

clear
set obs 1
gen long x = 1
parqit open _data, name(first)

clear
set obs 1
gen long x = 2
parqit open _data, name(second)

parqit view first
parqit collect, clear

if (_N == 1 & x[1] == 2) {
    di "VERDICT(REPRO_OPEN_DATA_OVERWRITE): FAIL - first view read the second dataset"
    exit 9
}
if (_N == 1 & x[1] == 1) {
    di "VERDICT(REPRO_OPEN_DATA_OVERWRITE): PASS - first view retained its backing data"
    exit
}

di "VERDICT(REPRO_OPEN_DATA_OVERWRITE): FAIL - unexpected result from first view"
exit 9
