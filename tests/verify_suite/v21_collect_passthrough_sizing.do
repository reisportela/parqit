* V21 — collect-passthrough column sizing must equal direct `parqit use`.
*
* Change (A) lets a pure full-file passthrough collect size its columns from
* Parquet row-group statistics (the F2 metadata path), instead of a redundant
* full scan. Invariant: `parqit use F, clear` and `parqit use F` + `parqit collect,
* clear` must produce a BYTE-IDENTICAL dataset — same variables, same storage
* types, same display formats, same values. If the metadata-sized plan ever
* diverged from the scan-sized plan (e.g. a date column mis-sized), this fails.
*
* Files span the type spectrum on purpose:
*   workers_perf  int64/int32/double+nulls/string/DATE32   (date must fall back)
*   messy_perf    uint32/decimal/DUP-NAMES/all-null        (dup disables F2)
*   reference     all-numeric int8..double                 (F2 fully engages)
clear all
set more off
set varabbrev off

args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

* Self-contained small fixtures (generated via the duckdb CLI, an independent
* writer) so the test is a real regression guard on any dev machine, not only
* where the big benchmark/reference files exist. Two shapes:
*   gen_numeric : all-numeric -> F2 footer sizing fully engages on collect
*   gen_mixed   : int/double/string/DATE -> strings scan, date hits the floor
tempfile fnum fmix
local fnum_pq "`fnum'.parquet"
local fmix_pq "`fmix'.parquet"
shell duckdb -c "COPY (SELECT i AS id, (i*7)%32000 AS small, (i*131)::BIGINT AS big, i::DOUBLE/3 AS x, (i%2=0)::INTEGER*1.5 AS f FROM range(1,300000) t(i)) TO '`fnum_pq'' (FORMAT PARQUET)"
shell duckdb -c "COPY (SELECT i AS id, i::DOUBLE/7 AS w, 'g'||(i%4)::VARCHAR AS grp, (DATE '1985-01-01' + (i%40000)::INTEGER) AS d FROM range(1,200000) t(i)) TO '`fmix_pq'' (FORMAT PARQUET)"

local D "`repo'/benchmarks/_out/synthetic_medium_data"

local file1 "`fnum_pq'"
local lab1  "gen_numeric"
local file2 "`fmix_pq'"
local lab2  "gen_mixed"
local file3 "`D'/workers_perf.parquet"
local lab3  "workers_perf"
local file4 "`D'/messy_perf.parquet"
local lab4  "messy_perf"
local nfiles 4

* fingerprint of the current dataset: per-variable name:type:format, then signature
capture program drop _fp
program define _fp, rclass
    local fp ""
    foreach v of varlist _all {
        local fp `"`fp' `v':`:type `v'':`:format `v''"'
    }
    quietly datasignature
    return local fp `"`fp'"'
    return local sig "`r(datasignature)'"
end

local nfail = 0
forvalues i = 1/`nfiles' {
    local f   "`file`i''"
    local lab "`lab`i''"

    capture confirm file `"`f'"'
    if (_rc) {
        di as txt _n "==== `lab' : SKIP (not found: `f') ===="
        continue
    }

    di as txt _n "==== `lab' : `f' ===="

    * A) direct use, clear
    clear
    capture parqit close _all
    quietly parqit use using `"`f'"', clear
    _fp
    local fpA `"`r(fp)'"'
    local sigA "`r(sig)'"
    local kA = c(k)
    local nA = _N

    * B) view + collect, clear
    clear
    capture parqit close _all
    quietly parqit use using `"`f'"'
    quietly parqit collect, clear
    _fp
    local fpB `"`r(fp)'"'
    local sigB "`r(sig)'"
    local kB = c(k)
    local nB = _N

    capture parqit close _all

    local ok = 1
    if (`kA' != `kB' | `nA' != `nB') local ok = 0
    if (`"`fpA'"' != `"`fpB'"') local ok = 0
    if ("`sigA'" != "`sigB'") local ok = 0

    if (`ok') {
        di as result "  PASS `lab': use==collect  (k=`kA' n=`nA' sig=`sigA')"
    }
    else {
        local ++nfail
        di as error  "  FAIL `lab': use != collect"
        di as error  "    k:   use=`kA' collect=`kB'   n: use=`nA' collect=`nB'"
        di as error  "    sig: use=`sigA' collect=`sigB'"
        if (`"`fpA'"' != `"`fpB'"') {
            di as error "    types/formats differ:"
            di as error "      use:     `fpA'"
            di as error "      collect: `fpB'"
        }
    }
}

if (`nfail' == 0) di as result _n "VERDICT(v21_collect_passthrough_sizing): PASS"
else              di as error  _n "VERDICT(v21_collect_passthrough_sizing): FAIL (`nfail')"
