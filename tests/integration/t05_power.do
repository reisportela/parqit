* M4: reshape long/wide vs native Stata, parqit sql / parqit query escape
* hatches, summarize/tabulate pushdowns, parqit path.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

* ---------- reshape long: parqit vs native ----------
clear
set obs 6
gen long id = _n
gen str2 grp = cond(mod(_n,2), "a", "b")
gen double inc2019 = 100 * _n
gen double inc2020 = 200 * _n
replace    inc2020 = . in 2
gen double exp2019 = 10 + _n
gen double exp2020 = 20 + _n
tempfile wbase
local widepq `"`wbase'.parquet"'
parqit save `"`widepq'"', replace
tempfile widedta
qui save `"`widedta'"'

qui reshape long inc exp, i(id) j(year)
sort id year
tempfile long_oracle
qui save `"`long_oracle'"'

clear
parqit use using `"`widepq'"'
parqit reshape long inc exp, i(id) j(year)
parqit collect, clear
sort id year
* native reshape may order columns differently; compare by name
foreach v in id grp year inc exp {
    confirm variable `v'
}
qui count
local n1 = r(N)
preserve
use `"`long_oracle'"', clear
qui count
assert r(N) == `n1'
restore
qui merge 1:1 id year using `"`long_oracle'"', assert(match) nogenerate ///
    update replace
assert _N == 12
* values equal (merge update replace would have changed nothing if equal;
* assert exact equality directly instead)
use `"`long_oracle'"', clear
rename inc inc_o
rename exp exp_o
tempfile lo2
qui save `"`lo2'"'
clear
parqit use using `"`widepq'"'
parqit reshape long inc exp, i(id) j(year)
parqit collect, clear
qui merge 1:1 id year using `"`lo2'"', assert(match) nogenerate
gen double dinc = abs(inc - inc_o)
gen double dexp = abs(exp - exp_o)
qui count if (dinc > 1e-12 & !missing(dinc)) | (inc == . & inc_o != .) | (inc != . & inc_o == .)
assert r(N) == 0
qui count if dexp > 1e-12 & !missing(dexp)
assert r(N) == 0

* unbalanced stubs are loud
clear
parqit use using `"`widepq'"'
parqit drop exp2020
capture parqit reshape long inc exp, i(id) j(year)
assert _rc != 0
parqit close

* ---------- reshape wide: parqit vs native ----------
use `"`long_oracle'"', clear
tempfile lbase
local longpq `"`lbase'.parquet"'
parqit open _data
parqit save `"`longpq'"', replace
parqit close

qui reshape wide inc exp, i(id) j(year)
sort id
tempfile wide_oracle
qui save `"`wide_oracle'"'

clear
parqit use using `"`longpq'"'
parqit drop grp
capture parqit reshape wide inc exp, i(id) j(year)
assert _rc == 0
parqit collect, clear
sort id
foreach v in inc2019 inc2020 exp2019 exp2020 {
    confirm variable `v'
}
qui merge 1:1 id using `"`wide_oracle'"', assert(match) nogenerate keepusing(inc2019 inc2020)
assert _N == 6

* extra variable not in i() is loud (Stata contract)
clear
parqit use using `"`longpq'"'
capture parqit reshape wide inc exp, i(id) j(year)
assert _rc != 0      // grp is in the way
parqit close

* (i, j) duplication is loud
clear
parqit use using `"`longpq'"'
parqit replace year = 2019
capture parqit reshape wide inc exp, i(id) j(year)
assert _rc != 0
parqit close

* ---------- parqit sql + parqit query ----------
parqit sql `"SELECT g, sum(v) AS sv FROM (SELECT range % 3 AS g, range AS v FROM range(30)) GROUP BY g"'
parqit count
assert r(N) == 3
parqit sort g
parqit collect, clear
assert _N == 3
qui summ sv
assert r(sum) == 435   // 0+...+29

* sql straight over a parquet file
parqit sql `"SELECT count(*) AS n FROM read_parquet('`widepq'')"', clear
assert _N == 1 & n[1] == 6

* parqit query fragment (qualify)
clear
parqit use using `"`longpq'"'
parqit sort id year
parqit query `"qualify row_number() over (partition by id order by year) = 1"'
parqit count
assert r(N) == 6
* a broken fragment is loud and leaves the prior view unchanged
capture parqit query `"qualify frobnicate("'
assert _rc != 0
parqit count
assert r(N) == 6
parqit close

* ---------- summarize / tabulate ----------
clear
parqit use using `"`widepq'"'
parqit summarize inc2019
assert r(N) == 6
assert reldif(r(mean), 350) < 1e-12
assert r(min) == 100 & r(max) == 600

parqit tabulate grp
assert r(N) == 6
assert r(r) == 2
parqit close

* ---------- parqit path ----------
parqit path `"`widepq'"'
assert r(exists) == 1
local p `"`r(path)'"'
assert strpos(`"`p'"', ".parquet") > 0

di "VERDICT(T05_POWER): PASS - reshape both ways vs native, sql/query, summarize/tabulate, path"
