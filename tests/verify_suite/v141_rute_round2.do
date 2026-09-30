* V141 — BPLIM feedback, second round (2026-09-29): appendin generate()
*   (APPENDIN-GEN-1), append keep() (APPEND-KEEP-1), tabulate if (TAB-IF-1),
*   duplicates/correlate/pwcorr without a varlist (DUP-ALL-1, CORR-ALL-1),
*   duplicates list shown as native duplicates list (DUP-LIST-1), distinct's
*   Obs and missing as the SSC distinct (DISTINCT-OBS-1), the missing-value
*   warning (STATAMISS-WARN-1) and the reserved-word hint of sql
*   (SQL-KEYWORD-1).
* Oracles: native Stata — append, tabulate, duplicates report and the printed
*   duplicates list table, correlate, pwcorr, egen group(), count.
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile stem

* does <needle> appear in <logfile>? (continuation lines are reassembled)
program define _v141_grep, rclass
    version 16.0
    args logfile needle
    tempname fh
    local found 0
    local cur ""
    file open `fh' using `"`logfile'"', read text
    file read `fh' line
    while (!r(eof)) {
        if (substr(`"`macval(line)'"', 1, 2) == "> ") {
            local cur `"`macval(cur)'`=substr(`"`macval(line)'"', 3, .)'"'
        }
        else {
            if (strpos(`"`macval(cur)'"', `"`needle'"')) local found 1
            local cur `"`macval(line)'"'
        }
        file read `fh' line
    }
    if (strpos(`"`macval(cur)'"', `"`needle'"')) local found 1
    file close `fh'
    return scalar found = `found'
end

* a unique sort key keeps these checks about the verbs themselves (the order
* within a tie of auto's own sort, foreign, is v142's subject)
sysuse auto, clear
gen int id = _n
sort id
save `"`stem'_auto.dta"', replace
parqit save `"`stem'_auto.parquet"', data replace
keep in 1/5
save `"`stem'_auto5.dta"', replace
parqit save `"`stem'_auto5.parquet"', data replace

* ---------- appendin generate(): native append's marker ----------------------
use in 70/74 using `"`stem'_auto.dta"', clear
append using `"`stem'_auto5.dta"', keep(price rep78) generate(src)
tempfile nat
save `"`nat'"'
use in 70/74 using `"`stem'_auto.dta"', clear
parqit appendin using `"`stem'_auto5.parquet"', keep(price rep78) generate(src)
cf _all using `"`nat'"'
assert "`: value label src'" != ""

* ---------- append keep(): the lazy union equals native append ---------------
use in 70/74 using `"`stem'_auto.dta"', clear
append using `"`stem'_auto5.dta"' `"`stem'_auto5.dta"', keep(pri* rep78) generate(src)
keep make price mpg rep78 src
sort src make price rep78
save `"`nat'"', replace
parqit use using `"`stem'_auto.parquet"', name(ap)
parqit keep in 70/74
parqit append using `"`stem'_auto5.parquet"' `"`stem'_auto5.parquet"', keep(pri* rep78) generate(src)
parqit collect, clear
keep make price mpg rep78 src
sort src make price rep78
cf _all using `"`nat'"'
* the marker is labelled as native append's (APPEND-GEN-LABEL-1)
parqit use using `"`stem'_auto.parquet"', name(ag)
parqit append using `"`stem'_auto5.parquet"' `"`stem'_auto5.parquet"', generate(src)
parqit append using `"`stem'_auto5.parquet"', generate(src2)
parqit collect, clear
assert "`: value label src'" == "_append" & "`: variable label src'" == "Dataset source"
assert "`: label _append 0'" == "Master" & "`: label _append 2'" == "Appended dataset 2"
assert "`: value label src2'" == "__append1" & "`: label __append1 1'" == "Appended dataset 1"
assert "`: format src'" == "%18.0g"
* a name absent from a using source: rc 111 (native), the view unchanged
parqit use using `"`stem'_auto.parquet"', name(ak)
capture noisily parqit append using `"`stem'_auto5.parquet"', keep(price nonexistent)
assert _rc == 111
parqit count
assert r(N) == 74
parqit close _all

* ---------- tabulate if: the rows that satisfy exp, as native ----------------
use `"`stem'_auto.dta"', clear
tabulate foreign if price > 5000
local n1 = r(N)
tabulate rep78 foreign if inlist(rep78, 3, 4) & make != "AMC Concord", missing
local n2 = r(N)
local r2 = r(r)
local c2 = r(c)
parqit use using `"`stem'_auto.parquet"', name(t)
parqit tabulate foreign if price > 5000
assert r(N) == `n1'
parqit tabulate rep78 foreign if inlist(rep78, 3, 4) & make != "AMC Concord", missing
assert r(N) == `n2' & r(r) == `r2' & r(c) == `c2'
capture noisily parqit tabulate foreign if
assert _rc == 198
parqit count
assert r(N) == 74

* ---------- if on every statistics command (STATS-IF-1): as native ----------
use `"`stem'_auto.dta"', clear
quietly summarize price mpg if price > 5000 & !missing(rep78)
local sm = r(mean)
local sn = r(N)
quietly tabstat price mpg if price > 5000 & !missing(rep78), statistics(mean sd) save
matrix Ti = r(StatTotal)
quietly levelsof rep78 if price > 5000
local lv `"`r(levels)'"'
quietly duplicates report rep78 if price > 5000
local du = r(unique_value)
quietly correlate price mpg weight if price > 5000 & !missing(rep78)
matrix Ci = r(C)
quietly pwcorr price mpg if foreign
matrix Pi = r(C)
quietly count if !missing(rep78) & price > 5000
local dn = r(N)
parqit use using `"`stem'_auto.parquet"', name(si)
parqit summarize price mpg if price > 5000 & !missing(rep78)
assert reldif(r(mean), `sm') < 1e-12 & r(N) == `sn'
parqit tabstat price mpg if price > 5000 & !missing(rep78), statistics(mean sd) save
assert mreldif(r(StatTotal), Ti) < 1e-12
parqit levelsof rep78 if price > 5000
assert `"`r(levels)'"' == `"`lv'"'
parqit duplicates report rep78 if price > 5000
assert r(unique_value) == `du'
parqit correlate price mpg weight if price > 5000 & !missing(rep78)
assert mreldif(r(C), Ci) < 1e-12
parqit pwcorr price mpg if foreign
assert mreldif(r(C), Pi) < 1e-12
parqit distinct rep78 if price > 5000
assert r(N) == `dn'
parqit codebook rep78 if price > 5000
parqit misstable summarize if price > 5000
parqit histogram price if price > 5000, nodraw
capture noisily parqit summarize price if
assert _rc == 198
parqit count
assert r(N) == 74
* duplicates list: Obs is the row's _n in the whole view, as native
tempfile ilog nilog
log using `"`ilog'"', text replace name(v141i)
parqit duplicates list rep78 foreign if price > 11000, limit(1000)
log close v141i
parqit close si
parqit view t
use `"`stem'_auto.dta"', clear
log using `"`nilog'"', text replace name(v141ni)
duplicates list rep78 foreign if price > 11000
log close v141ni
python:
from sfi import Macro
def rows(path):
    return [x.rstrip("\n") for x in open(path, encoding="utf-8")
            if x.startswith("  +") or x.startswith("  |")]
a, b = rows(Macro.getLocal("ilog")), rows(Macro.getLocal("nilog"))
assert a and a == b, (a, b)
end

* ---------- duplicates report with and without a varlist ---------------------
use `"`stem'_auto.dta"', clear
duplicates report
local u0 = r(unique_value)
local n0 = r(N)
duplicates report rep78
local u1 = r(unique_value)
parqit duplicates report
assert r(unique_value) == `u0' & r(N) == `n0'
parqit duplicates report rep78
assert r(unique_value) == `u1'

* ---------- duplicates list: the table native duplicates list prints ---------
tempfile nlog plog
use `"`stem'_auto.dta"', clear
log using `"`nlog'"', text replace name(v141n)
duplicates list rep78 foreign
duplicates list make
duplicates list foreign
log close v141n
parqit use using `"`stem'_auto.parquet"', name(dl)
log using `"`plog'"', text replace name(v141p)
parqit duplicates list rep78 foreign, limit(1000)
parqit duplicates list make
parqit duplicates list foreign, limit(1000)
log close v141p
python:
from sfi import Macro
def table(path):
    keep = []
    for line in open(path, encoding="utf-8"):
        s = line.rstrip("\n")
        if s.startswith("  +") or s.startswith("  |") or s.startswith("Duplicates in terms of") \
           or s.startswith("(0 observations"):
            keep.append(s)
    return keep
nat, pq_ = table(Macro.getLocal("nlog")), table(Macro.getLocal("plog"))
assert len(nat) > 20, nat
assert nat == pq_, "\n".join(a + "  <>  " + b for a, b in zip(nat, pq_) if a != b)
end
* limit() shows the first rows and says how many there are
log using `"`plog'"', text replace name(v141q)
parqit duplicates list rep78, limit(3)
log close v141q
_v141_grep `"`plog'"' "(first 3 of 74 observations in duplicate groups"
assert r(found) == 1

* ---------- correlate and pwcorr without a varlist ---------------------------
use `"`stem'_auto.dta"', clear
correlate
matrix Cn = r(C)
local cn = r(N)
pwcorr
matrix Pn = r(C)
parqit use using `"`stem'_auto.parquet"', name(c)
parqit correlate
assert mreldif(r(C), Cn) < 1e-12 & r(N) == `cn'
assert colsof(r(C)) == 12
parqit pwcorr
assert mreldif(r(C), Pn) < 1e-12
parqit close _all

* ---------- distinct: Obs and missing as the SSC distinct --------------------
use `"`stem'_auto.dta"', clear
quietly count if !missing(rep78)
local nm = r(N)
egen g1 = group(rep78 foreign)
summarize g1, meanonly
local j1 = r(max)
local jn1 = r(N)
egen g2 = group(rep78 foreign), missing
summarize g2, meanonly
local j2 = r(max)
parqit use using `"`stem'_auto.parquet"', name(s)
parqit distinct rep78
assert r(N) == `nm' & r(ndistinct) == 5
parqit distinct rep78, missing
assert r(N) == 74 & r(ndistinct) == 6
parqit distinct rep78 foreign, joint
assert r(N) == `jn1' & r(ndistinct) == `j1'
parqit distinct rep78 foreign, joint missing
assert r(N) == 74 & r(ndistinct) == `j2'
parqit close _all

* ---------- sql: a reserved word used as a name ------------------------------
tempfile slog
log using `"`slog'"', text replace name(v141s)
capture noisily parqit sql `"SELECT make, foreign FROM read_parquet('`stem'_auto.parquet')"', name(q)
local rc = _rc
log close v141s
assert `rc' == 920
_v141_grep `"`slog'"' `"foreign is a reserved word in SQL"'
assert r(found) == 1
parqit sql `"SELECT make, "foreign" FROM read_parquet('`stem'_auto.parquet')"', name(q)
parqit count
assert r(N) == 74
parqit close _all

* ---------- partitions(): what differs, and a partition extended -------------
* (PART-META-DIFF-1, PART-NOTE-1)
tempfile plog2
clear
set obs 4
gen int year = 2025
gen byte month = 1 + mod(_n - 1, 2)
gen str5 cae = "47111"
gen double wage = _n
label variable wage "Wage"
parqit save `"`stem'_tree"', data replace partition_by(year month)
log using `"`plog2'"', text replace name(v141pa)
recast str8 cae
label variable wage "Salary"
capture noisily parqit save `"`stem'_tree"', data partition_by(year month) partitions(replace)
local rc = _rc
recast str5 cae
label variable wage "Wage"
keep if month == 1
parqit save `"`stem'_tree"', data partition_by(year month) partitions(append)
log close v141pa
assert `rc' == 198
_v141_grep `"`plog2'"' "cae: storage type str5 in the tree, str8 in the result"
assert r(found) == 1
_v141_grep `"`plog2'"' `"wage: variable label "Wage" in the tree, "Salary" in the result"'
assert r(found) == 1
_v141_grep `"`plog2'"' "(0 partitions replaced, 0 added, 1 extended with a new file under"
assert r(found) == 1

* ---------- the missing-value warning ----------------------------------------
* (last: parqit set statamissing silences it for the rest of the session)
use `"`stem'_auto.dta"', clear
count if rep78 > 3
local stata_n = r(N)
tempfile wlog
parqit use using `"`stem'_auto.parquet"', name(w)
log using `"`wlog'"', text replace name(v141w)
parqit count if rep78 > 3
local sql_n = r(N)
log close v141w
assert `sql_n' == `stata_n' - 5
_v141_grep `"`wlog'"' "warning: rep78 > 3:"
assert r(found) == 1
* like any output it is silenced by quietly and capture, and never carried over
log using `"`wlog'"', text replace name(v141wq)
quietly parqit count if rep78 > 3
assert r(N) == `sql_n'
capture parqit count if rep78 > 3
assert _rc == 0
log close v141wq
_v141_grep `"`wlog'"' "warning:"
assert r(found) == 0
assert `"$PARQIT_MISSWARN"' == ""
* idioms that settle the missing rows stay silent
log using `"`wlog'"', text replace name(v141x)
parqit count if rep78 > 3 & rep78 < .
parqit count if rep78 < 3
parqit count if missing(rep78) | rep78 > 3
parqit gen byte hi = price > 6000 if !missing(price)
log close v141x
_v141_grep `"`wlog'"' "warning:"
assert r(found) == 0
* an assigned comparison, and a filter verb
log using `"`wlog'"', text replace name(v141y)
parqit gen byte good = rep78 > 3
parqit keep if rep78 != 3
log close v141y
_v141_grep `"`wlog'"' "warning: rep78 > 3:"
assert r(found) == 1
_v141_grep `"`wlog'"' "warning: rep78 != 3:"
assert r(found) == 1
parqit close _all
* choosing a mode silences it; on follows Stata
parqit use using `"`stem'_auto.parquet"', name(w2)
log using `"`wlog'"', text replace name(v141z)
parqit set statamissing on
parqit count if rep78 > 3
assert r(N) == `stata_n'
parqit set statamissing off
parqit count if rep78 > 3
assert r(N) == `sql_n'
log close v141z
_v141_grep `"`wlog'"' "warning:"
assert r(found) == 0
parqit close _all

di "VERDICT(V141_RUTE_ROUND2): PASS - appendin generate() and append keep() equal native append (keep() refused loudly before mutation); tabulate if equals native; duplicates report/correlate/pwcorr use every variable without a varlist; duplicates list prints native duplicates list's table; distinct Obs/missing/joint equal the SSC distinct; the missing-value warning names the differing comparisons, is silenced by quietly and capture, stays silent on the settling idioms and after parqit set statamissing; sql names a reserved word"
