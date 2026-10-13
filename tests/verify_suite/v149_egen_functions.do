* V149 — BPLIM feedback, third round (2026-10-13): the native egen functions
*   on a lazy view (EGEN-ALL-1) — the group functions with by(), the key
*   functions, the row functions and the string functions, each option
*   native egen gives them, the if qualifier's rules per function (tag,
*   anycount and anymatch give 0 to the rows outside it, concat and ends "",
*   the others missing), and the refusals (fill and mtr, by on the functions
*   native refuses it for, unknown functions, exclusive options).
*   Native stores most results as float: parqit keeps the double (ASSUMPTIONS
*   #122), so the comparison rounds parqit's double to float where native's
*   value is float.
* Oracle: native egen.
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile stem nat
parqit set statamissing off

* ---------- the if qualifier, function by function --------------------------
sysuse auto, clear
gen int id = _n
parqit save `"`stem'_auto.parquet"', data replace
keep in 1/20
egen t = tag(foreign) if mpg > 20
egen ac = anycount(rep78 trunk) if mpg > 20, values(3 4)
egen am = anymatch(rep78) if mpg > 20, values(3 4)
egen av = anyvalue(rep78) if mpg > 20, values(3 4)
egen g = group(foreign rep78) if mpg > 20
egen s = seq() if mpg > 20, by(foreign)
egen r = rank(price) if mpg > 20, by(foreign)
egen md = median(price) if mpg > 20, by(foreign)
egen rt = rowtotal(price mpg) if mpg > 20
egen rm = rowmiss(price rep78) if mpg > 20
egen cc = concat(make rep78 mpg) if mpg > 20, punct(-)
egen df = diff(price mpg) if mpg > 20
egen mo = mode(rep78) if mpg > 20
egen en = ends(make) if mpg > 20, punct(" ") last
recast double av g s r md rt rm, force
save `"`nat'"', replace
parqit use using `"`stem'_auto.parquet"'
parqit keep if id <= 20
parqit egen t = tag(foreign) if mpg > 20
parqit egen ac = anycount(rep78 trunk) if mpg > 20, values(3 4)
parqit egen am = anymatch(rep78) if mpg > 20, values(3 4)
parqit egen av = anyvalue(rep78) if mpg > 20, values(3 4)
parqit egen g = group(foreign rep78) if mpg > 20
parqit egen s = seq() if mpg > 20, by(foreign)
parqit egen r = rank(price) if mpg > 20, by(foreign)
parqit egen md = median(price) if mpg > 20, by(foreign)
parqit egen rt = rowtotal(price mpg) if mpg > 20
parqit egen rm = rowmiss(price rep78) if mpg > 20
parqit egen cc = concat(make rep78 mpg) if mpg > 20, punct(-)
parqit egen df = diff(price mpg) if mpg > 20
parqit egen mo = mode(rep78) if mpg > 20
parqit egen en = ends(make) if mpg > 20, punct(" ") last
parqit collect, clear
assert "`: type t'" == "byte" & "`: type ac'" == "byte" & "`: type am'" == "byte" & "`: type df'" == "byte"
cf _all using `"`nat'"', verbose
parqit close _all

* ---------- every function with its options -------------------------------
sysuse auto, clear
gen int id = _n
gen double x = price
replace x = . if mod(_n, 3) == 0
sort foreign id
tempfile auto2
parqit save `"`auto2'.parquet"', data replace
by foreign: egen double tm = total(x), missing
by foreign: egen double tz = total(x)
egen gm = group(foreign rep78), missing
egen gl = group(foreign rep78), label
egen gt = group(make), label truncate(3)
egen cu = cut(price), group(4)
egen cu2 = cut(price), at(3000 5000 10000 20000)
egen cu3 = cut(price), at(3000 5000 10000 20000) icodes
egen cul = cut(price), at(3000 5000 10000 20000) label
egen cc2 = concat(make price), format(%9.2f) punct(|)
egen cd = concat(foreign rep78), decode punct(" ") maxlength(3)
egen se2 = seq(), from(1) to(3) block(2)
egen se3 = seq(), from(5) to(1)
egen rk_u = rank(mpg), unique
egen rk_f = rank(mpg), field
egen rk_t = rank(mpg), track
egen double st = std(price), mean(100) sd(10)
egen double pcp = pc(price), prop
egen pcc = pc(price), by(foreign)
egen m_min = mode(rep78), minmode
egen m_max = mode(rep78), maxmode
egen m_1 = mode(rep78), nummode(1)
egen m_s = mode(make)
egen m_mi = mode(rep78), missing by(foreign)
egen rnm = rownonmiss(make price rep78), strok
egen double rp = rowpctile(price mpg trunk), p(30)
egen double rp2 = rowpctile(price mpg trunk weight), p(50)
egen double rmd = rowmedian(price mpg trunk weight)
egen en1 = ends(make), punct(" ") last
egen en2 = ends(make), punct(" ") tail
egen en3 = ends(make), punct(" ") trim
egen double iq = iqr(price), by(foreign)
egen sk = skew(price), by(foreign)
egen ku = kurt(price), by(foreign)
egen double mad_ = mad(price), by(foreign)
egen mdev = mdev(price), by(foreign)
egen rsd = rowsd(price mpg trunk)
egen rmin = rowmin(price mpg)
egen rmax = rowmax(price mpg)
egen rfirst = rowfirst(rep78 price)
egen rlast = rowlast(rep78 price)
egen double rmean = rowmean(price mpg)
egen rtm = rowtotal(rep78 x), missing
egen mx = max(x), by(foreign)
egen mxm = max(x), by(foreign) missing
egen mn = min(x), by(foreign)
egen double p25 = pctile(price), p(25) by(foreign)
egen double p90 = pctile(price), p(90)
egen cnt = count(x), by(foreign)
egen double sdv = sd(x), by(foreign)
egen double mea = mean(x), by(foreign)
egen ac2 = anycount(rep78 trunk mpg), values(3/5 22)
recast double tm tz gm gl gt cu cu2 cu3 cul se2 se3 rk_u rk_f rk_t rnm rp rp2 rmd iq mx mxm mn p25 p90 cnt mea ac2, force
* unique ranks: native breaks ties arbitrarily (an unstable sort), parqit by
* the view's order; the comparison is on the properties, not the values
drop rk_u
recast double st pcp pcc sk ku mad_ mdev rsd rmin rmax rfirst rlast rmean rtm sdv, force
save `"`nat'"', replace
parqit use using `"`auto2'.parquet"'
parqit sort foreign id
parqit by foreign: egen tm = total(x), missing
parqit by foreign: egen tz = total(x)
parqit egen gm = group(foreign rep78), missing
parqit egen gl = group(foreign rep78), label
parqit egen gt = group(make), label truncate(3)
parqit egen cu = cut(price), group(4)
parqit egen cu2 = cut(price), at(3000 5000 10000 20000)
parqit egen cu3 = cut(price), at(3000 5000 10000 20000) icodes
parqit egen cul = cut(price), at(3000 5000 10000 20000) label
parqit egen cc2 = concat(make price), format(%9.2f) punct(|)
parqit egen cd = concat(foreign rep78), decode punct(" ") maxlength(3)
parqit egen se2 = seq(), from(1) to(3) block(2)
parqit egen se3 = seq(), from(5) to(1)
parqit egen rk_u = rank(mpg), unique
parqit egen rk_f = rank(mpg), field
parqit egen rk_t = rank(mpg), track
parqit egen st = std(price), mean(100) sd(10)
parqit egen pcp = pc(price), prop
parqit egen pcc = pc(price), by(foreign)
parqit egen m_min = mode(rep78), minmode
parqit egen m_max = mode(rep78), maxmode
parqit egen m_1 = mode(rep78), nummode(1)
parqit egen m_s = mode(make)
parqit egen m_mi = mode(rep78), missing by(foreign)
parqit egen rnm = rownonmiss(make price rep78), strok
parqit egen rp = rowpctile(price mpg trunk), p(30)
parqit egen rp2 = rowpctile(price mpg trunk weight), p(50)
parqit egen rmd = rowmedian(price mpg trunk weight)
parqit egen en1 = ends(make), punct(" ") last
parqit egen en2 = ends(make), punct(" ") tail
parqit egen en3 = ends(make), punct(" ") trim
parqit egen iq = iqr(price), by(foreign)
parqit egen sk = skew(price), by(foreign)
parqit egen ku = kurt(price), by(foreign)
parqit egen mad_ = mad(price), by(foreign)
parqit egen mdev = mdev(price), by(foreign)
parqit egen rsd = rowsd(price mpg trunk)
parqit egen rmin = rowmin(price mpg)
parqit egen rmax = rowmax(price mpg)
parqit egen rfirst = rowfirst(rep78 price)
parqit egen rlast = rowlast(rep78 price)
parqit egen rmean = rowmean(price mpg)
parqit egen rtm = rowtotal(rep78 x), missing
parqit egen mx = max(x), by(foreign)
parqit egen mxm = max(x), by(foreign) missing
parqit egen mn = min(x), by(foreign)
parqit egen p25 = pctile(price), p(25) by(foreign)
parqit egen p90 = pctile(price), p(90)
parqit egen cnt = count(x), by(foreign)
parqit egen sdv = sd(x), by(foreign)
parqit egen mea = mean(x), by(foreign)
parqit egen ac2 = anycount(rep78 trunk mpg), values(3/5 22)
parqit collect, clear
* EXACT-ORACLE: the five results whose native arithmetic differs are compared
* with exact rational (fractions) and 50-digit (decimal) values of the same
* definitions: the double must be within 4 ulp of the correctly rounded value
python:
from sfi import Data, Scalar
from fractions import Fraction as F
from decimal import Decimal, getcontext
import math
getcontext().prec = 50
rows = Data.get(var=["foreign", "price", "mpg", "trunk", "mdev", "rsd", "pcc", "sk", "ku"], missingval=None)
def ulp(b):
    return math.ulp(b) if hasattr(math, "ulp") else 2.0 ** (math.frexp(b)[1] - 53)
def dec(f):
    return Decimal(f.numerator) / Decimal(f.denominator)
def check(name, got, exact):
    global worst
    e = float(exact)
    d = abs(got - e) / ulp(e) if e != 0 else abs(got - e)
    worst = max(worst, d)
    assert d <= 4, "%s: %r vs exact %r (%g ulp)" % (name, got, e, d)
worst = 0.0
groups = {}
for r in rows:
    groups.setdefault(r[0], []).append(r)
for g, rs in groups.items():
    xs = [F(int(r[1])) for r in rs]
    n = len(xs)
    mean = sum(xs) / n
    mdev = sum(abs(x - mean) for x in xs) / n
    m2 = sum((x - mean) ** 2 for x in xs) / n
    m3 = sum((x - mean) ** 3 for x in xs) / n
    m4 = sum((x - mean) ** 4 for x in xs) / n
    sk = dec(m3) / (dec(m2).sqrt() ** 3)
    ku = m4 / (m2 * m2)
    total = sum(xs)
    for r in rs:
        check("mdev", r[4], mdev)
        check("sk", r[7], sk)
        check("ku", r[8], ku)
        check("pcc", r[6], F(100) * F(int(r[1])) / total)
        v = [F(int(r[1])), F(int(r[2])), F(int(r[3]))]
        m = sum(v) / 3
        check("rsd", r[5], (dec(sum((a - m) ** 2 for a in v) / 2)).sqrt())
Scalar.setValue("worst_ulp", worst)
end
di "worst distance from the exact value: " scalar(worst_ulp) " ulp"
assert scalar(worst_ulp) <= 4
* native computes mean, sd, total, std, mad, pc(prop), the percentiles and
* the row means in double exactly as parqit does (bit-for-bit, egen double
* above); mdev, rowsd, pc() by group, skewness and kurtosis accumulate in a
* different order, so for those the double is checked against an exact
* oracle below and only its float rounding against native here
foreach v in mdev rsd pcc sk ku {
    replace `v' = float(`v')
}
* unique ranks: a permutation of 1..N that increases with mpg
sort mpg rk_u
assert rk_u == _n
drop rk_u
sort foreign id
cf _all using `"`nat'"', verbose
* the labels group(), label and cut(), label define: the same texts as native
label list gl
assert "`: label gl 1'" == "Domestic 1" & "`: label gl 8'" == "Foreign 5"
assert "`: label gt 1'" == "AMC"
assert "`: label cul 0'" == "3000-" & "`: label cul 2'" == "10000-"
parqit close _all

* ---------- refusals ---------------------------------------------------------
parqit use using `"`stem'_auto.parquet"'
parqit describe
local k = r(n_cols)
capture noisily parqit egen f = fill(1 2 3)
assert _rc == 198
capture noisily parqit egen f = mtr(1990 price)
assert _rc == 198
capture noisily parqit egen f = nosuchfunction(price)
assert _rc == 198
capture noisily parqit egen f = rowtotal(price mpg), by(foreign)
assert _rc == 198
capture noisily parqit egen f = tag(foreign), by(foreign)
assert _rc == 198
capture noisily parqit egen f = rank(price), field track
assert _rc == 198
capture noisily parqit egen f = mode(rep78), minmode maxmode
assert _rc == 198
capture noisily parqit egen f = cut(price), at(1 2) group(3)
assert _rc == 198
capture noisily parqit egen f = cut(price)
assert _rc == 198
capture noisily parqit egen f = anycount(rep78)
assert _rc == 198
capture noisily parqit egen f = rowtotal(make price)
assert _rc == 198
capture noisily parqit egen f = ends(price)
assert _rc == 198
capture noisily parqit egen f = concat(make price), format(%9.0g)
assert _rc == 198
capture noisily parqit egen f = pctile(price), p(100)
assert _rc == 198
capture noisily parqit egen str8 f = mean(price)
assert _rc == 198
capture noisily parqit egen f = seq(price)
assert _rc == 198
capture noisily parqit egen f = mean(price) if, by(foreign)
assert _rc == 198
capture noisily parqit by foreign: egen f = mean(price), by(rep78)
assert _rc == 198
parqit describe
assert r(n_cols) == `k'
* an explicit storage type is value semantics, as native
parqit egen byte tb = total(price), by(foreign)
parqit egen int ti = count(price), by(foreign)
parqit collect, clear
assert tb[1] == . & "`: type tb'" == "byte" & ti[1] == 52 & "`: type ti'" == "int"
parqit close _all

di "VERDICT(V149_EGEN_FUNCTIONS): PASS - every native egen function supported on a view equals native with its options (bit-for-bit in double where the arithmetic coincides, within 4 ulp of an exact oracle where native accumulates differently) and under if (tag/anycount/anymatch 0, concat/ends empty, the others missing outside the condition); group label, cut label and truncate define native's labels; fill, mtr, by on the row/key/string functions, exclusive options and type mismatches are refused with the view unchanged"
