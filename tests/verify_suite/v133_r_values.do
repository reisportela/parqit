* V133 — R-READ-1 (values and metadata): `parqit save <x>.parquet using <x.rds>`
* (and <x.RData>, object()) writes exactly the data frame R holds. Oracle:
* tests/fixtures/r/expected.json, which R itself wrote from the same objects
* it saved (make_r_fixtures.R), against pyarrow on the Parquet file — numbers
* bit for bit (NA, NaN, ±Inf told apart), haven tagged NAs and SPSS
* user-missing values recovered from their companion codes, dates, date-times
* and times to the microsecond, integer64 exactly, strings byte for byte,
* factors as codes with their levels as value labels — plus the parqit.*
* metadata (labels, formats, characteristics, notes). The same data frame
* saved as serialization version 2, uncompressed and zstd-compressed must give
* the same values. Needs pyarrow importable from Stata's Python.
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile stem
local fix `"`repo'/tests/fixtures/r"'
local fails 0

python:
from sfi import Macro
try:
    import pyarrow
    Macro.setLocal("have_oracle", "1")
except Exception as e:
    print("oracle unavailable:", e)
    Macro.setLocal("have_oracle", "0")
end
if ("`have_oracle'" != "1") {
    di "VERDICT(V133_R_VALUES): FAIL - pyarrow not importable from Stata's Python (see python query)"
    exit
}

* ---- conversions --------------------------------------------------------------
foreach f in frame frame_v2 frame_none frame_zstd rownames edge latin1 empty {
    capture noisily parqit save `"`stem'_`f'.parquet"' using `"`fix'/`f'.rds"', replace
    if (_rc) {
        di as err "conversion of `f'.rds failed: rc=" _rc
        local ++fails
    }
}
capture noisily parqit save `"`stem'_frame.parquet"' using `"`fix'/frame.rds"', replace
if (r(N) != 5 | r(k) != 16 | "`r(r_format)'" != "rds" | "`r(r_compression)'" != "gzip" | ///
    "`r(xmissing_vars)'" != "lab") {
    di as err "frame.rds: r() N=`r(N)' k=`r(k)' format=`r(r_format)' compression=`r(r_compression)' xm=`r(xmissing_vars)'"
    local ++fails
}
capture noisily parqit save `"`stem'_frame_zstd.parquet"' using `"`fix'/frame_zstd.rds"', replace
if ("`r(r_compression)'" != "zstd") local ++fails
foreach o in people towns {
    capture noisily parqit save `"`stem'_ws_`o'.parquet"' using `"`fix'/workspace.RData"', replace object(`o')
    if (_rc | "`r(r_object)'" != "`o'" | "`r(r_format)'" != "RData") {
        di as err "workspace.RData object(`o'): rc=" _rc " object=`r(r_object)'"
        local ++fails
    }
}
capture noisily parqit save `"`stem'_list_first.parquet"' using `"`fix'/list.rds"', replace object(first)
if (_rc) local ++fails
capture noisily parqit save `"`stem'_edge.parquet"' using `"`fix'/edge.rds"', replace
if (r(k_dropped) != 4) {
    di as err "edge.rds: expected 4 columns left out (list, matrix, complex, POSIXlt), got `r(k_dropped)'"
    local ++fails
}

* ---- refusals: loud, nothing written ---------------------------------------------
foreach f in refuse_xz refuse_bz2 refuse_ascii {
    capture erase `"`stem'_`f'.parquet"'
    capture noisily parqit save `"`stem'_`f'.parquet"' using `"`fix'/`f'.rds"', replace
    if (_rc != 610) {
        di as err "`f'.rds: expected rc 610, got " _rc
        local ++fails
    }
    capture confirm file `"`stem'_`f'.parquet"'
    if (!_rc) {
        di as err "`f'.rds: a refused conversion left a file behind"
        local ++fails
    }
}
* which object: several data frames need object(); a wrong one is named
capture noisily parqit save `"`stem'_ws.parquet"' using `"`fix'/workspace.RData"', replace
if (_rc != 198) local ++fails
capture noisily parqit save `"`stem'_ws.parquet"' using `"`fix'/workspace.RData"', replace object(lst)
if (_rc != 198) local ++fails
capture noisily parqit save `"`stem'_ws.parquet"' using `"`fix'/workspace.RData"', replace object(f)
if (_rc != 198) local ++fails
capture noisily parqit save `"`stem'_ws.parquet"' using `"`fix'/workspace.RData"', replace object(Nope)
if (_rc != 198) local ++fails
capture noisily parqit save `"`stem'_ls.parquet"' using `"`fix'/list.rds"', replace
if (_rc != 198) local ++fails
capture noisily parqit save `"`stem'_fr.parquet"' using `"`fix'/frame.rds"', replace object(x)
if (_rc != 198) local ++fails
capture noisily parqit save `"`stem'_x.parquet"' using `"`fix'/../spss/survey.sav"', replace object(x)
if (_rc != 198) local ++fails
capture noisily parqit save `"`stem'_x.parquet"', replace object(x)
if (_rc != 198) local ++fails

python:
from sfi import Macro
import datetime as dt
import decimal
import json
import math
import struct
import pyarrow.parquet as pq

stem = Macro.getLocal("stem")
fix = Macro.getLocal("fix")
fails = 0

def bad(msg):
    global fails
    fails += 1
    print("FAIL:", msg)

def one(x):
    return x[0] if isinstance(x, list) and len(x) == 1 else x

def half_away(x):
    return int(decimal.Decimal(x).to_integral_value(rounding=decimal.ROUND_HALF_UP))

def us_of_seconds(s):
    """the reader's rule: whole seconds exactly, the fraction rounded half away"""
    w = math.floor(s)
    return int(w) * 1000000 + half_away((s - w) * 1e6)

def us_of_days(d):
    w = math.floor(d)
    return int(w) * 86400000000 + half_away((d - w) * 86400000000.0)

def real(text):
    return {"NaN": math.nan, "Inf": math.inf, "-Inf": -math.inf}.get(text, None) if text in ("NaN", "Inf", "-Inf") else float(text)

def same_double(a, b):
    if math.isnan(b):
        return a is not None and math.isnan(a)
    return a is not None and struct.pack("<d", a) == struct.pack("<d", b)

expected = json.load(open(fix + "/expected.json", encoding="utf-8"))
files = {
    "frame.rds": "_frame", "frame_v2.rds": "_frame_v2", "frame_none.rds": "_frame_none",
    "frame_zstd.rds": "_frame_zstd", "rownames.rds": "_rownames", "edge.rds": "_edge",
    "latin1.rds": "_latin1", "empty.rds": "_empty",
    "workspace.RData:people": "_ws_people", "workspace.RData:towns": "_ws_towns",
    "list.rds:first": "_list_first",
}
values = {}
for key, suffix in files.items():
    exp = expected[key]
    t = pq.read_table(stem + suffix + ".parquet")
    md = {k.decode(): v.decode() for k, v in t.schema.metadata.items()}
    schema = json.loads(md["parqit.schema"])
    chars = json.loads(md["parqit.chars"])
    vallabs = json.loads(md["parqit.vallabs"])
    xm = json.loads(md.get("parqit.xmissing", "{}"))
    stata_of = {v["src"]: v for v in schema["vars"]}
    nrow = one(exp["nrow"])
    if t.num_rows != nrow:
        bad(f"{key}: {t.num_rows} rows, R has {nrow}")
    names = [c for c in t.schema.names if c not in xm.values()]
    rn = exp.get("rownames")
    if rn:
        if names[0] != "rowname" or t.column("rowname").to_pylist() != rn:
            bad(f"{key}: the row names are not the first column, rowname")
        names = names[1:]
    cols = exp["columns"]
    if len(names) != len(cols):
        bad(f"{key}: {len(names)} columns, R has {len(cols)} Stata can hold")
        continue
    got_values = {}
    for pname, c in zip(names, cols):
        rname = one(c["name"])
        where = f"{key} {rname!r} ({pname})"
        if rname and [one(x["name"]) for x in cols].count(rname) == 1 and pname != rname:
            bad(f"{where}: written as {pname}")
        cls = c["class"]
        typeof = one(c["typeof"])
        typ = str(t.schema.field(pname).type)
        vals = t.column(pname).to_pylist()
        got_values[pname] = vals
        comp = t.column(xm[pname]).to_pylist() if pname in xm else [0] * len(vals)
        exp_vals = c["values"]
        meta = stata_of.get(pname, {})
        vc = chars.get(meta.get("name", ""), {})
        code_of = {}
        for part in vc.get("r_missing_map", "").split(" "):
            if "=" in part:
                code, val = part.split("=", 1)
                code_of[float(val)] = ord(code[1]) - ord("a") + 1
        for i, (v, e, k) in enumerate(zip(vals, exp_vals, comp)):
            w = f"{where} row {i + 1}"
            if "integer64" in cls:
                if (None if e is None else int(e)) != v or typ != "int64":
                    bad(f"{w}: integer64 {e!r} became {v!r} ({typ})")
            elif "factor" in cls:
                if v != e or typ != "int32":
                    bad(f"{w}: factor code {e!r} became {v!r} ({typ})")
            elif "Date" in cls:
                if e is None:
                    if v is not None: bad(f"{w}: NA date became {v!r}")
                    continue
                days = int(float(e))
                if typ != "date32[day]" or (v - dt.date(1970, 1, 1)).days != days:
                    bad(f"{w}: date {e!r} became {v!r} ({typ})")
            elif "POSIXct" in cls:
                if e is None:
                    if v is not None: bad(f"{w}: NA date-time became {v!r}")
                    continue
                us = (v - dt.datetime(1970, 1, 1)) // dt.timedelta(microseconds=1)
                if typ != "timestamp[us]" or us != us_of_seconds(float(e)):
                    bad(f"{w}: date-time {e!r} became {v!r} ({typ})")
            elif "hms" in cls:
                if e is None:
                    if v is not None: bad(f"{w}: NA time became {v!r}")
                    continue
                us = ((v.hour * 60 + v.minute) * 60 + v.second) * 1000000 + v.microsecond
                if typ != "time64[us]" or us != us_of_seconds(float(e)):
                    bad(f"{w}: time {e!r} became {v!r} ({typ})")
            elif typeof == "character" and not typ.startswith("double"):
                if v != e or typ != "string":
                    bad(f"{w}: {e!r} became {v!r} ({typ})")
            elif typeof in ("integer", "logical"):
                want_t = "bool" if typeof == "logical" else "int32"
                if v != e or typ != want_t:
                    bad(f"{w}: {e!r} became {v!r} ({typ})")
            else:
                # double (plain, difftime, labelled) — and the numbers of a
                # deferred as.character() of doubles
                if typ != "double":
                    bad(f"{w}: stored as {typ}, expected double")
                    continue
                if e is None:
                    if v is not None or k != 0: bad(f"{w}: NA became {v!r} code {k}")
                elif e.startswith("NA("):
                    want = ord(e[3]) - ord("a") + 1
                    if v is not None or k != want: bad(f"{w}: tagged {e} became {v!r} code {k}")
                else:
                    x = real(e)
                    if not math.isnan(x) and x in code_of:
                        if v is not None or k != code_of[x]:
                            bad(f"{w}: user-missing {e} became {v!r} code {k} (map {vc.get('r_missing_map')})")
                    elif not same_double(v, x) or k != 0:
                        bad(f"{w}: {e!r} became {v!r} code {k}")
    values[key] = got_values

# the same data frame in every form R writes it
base = values.get("frame.rds")
for other in ("frame_v2.rds", "frame_none.rds", "frame_zstd.rds"):
    got = values.get(other)
    if got is None or got.keys() != base.keys():
        bad(f"{other}: columns differ from frame.rds")
        continue
    for name in base:
        a, b = base[name], got[name]
        if not all((x == y) or (isinstance(x, float) and isinstance(y, float) and math.isnan(x) and math.isnan(y))
                   for x, y in zip(a, b)):
            bad(f"{other}: column {name} differs from frame.rds")

# metadata of frame.rds
t = pq.read_table(stem + "_frame.parquet")
md = {k.decode(): v.decode() for k, v in t.schema.metadata.items()}
schema = json.loads(md["parqit.schema"])
chars = json.loads(md["parqit.chars"])
vallabs = json.loads(md["parqit.vallabs"])
var = {v["name"]: v for v in schema["vars"]}
def check(cond, msg):
    if not cond: bad("frame.rds metadata: " + msg)
check(json.loads(md["parqit.dtalabel"]) == "Fixture data frame", "dataset label")
check(var["lab"]["varlab"] == "Question one" and var["lab"]["vallab"] == "lab", "lab labels")
check(var["sex"]["varlab"] == "Sex", "sex variable label")
check(var["stata_fmt"]["fmt"] == "%9.2f", "format.stata becomes the display format")
check(chars["stata_fmt"].get("r_label", "").startswith("long long") and len(chars["stata_fmt"]["r_label"]) > 80,
      "a label longer than 80 characters is kept whole in char r_label")
check(var["dt"]["fmt"] == "%td" and var["ct"]["fmt"] == "%tc" and var["hm"]["fmt"] == "%tcHH:MM:SS",
      "temporal formats")
check(var["i"]["type"] == "double" and var["l"]["type"] == "byte" and var["s"]["type"].startswith("str"),
      "recorded Stata types")
check(vallabs["f"]["entries"] == [["1", "lo"], ["2", "mid"], ["3", "hi"]], "factor levels as a value label")
check(vallabs["o"]["entries"] == [["1", "a"], ["2", "b"], ["3", "c"]], "ordered factor levels")
lab = {k: v for k, v in vallabs["lab"]["entries"]}
check(lab.get(".a") == "Skipped" and lab.get(".b") == "Refused" and lab.get(".c") == "Unknown" and
      lab.get("98") == "Refused" and lab.get("1") == "Yes", f"haven labels with tagged and user-missing codes: {lab}")
check(chars["lab"].get("r_missing_map") == ".b=98 .c=99", "the tagged NA keeps .a; user-missing take .b .c")
check(chars["lab"].get("r_missing") == "na_values: 98, 99", "na_values recorded")
check(json.loads(chars["score"]["r_value_labels"]) == [[0.5, "Half"], [1, "One"]], "non-integer keys kept in full")
check(vallabs.get("score", {}).get("entries") == [["1", "One"]], "the integer key still labels natively")
check(json.loads(chars["sex"]["r_value_labels"]) == [["M", "Male"], ["F", "Female"], ["X", "Not stated"]],
      "labels of a character vector")
check(chars["sex"].get("r_missing") == 'na_values: "X"', "string user-missing values recorded as text")
check(chars["ct"].get("r_tzone") == "Europe/Lisbon", "time zone kept")
check(chars["dtm"].get("r_units") == "days" and chars["hm"].get("r_units") == "secs", "difftime units kept")
check(chars["o"].get("r_class") == "ordered factor" and chars["idate"].get("r_class") == "IDate Date", "R classes")
check(chars["i"].get("note1") == "first note on i" and chars["i"].get("note0") == "2", "comment() becomes notes")
dta = chars["_dta"]
check(dta.get("note1") == "a note on the data frame" and dta.get("r_class") == "data.frame", "_dta notes and class")
check("source_info" in json.loads(dta.get("r_attributes", "{}")), "other data frame attributes kept as JSON")
check("serialization version 3, gzip" in dta.get("r_written_by", ""), "writer recorded")
t2 = pq.read_table(stem + "_frame_v2.parquet")
d2 = json.loads({k.decode(): v.decode() for k, v in t2.schema.metadata.items()}["parqit.chars"])["_dta"]
check("serialization version 2" in d2.get("r_written_by", "") and d2.get("r_encoding", "").startswith("UTF-8"),
      "a version 2 file says so")
# the workspace object and the edge names
tw = pq.read_table(stem + "_ws_people.parquet")
cw = json.loads({k.decode(): v.decode() for k, v in tw.schema.metadata.items()}["parqit.chars"])
check(cw["_dta"].get("r_object") == "people", "the .RData object read is recorded")
te = pq.read_table(stem + "_edge.parquet")
check(te.schema.names[:5] == ["x", "x_1", "V3", "V4", "X"], f"edge names {te.schema.names[:5]}")
ce = json.loads({k.decode(): v.decode() for k, v in te.schema.metadata.items()}["parqit.chars"])
check(ce.get("x_1", {}).get("r_name") == "x" and "r_deferred" in ce.get("dr", {}), "renamed and deferred columns")

Macro.setLocal("pyfails", str(fails))
end
local fails = `fails' + `pyfails'
if (`fails' == 0) di "VERDICT(V133_R_VALUES): PASS"
else di "VERDICT(V133_R_VALUES): FAIL - `fails' check(s) failed"
