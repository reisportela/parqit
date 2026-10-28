* DuckDB 2.0 TIMESTAMPTZ_NS must retain its UTC instant at the Stata boundary.
clear all
set more off
set varabbrev off
set linesize 244
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
global V200_fails 0
program define _v200_check
    args ok message
    if (`ok') di as txt "PASS `message'"
    else {
        di as err "FAIL `message'"
        global V200_fails = $V200_fails + 1
    }
end
tempfile source expected_csv expected saved target
python:
import csv
import pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
ns = [-9223372036854775806, -9223372036854775500,
      -315619200000000001, -315619200000000000,
      -1000001, -1000000, -999999, -1001, -1000, -999, -1,
      0, 1, 999, 1000, 999999, 1000000, 1000001,
      1609459200123456789, 9223372036854775806, None]
zones = ['UTC', 'Europe/Lisbon', 'America/New_York', 'Asia/Kathmandu']
for i, zone in enumerate(zones):
    table = pa.table({
        'timestamp': pa.array(ns, pa.timestamp('ns', tz=zone)),
        '2m_dewpoint_temperature': pa.array([j / 8 for j in range(len(ns))], pa.float32()),
        '2m_temperature': pa.array([j / 4 for j in range(len(ns))], pa.float64()),
        'latitude': pa.array([43.125 + j / 8 for j in range(len(ns))]),
        'longitude': pa.array([-10.25 + j / 16 for j in range(len(ns))])})
    pq.write_table(table, Macro.getLocal('source') + '_' + str(i) + '.parquet', row_group_size=5)
with open(Macro.getLocal('expected_csv'), 'w', newline='') as f:
    writer = csv.writer(f)
    writer.writerow(['timestamp', '_2m_dewpoint_temperature', '_2m_temperature', 'latitude', 'longitude'])
    for j, value in enumerate(ns):
        writer.writerow(['' if value is None else value // 1000000 + 315619200000,
                         j / 8, j / 4, 43.125 + j / 8, -10.25 + j / 16])
for name, values in [('null', [None] * 3), ('negative_infinity', [0, -(2**63-1), None]),
                     ('positive_infinity', [0, 2**63-1, None])]:
    pq.write_table(pa.table({'timestamp': pa.array(values, pa.timestamp('ns', tz='UTC'))}),
                   Macro.getLocal('source') + '_' + name + '.parquet')
def _v200_saved_oracle(stem):
    result = pq.read_table(stem + '.parquet')
    actual = result['timestamp'].cast(pa.int64()).to_pylist()
    expected_us = [None if value is None else (value // 1000000) * 1000 for value in ns]
    ok = (result.num_rows == len(ns) and result.num_columns == 5
          and result.schema.field('timestamp').type == pa.timestamp('us')
          and actual == expected_us)
    Macro.setLocal('oracle', str(int(ok)))
end
import delimited using "`expected_csv'", clear varnames(1) asdouble
format timestamp %tc
save "`expected'"

* Fail directly on the reported regression, before a missing column can cascade.
parqit use "`source'_0.parquet", clear
capture confirm variable timestamp
if (_rc) {
    di as err "TIMESTAMPTZ_NS timestamp was dropped"
    di as txt "VERDICT(V200_TIMESTAMPTZ_NS): FAIL"
    exit 9
}

foreach workers in 1 2 {
    parqit set fill_threads `workers'
    forvalues zone = 0/3 {
        parqit use "`source'_`zone'.parquet", clear
        capture {
            assert c(k) == 5 & _N == 21
            assert "`: type timestamp'" == "double"
            assert "`: format timestamp'" == "%tc"
            assert "`: type _2m_dewpoint_temperature'" == "float"
            assert "`: char _2m_dewpoint_temperature[src_name]'" == "2m_dewpoint_temperature"
            assert "`: char _2m_temperature[src_name]'" == "2m_temperature"
            cf _all using "`expected'"
        }
        _v200_check `=_rc == 0' "eager UTC values, names and format: zone=`zone', workers=`workers'"

        parqit use using "`source'_`zone'.parquet"
        parqit collect, clear
        capture {
            assert c(k) == 5 & _N == 21
            assert "`: format timestamp'" == "%tc"
            cf _all using "`expected'"
        }
        _v200_check `=_rc == 0' "lazy UTC values: zone=`zone', workers=`workers'"

        parqit gen double next_ms = timestamp + 1
        parqit collect, clear
        capture assert next_ms == timestamp + 1
        _v200_check `=_rc == 0' "lazy datetime arithmetic: zone=`zone', workers=`workers'"
        parqit drop next_ms
        parqit save "`saved'.parquet", replace
        python: _v200_saved_oracle(Macro.getLocal('saved'))
        _v200_check `oracle' "independent Parquet timestamp oracle: zone=`zone', workers=`workers'"
        parqit close _all
    }
}

parqit use "`source'_null.parquet", clear
capture assert c(k) == 1 & _N == 3 & missing(timestamp)
_v200_check `=_rc == 0' "all-null timestamp remains a typed datetime column"

* Infinite instants remain errors, with the current dataset and destination intact.
clear
set obs 1
gen byte sentinel = 7
parqit save "`target'.parquet"
python:
from pathlib import Path
import hashlib
target = Path(Macro.getLocal('target') + '.parquet')
target_before = hashlib.sha256(target.read_bytes()).hexdigest()
end
foreach infinity in negative_infinity positive_infinity {
    capture noisily parqit use "`source'_`infinity'.parquet", clear
    local rc = _rc
    capture assert `rc' == 920 & c(k) == 1 & _N == 1 & sentinel[1] == 7
    _v200_check `=_rc == 0' "`infinity' refuses eager atomically"
    parqit use using "`source'_`infinity'.parquet"
    capture noisily parqit collect, clear
    local rc = _rc
    capture assert `rc' == 920 & c(k) == 1 & _N == 1 & sentinel[1] == 7
    _v200_check `=_rc == 0' "`infinity' refuses lazy atomically"
    capture noisily parqit save "`target'.parquet", replace
    _v200_check `=_rc == 920' "`infinity' refuses save"
    parqit close _all
}
python:
Macro.setLocal('preserved', str(int(hashlib.sha256(target.read_bytes()).hexdigest() == target_before)))
end
_v200_check `preserved' "failed saves preserve destination bytes"
parqit set fill_threads auto
if ($V200_fails) {
    di as txt "VERDICT(V200_TIMESTAMPTZ_NS): FAIL"
    exit 9
}
di as txt "VERDICT(V200_TIMESTAMPTZ_NS): PASS"
