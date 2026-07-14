* pq_to_parqit_common_workflows.do
* Lean comparison: pq -> parqit across the common functions. Requires the pq
* package to be installed (for a parqit-only introduction, start with
* examples/parqit_basics.do).
*
* For a development checkout, before running:
*   adopath ++ "/path/to/parqit/src/ado/p"
*   global PARQIT_PLUGIN_PATH "/path/to/parqit/build/dev/parqit.plugin"
*
* Run block 0 first. Blocks 1-6 can then be run separately.

clear all
set more off
set varabbrev off

* 0. Example data -------------------------------------------------------------

global PQP_ROOT "`c(tmpdir)'/pq_to_parqit_common"
capture mkdir "$PQP_ROOT"

global PQP_WORKERS "$PQP_ROOT/workers.parquet"
global PQP_FIRMS   "$PQP_ROOT/firms.parquet"
global PQP_EXTRA   "$PQP_ROOT/workers_extra.parquet"

global PQP_WORKERS_DTA "$PQP_ROOT/workers.dta"
global PQP_PQ_USE      "$PQP_ROOT/pq_use.dta"
global PQP_PARQIT_USE  "$PQP_ROOT/parqit_use.dta"
global PQP_PQ_SAVE     "$PQP_ROOT/pq_saved.parquet"
global PQP_PARQIT_SAVE "$PQP_ROOT/parqit_saved.parquet"
global PQP_PQ_SAVEDTA  "$PQP_ROOT/pq_saved.dta"
global PQP_PARQIT_SAVEDTA "$PQP_ROOT/parqit_saved.dta"
global PQP_PQ_MERGE    "$PQP_ROOT/pq_merge.dta"
global PQP_PARQIT_MERGE "$PQP_ROOT/parqit_merge.dta"
global PQP_PQ_APPEND   "$PQP_ROOT/pq_append.dta"
global PQP_PARQIT_APPEND "$PQP_ROOT/parqit_append.dta"

clear
set obs 120
gen long   id      = _n
gen int    firm_id = ceil(_n / 6)
gen int    year    = 2019 + mod(_n, 5)
gen double wage    = 900 + 7*_n + 15*mod(_n, 4)
gen str8   region  = cond(mod(firm_id, 3) == 0, "North", ///
                    cond(mod(firm_id, 3) == 1, "Centre", "South"))
gen byte   female  = mod(_n, 2)
label variable wage "monthly wage"
label define yesno 0 "No" 1 "Yes"
label values female yesno
save "$PQP_WORKERS_DTA", replace
parqit save "$PQP_WORKERS", replace data

clear
set obs 20
gen int    firm_id = _n
gen double tfp     = 1 + _n/10
gen double capital = 1000 * _n
gen str12  industry = "ind" + string(mod(_n, 4) + 1)
label variable tfp "total factor productivity"
parqit save "$PQP_FIRMS", replace data

clear
set obs 10
gen long   id      = 1000 + _n
gen int    firm_id = 20 + ceil(_n / 2)
gen int    year    = 2024
gen double wage    = 1400 + 11*_n
gen str8   region  = "New"
gen byte   female  = mod(_n, 2)
label values female yesno
parqit save "$PQP_EXTRA", replace data

* 1. Describe ----------------------------------------------------------------

pq describe using "$PQP_WORKERS", quietly
global PQP_PQ_ROWS = real("`r(n_rows)'")
global PQP_PQ_COLS = real("`r(n_columns)'")

parqit describe "$PQP_WORKERS"
assert r(n_rows) == $PQP_PQ_ROWS
assert r(n_cols)  == $PQP_PQ_COLS

* 2. Path --------------------------------------------------------------------

pq path "$PQP_WORKERS"
global PQP_PQ_PATH "`r(fullpath)'"

parqit path "$PQP_WORKERS"
global PQP_PARQIT_PATH "`r(path)'"
assert "$PQP_PQ_PATH" == "$PQP_PARQIT_PATH"

* 3. Reading: pq use vs parqit use + collect --------------------------------

pq use using "$PQP_WORKERS", clear
sort id
save "$PQP_PQ_USE", replace

clear
parqit use using "$PQP_WORKERS"
parqit collect, clear
sort id
save "$PQP_PARQIT_USE", replace

cf _all using "$PQP_PQ_USE"

* 4. Writing: pq save vs parqit save ..., data -------------------------------

use "$PQP_WORKERS_DTA", clear
pq save using "$PQP_PQ_SAVE", replace

use "$PQP_WORKERS_DTA", clear
parqit save "$PQP_PARQIT_SAVE", replace data

pq use using "$PQP_PQ_SAVE", clear
sort id
save "$PQP_PQ_SAVEDTA", replace

parqit use using "$PQP_PARQIT_SAVE"
parqit collect, clear
sort id
save "$PQP_PARQIT_SAVEDTA", replace

cf _all using "$PQP_PQ_SAVEDTA"

* 5. Merge -------------------------------------------------------------------
* pq: loads the master into memory; pq merge reads the using file.
* parqit: lazy master view; DuckDB filters and merges; collect at the end.

pq use using "$PQP_WORKERS", clear
keep if year == 2022
pq merge m:1 firm_id using "$PQP_FIRMS", keep(match) ///
    keepusing(tfp capital industry) nogenerate
sort id
save "$PQP_PQ_MERGE", replace

clear
parqit use using "$PQP_WORKERS"
parqit keep if year == 2022
parqit merge m:1 firm_id using "$PQP_FIRMS", keep(match) ///
    keepusing(tfp capital industry) nogenerate
parqit collect, clear
sort id
save "$PQP_PARQIT_MERGE", replace

cf _all using "$PQP_PQ_MERGE"

* 6. Append ------------------------------------------------------------------
* pq: loads the master into memory; pq append reads the extra file.
* parqit: lazy master view; DuckDB appends; collect at the end.

pq use using "$PQP_WORKERS", clear
pq append using "$PQP_EXTRA"
sort id
save "$PQP_PQ_APPEND", replace

clear
parqit use using "$PQP_WORKERS"
parqit append using "$PQP_EXTRA"
parqit collect, clear
sort id
save "$PQP_PARQIT_APPEND", replace

cf _all using "$PQP_PQ_APPEND"

* Rule of thumb --------------------------------------------------------------
*
* pq use using f, clear
*     -> parqit use using f
*        parqit collect, clear
*
* pq save using f, replace
*     -> parqit save f, replace data
*
* pq merge ... using f
*     -> parqit use/open master
*        parqit merge ... using f
*        parqit collect, clear
*
* pq append using f
*     -> parqit use/open master
*        parqit append using f
*        parqit collect, clear
*
* If the result does not need to enter memory:
*     parqit save result.parquet, replace
