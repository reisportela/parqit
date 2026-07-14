* M3: merge (all kinds, _merge, keep/keepusing/gen/nogen), append (union by
* name with markers), joinby — checked against native Stata on the same
* data. Using sides stay on disk throughout.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

* ---------- fixtures ----------
* master: worker-year
clear
set obs 12
gen long id   = ceil(_n / 2)      // ids 1..6, twice each
gen int  year = 2019 + mod(_n, 2)
gen double w  = 10 * _n
tempfile m1
local master `"`m1'.parquet"'
parqit save `"`master'"', replace
tempfile mdta
qui save `"`mdta'"'

* using: firm data for ids 1..4 and 9 (id 9 unmatched-using)
clear
set obs 5
gen long id = cond(_n <= 4, _n, 9)
gen double tfp = 100 * _n
gen str4 cls = "c" + string(_n)
label variable tfp "total factor productivity"
tempfile u1
local usingf `"`u1'.parquet"'
parqit save `"`usingf'"', replace
tempfile udta
qui save `"`udta'"'

* native oracle: m:1 merge
use `"`mdta'"', clear
qui merge m:1 id using `"`udta'"'
qui count if _merge == 3
local o_m3 = r(N)
qui count if _merge == 1
local o_m1 = r(N)
qui count if _merge == 2
local o_m2 = r(N)
qui summ tfp if _merge == 3
local o_tfpsum = r(sum)

* ---------- parqit merge m:1 ----------
clear
parqit use using `"`master'"'
parqit merge m:1 id using `"`usingf'"'
parqit collect, clear
qui count if _merge == 3
assert r(N) == `o_m3'
qui count if _merge == 1
assert r(N) == `o_m1'
qui count if _merge == 2
assert r(N) == `o_m2'
qui summ tfp if _merge == 3
assert reldif(r(sum), `o_tfpsum') < 1e-12
assert `"`: variable label tfp'"' == "total factor productivity"

* keep(match) + keepusing + nogenerate
clear
parqit use using `"`master'"'
parqit merge m:1 id using `"`usingf'"', keep(match) keepusing(tfp) nogenerate
parqit collect, clear
capture confirm variable _merge
assert _rc != 0
capture confirm variable cls
assert _rc != 0
assert _N == `o_m3'
confirm variable tfp

* uniqueness contract is loud: m:1 with duplicate using keys
clear
parqit use using `"`master'"'
capture parqit merge m:1 year using `"`master'"'
assert _rc != 0
parqit close

* 1:1 contract violation on master side
clear
parqit use using `"`master'"'
capture parqit merge 1:1 id using `"`usingf'"'
assert _rc != 0
parqit close

* ---------- append ----------
clear
set obs 3
gen long id = 100 + _n
gen double w = -_n
gen str3 src2only = "x" + string(_n)
tempfile a2
local app2 `"`a2'.parquet"'
parqit save `"`app2'"', replace

clear
parqit use using `"`master'"'
parqit append using `"`app2'"', generate(source)
parqit collect, clear
assert _N == 15
qui count if source == 0
assert r(N) == 12
qui count if source == 1
assert r(N) == 3
assert src2only != "" if source == 1
qui count if source == 1 & year != .
assert r(N) == 0          // year missing for appended rows

* type conflict is loud
* (the append view above is still open — collect no longer consumes it —
* so exporting this scratch dataset needs the explicit data option)
clear
set obs 2
gen str4 w = "oops"      // w is numeric in master
tempfile a3
local app3 `"`a3'.parquet"'
parqit save `"`app3'"', replace data
clear
parqit use using `"`master'"'
capture parqit append using `"`app3'"'
assert _rc != 0
parqit close

* ---------- joinby ----------
* patents: several per id
clear
set obs 7
gen long id = cond(_n <= 3, 1, cond(_n <= 5, 2, 33))
gen str6 pat = "p" + string(_n)
tempfile j1
local pats `"`j1'.parquet"'
parqit save `"`pats'"', replace
tempfile jdta
qui save `"`jdta'"'

use `"`mdta'"', clear
qui joinby id using `"`jdta'"'
local o_jn = _N
qui count if id == 1
local o_j1 = r(N)

clear
parqit use using `"`master'"'
parqit joinby id using `"`pats'"'
parqit collect, clear
assert _N == `o_jn'
qui count if id == 1
assert r(N) == `o_j1'

* ---------- lazy m:m refuses; native mergein m:m remains available ----------
clear
input long k double u
1 10
1 20
end
tempfile mmu mmu_b
local mmudta `"`mmu'.dta"'
qui save `"`mmudta'"'
local mmuf `"`mmu_b'.parquet"'
parqit open _data
parqit save `"`mmuf'"', replace
parqit close

clear
input long k double v
1 1
1 2
1 3
2 7
end
tempfile mmm
qui save `"`mmm'"'

qui merge m:m k using `"`mmudta'"'
sort k v
tempfile mm_oracle
qui save `"`mm_oracle'"'

use `"`mmm'"', clear
parqit open _data
capture noisily parqit merge m:m k using `"`mmuf'"'
assert _rc == 198
parqit collect, clear
cf _all using `"`mmm'"'
parqit close

use `"`mmm'"', clear
parqit mergein m:m k using `"`mmudta'"'
sort k v
cf _all using `"`mm_oracle'"'

di "VERDICT(T04_TWO_TABLE): PASS - lazy merge kinds/_merge/options, append, joinby; m:m refusal + native mergein control"
