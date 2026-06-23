* Named views: several lazy views side by side (frames-like vocabulary),
* collect does NOT consume, save is loud about its target with a data
* escape hatch, and the view prefix runs one-offs without switching.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

* ---------- fixtures (memory saves, no views yet) ----------
clear
set obs 12
gen long id = ceil(_n / 2)
gen double wage = 10 * _n
tempfile w1
local workers `"`w1'.parquet"'
parqit save `"`workers'"', replace

clear
set obs 6
gen long id = _n
gen double tfp = 100 * _n
tempfile f1
local firms `"`f1'.parquet"'
parqit save `"`firms'"', replace

* ---------- two views, independent pipelines ----------
clear
parqit use using `"`workers'"', name(qp)
assert "`r(view)'" == "qp"
parqit use using `"`firms'"', name(firms)
parqit views
assert r(n_views) == 2

* verbs hit the CURRENT view (firms); qp is untouched
parqit keep if tfp >= 300
parqit count
assert r(N) == 4

parqit view qp
parqit count
assert r(N) == 12
parqit keep if wage > 60
parqit count
assert r(N) == 6

* one-off prefix runs against firms and restores qp as current
parqit view firms: count
assert r(N) == 4
parqit count
assert r(N) == 6        // still qp, still filtered

* ---------- collect does not consume; views survive and re-execute ----------
parqit collect, clear
assert _N == 6 & r(N) == 6
parqit count               // the qp view is still there
assert r(N) == 6
parqit collect, clear      // re-executes the same pipeline
assert _N == 6
parqit views
assert r(n_views) == 2

* ---------- save targets: view by default (loud), memory with data ----------
tempfile o1 o2
local oview `"`o1'.parquet"'
local omem  `"`o2'.parquet"'
* memory currently holds the collected qp (id, wage); make it distinct
gen byte memmark = 1
parqit view firms
parqit save `"`oview'"', replace          // materialises view FIRMS
assert "`r(view)'" == "firms"
parqit save `"`omem'"', replace data      // exports the dataset in memory

preserve
parqit use using `"`oview'"', clear
assert c(k) == 2
confirm variable tfp
assert _N == 4
restore
preserve
parqit use using `"`omem'"', clear
confirm variable memmark
assert c(k) == 3 & _N == 6
restore

* ---------- close semantics ----------
parqit close firms
parqit views
assert r(n_views) == 1
capture parqit count                       // current was firms → gone, loud
assert _rc != 0
parqit view qp
parqit count
assert r(N) == 6
parqit close _all
parqit views
assert r(n_views) == 0
capture parqit count
assert _rc != 0

* unknown view name is loud
capture parqit view nope
assert _rc != 0

* ---------- open _data and sql take names too ----------
clear
set obs 5
gen x = _n
parqit open _data, name(mem)
parqit sql "SELECT range AS g FROM range(3)", name(sq)
parqit views
assert r(n_views) == 2
parqit view mem
parqit count
assert r(N) == 5
parqit view sq
parqit collect, clear
assert _N == 3
parqit close _all

di "VERDICT(T06_NAMED_VIEWS): PASS - multiple views, non-consuming collect, save targets, prefix and close semantics"
