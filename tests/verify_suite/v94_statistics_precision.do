* N1-N4/N6: mathematical oracles, scale/translation/order and verb consistency.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
parqit set threads 1

set obs 4
gen double x = 1e16+2*(_n-1)
gen double y = _n
foreach reverse in 0 1 {
    if `reverse' gsort -x
    parqit open _data
    quietly parqit summarize x, detail
    assert r(N)==4 & abs(r(Var)/(20/3)-1)<1e-14
    assert abs(r(skewness))<1e-14 & abs(r(kurtosis)-1.64)<1e-14
    quietly parqit pwcorr x y, obs sig
    assert r(C)[2,1]==1 & r(sig)[2,1]==0 & r(Nobs)[2,1]==4
    quietly parqit correlate y x
    assert r(rho)==1
    parqit close _all
}

clear
set obs 8
gen double x = 1e12+.001*(_n-1)
parqit open _data
quietly parqit summarize x, detail
assert abs(r(Var)/5.9814857585089547e-6-1)<1e-13
assert abs(r(skewness)+.019093391176168285)<1e-13
parqit close _all

clear
set obs 6
gen double x = cond(mod(_n,3)==1,1e16,cond(mod(_n,3)==0,-1e16,1))
gen byte g = 1
gen byte h = 1
parqit open _data
quietly parqit summarize x
assert r(mean)==1/3
quietly parqit tabstat x, s(n mean sum) save
assert r(StatTotal)[1,1]==6 & r(StatTotal)[2,1]==1/3 & r(StatTotal)[3,1]==2
parqit close _all
foreach method in egen collapse pivot {
    preserve
    parqit open _data
    if "`method'"=="egen" {
        parqit egen double sx = total(x), by(g)
        parqit egen double mx = mean(x), by(g)
        parqit egen double dx = sd(x), by(g)
    }
    if "`method'"=="collapse" parqit collapse (sum) sx=x (mean) mx=x (sd) dx=x, by(g)
    if "`method'"=="pivot" parqit pivot (sum) sx=x (mean) mx=x (sd) dx=x, rows(g) cols(h)
    parqit collect, clear
    if "`method'"=="pivot" rename (sx1 mx1 dx1) (sx mx dx)
    assert sx==2 & mx==1/3
    assert abs(dx/sqrt(8e31)-1)<1e-13
    parqit close _all
    restore
}

foreach power in -200 -108 -80 0 80 100 160 200 300 {
    clear
    set obs 4
    gen double x = cond(_n==4,10,_n)*10^`power'
    gen double y = _n*10^`power'
    parqit open _data
    quietly parqit summarize x, detail
    assert r(N)==4 & abs(r(sd)/10^`power'/sqrt(50/3)-1)<1e-12
    assert abs(r(skewness)-1.0182337649086284)<1e-12
    assert abs(r(kurtosis)-2.2304)<1e-12
    parqit egen double dx = sd(x)
    quietly parqit summarize dx
    assert abs(r(mean)/10^`power'/sqrt(50/3)-1)<1e-12
    parqit close _all
}

* Wide physical inputs are accumulated before conversion to Stata doubles.
parqit sql "SELECT (9007199254740993::BIGINT+i) AS x, i AS y FROM range(4) t(i)"
quietly parqit summarize x, detail
assert abs(r(Var)/(5/3)-1)<1e-14
quietly parqit correlate x y
assert r(rho)==1
parqit close _all
parqit sql "SELECT 170141183460469231731687303715884105727::HUGEINT AS x FROM range(2)"
quietly parqit tabstat x, s(n mean sum sd) save
assert r(StatTotal)[1,1]==2 & r(StatTotal)[2,1]==1.7014118346046923e38
assert r(StatTotal)[3,1]==3.4028236692093846e38 & r(StatTotal)[4,1]==0
parqit close _all

* Grouped updates/merges span multiple execution vectors and row groups.
clear
set obs 1048576
gen byte g = mod(floor((_n-1)/4),16)
gen double y = mod(_n-1,4)
gen double x = 1e12+.001*y
foreach threads in 1 4 {
    parqit set threads `threads'
    parqit open _data
    quietly parqit summarize x, detail
    assert abs(r(skewness)-.06573061752096527)<1e-12
    quietly parqit correlate x y
    assert abs(r(rho)-.9995648074066765)<1e-12
    quietly parqit tabstat x, by(g) s(mean sd) save
    matrix first = r(Stat1)
    assert first[1,1]==1000000000000.0015
    parqit close _all
}
di "VERDICT(V94_STATISTICS_PRECISION): PASS"
