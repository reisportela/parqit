* V130 — SPSS-READ-1 (paths and refusals): a .sav/.zsav is read wherever
* parqit reads a file — `parqit use` lazy and eager (with a varlist that keeps
* labels and characteristics on the right variables), the using side of
* merge/append, mergein — through a package-owned bridge that is cleaned up;
* `parqit save <file> using <x.sav>` never touches the dataset in memory or
* the open views; and every misuse is a loud, specific refusal that leaves no
* file behind: a truncated or non-SPSS file, a missing file, the SPSS file
* as its own destination, an existing destination without replace, options
* that belong to other saves, an encoding() parqit cannot decode, describe.
* Needs Stata's Python (standard library only); fixtures in tests/fixtures/spss.
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile stem lg
local fix `"`repo'/tests/fixtures/spss"'
local sav `"`fix'/survey.sav"'
local fails 0

program define _v130_grep, rclass
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

program define _v130_bridges, rclass
    version 16.0
    mata: st_numscalar("r(n)", rows(dir(c("tmpdir"), "dirs", "_parqit_bridge_spss_*")))
    return scalar n = r(n)
end

* ---- lazy view: user-missing codes fold to . (said), the lossless route named
log using `"`lg'"', text replace name(v130)
parqit use using `"`sav'"', name(sv)
log close v130
_v130_grep `"`lg'"' "extended missing values (.a-.z) preserved in the file for q1"
if (!r(found)) local ++fails
_v130_grep `"`lg'"' "convert it directly: parqit save newfile.parquet using"
if (!r(found)) local ++fails
_v130_grep `"`lg'"' `"opened over `sav'"'
if (!r(found)) local ++fails
parqit collect, clear
count if q1 >= .
local nmiss = r(N)
count if q1 > .
if (r(N) != 0 | `nmiss' == 0) local ++fails     /* folded to plain . */
local lab : value label q1
if ("`lab'" != "q1") local ++fails
local f : char q1[spss_format]
if ("`f'" != "F2.0") local ++fails
parqit close _all
_v130_bridges
if (r(n) != 0) {
    di as err "lazy-view bridge left behind"
    local ++fails
}

* ---- eager with a varlist: labels and chars stay on the right variables ----
parqit use q2 Satisfaction_with_public_service sex q_3 using `"`sav'"', clear
if (c(k) != 4 | _N != 60) local ++fails
local m : char q2[spss_missing_map]
if ("`m'" != ".a=99 .b=-9 .c=-8 .d=-2 .e=-1") local ++fails
local s : char Satisfaction_with_public_service[src_name]
if ("`s'" != "Satisfaction_with_public_services_overall") local ++fails
local s : char q_3[src_name]
if ("`s'" != "q.3") local ++fails
local s : char sex[spss_missing]
if (`"`s'"' != `""X""') local ++fails
count if q2 == .a
if (r(N) == 0) local ++fails
if (`"`: label q2 .b'"' != "Refused") local ++fails
_v130_bridges
if (r(n) != 0) local ++fails

* ---- two-table verbs with a .sav using side, and mergein --------------------
parqit save `"`stem'.parquet"' using `"`sav'"', replace
parqit use id q1 using `"`stem'.parquet"', name(m)
parqit merge 1:1 id using `"`fix'/survey.zsav"', keepusing(income) nogenerate
parqit count
if (r(N) != 60) local ++fails
parqit append using `"`sav'"' `"`fix'/survey_bc.sav"'
parqit count
if (r(N) != 180) local ++fails
parqit close _all
_v130_bridges
if (r(n) != 0) local ++fails
clear
set obs 60
gen double id = _n
gen byte mine = 1
parqit mergein 1:1 id using `"`sav'"', keepusing(q1 city)
count if _merge == 3
if (r(N) != 60) local ++fails
count if q1 == .a | q1 == .b | q1 == .c
local nx = r(N)
if (`nx' == 0) local ++fails                    /* mergein restores .a-.z */

* ---- parqit save ... using leaves memory and the open views alone ----------
sysuse auto, clear
quietly datasignature
local before `"`r(datasignature)'"'
parqit use using `"`stem'.parquet"', name(keep)
parqit keep if q1 == 1
parqit count
local nview = r(N)
parqit save `"`stem'_2.parquet"' using `"`fix'/survey.zsav"', replace
if (r(N) != 60 | r(k) != 15) local ++fails
quietly datasignature
if (`"`r(datasignature)'"' != `"`before'"') local ++fails
parqit count
if (r(N) != `nview') local ++fails
parqit close _all

* ---- refusals: rc, message, no output, memory untouched --------------------
* a truncated copy of the file
python:
from sfi import Macro
src = Macro.getLocal("sav")
stem = Macro.getLocal("stem")
data = open(src, "rb").read()
open(stem + "_trunc.sav", "wb").write(data[: len(data) - 700])
open(stem + "_text.sav", "wb").write(b"id,name\n1,a\n" * 40)
open(stem + "_self.sav", "wb").write(data)
end
capture noisily parqit save `"`stem'_t.parquet"' using `"`stem'_trunc.sav"', replace
if (_rc != 610) local ++fails
capture confirm file `"`stem'_t.parquet"'
if (!_rc) local ++fails
log using `"`lg'"', text replace name(v130)
capture noisily parqit save `"`stem'_t.parquet"' using `"`stem'_trunc.sav"', replace
log close v130
_v130_grep `"`lg'"' "truncated"
if (!r(found)) local ++fails
capture noisily parqit save `"`stem'_x.parquet"' using `"`stem'_text.sav"'
if (_rc != 610) local ++fails
capture noisily parqit use `"`stem'_trunc.sav"', clear
if (_rc != 610) local ++fails
quietly datasignature
if (`"`r(datasignature)'"' != `"`before'"') local ++fails   /* memory kept */
_v130_bridges
if (r(n) != 0) local ++fails                    /* the failed bridge is gone */
capture noisily parqit save `"`stem'_x.parquet"' using `"`fix'/nosuchfile.sav"'
if (_rc != 601) local ++fails
capture noisily parqit save `"`stem'_self.sav"' using `"`stem'_self.sav"', replace
if (_rc != 198) local ++fails
capture noisily confirm file `"`stem'_self.sav"'
if (_rc) local ++fails
parqit save `"`stem'_3.parquet"' using `"`sav'"'
capture noisily parqit save `"`stem'_3.parquet"' using `"`sav'"'
if (_rc != 602) local ++fails
capture noisily parqit save `"`stem'_4.parquet"' using `"`fix'/../far_dates.parquet"'
if (_rc != 198) local ++fails
foreach o in data copysource xmissing "partition_by(id)" "partitions(append)" "chunk(10)" {
    capture noisily parqit save `"`stem'_5.parquet"' using `"`sav'"', `o'
    if (_rc != 198) {
        di as err "option `o' was not refused"
        local ++fails
    }
}
* ENC-3: an encoding parqit does not read (a stateful one) is refused; KOI8-R,
* refused before ENC-3, is now one of those it reads
capture noisily parqit save `"`stem'_6.parquet"' using `"`sav'"', encoding(iso-2022-jp)
if (_rc != 198) local ++fails
capture noisily parqit save `"`stem'_6.parquet"' using `"`sav'"', encoding(latin1) replace
if (_rc) local ++fails                          /* a valid override is accepted */
capture noisily parqit save `"`stem'_6.parquet"' using `"`sav'"', encoding(koi8-r) replace
if (_rc) local ++fails
log using `"`lg'"', text replace name(v130)
capture noisily parqit describe `"`sav'"'
local rc = _rc
log close v130
if (`rc' != 198) local ++fails
_v130_grep `"`lg'"' "reads Parquet footers only"
if (!r(found)) local ++fails
capture noisily parqit use using `"`sav'"', filename(src)
if (_rc != 198) local ++fails
_v130_bridges
if (r(n) != 0) local ++fails

if (`fails') di "VERDICT(V130_SPSS_PATHS): FAIL - `fails' checks"
else di "VERDICT(V130_SPSS_PATHS): PASS"
