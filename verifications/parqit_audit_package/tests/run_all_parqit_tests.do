* ============================================================================
* parqit — guided test pack and usage tour
* ----------------------------------------------------------------------------
* WHAT THIS IS
*   A self-contained demonstration that also checks parqit's correctness. It
*   builds small synthetic datasets, writes/reads Parquet, exercises the
*   documented verbs by topic, and confirms each result against native Stata.
*
* HOW TO RUN
*   Run "SECTION 0 — SETUP" once (installs parqit, defines the file paths), then
*   run "SECTION 1 — SYNTHETIC DATA" once (creates the data and the reference
*   numbers). After that you can run ANY later section on its own: every file
*   name, folder and reference number lives in a GLOBAL ($NAME), so nothing
*   forces you to re-run from the top.
*
* HOW CHECKS WORK
*   Each check is a plain `assert <condition>`. If the condition holds, nothing
*   happens and the tour continues; if it fails, Stata stops on that exact line
*   so you can see what broke. Reaching the final line means everything passed.
* ============================================================================


* ============================================================================
* SECTION 0 — SETUP   (run once)
*   Installs the parqit under test and defines every folder/file path as a
*   global. Edit nothing else: later sections only read these globals.
* ============================================================================
version 16.0
clear all
set more off
set linesize 120
set varabbrev off
set seed 24062026

* parqit under test = the latest published release (no local build needed).
* To test a LOCAL build instead, comment the next line and uncomment these two,
* pointing them at your checkout:
*     adopath ++ "/path/to/parqit/src/ado/p"
*     global PARQIT_PLUGIN_PATH "/path/to/parqit/build/dev/parqit.plugin"
net install parqit, from("https://github.com/reisportela/parqit/releases/latest/download") replace

* identity check: parqit loads and its self-test passes
which parqit
parqit version
parqit selftest
assert "`r(selftest)'" == "ok"

* working folders (the data folder name contains spaces on purpose, to prove
* parqit copes with spaced paths). Forward slashes work on every OS.
global DATA    "`c(tmpdir)'/parqit demo data"
global TEMPDIR "`c(tmpdir)'/parqit demo spill"
capture mkdir "$DATA"
capture mkdir "$TEMPDIR"

* the files this tour creates (the whole manifest, in one place)
global PANEL_DTA      "$DATA/panel_native.dta"
global PANEL_PQ       "$DATA/panel.parquet"
global PANEL_LIGHT_PQ "$DATA/panel_light_no_var_chars.parquet"
global PANEL_NOCHAR_PQ "$DATA/panel_full_no_var_chars.parquet"
global FIRMS_DTA      "$DATA/firms.dta"
global FIRMS_PQ       "$DATA/firms.parquet"
global FIRMS_XLSX     "$DATA/firms.xlsx"
global PATENTS_DTA    "$DATA/patents.dta"
global PATENTS_PQ     "$DATA/patents.parquet"
global WIDE_DTA       "$DATA/wide_income.dta"
global WIDE_PQ        "$DATA/wide_income.parquet"
global PART2019_PQ    "$DATA/panel_2019.parquet"
global PART2019_CSV   "$DATA/panel_2019.csv"
global PART2020_PQ    "$DATA/panel_2020.parquet"
global CHUNK_PQ       "$DATA/chunked_zstd.parquet"
global MEM_PQ         "$DATA/memory_export.parquet"
global VIEW_PQ        "$DATA/view_export.parquet"
global PART_DIR       "$DATA/partitioned_panel"
global AUTO_DTA       "$DATA/public_auto.dta"
global AUTO_PQ        "$DATA/public_auto.parquet"
global TIPS_CSV       "$DATA/public_tips.csv"
global MISSING_PQ     "$DATA/definitely_not_here.parquet"
global NOREPL_PQ      "$DATA/no_replace_target.parquet"
global BADCODEC_PQ    "$DATA/bad_codec.parquet"

capture log close _all
log using "$DATA/parqit_adversarial_tests.log", replace text


* ============================================================================
* SECTION 1 — SYNTHETIC DATA   (run once, after SETUP)
*   Builds the firm/worker panel and three helper tables, saves each as both
*   .dta and .parquet, and stores the reference numbers (counts, means) that
*   later sections compare parqit's results against. All reference numbers are
*   globals so any later section can use them on its own.
* ============================================================================
di as txt _n(2) "===== Section 1 — synthetic data ====="

* the main panel: 240 worker-year rows, with labels, a value label, a
* characteristic on wage and notes (these exercise metadata round-trips later)
clear
set obs 240
gen long   id        = ceil(_n/3)
gen int    year      = 2019 + mod(_n - 1, 3)
gen int    firm_id   = mod(id - 1, 20) + 1
gen byte   education = mod(id, 4) + 1
gen str6   gender    = cond(mod(id, 2) == 0, "F", "M")
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
label define edlbl 1 "basic" 2 "secondary" 3 "tertiary" 4 "phd"
label values education edlbl
char wage[source] "synthetic"
note tenure: tenure measured in years
note: generated by tests/run_all_parqit_tests.do
order id year firm_id wage hours tenure gender education region sector hire_date note
compress
save "$PANEL_DTA", replace
parqit save "$PANEL_PQ", replace data

* reference numbers taken from the panel (parqit must reproduce these)
quietly count
global N_panel = r(N)
quietly count if missing(wage)
global N_wage_missing = r(N)
quietly count if !missing(wage) & wage > 0
global N_positive_wage = r(N)
quietly summarize wage
global native_wage_mean = r(mean)
quietly count if !missing(wage, hours, tenure, id, year, firm_id, education)
global native_complete_core = r(N)
quietly count if year == 2021
global N_2021 = r(N)
* number of distinct ids (each id appears 3 times) drives the duplicates checks
tempvar idtag
bysort id: gen byte `idtag' = _n == 1
quietly count if `idtag'
global N_id_distinct = r(N)
global N_id_surplus  = $N_panel - $N_id_distinct
drop `idtag'
assert $N_panel == 240

* firm table (for merges), patent table (for joinby), and a wide table (reshape)
clear
set obs 20
gen int    firm_id  = _n
gen double tfp      = round(50 + 3*_n + runiform(), .001)
gen str10  industry = cond(mod(_n, 2), "goods", "services")
gen byte   exporter = mod(_n, 3) == 0
label variable tfp "firm productivity"
save "$FIRMS_DTA", replace
parqit save "$FIRMS_PQ", replace data

clear
set obs 40
gen int firm_id     = ceil(_n/2)
gen int patent_id   = _n
gen int patent_year = 2018 + mod(_n, 5)
save "$PATENTS_DTA", replace
parqit save "$PATENTS_PQ", replace data

clear
set obs 30
gen long   pid     = _n
gen str4   grp     = cond(mod(_n, 2), "odd", "even")
gen double inc2019 = 1000 + _n
gen double inc2020 = 1100 + 2*_n
gen double inc2021 = 1200 + 3*_n
save "$WIDE_DTA", replace
parqit save "$WIDE_PQ", replace data

* year slices (for append/appendin) and CSV; plus two metadata variants of the
* panel used to isolate the characteristic round-trip
use "$PANEL_DTA", clear
keep if year == 2019
parqit save "$PART2019_PQ", replace data
export delimited using "$PART2019_CSV", replace

use "$PANEL_DTA", clear
keep if year == 2020
parqit save "$PART2020_PQ", replace data

use "$PANEL_DTA", clear
keep id year region sector
parqit save "$PANEL_LIGHT_PQ", replace data

use "$PANEL_DTA", clear
gen double wage_clean = wage
drop wage
rename wage_clean wage
label variable wage "hourly wage"
order id year firm_id wage hours tenure gender education region sector hire_date note
parqit save "$PANEL_NOCHAR_PQ", replace data


* ============================================================================
* SECTION 2 — reading files and round-tripping metadata
*   describe/glimpse/path inspect a file without loading it; `use` reads Parquet,
*   Stata .dta, Excel and CSV into memory; labels and characteristics survive.
* ============================================================================
di as txt _n(2) "===== Section 2 — use, describe, metadata, input formats ====="

* describe/glimpse report shape and that parqit wrote the metadata block
parqit describe "$PANEL_PQ"
assert r(n_rows) == $N_panel & r(n_cols) >= 12 & r(has_parqit_meta) == 1
parqit glimpse "$PANEL_PQ"
assert r(n_rows) == $N_panel
parqit path "$PANEL_PQ"
assert r(exists) == 1 & `"`r(path)'"' != ""

* read the Parquet into memory and confirm labels + characteristic came back
parqit use "$PANEL_PQ", clear
assert _N == $N_panel & c(k) >= 12
assert `"`: variable label wage'"' == "hourly wage" & `"`: label edlbl 3'"' == "tertiary"
assert `"`: char wage[source]'"' == "synthetic"

* parqit also bridges Stata .dta, Excel and CSV through the same `use`
parqit use using "$PANEL_DTA", clear
assert _N == $N_panel & c(k) >= 12

use "$FIRMS_DTA", clear
export excel using "$FIRMS_XLSX", firstrow(variables) replace
parqit use using "$FIRMS_XLSX", clear
assert _N == 20 & c(k) >= 4

parqit use using "$PART2019_CSV", clear
assert _N == 80 & c(k) >= 12


* ============================================================================
* SECTION 3 — lazy views and non-mutating exploration
*   A lazy `use ... , name()` opens a view WITHOUT loading data; exploration
*   verbs (count, summarize, tabulate, ...) compute on disk and leave memory
*   empty until you `collect`.
* ============================================================================
di as txt _n(2) "===== Section 3 — lazy views and exploration ====="

* open two named views; memory stays empty, the registry lists them
clear
parqit use using "$PANEL_PQ", name(panel)
assert _N == 0 & c(k) == 0 & r(k) >= 12 & "`r(view)'" == "panel"
parqit use using "$FIRMS_PQ", name(firms)
parqit views
assert r(n_views) == 2

* switch the current view, and run one command against another view by prefix
parqit view panel
assert "`r(view)'" == "panel"
parqit view firms: count
assert r(N) == 20

* schema and name searches on the current (lazy) view
parqit describe
assert r(n_cols) >= 12 & r(n_steps) == 0
parqit ds
assert strpos("`r(varlist)'", "wage") > 0 & strpos("`r(varlist)'", "firm_id") > 0
parqit lookfor wage firm
assert strpos("`r(varlist)'", "wage") > 0 | strpos("`r(varlist)'", "firm_id") > 0

* counts (with and without a condition) materialise only the number, not the data
parqit count
assert r(N) == $N_panel
parqit count if missing(wage)
assert r(N) == $N_wage_missing

* row previews
parqit list id year wage in 1/5
assert r(N) == 5
parqit head 4
assert r(N) == 4

* summary statistics match native Stata
parqit summarize wage, detail
assert r(N) == $N_panel - $N_wage_missing & abs(r(mean) - $native_wage_mean) < 1e-8
parqit tabulate education
assert r(N) == $N_panel & r(r) == 4
parqit tabulate gender education, row col
assert r(N) == $N_panel & r(r) >= 2 & r(c) == 4
parqit misstable summarize wage hours tenure id year firm_id education
assert r(N) == $N_panel & r(n_complete) == $native_complete_core
parqit misstable patterns wage region tenure
assert r(r) > 0
parqit levelsof education
assert r(r) == 4 & "`r(levels)'" == "1 2 3 4"
parqit codebook wage region education
parqit distinct id year firm_id
assert r(N) == $N_panel & r(ndistinct) > 0

* duplicates surplus = rows minus distinct ids (derived, not a magic number)
parqit duplicates report id
assert r(N) == $N_panel & r(surplus) == $N_id_surplus
parqit duplicates list id, limit(4)

* correlations and a histogram return their stored results
parqit tabstat wage hours tenure, statistics(n mean sd min max p50)
parqit correlate wage hours tenure
assert r(N) > 0 & r(rho) < .
parqit pwcorr wage hours tenure, obs sig
assert r(N) > 0 & r(rho) < .
parqit histogram wage, bins(12) nodraw
assert r(N) == $N_panel - $N_wage_missing & r(bins) == 12

* none of the above loaded data into memory
assert _N == 0 & c(k) == 0
parqit close _all


* ============================================================================
* SECTION 4 — single-table verbs (keep, drop, gen, replace, egen, sort, ...)
*   These build a lazy pipeline; `collect` runs it and brings the result into
*   memory. Results are checked against native Stata.
* ============================================================================
di as txt _n(2) "===== Section 4 — single-table verbs ====="

* a pipeline of common transformations; the final row count matches native
clear
parqit use using "$PANEL_PQ", name(panel)
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
assert r(N) == $N_positive_wage

* the generated ln(wage) aggregate equals the native Stata one
parqit summarize lwage
local lazy_lwage_sum = r(N) * r(mean)
use "$PANEL_DTA", clear
keep if wage > 0 & !missing(wage)
gen double lwage = ln(wage)
quietly summarize lwage
assert abs(`lazy_lwage_sum' - r(sum)) < 1e-8

* drop + drop-if + descending sort, then collect
clear
parqit use using "$PANEL_PQ", name(panel)
parqit drop note hire_date
parqit drop if education == 1
parqit gsort -wage id year
parqit collect, clear
assert c(k) == 10 & education[1] != 1
parqit close _all

* keep a row range, then a random sample of fixed size
clear
parqit use using "$PANEL_PQ"
parqit sort id year
parqit keep in 1/30
parqit sample 10, count seed(123)
parqit collect, clear
assert _N == 10
parqit close _all

* duplicates drop on id keeps one row per id (= distinct id count)
clear
parqit use using "$PANEL_PQ"
parqit sort id year
parqit duplicates drop id, force
parqit count
assert r(N) == $N_id_distinct
parqit close _all

* contract collapses to group frequencies; it drops the char-bearing wage column
clear
parqit use using "$PANEL_PQ"
parqit contract region sector, freq(freq)
parqit collect, clear
quietly summarize freq
assert r(sum) == $N_panel & c(k) == 3
parqit close _all

* same contract on the no-characteristics variant (the normal, non-edge case)
clear
parqit use using "$PANEL_LIGHT_PQ"
parqit contract region sector, freq(freq)
parqit collect, clear
quietly summarize freq
assert r(sum) == $N_panel & c(k) == 3
parqit close _all

* PARQIT-CHAR-01 guard: dropping a column that carried a characteristic or note
* (here wage's char and tenure's note) must NOT abort materialisation. Two ways
* to drop it: a column-subset use, and a collapse.
clear
parqit use id year region using "$PANEL_PQ", clear
assert _N == $N_panel & c(k) == 3

clear
parqit use using "$PANEL_PQ"
parqit collapse (mean) mhours = hours, by(region)
parqit collect, clear
parqit close _all


* ============================================================================
* SECTION 5 — collapse, reshape, save, open _data, partitioned output
*   Aggregation and reshaping, the two write paths (save data vs save view), and
*   Hive-style partitioned directories.
* ============================================================================
di as txt _n(2) "===== Section 5 — collapse, reshape, save, partitions ====="

* collapse cell-by-cell equals native Stata's collapse
clear
parqit use using "$PANEL_NOCHAR_PQ"
parqit keep if wage > 0 & !missing(wage)
parqit collapse (mean) mwage=wage (sum) total_hours=hours (count) n=wage, by(region sector)
parqit sort region sector
parqit collect, clear
tempfile lazy_collapse
save "`lazy_collapse'", replace
use "$PANEL_DTA", clear
keep if wage > 0 & !missing(wage)
collapse (mean) mwage_native=wage (sum) total_hours_native=hours (count) n_native=wage, by(region sector)
sort region sector
merge 1:1 region sector using "`lazy_collapse'", assert(match) nogenerate
gen double d_mean  = abs(mwage - mwage_native)
gen double d_hours = abs(total_hours - total_hours_native)
quietly summarize d_mean
local max_mean = r(max)
quietly summarize d_hours
assert `max_mean' < 1e-8 & r(max) < 1e-8 & n == n_native

* reshape long then wide returns to the original shape
clear
parqit use using "$WIDE_PQ"
parqit reshape long inc, i(pid) j(year)
parqit count
assert r(N) == 90
parqit reshape wide inc, i(pid grp) j(year)
parqit collect, clear
assert _N == 30 & inc2019[1] == 1001
parqit close _all

* save options: compression and chunk() set the number of row groups
clear
set obs 10000
gen long rid   = _n
gen int  group = mod(_n, 4)
parqit save "$CHUNK_PQ", replace data compression(zstd) compression_level(3) chunk(2048)
parqit describe "$CHUNK_PQ"
assert r(n_rows) == 10000 & r(n_row_groups) == ceil(10000/2048)

* open _data promotes the in-memory dataset to a lazy view; the two write paths:
* `save ... , data` writes memory, `save` (no data) writes the current view
clear
use "$PANEL_DTA", clear
parqit open _data, name(memory_panel)
parqit keep if year == 2021
parqit keep id year firm_id wage
parqit collect, clear
assert _N == $N_2021 & c(k) == 4
gen byte from_memory = 1
parqit save "$MEM_PQ", replace data
parqit save "$VIEW_PQ", replace
parqit close _all
parqit describe "$MEM_PQ"
local mem_cols = r(n_cols)
parqit describe "$VIEW_PQ"
assert `mem_cols' == 5 & r(n_cols) == 4 & r(n_rows) == $N_2021

* partitioned output: write a Hive-style directory (one folder per year) and read it back
clear
parqit use using "$PANEL_PQ"
parqit keep id year firm_id wage region
parqit save "$PART_DIR", replace partition_by(year)
parqit close _all
parqit use using "$PART_DIR", clear
assert _N == $N_panel & c(k) >= 5


* ============================================================================
* SECTION 6 — two-table verbs (merge, joinby, append, mergein, appendin)
*   merge/joinby/append run on disk; mergein/appendin keep the master in memory
*   and bring the disk side to it. Each is checked against the native twin.
* ============================================================================
di as txt _n(2) "===== Section 6 — merge, append, joinby, mergein, appendin ====="

* native m:1 merge, kept as the reference
use "$PANEL_DTA", clear
merge m:1 firm_id using "$FIRMS_DTA", keep(match) keepusing(tfp industry) nogenerate
quietly count
local native_merge_N = r(N)
quietly summarize tfp
local native_tfp_sum = r(sum)
* parqit m:1 merge matches the native row count and values
clear
parqit use using "$PANEL_PQ"
parqit merge m:1 firm_id using "$FIRMS_PQ", keep(match) keepusing(tfp industry) nogenerate
parqit count
assert r(N) == `native_merge_N'
parqit summarize tfp
assert abs(r(N) * r(mean) - `native_tfp_sum') < 1e-8
parqit close _all

* joinby (many-to-many) matches native joinby's row count
use "$PANEL_DTA", clear
joinby firm_id using "$PATENTS_DTA"
local native_join_N = _N
clear
parqit use using "$PANEL_PQ"
parqit joinby firm_id using "$PATENTS_PQ"
parqit count
assert r(N) == `native_join_N'
parqit close _all

* append one view onto another, tagging the source
clear
parqit use using "$PART2019_PQ", name(y2019)
parqit use using "$PART2020_PQ", name(y2020)
parqit view y2019
parqit append using view:y2020, generate(source_part)
parqit collect, clear
assert _N == 160 & source_part[1] < .
parqit close _all

* mergein brings the disk firm table to the in-memory panel; equals native merge
use "$PANEL_DTA", clear
parqit mergein m:1 firm_id using "$FIRMS_PQ", keepusing(tfp industry) nogenerate
sort id year
tempfile got_mergein
save "`got_mergein'", replace
use "$PANEL_DTA", clear
merge m:1 firm_id using "$FIRMS_DTA", keepusing(tfp industry) nogenerate
sort id year
cf _all using "`got_mergein'"

* appendin stacks a disk table under the in-memory one; equals native append
parqit use "$PART2019_PQ", clear
parqit appendin using "$PART2020_PQ"
sort id year
tempfile got_appendin
save "`got_appendin'", replace
use "$PANEL_DTA", clear
keep if year == 2019
tempfile part2019_dta
save "`part2019_dta'", replace
use "$PANEL_DTA", clear
keep if year == 2020
append using "`part2019_dta'"
sort id year
cf _all using "`got_appendin'"


* ============================================================================
* SECTION 7 — SQL escape hatches and settings
*   `sql` opens a view from raw SQL; `query` appends a SQL fragment to a view;
*   `set` controls threads/memory/tempdir and Stata-vs-SQL missing semantics.
* ============================================================================
di as txt _n(2) "===== Section 7 — SQL and settings ====="

* a full SQL statement as a lazy view
clear
parqit sql `"SELECT region, sector, avg(wage) AS awage, count(*) AS n FROM read_parquet('$PANEL_PQ') WHERE wage IS NOT NULL GROUP BY region, sector"', name(sqlview)
parqit count
assert r(N) > 0
parqit sort region sector
parqit head 3
parqit close _all

* a SQL fragment (keep the first year per id) appended to a parqit view
clear
parqit use using "$PANEL_PQ"
parqit sort id year
parqit query "qualify row_number() over (partition by id order by year) = 1"
parqit count
assert r(N) == $N_id_distinct
parqit close _all

* settings: tempdir must accept an absolute path and one containing spaces
clear
parqit use using "$PANEL_PQ"
parqit set threads 1
parqit set memory_limit 1GB
parqit set tempdir "$DATA"
parqit set tempdir "$TEMPDIR"
* statamissing on counts Stata's "missing > everything"; off uses SQL semantics
parqit set statamissing on
quietly parqit count if wage > 0
local n_stata_missing = r(N)
parqit set statamissing off
quietly parqit count if wage > 0
assert `n_stata_missing' == r(N) + $N_wage_missing
parqit close _all


* ============================================================================
* SECTION 8 — bounded internet downloads   (needs internet)
*   Downloads two small public datasets and reads them with parqit. If a
*   download fails (offline/firewall) the check is skipped, not failed.
* ============================================================================
di as txt _n(2) "===== Section 8 — public downloads ====="

* Stata Press auto.dta -> parqit reads it and round-trips it to Parquet
capture copy "https://www.stata-press.com/data/r18/auto.dta" "$AUTO_DTA", replace
if !_rc {
    parqit use using "$AUTO_DTA", clear
    assert _N == 74 & c(k) >= 12
    parqit save "$AUTO_PQ", replace data
    parqit describe "$AUTO_PQ"
    assert r(n_rows) == 74
}
else di as txt "skipped: no internet for the Stata Press download"

* a public CSV from GitHub -> parqit scans it
capture copy "https://raw.githubusercontent.com/mwaskom/seaborn-data/master/tips.csv" "$TIPS_CSV", replace
if !_rc {
    parqit use using "$TIPS_CSV", clear
    assert _N == 244 & c(k) >= 7
}
else di as txt "skipped: no internet for the GitHub CSV download"


* ============================================================================
* SECTION 9 — things that MUST fail loudly
*   parqit should refuse bad input with a clear error (a non-zero return code),
*   never silently. `capture noisily` runs the command, `assert _rc != 0` checks
*   it failed; the surrounding good data must survive.
* ============================================================================
di as txt _n(2) "===== Section 9 — expected failures ====="

* reading a missing file fails, and the dataset already in memory is untouched
clear
set obs 2
gen sentinel = 100 + _n
capture noisily parqit use using "$MISSING_PQ", clear
assert _rc != 0
assert _N == 2 & sentinel[1] == 101

* unsupported function, redefining a variable, wrong storage type, bad date
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
* an out-of-range `keep in` is accepted lazily but fails when the pipeline runs
parqit keep in 200/9999
capture noisily parqit count
assert _rc != 0
parqit close _all

* m:1 merge rejects a non-unique key on the using side
clear
parqit use using "$PANEL_PQ"
capture noisily parqit merge m:1 year using "$FIRMS_PQ"
assert _rc != 0
parqit close _all

* append generate() name colliding with an existing variable is refused
clear
parqit use using "$PART2019_PQ"
capture noisily parqit append using "$PART2020_PQ", generate(year)
assert _rc != 0
parqit close _all

* save refuses an existing target without replace, and an unknown codec
clear
set obs 2
gen long id = _n
gen x = _n
parqit save "$NOREPL_PQ", replace data
capture noisily parqit save "$NOREPL_PQ", data
assert _rc != 0
clear
parqit use using "$PANEL_PQ"
capture noisily parqit save "$BADCODEC_PQ", replace compression(not_a_codec)
assert _rc != 0
parqit close _all


* ============================================================================
* DONE — reaching this line means every check above passed.
* ============================================================================
di as result _n(2) "VERDICT(PARQIT_ADVERSARIAL_TEST_PACK): PASS — all checks passed"
di as txt "data + log written under: $DATA"
capture log close _all
