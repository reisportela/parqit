* V31 — query-state safety plus Stata-faithful string()/substr() parity.
* Covers the 2026-06-16 fixes:
*   QUERY-1  a failed parqit query validates on a copy and preserves the view
*   STR-3    string()/strofreal() default formatting follows Stata %9.0g
*   STR-4    substr() indexes bytes; valid UTF-8 slices are exact, split
*            codepoints become U+FFFD because DuckDB/Arrow VARCHAR must be UTF-8
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

local fails 0
tempfile t

* ---- STR-3 : string() exact parity against native Stata ------------------
clear
set obs 31
gen long id = _n
gen double x = .
replace x = 1e100 in 1
replace x = -1e100 in 2
replace x = .00009999999 in 3
replace x = .000123456 in 4
replace x = 123456789 in 5
replace x = 1/3 in 6
replace x = 123456.789 in 7
replace x = 9999999.9 in 8
replace x = .0000123456 in 9
replace x = .000009999999 in 10
replace x = 0 in 11
replace x = 1 in 12
replace x = -1 in 13
replace x = 42 in 14
replace x = -0.03 in 15
replace x = .9999999 in 16
replace x = 9.999999 in 17
replace x = 99.99999 in 18
replace x = 999.9999 in 19
replace x = 9999.999 in 20
replace x = 99999.99 in 21
replace x = 999999.9 in 22
replace x = 1000000 in 23
replace x = 9999999 in 24
replace x = 9999999.4 in 25
replace x = 9999999.5 in 26
replace x = 10000000 in 27
replace x = .00001 in 28
replace x = .0001 in 29
replace x = -.0001 in 30
replace x = . in 31
gen str40 native = string(x)
parqit save `"`t'_str.parquet"', replace data
parqit use using `"`t'_str.parquet"'
parqit gen sx = string(x)
parqit gen sy = strofreal(x)
parqit collect, clear
capture assert sx == native & sy == native
if (_rc) {
    di as err "FAIL STR-3: string()/strofreal() mismatch vs native Stata"
    list id x native sx sy if sx != native | sy != native, noobs
}
local fails = `fails' + (_rc!=0)

* ---- STR-4 : substr() byte indexing and UTF-8 boundary handling ----------
clear
set obs 2
gen long id = _n
gen str20 s = cond(_n==1, "éx", "abc")
gen str20 native_good = substr(s, 1, 2)
gen str20 native_neg = substr(s, -1, 1)
parqit save `"`t'_sub.parquet"', replace data
parqit use using `"`t'_sub.parquet"'
parqit gen good = substr(s, 1, 2)
parqit gen neg = substr(s, -1, 1)
parqit gen toend = substr(s, 3, .)
parqit gen missp = substr(s, ., 1)
parqit gen zerop = substr(s, 0, 1)
parqit gen bad1 = substr(s, 1, 1)
parqit gen bad2 = substr(s, 2, 1)
parqit collect, clear
capture assert good == native_good & neg == native_neg
if (_rc) di as err "FAIL STR-4(valid): byte substr valid slices differ from native"
local fails = `fails' + (_rc!=0)
capture assert toend[1]=="x" & toend[2]=="c" & missp[1]=="" & missp[2]=="" & zerop[1]=="" & zerop[2]==""
if (_rc) di as err "FAIL STR-4(edge): missing/zero/length-to-end semantics wrong"
local fails = `fails' + (_rc!=0)
capture assert strlen(bad1[1])==3 & ustrlen(bad1[1])==1 & strlen(bad2[1])==3 & ustrlen(bad2[1])==1
if (_rc) di as err "FAIL STR-4(split): split UTF-8 byte slices should be one replacement character, not blank/fail"
local fails = `fails' + (_rc!=0)

* ---- QUERY-1 : failed raw query must not discard the existing view --------
clear
input long id double x
1 10
2 20
3 30
end
parqit save `"`t'_q.parquet"', replace data
parqit use using `"`t'_q.parquet"'
capture noisily parqit query "where nosuch_column = 1"
local badrc = _rc
capture noisily parqit count
local countrc = _rc
local n = r(N)
capture assert `badrc' != 0 & `countrc' == 0 & `n' == 3
if (_rc) di as err "FAIL QUERY-1(open): failed parqit query closed or mutated the plain view"
local fails = `fails' + (_rc!=0)

parqit keep if x >= 20
capture noisily parqit query "where nosuch_column = 1"
local badrc2 = _rc
capture noisily parqit collect, clear
local colrc = _rc
capture assert `badrc2' != 0 & `colrc' == 0 & _N == 2 & id[1] == 2 & id[2] == 3
if (_rc) di as err "FAIL QUERY-1(pipeline): failed parqit query did not preserve prior transforms"
local fails = `fails' + (_rc!=0)

di as txt "VERDICT(V31_QUERY_STRING_SUBSTR_PARITY): " cond(`fails'==0, "PASS", "FAIL - `fails' failures") ///
    " - query state / string %9.0g / byte substr"
if (`fails') exit 9
