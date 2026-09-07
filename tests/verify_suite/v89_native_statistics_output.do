* V89: native layouts, full-precision returned tables and unchanged Stata data.
clear all
set more off
set varabbrev off
set linesize 80
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile stem
local out `"`stem'_output"'
mkdir `"`out'"'

program _v89_pair
    syntax, Dir(string) Tag(name) Native(string asis) Lazy(string asis)
    quietly log using `"`dir'/`tag'_native.log"', text name(v89native)
    noisily `native'
    quietly log close v89native
    quietly datasignature
    local signature `"`r(datasignature)'"'
    local changed = c(changed)
    local filename `"`c(filename)'"'
    quietly parqit describe
    local steps = r(n_steps)
    quietly log using `"`dir'/`tag'_parqit.log"', text name(v89lazy)
    noisily parqit `lazy'
    quietly log close v89lazy
    quietly datasignature
    assert `"`r(datasignature)'"' == `"`signature'"'
    assert c(changed) == `changed'
    assert `"`c(filename)'"' == `"`filename'"'
    quietly parqit describe
    assert r(n_steps) == `steps'
end

sysuse auto, clear
parqit open _data
_v89_pair, dir(`"`out'"') tag(summary) native(summarize price mpg weight) lazy(summarize price mpg weight)
_v89_pair, dir(`"`out'"') tag(detail) native(summarize price, detail) lazy(summarize price, detail)
_v89_pair, dir(`"`out'"') tag(tab1) native(tabulate foreign) lazy(tabulate foreign)
_v89_pair, dir(`"`out'"') tag(tab2) native(tabulate foreign rep78, row col) lazy(tabulate foreign rep78, row col)
_v89_pair, dir(`"`out'"') tag(tabstat) native(tabstat price mpg, s(n mean sd min p50 max)) lazy(tabstat price mpg, s(n mean sd min p50 max))
_v89_pair, dir(`"`out'"') tag(tabstat_by) native(tabstat price mpg, s(n mean sd) by(foreign) nototal) lazy(tabstat price mpg, s(n mean sd) by(foreign))
_v89_pair, dir(`"`out'"') tag(tabstat_one) native(tabstat price, s(n mean sd min p50 max)) lazy(tabstat price, s(n mean sd min p50 max))
_v89_pair, dir(`"`out'"') tag(tabstat_one_by) native(tabstat price, s(mean sd) by(foreign) nototal) lazy(tabstat price, s(mean sd) by(foreign))
_v89_pair, dir(`"`out'"') tag(corr) native(correlate price mpg weight) lazy(correlate price mpg weight)
_v89_pair, dir(`"`out'"') tag(pwcorr) native(pwcorr price mpg rep78, obs sig) lazy(pwcorr price mpg rep78, obs sig)
_v89_pair, dir(`"`out'"') tag(corr_many) native(correlate price mpg weight length turn displacement gear_ratio headroom) lazy(correlate price mpg weight length turn displacement gear_ratio headroom)
_v89_pair, dir(`"`out'"') tag(dups) native(duplicates report foreign) lazy(duplicates report foreign)

* Returned tables must retain precision independently of printed rounding.
quietly correlate price mpg rep78
matrix C = r(C)
quietly parqit correlate price mpg rep78
mata: assert(mreldif(st_matrix("C"),st_matrix("r(C)")) < 1e-12)
quietly pwcorr price mpg rep78, obs sig
matrix C = r(C)
matrix N = r(Nobs)
matrix P = r(sig)
quietly parqit pwcorr price mpg rep78, obs sig
mata: assert(mreldif(st_matrix("C"),st_matrix("r(C)")) < 1e-12)
mata: assert(mreldif(st_matrix("N"),st_matrix("r(Nobs)")) == 0)
mata: assert(mreldif(st_matrix("P"),st_matrix("r(sig)")) < 1e-12)
quietly tabstat price mpg, s(n mean sd p25 p50 p75) save
matrix S = r(StatTotal)
quietly parqit tabstat price mpg, s(n mean sd p25 p50 p75) save
mata: assert(mreldif(st_matrix("S"),st_matrix("r(StatTotal)")) < 1e-12)
quietly tabstat price mpg, s(n mean sd p25 p50 p75) by(foreign) nototal save
matrix S1 = r(Stat1)
matrix S2 = r(Stat2)
local n1 `"`r(name1)'"'
local n2 `"`r(name2)'"'
quietly parqit tabstat price mpg, s(n mean sd p25 p50 p75) by(foreign) save
assert `"`r(name1)'"' == `"`n1'"'
assert `"`r(name2)'"' == `"`n2'"'
mata: assert(mreldif(st_matrix("S1"),st_matrix("r(Stat1)")) < 1e-12)
mata: assert(mreldif(st_matrix("S2"),st_matrix("r(Stat2)")) < 1e-12)
parqit close _all

foreach n in 0 1 2 3 5 {
    clear
    set obs `n'
    gen double x = 7 + _n
    parqit open _data
    _v89_pair, dir(`"`out'"') tag(n`n') native(summarize x, detail) lazy(summarize x, detail)
    parqit close _all
}
clear
set obs 4
gen double tiny = _n * 1.23456789012345e-100
gen double huge = _n * 1.23456789012345e100
gen double hole = .
gen double constant = 7
parqit open _data
_v89_pair, dir(`"`out'"') tag(scale) native(summarize tiny huge) lazy(summarize tiny huge)
_v89_pair, dir(`"`out'"') tag(empty_constant) native(summarize hole constant, detail) lazy(summarize hole constant, detail)
_v89_pair, dir(`"`out'"') tag(perfect) native(pwcorr constant tiny huge, obs sig) lazy(pwcorr constant tiny huge, obs sig)
parqit close _all
gen double category = _n/10
gen str12 code = cond(_n<=2,".a",".b")
parqit open _data
_v89_pair, dir(`"`out'"') tag(decimal_levels) native(tabulate category) lazy(tabulate category)
_v89_pair, dir(`"`out'"') tag(decimal_by) native(tabstat tiny, by(category) nototal) lazy(tabstat tiny, by(category))
_v89_pair, dir(`"`out'"') tag(dot_string_levels) native(tabulate code) lazy(tabulate code)
parqit close _all

* View labels must be independent of labels in the current Stata dataset.
clear
set obs 5
gen double salario = _n + 0.25
label variable salario "Remuneração diária"
parqit open _data
_v89_pair, dir(`"`out'"') tag(unicode) native(summarize salario, detail) lazy(summarize salario, detail)
label variable salario "Another dataset label"
quietly log using `"`out'/label_isolation.log"', text name(v89label)
parqit summarize salario, detail
quietly log close v89label
assert `"`: variable label salario'"' == "Another dataset label"
parqit close _all
gen double salário = _n
gen str12 região = cond(_n<=2,"Nórte","Súl")
parqit open _data
_v89_pair, dir(`"`out'"') tag(unicode_name) native(summarize salário) lazy(summarize salário)
_v89_pair, dir(`"`out'"') tag(unicode_levels) native(tabulate região) lazy(tabulate região)
_v89_pair, dir(`"`out'"') tag(unicode_by) native(tabstat salário, by(região) nototal) lazy(tabstat salário, by(região))
parqit close _all

* A capped pattern table must use the full view as its percentage denominator.
clear
set obs 128
forvalues j=1/7 {
    gen byte x`j' = 1 if mod(floor((_n-1)/2^(`j'-1)),2)
}
parqit open _data
quietly log using `"`out'/patterns_cap.log"', text name(v89patterns)
parqit misstable patterns
assert r(N) == 128 & r(r) == 100
quietly log close v89patterns
parqit close _all

python:
from pathlib import Path
from sfi import Macro
import difflib, re
p = Path(Macro.getLocal('out'))
normalize = lambda path: [s.rstrip() for s in path.read_text().splitlines() if s.strip()]
files = sorted(p.glob('*_native.log'))
differences = []
for native in files:
    lazy = p/native.name.replace('_native.log','_parqit.log')
    a, b = normalize(native), normalize(lazy)
    if a != b:
        differences.extend(difflib.unified_diff(a,b,fromfile=native.name,tofile=lazy.name))
assert not differences, '\n'.join(differences)
assert len(files) >= 20
text = (p/'label_isolation.log').read_text()
assert 'Remuneração diária' in text and 'Another dataset label' not in text
text = (p/'patterns_cap.log').read_text()
assert '128 observations in all' in text
assert re.search(r'^\s+100\s+78\.1[23]\s+\|', text, re.M)
print('Native layout comparisons:', len(files))
end
di "VERDICT(V89_NATIVE_STATISTICS_OUTPUT): PASS"
