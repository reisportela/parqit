* V131 — SPSS-ENCODE-1: parqit spssencode makes the labelled numeric version of
* an SPSS string variable from char var[spss_value_labels]. Oracles independent
* of the command's Mata reader: pyreadstat on the SPSS fixture (codes, labels,
* user-missing definition) and Python's json on characteristics written from
* Python with adversarial label texts. Checks both numberings, the user-missing
* codes, the storage type, and that every refusal leaves nothing behind.
* Needs pyreadstat importable from Stata's Python (`python query`).
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
local fix `"`repo'/tests/fixtures/spss"'
local fails 0

python:
from sfi import Macro
try:
    import pyreadstat
    Macro.setLocal("have_oracle", "1")
except Exception as e:
    print("oracle unavailable:", e)
    Macro.setLocal("have_oracle", "0")
end
if ("`have_oracle'" != "1") {
    di "VERDICT(V131_SPSS_ENCODE): FAIL - pyreadstat not importable from Stata's Python (see python query)"
    exit
}

* --- A. the fixture's letter codes: sequential numbering, X user-missing ------
parqit use using `"`fix'/survey.sav"', clear
parqit spssencode sex, generate(sex_num)
if ("`r(mode)'" != "sequential" | `"`r(missing_map)'"' != `".a="X""' | ///
    r(N_labels) != 3 | r(N_unlabeled) != 0 | "`r(label)'" != "sex_num") {
    di as err "A: returned results"
    local ++fails
}
if (`"`: variable label sex_num'"' != `"`: variable label sex'"' | ///
    `"`: char sex_num[spss_missing_map]'"' != `".a="X""') {
    di as err "A: variable label or map characteristic"
    local ++fails
}
gen str3 got = string(sex_num)
python:
import pyreadstat
from sfi import Data, Macro, ValueLabel
_, meta = pyreadstat.read_sav(Macro.getLocal("fix") + "/survey.sav", user_missing=True,
                              output_format="dict", metadataonly=True)
labels = meta.variable_value_labels["sex"]
missing = sorted(r["lo"] for r in meta.missing_ranges["sex"] if r["lo"] == r["hi"])
codes = sorted(k for k in labels if k not in missing)
expect = {k: str(i) for i, k in enumerate(codes, 1)}
expect.update({m: "." + chr(97 + i) for i, m in enumerate(missing)})
expect[""] = "."
bad = sum(expect.get(s) != g for s, g in zip(Data.get("sex"), Data.get("got")))
bad += sum(ValueLabel.getLabel("sex_num", i) != labels[k] for i, k in enumerate(codes, 1))
Macro.setLocal("pyfails", str(bad))
Macro.setLocal("xlabel", labels[missing[0]])
end
if (`pyfails' | `"`: label (sex_num) .a'"' != `"`xlabel'"') {
    di as err "A: `pyfails' cells/labels differ from pyreadstat"
    local ++fails
}

* --- B. integer codes and adversarial label texts written from Python --------
clear
input str3 code
"1"
"2"
"02"
"3"
"9"
""
"10"
"7"
end
label variable code "a code"
python:
import json, re
from sfi import Characteristic, Macro
pairs = [["1", 'quote " and back\\slash'], ["2", "line\nbreak\ttab\u0001ctl"],
         ["3", "comma, [brackets] and {braces}"], ["9", "ç and 😀"], ["10", ""]]
text = json.dumps(pairs, ensure_ascii=False, separators=(",", ":"))
# the escaped form of ["9", ...]: uppercase \u escapes and a surrogate pair
esc = json.dumps(pairs[3][1], ensure_ascii=True)
esc = re.sub(r"\\u([0-9a-f]{4})", lambda m: "\\u" + m.group(1).upper(), esc)
text = text.replace(json.dumps(pairs[3][1], ensure_ascii=False), esc)
assert json.loads(text) == pairs
Characteristic.setVariableChar("code", "spss_value_labels", text)
Characteristic.setVariableChar("code", "spss_missing", '"9"')
Macro.setLocal("json_ok", "1")
end
parqit spssencode code, generate(code_num)
if ("`r(mode)'" != "codes" | r(N_labels) != 4 | r(N_unlabeled) != 1 | ///
    `"`r(missing_map)'"' != `".a="9""' | "`: type code_num'" != "byte") {
    di as err "B: returned results or type"
    local ++fails
}
gen str3 got = string(code_num)
python:
import json
from sfi import Characteristic, Data, Macro, ValueLabel
pairs = json.loads(Characteristic.getVariableChar("code", "spss_value_labels"))
expect = {"1": "1", "2": "2", "02": "2", "3": "3", "9": ".a", "": ".", "10": "10", "7": "7"}
bad = sum(expect[s] != g for s, g in zip(Data.get("code"), Data.get("got")))
bad += sum(ValueLabel.getLabel("code_num", int(k)) != t for k, t in pairs if t and k != "9")
bad += ValueLabel.getLabel("code_num", 10) != ""
Macro.setLocal("pyfails", str(bad))
Macro.setLocal("nine", dict(pairs)["9"])
end
if (`pyfails' | `"`: label (code_num) .a'"' != `"`nine'"') {
    di as err "B: `pyfails' cells/labels differ from json.loads"
    local ++fails
}

* --- C. the same dictionary numbered sequentially ----------------------------
parqit spssencode code, generate(code_seq) label(seq_lab) sequential
if ("`r(mode)'" != "sequential" | "`r(label)'" != "seq_lab" | r(N_unlabeled) != 2) {
    di as err "C: returned results"
    local ++fails
}
gen str3 gots = string(code_seq)
python:
from sfi import Data, Macro, ValueLabel
keys = sorted(["1", "2", "3", "10"])              # the dictionary, 9 is user-missing
extra = sorted({"02", "7"})                        # values the dictionary lacks
num = {k: str(i) for i, k in enumerate(keys + extra, 1)}
num.update({"9": ".a", "": "."})
bad = sum(num[s] != g for s, g in zip(Data.get("code"), Data.get("gots")))
bad += ValueLabel.getLabel("seq_lab", int(num["02"])) != "02"
bad += ValueLabel.getLabel("seq_lab", int(num["10"])) != "10"   # an empty SPSS label keeps the code
Macro.setLocal("pyfails", str(bad))
end
if (`pyfails') {
    di as err "C: `pyfails' cells/labels differ from the expected numbering"
    local ++fails
}

* --- E. one observation and one-pair dictionaries (Mata's 1x1 row/column case)
preserve
clear
set obs 1
gen str2 c = "7"
char c[spss_value_labels] `"[["7","seven"]]"'
capture noisily parqit spssencode c, generate(e_num)
if (_rc | e_num[1] != 7 | `"`: label (e_num) 7'"' != "seven") {
    di as err "E: one integer code"
    local ++fails
}
replace c = "A"
char c[spss_value_labels] `"[["A","letter a"]]"'
capture noisily parqit spssencode c, generate(e_seq)
if (_rc | e_seq[1] != 1 | `"`: label (e_seq) 1'"' != "letter a") {
    di as err "E: one letter code"
    local ++fails
}
replace c = "9"
char c[spss_value_labels] `"[["9","missing nine"]]"'
char c[spss_missing] `""9""'
capture noisily parqit spssencode c, generate(e_mis)
if (_rc | e_mis[1] != .a | `"`: label (e_mis) .a'"' != "missing nine") {
    di as err "E: one user-missing code"
    local ++fails
}
restore

* --- D. refusals leave no variable and no label behind -----------------------
capture program drop _v131_refused
program define _v131_refused
    args want msg
    local got = _rc
    capture confirm variable zz
    local var = (_rc == 0)
    mata: st_local("lab", strofreal(st_vlexists("zz")))
    if (`got' != `want' | `var' | `lab') {
        di as err "D: `msg': rc `got' (wanted `want'), variable `var', label `lab'"
        global V131_FAILS = ${V131_FAILS} + 1
    }
end
global V131_FAILS 0
capture noisily parqit spssencode code, generate(zz) label(seq_lab)
_v131_refused 110 "existing label"
capture noisily parqit spssencode code, generate(code_num)
if (_rc != 110) {
    di as err "D: existing variable accepted"
    local ++fails
}
replace code = "ZZ" in 8
capture noisily parqit spssencode code, generate(zz)
_v131_refused 198 "value that is not an integer code"
replace code = "7" in 8
python:
from sfi import Characteristic
full = Characteristic.getVariableChar("code", "spss_value_labels")
bad_texts = {"truncated": full[:-5], "numeric code": '[[1,"one"]]', "trailing text": full + "x",
             "raw control": '[["1","a' + "\t" + 'b"]]', "nul": '[["1","a\\u0000b"]]',
             "lone surrogate": '[["1","\\ud83d"]]', "unknown escape": '[["1","\\q"]]'}
end
foreach t in truncated "numeric code" "trailing text" "raw control" nul "lone surrogate" "unknown escape" {
    python: Characteristic.setVariableChar("code", "spss_value_labels", bad_texts["`t'"])
    capture noisily parqit spssencode code, generate(zz)
    _v131_refused 198 "`t'"
}
char code[spss_value_labels]
capture noisily parqit spssencode code, generate(zz)
_v131_refused 198 "no characteristic"
sysuse auto, clear
capture noisily parqit spssencode price, generate(zz)
_v131_refused 109 "numeric variable"
local fails = `fails' + ${V131_FAILS}

if (`fails') di "VERDICT(V131_SPSS_ENCODE): FAIL - `fails' checks"
else di "VERDICT(V131_SPSS_ENCODE): PASS"
