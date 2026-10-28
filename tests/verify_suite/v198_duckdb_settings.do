* Native settings remain live, explicit and separate from the Stata dataset.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
parqit set threads 2
set obs 7
gen long sentinel = _n
label variable sentinel "Unrelated dataset"

quietly parqit set duckdb threads 1
assert "`r(canonical)'"=="threads" & "`r(value)'"=="1"
quietly parqit set duckdb
assert r(N)>100
assert strpos(" `r(settings)' "," read_ahead_depth ")>0
quietly parqit set duckdb temp_directory
local original_temp `"`r(value)'"'
quietly parqit set duckdb temp_directory ""
assert r(is_null)==0 & `"`r(value)'"'==""
quietly parqit set duckdb temp_directory `"`original_temp'"'
assert `"`r(value)'"'==`"`original_temp'"'

local quoted_temp `"`original_temp'/synthetic O'Brien UTF-8 é"'
quietly parqit set duckdb temp_directory `"`quoted_temp'"'
assert `"`r(value)'"'==`"`quoted_temp'"'
quietly parqit set duckdb temp_directory "NULL"
assert r(is_null)==0 & "`r(value)'"=="NULL"
quietly parqit set duckdb temp_directory `"`original_temp'"'
capture noisily parqit set duckdb custom_user_agent "synthetic"
assert _rc==198
quietly parqit set duckdb operator_memory_limit NULL
assert r(is_null)==1 & "`r(scope)'"=="LOCAL"
quietly parqit set duckdb streaming_buffer_size "1234567B"
assert "`r(canonical)'"=="max_streaming_buffer_size" & r(overridden)==1

parqit open _data, name(settings_target)
quietly parqit set duckdb async_threads 3
quietly parqit set threads 1
quietly parqit set duckdb async_threads
assert "`r(value)'"=="3"
quietly parqit set duckdb worker_threads 2
assert "`r(canonical)'"=="threads" & "`r(value)'"=="2"
quietly parqit set duckdb memory_limit "64MB"
assert "`r(canonical)'"=="max_memory"
quietly parqit set duckdb read_ahead_depth 0
quietly parqit set duckdb cache_local_files true
quietly parqit set duckdb enable_external_file_cache true
quietly parqit set duckdb preserve_insertion_order false
parqit view settings_target: count
assert r(N)==7
quietly parqit set duckdb preserve_insertion_order true

capture noisily parqit set duckdb threads -1
assert _rc==198
quietly parqit set duckdb threads
assert "`r(value)'"=="2"
capture noisily parqit set duckdb standard_vector_size 4096
assert _rc==198
quietly parqit set duckdb standard_vector_size
assert "`r(value)'"=="2048"
capture noisily parqit set duckdb pandas_analyze_sample 1000
assert _rc==198
capture noisily parqit set duckdb threads "2; DROP TABLE anything"
assert _rc==198
quietly parqit set duckdb threads
assert "`r(value)'"=="2"

quietly parqit set duckdb disabled_optimizers "join_order"
parqit view settings_target: count
assert r(N)==7
quietly parqit set duckdb disabled_optimizers
assert strpos("`r(value)'","join_order")>0
quietly parqit set duckdb reset disabled_optimizers
quietly parqit set duckdb reset streaming_buffer_size
assert r(overridden)==1
quietly parqit set stream_buffer_mb auto
quietly parqit set duckdb streaming_buffer_size
assert r(overridden)==0

* RESET must remain available even when a setting restricts SQL introspection.
quietly parqit set duckdb max_expression_depth 1
assert "`r(value)'"=="1"
quietly parqit set duckdb reset max_expression_depth
assert real("`r(value)'")>1

assert _N==7 & sentinel==_n
assert "`: variable label sentinel'"=="Unrelated dataset"
parqit view settings_target: count
assert r(N)==7
display "VERDICT(V198_DUCKDB_SETTINGS): PASS"
