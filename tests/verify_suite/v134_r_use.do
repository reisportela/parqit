* V134 — R-READ-1 (reading R data into Stata): `parqit use using <x.rds>`
* loads the data frame R holds — every cell against the oracle R wrote
* (tests/fixtures/r/expected.json), read back with Stata's own Python API —
* with its metadata applied in memory (variable and value labels, extended
* missing values .a-.z from haven tagged NAs and SPSS user-missing values,
* display formats, notes, characteristics, dataset label). Also: integer64
* beyond 2^53 is refused unless int64() says how to load it; object() picks
* an .RData object for use and for a lazy view; merge, append, mergein and
* appendin take an R file on the using side; parqit spssencode turns haven's
* labels of a character vector into a labelled numeric variable; parqit
* describe marks R labels; copysource after an R read refuses without naming
* the temporary bridge; encoding() is validated.
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

* ---- every cell of frame.rds in memory, against R -------------------------------
capture noisily parqit use using `"`fix'/frame.rds"', clear int64(string)
if (_rc) {
    di as err "parqit use frame.rds failed: rc=" _rc
    local ++fails
}
else {
    python:
from sfi import Characteristic, Data, Macro, Missing
import json
import math
import decimal

fix = Macro.getLocal("fix")
exp = json.load(open(fix + "/expected.json", encoding="utf-8"))["frame.rds"]
fails = 0

def bad(msg):
    global fails
    fails += 1
    print("FAIL:", msg)

def half_away(x):
    return int(decimal.Decimal(x).to_integral_value(rounding=decimal.ROUND_HALF_UP))

def us_of_seconds(s):
    w = math.floor(s)
    return int(w) * 1000000 + half_away((s - w) * 1e6)

DOT = Missing.getValue(".")
def xm(letter):
    return Missing.getValue("." + letter)

names = [Data.getVarName(j) for j in range(Data.getVarCount())]
want = [c["name"][0] for c in exp["columns"]]
if names != want:
    bad(f"variables {names} != R's {want}")
n = Data.getObsTotal()
if n != exp["nrow"][0]:
    bad(f"{n} observations, R has {exp['nrow'][0]}")

for c in exp["columns"]:
    name = c["name"][0]
    if name not in names:
        continue
    cls = c["class"]
    code_of = {}
    for part in (Characteristic.getVariableChar(name, "r_missing_map") or "").split(" "):
        if "=" in part:
            code, val = part.split("=", 1)
            code_of[float(val)] = code[1]
    for i, e in enumerate(c["values"]):
        got = Data.getAt(name, i)
        w = f"{name} obs {i + 1}"
        if "integer64" in cls or c["typeof"][0] == "character":
            want_v = "" if e is None else e
        elif c["typeof"][0] == "logical":
            want_v = DOT if e is None else float(e)
        elif e is None or e == "NaN" or e in ("Inf", "-Inf"):
            want_v = DOT
        elif isinstance(e, str) and e.startswith("NA("):
            want_v = xm(e[3])
        elif "Date" in cls:
            want_v = float(e) + 3653
        elif "POSIXct" in cls:
            want_v = float(us_of_seconds(float(e)) // 1000 + 315619200000)
        elif "hms" in cls:
            want_v = float(us_of_seconds(float(e)) // 1000)
        else:
            x = float(e)
            want_v = xm(code_of[x]) if x in code_of else x
        if isinstance(want_v, str):
            if got != want_v:
                bad(f"{w}: {got!r} != {want_v!r}")
        elif got != want_v:
            bad(f"{w}: {got!r} != {want_v!r} (R {e!r})")
Macro.setLocal("pyfails", str(fails))
end
    local fails = `fails' + `pyfails'

    * metadata in memory
    if (`"`: data label'"' != "Fixture data frame") local ++fails
    if (`"`: variable label lab'"' != "Question one") local ++fails
    if (`"`: value label lab'"' != "lab" | `"`: value label f'"' != "f") local ++fails
    if (`"`: label lab .a'"' != "Skipped" | `"`: label lab .b'"' != "Refused" | ///
        `"`: label lab .c'"' != "Unknown" | `"`: label lab 1'"' != "Yes") {
        di as err "lab value labels"
        local ++fails
    }
    if (`"`: label f 3'"' != "hi" | `"`: label o 2'"' != "b") local ++fails
    if ("`: format stata_fmt'" != "%9.2f" | "`: format dt'" != "%td" | "`: format ct'" != "%tc" | ///
        "`: format hm'" != "%tcHH:MM:SS" | "`: format idate'" != "%td") {
        di as err "display formats"
        local ++fails
    }
    local full : char stata_fmt[r_label]
    if (ustrlen(`"`full'"') <= 80 | `"`: variable label stata_fmt'"' != usubstr(`"`full'"', 1, 80)) {
        di as err "a label longer than 80 characters: truncated in memory, whole in char r_label"
        local ++fails
    }
    if (`"`: char i[note1]'"' != "first note on i" | `"`: char _dta[note1]'"' != "a note on the data frame") local ++fails
    if (`"`: char ct[r_tzone]'"' != "Europe/Lisbon" | `"`: char dtm[r_units]'"' != "days") local ++fails
    if (`"`: char lab[r_missing_map]'"' != ".b=98 .c=99") local ++fails
    if ("`: type i'" != "double" | "`: type l'" != "byte" | "`: type f'" != "byte" | "`: type dt'" != "long") {
        di as err "storage types"
        local ++fails
    }

    * haven labels of a character vector -> a labelled numeric variable
    capture noisily parqit spssencode sex, generate(sex_n)
    if (_rc) local ++fails
    else {
        if ("`r(mode)'" != "sequential") local ++fails
        if (sex_n[1] != 2 | sex_n[2] != 1 | sex_n[3] != .a | sex_n[4] != . | sex_n[5] != 1) {
            di as err "spssencode sex: the codes"
            local ++fails
        }
        if (`"`: label sex_n 1'"' != "Female" | `"`: label sex_n 2'"' != "Male" | ///
            `"`: label sex_n .a'"' != "Not stated") local ++fails
    }
}

* integer64 beyond 2^53 is refused unless int64() says how to load it
capture noisily parqit use using `"`fix'/frame.rds"', clear
if (_rc != 198) {
    di as err "integer64 beyond 2^53 without int64(): expected rc 198, got " _rc
    local ++fails
}

* ---- which object; a lazy view over an .RData object ---------------------------------
capture noisily parqit use using `"`fix'/workspace.RData"', clear object(towns)
if (_rc | "`r(r_object)'" != "towns") local ++fails
else if (_N != 2 | town[1] != "Porto" | pop[2] != 193300) local ++fails
capture noisily parqit use using `"`fix'/workspace.RData"', clear
if (_rc != 198) local ++fails
capture noisily parqit use using `"`fix'/workspace.RData"', name(w) object(people)
if (_rc | "`r(r_object)'" != "people") local ++fails
else {
    capture noisily parqit view w: collect, clear
    if (_rc | _N != 3 | name[3] != "Eva" | id[1] != 101) local ++fails
}
capture noisily parqit use using `"`fix'/rownames.rds"', clear object(x)
if (_rc != 198) local ++fails
capture noisily parqit use using `"`fix'/rownames.rds"', clear encoding(klingon)
if (_rc != 198) local ++fails

* ---- the R file on the using side ---------------------------------------------------
capture noisily parqit use using `"`fix'/rownames.rds"', clear
if (_rc) local ++fails
else {
    tempfile wt
    keep rowname wt
    quietly save `"`wt'"'
}
capture noisily parqit use using `"`fix'/rownames.rds"', name(m)
capture noisily parqit keep rowname mpg
capture noisily parqit merge 1:1 rowname using `"`fix'/rownames.rds"', keepusing(wt)
capture noisily parqit collect, clear
if (_rc | _N != 6) local ++fails
else {
    rename wt wt_merged
    capture noisily merge 1:1 rowname using `"`wt'"', assert(match) nogenerate
    if (_rc) local ++fails
    else capture assert wt == wt_merged
    if (_rc) {
        di as err "parqit merge with an R using side"
        local ++fails
    }
}
capture noisily parqit use using `"`fix'/rownames.rds"', clear
keep rowname mpg
capture noisily parqit mergein 1:1 rowname using `"`fix'/rownames.rds"', keepusing(wt) nogenerate
if (_rc) local ++fails
else {
    capture noisily merge 1:1 rowname using `"`wt'"', assert(match) nogenerate
    if (_rc) local ++fails
}
capture noisily parqit use using `"`fix'/rownames.rds"', clear
capture noisily parqit appendin using `"`fix'/rownames.rds"'
if (_rc | _N != 12) local ++fails
capture noisily parqit use using `"`fix'/rownames.rds"', name(a)
capture noisily parqit append using `"`fix'/rownames.rds"'
capture noisily parqit collect, clear
if (_rc | _N != 12) local ++fails

* ---- edge names, deferred text and the empty frame -------------------------------------
capture noisily parqit use using `"`fix'/edge.rds"', clear
if (_rc) local ++fails
else {
    unab vars : _all
    if ("`vars'" != "x x_1 V3 V4 X seq ds dr") {
        di as err "edge variables: `vars'"
        local ++fails
    }
    if (`"`: char x_1[r_name]'"' != "x" | ds[1] != "10" | ds[2] != "" | dr[3] != 100000 | seq[3] != 3) local ++fails
}
capture noisily parqit use using `"`fix'/empty.rds"', clear
if (_rc | _N != 0 | c(k) != 2) local ++fails
capture noisily parqit use using `"`fix'/latin1.rds"', clear
if (_rc | s[1] != "caf" + uchar(233) | s[3] != "na" + uchar(239) + "ve") local ++fails

* ---- describe: R labels in a converted file; an R file itself is refused ----------------
capture noisily parqit save `"`stem'_conv.parquet"' using `"`fix'/frame.rds"', replace
capture noisily parqit describe `"`stem'_conv.parquet"', labels
if (_rc | "`r(r_labels)'" != "score sex") {
    di as err "describe r(r_labels) = `r(r_labels)'"
    local ++fails
}
capture noisily parqit describe `"`fix'/frame.rds"'
if (_rc != 198) local ++fails

* ---- copysource after an R read: refused, and not about a temporary file -----------------
capture noisily parqit use using `"`fix'/rownames.rds"', clear
if (`"`: char _dta[_parqit_fast_source_nonce]'"' != "") {
    di as err "an R bridge was registered as a copysource source"
    local ++fails
}
capture noisily parqit save `"`stem'_cs.parquet"', copysource replace
if (_rc != 198) local ++fails

if (`fails' == 0) di "VERDICT(V134_R_USE): PASS"
else di "VERDICT(V134_R_USE): FAIL - `fails' check(s) failed"
