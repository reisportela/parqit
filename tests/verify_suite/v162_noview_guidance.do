* V162 — NOVIEW-GUIDE-1: discover the memory route without changing either route.
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

* Check real output, including exact quoting, spacing and option abbreviations.
program define _v162_has, rclass
    args logfile needle
    tempname fh
    local found 0
    file open `fh' using `"`logfile'"', read text
    file read `fh' line
    while (!r(eof)) {
        if (strpos(`"`macval(line)'"', `"`macval(needle)'"')) local found 1
        file read `fh' line
    }
    file close `fh'
    return scalar found = `found'
end

sysuse auto, clear
tempfile master
quietly save `"`master'"'
preserve
keep make price
rename price x
parqit save "lookup with spaces.parquet", data
restore
parqit close _all

* A. The exact typed merge tokens survive; refusal leaves memory and views alone.
quietly log using merge_guidance.log, text name(v162)
capture noisily parqit merge m:1 make using "lookup with spaces.parquet", keepusing(x) nogen
local rc = _rc
quietly log close v162
assert `rc' == 198
_v162_has merge_guidance.log `"to merge the file into the data in memory: parqit mergein m:1 make using "lookup with spaces.parquet", keepusing(x) nogen"'
assert r(found) == 1
cf _all using `"`master'"', all
parqit views
assert r(n_views) == 0
display "VERDICT(V162_MERGE_GUIDANCE): PASS"

* B. Append preserves the user's option spelling and quoted source too.
quietly log using append_guidance.log, text name(v162)
capture noisily parqit append using "lookup with spaces.parquet", keep(make x) gen(source) nonotes
local rc = _rc
quietly log close v162
assert `rc' == 198
_v162_has append_guidance.log `"to append the file into the data in memory: parqit appendin using "lookup with spaces.parquet", keep(make x) gen(source) nonotes"'
assert r(found) == 1
cf _all using `"`master'"', all
parqit views
assert r(n_views) == 0
display "VERDICT(V162_APPEND_GUIDANCE): PASS"

* C. There is no native-memory parqit joinby twin.
quietly log using joinby_guidance.log, text name(v162)
capture noisily parqit joinby make using "lookup with spaces.parquet"
local rc = _rc
quietly log close v162
assert `rc' == 198
_v162_has joinby_guidance.log "parqit use using <file> (or parqit open _data)"
assert r(found) == 1
_v162_has joinby_guidance.log "then parqit joinby ..., parqit collect, clear"
assert r(found) == 1
_v162_has joinby_guidance.log "into the data in memory"
assert r(found) == 0
cf _all using `"`master'"', all
display "VERDICT(V162_JOINBY_GUIDANCE): PASS"

* D. No view refuses BEFORE a bridge import. A live view keeps the real error.
tempname junk
file open `junk' using x.dta, write text
file write `junk' "This is not a Stata dataset." _n
file close `junk'
foreach verb in merge append joinby {
    local keys "make"
    if ("`verb'" == "merge") local keys "m:1 make"
    if ("`verb'" == "append") local keys ""
    quietly log using `verb'_junk_closed.log, text name(v162)
    capture noisily parqit `verb' `keys' using x.dta
    local rc = _rc
    quietly log close v162
    assert `rc' == 198
    _v162_has `verb'_junk_closed.log "parqit `verb': no view is open"
    assert r(found) == 1
    _v162_has `verb'_junk_closed.log "not Stata format"
    assert r(found) == 0
    cf _all using `"`master'"', all
    parqit views
    assert r(n_views) == 0

    parqit use using "lookup with spaces.parquet"
    quietly log using `verb'_junk_live.log, text name(v162)
    capture noisily parqit `verb' `keys' using x.dta
    local rc = _rc
    quietly log close v162
    assert `rc' == 610
    _v162_has `verb'_junk_live.log "not Stata format"
    assert r(found) == 1
    _v162_has `verb'_junk_live.log "no view is open"
    assert r(found) == 0
    cf _all using `"`master'"', all
    parqit views
    assert r(n_views) == 1
    parqit count
    assert r(N) == 74
    parqit close _all
}
display "VERDICT(V162_REFUSAL_ORDER): PASS"

* E. No data in memory: only the out-of-core route is suggested.
clear
foreach verb in merge append {
    local keys ""
    if ("`verb'" == "merge") local keys "m:1 make"
    quietly log using `verb'_empty.log, text name(v162)
    capture noisily parqit `verb' `keys' using x.dta
    local rc = _rc
    quietly log close v162
    assert `rc' == 198
    _v162_has `verb'_empty.log "parqit use using <file> (or parqit open _data)"
    assert r(found) == 1
    _v162_has `verb'_empty.log "parqit `verb'in"
    assert r(found) == 0
    assert c(k) == 0 & _N == 0
}
parqit views
assert r(n_views) == 0
display "VERDICT(V162_EMPTY_MEMORY): PASS"

* A view: using side retains the plugin refusal; never suggest a file-only twin.
quietly use `"`master'"', clear
foreach verb in merge append joinby {
    local keys "make"
    if ("`verb'" == "merge") local keys "m:1 make"
    if ("`verb'" == "append") local keys ""
    quietly log using `verb'_view.log, text name(v162)
    capture noisily parqit `verb' `keys' using view:absent
    local rc = _rc
    quietly log close v162
    assert `rc' == 198
    _v162_has `verb'_view.log "no lazy view is open"
    assert r(found) == 1
    _v162_has `verb'_view.log "into the data in memory"
    assert r(found) == 0
}
parqit use using "lookup with spaces.parquet"
quietly log using named_view.log, text name(v162)
capture noisily parqit merge m:1 make using view:absent
local rc = _rc
quietly log close v162
assert `rc' == 198
_v162_has named_view.log "no view named absent is open"
assert r(found) == 1
parqit close _all
display "VERDICT(V162_VIEW_SOURCE_BACKSTOP): PASS"

* Multiple lazy sources: suggest a valid first appendin, with the same options.
quietly log using multiple_files.log, text name(v162)
capture noisily parqit append using "lookup with spaces.parquet" x.dta, keep(make x) gen(source)
local rc = _rc
quietly log close v162
assert `rc' == 198
_v162_has multiple_files.log "parqit appendin reads one file at a time"
assert r(found) == 1
_v162_has multiple_files.log `"parqit appendin using "lookup with spaces.parquet", keep(make x) gen(source)"'
assert r(found) == 1
cf _all using `"`master'"', all
parqit views
assert r(n_views) == 0
display "VERDICT(V162_MULTIPLE_FILES): PASS"

* Literal braces in filenames must not be interpreted as SMCL instructions.
quietly log using literal_tokens.log, text name(v162)
capture noisily parqit merge m:1 make using "lookup {part}.parquet",  keepusing(x)  nogen
local rc = _rc
quietly log close v162
assert `rc' == 198
_v162_has literal_tokens.log `"parqit mergein m:1 make using "lookup {part}.parquet",  keepusing(x)  nogen"'
assert r(found) == 1

* Execute the suggested commands against native independent-data oracles.
parqit mergein m:1 make using "lookup with spaces.parquet", keepusing(x) nogen
assert _N == 74 & x == price
quietly use `"`master'"', clear
parqit appendin using "lookup with spaces.parquet", keep(make x) gen(source) nonotes
assert _N == 148
assert source == 0 in 1/74
assert source == 1 in 75/148
assert x == price[_n-74] in 75/148
parqit views
assert r(n_views) == 0
display "VERDICT(V162_COPYABLE_COMMANDS): PASS"
display "VERDICT(V162_NOVIEW_GUIDANCE): PASS"
