* ============================================================================
* parqit basics — START HERE: use / save / merge / append, two ways
*
* This is the first do-file to run after installing parqit. It walks the four
* base operations twice over the same small data:
*
*   A. the eager way ("pq-style"): read everything into Stata's memory first,
*      then work — the right mental model when the data is small; and
*   B. the parqit way (lazy): open a VIEW over the file, stack verbs (nothing
*      runs), and materialise ONLY the result — the efficient path when the
*      data is large, because what never enters Stata is never paid for.
*
* Every lazy result is asserted against a native-Stata twin, so the guide is
* also a proof; it ends in VERDICT(PARQIT_BASICS): PASS. Runtime: seconds.
*
* Usage:
*   . do parqit_basics.do                       (parqit installed/adopath'd)
*   . do parqit_basics.do <repo_root> <plugin>  (development tree)
*
* The full feature tour (reshape, pivot, sql, hostile files, ...) is the
* companion examples/parqit_tour.do; `help parqit` documents everything.
* ============================================================================
clear all
set more off
set linesize 100
args repo plugin

if (`"`repo'"' != "") {
    adopath ++ `"`repo'/src/ado/p"'
    global PARQIT_PLUGIN_PATH `"`plugin'"'
}
else {
    * installed mode: if parqit is not on the adopath yet, try the repo-local
    * install tree (this file lives in <repo>/examples, the tree in ado/plus/p)
    capture which parqit
    if (_rc) {
        foreach try in "../ado/plus/p" "ado/plus/p" {
            capture confirm file "`try'/parqit.ado"
            if (_rc == 0) {
                adopath ++ "`try'"
                continue, break
            }
        }
    }
    which parqit
}

* ----------------------------------------------------------------------------
di as txt _n "{hline 78}"
di as txt "parqit basics — section 0: setup and small example data"
di as txt "{hline 78}"

parqit version
parqit selftest
assert "`r(selftest)'" == "ok"

* Everything this guide writes lives in one disposable folder.
capture mkdir parqit_basics_files
local W  "parqit_basics_files/workers.parquet"
local F  "parqit_basics_files/firms.parquet"
local X  "parqit_basics_files/workers_extra.parquet"
local Wd "parqit_basics_files/workers.dta"
local Fd "parqit_basics_files/firms.dta"
local Xd "parqit_basics_files/workers_extra.dta"

* A worker panel (120 obs), a firm lookup (20 obs) and a late-arrivals file
* (10 obs). Each is saved both as .dta (for the native-Stata oracle) and as
* Parquet via parqit — with no view open, `parqit save` writes the dataset in
* memory, labels and all.
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
sort id
qui save `"`Wd'"', replace
parqit save `"`W'"', replace

clear
set obs 20
gen int    firm_id  = _n
gen double tfp      = 1 + _n/10
gen str12  industry = "ind" + string(mod(_n, 4) + 1)
label variable tfp "total factor productivity"
sort firm_id
qui save `"`Fd'"', replace
parqit save `"`F'"', replace

clear
set obs 10
gen long   id      = 1000 + _n
gen int    firm_id = 10 + ceil(_n / 2)
gen int    year    = 2024
gen double wage    = 1400 + 11*_n
gen str8   region  = "New"
gen byte   female  = mod(_n, 2)
label define yesno 0 "No" 1 "Yes"
label values female yesno
sort id
qui save `"`Xd'"', replace
parqit save `"`X'"', replace

* ----------------------------------------------------------------------------
di as txt _n "{hline 78}"
di as txt "section 1: USE — read into memory (eager) vs open a view (lazy)"
di as txt "{hline 78}"

* A. Eager, pq-style: the whole file lands in memory right now. Perfect for
*    small files — this is plain, fast I/O with the full type/label map.
parqit use `"`W'"', clear
assert _N == 120
local k0 = c(k)
sort id
tempfile eager
qui save `"`eager'"'
di as txt "eager: " as res _N as txt " obs are in memory (labels restored too)"

* B. Lazy, the parqit way: `use using` opens a VIEW — a plan over the file,
*    not data. It returns instantly whether the file has 120 rows or 2 billion,
*    and Stata's memory stays exactly as it was.
clear
parqit use using `"`W'"'
assert _N == 0                        // nothing was loaded...
parqit describe                       // ...yet we can see the schema,
parqit head 5                         // preview rows,
parqit count                          // count,
assert r(N) == 120
parqit summarize wage                 // and summarise — all computed by the
assert _N == 0                        // engine; memory is STILL empty.
di as txt "lazy: explored the file with 0 obs in memory — explore first, load last"

* Materialise only when (and if) you actually want the rows:
parqit collect, clear
assert _N == 120 & c(k) == `k0'
sort id
cf _all using `"`eager'"'             // cell-for-cell identical to the eager read
parqit close
di as txt "collect: the lazy path delivered exactly the eager result"

* ----------------------------------------------------------------------------
di as txt _n "{hline 78}"
di as txt "section 2: SAVE — write memory (eager) vs Parquet -> Parquet (lazy)"
di as txt "{hline 78}"

* A. Eager: with no view open, `parqit save` writes the in-memory dataset —
*    the pq-style export. (Data is still the 120 workers from section 1.)
qui count if year >= 2022
local nfilt = r(N)
parqit save "parqit_basics_files/mem_export.parquet", replace
di as txt "eager save: the in-memory dataset went to Parquet"

* B. Lazy: transform a file into another file WITHOUT touching memory. The
*    view runs filter + derived column inside the engine and `parqit save`
*    streams the result straight to disk.
parqit use using `"`W'"'
parqit keep if year >= 2022
parqit gen double lwage = ln(wage)
parqit save "parqit_basics_files/filtered.parquet", replace
assert _N == 120                      // memory untouched throughout
parqit describe "parqit_basics_files/filtered.parquet"
assert r(n_rows) == `nfilt'
di as txt "lazy save: `nfilt' filtered rows written; memory never touched"

* With a view open, plain `parqit save` materialises the VIEW; add `data` to
* export the in-memory dataset instead:
parqit save "parqit_basics_files/mem_export2.parquet", replace data
parqit describe "parqit_basics_files/mem_export2.parquet"
assert r(n_rows) == 120
parqit close

* ----------------------------------------------------------------------------
di as txt _n "{hline 78}"
di as txt "section 3: MERGE — out-of-core (lazy) vs native mergein (in-memory)"
di as txt "{hline 78}"

* The native oracle, built the classic way from the .dta twins:
use `"`Wd'"', clear
keep if year == 2022
qui merge m:1 firm_id using `"`Fd'"', keep(match) keepusing(tfp industry) nogenerate
sort id
tempfile oracle_m
qui save `"`oracle_m'"'

* A. Lazy, the parqit way for LARGE data: the master never enters Stata.
*    Filter and join run as one engine query; only the result is collected.
clear
parqit use using `"`W'"'
parqit keep if year == 2022
parqit merge m:1 firm_id using `"`F'"', keep(match) keepusing(tfp industry) nogenerate
parqit collect, clear
parqit close
sort id
cf _all using `"`oracle_m'"'
di as txt "lazy merge: identical to native — and the master never entered memory"

* B. mergein, the pq-style shape done efficiently: your data is ALREADY in
*    memory and the disk side is a small lookup. mergein runs a NATIVE merge,
*    reading only the needed columns of the file — no engine round-trip.
parqit use `"`W'"', clear
keep if year == 2022
parqit mergein m:1 firm_id using `"`F'"', keep(match) keepusing(tfp industry) nogenerate
sort id
cf _all using `"`oracle_m'"'
di as txt "mergein: native merge against a disk lookup — same result again"

* Rule of thumb: small lookup + data in memory -> mergein;
*                big-on-big                    -> lazy merge + collect/save.

* ----------------------------------------------------------------------------
di as txt _n "{hline 78}"
di as txt "section 4: APPEND — lazy append vs native appendin"
di as txt "{hline 78}"

* Native oracle from the .dta twins:
use `"`Wd'"', clear
append using `"`Xd'"'
sort id
tempfile oracle_a
qui save `"`oracle_a'"'

* A. Lazy: stack files on the engine; collect (or save) only the union.
clear
parqit use using `"`W'"'
parqit append using `"`X'"'
parqit collect, clear
parqit close
sort id
cf _all using `"`oracle_a'"'
di as txt "lazy append: identical to native append"

* B. appendin: data already in memory, disk rows appended NATIVELY.
parqit use `"`W'"', clear
parqit appendin using `"`X'"'
sort id
cf _all using `"`oracle_a'"'
di as txt "appendin: native append of a disk file — same result again"

* ----------------------------------------------------------------------------
di as txt _n "{hline 78}"
di as txt "section 5: the payoff — a whole pipeline, one engine query"
di as txt "{hline 78}"

* Filter + derive + aggregate declared lazily, executed once. On real data the
* engine reads only the columns involved and skips row groups the filter
* excludes — this is where the lazy philosophy pays off.
clear
parqit use using `"`W'"'
parqit keep if !missing(wage) & year >= 2021
parqit gen double lwage = ln(wage)
parqit collapse (mean) mlwage = lwage (count) n = lwage, by(firm_id year)
parqit show                           // the single SQL query this compiled to
parqit collect, clear
sort firm_id year
tempfile pipeline
qui save `"`pipeline'"'

* Native twin of the same pipeline:
use `"`Wd'"', clear
keep if !missing(wage) & year >= 2021
gen double lwage = ln(wage)
collapse (mean) mlwage_o = lwage (count) n_o = lwage, by(firm_id year)
sort firm_id year
qui merge 1:1 firm_id year using `"`pipeline'"', assert(match) nogenerate
gen double dm = reldif(mlwage, mlwage_o)
qui summ dm
assert r(max) < 1e-12
assert n == n_o
local ncells = _N
di as txt "pipeline: every aggregated cell equals native Stata"

* Same pipeline, other ending: straight to disk, memory never involved. The
* view is still open (collect does not consume it), so just save it.
parqit save "parqit_basics_files/firm_year.parquet", replace
parqit describe "parqit_basics_files/firm_year.parquet"
assert r(n_rows) == `ncells'
parqit close _all

* ----------------------------------------------------------------------------
di as txt _n "{hline 78}"
di as txt "recap — the eager-to-lazy translation card"
di as txt "{hline 78}"

di as txt "  read a file        parqit use f, clear          (eager, small data)"
di as txt "                     parqit use using f  ... parqit collect, clear   (lazy)"
di as txt "  write a file       parqit save f, replace       (memory -> Parquet)"
di as txt "                     view + verbs + parqit save f, replace  (disk -> disk)"
di as txt "  merge              data in memory + small lookup  -> parqit mergein"
di as txt "                     big-on-big                     -> lazy parqit merge"
di as txt "  append             data in memory + small file    -> parqit appendin"
di as txt "                     files -> one result             -> lazy parqit append"
di as txt "  golden rule        explore first, load last: describe/head/count/"
di as txt "                     summarize run on the view without loading anything"

di as result _n "VERDICT(PARQIT_BASICS): PASS — use/save/merge/append verified eager vs lazy against native Stata"
