* V147 — BPLIM feedback, third round (2026-10-13): the if qualifier on every
*   verb native Stata gives one to (IF-ALL-1: egen, collapse, contract, pivot,
*   duplicates drop, use — lazy and eager), the complete native collapse
*   statistics (COLLAPSE-STATS-2: semean sebinomial sepoisson rawsum percent
*   iqr, with the all-missing, one-observation, non-0/1 and negative cases),
*   the compress option of collect and use (COMPRESS-1, lossless: native
*   compress changes nothing afterwards) and the explicit view option of save
*   (SAVE-VIEW-1).
* Oracles: native Stata — collapse, contract, duplicates drop, egen, use with
*   if, compress; pyarrow reads the written file.
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile stem nat
parqit set statamissing off

sysuse auto, clear
gen int id = _n
gen byte fcode = foreign
parqit save `"`stem'_auto.parquet"', data replace

* ---------- collapse: the six new statistics, with if ------------------------
sysuse auto, clear
gen byte fcode = foreign
collapse (mean) price (count) n=price (sum) s=price (sd) sd=price (median) md=price ///
    (p25) q1=price (iqr) iq=price (semean) se=price (sebinomial) seb=fcode ///
    (sepoisson) sep=rep78 (rawsum) rs=price (percent) pct=rep78 (first) f=make ///
    (lastnm) ln=rep78 (max) mx=mpg if mpg > 15, by(foreign)
save `"`nat'"', replace
parqit use using `"`stem'_auto.parquet"'
parqit collapse (mean) price (count) n=price (sum) s=price (sd) sd=price (median) md=price ///
    (p25) q1=price (iqr) iq=price (semean) se=price (sebinomial) seb=fcode ///
    (sepoisson) sep=rep78 (rawsum) rs=price (percent) pct=rep78 (first) f=make ///
    (lastnm) ln=rep78 (max) mx=mpg if mpg > 15, by(foreign)
parqit collect, clear
assert "`: type price'" == "double" & "`: type se'" == "double" & "`: type pct'" == "double"
* native stores these statistics as float (ASSUMPTIONS #122 keeps the double):
* the double rounded to float must equal native's float exactly
recast float price sd md q1 iq se seb sep, force
cf _all using `"`nat'"', verbose
assert abs(pct[1] + pct[2] - 100) < 1e-9
parqit close _all

* edge cases: all-missing group, one observation, a 0/1 variable that is not,
* a negative mean (native: sebinomial and sepoisson are missing, percent 0)
clear
input byte g double x
1 .
1 .
2 5
3 1
3 2
3 .
4 2
4 3
5 -1
5 -3
end
tempfile edge
parqit save `"`edge'.parquet"', data replace
collapse (mean) m=x (sd) sd=x (semean) se=x (sebinomial) sb=x (sepoisson) sp=x ///
    (iqr) iq=x (percent) pc=x (count) n=x (median) md=x (sum) s=x (rawsum) rs=x, by(g)
save `"`nat'"', replace
parqit use using `"`edge'.parquet"'
parqit collapse (mean) m=x (sd) sd=x (semean) se=x (sebinomial) sb=x (sepoisson) sp=x ///
    (iqr) iq=x (percent) pc=x (count) n=x (median) md=x (sum) s=x (rawsum) rs=x, by(g)
parqit collect, clear
assert missing(m[1]) & missing(sd[2]) & missing(se[2]) & missing(sb[2]) & missing(sb[3]) ///
    & missing(sb[4]) & missing(sp[5]) & pc[1] == 0 & n[1] == 0 & s[1] == 0 & rs[1] == 0
* a double source: native keeps double too, so the doubles compare exactly
cf _all using `"`nat'"', verbose
parqit close _all
* no nonmissing value in any group: native's percent is missing (0/0)
clear
input byte g double x
1 .
2 .
end
tempfile allmiss
parqit save `"`allmiss'.parquet"', data replace
collapse (percent) pc=x (count) n=x, by(g)
assert missing(pc[1]) & missing(pc[2]) & n[1] == 0
save `"`nat'"', replace
parqit use using `"`allmiss'.parquet"'
parqit collapse (percent) pc=x (count) n=x, by(g)
parqit collect, clear
cf _all using `"`nat'"', verbose
parqit close _all
* the same on the view, before any collect: a NaN would count as a value
* (native counts 0 nonmissing percents), and the file written holds nulls
parqit use using `"`allmiss'.parquet"'
parqit collapse (percent) pc=x, by(g)
local pcfile `"`allmiss'_pc.parquet"'
parqit save `"`pcfile'"', view replace
parqit collapse (count) n=pc
parqit collect, clear
assert _N == 1 & n[1] == 0
parqit close _all
python:
from sfi import Macro
import pyarrow.parquet as pq
t = pq.read_table(Macro.getLocal("pcfile"))
assert t.num_rows == 2 and t.column("pc").null_count == 2, "a zero denominator must be null on disk"
end

* ---------- contract if, duplicates drop if (varlist and all variables) ------
sysuse auto, clear
contract foreign rep78 if mpg > 25
save `"`nat'"', replace
parqit use using `"`stem'_auto.parquet"'
parqit contract foreign rep78 if mpg > 25
parqit collect, clear
cf _all using `"`nat'"', verbose

sysuse auto, clear
gen int id = _n
sort foreign id
duplicates drop foreign if mpg > 25, force
keep id foreign mpg
save `"`nat'"', replace
parqit use using `"`stem'_auto.parquet"'
parqit sort foreign id
parqit duplicates drop foreign if mpg > 25, force
parqit keep id foreign mpg
parqit collect, clear
cf _all using `"`nat'"', verbose
* the rows outside the condition were kept untouched, those inside kept once
assert _N == 62

sysuse auto, clear
keep foreign rep78 mpg
sort foreign rep78 mpg
duplicates drop if mpg > 20
sort foreign rep78 mpg
save `"`nat'"', replace
parqit use foreign rep78 mpg using `"`stem'_auto.parquet"'
parqit sort foreign rep78 mpg
parqit duplicates drop if mpg > 20
parqit collect, clear
sort foreign rep78 mpg
cf _all using `"`nat'"', verbose
parqit close _all

* ---------- pivot if: the same rows as collapse if ---------------------------
* the if restricts the sample before the cols() check: three cars with
* mpg > 20 have no rep78, so that sample is refused as native reshape would,
* and excluding them in the if is accepted although the column has missing
sysuse auto, clear
parqit use using `"`stem'_auto.parquet"'
capture noisily parqit pivot (mean) price if mpg > 20, rows(foreign) cols(rep78)
assert _rc == 198
parqit pivot (mean) price if mpg > 20 & rep78 < ., rows(foreign) cols(rep78)
parqit collect, clear
assert _N == 2
sysuse auto, clear
* native collapse would store the mean as float (price is int); summarize
* keeps the double the pivot carries
summarize price if mpg > 20 & foreign == 0 & rep78 == 3, meanonly
local native3 = r(mean)
parqit use using `"`stem'_auto.parquet"'
parqit pivot (mean) price if mpg > 20 & rep78 < ., rows(foreign) cols(rep78)
parqit collect, clear
assert reldif(price3[1], `native3') < 1e-12
parqit close _all
* the whole table against native keep if, collapse and reshape wide on a
* double source (native collapse keeps a double source's mean double), and
* the view untouched by the refusal
sysuse auto, clear
gen double p = price
keep if mpg > 20 & rep78 < .
collapse (mean) p, by(foreign rep78)
reshape wide p, i(foreign) j(rep78)
save `"`nat'"', replace
parqit use using `"`stem'_auto.parquet"'
parqit gen p = price
parqit describe
local steps = r(n_steps)
local cols = r(n_cols)
capture noisily parqit pivot (mean) p if mpg > 20, rows(foreign) cols(rep78)
assert _rc == 198
parqit describe
assert r(n_steps) == `steps' & r(n_cols) == `cols'
parqit pivot (mean) p if mpg > 20 & rep78 < ., rows(foreign) cols(rep78)
parqit collect, clear
sort foreign
cf _all using `"`nat'"', verbose
parqit close _all

* ---------- egen if: the sample and the receivers ----------------------------
sysuse auto, clear
* double receivers: native egen's float default would round the mean
egen double tot = total(price) if mpg > 25, by(foreign)
egen double cnt = count(price) if mpg > 25, by(foreign)
egen double mn = mean(price) if mpg > 25, by(foreign)
egen double mx = max(price) if mpg > 25
egen double totm = total(price) if mpg > 25, by(foreign) missing
save `"`nat'"', replace
parqit use using `"`stem'_auto.parquet"'
parqit drop id fcode
parqit egen tot = total(price) if mpg > 25, by(foreign)
parqit egen cnt = count(price) if mpg > 25, by(foreign)
parqit egen mn = mean(price) if mpg > 25, by(foreign)
parqit egen mx = max(price) if mpg > 25
parqit egen totm = total(price) if mpg > 25, by(foreign) missing
parqit collect, clear
cf _all using `"`nat'"', verbose
assert tot[18] == . & tot[7] == 31683
parqit close _all

* ---------- use ... if: eager and lazy, varlist after the condition ----------
sysuse auto, clear
keep if foreign == 1 & mpg > 20
keep make price mpg
save `"`nat'"', replace
parqit use make price mpg if foreign == 1 & mpg > 20 using `"`stem'_auto.parquet"', clear
assert _N == 17
cf _all using `"`nat'"', verbose
parqit use make price mpg if foreign == 1 & mpg > 20 using `"`stem'_auto.parquet"'
parqit describe
assert r(n_cols) == 3
parqit collect, clear
cf _all using `"`nat'"', verbose
parqit close _all
* the eager if keeps the dates and booleans of the file as Stata values
clear
input double d byte b
21915 1
21916 0
21917 1
end
format d %td
tempfile dt
parqit save `"`dt'.parquet"', data replace
parqit use if d > td(01jan2020) & b == 1 using `"`dt'.parquet"', clear
assert _N == 1 & d[1] == 21917
* an eager if keeps the extended missing values the companions preserve
clear
input double v byte k
1 1
.a 2
.b 3
4 4
end
tempfile xm
parqit save `"`xm'.parquet"', data replace xmissing
parqit use if k >= 2 using `"`xm'.parquet"', clear
assert _N == 3 & v[1] == .a & v[2] == .b & v[3] == 4
* refusals: if without using (the file form's syntax, r(101) "if not
* allowed"), if without an expression, _n in the if
capture noisily parqit use `"`stem'_auto.parquet"' if mpg > 20, clear
assert _rc == 101
capture noisily parqit use if using `"`stem'_auto.parquet"', clear
assert _rc == 198
capture noisily parqit use if _n < 3 using `"`stem'_auto.parquet"', clear
assert _rc == 198

* ---------- compress: lossless, as native compress --------------------------
parqit use using `"`stem'_auto.parquet"'
parqit collapse (mean) price (sum) s=price (count) n=price (max) mx=mpg, by(foreign)
parqit collect, clear compress
assert "`: type s'" == "long" & "`: type n'" == "byte" & "`: type mx'" == "byte"
assert "`: type price'" == "double"
tempfile before
save `"`before'"', replace
compress
cf _all using `"`before'"', verbose
describe
parqit close _all
parqit use using `"`stem'_auto.parquet"', clear compress
assert "`: type id'" == "byte"
capture noisily parqit use using `"`stem'_auto.parquet"', compress
assert _rc == 198

* ---------- save, view: explicit, refused without a view ---------------------
sysuse auto, clear
capture noisily parqit save `"`stem'_v.parquet"', view replace
assert _rc == 198
parqit use using `"`stem'_auto.parquet"'
parqit keep if foreign == 1
parqit save `"`stem'_v.parquet"', view replace
assert r(N) == 22
capture noisily parqit save `"`stem'_v.parquet"', view data replace
assert _rc == 198
parqit close _all
parqit describe `"`stem'_v.parquet"'
assert r(n_rows) == 22

di "VERDICT(V147_IF_EVERYWHERE_COLLAPSE_STATS): PASS - collapse's six added statistics equal native (edge groups included); if on collapse, contract, duplicates drop, pivot, egen and use (eager and lazy, dates, booleans and .a-.z preserved) equals native; compress is native compress's lossless narrowing; save, view is explicit and refused without a view"
