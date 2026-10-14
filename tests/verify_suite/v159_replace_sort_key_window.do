* V159: a CSV has no hidden Parquet row carrier. Replacing an ordering key
* must evaluate its own positional expression against the old complete order.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
parqit set threads 8
tempfile source oracle
set obs 1000000
gen long row = _n
gen int id = ceil(_n/1000)
gen long spell = mod(_n*37,1000003)
gen long u = mod(_n*97,1000003)
sort u
drop u
export delimited using "`source'.csv", replace
bysort id (spell): replace spell = _n
sort row
save "`oracle'", replace
forvalues rep=1/2 {
    parqit use using "`source'.csv"
    parqit bysort id (spell): replace spell = _n
    parqit by id: gen double first = spell[1]
    parqit by id: gen double position = _n
    parqit collect, clear
    assert first == 1 & spell == position
    drop first position
    sort row
    cf _all using "`oracle'"
    parqit close _all
}
* Tie resolution must be shared by the replacement and subsequent windows.
replace spell = mod(row,7)
export delimited using "`source'_ties.csv", replace
parqit use using "`source'_ties.csv"
parqit bysort id (spell): replace spell = _n
parqit by id: gen double first = spell[1]
parqit by id: gen double position = _n
parqit collect, clear
assert first == 1 & spell == position
parqit close _all
* Even when every declared sort key is replaced, the old physical order stays.
parqit use using "`source'_ties.csv"
parqit sort row
parqit replace row = -row
parqit gen double position = _n
parqit collect, clear
assert position == -row
assert "`: sortedby'" == ""
parqit close _all
display "VERDICT(V159_REPLACE_SORT_KEY_WINDOW): PASS - one million CSV rows match native replacement; later windows share the same order with ties and after every sort key is replaced"
