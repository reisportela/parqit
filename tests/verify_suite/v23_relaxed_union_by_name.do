* V23 — `parqit use … , relaxed` reads a heterogeneous-schema file set by union of
* column names (missing columns -> Stata missing), mirroring pq's `relaxed`.
* Without the option a schema mismatch across a glob is loud (never silent).
* Independent oracle: duckdb writes two files with different schemas; the union
* (a,b,c) with the exact NULL pattern is checked against a hand-computed truth.
clear all
set more off
set varabbrev off

args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

tempfile d
local dir "`d'_relaxed"
shell mkdir -p "`dir'"
shell duckdb -c "COPY (SELECT 1 AS a, 2 AS b)            TO '`dir'/f1.parquet' (FORMAT PARQUET)"
shell duckdb -c "COPY (SELECT 3 AS a, 9 AS c)            TO '`dir'/f2.parquet' (FORMAT PARQUET)"

local nfail = 0

* --- 1) without relaxed: heterogeneous glob must be a LOUD error (never silent) ---
capture noisily parqit use using "`dir'/f*.parquet", clear
if (_rc == 0) {
    di as error "  FAIL: heterogeneous glob loaded WITHOUT relaxed (should error)"
    local ++nfail
}
else di as result "  PASS: heterogeneous glob without relaxed errors loudly (rc=`=_rc')"

* --- 2) with relaxed: union by name, missing columns -> Stata missing ---
capture noisily parqit use using "`dir'/f*.parquet", clear relaxed
if (_rc) {
    di as error "  FAIL: relaxed read errored (rc=`=_rc')"
    local ++nfail
}
else {
    sort a
    local ok = 1
    if (_N != 2) local ok = 0
    capture confirm variable a
    if (_rc) local ok = 0
    capture confirm variable b
    if (_rc) local ok = 0
    capture confirm variable c
    if (_rc) local ok = 0
    if (`ok') {
        * oracle: a={1,3}; b={2,.}; c={.,9}
        if (a[1]!=1 | a[2]!=3)        local ok = 0
        if (b[1]!=2 | !missing(b[2])) local ok = 0
        if (!missing(c[1]) | c[2]!=9) local ok = 0
    }
    if (`ok') di as result "  PASS: relaxed unions by name with exact NULL pattern (a,b,c)"
    else {
        di as error "  FAIL: relaxed union content wrong"
        local ++nfail
    }
}

* --- 3) a homogeneous glob is unaffected by relaxed (still works) ---
shell duckdb -c "COPY (SELECT 5 AS a, 6 AS b) TO '`dir'/gh1.parquet' (FORMAT PARQUET)"
shell duckdb -c "COPY (SELECT 7 AS a, 8 AS b) TO '`dir'/gh2.parquet' (FORMAT PARQUET)"
capture noisily parqit use using "`dir'/gh*.parquet", clear relaxed
if (_rc==0 & _N==2) di as result "  PASS: relaxed on a homogeneous glob still reads normally"
else {
    di as error "  FAIL: relaxed broke a homogeneous read (rc=`=_rc' N=`=_N')"
    local ++nfail
}

if (`nfail' == 0) di as result _n "VERDICT(v23_relaxed_union_by_name): PASS"
else              di as error  _n "VERDICT(v23_relaxed_union_by_name): FAIL (`nfail')"
