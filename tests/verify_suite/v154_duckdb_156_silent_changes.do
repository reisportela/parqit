* V154 — DuckDB 1.5.6 (DUCKDB-156, 2026-10-13): the engine changes that could
*   alter a result or a message without a parqit change, each against an
*   independent oracle: MAX/MIN of a hive partition column after the view
*   pruned partitions (1.5.6 "Fix MAX on hive partition column after file
*   pruning"), a Parquet TIME_NS column read as a clock (1.5.6 "Fix TIME_NS
*   read/write"), DATE comparisons in both directions with a date literal
*   (1.5.6 "avoid per-row DATE->TIMESTAMP casts in comparisons"), a very long
*   integer literal in an expression (1.5.6 "silent truncation of very long
*   integer literals into HUGEINT": parqit sends canonical binary64 literals,
*   so the value is Stata's), an append of two filtered views sharing one
*   merge (1.5.6 common_subplan with UNION ALL arms sharing a join subtree),
*   and parqit's own messages for an unknown variable in a quoted-identifier
*   world (1.5.6 "Always quote identifiers in error messages").
* Oracles: pyarrow for the files written and read; native Stata for the
*   comparisons, the literal, the append and the merge.
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile stem nat
parqit set statamissing off

* ---------- hive partitions: MAX/MIN of the key after pruning ---------------
clear
set obs 3000
gen int year = 2018 + mod(_n, 5)
gen long id = _n
gen double x = _n / 7
parqit save `"`stem'_tree"', data replace partition_by(year)
parqit use using `"`stem'_tree"'
parqit keep if year < 2021
parqit summarize year
assert r(max) == 2020 & r(min) == 2018 & r(N) == 1800
parqit collapse (max) mx=year (min) mn=year (count) n=id
parqit collect, clear
assert mx[1] == 2020 & mn[1] == 2018 & n[1] == 1800
parqit use using `"`stem'_tree"'
parqit keep if year > 2019
parqit collapse (max) mx=year (min) mn=year (max) mid=id, by(year)
parqit collect, clear
sort year
assert _N == 3 & mn[1] == 2020 & mx[3] == 2022
local treedir `"`stem'_tree"'
python:
from sfi import Macro
import pyarrow.dataset as ds, pyarrow.compute as pc
t = ds.dataset(Macro.getLocal("treedir"), format="parquet", partitioning="hive").to_table()
f = t.filter(pc.field("year") > 2019)
assert pc.max(f.column("year")).as_py() == 2022 and pc.min(f.column("year")).as_py() == 2020 and f.num_rows == 1800
end
parqit close _all

* ---------- TIME_NS in Parquet: a clock within the day ---------------------
local tns `"`stem'_tns.parquet"'
python:
from sfi import Macro
import pyarrow as pa, pyarrow.parquet as pq
t = pa.table({"i": pa.array([1, 2, 3, 4, 5, 6, 7], pa.int32()),
              "t": pa.array([0, 37845678901234, 86399999000000, None, 86399999999999, 999999, 37845678999999], pa.time64("ns"))})
pq.write_table(t, Macro.getLocal("tns"))
end
parqit use using `"`tns'"', clear
sort i
* parqit's contract for a time of day: milliseconds since midnight, the
* nanoseconds truncated with a note (10:30:45.678901234 is 37845678,
* 23:59:59.999 is 86399999), including the last 500 ns before a millisecond
* boundary, which the engine's TIME_NS to TIME cast would round up
* (TIME-NS-FLOOR-1: 23:59:59.999999999 is 86399999, not 24:00:00;
* 00:00:00.000999999 is 0; 10:30:45.678999999 is 37845678)
describe t
assert t[1] == 0
assert t[2] == 37845678
assert t[3] == 86399999
assert missing(t[4])
assert t[5] == 86399999
assert t[6] == 0
assert t[7] == 37845678

* ---------- DATE columns compared with a date literal, both ways -----------
local dd `"`stem'_dates.parquet"'
python:
from sfi import Macro
import pyarrow as pa, pyarrow.parquet as pq, datetime as dt
days = [dt.date(1960, 1, 1), dt.date(1969, 12, 31), dt.date(1970, 1, 1), dt.date(2020, 2, 29), dt.date(2020, 3, 1), None, dt.date(1899, 12, 31)]
t = pa.table({"i": pa.array(list(range(1, 8)), pa.int32()), "d": pa.array(days, pa.date32())})
pq.write_table(t, Macro.getLocal("dd"))
end
parqit use using `"`dd'"', clear
sort i
assert "`: format d'" == "%td"
gen byte a1 = d >= td(01jan1970)
gen byte a2 = td(01jan1970) <= d
gen byte a3 = d == td(29feb2020)
gen byte a4 = d < td(01jan1960)
gen byte a5 = td(01mar2020) > d
save `"`nat'"', replace
* native's missing date is larger than every date; parqit's Stata mode
* reproduces that ordering, its default (SQL) gives missing for such a row
parqit set statamissing on
parqit use using `"`dd'"'
parqit gen byte a1 = d >= td(01jan1970)
parqit gen byte a2 = td(01jan1970) <= d
parqit gen byte a3 = d == td(29feb2020)
parqit gen byte a4 = d < td(01jan1960)
parqit gen byte a5 = td(01mar2020) > d
parqit collect, clear
sort i
cf _all using `"`nat'"', verbose
assert a1[6] == 1 & a2[6] == 1 & a3[6] == 0 & a4[6] == 0 & a5[6] == 0
parqit set statamissing off
parqit use using `"`dd'"'
parqit keep if d >= td(01jan1970) & d <= td(01mar2020)
parqit count
assert r(N) == 3
parqit close _all

* ---------- a very long integer literal: Stata's double, never a HUGEINT ---
sysuse auto, clear
keep make price
gen double big = price + 123456789012345678901234567890
gen double big2 = 99999999999999999999 - price
gen byte flag = price > 1000000000000000000000
save `"`nat'"', replace
parqit save `"`stem'_auto.parquet"', data replace
parqit use make price using `"`stem'_auto.parquet"'
parqit gen big = price + 123456789012345678901234567890
parqit gen big2 = 99999999999999999999 - price
parqit gen byte flag = price > 1000000000000000000000
parqit collect, clear
cf _all using `"`nat'"', verbose
parqit close _all

* ---------- two filtered views over one merge, appended (UNION ALL arms
* sharing a join subtree) -------------------------------------------------
sysuse auto, clear
keep make foreign price
tempfile keys
preserve
clear
input byte foreign str8 tag
0 "domestic"
1 "foreign"
end
parqit save `"`stem'_keys.parquet"', data replace
save `"`keys'"', replace
restore
parqit save `"`stem'_mp.parquet"', data replace
merge m:1 foreign using `"`keys'"'
preserve
keep if price > 10000
tempfile hi
save `"`hi'"'
restore
keep if price < 4000
append using `"`hi'"'
sort make
drop _merge
save `"`nat'"', replace
parqit use using `"`stem'_mp.parquet"', name(a)
parqit merge m:1 foreign using `"`stem'_keys.parquet"', nogen
parqit keep if price < 4000
parqit use using `"`stem'_mp.parquet"', name(b)
parqit merge m:1 foreign using `"`stem'_keys.parquet"', nogen
parqit keep if price > 10000
parqit use using view:a, name(c)
parqit append using view:b
parqit collect, clear
sort make
cf _all using `"`nat'"', verbose
* 13 cars below 4,000 and 8 above 10,000
assert _N == 21
parqit close _all

* ---------- messages are parqit's own, not the engine's quoted identifiers --
parqit use using `"`stem'_auto.parquet"'
capture noisily parqit keep if nosuchvar == 1
assert _rc == 198
capture noisily parqit gen z = "price" + 1
assert _rc == 198
parqit close _all

di "VERDICT(V154_DUCKDB_156_SILENT_CHANGES): PASS - with DuckDB 1.5.6, a hive key's MAX/MIN after pruning, a TIME_NS column, DATE comparisons in both directions, a very long integer literal, an append of two filtered views over one merge and parqit's own messages all equal the pyarrow and native oracles"
