* CA-01: zero restores the engine default after numeric/auto/environment caps.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile source
local fails 0
python:
import os
os.environ.pop("PARQIT_STREAM_BUFFER_MB", None)
os.environ.pop("PARQIT_FETCH_MATERIALIZED", None)
end

program define buffer_value, rclass
    parqit sql "SELECT current_setting('streaming_buffer_size') AS buffer"
    parqit levelsof buffer
    local value `"`r(levels)'"'
    parqit close
    return local value `"`value'"'
end
buffer_value
local initial `"`r(value)'"'
set obs 100
gen long x = _n
parqit save `"`source'.parquet"', data

foreach mode in 8 auto {
    parqit set stream_buffer_mb `mode'
    parqit use `"`source'.parquet"', clear
    parqit set stream_buffer_mb 0
    parqit use `"`source'.parquet"', clear
    assert _N == 100 & x == _n
    buffer_value
    di `"after `mode' -> 0: `r(value)'; default: `initial'"'
    if (`"`r(value)'"' != `"`initial'"') local ++fails
}
python: os.environ["PARQIT_STREAM_BUFFER_MB"] = "8"
parqit set stream_buffer_mb auto
parqit use `"`source'.parquet"', clear
parqit set stream_buffer_mb 0
parqit use `"`source'.parquet"', clear
buffer_value
if (`"`r(value)'"' != `"`initial'"') local ++fails
python: os.environ["PARQIT_STREAM_BUFFER_MB"] = "0"
parqit set stream_buffer_mb 8
parqit use `"`source'.parquet"', clear
parqit set stream_buffer_mb auto
parqit use `"`source'.parquet"', clear
buffer_value
if (`"`r(value)'"' != `"`initial'"') local ++fails
if (`fails') di "VERDICT(V110_STREAM_BUFFER_RESET): FAIL - `fails' transitions"
else di "VERDICT(V110_STREAM_BUFFER_RESET): PASS"
