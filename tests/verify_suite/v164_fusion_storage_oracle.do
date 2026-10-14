* V164 — storage, state and metadata fidelity with an exact independent oracle.
clear all
set more off
set linesize 255
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
parqit set threads 4
parqit set fill_threads 4
parqit set statamissing on
tempfile source expected work
local directory `"`work'_fusion"'
quietly mkdir `"`directory'"'
set obs 2049
generate long id = _n
generate byte g = mod(id,11)
replace g = . if mod(id,19) == 0
generate str4 gs = string(g)
replace gs = "" if missing(g)
generate float x = (mod(id*29,1024)-512)/8
replace x = . if mod(id,17) == 0 | g == 10
generate double y = (mod(id*7,4096)-2048)/16
label variable x "Dyadic float input"
char x[units] "eighths"
label define groups 0 "zero" 1 "one" 10 "all missing"
label values g groups
sort id
quietly save `"`source'.dta"'
parqit save `"`source'.parquet"', data
local serial 0
foreach group in g gs {
    foreach mode in fused separated {
        foreach typ in double float byte {
            local ++serial
            quietly use `"`source'.dta"', clear
            egen `typ' m = mean(x), by(`group')
            egen `typ' s = total(x), by(`group')
            egen `typ' v = sd(x), by(`group')
            egen `typ' lo = min(x), by(`group')
            egen `typ' hi = max(x), by(`group')
            egen `typ' n = count(x), by(`group')
            egen double dep = total(m), by(`group')
            sort id
            keep in 7/2000
            quietly save `"`expected'"', replace

            parqit use using `"`source'.parquet"'
            parqit sort id
            foreach pair in "m mean" "s total" "v sd" "lo min" "hi max" "n count" {
                gettoken var fn : pair
                parqit egen `typ' `var' = `fn'(x), by(`group')
                if ("`mode'" == "separated") {
                    parqit gen byte barrier = 1
                    parqit drop barrier
                }
            }
            parqit egen double dep = total(m), by(`group')
            parqit keep in 7/2000
            parqit use using view:default, name(copy)
            capture noisily parqit egen double bad = mean(absent)
            assert _rc != 0
            parqit save `"`directory'/case_`serial'.parquet"'
            parqit collect, clear
            assert _N == 1994 & id == _n+6
            cf id g gs x y m s lo hi n using `"`expected'"'
            assert "`: type m'" == "`typ'"
            assert "`: variable label x'" == "Dyadic float input"
            assert "`: char x[units]'" == "eighths"
            assert "`: value label g'" == "groups"
            frame create oracle
            frame oracle: use `"`expected'"'
            frlink 1:1 id, frame(oracle)
            frget oracle_v=v oracle_dep=dep, from(oracle)
            if ("`typ'" == "double") assert abs(v-oracle_v) <= 1e-12*(1+abs(oracle_v)) | missing(v,oracle_v)
            else assert v == oracle_v
            assert missing(v) == missing(oracle_v)
            * The initial exact cf on dep disagreed with native summation.
            * Fraction proved parqit's value exactly equals the rounded mean
            * times group size; retain a native tolerance and the exact oracle.
            assert abs(dep-oracle_dep) <= 1e-12*(1+abs(oracle_dep))
            frame drop oracle
            parqit close _all
            display "AUDIT_FUSION case=`serial' group=`group' mode=`mode' storage=`typ' PASS"
        }
    }
}
display "VERDICT(V164_NATIVE_STORAGE_MATRIX): PASS"

python:
from collections import defaultdict
from decimal import Decimal, localcontext
from fractions import Fraction
from pathlib import Path
from sfi import Macro
import json
import math
import struct

import pyarrow.parquet as pq

run = Path(Macro.getLocal("directory"))
groups = defaultdict(list)
sizes = defaultdict(int)
for i in range(1, 2050):
    g = None if i % 19 == 0 else i % 11
    sizes[g] += 1
    if i % 17 and g != 10:
        groups[g].append(Fraction((i * 29) % 1024 - 512, 8))


def stored(value, storage):
    if value is None:
        return None
    if storage == "byte":
        value = math.trunc(value)
        return value if -127 <= value <= 100 else None
    value = float(value)
    if storage == "float":
        value = struct.unpack("f", struct.pack("f", value))[0]
    return value


expected = {}
for storage in ("double", "float", "byte"):
    for g, size in sizes.items():
        values = groups[g]
        total = sum(values, Fraction())
        mean = total / len(values) if values else None
        variance = (
            sum((v - mean) ** 2 for v in values) / (len(values) - 1)
            if len(values) > 1 else None
        )
        with localcontext() as context:
            context.prec = 80
            sd = (
                (Decimal(variance.numerator) / Decimal(variance.denominator)).sqrt()
                if variance is not None else None
            )
        m = stored(mean, storage)
        expected[storage, g] = {
            "m": m,
            "s": stored(total, storage),
            "v": stored(sd, storage),
            "lo": stored(min(values) if values else None, storage),
            "hi": stored(max(values) if values else None, storage),
            "n": stored(len(values), storage),
            "dep": float(Fraction.from_float(float(m)) * size) if m is not None else 0.0,
        }

summary = []
for case in range(1, 13):
    storage = ("double", "float", "byte")[(case - 1) % 3]
    table = pq.read_table(run / f"case_{case}.parquet")
    data = table.to_pydict()
    assert data["id"] == list(range(7, 2001))
    largest_ulp = 0.0
    for i, g in enumerate(data["g"]):
        for column, wanted in expected[storage, g].items():
            actual = data[column][i]
            if column == "v" and storage == "double" and wanted is not None:
                ulps = abs(actual - wanted) / math.ulp(wanted)
                largest_ulp = max(largest_ulp, ulps)
                assert ulps <= 2, (case, column, g, ulps)
            else:
                assert actual == wanted, (case, column, g)
    summary.append({"case": case, "storage": storage, "rows": table.num_rows,
                    "sd_max_ulp": largest_ulp, "status": "PASS"})

parity = []
for fused, separate in ((1, 4), (2, 5), (3, 6), (7, 10), (8, 11), (9, 12)):
    a = pq.read_table(run / f"case_{fused}.parquet")
    b = pq.read_table(run / f"case_{separate}.parquet")
    parity.append({"fused": fused, "separate": separate,
                   "values_types_metadata_equal": a.equals(b, check_metadata=True)})
    assert parity[-1]["values_types_metadata_equal"], parity[-1]

result = {"method": "Fraction and Decimal(80), exact storage coercion; sd double <= 2 ULP",
          "cases": summary, "fusion_parity": parity, "status": "PASS"}

print("V164 exact oracle: 12 cases, 23928 rows, 167496 result cells passed")
print("V164 fusion parity: 6 paired outputs, values/types/metadata identical")

end
display "VERDICT(V164_EXACT_ORACLE): PASS"
