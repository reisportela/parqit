* Run profile_parqit on the large parquet dataset
clear all
set more off
set varabbrev off

local repo : env PARQIT_REPO
if (`"`repo'"' == "") local repo `"`c(pwd)'"'
local plugin `"`repo'/build/dev/parqit.plugin"'
local parquet : env PARQIT_BENCH_REF
if (`"`parquet'"' == "") local parquet "parqit_benchmark_ref.parquet"

adopath ++ `"`repo'/benchmarks"'
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

di as txt "=== running profile_parqit use ..., clear ==="
timer clear 1
timer on 1
profile_parqit use using `"`parquet'"', clear
timer off 1
quietly timer list 1
di as txt "Total profile_parqit use ..., clear took " as res %8.3f r(t1) " sec"

clear
capture profile_parqit close _all
