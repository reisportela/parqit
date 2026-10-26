* V155 — DuckDB 1.5.6 (DUCKDB-156, Codex round 5): the literal and predicate
*   forms whose engine handling changed between 1.5.3 and 1.5.6, each against
*   native Stata on the same rows: integer literals beyond HUGEINT's 39
*   digits and with signs, leading zeros and exponents, in gen, in keep if
*   and through parqit sql; DATE and TIMESTAMP comparisons with fractional
*   literals, missing values and both operand orders, as a filter (the
*   sample selected) and as a value; the same instants through the eager
*   read, collect and save (the written file read back by pyarrow).
* Oracles: native Stata; pyarrow; Python integers.
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile stem nat
parqit set statamissing off

* ---------- integer literals beyond 39 digits, signed, with leading zeros ---
sysuse auto, clear
keep make price
gen double b40 = 1234567890123456789012345678901234567890 + price
gen double b45 = price - 123456789012345678901234567890123456789012345
gen double bneg = -99999999999999999999999999999999999999999 + price
gen double bz = 000123456789012345678901234567890 + price
gen double be = 1e40 + price
gen byte f40 = price < 1234567890123456789012345678901234567890
gen byte fneg = price > -99999999999999999999999999999999999999999
save `"`nat'"', replace
parqit save `"`stem'_auto.parquet"', data replace
parqit use make price using `"`stem'_auto.parquet"'
parqit gen b40 = 1234567890123456789012345678901234567890 + price
parqit gen b45 = price - 123456789012345678901234567890123456789012345
parqit gen bneg = -99999999999999999999999999999999999999999 + price
parqit gen bz = 000123456789012345678901234567890 + price
parqit gen be = 1e40 + price
parqit gen byte f40 = price < 1234567890123456789012345678901234567890
parqit gen byte fneg = price > -99999999999999999999999999999999999999999
parqit collect, clear
cf _all using `"`nat'"', verbose
* the same literals in a filter select every row, as natively
parqit use make price using `"`stem'_auto.parquet"'
parqit keep if price < 1234567890123456789012345678901234567890 & price > -99999999999999999999999999999999999999999
parqit count
assert r(N) == 74
parqit close _all
* through parqit sql, the engine's own literal: a 40-digit integer does not
* fit HUGEINT; the engine must either refuse it or carry the exact double
* (1.5.6 fixed a silent truncation), never a truncated integer
capture noisily parqit sql "SELECT 1234567890123456789012345678901234567890 AS h"
local sqlrc = _rc
if `sqlrc' == 0 {
    capture noisily parqit collect, clear
    local sqlrc = _rc
}
parqit close _all
if (`sqlrc' == 0) {
    assert _N == 1
    assert h[1] == 1234567890123456789012345678901234567890
}

* ---------- DATE and TIMESTAMP predicates: fractions, missing, both orders --
local f `"`stem'_dt.parquet"'
python:
from sfi import Macro
import pyarrow as pa, pyarrow.parquet as pq, datetime as dt
d = [dt.date(2020, 2, 29), dt.date(2020, 3, 1), None, dt.date(1959, 12, 31), dt.date(1960, 1, 1)]
ts = [dt.datetime(2020, 2, 29, 23, 59, 59, 999000), dt.datetime(2020, 3, 1, 0, 0, 0, 0), None,
      dt.datetime(1959, 12, 31, 23, 59, 59, 500000), dt.datetime(1960, 1, 1, 0, 0, 0, 1000)]
t = pa.table({"i": pa.array([1, 2, 3, 4, 5], pa.int32()), "d": pa.array(d, pa.date32()),
              "ts": pa.array(ts, pa.timestamp("us"))})
pq.write_table(t, Macro.getLocal("f"))
end
parqit use using `"`f'"', clear
sort i
assert "`: format d'" == "%td" & "`: format ts'" == "%tc"
parqit set statamissing on
gen byte p1 = d > td(29feb2020)
gen byte p2 = td(29feb2020) < d
gen byte p3 = ts >= tc(29feb2020 23:59:59.999)
gen byte p4 = tc(01mar2020 00:00:00) > ts
gen byte p5 = ts > tc(31dec1959 23:59:59.5)
gen byte p6 = d == td(01jan1960) & ts == tc(01jan1960 00:00:00.001)
save `"`nat'"', replace
parqit use using `"`f'"'
parqit gen byte p1 = d > td(29feb2020)
parqit gen byte p2 = td(29feb2020) < d
parqit gen byte p3 = ts >= tc(29feb2020 23:59:59.999)
parqit gen byte p4 = tc(01mar2020 00:00:00) > ts
parqit gen byte p5 = ts > tc(31dec1959 23:59:59.5)
parqit gen byte p6 = d == td(01jan1960) & ts == tc(01jan1960 00:00:00.001)
parqit collect, clear
sort i
cf _all using `"`nat'"', verbose
* the sample selected by the same predicates: in Stata mode the missing
* instant is larger than every instant and differs from every date (rows
* 1, 2 and 3), in SQL mode it compares to nothing (rows 1 and 2)
parqit use using `"`f'"'
parqit keep if ts >= tc(29feb2020 23:59:59.999) & d != td(01jan2021)
parqit count
assert r(N) == 3
parqit close _all
parqit set statamissing off
parqit use using `"`f'"'
parqit keep if ts >= tc(29feb2020 23:59:59.999) & d != td(01jan2021)
parqit count
assert r(N) == 2
parqit close _all

* ---------- the same instants through eager read, collect and save -------
parqit use using `"`f'"', clear
sort i
assert ts[1] == tc(29feb2020 23:59:59.999) & ts[4] == tc(31dec1959 23:59:59.5) & ts[5] == tc(01jan1960 00:00:00.001)
assert d[1] == td(29feb2020) & d[4] == td(31dec1959) & missing(d[3]) & missing(ts[3])
local out `"`stem'_dt_out.parquet"'
parqit save `"`out'"', data replace
parqit use using `"`f'"'
parqit keep if i != 3
parqit save `"`stem'_dt_view.parquet"', view replace
local outv `"`stem'_dt_view.parquet"'
python:
from sfi import Macro
import pyarrow.parquet as pq, datetime as dt
a = pq.read_table(Macro.getLocal("out")).to_pydict()
assert a["ts"][0] == dt.datetime(2020, 2, 29, 23, 59, 59, 999000) and a["ts"][3] == dt.datetime(1959, 12, 31, 23, 59, 59, 500000)
assert a["ts"][4] == dt.datetime(1960, 1, 1, 0, 0, 0, 1000) and a["ts"][2] is None and a["d"][2] is None
assert a["d"][0] == dt.date(2020, 2, 29) and a["d"][3] == dt.date(1959, 12, 31)
b = pq.read_table(Macro.getLocal("outv")).to_pydict()
assert len(b["i"]) == 4 and b["ts"][0] == dt.datetime(2020, 2, 29, 23, 59, 59, 999000)
end
parqit close _all

di "VERDICT(V155_LITERALS_AND_PREDICATES): PASS - integer literals beyond 39 digits (signed, zero-led, exponent) equal native in gen and keep if and are refused or exact through parqit sql; DATE and TIMESTAMP predicates with fractions, missing values and both operand orders equal native as values and as the selected sample in both missing modes; the instants survive eager read, collect and save, read back by pyarrow"
