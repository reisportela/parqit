* Regressions for the 2026-06-12 independent audit (PARQIT_AUDIT_REPORT.md):
*  01 named _data views must keep independent backing data
*  02 reshape long must reject duplicate i() (Stata's contract)
*  03 misstable r(n_complete) is the complete-observation count
*  04 tabulate, row col prints percentage panels
*  05 save chunk() reaches the engine as the row-group size
*  06 an uncaught C++ exception returns rc, never kills Stata
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

local fails 0

* ---------- 01: two promoted views keep their own data --------------------
clear
set obs 1
gen double x = 1
parqit open _data, name(first)
clear
set obs 1
gen double x = 2
parqit open _data, name(second)
parqit view first
parqit collect, clear
if (_N != 1 | x[1] != 1) {
    di as err "FAIL 01: first view returned the second dataset"
    local ++fails
}
* re-collect must still work (backing file not consumed/erased)
parqit collect, clear
if (_N != 1 | x[1] != 1) {
    di as err "FAIL 01b: re-collect of first view broke"
    local ++fails
}
* closing must erase the owned bridge files
local tdir "`c(tmpdir)'"
parqit close _all
local leftover : dir "`tdir'" files "_parqit_opendata_`c(pid)'_*.parquet"
if (`"`leftover'"' != "") {
    di as err `"FAIL 01c: bridge files not cleaned on close: `leftover'"'
    local ++fails
}

* ---------- 02: reshape long rejects duplicate i() ------------------------
clear
input long id double(inc2019 inc2020)
1 10 11
1 20 21
2 30 31
end
parqit open _data
capture parqit reshape long inc, i(id) j(year)
if (_rc == 0) {
    di as err "FAIL 02: duplicate i() accepted by reshape long"
    local ++fails
}
parqit close _all
* unique i() must still work and match native
clear
input long id double(inc2019 inc2020)
1 10 11
2 30 31
end
tempfile w
qui save `"`w'"'
parqit open _data
parqit reshape long inc, i(id) j(year)
parqit sort id year
parqit collect, clear
tempfile got
qui save `"`got'"'
use `"`w'"', clear
qui reshape long inc, i(id) j(year)
sort id year
cf _all using `"`got'"'
parqit close _all

* ---------- 03: r(n_complete) is a row-wise count -------------------------
clear
input double(a b)
1 10
. 20
3  .
4 40
end
egen byte miss = rowmiss(a b)
qui count if miss == 0
local o_complete = r(N)
drop miss
parqit open _data
parqit misstable summarize
if (r(n_complete) != `o_complete') {
    di as err "FAIL 03: r(n_complete)=" r(n_complete) " native=`o_complete'"
    local ++fails
}
parqit close _all

* ---------- 04: tabulate, row col prints percentage panels ----------------
clear
input str1 g int y
"a" 1
"a" 1
"a" 2
"b" 2
end
parqit open _data
tempname lg
local plog "`c(tmpdir)'/_parqit_t10_tab2.log"
capture erase `"`plog'"'
log using `"`plog'"', text name(`lg')
parqit tabulate g y, row col
log close `lg'
mata: st_local("tabtxt", invtokens(cat(st_local("plog"))', char(10)))
* row a: 2 of 3 -> 66.67%; col y=1: 2 of 2 -> 100.00%
if (strpos(`"`tabtxt'"', "66.67%") == 0 | strpos(`"`tabtxt'"', "100.00%") == 0) {
    di as err "FAIL 04: percentage panels missing from tabulate, row col"
    local ++fails
}
capture erase `"`plog'"'
parqit close _all

* ---------- 05: chunk() controls parquet row groups ------------------------
clear
set obs 10000
gen long x = _n
local chunked "`c(tmpdir)'/_parqit_t10_chunked.parquet"
parqit save `"`chunked'"', replace chunk(2048)
* oracle: pyarrow reads the row-group structure back
local pyout "`c(tmpdir)'/_parqit_t10_pyout.txt"
shell python3 -c "import sys, pyarrow.parquet as pq; print(pq.ParquetFile(sys.argv[1]).metadata.num_row_groups)" "`chunked'" > "`pyout'"
file open fh using `"`pyout'"', read text
file read fh ngroups
file close fh
if (trim("`ngroups'") != "5") {
    di as err "FAIL 05: chunk(2048) over 10000 rows gave `ngroups' row groups (want 5)"
    local ++fails
}
capture erase `"`chunked'"'
* invalid chunk must be loud
clear
set obs 5
gen x = 1
capture parqit save `"`chunked'"', replace chunk(0)
if (_rc == 0) {
    di as err "FAIL 05b: chunk(0) accepted"
    local ++fails
}

* ---------- 06: exception boundary — loud rc, Stata survives ---------------
capture plugin call parqit_plugin, selftest throw
if (_rc == 0) {
    di as err "FAIL 06: deliberate exception returned rc 0"
    local ++fails
}
capture parqit version
if (_rc != 0 | `"`r(parqit_version)'"' == "") {
    di as err "FAIL 06b: plugin dead after caught exception"
    local ++fails
}

* ---------------------------------------------------------------------------
if (`fails' == 0) di "VERDICT(T10_AUDIT_FIXES): PASS - audit S0/S2/S3 regressions hold"
else {
    di as err "VERDICT(T10_AUDIT_FIXES): FAIL - `fails' check(s) failed"
    exit 9
}
