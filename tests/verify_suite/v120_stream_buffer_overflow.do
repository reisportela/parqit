* Invalid environment caps must not overflow into a request for the default.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile source
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

set obs 100
gen long x = _n
parqit save `"`source'.parquet"', data
parqit set stream_buffer_mb auto
parqit use `"`source'.parquet"', clear
buffer_value
local expected `"`r(value)'"'

foreach invalid in 18446744073710 9223372036854775807 9223372036855 999999999999999999999999999 -1 invalid {
    python: os.environ["PARQIT_STREAM_BUFFER_MB"] = "`invalid'"
    parqit use `"`source'.parquet"', clear
    assert _N == 100 & x == _n
    buffer_value
    assert `"`r(value)'"' == `"`expected'"'
}
python: os.environ.pop("PARQIT_STREAM_BUFFER_MB", None)
di "VERDICT(V120_STREAM_BUFFER_OVERFLOW): PASS"
