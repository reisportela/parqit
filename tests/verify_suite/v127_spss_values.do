* V127 — SPSS-READ-1 (values): `parqit save <x>.parquet using <x.sav>` writes
* exactly the values the SPSS file holds, in the three data layouts of the
* format (uncompressed, bytecode-compressed, ZLIB .zsav). Oracle: pyreadstat
* (ReadStat, a reader independent of parqit's) on the SPSS file against
* pyarrow on the Parquet file — numbers bit for bit, every user-missing value
* recovered from its companion code and char var[spss_missing_map], dates,
* date-times and times back to exact SPSS seconds, strings byte for byte —
* and the three conversions agree cell for cell.
* Needs pyreadstat and pyarrow importable from Stata's Python (`python query`;
* pyreadstat's dict output needs no pandas); fixtures in tests/fixtures/spss.
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile stem
local fix `"`repo'/tests/fixtures/spss"'
local fails 0

python:
from sfi import Macro
try:
    import pyreadstat, pyarrow
    Macro.setLocal("have_oracle", "1")
except Exception as e:
    print("oracle unavailable:", e)
    Macro.setLocal("have_oracle", "0")
end
if ("`have_oracle'" != "1") {
    di "VERDICT(V127_SPSS_VALUES): FAIL - pyreadstat/pyarrow not importable from Stata's Python (see python query)"
    exit
}

foreach f in survey survey_bc {
    capture noisily parqit save `"`stem'_`f'.parquet"' using `"`fix'/`f'.sav"', replace
    if (_rc) {
        di as err "conversion of `f'.sav failed: rc=" _rc
        local ++fails
    }
    else if (r(N) != 60 | r(k) != 15) local ++fails
}
capture noisily parqit save `"`stem'_survey_z.parquet"' using `"`fix'/survey.zsav"', replace
if (_rc) local ++fails
else if ("`r(spss_compression)'" != "zlib") local ++fails

python:
from sfi import Macro
import datetime as dt
import decimal
import json
import math
import pyarrow.parquet as pq
import pyreadstat

stem = Macro.getLocal("stem")
fix = Macro.getLocal("fix")
EPOCH = 12219379200  # seconds from 14 Oct 1582 to 1 Jan 1970
fails = 0

def bad(msg):
    global fails
    fails += 1
    print("FAIL:", msg)

def us_of(seconds):
    """exact microseconds since 1970 of SPSS seconds, rounding half away from zero"""
    q = (decimal.Decimal(seconds) - EPOCH) * 1000000
    return int(q.to_integral_value(rounding=decimal.ROUND_HALF_UP))

def code_map(chars, stata):
    out = {}
    for part in chars.get(stata, {}).get("spss_missing_map", "").split(" "):
        if "=" in part:
            code, val = part.split("=", 1)
            if not val.startswith("other") and "," not in val:
                out[ord(code[1]) - ord("a") + 1] = float(val)
    return out

tables = []
for sav, pqf in [("survey.sav", "_survey"), ("survey_bc.sav", "_survey_bc"), ("survey.zsav", "_survey_z")]:
    d, m = pyreadstat.read_sav(fix + "/" + sav, user_missing=True,
                               disable_datetime_conversion=True, output_format="dict")
    t = pq.read_table(stem + pqf + ".parquet")
    tables.append(t)
    md = {k.decode(): v.decode() for k, v in t.schema.metadata.items()}
    chars = json.loads(md["parqit.chars"])
    schema = json.loads(md["parqit.schema"])
    xm = json.loads(md.get("parqit.xmissing", "{}"))
    stata_of = {v["src"]: v["name"] for v in schema["vars"]}
    names = m.column_names
    cols = t.schema.names
    if cols[:len(names)] != names:
        bad(f"{sav}: column order {cols[:len(names)]} != {names}")
    if sorted(cols[len(names):]) != sorted(xm.values()):
        bad(f"{sav}: companions {cols[len(names):]} != parqit.xmissing {xm}")
    if t.num_rows != m.number_rows:
        bad(f"{sav}: {t.num_rows} rows != {m.number_rows}")
    want_type = {"bday": "date32[day]", "visit": "date32[day]", "stamp": "timestamp[us]",
                 "clock": "time64[us]", "comment": "string", "sex": "string",
                 "city": "string", "income": "double", "q1": "double"}
    for name, typ in want_type.items():
        if str(t.schema.field(name).type) != typ:
            bad(f"{sav}: {name} is {t.schema.field(name).type}, expected {typ}")
    for name in names:
        typ = t.schema.field(name).type
        vals = t.column(name).to_pylist()
        comp = t.column(xm[name]).to_pylist() if name in xm else [0] * len(vals)
        codes = code_map(chars, stata_of[name])
        is_string = m.readstat_variable_types[name] == "string"
        for i, (v, r, c) in enumerate(zip(vals, d[name], comp)):
            where = f"{sav} {name} case {i + 1}"
            if is_string:
                if v != r:
                    bad(f"{where}: {v!r} != {r!r}")
                continue
            if r is None or (isinstance(r, float) and math.isnan(r)):
                if v is not None or c != 0:
                    bad(f"{where}: system-missing became {v!r} (code {c})")
                continue
            if c:
                if v is not None or codes.get(c) != r:
                    bad(f"{where}: user-missing {r!r} became {v!r} with code {c} -> {codes.get(c)!r}")
                continue
            if v is None:
                bad(f"{where}: value {r!r} became null")
                continue
            if str(typ) == "double":
                if not (v == r and math.copysign(1, v) == math.copysign(1, r)):
                    bad(f"{where}: {v!r} != {r!r}")
            elif str(typ) == "date32[day]":
                secs = ((v - dt.date(1970, 1, 1)).days + 141428) * 86400
                if secs != r:
                    bad(f"{where}: date {v} is {secs} s, SPSS has {r!r}")
            elif str(typ) == "timestamp[us]":
                us = (v - dt.datetime(1970, 1, 1)) // dt.timedelta(microseconds=1)
                if us != us_of(r):
                    bad(f"{where}: timestamp {v} != SPSS {r!r}")
            elif str(typ) == "time64[us]":
                us = ((v.hour * 60 + v.minute) * 60 + v.second) * 1000000 + v.microsecond
                if us != us_of(r + EPOCH):
                    bad(f"{where}: time {v} != SPSS {r!r}")
            else:
                bad(f"{where}: unexpected Parquet type {typ}")

# the three layouts hold the same file: identical data, column for column
for other in tables[1:]:
    if not tables[0].equals(other):
        bad("the uncompressed, bytecode and ZLIB conversions differ")

print("python oracle failures:", fails)
Macro.setLocal("pyfails", str(fails))
end
local fails = `fails' + `pyfails'

if (`fails') di "VERDICT(V127_SPSS_VALUES): FAIL - `fails' checks"
else di "VERDICT(V127_SPSS_VALUES): PASS"
