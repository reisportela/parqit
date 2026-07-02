* parqit pivot — the Excel pivot table as one lazy verb, end-to-end: the
* native twin (collapse + reshape wide in memory) is the oracle for values,
* names and unbalanced/missing cells; pyarrow independently checks the saved
* payload; every refusal must be loud AND leave the view exactly as it was
* (the two stages land atomically or not at all).
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

* ---------- fixture: region × year panel with the hard cells ----------
set obs 240
gen long id      = _n
gen int  year    = 2016 + mod(_n, 4)
gen str8 region  = word("north south east", 1 + mod(_n, 3))
gen double wage  = 100 + 13.7 * mod(_n, 7) + 0.01 * _n
gen str4 tag     = "x"
replace wage = . if mod(_n, 11) == 0
replace wage = . if region == "east" & year == 2019   // a cell all-missing
drop if region == "north" & year == 2019              // a cell absent
local N0 = _N

tempfile fbase
local panel `"`fbase'.parquet"'
parqit save `"`panel'"', replace

* native twin — the strongest oracle for the pivot's semantics
preserve
collapse (mean) mw = wage (count) n = wage, by(region year)
reshape wide mw n, i(region) j(year)
sort region
local want_mw2016_south = mw2016[3]
tempfile oracle
qui save `"`oracle'"'
restore

* second native twin — the string-cols() pivot
preserve
collapse (sum) s = wage, by(year region)
reshape wide s, i(year) j(region) string
sort year
tempfile oracle2
qui save `"`oracle2'"'
restore

* ---------- the pivot, lazily, values cell-exact vs the twin ----------
clear
parqit use using `"`panel'"'
assert r(k) == 5
parqit pivot (mean) mw=wage (count) n=wage, rows(region) cols(year)
parqit describe
assert r(n_cols) == 9
parqit collect, clear
assert _N == 3 & c(k) == 9

* column order is the reshape-wide contract: i, then stubs j-major
unab got : _all
assert "`got'" == "region mw2016 n2016 mw2017 n2017 mw2018 n2018 mw2019 n2019"

foreach v of varlist mw* n* {
    rename `v' s_`v'
}
qui merge 1:1 region using `"`oracle'"', assert(match) nogenerate
assert _N == 3
foreach y in 2016 2017 2018 2019 {
    assert (mi(s_mw`y') & mi(mw`y')) | reldif(s_mw`y', mw`y') < 1e-12
    assert (mi(s_n`y') & mi(n`y')) | s_n`y' == n`y'
}
* the absent (north, 2019) cell is missing; the all-missing (east, 2019)
* cell is mean-missing but count-0 — never a fabricated value
sort region
assert mi(s_mw2019[2]) & mi(s_n2019[2])   // north: no rows at all
assert mi(s_mw2019[1]) & s_n2019[1] == 0  // east: rows, all wage missing
parqit close

* ---------- default statistic (mean) and default target (source name) ----
clear
parqit use using `"`panel'"'
parqit pivot wage, rows(region) cols(year)
parqit collect, clear
assert c(k) == 5
assert reldif(wage2016[3], `want_mw2016_south') < 1e-12   // south sorted 3rd
parqit close

* ---------- wildcards in rows(); string cols() spreads by value ----------
clear
parqit use using `"`panel'"'
parqit pivot (sum) s=wage, rows(ye*) cols(region)
parqit collect, clear
foreach v of varlist s* {
    rename `v' g_`v'
}
qui merge 1:1 year using `"`oracle2'"', assert(match) nogenerate
assert _N == 4
foreach r in east north south {
    assert (mi(g_s`r') & mi(s`r')) | reldif(g_s`r', s`r') < 1e-12
}
parqit close

* ---------- refusals are loud and the view survives untouched ----------
* (a) missing cols() values — the reshape-wide contract, before any stage
clear
set obs 10
gen g = mod(_n, 2)
gen y = cond(_n == 10, ., 2000 + mod(_n, 2))
gen v = _n
parqit open _data
capture noisily parqit pivot (sum) v, rows(g) cols(y)
assert _rc != 0
parqit describe
assert r(n_cols) == 3
parqit count
assert r(N) == 10
parqit close

* (b) a cols() value that cannot form a Stata name — rollback after collapse
clear
set obs 10
gen g = mod(_n, 2)
gen double jj = cond(_n <= 5, 1, 2.5)
gen v = _n
parqit open _data
capture noisily parqit pivot (sum) v, rows(g) cols(jj)
assert _rc != 0
parqit describe
assert r(n_cols) == 3
parqit collect, clear
assert _N == 10 & c(k) == 3
parqit close

* (c) >2000 distinct cols() values refuse (the reshape-wide cap)
clear
set obs 2001
gen id = _n
gen g = mod(_n, 2)
gen v = 1
parqit open _data
capture noisily parqit pivot (sum) v, rows(g) cols(id)
assert _rc != 0
parqit describe
assert r(n_cols) == 3
parqit close

* (d) empty pipeline → no cols() values: loud, and the view stays collectable
clear
parqit use using `"`panel'"'
parqit keep if wage < -99
capture noisily parqit pivot (mean) wage, rows(region) cols(year)
assert _rc != 0
parqit describe
assert r(n_cols) == 5
parqit collect, clear
assert _N == 0 & c(k) == 5
parqit close

* (e) usage errors: overlap, unknown variable, string source, weights,
*     duplicate targets, no specs — all r(≠0), none mutates the view
clear
parqit use using `"`panel'"'
parqit describe
local k0 = r(n_cols)
capture parqit pivot (mean) wage, rows(region year) cols(year)
assert _rc != 0
capture parqit pivot (mean) wage, rows(nosuchvar) cols(year)
assert _rc != 0
capture parqit pivot (sum) tag, rows(region) cols(year)
assert _rc != 0
capture parqit pivot (mean) wage [fw=2], rows(region) cols(year)
assert _rc != 0
capture parqit pivot (mean) wage (sum) wage, rows(region) cols(year)
assert _rc != 0
capture parqit pivot , rows(region) cols(year)
assert _rc != 0
capture parqit pivot (mean) wage, cols(year)
assert _rc != 0
capture parqit pivot (mean) wage, rows(region)
assert _rc != 0
parqit describe
assert r(n_cols) == `k0'
parqit count
assert r(N) == `N0'

* ---------- save path: pyarrow is the independent oracle ----------
parqit pivot (mean) mw=wage (count) n=wage, rows(region) cols(year)
tempfile obase
local pout `"`obase'.parquet"'
parqit save `"`pout'"', replace
parqit close

python:
from sfi import Macro
import pyarrow.parquet as pq
import pyarrow.compute as pc
src = pq.read_table(Macro.getLocal("panel"))
out = pq.read_table(Macro.getLocal("pout"))
assert out.num_rows == 3, out.num_rows
names = out.schema.names
assert names == ["region", "mw2016", "n2016", "mw2017", "n2017",
                 "mw2018", "n2018", "mw2019", "n2019"], names
mask = pc.and_(pc.equal(src["region"], "south"), pc.equal(src["year"], 2016))
w = src.filter(mask)["wage"]
exp_n = len(w) - w.null_count
exp_m = pc.mean(w).as_py()
row = out.filter(pc.equal(out["region"], "south"))
got_n = row["n2016"][0].as_py()
got_m = row["mw2016"][0].as_py()
assert got_n == exp_n, (got_n, exp_n)
assert abs(got_m - exp_m) < 1e-9, (got_m, exp_m)
# the all-missing (east, 2019) mean cell is a real NULL in the payload
east = out.filter(pc.equal(out["region"], "east"))
assert not east["mw2019"][0].is_valid
assert east["n2019"][0].as_py() == 0
end

di "VERDICT(T12_PIVOT): PASS - pivot matches the native collapse+reshape twin cell-exact; refusals loud and atomic; saved payload verified by pyarrow"
