* V132 — DESCRIBE-META-1: parqit describe <file> reports the file's variable
* labels, value labels, notes and the SPSS labels kept in characteristics, from
* the footer, and lists them on request. Oracle: pyarrow reading the parqit.*
* footer keys directly (not through the plugin or the ado). Also checks a label
* hostile to macros and SMCL, a file without parqit metadata, and the refusal
* of the options on the view form.
clear all
set more off
set varabbrev off
set linesize 200
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile stem
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
    di "VERDICT(V132_DESCRIBE_METADATA): FAIL - pyarrow not importable from Stata's Python (see python query)"
    exit
}

* a Stata dataset with labels and notes; one variable label hostile to macros/SMCL
sysuse auto, clear
notes: a dataset note
notes price: price in {bf:dollars} with a "quote"
notes price: second note
mata: st_varlabel("make", "Make " + char(96) + "q" + char(39) + " {bf:x} " + char(34) + "y" + char(34) + " ç")
parqit save `"`stem'_a.parquet"', data replace
* the SPSS fixture: string and non-integer labels kept in characteristics
parqit save `"`stem'_s.parquet"' using `"`repo'/tests/fixtures/spss/survey.sav"', replace
* a file with no parqit metadata at all
python:
import pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
pq.write_table(pa.table({"x": [1, 2], "s": ["a", "b"]}), Macro.getLocal("stem") + "_p.parquet")
end

python:
import json, re
import pyarrow.parquet as pq
from sfi import Macro, Scalar

def v132_check(f):
    base = Macro.getLocal("stem") + "_" + f
    md = pq.ParquetFile(base + ".parquet").schema_arrow.metadata or {}
    meta = lambda k, d: json.loads(md[k]) if k in md else d
    vars_ = meta(b"parqit.schema", {"vars": []})["vars"]
    vallabs = meta(b"parqit.vallabs", {})
    chars = meta(b"parqit.chars", {})
    dtalabel = meta(b"parqit.dtalabel", "")
    bad = []
    names = [v["name"] for v in vars_] or ["x", "s"]
    for i, v in enumerate(vars_, 1):
        if Macro.getGlobal("r(varlab_%d)" % i) != v.get("varlab", ""): bad.append("varlab %d" % i)
        if Macro.getGlobal("r(vallab_%d)" % i) != v.get("vallab", ""): bad.append("vallab %d" % i)
    notes = {t: {k: x for k, x in c.items() if re.fullmatch(r"note[1-9][0-9]*", k) and x}
             for t, c in chars.items()}
    if Scalar.getValue("r(n_value_labels)") != len(vallabs): bad.append("n_value_labels")
    if Scalar.getValue("r(n_notes)") != sum(len(n) for n in notes.values()): bad.append("n_notes")
    spss = [n for n in names if "spss_value_labels" in chars.get(n, {})]
    if Macro.getGlobal("r(spss_labels)").split() != spss: bad.append("spss_labels")
    if Macro.getGlobal("r(label)") != dtalabel: bad.append("label")
    # the listing: every label, SPSS label and note text appears as written
    log = open(base + ".log", encoding="utf-8").read()
    texts = [t for s in vallabs.values() for _, t in s["entries"]]
    texts += [t for n in spss for _, t in json.loads(chars[n]["spss_value_labels"])]
    texts += [x for n in notes.values() for x in n.values()]
    bad += ["listed: " + t for t in texts if t not in log]
    # the table's markers
    if any(n for n in notes.get("_dta", {})) != ("(_dta has notes)" in log): bad.append("dta marker")
    for n in names:
        row = [l for l in log.splitlines() if l.startswith("  " + n + " ")]
        if not row: bad.append("row " + n); continue
        if bool(notes.get(n)) != ("*" in row[0].split()[2]): bad.append("note marker " + n)
        if (n in spss and not any(v.get("vallab") for v in vars_ if v["name"] == n)) != ("(spss)" in row[0]):
            bad.append("spss marker " + n)
    if f == "p" and ("labels:" in log or "notes:" in log): bad.append("closing lines without metadata")
    print("V132", f, "mismatches:", bad)
    Macro.setLocal("pyfails", str(len(bad)))

end

foreach f in a s p {
    capture log close v132
    log using `"`stem'_`f'.log"', text replace name(v132)
    parqit describe `"`stem'_`f'.parquet"', labels notes
    * log is r-class: keep describe's results across log close
    _return hold v132r
    log close v132
    _return restore v132r
    python: v132_check("`f'")
    local fails = `fails' + `pyfails'
}

* a one-column file (Mata's 1x1 row/column case), with and without a note
clear
set obs 3
gen long rid = _n
parqit save `"`stem'_one.parquet"', data replace
capture noisily parqit describe `"`stem'_one.parquet"', labels notes
if (_rc | r(n_cols) != 1 | r(n_notes) != 0 | `"`r(spss_labels)'"' != "") {
    di as err "one-column file: rc " _rc
    local ++fails
}
notes rid: the only note
parqit save `"`stem'_one.parquet"', data replace
capture noisily parqit describe `"`stem'_one.parquet"', notes
if (_rc | r(n_notes) != 1) {
    di as err "one-column file with a note: rc " _rc
    local ++fails
}

* the hostile variable label arrives intact in r()
parqit describe `"`stem'_a.parquet"'
mata: st_local("ok", strofreal(st_global("r(varlab_1)") == "Make " + char(96) + "q" + char(39) + " {bf:x} " + char(34) + "y" + char(34) + " ç"))
if (!`ok') {
    di as err "hostile variable label changed in r(varlab_1)"
    local ++fails
}

* the options belong to the file form
parqit use using `"`stem'_a.parquet"'
capture noisily parqit describe, labels
if (_rc != 198) {
    di as err "view form accepted labels: rc " _rc
    local ++fails
}

if (`fails') di "VERDICT(V132_DESCRIBE_METADATA): FAIL - `fails' checks"
else di "VERDICT(V132_DESCRIBE_METADATA): PASS"
