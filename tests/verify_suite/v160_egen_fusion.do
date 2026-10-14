* V160: fused independent windows preserve native values, metadata, order and
* sequential dependencies. The on-disk payload has a separate Python oracle.
clear all
set more off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
parqit set threads 4
parqit set statamissing on
tempfile source expected result
set obs 6000
gen long id = _n
gen byte g = mod(id,5)
replace g = . if mod(id,11) == 0
gen double x = mod(id*37,8192)/8
replace x = . if mod(id,23) == 0 | g == 4
label variable x "Synthetic exact dyadic values"
char x[units] "eighths"
label define groups 0 "zero" 1 "one" 2 "two" 3 "three" 4 "missing"
label values g groups
sort id
parqit save "`source'.parquet", data replace
egen double m = mean(x), by(g)
egen double s = total(x), by(g)
egen double lo = min(x), by(g)
egen double hi = max(x), by(g)
egen double n = count(x), by(g)
egen float fm = mean(x), by(g)
egen double dep = total(s), by(g)
egen double first = total(x) if id <= 50, by(g)
sort id
save "`expected'", replace

parqit use using "`source'.parquet"
parqit sort id
parqit egen double m = mean(x), by(g)
parqit egen double s = total(x), by(g)
parqit egen double lo = min(x), by(g)
parqit egen double hi = max(x), by(g)
parqit egen double n = count(x), by(g)
parqit egen float fm = mean(x), by(g)
* Validate the independent batch before adding dependent window stages.
parqit save "`result'_fused.parquet", replace
parqit collect, clear
cf id g x m s lo hi n fm using "`expected'"
parqit egen double dep = total(s), by(g)
parqit egen double first = total(x) if id <= 50, by(g)
parqit gen double position = _n
parqit save "`result'.parquet", replace
parqit collect, clear
assert id == position & id == _n
drop position
cf _all using "`expected'"
assert "`: type fm'" == "float"
assert "`: variable label x'" == "Synthetic exact dyadic values"
assert "`: char x[units]'" == "eighths"
assert "`: value label g'" == "groups"
assert "`: sortedby'" == "id"
parqit close _all

* SQL NULL and the native empty string belong to the same string group.
parqit sql "SELECT * FROM (VALUES (NULL::VARCHAR,10.0),('',20.0),('a',NULL),('a',NULL)) t(g,x)"
parqit egen double m = mean(x), by(g)
parqit egen double s = total(x), by(g)
parqit egen double n = count(x), by(g)
parqit collect, clear
assert _N == 4
assert m == 15 & s == 30 & n == 2 if g == ""
assert missing(m) & s == 0 & n == 0 if g == "a"
parqit close _all

python:
from sfi import Macro
import pyarrow.parquet as pq
from fractions import Fraction
key = lambda i: None if i % 11 == 0 else i % 5
for suffix in ["_fused.parquet", ".parquet"]:
    table = pq.read_table(Macro.getLocal("result") + suffix)
    assert table.num_rows == 6000
    data = table.to_pydict()
    for g in [0,1,2,3,4,None]:
        members = [i for i in range(1,6001) if key(i) == g]
        values = [Fraction((i*37) % 8192,8) for i in members if i % 23 and g != 4]
        total = sum(values, Fraction(0))
        mean = float(total / len(values)) if values else None
        for i in range(6000):
            if data["g"][i] != g:
                continue
            assert data["m"][i] == mean
            assert data["s"][i] == float(total)
            assert data["n"][i] == len(values)
            if "dep" in data:
                assert data["dep"][i] == float(total*len(members))
    assert data["id"] == list(range(1,6001))
    if "position" in data:
        assert data["id"] == data["position"]
    assert b"parqit.schema" in table.schema.metadata
end

* A range saved between aggregates validates its historical prefix.
parqit use using "`source'.parquet"
parqit egen double s = total(x), by(g)
parqit egen double m = mean(x), by(g)
parqit keep in 1/50
parqit egen double after = total(x), by(g)
parqit collect, clear
assert _N == 50
bysort g: egen double check = total(x)
assert after == check
parqit close _all
display "VERDICT(V160_EGEN_FUSION): PASS - native and Fraction oracles, dependencies, missing groups, float storage, order, metadata, collect and save"
