* V91: typed table axes, complete text records and exact view-name identity.
clear all
set more off
set linesize 80
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile stem source
local out `"`stem'_stats"'
mkdir `"`out'"'

program _v91_pair
    syntax, Dir(string) Tag(name) Vars(string) [Missing]
    quietly log using `"`dir'/`tag'_native.log"', text name(v91n)
    noisily tabulate `vars', `missing'
    quietly log close v91n
    quietly log using `"`dir'/`tag'_lazy.log"', text name(v91l)
    noisily parqit tabulate `vars', `missing'
    quietly log close v91l
end

set obs 4
gen x = cond(_n==4,.,cond(_n==3,10,_n))
gen y = 1
gen str3 s = string(x)
replace s = "" in 4
parqit open _data
_v91_pair, dir(`"`out'"') tag(numeric_rows) vars(x y) missing
_v91_pair, dir(`"`out'"') tag(numeric_cols) vars(y x) missing
_v91_pair, dir(`"`out'"') tag(string_rows) vars(s y)
_v91_pair, dir(`"`out'"') tag(string_cols) vars(y s)
_v91_pair, dir(`"`out'"') tag(string_missing) vars(s y) missing
python:
from sfi import Macro
from pathlib import Path
p=Path(Macro.getLocal('out'))
for tag in ['numeric_rows','numeric_cols','string_rows','string_cols','string_missing']:
    def lines(s): return [x.rstrip() for x in s.splitlines() if x.strip()]
    assert lines((p/(tag+'_native.log')).read_text()) == lines((p/(tag+'_lazy.log')).read_text()), tag
end
parqit close _all

* Cell separators and SMCL must remain data; strL records cross 32 KiB intact.
clear
set obs 2
gen byte k = 1
gen strL s = "a"+char(31)+"b"
gen long tail = 77
label variable s "{hline 8}"
parqit open _data
log using `"`out'/controls.log"', text name(v91c)
parqit duplicates list k
parqit lookfor hline
parqit describe
log close v91c
parqit close _all
replace s = "{hline 8}"
parqit open _data
log using `"`out'/braces.log"', text name(v91b)
parqit duplicates list k
log close v91b
parqit close _all
replace s = 40000*"a"
parqit open _data
log using `"`out'/long.log"', text name(v91long)
parqit duplicates list k
parqit codebook s
log close v91long
quietly parqit tabstat tail, by(s) save
mata: assert(strlen(st_global("r(name1)")) == 40000)
assert r(Stat1)[1,1] == 77
quietly parqit levelsof s
mata: assert(strlen(st_global("r(levels)")) == 40004)
parqit close _all
python:
from sfi import Macro
from pathlib import Path
p=Path(Macro.getLocal('out'))
controls=(p/'controls.log').read_text()
braces=(p/'braces.log').read_text()
long=(p/'long.log').read_text()
assert 'a\\x1fb' in controls and '{hline 8}' in controls
assert braces.count('{hline 8}') >= 2
assert long.count('77') >= 2
assert '["'+23*'a'+'~","'+23*'a'+'~"]' in long
end

* A foreign binary-text key must not merge distinct categories in the table.
python:
from sfi import Macro
import pyarrow as pa, pyarrow.parquet as pq
pq.write_table(pa.table({'s':['a\x00b','a\x00c'],'t':[1,1]}),Macro.getLocal('source'))
end
parqit use using `"`source'"'
quietly parqit tabulate s t
assert r(N) == 2 & r(r) == 2 & r(c) == 1
quietly parqit tabulate t s
assert r(N) == 2 & r(r) == 1 & r(c) == 2
parqit close _all
python:
from sfi import Macro
import pyarrow as pa, pyarrow.parquet as pq
pq.write_table(pa.table({'s':['a\x00'+str(i) for i in range(31)], 't':[1]*31}),Macro.getLocal('source'))
end
parqit use using `"`source'"'
capture noisily parqit tabulate t s
assert _rc == 198
parqit close _all

* Exact exposed names and reported aliases identify the same view column.
python:
from sfi import Macro
import pyarrow as pa, pyarrow.parquet as pq
pq.write_table(pa.table({'a':[1.,2.,3.,4.],'A':[10.,20.,30.,40.],
                        'g':[0,0,0,0],'G':[1,1,2,2]}),Macro.getLocal('source'))
end
parqit use using `"`source'"'
quietly parqit summarize A
assert r(mean) == 25
quietly parqit tabstat A, by(G) s(n mean) save
assert r(Stat1)[2,1] == 15 & r(Stat2)[2,1] == 35
local columns : colnames r(Stat1)
assert "`columns'" == "A"
quietly parqit correlate a A
local columns : colnames r(C)
assert "`columns'" == "a A"
assert reldif(r(C)[2,1],1) < 1e-12
parqit generate double twice = 2*A_1
quietly parqit summarize twice
assert r(mean) == 50
parqit close _all

* Preserve documented empty-group success and reject an invalid bin request.
clear
set obs 4
gen double x = 7
gen str1 g = ""
capture noisily tabstat x, by(g) save
di "NATIVE_EMPTY_GROUP_RC=" _rc
parqit open _data
quietly parqit tabstat x, by(g) save
local matrices : r(matrices)
assert "`matrices'" == ""
parqit histogram x
assert r(width) == 0 & r(bins) == 1 & r(N) == 4
capture noisily parqit histogram x, bins(-1) nodraw
assert _rc == 198
parqit close _all
di "VERDICT(V91_STATISTICS_TEXT_AND_NAMES): PASS"
