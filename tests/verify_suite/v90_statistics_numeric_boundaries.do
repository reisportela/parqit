* V90: numeric results are values, overflow is local to its aggregate.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

foreach power in -200 -100 0 80 100 160 200 300 {
    clear
    set obs 4
    gen double x = _n*10^`power'
    gen double y = (5-_n)*10^`power'
    gen double ordinary = _n
    parqit open _data
    quietly summarize x
    matrix native = (r(N),r(mean),r(min),r(max))
    quietly parqit summarize ordinary x
    matrix lazy = (r(N),r(mean),r(min),r(max))
    assert abs(r(sd)/10^`power'-sqrt(5/3)) < 1e-12
    mata: assert(mreldif(st_matrix("native"),st_matrix("lazy")) < 1e-12)
    quietly summarize x, detail
    matrix native = (r(N),r(mean),r(min),r(max),r(p50))
    mata: st_matrix("native",strtoreal(strofreal(st_matrix("native"),"%24.17g")))
    quietly parqit summarize x, detail
    matrix lazy = (r(N),r(mean),r(min),r(max),r(p50))
    assert abs(r(sd)/10^`power'-sqrt(5/3)) < 1e-12
    mata: assert(mreldif(st_matrix("native"),st_matrix("lazy")) < 1e-12)
    quietly parqit tabstat x ordinary, s(n mean sd var min max p50) save
    assert r(StatTotal)[1,1] == 4 & r(StatTotal)[1,2] == 4
    assert reldif(r(StatTotal)[3,2],sqrt(5/3)) < 1e-12
    if (`power'>154) {
        assert abs(r(StatTotal)[3,1]/10^`power'-sqrt(5/3)) < 1e-12
        assert missing(r(StatTotal)[4,1])
        quietly parqit pwcorr x y ordinary, obs sig
        assert r(Nobs)[2,1] == 4 & abs(r(C)[2,1]+1)<1e-12 & r(sig)[2,1]<1e-10
        quietly parqit correlate x y ordinary
        assert r(N) == 4 & abs(r(C)[2,1]+1)<1e-12
    }
    parqit close _all
}

* Non-finite text must never resolve to a scalar or an abbreviated variable.
clear
set obs 4
gen double x = _n*1e80
gen byte nanny = 71
gen byte inflation = 82
gen byte constant = 1
set varabbrev on
parqit open _data
scalar nan = 123
scalar inf = 456
quietly parqit summarize x, detail
assert abs(r(kurtosis)-1.64)<1e-12 & r(N) == 4
quietly parqit summarize constant, detail
assert missing(r(kurtosis)) & r(N)==4
scalar drop nan inf
quietly parqit summarize x, detail
assert abs(r(kurtosis)-1.64)<1e-12 & r(N) == 4
parqit close _all
clear
set obs 100
gen double x = 8e307
gen byte inflation = 82
parqit open _data
quietly parqit summarize x
assert r(N) == 100 & r(mean)==8e307 & r(sd) == 0
assert r(min) == 8e307 & r(max) == 8e307
quietly parqit summarize x, detail
assert r(N) == 100 & r(mean)==8e307 & missing(r(kurtosis))
assert r(p50) == 8e307 & r(sd) == 0
parqit close _all

* Float extrema must be promoted before conversion to text, like native r().
clear
set obs 3
gen float x = _n/10
quietly summarize x
scalar lo = r(min)
scalar hi = r(max)
parqit open _data
quietly parqit summarize x
assert r(min) == lo & r(max) == hi
quietly parqit summarize x, detail
assert r(min) == lo & r(max) == hi
quietly parqit tabstat x, s(min max) save
assert r(StatTotal)[1,1] == lo & r(StatTotal)[2,1] == hi
parqit close _all

* A bad group must not erase finite statistics in another group or column.
clear
set obs 8
gen byte g = (_n>4)+1
gen double x = mod(_n-1,4)+1
gen double z = x
replace x = x*1e200 if g==2
quietly tabstat x z, by(g) s(n mean sd var min max p50) nototal save
matrix S1 = r(Stat1)
matrix S2 = r(Stat2)
matrix S2[3,1] = sqrt(5/3)*1e200
mata: st_matrix("S2",strtoreal(strofreal(st_matrix("S2"),"%24.17g")))
parqit open _data
quietly datasignature
local signature `"`r(datasignature)'"'
quietly parqit tabstat x z, by(g) s(n mean sd var min max p50) save
mata: assert(mreldif(st_matrix("S1"),st_matrix("r(Stat1)")) < 1e-12)
mata: assert(mreldif(st_matrix("S2"),st_matrix("r(Stat2)")) < 1e-12)
quietly datasignature
assert `"`r(datasignature)'"' == `"`signature'"'
quietly parqit describe
assert r(n_steps) == 0
parqit close _all

* Source errors remain errors after introducing the numerical retry.
parqit sql "SELECT CASE WHEN i=2 THEN error('v90_source_error') ELSE i END AS x FROM range(4) t(i)"
capture noisily parqit summarize x
assert _rc == 920
parqit close _all

* Pin the suspected NaN-source gap: the existing view boundary already folds it.
parqit sql "SELECT CAST('nan' AS DOUBLE) AS x UNION ALL SELECT 1.0"
quietly parqit summarize x
assert r(N) == 1 & r(mean) == 1
quietly parqit misstable
assert r(N) == 2 & r(n_complete) == 1
parqit close _all
di "VERDICT(V90_STATISTICS_NUMERIC_BOUNDARIES): PASS"
