* T15 — the dialog→ado contract: every command shape the ten parqit dialogs
* can emit is executed here against a synthetic fixture. Batch Stata cannot
* open a dialog, so this is the executable half of the point-and-click
* surface (the dialogs themselves are opened and driven in GUI Stata under
* Xvfb during a release; see ASSUMPTIONS #100). One block per dialog, in the
* order of the User > parqit menu; every shape must run with rc 0, and the
* shapes a dialog constrains or refuses before submitting (singular-variable
* reports, a pivot/collapse without a statistic, a lazy merge m:m) must be
* refused by the ado as well.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

local fail 0
program define _shape
    * run one command shape; a nonzero rc is a contract failure, reported and
    * counted rather than aborting so the whole surface is exercised
    args rc cmd
    if (`rc' != 0) {
        di as err `"  FAIL rc=`rc': `cmd'"'
        global T15_FAIL = ${T15_FAIL} + 1
    }
    else di as txt `"  ok: `cmd'"'
end
global T15_FAIL 0

* --- fixtures -----------------------------------------------------------------
tempfile stem
local auto   `"`stem'_auto.parquet"'
local autodta `"`stem'_auto.dta"'
local lookup `"`stem'_lookup.parquet"'
local long   `"`stem'_long.parquet"'
local wide   `"`stem'_wide.parquet"'
local out    `"`stem'_out.parquet"'
local out2   `"`stem'_out2.parquet"'
local part   `"`stem'_part"'
local copy   `"`stem'_copy.parquet"'

sysuse auto, clear
gen long id = _n
gen byte yr = 1 + (_n > 37)
gen str1 grp = cond(mod(_n, 2), "A", "B")
save `"`autodta'"', replace
parqit save `"`auto'"', replace

clear
set obs 2
gen byte foreign = _n - 1
gen double rate = _n / 4
parqit save `"`lookup'"', replace

clear
set obs 20
gen long id = ceil(_n / 2)
gen byte yr = 1 + mod(_n, 2)
gen double inc = _n * 10
parqit save `"`long'"', replace

* --- Read Parquet data (db parqit_read) ---------------------------------------
di as txt _n "db parqit_read"
capture noisily parqit use using `"`auto'"'
_shape `=_rc' "parqit use using file"
capture noisily parqit use make price using `"`auto'"', name(sub) relaxed
_shape `=_rc' "parqit use varlist using file, name() relaxed"
capture noisily parqit use using `"`auto'"', clear
_shape `=_rc' "parqit use using file, clear"
capture noisily parqit use using `"`autodta'"', encoding(latin1)
_shape `=_rc' "parqit use using file.dta, encoding()"
capture noisily parqit use using `"`autodta'"', clear encoding(latin1)
_shape `=_rc' "parqit use using file.dta, clear encoding()"
capture noisily parqit open _data
_shape `=_rc' "parqit open _data"
capture noisily parqit open _data, name(mem) encoding(latin1)
_shape `=_rc' "parqit open _data, name() encoding()"
capture noisily parqit path `"`auto'"'
_shape `=_rc' "parqit path file"
capture noisily parqit describe `"`auto'"'
_shape `=_rc' "parqit describe file (Describe button)"
* the Populate helper never raises: no dialog, a Parquet source, a CSV source
capture noisily parqit _dlgvars no_such_dlg vv_list using `"`auto'"'
local source_helper_rc = _rc
local source_helper_k = r(k)
local source_helper_vars `"`r(varlist)'"'
_shape `source_helper_rc' "parqit _dlgvars <dlg> <list> using parquet"
if (`source_helper_rc' == 0) {
    if (`source_helper_k' == 15 & strpos(`" `source_helper_vars' "', " make ") & ///
        strpos(`" `source_helper_vars' "', " price ")) {
        _shape 0 "parqit _dlgvars using returns the source variable contract"
    }
    else _shape 1 "parqit _dlgvars using must return all source variables"
}
capture noisily parqit _dlgvars no_such_dlg vv_list using `"`autodta'"'
_shape `=_rc' "parqit _dlgvars <dlg> <list> using dta (silently empty)"
capture noisily parqit _dlgvars no_such_dlg vv_list
_shape `=_rc' "parqit _dlgvars <dlg> <list> (current view)"
capture noisily parqit _dlgvars "x y" vv_list
_shape `=_rc' "parqit _dlgvars rejects an invalid class name defensively"
preserve
clear
set obs 1
gen long memonly = 1
gen double payload = 2
capture noisily parqit _dlgvars no_such_dlg vv_list, data
local data_helper_rc = _rc
local data_helper_k = r(k)
local data_helper_vars `"`r(varlist)'"'
if (`data_helper_rc' == 0 & `"`data_helper_vars'"' == "memonly payload" & ///
    `data_helper_k' == 2) {
    _shape 0 "parqit _dlgvars, data returns variables in Stata memory"
}
else _shape `=cond(`data_helper_rc', `data_helper_rc', 1)' ///
    "parqit _dlgvars, data must return variables in Stata memory"
restore
parqit close _all

* --- Describe and explore data (db parqit_explore) ----------------------------
di as txt _n "db parqit_explore"
parqit use using `"`auto'"'
capture noisily parqit describe
_shape `=_rc' "parqit describe"
capture noisily parqit describe `"`auto'"'
_shape `=_rc' "parqit describe file"
capture noisily parqit glimpse
_shape `=_rc' "parqit glimpse"
capture noisily parqit glimpse `"`auto'"'
_shape `=_rc' "parqit glimpse file"
capture noisily parqit ds
_shape `=_rc' "parqit ds"
capture noisily parqit lookfor make price
_shape `=_rc' "parqit lookfor words"
capture noisily parqit codebook
_shape `=_rc' "parqit codebook"
capture noisily parqit codebook price mpg
_shape `=_rc' "parqit codebook varlist"
capture noisily parqit head 5
_shape `=_rc' "parqit head 5"
capture noisily parqit list
_shape `=_rc' "parqit list"
capture noisily parqit list make price if price > 5000 in 1/5
_shape `=_rc' "parqit list varlist if exp in f/l"
capture noisily parqit list make price in 1/5
_shape `=_rc' "parqit list varlist in f/l"
capture noisily parqit count
_shape `=_rc' "parqit count"
capture noisily parqit count if foreign == 1
_shape `=_rc' "parqit count if exp"
capture noisily parqit misstable
_shape `=_rc' "parqit misstable"
capture noisily parqit misstable rep78 price
_shape `=_rc' "parqit misstable varlist"
capture noisily parqit misstable patterns rep78 price mpg
_shape `=_rc' "parqit misstable patterns varlist"
capture noisily parqit levelsof rep78
_shape `=_rc' "parqit levelsof var"
capture noisily parqit levelsof rep78, limit(100)
_shape `=_rc' "parqit levelsof var, limit()"
capture noisily parqit distinct
_shape `=_rc' "parqit distinct"
capture noisily parqit distinct foreign rep78, joint
_shape `=_rc' "parqit distinct varlist, joint"
capture noisily parqit duplicates report foreign
_shape `=_rc' "parqit duplicates report keys"
capture noisily parqit duplicates report foreign, limit(20)
_shape `=_rc' "parqit duplicates report keys, limit()"
capture noisily parqit duplicates list foreign, limit(20)
_shape `=_rc' "parqit duplicates list keys, limit()"

* --- Summary statistics, tables, and correlations (db parqit_stats) -----------
di as txt _n "db parqit_stats"
capture noisily parqit summarize
_shape `=_rc' "parqit summarize"
capture noisily parqit summarize price mpg, detail
_shape `=_rc' "parqit summarize varlist, detail"
capture noisily parqit tabulate foreign
_shape `=_rc' "parqit tabulate var"
capture noisily parqit tabulate foreign rep78, missing row col
_shape `=_rc' "parqit tabulate var1 var2, missing row col"
capture noisily parqit tabstat price mpg
_shape `=_rc' "parqit tabstat varlist (no statistics(): default mean)"
capture noisily parqit tabstat price mpg, statistics(n mean sd min max median sum var p90 range) by(foreign)
_shape `=_rc' "parqit tabstat varlist, statistics(...) by()"
capture noisily parqit correlate price mpg weight
_shape `=_rc' "parqit correlate varlist"
capture noisily parqit pwcorr price mpg weight, obs sig
_shape `=_rc' "parqit pwcorr varlist, obs sig"
capture noisily parqit histogram price, bins(10) nodraw
_shape `=_rc' "parqit histogram var, bins() nodraw"
capture noisily parqit tabulate foreign rep78 yr
if (_rc == 0) _shape 1 "parqit tabulate with three variables must be refused"
else _shape 0 "parqit tabulate with three variables refused (rc `=_rc')"
capture noisily parqit levelsof foreign rep78
if (_rc == 0) _shape 1 "parqit levelsof with two variables must be refused"
else _shape 0 "parqit levelsof with two variables refused (rc `=_rc')"
capture noisily parqit histogram price mpg, nodraw
if (_rc == 0) _shape 1 "parqit histogram with two variables must be refused"
else _shape 0 "parqit histogram with two variables refused (rc `=_rc')"

* --- Keep or drop observations, or draw a sample (db parqit_filter) ----------
di as txt _n "db parqit_filter"
capture noisily parqit keep if price > 4000
_shape `=_rc' "parqit keep if exp"
capture noisily parqit drop if mpg < 15
_shape `=_rc' "parqit drop if exp"
capture noisily parqit keep in 1/l
_shape `=_rc' "parqit keep in 1/l"
capture noisily parqit keep in 2/-1
_shape `=_rc' "parqit keep in 2/-1"
capture noisily parqit keep in f/10
_shape `=_rc' "parqit keep in f/10"
capture noisily parqit sample 50
_shape `=_rc' "parqit sample #"
capture noisily parqit sample 5, count seed(1)
_shape `=_rc' "parqit sample #, count seed()"
parqit close _all

* --- Keep, drop, order, sort, or rename variables (db parqit_vars) -----------
di as txt _n "db parqit_vars"
parqit use using `"`auto'"'
capture noisily parqit keep make price mpg foreign rep78 id yr grp
_shape `=_rc' "parqit keep varlist"
capture noisily parqit drop grp
_shape `=_rc' "parqit drop varlist"
capture noisily parqit order id make
_shape `=_rc' "parqit order varlist"
capture noisily parqit sort foreign price
_shape `=_rc' "parqit sort varlist"
capture noisily parqit gsort -price make
_shape `=_rc' "parqit gsort -var var"
capture noisily parqit rename (make price) (car cost)
_shape `=_rc' "parqit rename (oldlist) (newlist)"
capture noisily parqit sort foreign cost
_shape `=_rc' "parqit sort (before a keyed duplicates drop)"
capture noisily parqit duplicates drop foreign, force
_shape `=_rc' "parqit duplicates drop keys, force"
capture noisily parqit duplicates drop
_shape `=_rc' "parqit duplicates drop (entire observations)"
parqit close _all

* --- Create or change variables (db parqit_gen) -------------------------------
di as txt _n "db parqit_gen"
parqit use using `"`auto'"'
capture noisily parqit gen x = price * 2
_shape `=_rc' "parqit gen name = exp"
capture noisily parqit gen byte cheap = price < 5000 if mpg < 30
_shape `=_rc' "parqit gen type name = exp if exp"
capture noisily parqit gen str8 ini = substr(make, 1, 3)
_shape `=_rc' "parqit gen str# name = exp"
capture noisily parqit gen str17 s17 = make
_shape `=_rc' "parqit gen arbitrary str# type"
capture noisily parqit gen strL sl = make
_shape `=_rc' "parqit gen strL type"
capture noisily parqit egen double fm = mean(price), by(foreign)
_shape `=_rc' "parqit egen type name = fcn(exp), by()"
capture noisily parqit egen n = count(price)
_shape `=_rc' "parqit egen name = fcn(exp)"
foreach fcn in total mean sd min max count {
    capture noisily parqit egen e_`fcn' = `fcn'(price), by(foreign)
    _shape `=_rc' "parqit egen function `fcn'()"
}
capture noisily parqit replace price = . if price <= 0
_shape `=_rc' "parqit replace name = exp if exp"
parqit close _all

* --- Collapse, contract, pivot table, or reshape (db parqit_pivot) ------------
di as txt _n "db parqit_pivot"
parqit use using `"`auto'"'
capture noisily parqit collapse (mean) price (count) n=price, by(foreign)
_shape `=_rc' "parqit collapse (stat) var (stat) tgt=var, by()"
parqit close _all
parqit use using `"`auto'"'
capture noisily parqit collapse (mean) price (sum) tot=price (p50) med=mpg
_shape `=_rc' "parqit collapse two rows + additional specifications"
parqit close _all
parqit use using `"`auto'"'
capture noisily parqit pivot (mean) price (count) n=price, rows(foreign) cols(yr)
_shape `=_rc' "parqit pivot (stat) var (stat) tgt=var, rows() cols()"
parqit close _all
parqit use using `"`auto'"'
capture noisily parqit pivot, rows(foreign) cols(yr)
if (_rc == 0) _shape 1 "parqit pivot without a statistic must be refused"
else _shape 0 "parqit pivot without a statistic refused (rc `=_rc')"
capture noisily parqit collapse, by(foreign)
if (_rc == 0) _shape 1 "parqit collapse without a statistic must be refused"
else _shape 0 "parqit collapse without a statistic refused (rc `=_rc')"
capture noisily parqit contract foreign yr, freq(n)
_shape `=_rc' "parqit contract varlist, freq()"
parqit close _all
parqit use using `"`long'"'
capture noisily parqit reshape wide inc, i(id) j(yr)
_shape `=_rc' "parqit reshape wide stubs, i() j()"
capture noisily parqit save `"`wide'"', replace
_shape `=_rc' "parqit save (wide fixture)"
parqit close _all
parqit use using `"`wide'"'
capture noisily parqit reshape long inc, i(id) j(yr)
_shape `=_rc' "parqit reshape long stubs, i() j()"
parqit close _all

* --- Combine datasets (db parqit_combine) -------------------------------------
di as txt _n "db parqit_combine"
parqit use using `"`auto'"'
capture noisily parqit merge m:1 foreign using `"`lookup'"', keep(match master) keepusing(rate) generate(_m) encoding(latin1)
_shape `=_rc' "parqit merge m:1 keys using file, keep() keepusing() generate() encoding()"
parqit close _all
parqit use using `"`auto'"'
capture noisily parqit merge m:1 foreign using `"`lookup'"', nogenerate
_shape `=_rc' "parqit merge m:1 keys using file, nogenerate"
parqit close _all
parqit use using `"`auto'"', name(a)
capture noisily parqit merge m:m foreign using `"`lookup'"'
if (_rc == 0) _shape 1 "lazy parqit merge m:m must be refused"
else _shape 0 "lazy parqit merge m:m refused (rc `=_rc')"
parqit use using `"`lookup'"', name(b)
parqit view a
capture noisily parqit merge m:1 foreign using view:b, keep(match)
_shape `=_rc' "parqit merge m:1 keys using view:name, keep()"
parqit close _all
parqit use using `"`lookup'"'
capture noisily parqit merge 1:1 foreign using `"`lookup'"', nogenerate
_shape `=_rc' "parqit merge 1:1 keys using file"
parqit close _all
parqit use using `"`lookup'"'
capture noisily parqit merge 1:m foreign using `"`auto'"', nogenerate
_shape `=_rc' "parqit merge 1:m keys using file"
parqit close _all
parqit use using `"`auto'"'
capture noisily parqit append using `"`auto'"' `"`auto'"', generate(src) encoding(latin1)
_shape `=_rc' "parqit append using file file, generate() encoding()"
parqit close _all
parqit use using `"`auto'"'
capture noisily parqit joinby foreign using `"`lookup'"', encoding(latin1)
_shape `=_rc' "parqit joinby keys using file, encoding()"
parqit close _all
use `"`autodta'"', clear
capture noisily parqit mergein m:1 foreign using `"`lookup'"', keep(match master) keepusing(rate) generate(_mm) nolabel nonotes noreport
_shape `=_rc' "parqit mergein m:1 keys using file, keep() keepusing() generate() nolabel nonotes noreport"
use `"`autodta'"', clear
capture noisily parqit mergein m:1 foreign using `"`lookup'"', nogenerate assert(match master) update replace force
_shape `=_rc' "parqit mergein m:1 keys using file, nogenerate assert() update replace force"
use `"`autodta'"', clear
capture noisily parqit appendin using `"`auto'"', keep(make price) force
_shape `=_rc' "parqit appendin using file, keep() force"

* --- Collect into memory or save as Parquet (db parqit_write) -----------------
di as txt _n "db parqit_write"
parqit use using `"`auto'"'
parqit keep if foreign == 1
clear
capture noisily parqit collect
_shape `=_rc' "parqit collect"
capture noisily parqit collect, clear
_shape `=_rc' "parqit collect, clear"
capture noisily parqit save `"`out'"', replace compression(zstd) compression_level(3) chunk(2048) encoding(latin1)
_shape `=_rc' "parqit save file, replace compression() compression_level() chunk() encoding()"
capture noisily parqit save `"`part'"', replace partition_by(foreign)
_shape `=_rc' "parqit save dir, replace partition_by()"
capture noisily parqit save `"`out2'"', replace data
_shape `=_rc' "parqit save file, replace data (memory while a view is open)"
parqit close _all
parqit use using `"`auto'"', clear
capture noisily parqit save `"`copy'"', replace data copysource
_shape `=_rc' "parqit save file, replace data copysource"

* --- Views, SQL, and engine settings (db parqit_views) ------------------------
di as txt _n "db parqit_views"
parqit use using `"`auto'"', name(v1)
parqit use using `"`lookup'"', name(v2)
capture noisily parqit views
_shape `=_rc' "parqit views"
capture noisily parqit show
_shape `=_rc' "parqit show"
capture noisily parqit explain
_shape `=_rc' "parqit explain"
capture noisily parqit describe
_shape `=_rc' "parqit describe (report button)"
capture noisily parqit ds
_shape `=_rc' "parqit ds (report button)"
capture noisily parqit version
_shape `=_rc' "parqit version"
capture noisily parqit selftest
_shape `=_rc' "parqit selftest"
capture noisily parqit view v1
_shape `=_rc' "parqit view name"
capture noisily parqit view v2: count
_shape `=_rc' "parqit view name: command"
capture noisily parqit view v1: count if strpos(make, ":") >= 0
_shape `=_rc' "parqit view name: command containing quotes and a colon"
capture noisily parqit close v2
_shape `=_rc' "parqit close name"
capture noisily parqit close
_shape `=_rc' "parqit close"
capture noisily parqit sql "select 1 as one, 'a' as s"
_shape `=_rc' "parqit sql query"
capture noisily parqit sql "select 1 as one", name(q)
_shape `=_rc' "parqit sql query, name()"
capture noisily parqit sql `"select 2 as "two col", 'b' as s"', clear
_shape `=_rc' "parqit sql compound-quoted query (a double-quoted identifier), clear"
capture noisily parqit sql "select 3 as three;"
_shape `=_rc' "parqit sql query with a trailing semicolon"
capture noisily parqit sql `"select ';' as semi;"'
_shape `=_rc' "parqit sql preserves a semicolon inside a string literal"
capture noisily parqit sql ";;"
if (_rc == 0) _shape 1 "parqit sql containing only terminators must be refused"
else _shape 0 "parqit sql containing only terminators refused (rc `=_rc')"
parqit close _all
parqit use using `"`auto'"'
parqit sort yr price
* (raw SQL: a Stata name that is an SQL keyword, such as foreign, must be quoted by the user)
capture noisily parqit query "qualify row_number() over (partition by yr order by price) = 1"
_shape `=_rc' "parqit query fragment"
capture noisily parqit set statamissing on
_shape `=_rc' "parqit set statamissing on"
capture noisily parqit set statamissing off
_shape `=_rc' "parqit set statamissing off"
capture noisily parqit set threads 2
_shape `=_rc' "parqit set threads #"
capture noisily parqit set memory_limit 1GB
_shape `=_rc' "parqit set memory_limit value"
capture noisily parqit set tempdir `"`c(tmpdir)'"'
_shape `=_rc' "parqit set tempdir path"
capture noisily parqit close _all
_shape `=_rc' "parqit close _all"

if (${T15_FAIL} == 0) {
    di as result "VERDICT(T15_DIALOG_SHAPES): PASS — emitted command shapes run and dialog-constrained shapes are refused by the ado too"
}
else {
    di as err "VERDICT(T15_DIALOG_SHAPES): FAIL — ${T15_FAIL} shape(s) failed"
}
