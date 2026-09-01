* Independent repro (audit 2026-09-01, F1 / PART-STRKEY-1): a string partition
* key holding the literal text "NULL" was written to a k=NULL directory, which
* the engine reads back as a missing partition, so the value loaded as "".
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

tempfile stem
local dir `"`stem'_d"'
mkdir `"`dir'"'
clear
set obs 3
gen long id = _n
gen str6 k = cond(_n == 2, "NULL", "a")
capture noisily parqit save `"`dir'/tree"', replace data partition_by(k)
assert _rc == 198                       // refused before publishing
replace k = "" in 2
parqit save `"`dir'/tree"', replace data partition_by(k)
parqit use `"`dir'/tree"', clear
sort id
assert k[1] == "a" & k[2] == "" & k[3] == "a"

di as result "VERDICT(REPRO_PARTITION_STRING_NULL): PASS - the unreadable string partition value is refused; ordinary values round-trip"
