* V143 — Stata core's abbreviations of commands, subcommands and options
*   (CMD-ABBREV-1, 2026-09-29). Every short form native Stata accepts for a
*   command parqit mirrors reaches that parqit verb; the words native Stata
*   does not accept stay refused; each exact parqit verb still reaches its own
*   program; and the options where parqit was stricter than Stata take
*   Stata's forms, the old spellings included.
* Oracles: native Stata — the same command on the same data, r() and printed
*   tables compared. The accepted forms are those -which- resolves to the
*   command in Stata (ASSUMPTIONS #175).
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile stem nlog plog

* save and use
sysuse auto, clear
gen int id = _n
save `"`stem'_auto.dta"', replace
parqit sa `"`stem'_auto.parquet"', data replace
assert r(N) == 74
keep in 1/5
save `"`stem'_auto5.dta"', replace
parqit sav `"`stem'_auto5.parquet"', data replace
assert r(N) == 5
parqit u `"`stem'_auto.parquet"', clear
assert _N == 74
parqit us using `"`stem'_auto.parquet"', name(v)

* summarize, with detail, under every short form
use `"`stem'_auto.dta"', clear
summarize price, detail
local p50 = r(p50)
local mean = r(mean)
foreach c in su sum summ summa summar summari summariz {
    parqit `c' price, d
    assert r(N) == 74 & r(p50) == `p50' & reldif(r(mean), `mean') < 1e-14
}

* tabulate, with column from co (col, the old spelling, included) and row
* from r, and pwcorr's o(bs): each form prints native's table
tabulate rep78 foreign
local r = r(r)
local c = r(c)
foreach t in ta tab tabu tabul tabula tabulat {
    parqit `t' rep78 foreign
    assert r(N) == 69 & r(r) == `r' & r(c) == `c'
}
log using `"`nlog'"', text replace name(v143n)
tabulate foreign rep78, column row
pwcorr price mpg, obs sig
log close v143n
log using `"`plog'"', text replace name(v143p)
parqit tabulate foreign rep78, col row
parqit pwcorr price mpg, obs sig
parqit tabulate foreign rep78, co r
parqit pwcorr price mpg, o sig
parqit tabulate foreign rep78, column ro
parqit pwcorr price mpg, ob sig
log close v143p
python:
from sfi import Macro
def rows(path):
    return [s.rstrip() for s in open(path, encoding="utf-8") if "|" in s]
nat, pq_ = rows(Macro.getLocal("nlog")), rows(Macro.getLocal("plog"))
assert len(nat) > 20 and pq_ == nat * 3, \
    "\n".join(a + "  <>  " + b for a, b in zip(nat * 3, pq_) if a != b)
end

* correlate
correlate price mpg
local rho = r(rho)
foreach c in cor corr corre correl correla correlat {
    parqit `c' price mpg
    assert r(N) == 74 & reldif(r(rho), `rho') < 1e-12
}

* tabstat: stats() as native, and never together with statistics()
tabstat price mpg, stats(mean sd p50) save
matrix nat = r(StatTotal)
parqit tabstat price mpg, stats(mean sd p50) save
assert mreldif(r(StatTotal), nat) < 1e-12
parqit tabstat price mpg, statistics(mean sd p50) save
assert mreldif(r(StatTotal), nat) < 1e-12
capture parqit tabstat price, stats(mean) statistics(sd)
assert _rc == 198

* duplicates report and list from one letter, as native
duplicates report rep78
local u = r(unique_value)
foreach s in r re rep repo repor {
    parqit duplicates `s' rep78
    assert r(unique_value) == `u' & r(N) == 74
}
log using `"`nlog'"', text replace name(v143n)
duplicates l rep78 foreign
log close v143n
log using `"`plog'"', text replace name(v143p)
parqit duplicates l rep78 foreign, limit(1000)
parqit duplicates li rep78 foreign, limit(1000)
parqit duplicates lis rep78 foreign, limit(1000)
log close v143p
python:
from sfi import Macro
def rows(path):
    return [s.rstrip() for s in open(path, encoding="utf-8") if s.startswith(("  +", "  |"))]
nat, pq_ = rows(Macro.getLocal("nlog")), rows(Macro.getLocal("plog"))
assert len(nat) > 20 and pq_ == nat * 3, "\n".join(pq_[:40])
end
capture parqit duplicates dro rep78
assert _rc == 198

* misstable summarize and patterns from three letters, as native
parqit misstable summarize
local nc = r(n_complete)
foreach s in sum summ summa summar summari summariz {
    parqit misstable `s'
    assert r(n_complete) == `nc' & r(N) == 74
}
parqit misstable patterns
local pr = r(r)
foreach s in pat patt patte patter pattern {
    parqit misstable `s'
    assert r(r) == `pr' & r(N) == 74
}
capture parqit misstable pa
assert _rc != 0

* count, list, describe, histogram
foreach c in cou coun {
    parqit `c'
    assert r(N) == 74
}
foreach c in l li lis {
    parqit `c' make price in 1/3
    assert r(N) == 3
}
foreach c in d de des desc descr descri describ {
    parqit `c'
    assert r(n_cols) == 13
}
parqit histogram price, nodraw
scalar w = r(width)
parqit hist price, nodraw
assert r(N) == 74 & r(width) == scalar(w)

* generate, rename and sort change the view as the full names do
parqit use using `"`stem'_auto.parquet"', name(v)
foreach c in g ge gen gene gener genera generat {
    parqit `c' double `c'_x = price * 2
}
parqit ren g_x first
parqit rena ge_x second
parqit renam gen_x third
parqit so price
parqit sor mpg id
parqit collect, clear
assert first == 2 * price & second == first & third == first & generat_x == first
assert mpg >= mpg[_n - 1] if _n > 1
assert id > id[_n - 1] if _n > 1 & mpg == mpg[_n - 1]

* merge and append: the rows native merge and append give
use `"`stem'_auto.dta"', clear
merge 1:1 id using `"`stem'_auto5.dta"', keep(match) nogenerate
local nm = _N
use `"`stem'_auto.dta"', clear
append using `"`stem'_auto5.dta"'
local na = _N
foreach c in mer merg {
    parqit use using `"`stem'_auto.parquet"', name(v)
    parqit `c' 1:1 id using `"`stem'_auto5.parquet"', keep(match) nogenerate
    parqit count
    assert r(N) == `nm'
}
foreach c in ap app appe appen {
    parqit use using `"`stem'_auto.parquet"', name(v)
    parqit `c' using `"`stem'_auto5.parquet"'
    parqit count
    assert r(N) == `na'
}

* mergein takes native merge's nolabels as well as nolabel: neither copies
* the using's value label, which a plain mergein does
foreach o in "" nolabel nolabels {
    use id price using `"`stem'_auto.dta"', clear
    label drop _all
    parqit mergein 1:1 id using `"`stem'_auto.parquet"', keepusing(foreign) nogenerate `o'
    capture label list origin
    assert _rc == cond("`o'" == "", 0, 111)
}

* set: se is Stata's set
parqit use using `"`stem'_auto.parquet"', name(v)
parqit se statamissing on
parqit count if rep78 > 3
assert r(N) == 34
parqit set statamissing off

* each exact parqit verb still reaches its own program
parqit ds
assert `"`r(varlist)'"' != ""
parqit distinct rep78
assert r(ndistinct) == 5
parqit tabstat price
parqit sample 10, count seed(1)
parqit count
assert r(N) == 10
parqit use using `"`stem'_auto.parquet"', name(v)
parqit drop mpg
parqit describe
assert r(n_cols) == 12
parqit selftest
assert `"`r(selftest)'"' != ""
use in 1/5 using `"`stem'_auto.dta"', clear
parqit appendin using `"`stem'_auto5.parquet"'
assert _N == 10

* the words native Stata does not accept for these commands stay refused
foreach w in s co dup dupl samp sam histo histog histogr histogra q qu join h mi ///
    levels kee dro ord resh coll cont codeb gs tabst misst pwc lookf lo glimps {
    capture parqit `w'
    assert _rc == 198
}
parqit close _all

di "VERDICT(V143_STATA_ABBREVIATIONS): PASS - every Stata core short form of use, save, describe, generate, rename, sort, count, list, merge, append, summarize, tabulate, correlate, histogram (hist) and set reaches that verb with native results; duplicates r/l and misstable sum/pat follow native's rules; tabulate column, pwcorr o, tabstat stats() and mergein nolabels take Stata's forms with the old spellings intact; exact parqit verbs are untouched and the words Stata refuses stay refused"
