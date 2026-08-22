* V66 — the behavioural half of the public help contract.
* tests/release_lint.sh can prove the help *mentions* every subcommand and every
* expression function, but not that the sentences are true. This file pins the
* claims a help audit corrected, one assert per statement, so a silent drift
* between parqit.sthlp and parqit's behaviour fails the suite:
*   - lazy `parqit merge` takes only keep/keepusing/generate/nogenerate;
*   - `parqit correlate` takes no options, `parqit pwcorr` takes obs/sig;
*   - save's r(ext_missing)/r(frac_dates) exist only when a loss occurred;
*   - _n/_N: keep if/drop if and gen's main expression yes; gen's if qualifier,
*     replace, and the read-only count if/list if filters no;
*   - `using` may be omitted from `parqit use` when no varlist is given.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

tempfile stem
local master `"`stem'_master.parquet"'
local lookup `"`stem'_lookup.parquet"'
local clean  `"`stem'_clean.parquet"'
local lossy  `"`stem'_lossy.parquet"'

clear
set obs 20
gen long id = _n
gen int year = 2019 + mod(_n, 2)
gen double wage = _n * 1.5
parqit save `"`master'"', replace data

clear
set obs 20
gen long id = _n
gen double rate = _n / 3
parqit save `"`lookup'"', replace data

* --- lazy merge is not a wrapper around native merge ------------------------
* The syntax line promises exactly four options; every other native merge
* option must be refused, and a refused verb must leave the view usable.
parqit use using `"`master'"'
foreach bad in force update "assert(match)" nolabel nonotes noreport replace {
    capture parqit merge 1:1 id using `"`lookup'"', `bad'
    assert _rc == 198
}
parqit merge 1:1 id using `"`lookup'"', keep(match) keepusing(rate) generate(_m)
assert _rc == 0
parqit count
assert r(N) == 20
parqit close _all

* --- correlate takes no options; pwcorr takes obs/sig -----------------------
parqit use using `"`master'"'
capture parqit correlate id wage, obs
assert _rc == 101
capture parqit correlate id wage, sig
assert _rc == 101
parqit correlate id wage
assert _rc == 0
parqit pwcorr id wage, obs sig
assert _rc == 0
parqit close _all

* --- save's loss locals are stored only when something was lost -------------
clear
set obs 3
gen long id = _n
parqit save `"`clean'"', replace data
assert `"`r(filename)'"' != ""
assert r(N) == 3 & r(k) == 1
assert `"`r(ext_missing)'"' == ""
assert `"`r(frac_dates)'"' == ""

clear
set obs 3
gen long id = _n
gen double x = _n
replace x = .a in 2
gen double period = 100.5
format period %tm
parqit save `"`lossy'"', replace data
assert `"`r(ext_missing)'"' == "x"
assert `"`r(frac_dates)'"' == "period"

* --- _n/_N: exactly where the help says they work ---------------------------
parqit use using `"`master'"'
parqit sort id

* supported: keep if / drop if, and the MAIN expression of gen (with or
* without an if qualifier)
parqit keep if _n <= 15
assert _rc == 0
parqit drop if _n > 12
assert _rc == 0
parqit gen double rn = _n
assert _rc == 0
parqit gen double rn2 = _n if id > 5
assert _rc == 0
parqit count
assert r(N) == 12

* refused: _n inside gen's if qualifier, and anywhere in replace
capture parqit gen double bad = wage if _n > 3
assert _rc == 198
capture parqit replace wage = _n
assert _rc == 198
capture parqit replace wage = wage if _n > 3
assert _rc == 198
* the refusals left the view exactly as it was
parqit count
assert r(N) == 12

* refused: the read-only stats filters. These used to escape as the engine's
* rc 920 with a raw Binder Error naming __PARQIT_ROW__; ROWCTX-1 made the
* refusal parqit's own, so the code is now contractual (see v67 for the
* message itself).
capture parqit count if _n <= 5
assert _rc == 198
capture parqit list if _N > 1
assert _rc == 198
parqit count
assert r(N) == 12
parqit close _all

* --- `using` is optional when no varlist is given ---------------------------
parqit use `"`master'"'
assert _rc == 0
assert `"`r(view)'"' == "default"
parqit count
assert r(N) == 20
parqit close _all

parqit use `"`master'"', name(named)
assert _rc == 0
assert `"`r(view)'"' == "named"
parqit close _all

clear
parqit use `"`master'"', clear
assert _rc == 0
assert _N == 20 & c(k) == 3

* --- one-way tabulate accepts and ignores row/col ---------------------------
parqit use using `"`master'"'
quietly parqit tabulate year, row col
assert _rc == 0
assert r(r) == 2 & r(N) == 20
parqit close _all

di as result "VERDICT(V66_HELP_CONTRACT): PASS - documented option sets, r() presence, _n/_N scope and use-without-using all match the help"
