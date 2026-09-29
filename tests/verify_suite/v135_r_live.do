* V135 — R-READ-1 (against a live R): R, when installed, writes random data
* frames here and now (seeded), in every form it writes them — gzip, zstd,
* uncompressed, serialization version 2, and an .RData beside a function — and
* writes, from the same objects, the text of every value (%.17g for doubles).
* `parqit save … using` converts each; pyarrow on the Parquet files must give
* R's values cell for cell: NA/NaN/±Inf/-0/subnormals, haven tagged NAs of
* random letters, integer64 across the whole range, dates with fractions of a
* day, date-times to the microsecond, text with every kind of character (long
* texts too), factors with hundreds of levels. A 3,000,000-row data frame then
* goes through the same path out of core (sums and counts against R's). The
* test is skipped with a note when Rscript or jsonlite is not available.
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile stem
local fails 0

python:
from sfi import Macro
import shutil, subprocess
stem = Macro.getLocal("stem")
gen = Macro.getLocal("repo") + "/tests/fixtures/r/make_live_fixtures.R"
rscript = shutil.which("Rscript")
res = subprocess.run([rscript, gen, stem], capture_output=True, text=True, timeout=1200) if rscript else None
status = "Rscript not found" if rscript is None else "jsonlite not installed" if "NOJSONLITE" in res.stdout else "ok" if "DONE" in res.stdout else "failed"
print("R failed:", res.stdout[-2000:], res.stderr[-2000:]) if status == "failed" else None
Macro.setLocal("status", status)
end
if ("`status'" == "Rscript not found" | "`status'" == "jsonlite not installed") {
    di "note: `status'; the live R comparison is skipped"
    di "VERDICT(V135_R_LIVE): PASS (skipped: `status')"
    exit
}
if ("`status'" != "ok") {
    di "VERDICT(V135_R_LIVE): FAIL - R could not write the fixtures"
    exit
}

foreach f in gz none v2 zstd {
    capture confirm file `"`stem'_`f'.rds"'
    if (_rc) continue
    capture noisily parqit save `"`stem'_`f'.parquet"' using `"`stem'_`f'.rds"', replace
    if (_rc) {
        di as err "conversion of the `f' file failed: rc=" _rc
        local ++fails
    }
}
capture noisily parqit save `"`stem'_ws.parquet"' using `"`stem'_ws.RData"', replace
if (_rc | "`r(r_object)'" != "df") local ++fails
timer clear 1
timer on 1
capture noisily parqit save `"`stem'_big.parquet"' using `"`stem'_big.rds"', replace
timer off 1
if (_rc) local ++fails
quietly timer list 1
di "3,000,000 rows converted in " %6.2f r(t1) " s"

python:
from sfi import Macro
import datetime as dt
import decimal
import json
import math
import struct
import pyarrow.parquet as pq
import pyarrow.compute as pc

stem = Macro.getLocal("stem")
fails = 0
def bad(msg):
    global fails
    fails += 1
    if fails <= 40:
        print("FAIL:", msg)

def half_away(x):
    return int(decimal.Decimal(x).to_integral_value(rounding=decimal.ROUND_HALF_UP))

def us_of(s, scale):
    w = math.floor(s)
    return int(w) * scale + half_away((s - w) * float(scale))

def real(t):
    return {"NaN": math.nan, "Inf": math.inf, "-Inf": -math.inf, "-0": -0.0}.get(t) if t in ("NaN", "Inf", "-Inf", "-0") else float(t)

def same(a, b):
    if math.isnan(b):
        return a is not None and math.isnan(a)
    return a is not None and struct.pack("<d", a) == struct.pack("<d", b)

o = json.load(open(stem + "_oracle.json", encoding="utf-8"))
w = json.load(open(stem + "_i64.json"))
i64_want = [(int(h) << 32) + (int(l) & 0xFFFFFFFF) for h, l in zip(w["hi"], w["lo"])]
n = o["n"]
forms = ["gz", "none", "v2", "ws"] + (["zstd"] if o["zstd"] else [])
for form in forms:
    t = pq.read_table(stem + "_" + form + ".parquet")
    md = {k.decode(): v.decode() for k, v in t.schema.metadata.items()}
    xm = json.loads(md.get("parqit.xmissing", "{}"))
    vl = json.loads(md["parqit.vallabs"])
    if t.num_rows != n: bad(f"{form}: {t.num_rows} rows != {n}")
    col = {c: t.column(c).to_pylist() for c in t.schema.names}
    tagc = col.get(xm.get("tag", ""), [0] * n)
    for i in range(n):
        if col["ints"][i] != o["ints"][i]: bad(f"{form} ints {i}: {col['ints'][i]} != {o['ints'][i]}")
        e = o["dbl"][i]
        if e is None:
            if col["dbl"][i] is not None: bad(f"{form} dbl {i}: NA became {col['dbl'][i]}")
        elif not same(col["dbl"][i], real(e)): bad(f"{form} dbl {i}: {e} became {col['dbl'][i]!r}")
        e = o["tag"][i]
        if e is None:
            if col["tag"][i] is not None or tagc[i] != 0: bad(f"{form} tag {i}: NA became {col['tag'][i]} code {tagc[i]}")
        elif e.startswith("NA("):
            if col["tag"][i] is not None or tagc[i] != ord(e[3]) - 96: bad(f"{form} tag {i}: {e} became code {tagc[i]}")
        elif not same(col["tag"][i], real(e)) or tagc[i] != 0: bad(f"{form} tag {i}: {e} became {col['tag'][i]!r}")
        if col["txt"][i] != o["txt"][i]: bad(f"{form} txt {i}: {o['txt'][i]!r} became {col['txt'][i]!r}")
        if col["lg"][i] != o["lg"][i]: bad(f"{form} lg {i}")
        if col["fac"][i] != o["fac"][i]: bad(f"{form} fac {i}")
        e = o["day"][i]
        got = col["day"][i]
        if (e is None) != (got is None) or (e is not None and (got - dt.date(1970, 1, 1)).days != int(float(e))):
            bad(f"{form} day {i}: {e} became {got}")
        e = o["fday"][i]
        got = col["fday"][i]
        if (e is None) != (got is None) or (e is not None and (got - dt.datetime(1970, 1, 1)) // dt.timedelta(microseconds=1) != us_of(float(e), 86400000000)):
            bad(f"{form} fday {i}: {e} became {got}")
        e = o["when"][i]
        got = col["when"][i]
        if (e is None) != (got is None) or (e is not None and (got - dt.datetime(1970, 1, 1)) // dt.timedelta(microseconds=1) != us_of(float(e), 1000000)):
            bad(f"{form} when {i}: {e} became {got}")
        e = o["clock"][i]
        got = col["clock"][i]
        us = ((got.hour * 60 + got.minute) * 60 + got.second) * 1000000 + got.microsecond
        if us != us_of(float(e), 1000000): bad(f"{form} clock {i}: {e} became {got}")
        if col["i64"][i] != i64_want[i]: bad(f"{form} i64 {i}: {i64_want[i]} became {col['i64'][i]}")
    if [e[1] for e in vl["fac"]["entries"]] != o["levels"]: bad(f"{form}: factor levels")
    if str(t.schema.field("fday").type) != "timestamp[us]" or str(t.schema.field("day").type) != "date32[day]":
        bad(f"{form}: Date types {t.schema.field('day').type} {t.schema.field('fday').type}")

import numpy as np
b = json.load(open(stem + "_big.json"))
t = pq.read_table(stem + "_big.parquet")
if t.num_rows != b["n"]: bad(f"big: {t.num_rows} rows")
x = t.column("x").to_numpy()
if not np.array_equal(x, np.arange(1, b["n"] + 1) / 7): bad("big: x differs from (1:N)/7")
if t.column("id").to_pylist()[-1] != b["n"] or t.column("s").to_pylist()[-1] != b["s_last"]: bad("big: last row")
if pc.sum(pc.equal(t.column("g"), 1)).as_py() != b["g_a"]: bad("big: factor counts")
Macro.setLocal("pyfails", str(fails))
end
local fails = `fails' + `pyfails'
if (`fails' == 0) di "VERDICT(V135_R_LIVE): PASS"
else di "VERDICT(V135_R_LIVE): FAIL - `fails' check(s) failed"
