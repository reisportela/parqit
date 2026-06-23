* Cross-view two-table verbs: `using view:<name>` lets merge/joinby/append
* take another open view as the using side — no materialisation anywhere.
* Checked against native Stata performing the same operations in memory.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

* ---------- fixtures ----------
clear
set obs 24
gen long id    = ceil(_n / 4)            // ids 1..6
gen int  year  = 2017 + mod(_n, 4)
gen double wage = 10 * _n + mod(_n, 3)
replace wage = . in 5
tempfile w1
local workers `"`w1'.parquet"'
parqit save `"`workers'"', replace
tempfile wdta
qui save `"`wdta'"'

* native oracle: per-id mean wage over the filtered panel, merged back
use `"`wdta'"', clear
keep if wage < 200
preserve
collapse (mean) mw = wage (count) nw = wage, by(id)
tempfile udta
qui save `"`udta'"'
restore
qui merge m:1 id using `"`udta'"'
qui count if _merge == 3
local o_m3 = r(N)
qui summ mw if _merge == 3
local o_mwsum = r(sum)

* ---------- the same thing across two views ----------
clear
parqit use using `"`workers'"', name(panel)
parqit keep if wage < 200

parqit use using `"`workers'"', name(stats)
parqit keep if wage < 200
parqit collapse (mean) mw=wage (count) nw=wage, by(id)

parqit view panel
parqit merge m:1 id using view:stats
parqit collect, clear
qui count if _merge == 3
assert r(N) == `o_m3'
qui summ mw if _merge == 3
assert reldif(r(sum), `o_mwsum') < 1e-12

* both views still open and intact after the merge + collect
parqit views
assert r(n_views) == 2
parqit view stats
parqit count
assert r(N) == 5      // ids 1..5 survive wage<200 (id 6's wages are all above)

* ---------- uniqueness contract applies to view-using too ----------
parqit use using `"`workers'"', name(dups)   // ids repeat → not unique
parqit view panel
capture parqit merge m:1 id using view:dups
assert _rc != 0

* ---------- joinby across views ----------
parqit use using `"`workers'"', name(pats)
parqit keep if year == 2018
parqit keep id wage
parqit rename wage pw
parqit view panel
parqit joinby id using view:pats
parqit count
local jn = r(N)
assert `jn' > 0

use `"`wdta'"', clear
keep if wage < 200
tempfile mast2
qui save `"`mast2'"'
use `"`wdta'"', clear
keep if year == 2018
keep id wage
rename wage pw
tempfile pat2
qui save `"`pat2'"'
use `"`mast2'"', clear
qui joinby id using `"`pat2'"'
assert _N == `jn'

* ---------- append a view (and mix with a file) ----------
clear
set obs 3
gen long id = 100 + _n
gen double extra = _n
tempfile xf
local xfile `"`xf'.parquet"'
parqit save `"`xfile'"', replace data

parqit use using `"`workers'"', name(base)
parqit keep in 1/4
parqit use using `"`workers'"', name(tail)
parqit keep in 21/24
parqit view base
parqit append using view:tail `"`xfile'"', generate(src)
parqit count
assert r(N) == 11                         // 4 + 4 + 3
parqit collect, clear
qui count if src == 1
assert r(N) == 4
qui count if src == 2
assert r(N) == 3
qui count if extra != . & src == 2
assert r(N) == 3

* ---------- self-merge: a view used against itself ----------
parqit close _all
clear
parqit use using `"`workers'"', name(me)
parqit collapse (mean) mw=wage, by(id)
parqit merge 1:1 id using view:me
parqit count
assert r(N) == 6

* unknown view is loud
capture parqit merge m:1 id using view:ghost
assert _rc != 0
parqit close _all

di "VERDICT(T07_VIEW_USING): PASS - merge/joinby/append across views match native; contracts hold; self-merge works"
