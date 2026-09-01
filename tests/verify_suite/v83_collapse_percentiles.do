* V83 — PCT-WINDOW-1 (audit 2026-09-01, T9): collapse percentiles are computed
*   from per-group ranks (a window over the nonmissing values) instead of an
*   in-memory list_sort(list(x)) per group. The values must stay exactly
*   native's (summarize-detail rule: integral np averages x[np], x[np+1];
*   otherwise x[ceil(np)]): odd and even group sizes, n = 1, all-missing
*   groups, ties, integer and float sources, p1/p25/p33.3/p50/p75/p99, with and
*   without by(), several percentiles of one source, alongside first/last;
*   tabstat median/p## (same rank window) against native tabstat, save.
*   Oracle: native collapse on the same data, compared with cf; native tabstat.
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

tempfile stem
local src `"`stem'_src.parquet"'
set seed 20260901
clear
set obs 5000
gen long id = _n
gen int g = floor(runiform() * 97)          /* group sizes ~ 1..100, uneven */
replace g = 200 in 1                          /* a group of size 1 */
replace g = 201 in 2/3                        /* size 2: an even split */
gen double x = round(rnormal() * 100, 0.01)
replace x = . if mod(_n, 13) == 0             /* missings inside groups */
replace x = . if g == 5                       /* an all-missing group */
gen float f = round(runiform() * 10, 0.5)     /* many ties */
gen long k = floor(runiform() * 20)           /* integer source, ties */
replace k = . in 4/6
gen double hole = .                           /* all missing everywhere */
tempfile ref
save `"`ref'"', replace
parqit save `"`src'"', replace data

* ---------- with by() ----------------------------------------------------------
use `"`ref'"', clear
collapse (median) mx = x (p25) x25 = x (p75) x75 = x (p1) x1 = x (p99) x99 = x ///
    (p33) x33 = x (median) mf = f (p10) f10 = f (median) mk = k (p90) k90 = k    ///
    (median) mh = hole (count) n = x, by(g)
sort g
tempfile nat
save `"`nat'"', replace
parqit use using `"`src'"'
parqit collapse (median) mx = x (p25) x25 = x (p75) x75 = x (p1) x1 = x (p99) x99 = x ///
    (p33) x33 = x (median) mf = f (p10) f10 = f (median) mk = k (p90) k90 = k    ///
    (median) mh = hole (count) n = x, by(g)
parqit collect, clear
sort g
cf _all using `"`nat'"'
parqit close _all

* ---------- without by() ---------------------------------------------------------
use `"`ref'"', clear
collapse (median) mx = x (p25) x25 = x (p99) x99 = x (median) mf = f (p50) mk = k (median) mh = hole
tempfile nat2
save `"`nat2'"', replace
parqit use using `"`src'"'
parqit collapse (median) mx = x (p25) x25 = x (p99) x99 = x (median) mf = f (p50) mk = k (median) mh = hole
parqit collect, clear
cf _all using `"`nat2'"'
parqit close _all

* ---------- percentiles beside first/last on a sorted view -----------------------
use `"`ref'"', clear
sort g id
collapse (first) fx = x (last) lx = x (median) mx = x (p75) x75 = f, by(g)
sort g
tempfile nat3
save `"`nat3'"', replace
parqit use using `"`src'"'
parqit sort g id
parqit collapse (first) fx = x (last) lx = x (median) mx = x (p75) x75 = f, by(g)
parqit collect, clear
sort g
cf _all using `"`nat3'"'
parqit close _all

* ---------- tabstat percentiles (same rank window; oracle native tabstat, save) ---
use `"`ref'"', clear
tempfile nat_ts
file open fh using `"`nat_ts'"', write text replace
tabstat x f, statistics(median p25 p75 p90) by(g) save
local k = 1
while ("`r(name`k')'" != "") {
    matrix S = r(Stat`k')
    forvalues c = 1/2 {
        forvalues r = 1/4 {
            file write fh "`=word("x f", `c')' `r(name`k')' `r' `=string(S[`r', `c'], "%18.0g")'" _n
        }
    }
    local ++k
}
tabstat x f, statistics(median p25 p75 p90) save
matrix S = r(StatTotal)
forvalues c = 1/2 {
    forvalues r = 1/4 {
        file write fh "`=word("x f", `c')' (all) `r' `=string(S[`r', `c'], "%18.0g")'" _n
    }
}
file close fh
tempfile tslog
log using `"`tslog'"', replace name(v83ts) text
parqit use using `"`src'"'
parqit tabstat x f, statistics(median p25 p75 p90) by(g)
parqit tabstat x f, statistics(median p25 p75 p90)
parqit close _all
log close v83ts
python:
from sfi import Macro
import re
num = lambda t: None if t == "." else float(t)     # "." is missing on both sides
nat = {}
for line in open(Macro.getLocal("nat_ts"), encoding="utf-8"):
    v, g, r, val = line.split()
    nat[(v, g, int(r))] = num(val)
got = {}
for line in open(Macro.getLocal("tslog"), encoding="utf-8", errors="replace"):
    m = re.match(r"^\s+(x|f)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s*$", line)
    if m:
        for r in range(4):
            got[(m.group(1), m.group(2), r + 1)] = num(m.group(3 + r))
same = lambda a, b: (a is None and b is None) or (a is not None and b is not None and abs(a - b) <= 1e-8 * max(1.0, abs(b)))
bad = [k for k in nat if k not in got or not same(got[k], nat[k])]
Macro.setLocal("ts_ok", "1" if not bad and len(got) == len(nat) and len(nat) >= 8 * 100 else "0")
Macro.setLocal("ts_info", "native %d parqit %d bad %d" % (len(nat), len(got), len(bad)))
end
di "tabstat percentiles: `ts_info'"
assert "`ts_ok'" == "1"

* ---------- a string source is refused for a percentile ---------------------------
parqit use using `"`src'"'
parqit gen s = cond(k > 5, "hi", "lo")
capture noisily parqit collapse (median) ms = s, by(g)
assert _rc != 0
parqit close _all

di "VERDICT(V83_COLLAPSE_PERCENTILES): PASS - window-ranked collapse percentiles equal native collapse (by/no by, odd/even/one-row/all-missing groups, ties, integer and float sources, p1..p99, beside first/last; tabstat median/p## by/no-by against native tabstat)"
