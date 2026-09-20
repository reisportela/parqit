* CA-05: discard refreshes ado programs; an already registered plugin persists.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
parqit set fill_threads 1
parqit set stream_buffer_mb 8
parqit set threads 1
parqit sql "SELECT 42 AS x"
discard
parqit version
assert `"`r(fill_threads)'"' == "1" & `"`r(stream_buffer_mb)'"' == "8" & r(threads) == 1
parqit count
assert r(N) == 1
parqit collect, clear
assert x == 42
parqit close _all
parqit version
assert `"`r(fill_threads)'"' == "1" & `"`r(stream_buffer_mb)'"' == "8" & r(threads) == 1
di "VERDICT(V114_DISCARD_PLUGIN_PERSISTENCE): PASS"
