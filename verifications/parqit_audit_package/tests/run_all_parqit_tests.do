* ============================================================================
* parqit adversarial test pack and usage template
*
* Usage from the PARQIT workspace root:
*     . do tests/run_all_parqit_tests.do
*
* Usage from another directory:
*     . do C:/path/to/PARQIT/tests/run_all_parqit_tests.do C:/path/to/PARQIT
*
* The pack ALWAYS exercises the most recent parqit, never a stale copy already on
* the adopath. To test a local working-tree build explicitly, pass the parqit
* repo root as the 2nd argument (and, optionally, an explicit .plugin as the 3rd):
*     . do .../run_all_parqit_tests.do . C:/path/to/parqit-repo
* With no 2nd argument it auto-detects a dev tree (this kit checked out under the
* repo's verifications/ folder, or ROOT itself being the repo root); failing that
* it net installs the LATEST GitHub release with `replace`.
*
* The script is intentionally self-contained: it creates synthetic data, writes
* and reads Parquet, downloads two small public datasets, exercises documented
* parqit verbs by topic, and checks representative adversarial failure paths.
* ============================================================================

version 16.0
clear all
set more off
set linesize 120
set varabbrev off
set seed 24062026

args ROOT PARQIT_REPO PARQIT_PLUGIN
if `"`ROOT'"' == "" local ROOT `"`c(pwd)'"'

local OUT  `"`ROOT'/tests/outputs"'
local SCRATCH_PARENT `"`OUT'/scratch path with spaces"'
local TEMPDIR_PARENT `"`OUT'/tempdir"'
* RUNID keys the scratch dir; date+time (not a bare HH:MM:SS) so back-to-back
* runs on the same day cannot collide on a timestamp that repeats within a second.
local RUNID "`c(current_date)' `c(current_time)'"
local RUNID : subinstr local RUNID ":" "", all
local RUNID : subinstr local RUNID "." "", all
local RUNID : subinstr local RUNID " " "", all
local DATA `"`SCRATCH_PARENT'/run_`RUNID'"'
local TEMPDIR `"`TEMPDIR_PARENT'/run_`RUNID'"'
capture mkdir `"`ROOT'/tests"'
capture mkdir `"`OUT'"'
capture mkdir `"`SCRATCH_PARENT'"'
capture mkdir `"`TEMPDIR_PARENT'"'
capture mkdir `"`DATA'"'
capture mkdir `"`TEMPDIR'"'
* `capture mkdir` hides real errors (permission, read-only volume, bad path);
* fail fast with a clear message rather than collapsing into confusing save
* errors later. Forward slashes work on every OS — Stata accepts them on Windows
* too, so no backslash conversion is needed (that broke `set tempdir` on
* macOS/Linux, where '\' is a literal filename character).
foreach _d in `"`DATA'"' `"`TEMPDIR'"' {
    mata: st_local("_okdir", strofreal(direxists(st_local("_d"))))
    if ("`_okdir'" != "1") {
        di as error "FATAL: cannot create scratch directory `_d' — check permissions / path"
        exit 693
    }
}

capture log close _all
local LOG `"`OUT'/parqit_adversarial_tests.log"'
log using `"`LOG'"', replace text

global PARQIT_AUDIT_FAILS 0

capture program drop _pq_section
program define _pq_section
    version 16.0
    args title
    di as txt _n "{hline 88}"
    di as txt "`title'"
    di as txt "{hline 88}"
end

capture program drop _pq_assert
program define _pq_assert
    version 16.0
    syntax, RC(integer) MSG(string)
    if (`rc' == 0) {
        di as result "PASS: `msg'"
    }
    else {
        di as error "FAIL: `msg' (rc=`rc')"
        local failn = real("${PARQIT_AUDIT_FAILS}") + 1
        global PARQIT_AUDIT_FAILS `failn'
    }
end

capture program drop _pq_expect_fail
program define _pq_expect_fail
    version 16.0
    syntax, RC(integer) MSG(string)
    if (`rc' != 0) {
        di as result "PASS: `msg' failed loudly as expected (rc=`rc')"
    }
    else {
        di as error "FAIL: `msg' unexpectedly succeeded"
        local failn = real("${PARQIT_AUDIT_FAILS}") + 1
        global PARQIT_AUDIT_FAILS `failn'
    }
end

_pq_section "0. Environment, installation, and package identity"

* ---------------------------------------------------------------------------
* parqit under test — ALWAYS the most recent build, never a stale adopath copy.
* Priority:
*   1. explicit dev tree: PARQIT_REPO (2nd arg) + optional PARQIT_PLUGIN (3rd).
*   2. an auto-detected dev tree (this kit checked out under <repo>/verifications,
*      or ROOT itself being the repo root).
*   3. otherwise net install the LATEST GitHub release with `replace`, which
*      overwrites any older copy already on the adopath.
* `discard` first so a plugin/ado already loaded this session cannot win.
* ---------------------------------------------------------------------------
capture discard

if `"`PARQIT_REPO'"' == "" {
    foreach cand in `"`ROOT'"' `"`ROOT'/.."' `"`ROOT'/../.."' `"`ROOT'/../../.."' {
        if (fileexists(`"`cand'/src/ado/p/parqit.ado"')) {
            local PARQIT_REPO `"`cand'"'
            continue, break
        }
    }
}

if `"`PARQIT_REPO'"' != "" {
    adopath ++ `"`PARQIT_REPO'/src/ado/p"'
    if `"`PARQIT_PLUGIN'"' == "" {
        foreach p in `"`PARQIT_REPO'/build/dev/parqit.plugin"' `"`PARQIT_REPO'/ado/plus/p/parqit.plugin"' {
            if (fileexists(`"`p'"')) {
                local PARQIT_PLUGIN `"`p'"'
                continue, break
            }
        }
    }
    if `"`PARQIT_PLUGIN'"' != "" global PARQIT_PLUGIN_PATH `"`PARQIT_PLUGIN'"'
    di as txt "parqit under test: local dev tree `PARQIT_REPO'"
    if `"`PARQIT_PLUGIN'"' != "" di as txt "  plugin: `PARQIT_PLUGIN'"
}
else {
    local release_url `"https://github.com/reisportela/parqit/releases/latest/download"'
    di as txt "no dev tree found; net install LATEST release from `release_url'"
    capture noisily net install parqit, from(`"`release_url'"') replace
    local rc = _rc
    _pq_assert, rc(`rc') msg("net install latest parqit release")
}

which parqit
findfile parqit.sthlp
parqit version
local tested_version `"`r(parqit_version)'"'
di as txt "tested parqit version: `tested_version'"

capture noisily parqit selftest
local rc = _rc
_pq_assert, rc(`rc') msg("parqit selftest")
capture assert `"`r(selftest)'"' == "ok"
local rc = _rc
_pq_assert, rc(`rc') msg("selftest stores r(selftest)==ok")

* ---------------------------------------------------------------------------
* The test body (sections 1-9) runs inside a captured program so that a HARD
* error in any single verb (a regression that aborts rather than asserting
* false) becomes one counted FAIL and the final VERDICT still prints, instead
* of the do-file dying mid-run with no verdict. Globals carry the two scratch
* paths into the program's local scope; everything else is local to the body.
* ---------------------------------------------------------------------------
global PQ_DATA    `"`DATA'"'
global PQ_TEMPDIR `"`TEMPDIR'"'
capture program drop _pq_body
program define _pq_body
    version 16.0
    local DATA    `"$PQ_DATA"'
    local TEMPDIR `"$PQ_TEMPDIR"'

_pq_section "1. Synthetic source data"

clear
set obs 240
gen long id = ceil(_n/3)
gen int year = 2019 + mod(_n - 1, 3)
gen int firm_id = mod(id - 1, 20) + 1
gen byte education = mod(id, 4) + 1
gen str6 gender = cond(mod(id, 2) == 0, "F", "M")
gen str8 region = cond(firm_id <= 5, "North", cond(firm_id <= 10, "Center", cond(firm_id <= 15, "South", "Islands")))
replace region = "" if mod(_n, 37) == 0
gen str8 sector = cond(mod(firm_id, 3) == 0, "tradable", cond(mod(firm_id, 3) == 1, "public", "other"))
gen double wage = round(exp(rnormal(2.5, .45))*10, .01)
replace wage = . if mod(_n, 17) == 0
replace wage = -abs(wage) if mod(_n, 71) == 0
gen double hours = 35 + mod(_n, 15)
gen double tenure = round(runiform()*12, .01)
gen double hire_date = td(01jan2015) + id + year - 2019
format hire_date %td
gen strL note = cond(mod(_n, 19) == 0, "long text note with spaces", "")
label variable wage "hourly wage"
label define edlbl 1 "basic" 2 "secondary" 3 "tertiary" 4 "phd"
label values education edlbl
char wage[source] "synthetic"
note tenure: tenure measured in years
note: generated by tests/run_all_parqit_tests.do
order id year firm_id wage hours tenure gender education region sector hire_date note
compress

local PANEL_DTA `"`DATA'/panel_native.dta"'
local PANEL_PQ  `"`DATA'/panel.parquet"'
save `"`PANEL_DTA'"', replace
parqit save `"`PANEL_PQ'"', replace data

quietly count
local N_panel = r(N)
quietly count if missing(wage)
local N_wage_missing = r(N)
quietly count if !missing(wage) & wage > 0
local N_positive_wage = r(N)
quietly summarize wage
local native_wage_mean = r(mean)
quietly count if region != ""
local N_region_nonempty = r(N)
quietly count if !missing(wage, hours, tenure, id, year, firm_id, education)
local native_complete_core = r(N)
quietly count if year == 2021
local N_2021 = r(N)
* distinct id count derived from the source (panel is still in memory; the
* parquet/dta were already saved above, so sorting here is harmless). Drives the
* duplicates surplus / dedup / qualify-row_number asserts instead of magic numbers.
tempvar idtag
bysort id: gen byte `idtag' = _n == 1
quietly count if `idtag'
local N_id_distinct = r(N)
local N_id_surplus  = `N_panel' - `N_id_distinct'
drop `idtag'

clear
set obs 20
gen int firm_id = _n
gen double tfp = round(50 + 3*_n + runiform(), .001)
gen str10 industry = cond(mod(_n, 2), "goods", "services")
gen byte exporter = mod(_n, 3) == 0
label variable tfp "firm productivity"
local FIRMS_DTA `"`DATA'/firms.dta"'
local FIRMS_PQ  `"`DATA'/firms.parquet"'
save `"`FIRMS_DTA'"', replace
parqit save `"`FIRMS_PQ'"', replace data

clear
set obs 40
gen int firm_id = ceil(_n/2)
gen int patent_id = _n
gen int patent_year = 2018 + mod(_n, 5)
local PATENTS_DTA `"`DATA'/patents.dta"'
local PATENTS_PQ  `"`DATA'/patents.parquet"'
save `"`PATENTS_DTA'"', replace
parqit save `"`PATENTS_PQ'"', replace data

clear
set obs 30
gen long pid = _n
gen str4 grp = cond(mod(_n, 2), "odd", "even")
gen double inc2019 = 1000 + _n
gen double inc2020 = 1100 + 2*_n
gen double inc2021 = 1200 + 3*_n
local WIDE_DTA `"`DATA'/wide_income.dta"'
local WIDE_PQ  `"`DATA'/wide_income.parquet"'
save `"`WIDE_DTA'"', replace
parqit save `"`WIDE_PQ'"', replace data

use `"`PANEL_DTA'"', clear
keep if year == 2019
local PART2019_PQ `"`DATA'/panel_2019.parquet"'
local PART2019_CSV `"`DATA'/panel_2019.csv"'
parqit save `"`PART2019_PQ'"', replace data
export delimited using `"`PART2019_CSV'"', replace

use `"`PANEL_DTA'"', clear
keep if year == 2020
local PART2020_PQ `"`DATA'/panel_2020.parquet"'
parqit save `"`PART2020_PQ'"', replace data

use `"`PANEL_DTA'"', clear
keep id year region sector
local PANEL_LIGHT_PQ `"`DATA'/panel_light_no_var_chars.parquet"'
parqit save `"`PANEL_LIGHT_PQ'"', replace data

use `"`PANEL_DTA'"', clear
gen double wage_clean = wage
drop wage
rename wage_clean wage
label variable wage "hourly wage"
order id year firm_id wage hours tenure gender education region sector hire_date note
local PANEL_NOCHAR_PQ `"`DATA'/panel_full_no_var_chars.parquet"'
parqit save `"`PANEL_NOCHAR_PQ'"', replace data

capture assert `N_panel' == 240
local rc = _rc
_pq_assert, rc(`rc') msg("synthetic panel has expected row count")

_pq_section "2. use, describe, path, metadata, and input formats"

parqit describe `"`PANEL_PQ'"'
capture assert r(n_rows) == `N_panel' & r(n_cols) >= 12 & r(has_parqit_meta) == 1
local rc = _rc
_pq_assert, rc(`rc') msg("describe reports rows, columns, and parqit metadata")

parqit glimpse `"`PANEL_PQ'"'
capture assert r(n_rows) == `N_panel'
local rc = _rc
_pq_assert, rc(`rc') msg("glimpse alias returns file metadata")

parqit path `"`PANEL_PQ'"'
capture assert r(exists) == 1 & `"`r(path)'"' != ""
local rc = _rc
_pq_assert, rc(`rc') msg("path resolves an existing file")

parqit use `"`PANEL_PQ'"', clear
capture assert _N == `N_panel' & c(k) >= 12
local rc = _rc
_pq_assert, rc(`rc') msg("use <parquet>, clear loads a Parquet file")
capture assert `"`: variable label wage'"' == "hourly wage" & `"`: label edlbl 3'"' == "tertiary"
local rc = _rc
_pq_assert, rc(`rc') msg("metadata round-trip preserves labels")
capture assert `"`: char wage[source]'"' == "synthetic"
local rc = _rc
_pq_assert, rc(`rc') msg("metadata round-trip preserves characteristics")

parqit use using `"`PANEL_DTA'"', clear
capture assert _N == `N_panel' & c(k) >= 12
local rc = _rc
_pq_assert, rc(`rc') msg("use using <dta>, clear bridges Stata data")

use `"`FIRMS_DTA'"', clear
local FIRMS_XLSX `"`DATA'/firms.xlsx"'
export excel using `"`FIRMS_XLSX'"', firstrow(variables) replace
parqit use using `"`FIRMS_XLSX'"', clear
capture assert _N == 20 & c(k) >= 4
local rc = _rc
_pq_assert, rc(`rc') msg("use using <xlsx>, clear bridges Excel")

parqit use using `"`PART2019_CSV'"', clear
capture assert _N == 80 & c(k) >= 12
local rc = _rc
_pq_assert, rc(`rc') msg("use using <csv>, clear scans delimited text")

_pq_section "3. Lazy views, view registry, and non-mutating exploration"

clear
parqit use using `"`PANEL_PQ'"', name(panel)
capture assert _N == 0 & c(k) == 0 & r(k) >= 12 & "`r(view)'" == "panel"
local rc = _rc
_pq_assert, rc(`rc') msg("lazy use opens a named view and leaves memory empty")

parqit use using `"`FIRMS_PQ'"', name(firms)
parqit views
capture assert r(n_views) == 2
local rc = _rc
_pq_assert, rc(`rc') msg("views lists multiple open named views")

parqit view panel
capture assert "`r(view)'" == "panel"
local rc = _rc
_pq_assert, rc(`rc') msg("view switches the current view")

parqit view firms: count
capture assert r(N) == 20
local rc = _rc
_pq_assert, rc(`rc') msg("view prefix runs one command against another view")

parqit describe
capture assert r(n_cols) >= 12 & r(n_steps) == 0
local rc = _rc
_pq_assert, rc(`rc') msg("describe current view returns lazy schema")

parqit ds
capture assert strpos("`r(varlist)'", "wage") > 0 & strpos("`r(varlist)'", "firm_id") > 0
local rc = _rc
_pq_assert, rc(`rc') msg("ds returns view variable list")

parqit lookfor wage firm
capture assert strpos("`r(varlist)'", "wage") > 0 | strpos("`r(varlist)'", "firm_id") > 0
local rc = _rc
_pq_assert, rc(`rc') msg("lookfor searches names and labels")

parqit count
capture assert r(N) == `N_panel'
local rc = _rc
_pq_assert, rc(`rc') msg("count materializes only row count")

parqit count if missing(wage)
capture assert r(N) == `N_wage_missing'
local rc = _rc
_pq_assert, rc(`rc') msg("count if supports missing()")

parqit list id year wage in 1/5
capture assert r(N) == 5
local rc = _rc
_pq_assert, rc(`rc') msg("list varlist in range previews rows")

parqit head 4
capture assert r(N) == 4
local rc = _rc
_pq_assert, rc(`rc') msg("head previews rows")

parqit summarize wage, detail
capture assert r(N) == `N_panel' - `N_wage_missing' & abs(r(mean) - `native_wage_mean') < 1e-8
local rc = _rc
_pq_assert, rc(`rc') msg("summarize, detail matches native mean and N")

parqit tabulate education
capture assert r(N) == `N_panel' & r(r) == 4
local rc = _rc
_pq_assert, rc(`rc') msg("tabulate one-way returns row count and categories")

parqit tabulate gender education, row col
capture assert r(N) == `N_panel' & r(r) >= 2 & r(c) == 4
local rc = _rc
_pq_assert, rc(`rc') msg("tabulate two-way accepts row/col and returns dimensions")

parqit misstable summarize wage hours tenure id year firm_id education
capture assert r(N) == `N_panel' & r(n_complete) == `native_complete_core'
local rc = _rc
_pq_assert, rc(`rc') msg("misstable returns complete-observation count")

parqit misstable patterns wage region tenure
capture assert r(r) > 0
local rc = _rc
_pq_assert, rc(`rc') msg("misstable patterns returns at least one pattern")

parqit levelsof education
capture assert r(r) == 4 & "`r(levels)'" == "1 2 3 4"
local rc = _rc
_pq_assert, rc(`rc') msg("levelsof numeric returns sorted levels")

parqit codebook wage region education
parqit distinct id year firm_id
capture assert r(N) == `N_panel' & r(ndistinct) > 0
local rc = _rc
_pq_assert, rc(`rc') msg("distinct returns stored counts")

parqit duplicates report id
capture assert r(N) == `N_panel' & r(surplus) == `N_id_surplus'
local rc = _rc
_pq_assert, rc(`rc') msg("duplicates report returns surplus")
parqit duplicates list id, limit(4)

parqit tabstat wage hours tenure, statistics(n mean sd min max p50)
parqit correlate wage hours tenure
capture assert r(N) > 0 & r(rho) < .
local rc = _rc
_pq_assert, rc(`rc') msg("correlate returns N and rho")
parqit pwcorr wage hours tenure, obs sig
capture assert r(N) > 0 & r(rho) < .
local rc = _rc
_pq_assert, rc(`rc') msg("pwcorr returns N and rho")
parqit histogram wage, bins(12) nodraw
capture assert r(N) == `N_panel' - `N_wage_missing' & r(bins) == 12
local rc = _rc
_pq_assert, rc(`rc') msg("histogram nodraw returns bin metadata")

capture assert _N == 0 & c(k) == 0
local rc = _rc
_pq_assert, rc(`rc') msg("exploration commands leave Stata memory empty")
parqit close _all

_pq_section "4. Single-table lazy verbs: keep, drop, gen, replace, egen, sort, sample"

clear
parqit use using `"`PANEL_PQ'"', name(panel)
parqit keep if wage > 0 & !missing(wage)
parqit gen double lwage = ln(wage)
parqit gen byte high_hours = hours >= 42
parqit egen double firm_wage = total(wage), by(firm_id year)
parqit replace note = "checked" if note == ""
parqit rename hours weekly_hours
parqit order id year firm_id wage lwage weekly_hours
parqit sort firm_id year id
parqit show
parqit explain
parqit count
capture assert r(N) == `N_positive_wage'
local rc = _rc
_pq_assert, rc(`rc') msg("single-table lazy pipeline count")

parqit summarize lwage
local lazy_lwage_sum = r(N) * r(mean)
use `"`PANEL_DTA'"', clear
keep if wage > 0 & !missing(wage)
gen double lwage = ln(wage)
quietly summarize lwage
local native_lwage_sum = r(sum)
capture assert abs(`lazy_lwage_sum' - `native_lwage_sum') < 1e-8
local rc = _rc
_pq_assert, rc(`rc') msg("gen ln(wage) matches native Stata aggregate")

clear
parqit use using `"`PANEL_PQ'"', name(panel)
parqit drop note hire_date
parqit drop if education == 1
parqit gsort -wage id year
parqit collect, clear
capture assert c(k) == 10 & education[1] != 1
local rc = _rc
_pq_assert, rc(`rc') msg("drop, drop if, and gsort collect")
parqit close _all

clear
parqit use using `"`PANEL_PQ'"'
parqit sort id year
parqit keep in 1/30
parqit sample 10, count seed(123)
parqit collect, clear
capture assert _N == 10
local rc = _rc
_pq_assert, rc(`rc') msg("keep in and sample, count")
parqit close _all

clear
parqit use using `"`PANEL_PQ'"'
parqit sort id year
parqit duplicates drop id, force
parqit count
capture assert r(N) == `N_id_distinct'
local rc = _rc
_pq_assert, rc(`rc') msg("duplicates drop after sort")
parqit close _all

clear
parqit use using `"`PANEL_PQ'"'
parqit contract region sector, freq(freq)
capture noisily parqit collect, clear
local rc = _rc
_pq_assert, rc(`rc') msg("contract drops char-bearing variables without internal metadata error")
if (`rc' == 0) {
    quietly summarize freq
    capture assert r(sum) == `N_panel' & c(k) == 3
    local rc2 = _rc
    _pq_assert, rc(`rc2') msg("contract with custom freq variable sums to source rows")
}
capture parqit close _all

clear
parqit use using `"`PANEL_LIGHT_PQ'"'
parqit contract region sector, freq(freq)
parqit collect, clear
quietly summarize freq
capture assert r(sum) == `N_panel' & c(k) == 3
local rc = _rc
_pq_assert, rc(`rc') msg("contract normal case with custom freq variable and empty strings")
parqit close _all

* ---- PARQIT-CHAR-01 reach: a projection that DROPS a char/note-bearing column
* must not abort materialisation. PANEL_PQ carries char wage[source] and a note
* on tenure; the cases below drop both via a column-subset use and a collapse.
* (Regression guard for the rc-3300 st_global abort; upstream pin is v39.)
clear
capture noisily parqit use id year region using `"`PANEL_PQ'"', clear
local rc = _rc
_pq_assert, rc(`rc') msg("subset use dropping char/note-bearing columns does not abort (PARQIT-CHAR-01)")
if (`rc' == 0) {
    capture assert _N == `N_panel' & c(k) == 3
    local rc = _rc
    _pq_assert, rc(`rc') msg("subset use returns exactly the requested columns")
}

clear
parqit use using `"`PANEL_PQ'"'
parqit collapse (mean) mhours = hours, by(region)
capture noisily parqit collect, clear
local rc = _rc
_pq_assert, rc(`rc') msg("collapse dropping a char-bearing column does not abort (PARQIT-CHAR-01)")
capture parqit close _all

_pq_section "5. collapse, reshape, save, collect, open _data, and partitioned output"

clear
parqit use using `"`PANEL_NOCHAR_PQ'"'
parqit keep if wage > 0 & !missing(wage)
parqit collapse (mean) mwage=wage (sum) total_hours=hours (count) n=wage, by(region sector)
parqit sort region sector
parqit collect, clear
tempfile lazy_collapse
save `"`lazy_collapse'"', replace

use `"`PANEL_DTA'"', clear
keep if wage > 0 & !missing(wage)
collapse (mean) mwage_native=wage (sum) total_hours_native=hours (count) n_native=wage, by(region sector)
sort region sector
merge 1:1 region sector using `"`lazy_collapse'"', assert(match) nogenerate
gen double d_mean = abs(mwage - mwage_native)
gen double d_hours = abs(total_hours - total_hours_native)
quietly summarize d_mean
local max_mean = r(max)
quietly summarize d_hours
local max_hours = r(max)
capture assert `max_mean' < 1e-8 & `max_hours' < 1e-8 & n == n_native
local rc = _rc
_pq_assert, rc(`rc') msg("collapse cells match native Stata")

clear
parqit use using `"`WIDE_PQ'"'
parqit reshape long inc, i(pid) j(year)
parqit count
capture assert r(N) == 90
local rc = _rc
_pq_assert, rc(`rc') msg("reshape long expands wide panel")
parqit reshape wide inc, i(pid grp) j(year)
parqit collect, clear
capture assert _N == 30 & inc2019[1] == 1001
local rc = _rc
_pq_assert, rc(`rc') msg("reshape wide returns original shape")
parqit close _all

clear
set obs 10000
gen long rid = _n
gen int group = mod(_n, 4)
local CHUNK_PQ `"`DATA'/chunked_zstd.parquet"'
parqit save `"`CHUNK_PQ'"', replace data compression(zstd) compression_level(3) chunk(2048)
parqit describe `"`CHUNK_PQ'"'
* row groups derive from the chunk size (ceil(rows/chunk)); assert the derivation
* rather than a hardcoded 5, so the test documents intent and tracks chunk().
capture assert r(n_rows) == 10000 & r(n_row_groups) == ceil(10000/2048)
local rc = _rc
_pq_assert, rc(`rc') msg("save, compression(), compression_level(), and chunk()")

clear
use `"`PANEL_DTA'"', clear
parqit open _data, name(memory_panel)
parqit keep if year == 2021
parqit keep id year firm_id wage
parqit collect, clear
capture assert _N == `N_2021' & c(k) == 4
local rc = _rc
_pq_assert, rc(`rc') msg("open _data promotes memory and collect materializes result")
gen byte from_memory = 1
local MEM_PQ `"`DATA'/memory_export.parquet"'
local VIEW_PQ `"`DATA'/view_export.parquet"'
parqit save `"`MEM_PQ'"', replace data
parqit save `"`VIEW_PQ'"', replace
parqit close _all
parqit describe `"`MEM_PQ'"'
local mem_cols = r(n_cols)
parqit describe `"`VIEW_PQ'"'
capture assert `mem_cols' == 5 & r(n_cols) == 4 & r(n_rows) == `N_2021'
local rc = _rc
_pq_assert, rc(`rc') msg("save, data exports memory while save without data materializes current view")

clear
parqit use using `"`PANEL_PQ'"'
parqit keep id year firm_id wage region
tempfile partbase
local PART_DIR `"`partbase'_partitioned_panel"'
parqit save `"`PART_DIR'"', replace partition_by(year)
parqit close _all
parqit use using `"`PART_DIR'"', clear
capture assert _N == `N_panel' & c(k) >= 5
local rc = _rc
_pq_assert, rc(`rc') msg("save, partition_by() writes and reads a Hive-style directory")

_pq_section "6. Two-table verbs: merge, append, joinby, mergein, appendin"

use `"`PANEL_DTA'"', clear
merge m:1 firm_id using `"`FIRMS_DTA'"', keep(match) keepusing(tfp industry) nogenerate
quietly count
local native_merge_N = r(N)
quietly summarize tfp
local native_tfp_sum = r(sum)

clear
parqit use using `"`PANEL_PQ'"'
parqit merge m:1 firm_id using `"`FIRMS_PQ'"', keep(match) keepusing(tfp industry) nogenerate
parqit count
capture assert r(N) == `native_merge_N'
local rc = _rc
_pq_assert, rc(`rc') msg("merge m:1 row count matches native Stata")
parqit summarize tfp
capture assert abs(r(N) * r(mean) - `native_tfp_sum') < 1e-8
local rc = _rc
_pq_assert, rc(`rc') msg("merge m:1 keepusing values match native aggregate")
parqit close _all

use `"`PANEL_DTA'"', clear
joinby firm_id using `"`PATENTS_DTA'"'
local native_join_N = _N

clear
parqit use using `"`PANEL_PQ'"'
parqit joinby firm_id using `"`PATENTS_PQ'"'
parqit count
capture assert r(N) == `native_join_N'
local rc = _rc
_pq_assert, rc(`rc') msg("joinby row count matches native Stata")
parqit close _all

clear
parqit use using `"`PART2019_PQ'"', name(y2019)
parqit use using `"`PART2020_PQ'"', name(y2020)
parqit view y2019
parqit append using view:y2020, generate(source_part)
parqit collect, clear
capture assert _N == 160 & source_part[1] < .
local rc = _rc
_pq_assert, rc(`rc') msg("append using view: aligns rows and generate()")
parqit close _all

use `"`PANEL_DTA'"', clear
parqit mergein m:1 firm_id using `"`FIRMS_PQ'"', keepusing(tfp industry) nogenerate
sort id year
tempfile got_mergein
save `"`got_mergein'"', replace
use `"`PANEL_DTA'"', clear
merge m:1 firm_id using `"`FIRMS_DTA'"', keepusing(tfp industry) nogenerate
sort id year
capture cf _all using `"`got_mergein'"'
local rc = _rc
_pq_assert, rc(`rc') msg("mergein equals native merge")

parqit use `"`PART2019_PQ'"', clear
parqit appendin using `"`PART2020_PQ'"'
sort id year
tempfile got_appendin
save `"`got_appendin'"', replace

use `"`PANEL_DTA'"', clear
keep if year == 2019
tempfile part2019_dta
save `"`part2019_dta'"', replace
use `"`PANEL_DTA'"', clear
keep if year == 2020
tempfile part2020_dta
save `"`part2020_dta'"', replace
use `"`part2019_dta'"', clear
append using `"`part2020_dta'"'
sort id year
capture cf _all using `"`got_appendin'"'
local rc = _rc
_pq_assert, rc(`rc') msg("appendin equals native append")

_pq_section "7. SQL escape hatches and settings"

clear
parqit sql `"SELECT region, sector, avg(wage) AS awage, count(*) AS n FROM read_parquet('`PANEL_PQ'') WHERE wage IS NOT NULL GROUP BY region, sector"', name(sqlview)
parqit count
capture assert r(N) > 0
local rc = _rc
_pq_assert, rc(`rc') msg("sql opens a lazy view")
parqit sort region sector
parqit head 3
parqit close _all

clear
parqit use using `"`PANEL_PQ'"'
parqit sort id year
parqit query "qualify row_number() over (partition by id order by year) = 1"
parqit count
capture assert r(N) == `N_id_distinct'
local rc = _rc
_pq_assert, rc(`rc') msg("query accepts a DuckDB SQL fragment")
parqit close _all

clear
parqit use using `"`PANEL_PQ'"'
parqit set threads 1
parqit set memory_limit 1GB
capture noisily parqit set tempdir `"`DATA'"'
local rc = _rc
_pq_assert, rc(`rc') msg("set tempdir accepts a path with spaces")
capture noisily parqit set tempdir `"`TEMPDIR'"'
local rc = _rc
_pq_assert, rc(`rc') msg("set tempdir accepts a simple workspace path")
parqit set statamissing on
quietly parqit count if wage > 0
local n_stata_missing = r(N)
parqit set statamissing off
quietly parqit count if wage > 0
local n_sql_missing = r(N)
capture assert `n_stata_missing' == `n_sql_missing' + `N_wage_missing'
local rc = _rc
_pq_assert, rc(`rc') msg("set statamissing on/off changes upper-tail missing semantics")
parqit close _all

_pq_section "8. Bounded internet downloads"

local AUTO_DTA `"`DATA'/public_auto.dta"'
capture copy "https://www.stata-press.com/data/r18/auto.dta" `"`AUTO_DTA'"', replace
local rc = _rc
if (`rc') {
    _pq_assert, rc(`rc') msg("download public Stata auto.dta")
}
else {
    parqit use using `"`AUTO_DTA'"', clear
    capture assert _N == 74 & c(k) >= 12
    local rc2 = _rc
    _pq_assert, rc(`rc2') msg("parqit reads downloaded Stata auto.dta")
    parqit save `"`DATA'/public_auto.parquet"', replace data
    parqit describe `"`DATA'/public_auto.parquet"'
    capture assert r(n_rows) == 74
    local rc2 = _rc
    _pq_assert, rc(`rc2') msg("downloaded auto.dta round-trips to Parquet")
}

local TIPS_CSV `"`DATA'/public_tips.csv"'
capture copy "https://raw.githubusercontent.com/mwaskom/seaborn-data/master/tips.csv" `"`TIPS_CSV'"', replace
local rc = _rc
if (`rc') {
    _pq_assert, rc(`rc') msg("download public tips.csv")
}
else {
    parqit use using `"`TIPS_CSV'"', clear
    capture assert _N == 244 & c(k) >= 7
    local rc2 = _rc
    _pq_assert, rc(`rc2') msg("parqit scans downloaded public CSV")
}

_pq_section "9. Adversarial expected-failure checks"

clear
set obs 2
gen sentinel = 100 + _n
capture noisily parqit use using `"`DATA'/definitely_not_here.parquet"', clear
local rc = _rc
_pq_expect_fail, rc(`rc') msg("missing file read")
capture assert _N == 2 & sentinel[1] == 101
local rc = _rc
_pq_assert, rc(`rc') msg("failed use, clear leaves existing memory intact")

clear
parqit use using `"`PANEL_PQ'"'
capture noisily parqit keep if frobnicate(wage)
local rc = _rc
_pq_expect_fail, rc(`rc') msg("unsupported expression function")
capture noisily parqit gen wage = 1
local rc = _rc
_pq_expect_fail, rc(`rc') msg("gen of existing variable")
capture noisily parqit gen byte bad_numeric = "abc"
local rc = _rc
_pq_expect_fail, rc(`rc') msg("numeric storage type with string expression")
capture noisily parqit gen d_bad = td(31feb2020)
local rc = _rc
_pq_expect_fail, rc(`rc') msg("impossible date literal")
capture noisily parqit keep in 200/9999
local rc = _rc
_pq_assert, rc(`rc') msg("keep in accepts range lazily before execution")
capture noisily parqit count
local rc = _rc
_pq_expect_fail, rc(`rc') msg("pipeline with invalid keep in fails at execution")
parqit close _all

clear
parqit use using `"`PANEL_PQ'"'
capture noisily parqit merge m:1 year using `"`FIRMS_PQ'"'
local rc = _rc
_pq_expect_fail, rc(`rc') msg("merge m:1 rejects non-unique using key")
parqit close _all

clear
parqit use using `"`PART2019_PQ'"'
capture noisily parqit append using `"`PART2020_PQ'"', generate(year)
local rc = _rc
_pq_expect_fail, rc(`rc') msg("append generate() colliding with an existing variable")
parqit close _all

clear
set obs 2
gen long id = _n
gen x = _n
parqit save `"`DATA'/no_replace_target.parquet"', replace data
capture noisily parqit save `"`DATA'/no_replace_target.parquet"', data
local rc = _rc
_pq_expect_fail, rc(`rc') msg("save refuses existing target without replace")

clear
parqit use using `"`PANEL_PQ'"'
capture noisily parqit save `"`DATA'/bad_codec.parquet"', replace compression(not_a_codec)
local rc = _rc
_pq_expect_fail, rc(`rc') msg("save rejects unknown compression codec")
parqit close _all
end

* Run the body; convert any uncaught abort into one counted failure so the
* verdict below always prints.
capture noisily _pq_body
if (_rc) {
    di as error "FAIL: test pack aborted with rc=`=_rc' before completing — a verb errored instead of asserting cleanly"
    local _failn = real("${PARQIT_AUDIT_FAILS}") + 1
    global PARQIT_AUDIT_FAILS `_failn'
}

_pq_section "10. Final verdict"

di as txt "log file: `LOG'"
di as txt "scratch data: `DATA'"
di as txt "failures: ${PARQIT_AUDIT_FAILS}"

if (${PARQIT_AUDIT_FAILS} == 0) {
    di as result "VERDICT(PARQIT_ADVERSARIAL_TEST_PACK): PASS"
    log close
    exit 0
}
else {
    di as error "VERDICT(PARQIT_ADVERSARIAL_TEST_PACK): FAIL (${PARQIT_AUDIT_FAILS} failures)"
    log close
    exit 459
}
