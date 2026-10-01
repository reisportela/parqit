* V145 — merge, append and joinby take the using data's notes and
*   characteristics as native Stata does (TWOTABLE-CHARS-1, 2026-10-01): a
*   characteristic the master lacks comes across (the master's wins on a
*   clash); the notes are appended after the master's, numbered on from its
*   note0, skipping a text it already has; nonotes keeps the other
*   characteristics only. Lazy merge/append/joinby dropped all of it, in
*   collect and in save; mergein/appendin run native merge/append. A dropped
*   variable's characteristics leave the view with it, as in native Stata, so
*   a later gen or using variable of that name starts clean.
* Oracles: native merge, append and joinby; pyarrow (the saved file's
*   parqit.chars).
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile stem
global V145 `"`stem'"'

* using: a key, a shared and a new variable; a gap, a repeated note and one
* differing only by case among its dataset notes
clear
input str3 id price region
"a" 10 1
"b" 20 2
"d" 40 3
end
label define reg 1 "North" 2 "Centre" 3 "South"
label values region reg
notes: shared note
notes: dup
notes: dup
notes: Shared note
notes: u to drop
notes: using only note
notes drop _dta in 5
char _dta[ucharonly] u-only
char _dta[both] from using
notes id: using id note
char id[ukey] using key char
notes price: shared price note
notes price: using price note
char price[uc] using price char
char price[both] using value
notes region: region note
char region[rc] region char
save `"${V145}_using.dta"', replace
parqit save `"${V145}_using.parquet"', data replace
notes: second using note
save `"${V145}_using2.dta"', replace
parqit save `"${V145}_using2.parquet"', data replace

* master, with a gap of its own
clear
input str3 id price x
"a" 1 7
"b" 2 8
"c" 3 9
end
notes: master note
notes: m to drop
notes: shared note
notes drop _dta in 2
char _dta[mcharonly] m-only
char _dta[both] from master
notes id: master id note
notes price: shared price note
notes price: master price note
char price[mc] master price char
char price[both] master value
save `"${V145}_master.dta"', replace
parqit save `"${V145}_master.parquet"', data replace

* every characteristic of _dta and of each variable, as one string (parqit's
* own _parqit_* bookkeeping, which parqit use sets, is not the data's)
program charsig, rclass
    unab vars : _all
    local vars : list sort vars
    local sig
    foreach o in _dta `vars' {
        local names : char `o'[]
        local names : list sort names
        foreach c of local names {
            if strpos("`c'", "_parqit_") == 1 continue
            local sig `"`sig'|`o'[`c']=`: char `o'[`c']'"'
        }
    }
    return local sig `"`sig'"'
end

* one case: native on the .dta files, then the lazy verb collected and saved
program check
    args native lazy
    use `"${V145}_master.dta"', clear
    `native'
    charsig
    local want `"`r(sig)'"'
    parqit use using `"${V145}_master.parquet"', name(t)
    `lazy'
    parqit save `"${V145}_saved.parquet"', replace
    parqit collect, clear
    charsig
    assert `"`r(sig)'"' == `"`want'"'
    parqit use using `"${V145}_saved.parquet"', clear
    charsig
    assert `"`r(sig)'"' == `"`want'"'
    parqit close _all
end

check `"merge 1:1 id using "${V145}_using.dta""' ///
      `"parqit merge 1:1 id using "${V145}_using.parquet""'
* the saved file itself, read by pyarrow, carries native merge's characteristics
use `"${V145}_master.dta"', clear
merge 1:1 id using `"${V145}_using.dta"'
charsig
local want `"`r(sig)'"'
python:
import json
import pyarrow.parquet as pq
from sfi import Macro
md = pq.read_metadata(Macro.getGlobal("V145") + "_saved.parquet").metadata
chars = json.loads(md[b"parqit.chars"])
owners = (["_dta"] if "_dta" in chars else []) + sorted(k for k in chars if k != "_dta")
sig = "".join("|%s[%s]=%s" % (o, c, chars[o][c]) for o in owners for c in sorted(chars[o]))
assert sig == Macro.getLocal("want"), (sig, Macro.getLocal("want"))
end

* a .dta using reaches the merge through a bridge, notes and all
check `"merge 1:1 id using "${V145}_using.dta""' ///
      `"parqit merge 1:1 id using "${V145}_using.dta""'
check `"merge 1:1 id using "${V145}_using.dta", nonotes"' ///
      `"parqit merge 1:1 id using "${V145}_using.parquet", nonotes"'
check `"merge 1:1 id using "${V145}_using.dta", keepusing(price)"' ///
      `"parqit merge 1:1 id using "${V145}_using.parquet", keepusing(price)"'
check `"append using "${V145}_using.dta""' ///
      `"parqit append using "${V145}_using.parquet""'
check `"append using "${V145}_using.dta", nonotes"' ///
      `"parqit append using "${V145}_using.parquet", nonotes"'
check `"append using "${V145}_using.dta", keep(price)"' ///
      `"parqit append using "${V145}_using.parquet", keep(price)"'
check `"append using "${V145}_using.dta" "${V145}_using2.dta""' ///
      `"parqit append using "${V145}_using.parquet" "${V145}_using2.parquet""'
check `"joinby id using "${V145}_using.dta""' ///
      `"parqit joinby id using "${V145}_using.parquet""'

* the using side as a view carries its characteristics too
use `"${V145}_master.dta"', clear
merge 1:1 id using `"${V145}_using.dta"'
charsig
local want `"`r(sig)'"'
parqit use using `"${V145}_using.parquet"', name(u)
parqit use using `"${V145}_master.parquet"', name(t)
parqit merge 1:1 id using view:u
parqit collect, clear
charsig
assert `"`r(sig)'"' == `"`want'"'
parqit close _all

* a dropped variable takes its notes along: a later one of that name starts clean
use `"${V145}_master.dta"', clear
drop price
merge 1:1 id using `"${V145}_using.dta"'
charsig
local want `"`r(sig)'"'
parqit use using `"${V145}_master.parquet"', name(t)
parqit drop price
parqit merge 1:1 id using `"${V145}_using.parquet"'
parqit collect, clear
charsig
assert `"`r(sig)'"' == `"`want'"'
parqit close _all
use `"${V145}_master.dta"', clear
drop price
generate price = 1
charsig
local want `"`r(sig)'"'
parqit use using `"${V145}_master.parquet"', name(t)
parqit drop price
parqit gen price = 1
parqit collect, clear
charsig
assert `"`r(sig)'"' == `"`want'"'
parqit close _all

* mergein and appendin run native merge and append: nonotes and nolabel reach them
use `"${V145}_master.dta"', clear
merge 1:1 id using `"${V145}_using.dta"', nonotes
charsig
local want `"`r(sig)'"'
use `"${V145}_master.dta"', clear
parqit mergein 1:1 id using `"${V145}_using.parquet"', nonotes
charsig
assert `"`r(sig)'"' == `"`want'"'
use `"${V145}_master.dta"', clear
append using `"${V145}_using.dta"', nonotes nolabel
charsig
local want `"`r(sig)'"'
use `"${V145}_master.dta"', clear
parqit appendin using `"${V145}_using.parquet"', nonotes nolabel
charsig
assert `"`r(sig)'"' == `"`want'"'
capture label list reg
assert _rc == 111

di "VERDICT(V145_TWO_TABLE_NOTES): PASS - lazy merge, append and joinby (collected and saved) take the using data's notes and characteristics as native Stata does (master wins a clash, notes appended after the master's from note0, repeated texts skipped, keepusing/keep() respected, files in order); nonotes keeps the other characteristics; a using view carries them; a dropped variable's notes do not pass to a later gen or using variable of that name; mergein/appendin pass nonotes/nolabel to native"
