* ============================================================================
* parqit_clean_demo.do
* ----------------------------------------------------------------------------
* Clean, self-contained tour of parqit only.
*
* Run Sections 0 and 1 once. Later sections can then be run one at a time:
* every persistent folder, file and reference number is stored in a global.
* ============================================================================


* ============================================================================
* SECTION 0 - Setup
* ============================================================================
* Purpose: define a portable workspace rooted at the current directory, install
* parqit from the pinned release, and verify that the plugin/engine is usable.
version 16.0
clear all
set more off
set linesize 120
set varabbrev off
set seed 24062026

global PARQIT_NOTIPS 1

global PARQIT_DEMO_ROOT "`c(pwd)'"
global PARQIT_DEMO_ROOT : subinstr global PARQIT_DEMO_ROOT "\" "/", all
global PARQIT_DEMO_HOME "$PARQIT_DEMO_ROOT/clean"
global PARQIT_DEMO_DATA "$PARQIT_DEMO_HOME/data"
global PARQIT_DEMO_OUT  "$PARQIT_DEMO_HOME/output"
global PARQIT_DEMO_LOG  "$PARQIT_DEMO_HOME/logs"
global PARQIT_DEMO_TEMP "$PARQIT_DEMO_HOME/temp"

capture mkdir "$PARQIT_DEMO_HOME"
capture mkdir "$PARQIT_DEMO_DATA"
capture mkdir "$PARQIT_DEMO_OUT"
capture mkdir "$PARQIT_DEMO_LOG"
capture mkdir "$PARQIT_DEMO_TEMP"

* File manifest. Globals are intentional here: after Sections 0 and 1 are run,
* later sections can be executed independently from the Stata editor.
global PANEL_DTA       "$PARQIT_DEMO_DATA/workers_panel.dta"
global PANEL_PQ        "$PARQIT_DEMO_DATA/workers_panel.parquet"
global PANEL_CSV       "$PARQIT_DEMO_DATA/workers_panel.csv"
global PANEL_TSV       "$PARQIT_DEMO_DATA/workers_panel.tsv"
global PANEL_TXT       "$PARQIT_DEMO_DATA/workers_panel.txt"
global FIRMS_DTA       "$PARQIT_DEMO_DATA/firms.dta"
global FIRMS_PQ        "$PARQIT_DEMO_DATA/firms.parquet"
global FIRMS_XLSX      "$PARQIT_DEMO_DATA/firms.xlsx"
global PATENTS_DTA     "$PARQIT_DEMO_DATA/patents.dta"
global PATENTS_PQ      "$PARQIT_DEMO_DATA/patents.parquet"
global WIDE_DTA        "$PARQIT_DEMO_DATA/wide_income.dta"
global WIDE_PQ         "$PARQIT_DEMO_DATA/wide_income.parquet"
global PART2019_PQ     "$PARQIT_DEMO_DATA/workers_2019.parquet"
global PART2020_PQ     "$PARQIT_DEMO_DATA/workers_2020.parquet"
global PART2021_PQ     "$PARQIT_DEMO_DATA/workers_2021.parquet"
global PART2019_CSV    "$PARQIT_DEMO_DATA/workers_2019.csv"
global RELAX_A_PQ      "$PARQIT_DEMO_DATA/relaxed_a.parquet"
global RELAX_B_PQ      "$PARQIT_DEMO_DATA/relaxed_b.parquet"
global RELAX_GLOB      "$PARQIT_DEMO_DATA/relaxed_*.parquet"
global TYPEMAP_PQ      "$PARQIT_DEMO_DATA/type_map.parquet"
global SMALL_A_PQ      "$PARQIT_DEMO_DATA/small_a.parquet"
global SMALL_B_PQ      "$PARQIT_DEMO_DATA/small_b.parquet"

global VIEW_SAVE_PQ    "$PARQIT_DEMO_OUT/view_saved.parquet"
global CHUNK_PQ        "$PARQIT_DEMO_OUT/chunked_zstd.parquet"
global PARTITION_DIR   "$PARQIT_DEMO_OUT/partitioned_workers"
global MEMORY_DATA_PQ  "$PARQIT_DEMO_OUT/memory_data.parquet"
global MEMORY_VIEW_PQ  "$PARQIT_DEMO_OUT/memory_view.parquet"
global SQL_PQ          "$PARQIT_DEMO_OUT/sql_result.parquet"
global NOREPL_PQ       "$PARQIT_DEMO_OUT/no_replace_target.parquet"
global BADCODEC_PQ     "$PARQIT_DEMO_OUT/bad_codec.parquet"
global MISSING_PQ      "$PARQIT_DEMO_OUT/definitely_not_here.parquet"
global CODEC_SNAPPY_PQ "$PARQIT_DEMO_OUT/codec_snappy.parquet"
global CODEC_GZIP_PQ   "$PARQIT_DEMO_OUT/codec_gzip.parquet"
global CODEC_LZ4_PQ    "$PARQIT_DEMO_OUT/codec_lz4.parquet"
global CODEC_LZ4R_PQ   "$PARQIT_DEMO_OUT/codec_lz4_raw.parquet"
global CODEC_BROTLI_PQ "$PARQIT_DEMO_OUT/codec_brotli.parquet"
global CODEC_UNCOMP_PQ "$PARQIT_DEMO_OUT/codec_uncompressed.parquet"
global WAREHOUSE_PQ    "$PARQIT_DEMO_OUT/warehouse_types.parquet"

capture log close _all
log using "$PARQIT_DEMO_LOG/parqit_clean_demo.log", replace text

net install parqit, from("https://github.com/reisportela/parqit/releases/latest/download") replace
which parqit
parqit version
parqit selftest
assert "`r(selftest)'" == "ok"
parqit set tempdir "$PARQIT_DEMO_TEMP"


* ============================================================================
* SECTION 1 - Small fixtures
* ============================================================================
* Purpose: create small deterministic datasets that exercise parqit's I/O,
* metadata, lazy transformations, joins, appends, reshape and type mapping.
di as txt _n(2) "===== Section 1 - small fixtures ====="

* -- Main worker-year panel: values, labels, characteristics, notes and dates.
clear
set obs 240
gen long   id        = ceil(_n/3)
gen int    year      = 2019 + mod(_n - 1, 3)
gen int    firm_id   = mod(id - 1, 20) + 1
gen byte   education = mod(id, 4) + 1
gen str1   gender    = cond(mod(id, 2) == 0, "F", "M")
gen str8   region    = cond(firm_id <= 5, "North", cond(firm_id <= 10, "Center", cond(firm_id <= 15, "South", "Islands")))
replace    region    = "" if mod(_n, 37) == 0
gen str8   sector    = cond(mod(firm_id, 3) == 0, "tradable", cond(mod(firm_id, 3) == 1, "public", "other"))
gen double wage      = round(exp(rnormal(2.5, .45))*10, .01)
replace    wage      = .          if mod(_n, 17) == 0
replace    wage      = -abs(wage) if mod(_n, 71) == 0
gen double hours     = 35 + mod(_n, 15)
gen double tenure    = round(runiform()*12, .01)
gen double hire_date = td(01jan2015) + id + year - 2019
format hire_date %td
gen strL   note      = cond(mod(_n, 19) == 0, "long text note with spaces", "")
label variable wage "hourly wage"
label define edlbl 1 "basic" 2 "secondary" 3 "tertiary" 4 "phd", replace
label values education edlbl
char wage[source] "synthetic"
note tenure: tenure measured in years
note: generated by parqit_clean_demo.do
order id year firm_id wage hours tenure gender education region sector hire_date note
compress
save "$PANEL_DTA", replace
export delimited using "$PANEL_CSV", replace
export delimited using "$PANEL_TSV", replace delimiter(tab)
export delimited id year firm_id wage region sector using "$PANEL_TXT", replace delimiter("|")
parqit save "$PANEL_PQ", replace data

* -- Reference numbers used by later sections as compact correctness checks.
quietly count
global N_PANEL = r(N)
quietly count if missing(wage)
global N_WAGE_MISSING = r(N)
quietly count if !missing(wage) & wage > 0
global N_POSITIVE_WAGE = r(N)
quietly count if year == 2019
global N_2019 = r(N)
quietly count if year == 2020
global N_2020 = r(N)
quietly count if year == 2021
global N_2021 = r(N)
quietly count if year == 2021 & !missing(wage) & wage > 0
global N_2021_POSITIVE = r(N)
quietly summarize wage
global WAGE_MEAN = r(mean)
quietly count if !missing(wage, hours, tenure, id, year, firm_id, education)
global N_COMPLETE_CORE = r(N)
egen byte id_tag = tag(id)
quietly count if id_tag
global N_ID_DISTINCT = r(N)
drop id_tag
global N_ID_SURPLUS = $N_PANEL - $N_ID_DISTINCT

* -- Year slices used for append, appendin and partition demonstrations.
use "$PANEL_DTA", clear
keep if year == 2019
parqit save "$PART2019_PQ", replace data
export delimited using "$PART2019_CSV", replace

use "$PANEL_DTA", clear
keep if year == 2020
parqit save "$PART2020_PQ", replace data

use "$PANEL_DTA", clear
keep if year == 2021
parqit save "$PART2021_PQ", replace data

* -- Firm lookup table: small disk-side table for merge and mergein.
clear
set obs 20
gen int    firm_id  = _n
gen double tfp      = round(50 + 3*_n + runiform(), .001)
gen double capital  = round(1000 + 20*_n + runiform()*50, .001)
gen str10  industry = cond(mod(_n, 2), "goods", "services")
gen byte   exporter = mod(_n, 3) == 0
label variable tfp "firm productivity"
save "$FIRMS_DTA", replace
export excel using "$FIRMS_XLSX", firstrow(variables) replace
parqit save "$FIRMS_PQ", replace data

use "$PANEL_DTA", clear
merge m:1 firm_id using "$FIRMS_DTA", keep(match) keepusing(tfp industry) nogenerate
quietly count
global MERGE_N = r(N)
quietly summarize tfp
global MERGE_TFP_SUM = r(sum)

* -- Patent table: deliberately many-to-many within firm for joinby.
clear
set obs 40
gen int firm_id     = ceil(_n/2)
gen int patent_id   = _n
gen int patent_year = 2018 + mod(_n, 5)
save "$PATENTS_DTA", replace
parqit save "$PATENTS_PQ", replace data

use "$PANEL_DTA", clear
joinby firm_id using "$PATENTS_DTA"
global JOIN_N = _N

* -- Wide income table: minimal fixture for reshape long and reshape wide.
clear
set obs 30
gen long   pid     = _n
gen str4   grp     = cond(mod(_n, 2), "odd", "even")
gen double inc2019 = 1000 + _n
gen double inc2020 = 1100 + 2*_n
gen double inc2021 = 1200 + 3*_n
save "$WIDE_DTA", replace
parqit save "$WIDE_PQ", replace data

* -- Type-map fixture: numeric widths, strings, Stata dates/periods and missing.
clear
set obs 6
gen byte   smallbyte = _n
gen int    smallint  = 100 + _n
gen long   biglong   = 100000 + _n
gen float  fvalue    = _n + .25
gen double dvalue    = _n + .123456
gen str8   shortstr  = "row" + string(_n)
gen strL   longstr   = "valid utf-8 text with spaces"
gen double ddate     = td(01jan2020) + _n
format ddate %td
gen double tclock    = clock("01jan2020 12:30:00", "DMYhms") + 1000*_n
format tclock %tc
gen int    mdate     = tm(2020m1) + _n
format mdate %tm
gen int    qdate     = tq(2020q1) + _n
format qdate %tq
gen int    hdate     = th(2020h1) + mod(_n, 3)
format hdate %th
gen int    wdate     = tw(2020w1) + _n
format wdate %tw
gen int    ydate     = 2020 + _n
format ydate %ty
gen double extmiss   = _n
replace extmiss = .a in 1
label define extlbl .a "extended missing", replace
label values extmiss extlbl
parqit save "$TYPEMAP_PQ", replace data

* -- Relaxed glob fixture: files with different schemas unioned by column name.
clear
set obs 3
gen long id = _n
gen double x = 10*_n
parqit save "$RELAX_A_PQ", replace data

clear
set obs 3
gen long id = 3 + _n
gen double y = 100*_n
parqit save "$RELAX_B_PQ", replace data

* -- Small duplicated-key tables: minimal example for merge m:m semantics.
clear
set obs 3
gen byte key = cond(_n <= 2, 1, 2)
gen double aval = _n
parqit save "$SMALL_A_PQ", replace data

clear
set obs 4
gen byte key = cond(_n <= 2, 1, cond(_n == 3, 2, 3))
gen double bval = 10*_n
parqit save "$SMALL_B_PQ", replace data


* ============================================================================
* SECTION 2 - Use, input formats and metadata
* ============================================================================
* Purpose: show file introspection, eager reads with clear, column projection,
* bridged non-Parquet inputs, relaxed schema union and metadata restoration.
di as txt _n(2) "===== Section 2 - use, input formats and metadata ====="

* -- File-level introspection: schema, row groups, existence and absolute path.
parqit describe "$PANEL_PQ"
assert r(n_rows) == $N_PANEL & r(n_cols) == 12 & r(has_parqit_meta) == 1
parqit glimpse "$PANEL_PQ"
assert r(n_rows) == $N_PANEL
parqit path "$PANEL_PQ"
assert r(exists) == 1 & `"`r(path)'"' != ""

* -- Plain Parquet read: labels, value labels and characteristics round-trip.
parqit use "$PANEL_PQ", clear
assert _N == $N_PANEL & c(k) == 12
assert `"`: variable label wage'"' == "hourly wage"
assert `"`: label edlbl 3'"' == "tertiary"
assert `"`: char wage[source]'"' == "synthetic"

* -- Column projection at read time.
parqit use id year wage using "$PANEL_PQ", clear
assert _N == $N_PANEL & c(k) == 3

* -- Non-Parquet bridges: Stata .dta and Excel are imported then scanned.
parqit use using "$PANEL_DTA", clear
assert _N == $N_PANEL & c(k) == 12

parqit use using "$FIRMS_XLSX", clear
assert _N == 20 & c(k) == 5

* -- Delimited text sources: CSV, TSV and generic text with delimiter inference.
parqit use using "$PANEL_CSV", clear
assert _N == $N_PANEL & c(k) == 12

parqit use using "$PANEL_TSV", clear
assert _N == $N_PANEL & c(k) == 12

parqit use using "$PANEL_TXT", clear
assert _N == $N_PANEL & c(k) == 6

* -- Relaxed union: heterogeneous Parquet schemas are aligned by name.
parqit use using "$RELAX_GLOB", clear relaxed
assert _N == 6 & c(k) == 3

* -- Type mapping: check representative values, strings, formats and missings.
parqit use using "$TYPEMAP_PQ", clear
assert _N == 6 & c(k) == 15
confirm numeric variable smallbyte smallint biglong fvalue dvalue
assert smallbyte[1] == 1 & smallint[1] == 101 & biglong[6] == 100006
assert abs(fvalue[1] - 1.25) < 1e-6 & abs(dvalue[1] - 1.123456) < 1e-12
assert "`: type shortstr'" == "str8"
assert "`: type longstr'" == "strL"
assert "`: format ddate'" == "%td"
assert "`: format tclock'" == "%tc"
assert "`: format mdate'" == "%tm"
assert "`: format qdate'" == "%tq"
assert "`: format hdate'" == "%th"
assert "`: format wdate'" == "%tw"
assert "`: format ydate'" == "%ty"
assert missing(extmiss[1])


* ============================================================================
* SECTION 3 - Lazy views and exploration
* ============================================================================
* Purpose: open named views without loading data, inspect them, and compute
* previews/statistics pushed down to the engine while Stata memory stays empty.
di as txt _n(2) "===== Section 3 - lazy views and exploration ====="

* -- Named views: open, list, switch, run a one-off command and close by name.
clear
parqit use using "$PANEL_PQ", name(panel)
assert _N == 0 & c(k) == 0 & "`r(view)'" == "panel"
parqit use using "$FIRMS_PQ", name(firms)
parqit views
assert r(n_views) == 2
parqit view
assert r(n_views) == 2
parqit view panel
assert "`r(view)'" == "panel"
parqit view firms: count
assert r(N) == 20
parqit close firms
parqit views
assert r(n_views) == 1

* -- Schema exploration on the current lazy view.
parqit describe
assert r(n_cols) == 12 & r(n_steps) == 0
parqit ds
assert strpos("`r(varlist)'", "wage") > 0
parqit lookfor wage firm
assert strpos("`r(varlist)'", "wage") > 0 | strpos("`r(varlist)'", "firm_id") > 0

* -- Counts and previews: no rows are permanently materialised into memory.
parqit count
assert r(N) == $N_PANEL
parqit count if missing(wage)
assert r(N) == $N_WAGE_MISSING

parqit list id year wage in 1/5
assert r(N) == 5
parqit list id year wage if id <= 2 in 1/40
assert r(N) > 0
parqit head
assert r(N) == 5
parqit head 4
assert r(N) == 4

* -- Descriptive summaries, tabulations, missingness and codebook-style checks.
parqit summarize wage, detail
assert r(N) == $N_PANEL - $N_WAGE_MISSING & abs(r(mean) - $WAGE_MEAN) < 1e-8
parqit tabulate education
assert r(N) == $N_PANEL & r(r) == 4
parqit tabulate region, missing
assert r(N) == $N_PANEL
parqit tabulate gender education, row col
assert r(N) == $N_PANEL & r(c) == 4
parqit misstable summarize wage hours tenure id year firm_id education
assert r(N) == $N_PANEL & r(n_complete) == $N_COMPLETE_CORE
parqit misstable patterns wage region tenure
assert r(r) > 0
parqit levelsof education
assert r(r) == 4 & "`r(levels)'" == "1 2 3 4"
parqit levelsof education, limit(4)
assert r(r) == 4
parqit codebook wage region education

* -- Distinct values and duplicate-key diagnostics.
parqit distinct id year firm_id
assert r(N) == $N_PANEL & r(ndistinct) > 0
parqit distinct id year, joint
assert r(ndistinct) == $N_PANEL
parqit duplicates report id
assert r(N) == $N_PANEL & r(surplus) == $N_ID_SURPLUS
parqit duplicates list id, limit(4)

* -- Correlations and histogram summaries are computed by the engine.
parqit tabstat wage hours tenure, statistics(n mean sd min max p50)
parqit tabstat wage hours, statistics(n mean sd) by(region)
parqit correlate wage hours tenure
assert r(N) > 0 & r(rho) < .
parqit pwcorr wage hours tenure, obs sig
assert r(N) > 0 & r(rho) < .
parqit histogram wage, bins(12) nodraw
assert r(N) == $N_PANEL - $N_WAGE_MISSING & r(bins) == 12
parqit histogram wage, bins(8)
assert r(bins) == 8
assert _N == 0 & c(k) == 0
parqit close _all


* ============================================================================
* SECTION 4 - Single-table verbs
* ============================================================================
* Purpose: build lazy single-table pipelines with Stata-like verbs, then run
* them through count, summarize, collect and save-oriented materialisation.
di as txt _n(2) "===== Section 4 - single-table verbs ====="

* -- Standard lazy pipeline: filter, generate, egen, replace, rename, order, sort.
clear
parqit use using "$PANEL_PQ", name(panel)
parqit keep if wage > 0 & !missing(wage)
parqit gen double lwage = ln(wage)
parqit gen byte long_hours = hours >= 42
parqit egen double firm_year_wage = total(wage), by(firm_id year)
parqit replace note = "checked" if note == ""
parqit rename hours weekly_hours
parqit order id year firm_id wage lwage weekly_hours
parqit sort firm_id year id
parqit show
parqit explain
parqit count
assert r(N) == $N_POSITIVE_WAGE
parqit summarize lwage
assert r(N) == $N_POSITIVE_WAGE
parqit collect, clear
assert _N == $N_POSITIVE_WAGE & c(k) >= 14
assert !missing(lwage)
parqit close _all

* -- Expression grammar: arithmetic, logic, string functions, dates and _n/_N.
clear
parqit use using "$PANEL_PQ", name(expr)
parqit sort id year
parqit keep if id <= 20
parqit gen double seq = _n
parqit gen double total_n = _N
parqit gen byte high_wage = wage > 0 if !missing(wage)
parqit gen byte senior_like = cond(hours >= 42, 1, 0)
parqit gen double expr_num = abs(wage) + exp(0) + ln(abs(wage)+1) + ///
    log(abs(wage)+1) + log10(abs(wage)+1) + sqrt(hours) + floor(tenure) + ///
    ceil(tenure) + int(tenure) + round(tenure,.1) + mod(id,3) + ///
    min(hours,tenure) + max(hours,tenure)
parqit gen byte expr_logic = (wage > 0 | missing(wage) | mi(wage)) & ///
    inrange(hours,35,49) & inlist(education,1,2,3,4)
parqit gen str12 sector_upper = upper(sector)
parqit gen str12 sector_lower = lower(sector_upper)
parqit gen str8 trim_demo = trim("  ok  ")
parqit gen str8 ltrim_demo = ltrim("  ok")
parqit gen str8 rtrim_demo = rtrim("ok  ")
parqit gen double string_score = strlen(note) + ustrlen(note) + ///
    strpos(sector,"a") + regexm(sector,"tradable|public|other")
parqit gen str8 sector3 = substr(sector,1,3)
parqit gen str12 sector_sub = subinstr(sector,"tradable","trade",.)
parqit gen str12 wage_string = strofreal(wage) if !missing(wage)
parqit gen str12 wage_string2 = string(wage) if !missing(wage)
parqit gen double wage_real = real(wage_string)
parqit gen double date_score = year(hire_date) + month(hire_date) + ///
    day(hire_date) + quarter(hire_date) + dow(hire_date) + doy(hire_date) + ///
    mdy(1,1,2020) + dofm(mofd(hire_date)) + yofd(hire_date)
parqit gen double date_literals = td(29feb2020) + ///
    tc(01jan2020 00:00:59) + tC(01jan2020 00:00:59) + tm(2020m1) + ///
    tq(2020q1) + th(2020h1) + tw(2020w1) + ty(2020)
parqit collect, clear
assert _N == 60 & seq[1] == 1 & total_n[1] == 60
assert trim_demo[1] == "ok" & sector3[1] != ""
parqit close _all

* -- All documented egen functions with by().
clear
parqit use using "$PANEL_PQ"
parqit keep if firm_id <= 4
parqit egen double eg_total = total(wage), by(firm_id)
parqit egen double eg_mean  = mean(wage), by(firm_id)
parqit egen double eg_sd    = sd(wage), by(firm_id)
parqit egen double eg_min   = min(wage), by(firm_id)
parqit egen double eg_max   = max(wage), by(firm_id)
parqit egen double eg_count = count(wage), by(firm_id)
parqit collect, clear
assert _N > 0 & eg_count[1] < .
parqit close _all

* -- Pairwise rename syntax.
clear
parqit use using "$PANEL_PQ"
parqit keep id year wage hours
parqit rename (wage hours) (earn hourly_hours)
parqit collect, clear
confirm variable earn
confirm variable hourly_hours
parqit close _all

* -- Percent sampling: sample percentage, not row count.
clear
parqit use using "$PANEL_PQ"
parqit sample 25, seed(321)
parqit count
assert r(N) > 0 & r(N) < $N_PANEL
parqit close _all

* -- Drop variables, drop rows and descending gsort before collection.
clear
parqit use using "$PANEL_PQ"
parqit drop note hire_date
parqit drop if education == 1
parqit gsort -wage id year
parqit collect, clear
assert c(k) == 10 & education[1] != 1
parqit close _all

* -- Row-range slicing and fixed-size sampling.
clear
parqit use using "$PANEL_PQ"
parqit sort id year
parqit keep in 1/30
parqit sample 10, count seed(123)
parqit collect, clear
assert _N == 10
parqit close _all

* -- Duplicate dropping after an explicit deterministic sort.
clear
parqit use using "$PANEL_PQ"
parqit sort id year
parqit duplicates drop id, force
parqit count
assert r(N) == $N_ID_DISTINCT
parqit close _all

* -- Contract: grouped frequencies with a user-chosen frequency variable.
clear
parqit use using "$PANEL_PQ"
parqit contract region sector, freq(freq)
parqit collect, clear
quietly summarize freq
assert r(sum) == $N_PANEL & c(k) == 3
parqit close _all

* -- Collapse statistics beyond mean/sum/count, including first/last variants.
clear
parqit use using "$PANEL_PQ"
parqit sort firm_id year id
parqit keep if wage > 0 & !missing(wage)
parqit collapse (mean) mean_wage=wage (sum) sum_hours=hours ///
    (sd) sd_wage=wage (count) n=wage (min) min_wage=wage ///
    (max) max_wage=wage (median) median_wage=wage ///
    (p25) p25_wage=wage (p75) p75_wage=wage ///
    (first) first_wage=wage (last) last_wage=wage ///
    (firstnm) firstnm_wage=wage (lastnm) lastnm_wage=wage, by(region)
parqit collect, clear
quietly summarize n
assert r(sum) == $N_POSITIVE_WAGE
parqit close _all


* ============================================================================
* SECTION 5 - Collapse and reshape
* ============================================================================
* Purpose: show compact aggregate and reshape workflows that run lazily and only
* materialise the final result when collect/save is called.
di as txt _n(2) "===== Section 5 - collapse and reshape ====="

* -- Collapse: grouped cell means, sums, counts and percentiles.
clear
parqit use using "$PANEL_PQ"
parqit keep if wage > 0 & !missing(wage)
parqit collapse (mean) mwage=wage (sum) total_hours=hours (count) n=wage (p50) p50_wage=wage, by(region sector)
parqit sort region sector
parqit collect, clear
quietly summarize n
assert r(sum) == $N_POSITIVE_WAGE
parqit save "$PARQIT_DEMO_OUT/collapse_result.parquet", replace data
parqit close _all

* -- Reshape: long and wide transformations over a lazy view.
clear
parqit use using "$WIDE_PQ"
parqit reshape long inc, i(pid) j(year)
parqit count
assert r(N) == 90
parqit reshape wide inc, i(pid grp) j(year)
parqit collect, clear
assert _N == 30 & c(k) == 5
parqit close _all


* ============================================================================
* SECTION 6 - Save, partition and open _data
* ============================================================================
* Purpose: demonstrate the two write paths, compression/chunk controls,
* partitioned Parquet output, collect without clear and promotion of memory.
di as txt _n(2) "===== Section 6 - save, partition and open _data ====="

* -- View save: Parquet-to-Parquet output without loading the source into Stata.
clear
parqit use using "$PANEL_PQ"
parqit keep if year == 2021 & wage > 0
parqit keep id year firm_id wage region
parqit save "$VIEW_SAVE_PQ", replace compression(zstd) compression_level(3) chunk(2048)
parqit close _all
parqit describe "$VIEW_SAVE_PQ"
assert r(n_rows) == $N_2021_POSITIVE & r(n_cols) == 5

* -- Data save with zstd and explicit Parquet row-group sizing.
clear
set obs 10000
gen long rid = _n
gen byte group = mod(_n, 4)
parqit save "$CHUNK_PQ", replace data compression(zstd) compression_level(3) chunk(2048)
parqit describe "$CHUNK_PQ"
assert r(n_rows) == 10000 & r(n_row_groups) == ceil(10000/2048)

* -- Compression codecs documented by parqit save.
clear
set obs 12
gen long id = _n
gen double value = _n + .5
parqit save "$CODEC_SNAPPY_PQ", replace data compression(snappy)
parqit save "$CODEC_GZIP_PQ", replace data compression(gzip)
parqit save "$CODEC_LZ4_PQ", replace data compression(lz4)
parqit save "$CODEC_LZ4R_PQ", replace data compression(lz4_raw)
parqit save "$CODEC_BROTLI_PQ", replace data compression(brotli)
parqit save "$CODEC_UNCOMP_PQ", replace data compression(uncompressed)
parqit describe "$CODEC_UNCOMP_PQ"
assert r(n_rows) == 12 & r(n_cols) == 2

* -- Hive-style partitioned output and read-back as a directory source.
clear
parqit use using "$PANEL_PQ"
parqit keep id year firm_id wage region
parqit save "$PARTITION_DIR", replace partition_by(year)
parqit close _all
parqit use using "$PARTITION_DIR", clear
assert _N == $N_PANEL & c(k) >= 5

* -- collect without clear: materialise a view into the current frame.
clear
parqit use using "$PANEL_PQ"
parqit keep in 1/3
parqit collect
assert _N == 3
parqit close _all

* -- open _data: promote the current in-memory dataset to a lazy view.
use "$PANEL_DTA", clear
parqit open _data, name(memory_panel)
parqit keep if year == 2021
parqit keep id year firm_id wage
parqit collect, clear
assert _N == $N_2021 & c(k) == 4
gen byte from_memory = 1
parqit save "$MEMORY_DATA_PQ", replace data
parqit save "$MEMORY_VIEW_PQ", replace
parqit close _all
parqit describe "$MEMORY_DATA_PQ"
assert r(n_cols) == 5
parqit describe "$MEMORY_VIEW_PQ"
assert r(n_rows) == $N_2021 & r(n_cols) == 4


* ============================================================================
* SECTION 7 - Merge, joinby, append, mergein and appendin
* ============================================================================
* Purpose: cover out-of-core two-table verbs and the in-memory/disk bridge
* verbs, including merge flavours, view sources and append variants.
di as txt _n(2) "===== Section 7 - merge, joinby, append, mergein and appendin ====="

* -- m:1 merge: large master view, Parquet lookup, selected using variables.
clear
parqit use using "$PANEL_PQ"
parqit merge m:1 firm_id using "$FIRMS_PQ", keep(match) keepusing(tfp industry) nogenerate
parqit count
assert r(N) == $MERGE_N
parqit summarize tfp
assert abs(r(N) * r(mean) - $MERGE_TFP_SUM) < 1e-8
parqit close _all

* -- 1:1 merge with a .dta using side, exercising the bridge path.
clear
parqit use firm_id tfp using "$FIRMS_PQ"
parqit merge 1:1 firm_id using "$FIRMS_DTA", keepusing(industry exporter) nogenerate
parqit collect, clear
assert _N == 20 & c(k) == 4
parqit close _all

* -- 1:m merge: firm master expanded by worker-year rows.
clear
parqit use using "$FIRMS_PQ"
parqit merge 1:m firm_id using "$PANEL_PQ", keep(match) keepusing(id year wage) nogenerate
parqit count
assert r(N) == $N_PANEL
parqit close _all

* -- Generated merge-status variable instead of nogenerate.
clear
parqit use using "$PANEL_PQ"
parqit merge m:1 firm_id using "$FIRMS_DTA", keep(match master) keepusing(tfp) generate(match_status)
parqit collect, clear
assert _N == $N_PANEL & match_status[1] == 3
parqit close _all

* -- m:m merge: included to expose Stata-compatible sequential pairing.
clear
parqit use using "$SMALL_A_PQ"
parqit merge m:m key using "$SMALL_B_PQ", keep(match master using) generate(mm_status)
parqit collect, clear
assert _N >= 4 & c(k) >= 4
parqit close _all

* -- joinby: many-to-many combinations within key groups.
clear
parqit use using "$PANEL_PQ"
parqit joinby firm_id using "$PATENTS_PQ"
parqit count
assert r(N) == $JOIN_N
parqit close _all

* -- Append one named view to another, tagging the source.
clear
parqit use using "$PART2019_PQ", name(y2019)
parqit use using "$PART2020_PQ", name(y2020)
parqit view y2019
parqit append using view:y2020, generate(source_part)
parqit collect, clear
assert _N == $N_2019 + $N_2020 & source_part[1] < .
parqit close _all

* -- Append multiple disk files in one command.
clear
parqit use using "$PART2019_PQ"
parqit append using "$PART2020_PQ" "$PART2021_PQ", generate(source_file)
parqit count
assert r(N) == $N_PANEL
parqit close _all

* -- Merge against another open view without materialising either side first.
clear
parqit use using "$PANEL_PQ", name(workers21)
parqit keep if year == 2021
parqit use using "$PANEL_PQ", name(firmstats)
parqit collapse (mean) avg_wage=wage, by(firm_id)
parqit view workers21
parqit merge m:1 firm_id using view:firmstats, keep(match) keepusing(avg_wage) nogenerate
parqit count
assert r(N) == $N_2021
parqit close _all

* -- mergein: keep Stata memory in place and read only selected disk columns.
parqit use "$PANEL_PQ", clear
parqit mergein m:1 firm_id using "$FIRMS_PQ", keepusing(tfp industry) nogenerate
assert _N == $N_PANEL
quietly summarize tfp
assert abs(r(sum) - $MERGE_TFP_SUM) < 1e-8

* -- mergein with native merge options forwarded through parqit.
parqit use "$PANEL_PQ", clear
parqit mergein m:1 firm_id using "$FIRMS_PQ", keepusing(tfp) generate(min_status) assert(match)
assert _N == $N_PANEL & min_status[1] == 3

* -- appendin: append a disk table into the current in-memory dataset.
parqit use "$PART2019_PQ", clear
parqit appendin using "$PART2020_PQ"
assert _N == $N_2019 + $N_2020

* -- appendin with keep(): restrict variables read from the disk side.
parqit use id year wage using "$PART2019_PQ", clear
parqit appendin using "$PART2020_PQ", keep(id year wage)
assert _N == $N_2019 + $N_2020 & c(k) == 3


* ============================================================================
* SECTION 8 - SQL, query and settings
* ============================================================================
* Purpose: expose raw SQL, SQL-as-view, query fragments, view close semantics
* and engine settings for threads, memory, tempdir and missing-value semantics.
di as txt _n(2) "===== Section 8 - SQL, query and settings ====="

* -- SQL view: raw DuckDB SQL becomes a lazy parqit view that can be saved.
clear
parqit sql `"SELECT region, sector, avg(wage) AS mean_wage, count(*) AS n FROM read_parquet('$PANEL_PQ') WHERE wage IS NOT NULL GROUP BY region, sector"', name(sqlview)
parqit count
assert r(N) > 0
parqit sort region sector
parqit head 3
parqit save "$SQL_PQ", replace
parqit close _all

* -- SQL with clear: run SQL and immediately materialise the result.
parqit sql `"SELECT 1 AS one, CAST(12.34 AS DOUBLE) AS x"', clear
assert _N == 1 & one == 1

* -- Warehouse-style types: DECIMAL and unsigned integer read back to Stata.
clear
parqit sql `"SELECT CAST(12.34 AS DECIMAL(9,2)) AS money, CAST(2147483648 AS UINTEGER) AS u32"', name(warehouse)
parqit save "$WAREHOUSE_PQ", replace
parqit close _all
parqit use "$WAREHOUSE_PQ", clear
assert abs(money[1] - 12.34) < 1e-12 & u32[1] == 2147483648

* -- query: append a raw SQL fragment to the current lazy pipeline.
clear
parqit use using "$PANEL_PQ"
parqit sort id year
parqit query "qualify row_number() over (partition by id order by year) = 1"
parqit count
assert r(N) == $N_ID_DISTINCT
parqit close _all

* -- close with no argument: close the current view.
clear
parqit use using "$PANEL_PQ", name(close_me)
parqit close

* -- Settings: engine resources, spill directory and Stata missing semantics.
clear
parqit use using "$PANEL_PQ"
parqit set threads 1
parqit set memory_limit 1GB
parqit set tempdir "$PARQIT_DEMO_TEMP"
parqit set tempdir "$PARQIT_DEMO_TEMP/not_created_yet"
parqit set tempdir "$PARQIT_DEMO_TEMP"
parqit set statamissing on
quietly parqit count if wage > 0
global N_STATA_MISSING_MODE = r(N)
parqit set statamissing off
quietly parqit count if wage > 0
assert $N_STATA_MISSING_MODE == r(N) + $N_WAGE_MISSING
quietly parqit count if missing(wage, region)
assert r(N) > 0
parqit close _all


* ============================================================================
* SECTION 9 - Expected loud failures
* ============================================================================
* Purpose: deliberately trigger representative user errors. The commands are
* captured so the do-file continues, but noisily so the messages stay visible.
di as txt _n(2) "===== Section 9 - expected loud failures ====="

* -- Missing input leaves the existing in-memory data untouched.
clear
set obs 2
gen sentinel = 100 + _n
capture noisily parqit use using "$MISSING_PQ", clear
assert _rc != 0
assert _N == 2 & sentinel[1] == 101

* -- Expression and generation errors are loud and return non-zero codes.
clear
parqit use using "$PANEL_PQ"
capture noisily parqit keep if frobnicate(wage)
assert _rc != 0
capture noisily parqit gen wage = 1
assert _rc != 0
capture noisily parqit gen byte bad_numeric = "abc"
assert _rc != 0
capture noisily parqit gen d_bad = td(31feb2020)
assert _rc != 0
parqit close _all

* -- Merge contract/key errors are caught before materialising bad results.
clear
parqit use using "$PANEL_PQ"
capture noisily parqit merge m:1 year using "$FIRMS_PQ"
assert _rc != 0
parqit close _all

* -- append generate() refuses a name that already exists.
clear
parqit use using "$PART2019_PQ"
capture noisily parqit append using "$PART2020_PQ", generate(year)
assert _rc != 0
parqit close _all

* -- save without replace refuses to overwrite an existing file.
clear
set obs 2
gen long id = _n
gen double x = _n
parqit save "$NOREPL_PQ", replace data
capture noisily parqit save "$NOREPL_PQ", data
assert _rc != 0

* -- Unknown compression codecs are rejected, never silently substituted.
clear
parqit use using "$PANEL_PQ"
capture noisily parqit save "$BADCODEC_PQ", replace compression(not_a_codec)
assert _rc != 0
parqit close _all

di as result _n(2) "VERDICT(PARQIT_CLEAN_DEMO): PASS"
di as txt "Artifacts written under: $PARQIT_DEMO_HOME"
capture log close _all
