* V67 — the runtime MESSAGE contract: what parqit prints, not just what it
* returns. release_lint.sh proves the help mentions every command; v66 proves
* the option sets and r() match it. Neither can see a message, and every
* defect pinned here was a message defect found by reading real output:
*   D1  _n/_N in count if / list if leaked the internal __PARQIT_ROW__ token
*       through a raw DuckDB Binder Error (rc 920) instead of refusing;
*   D2  head/list errors were attributed to "parqit collect";
*   D3  the lazy `use` line said "nothing read" while the help says the schema
*       IS read;
*   D4  `sort w*` was accepted when the wildcard matched exactly one column;
*   D5a a non-Parquet using side printed its import chatter and the temporary
*       bridge path;
*   D5b `sql …, clear` printed the candidate view's tempname (__000000);
*   D5c `summarize, detail` printed a stray "%" with no percentile;
*   D6  `describe` of a .csv/.dta gave the engine's raw error.
* Captured with a secondary named log so the assertions read the real output.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

* A text log wraps at linesize and marks the continuation with a leading "> ",
* which splits a needle in half ("no rows lo" / "aded"). Both helpers below
* therefore reassemble the LOGICAL line before testing it. linesize is widened
* too, so the reassembly has less work to do and stays readable in the log.
set linesize 255

* --- log-capture helper: does <needle> appear in <logfile>? -------------------
capture program drop _v67_grep
program define _v67_grep, rclass
    args logfile needle
    tempname fh
    local found 0
    local cur ""
    file open `fh' using `"`logfile'"', read text
    file read `fh' line
    while (!r(eof)) {
        if (substr(`"`macval(line)'"', 1, 2) == "> ") {
            local cur `"`macval(cur)'`=substr(`"`macval(line)'"', 3, .)'"'
        }
        else {
            if (strpos(`"`macval(cur)'"', `"`needle'"')) local found 1
            local cur `"`macval(line)'"'
        }
        file read `fh' line
    }
    if (strpos(`"`macval(cur)'"', `"`needle'"')) local found 1
    file close `fh'
    return scalar found = `found'
end

* Does any logical line end (ignoring trailing blanks) with a bare "%"? — D5c
capture program drop _v67_orphan_pct
program define _v67_orphan_pct, rclass
    args logfile
    tempname fh
    local found 0
    local cur ""
    file open `fh' using `"`logfile'"', read text
    file read `fh' line
    while (!r(eof)) {
        if (substr(`"`macval(line)'"', 1, 2) == "> ") {
            local cur `"`macval(cur)'`=substr(`"`macval(line)'"', 3, .)'"'
        }
        else {
            local t = strtrim(`"`macval(cur)'"')
            if (`"`t'"' != "" & substr(`"`t'"', -1, 1) == "%") local found 1
            local cur `"`macval(line)'"'
        }
        file read `fh' line
    }
    local t = strtrim(`"`macval(cur)'"')
    if (`"`t'"' != "" & substr(`"`t'"', -1, 1) == "%") local found 1
    file close `fh'
    return scalar found = `found'
end

tempfile stem
local src    `"`stem'_src.parquet"'
local csv    `"`stem'_look.csv"'
local junk   `"`stem'_junk.dta"'
local lg     `"`stem'_probe.log"'

clear
set obs 20
gen long id = _n
gen double wage = _n * 1.5
gen int year = 2019 + mod(_n, 2)
parqit save `"`src'"', replace data
quietly outsheet id wage using `"`csv'"', comma replace

* ===================================================== D1 + D2: messages ====
parqit use using `"`src'"'
parqit sort id

quietly log using `"`lg'"', text name(v67) replace
capture noisily parqit count if _n <= 5
local rc_countif = _rc
capture noisily parqit list if _N > 1
local rc_listif = _rc
capture noisily parqit list in 50/60
local rc_listin = _rc
capture noisily parqit list nosuchvar
local rc_listvar = _rc
quietly log close v67

assert `rc_countif' == 198
assert `rc_listif'  == 198
assert `rc_listin'  == 198
assert `rc_listvar' == 111

* D1: parqit's own refusal, never the engine's internal placeholder
_v67_grep `"`lg'"' "__PARQIT_"
assert r(found) == 0
_v67_grep `"`lg'"' "Binder Error"
assert r(found) == 0
_v67_grep `"`lg'"' "parqit count: _n/_N are not supported"
assert r(found) == 1
_v67_grep `"`lg'"' "parqit list: _n/_N are not supported"
assert r(found) == 1

* D2: the preview path names the command the user typed, not "collect"
_v67_grep `"`lg'"' "parqit list: in 50/60 is out of range"
assert r(found) == 1
_v67_grep `"`lg'"' "parqit list: variable nosuchvar not found"
assert r(found) == 1
_v67_grep `"`lg'"' "parqit collect:"
assert r(found) == 0

* a refused read-only filter never touches the view
parqit count
assert r(N) == 20
parqit close _all

* D2 continued: head and collect keep their own names on the shared path
quietly log using `"`lg'"', text name(v67) replace
capture noisily parqit head 3
local rc_head = _rc
quietly log close v67
assert `rc_head' == 198
_v67_grep `"`lg'"' "parqit head:"
assert r(found) == 1
_v67_grep `"`lg'"' "parqit collect:"
assert r(found) == 0

* ============================================ D3: the lazy `use` message ====
quietly log using `"`lg'"', text name(v67) replace
parqit use using `"`src'"'
quietly log close v67
_v67_grep `"`lg'"' "no rows loaded"
assert r(found) == 1
_v67_grep `"`lg'"' "nothing read"
assert r(found) == 0

* ================================================ D4: sort/gsort wildcards ==
* `w*` matches exactly one column here (wage): the old count-based guard let
* that through, so this case is the regression, not a many-match pattern.
capture parqit sort w*
assert _rc == 198
capture parqit gsort -w*
assert _rc == 198
capture parqit gsort +w*
assert _rc == 198
capture parqit sort ye?r
assert _rc == 198
* explicit names still work, in both commands
parqit sort wage
assert _rc == 0
parqit gsort -wage id
assert _rc == 0
parqit close _all

* ================================== D5a: a bridged using side stays quiet ===
quietly log using `"`lg'"', text name(v67) replace
parqit use using `"`src'"'
parqit merge 1:1 id using `"`csv'"', keepusing(wage)
local rc_bridge = _rc
quietly log close v67
assert `rc_bridge' == 0
_v67_grep `"`lg'"' "bridge.parquet"
assert r(found) == 0
_v67_grep `"`lg'"' "_parqit_bridge_import"
assert r(found) == 0
* the merge really happened (quiet is not the same as skipped)
parqit count
assert r(N) == 20
parqit close _all

* ...and a broken source is still LOUD: quietly must not swallow the failure
quietly {
    file open v67fh using `"`junk'"', write replace text
    file write v67fh "this is not a dta file" _n
    file close v67fh
}
parqit use using `"`src'"'
quietly log using `"`lg'"', text name(v67) replace
capture noisily parqit merge 1:1 id using `"`junk'"'
local rc_junk = _rc
quietly log close v67
assert `rc_junk' != 0
_v67_grep `"`lg'"' "not Stata format"
assert r(found) == 1
* the refused merge left the view usable
parqit count
assert r(N) == 20
parqit close _all

* ======================================= D5b: sql , clear names the target ==
quietly log using `"`lg'"', text name(v67) replace
parqit sql `"select year, count(*) n from read_parquet('`src'') group by 1 order by 1"', clear
* r() must be read before `log close`, which resets it
local sql_view `"`r(view)'"'
quietly log close v67
assert _N == 2
assert `"`sql_view'"' == "default"
_v67_grep `"`lg'"' "view default remains open"
assert r(found) == 1
_v67_grep `"`lg'"' "__00"
assert r(found) == 0
parqit close _all

* ====================================== D5c: no orphan % in summarize detail =
parqit use using `"`src'"'
quietly log using `"`lg'"', text name(v67) replace
parqit summarize wage, detail
local sum_p99 = r(p99)
quietly log close v67
_v67_orphan_pct `"`lg'"'
assert r(found) == 0
* the ninth percentile is still printed (the fix drops the empty cell, not p99)
_v67_grep `"`lg'"' "99%"
assert r(found) == 1
assert `sum_p99' == 30
parqit close _all

* =========================== D6: describe of a non-Parquet source is didactic =
quietly log using `"`lg'"', text name(v67) replace
capture noisily parqit describe `"`csv'"'
local rc_dcsv = _rc
capture noisily parqit glimpse `"`csv'"'
local rc_gcsv = _rc
quietly log close v67
assert `rc_dcsv' == 198
assert `rc_gcsv' == 198
_v67_grep `"`lg'"' "reads Parquet footers only"
assert r(found) == 1
_v67_grep `"`lg'"' "parqit use using"
assert r(found) == 1
_v67_grep `"`lg'"' "Invalid Input Error"
assert r(found) == 0

* a .dta source too, and a real Parquet source still describes normally
capture parqit describe `"`junk'"'
assert _rc == 198
parqit describe `"`src'"'
assert _rc == 0
assert r(n_rows) == 20 & r(n_cols) == 3

* ========================= D7: join keys are validated before any query =====
* The uniqueness contracts reference the keys, so an unknown key used to reach
* DuckDB first and come back as a Binder Error quoting parqit's generated SQL.
* Both verbs must name the key and the side instead. Missing names use rc 111;
* incompatible key types use native rc 106. Every refusal leaves the view
* untouched.
local ukey `"`stem'_ukey.parquet"'
local skey `"`stem'_skey.parquet"'
clear
set obs 10
gen long id = _n
gen double rate = _n / 2
gen long other = _n
parqit save `"`ukey'"', replace data
clear
set obs 10
gen str6 id = "k" + string(_n)
gen double z = _n
parqit save `"`skey'"', replace data

parqit use using `"`src'"'
quietly log using `"`lg'"', text name(v67) replace
foreach k in 1:1 m:1 1:m {
    capture noisily parqit merge `k' nosuchkey using `"`ukey'"'
    assert _rc == 111
}
* present on one side only, in either direction, and inside a multi-key list
capture noisily parqit merge 1:1 wage using `"`ukey'"'
assert _rc == 111
capture noisily parqit merge 1:1 other using `"`ukey'"'
assert _rc == 111
capture noisily parqit merge m:1 id year using `"`ukey'"'
assert _rc == 111
* a key that exists on both sides but with different kinds
capture noisily parqit merge 1:1 id using `"`skey'"'
assert _rc == 106
* joinby answers the same way, not with a generic usage error
capture noisily parqit joinby nosuchkey using `"`ukey'"'
assert _rc == 111
capture noisily parqit joinby id using `"`skey'"'
assert _rc == 106
quietly log close v67

_v67_grep `"`lg'"' "Binder Error"
assert r(found) == 0
_v67_grep `"`lg'"' "__parqit_s"
assert r(found) == 0
_v67_grep `"`lg'"' "parqit merge: key nosuchkey not found in the master view"
assert r(found) == 1
_v67_grep `"`lg'"' "parqit merge: key wage not found in the using data"
assert r(found) == 1
_v67_grep `"`lg'"' "parqit merge: key id is numeric in master but string in using"
assert r(found) == 1
_v67_grep `"`lg'"' "parqit joinby: key nosuchkey not found in the master view"
assert r(found) == 1
_v67_grep `"`lg'"' "parqit joinby: key id is numeric in master but string in using"
assert r(found) == 1
* the engine verbs prefix their own message: exactly one "verb:" per line
_v67_grep `"`lg'"' "parqit joinby: joinby:"
assert r(found) == 0
_v67_grep `"`lg'"' "parqit merge: merge:"
assert r(found) == 0

* every refusal left the view exactly as it was
parqit count
assert r(N) == 20
* ...and a good merge on the same view still works
parqit merge 1:1 id using `"`ukey'"', keepusing(rate)
assert _rc == 0
parqit collect, clear
assert _N == 20
parqit close _all

* an unknown keepusing() variable is likewise named, not a Binder Error
parqit use using `"`src'"'
quietly log using `"`lg'"', text name(v67) replace
capture noisily parqit merge 1:1 id using `"`ukey'"', keepusing(nosuch)
local rc_keepusing = _rc
capture noisily parqit append using `"`ukey'"', generate(id)
local rc_appendgen = _rc
quietly log close v67
assert `rc_keepusing' != 0
assert `rc_appendgen' != 0
_v67_grep `"`lg'"' "Binder Error"
assert r(found) == 0
_v67_grep `"`lg'"' "parqit merge: keepusing variable nosuch not found"
assert r(found) == 1
_v67_grep `"`lg'"' "parqit append: append:"
assert r(found) == 0
parqit count
assert r(N) == 20
parqit close _all

di as result "VERDICT(V67_RUNTIME_MESSAGE_CONTRACT): PASS - refusals are parqit's own, messages name the right command and the offending key, bridges are quiet on success and loud on failure"
