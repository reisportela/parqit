* Exact distinct-value aggregation: native tags + collapse, Python sets and
* independent PyArrow reads. Missing is one level, as in parqit unique.
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
parqit set threads 4
tempfile stem native
clear
input byte g byte t double x str4 s byte j
1 1 1 "a" 1
1 2 1 "a" 1
1 3 . "" 2
1 4 2 "" 2
2 5 . "" 1
2 6 . "" 2
. 7 3 "é" 1
. 8 3 "é" 2
3 9 0 " " 1
3 10 0 " " 1
end
format x %12.2f
parqit save `"`stem'.parquet"', data

* Native oracle: tag each value, including missing, then sum the tags.
bysort g x: gen byte tx = _n==1
bysort g s: gen byte ts = _n==1
collapse (sum) ux=tx us=ts (count) n=x, by(g)
save `"`native'"'
parqit use using `"`stem'.parquet"', name(source)
parqit use using view:source, name(aggregate)
parqit view source
parqit view aggregate: collapse (unique) ux=x us=s (nunique) nx_alias=x (count) n=x, by(g)
parqit count
assert r(N)==10
quietly parqit _dlgviews no_dialog, report
assert "`r(current)'"=="source"
parqit view aggregate: save `"`stem'_grouped.parquet"'
parqit view aggregate: collect, clear
cf g ux us n using `"`native'"', verbose
assert ux==nx_alias
assert "`: type ux'"=="long" & "`: type us'"=="long"
assert "`: format ux'"=="%12.2f" & "`: format us'"=="%8.0g"
assert "`: variable label ux'"=="(unique) x"
assert "`: variable label nx_alias'"=="(nunique) x"

* An if qualifier restricts values before the aggregate; aliases mix freely.
parqit view source: collapse (unique) ux=x us=s (nunique) nx_alias=x if t!=4, by(g)
parqit view source: save `"`stem'_filtered.parquet"'
parqit view source: collect, clear
assert ux==2 & us==2 if g==1
assert ux==1 & us==1 if g==2
parqit close _all

* Foreign integer values above 2^53 stay distinct before materialisation;
* string NULL/empty, strL-sized UTF-8, signed zero and nonfinite missings.
python:
from sfi import Macro
import pyarrow as pa, pyarrow.parquet as pq
stem = Macro.getLocal("stem")
raw = {
    "g": [1, 1, 1, 1, 2, 2],
    "big": [2**53, 2**53+1, None, 2**53, None, None],
    "d": [0., -0., float("nan"), float("inf"), None, -float("inf")],
    "s": ["é"*1500, "é"*1500, "", None, "", None],
}
pq.write_table(pa.table(raw), stem + "_foreign.parquet")
end
parqit use using `"`stem'_foreign.parquet"'
parqit collapse (unique) ub=big ud=d us=s (nunique) ab=big, by(g)
parqit save `"`stem'_foreign_counts.parquet"'
parqit collect, clear
assert ub==3 & ab==3 & ud==2 & us==2 if g==1
assert ub==1 & ab==1 & ud==1 & us==1 if g==2
parqit close _all

* No by(), an empty sample, and a pivot use the same aggregation semantics.
parqit use using `"`stem'.parquet"'
parqit collapse (unique) ux=x us=s
parqit collect, clear
assert _N==1 & ux==5 & us==4
parqit close _all
parqit use using `"`stem'.parquet"'
parqit collapse (unique) s if 0
parqit collect, clear
assert _N==0 & "`: type s'"=="long"
parqit close _all
parqit use using `"`stem'.parquet"'
parqit pivot (unique) ux=x (nunique) us=s, rows(g) cols(j)
parqit collect, clear
assert _N==4
assert ux1==1 & ux2==2 & us1==1 & us2==1 if g==1
assert ux1==1 & ux2==1 & us1==1 & us2==1 if g==2
assert ux1==1 & missing(ux2) if g==3
parqit close _all

* Failed requests must leave the source plan usable.
parqit use using `"`stem'.parquet"'
capture noisily parqit collapse (unique) u=x (notastat) bad=x if t>2, by(g)
assert _rc!=0
parqit count
assert r(N)==10
capture noisily parqit collapse (unique) x=x s=s, by(x)
assert _rc!=0
parqit count
assert r(N)==10
parqit unique x
assert r(unique)==5
parqit collapse (nunique) x
parqit collect, clear
assert _N==1 & x==5
parqit close _all

python:
from collections import defaultdict
from sfi import Macro
import math, pyarrow.parquet as pq
stem = Macro.getLocal("stem")
rows = pq.read_table(stem + ".parquet").to_pylist()
def norm(v):
    if v == "" or isinstance(v, float) and (not math.isfinite(v) or abs(v) >= 2.**1023):
        return None
    return v
def expected(rows, specs):
    groups = defaultdict(list)
    for row in rows:
        groups[norm(row["g"])].append(row)
    result = {}
    for g, items in groups.items():
        result[g] = {target: len({norm(r[src]) for r in items}) for target, src in specs.items()}
    return result
def check(path, source, specs):
    want = expected(source, specs)
    got = pq.read_table(path).to_pylist()
    assert len(got) == len(want)
    for row in got:
        for target, value in want[row["g"]].items():
            assert row[target] == value, (path, row["g"], target, row[target], value)
check(stem + "_grouped.parquet", rows, {"ux":"x", "nx_alias":"x", "us":"s"})
check(stem + "_filtered.parquet", [r for r in rows if r["t"] != 4], {"ux":"x", "nx_alias":"x", "us":"s"})
foreign = pq.read_table(stem + "_foreign.parquet").to_pylist()
check(stem + "_foreign_counts.parquet", foreign, {"ub":"big", "ab":"big", "ud":"d", "us":"s"})
print("Independent Python sets / PyArrow oracle: PASS")
end
display "VERDICT(V195_COLLAPSE_UNIQUE): PASS"
