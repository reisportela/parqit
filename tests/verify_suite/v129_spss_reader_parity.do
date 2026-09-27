* V129 — SPSS-READ-1 (parity with other readers): `parqit use x.sav, clear`
* gives the dataset Stata's own `import spss` gives, once the differences the
* parqit help documents are applied to parqit's side — user-missing values are
* .a-.z where import spss has . (both are missing), dates are %td days where
* import spss has %tc milliseconds, a time of day is milliseconds since
* 01jan1960 where import spss counts from 14oct1582, and Stata-invalid names
* are sanitised differently (matched here through each reader's record of the
* SPSS name). Second oracle, independent of ReadStat: R's foreign::read.spss
* (derived from PSPP) for every numeric value, user-missing codes included
* (skipped with a note when R or foreign is not installed).
* Needs pyarrow importable from Stata's Python; fixtures in tests/fixtures/spss.
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile stem ref
local fix `"`repo'/tests/fixtures/spss"'
local fails 0

foreach f in survey.sav survey_bc.sav survey.zsav {
    local zsav = cond(substr("`f'", -4, .) == "zsav", "zsav", "")
    * (import spss does not accept compound quotes around its file name)
    quietly import spss using "`fix'/`f'", clear `zsav'
    * import spss: rename each variable to parqit's name for the same SPSS name
    foreach v of varlist _all {
        local sp : char `v'[spss_variable_name]
        if (`"`sp'"' == "Satisfaction_with_public_services_overall") rename `v' Satisfaction_with_public_service
        else if (`"`sp'"' == "q.3") rename `v' q_3
    }
    * dates: import spss holds %tc ms; the time of day counts from 14oct1582
    foreach v in bday visit {
        replace `v' = dofc(`v')
    }
    replace clock = clock - tc(14oct1582 00:00:00)
    quietly save `"`ref'"', replace

    parqit use `"`fix'/`f'"', clear
    * documented differences: SPSS user-missing values are .a-.z here, . there;
    * a string user-missing value is kept as text here, blanked there
    quietly ds, has(type numeric)
    foreach v in `r(varlist)' {
        quietly replace `v' = . if `v' > .
    }
    local sm : char sex[spss_missing]
    if (`"`sm'"' != `""X""') local ++fails
    quietly replace sex = "" if sex == "X"
    capture noisily cf _all using `"`ref'"', all
    if (_rc) {
        di as err "`f': parqit use differs from import spss"
        local ++fails
    }
}

* R foreign (PSPP-derived): every numeric value of the uncompressed and the
* bytecode file, user-missing values as values (use.missings = FALSE)
parqit save `"`stem'.parquet"' using `"`fix'/survey.sav"', replace
python:
from sfi import Macro
import json
import math
import os
import shutil
import subprocess
import pyarrow.parquet as pq

stem = Macro.getLocal("stem")
fix = Macro.getLocal("fix")
fails = 0
rscript = shutil.which("Rscript")
code = r'''
x <- suppressWarnings(foreign::read.spss(commandArgs(TRUE)[1], to.data.frame = FALSE,
                      use.value.labels = FALSE, use.missings = FALSE, reencode = "UTF-8"))
num <- names(x)[sapply(x, is.numeric)]
out <- file(commandArgs(TRUE)[2], "w")
for (v in num) writeLines(paste(c(v, ifelse(is.na(x[[v]]), "NA", sprintf("%.17g", x[[v]]))),
                                collapse = "\t"), out)
close(out)
'''
if rscript is None:
    print("note: Rscript not found; the R foreign comparison is skipped")
else:
    with open(stem + "_r.R", "w") as fh:
        fh.write(code)
    t = pq.read_table(stem + ".parquet")
    md = {k.decode(): v.decode() for k, v in t.schema.metadata.items()}
    chars = json.loads(md["parqit.chars"])
    schema = json.loads(md["parqit.schema"])
    xm = json.loads(md.get("parqit.xmissing", "{}"))
    stata = {v["src"]: v["name"] for v in schema["vars"]}
    for sav in ("survey.sav", "survey_bc.sav"):
        res = subprocess.run([rscript, stem + "_r.R", fix + "/" + sav, stem + "_r.tsv"],
                             capture_output=True, text=True)
        if res.returncode != 0:
            if "there is no package called" in res.stderr:
                print("note: R package foreign not installed; the R comparison is skipped")
                break
            print("FAIL: Rscript failed:", res.stderr[-500:])
            fails += 1
            continue
        ncols = 0
        with open(stem + "_r.tsv", encoding="utf-8") as fh:
            for line in fh:
                parts = line.rstrip("\n").split("\t")
                name, vals = parts[0], parts[1:]
                if name not in t.schema.names:
                    continue
                ncols += 1
                typ = str(t.schema.field(name).type)
                got = t.column(name).to_pylist()
                comp = t.column(xm[name]).to_pylist() if name in xm else [0] * len(got)
                codes = {}
                for part in chars.get(stata[name], {}).get("spss_missing_map", "").split(" "):
                    if "=" in part:
                        c, v = part.split("=", 1)
                        codes[ord(c[1]) - 96] = float(v)
                for i, (g, r, c) in enumerate(zip(got, vals, comp)):
                    if r == "NA":
                        ok = g is None and c == 0
                    elif c:
                        ok = g is None and codes.get(c) == float(r)
                    elif typ == "double":
                        ok = g == float(r)
                    elif typ == "date32[day]":
                        ok = g is not None and ((g.toordinal() - 719163 + 141428) * 86400 ==
                                                float(r))
                    else:
                        ok = True  # date-times and times: covered by V127
                    if not ok:
                        fails += 1
                        print(f"FAIL: R foreign {sav} {name} case {i + 1}: R {r}, parqit {g!r} (code {c})")
        print(f"R foreign ({sav}): {ncols} numeric columns compared")
        if ncols < 10:
            fails += 1
            print(f"FAIL: R foreign returned only {ncols} numeric columns for {sav}")
print("python oracle failures:", fails)
Macro.setLocal("pyfails", str(fails))
end
local fails = `fails' + `pyfails'

if (`fails') di "VERDICT(V129_SPSS_READER_PARITY): FAIL - `fails' checks"
else di "VERDICT(V129_SPSS_READER_PARITY): PASS"
