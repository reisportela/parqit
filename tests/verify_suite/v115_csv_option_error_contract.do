* CA-06: incompatible delimiter/null markers give an actionable, atomic error.
clear all
set more off
set linesize 200
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile base
local fails 0
input long id str3 s
1 "abc"
2 "def"
end
export delimited using `"`base'.csv"', delimiter(";")
clear
set obs 1
gen sentinel = 987
log using `"`base'.txt"', text name(v115)
foreach mode in eager lazy {
    if ("`mode'" == "eager") capture noisily parqit use `"`base'.csv"', clear csv(delim(";") nullstr("a;b"))
    if ("`mode'" == "lazy") capture noisily parqit use using `"`base'.csv"', csv(delim(";") nullstr("a;b"))
    local rc = _rc
    if (`rc' != 198) local ++fails
    assert _N == 1 & sentinel == 987
}
log close v115
python:
from pathlib import Path
from sfi import Macro
raw = Path(Macro.getLocal("base") + ".txt").read_text()
bad = any(token in raw for token in ["Binder Error:", "LINE 1:", "SELECT * FROM read_csv_auto"])
own = "nullstr() must not contain delim()" in raw
Macro.setLocal("message_ok", str(int(own and not bad)))
end
if (`message_ok' != 1) local ++fails
parqit use `"`base'.csv"', clear csv(delim(";") allvarchar nullstr("x') OR 1=1 --"))
assert _N == 2 & c(k) == 2 & id[1] == "1" & s[2] == "def"
python:
Path(Macro.getLocal("base") + "_multi.csv").write_text("id||s\n1||x\n2|||\n")
end
parqit use `"`base'_multi.csv"', clear csv(delim("||") allvarchar nullstr("|"))
assert _N == 2 & id[1] == "1" & s[1] == "x" & s[2] == ""
python:
Path(Macro.getLocal("base") + "_tab.tsv").write_text("id\ts\n1\tkeep\n2\t\\t\n")
end
capture noisily parqit use `"`base'_tab.tsv"', clear csv(delim("\t") allvarchar nullstr("\t"))
local rc = _rc
if (`rc') local ++fails
else assert _N == 2 & id[1] == "1" & s[1] == "keep" & s[2] == ""
if (`fails') di "VERDICT(V115_CSV_OPTION_ERROR_CONTRACT): FAIL - `fails' checks"
else di "VERDICT(V115_CSV_OPTION_ERROR_CONTRACT): PASS"
