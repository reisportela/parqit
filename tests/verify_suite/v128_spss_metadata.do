* V128 — SPSS-READ-1 (metadata): everything an SPSS dictionary holds reaches
* the Parquet file (parqit.* metadata) and, through `parqit use`, Stata: the
* file label, documents, variable labels (a label longer than Stata's 80
* characters in full in char var[spss_label]), value labels (integer keys
* natively; the complete set, non-integer and string keys included, in char
* var[spss_value_labels]), missing-value definitions in SPSS syntax, their
* extended-missing codes and the labels those codes carry, print formats,
* measurement levels and display widths, and the original names of variables
* Stata cannot name. Oracle: pyreadstat's reading of the same file's
* dictionary (ReadStat, independent of parqit's reader).
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
    di "VERDICT(V128_SPSS_METADATA): FAIL - pyreadstat/pyarrow not importable from Stata's Python (see python query)"
    exit
}

parqit save `"`stem'.parquet"' using `"`fix'/survey.sav"', replace
parqit use `"`stem'.parquet"', clear

* the value labels as Stata holds them: one "label<TAB>value<TAB>text" line each
mata:
void _v128_dump_labels(string scalar path, string rowvector names)
{
    real scalar      fh, i, j
    real colvector   vals
    string colvector txt

    fh = fopen(path, "w")
    for (i = 1; i <= cols(names); i++) {
        st_vlload(names[i], vals, txt)
        for (j = 1; j <= rows(vals); j++)
            fput(fh, names[i] + char(9) + strofreal(vals[j], "%21.0g") + char(9) + txt[j])
    }
    fclose(fh)
}
end
quietly label dir
mata: _v128_dump_labels(st_local("stem") + "_labels.txt", tokens(st_global("r(names)")))

python:
from sfi import Characteristic, Data, Macro, ValueLabel
import json
import math
import pyarrow.parquet as pq
import pyreadstat

stem = Macro.getLocal("stem")
fix = Macro.getLocal("fix")
fails = 0

def bad(msg):
    global fails
    fails += 1
    print("FAIL:", msg)

def check(what, got, want):
    if got != want:
        bad(f"{what}: {got!r} != {want!r}")

d, m = pyreadstat.read_sav(fix + "/survey.sav", user_missing=True,
                           disable_datetime_conversion=True, output_format="dict")
md = {k.decode(): v.decode() for k, v in pq.read_table(stem + ".parquet").schema.metadata.items()}
schema = json.loads(md["parqit.schema"])
chars = json.loads(md["parqit.chars"])
vallabs = json.loads(md["parqit.vallabs"])
stata = {v["src"]: v["name"] for v in schema["vars"]}

# ---- the Parquet file's own metadata against the SPSS dictionary ----------
check("dataset label", json.loads(md["parqit.dtalabel"]), m.file_label)
check("variables in manifest order", [v["src"] for v in schema["vars"]], m.column_names)
for v in schema["vars"]:
    check(f"variable label of {v['src']}", v["varlab"],
          m.column_names_to_labels.get(v["src"]) or "")
check("renamed: long name", stata["Satisfaction_with_public_services_overall"],
      "Satisfaction_with_public_service")
check("renamed: name with a period", stata["q.3"], "q_3")
notes = [chars["_dta"].get(f"note{k}") for k in range(1, len(m.notes) + 1)]
check("documents -> notes", notes, m.notes)
check("note count", chars["_dta"].get("note0"), str(len(m.notes)))
for name in m.column_names:
    c = chars.get(stata[name], {})
    check(f"{name} spss_format", c.get("spss_format"), m.original_variable_types[name])
    meas = m.variable_measure.get(name)
    check(f"{name} spss_measure", c.get("spss_measure"), None if meas == "unknown" else meas)
    check(f"{name} spss_display_width", c.get("spss_display_width"),
          str(m.variable_display_width[name]))

def syntax(spec):
    parts = []
    for r in spec:
        lo, hi = r["lo"], r["hi"]
        if isinstance(lo, str):
            parts.append('"' + lo + '"')
            continue
        fmt = lambda x: str(int(x)) if float(x).is_integer() else repr(float(x))
        if lo == hi:
            parts.append(fmt(lo))
        else:
            parts.append(("LO" if math.isinf(lo) else fmt(lo)) + " THRU " +
                         ("HI" if math.isinf(hi) else fmt(hi)))
    ranges = [p for p in parts if "THRU" in p]
    return ", ".join(ranges + [p for p in parts if "THRU" not in p])

for name, spec in m.missing_ranges.items():
    check(f"{name} spss_missing", chars[stata[name]].get("spss_missing"), syntax(spec))

# codes: dictionary values first (discrete ascending, then labelled values
# inside the range ascending), then values observed in the range ascending
check("q1 map", chars["q1"]["spss_missing_map"], ".a=97 .b=98 .c=99")
check("q2 map", chars["q2"]["spss_missing_map"], ".a=99 .b=-9 .c=-8 .d=-2 .e=-1")
seen_income = sorted({x for x in d["income"] if x is not None and x <= 0})
check("income map", chars["income"]["spss_missing_map"],
      " ".join(f".{chr(97 + k)}={x:g}" for k, x in enumerate(seen_income)))

# value labels: integer keys natively (+ the codes of labelled missing
# values); the complete set in the char where Stata cannot hold all of it
for name, labs in m.variable_value_labels.items():
    st = stata[name]
    full = [[k, v] for k, v in labs.items()]
    native = vallabs.get(st, {}).get("entries", [])
    if name in ("score", "sex"):
        got = json.loads(chars[st]["spss_value_labels"])
        check(f"{name} spss_value_labels", [[float(k) if not isinstance(k, str) else k, v]
                                           for k, v in got],
              [[float(k) if not isinstance(k, str) else k, v] for k, v in full])
    else:
        check(f"{name} has no spss_value_labels", "spss_value_labels" in chars.get(st, {}), False)
    ints = {str(int(k)): v for k, v in labs.items()
            if not isinstance(k, str) and float(k).is_integer()}
    got_ints = {k: v for k, v in native if not k.startswith(".")}
    check(f"{name} native integer labels", got_ints, ints)

# ---- Stata, after parqit use ---------------------------------------------
long_label = m.column_names_to_labels["Satisfaction_with_public_services_overall"]
st = "Satisfaction_with_public_service"
check("Stata label is Stata's first 80 characters", Data.getVarLabel(st), long_label[:80])
check("the full label in char spss_label", Characteristic.getVariableChar(st, "spss_label"),
      long_label)
check("src_name of the long name", Characteristic.getVariableChar(st, "src_name"),
      "Satisfaction_with_public_services_overall")
check("src_name of q.3", Characteristic.getVariableChar("q_3", "src_name"), "q.3")
check("format of income", Data.getVarFormat("income"), "%12.2fc")
check("format of bday", Data.getVarFormat("bday"), "%tdDD-Mon-CCYY")
check("format of visit", Data.getVarFormat("visit"), "%tdNN/DD/CCYY")
check("format of stamp", Data.getVarFormat("stamp"), "%tcDD-Mon-CCYY_HH:MM:SS")
check("format of clock", Data.getVarFormat("clock"), "%tcHH:MM:SS")
check("format of q1", Data.getVarFormat("q1"), "%2.0f")
check("note 2 in Stata", Characteristic.getDtaChar("note2"), m.notes[1])

labels = {}
with open(stem + "_labels.txt", encoding="utf-8") as fh:
    for line in fh:
        lab, val, txt = line.rstrip("\n").split("\t", 2)
        labels.setdefault(lab, {})[val] = txt
check("q1 labels in Stata", labels.get("q1"),
      {"1": "Very poor", "2": "Poor", "3": "Fair", "4": "Good", "5": "Very good",
       "97": "Not applicable", "98": "Refused", "99": "Don't know",
       ".a": "Not applicable", ".b": "Refused", ".c": "Don't know"})
check("q2 labels in Stata", labels.get("q2"),
      {"-9": "Refused", "-8": "Don't know", "-2": "Not asked", "1": "Low", "2": "Medium",
       "3": "High", ".b": "Refused", ".c": "Don't know", ".d": "Not asked"})
check("score keeps its integer key only", labels.get("score"), {"1": "one"})
check("no Stata label on a string", "sex" in labels, False)
check("value label attached to q1", ValueLabel.getVarValueLabel("q1"), "q1")
check("value label attached to q2", ValueLabel.getVarValueLabel("q2"), "q2")

# every user-missing cell is its extended missing code in Stata
X0 = math.ldexp(1.0, 1023)
def code_of(x):
    if x is None or x < X0:
        return 0
    return round((x / X0 - 1) * 4096)
maps = {"q1": {97: 1, 98: 2, 99: 3}, "q2": {99: 1, -9: 2, -8: 3, -2: 4, -1: 5},
        "income": {x: k + 1 for k, x in enumerate(seen_income)}}
for name, mp in maps.items():
    col = Data.get(name)
    for i, (x, r) in enumerate(zip(col, d[name])):
        xv = x[0] if isinstance(x, list) else x
        want = mp.get(r, 0) if r is not None else 0
        if r is None:
            ok = xv is not None and xv >= X0 and code_of(xv) == 0
        elif want:
            ok = code_of(xv) == want
        else:
            ok = xv == r
        if not ok:
            bad(f"{name} case {i + 1}: Stata holds {xv!r}, SPSS {r!r} (code {want})")

print("python oracle failures:", fails)
Macro.setLocal("pyfails", str(fails))
end
local fails = `fails' + `pyfails'

if (`fails') di "VERDICT(V128_SPSS_METADATA): FAIL - `fails' checks"
else di "VERDICT(V128_SPSS_METADATA): PASS"
