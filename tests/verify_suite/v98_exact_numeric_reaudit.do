* Additional audit: exact sums, typed ranks/conversions, functions and mixed keys.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
parqit set threads 1

input double x
1e100
1e50
1
-1e100
-1e50
end
parqit open _data
quietly parqit summarize x
assert r(sum)==1 & r(mean)==.2
quietly parqit tabstat x, s(sum mean) save
assert r(StatTotal)[1,1]==1 & r(StatTotal)[2,1]==.2
parqit egen double sx = total(x)
parqit collect, clear
assert sx==1
parqit close _all

parqit sql "SELECT * FROM (VALUES (9007199254740993::BIGINT),(9007199254740994::BIGINT)) t(x)"
quietly parqit summarize x, detail
assert r(mean)==9007199254740994 & r(p50)==9007199254740994
assert r(sum)==18014398509481988 & r(Var)==.5
quietly parqit tabstat x, s(p50 range) save
assert r(StatTotal)[1,1]==9007199254740994 & r(StatTotal)[2,1]==1
parqit collapse (median) x
parqit collect, clear
assert x==9007199254740994
parqit close _all

parqit sql "SELECT CAST('1.0000000000000001110223024625156540424' AS DECIMAL(38,37)) AS x FROM range(3)"
quietly parqit summarize x, detail
assert r(mean)==1+2^-52 & r(min)==1+2^-52 & r(p50)==1+2^-52
parqit gen double y = x
parqit gen double z = max(x,1e-100)
parqit gen double w = min(x,2.25)
parqit gen double c = cond(1,x,.1)
parqit collect, clear
assert x==1+2^-52 & y==x & z==x & w==x & c==x
parqit close _all

clear
input double x double u
4503599627370497 1
9007199254740991 1
1e100 3
1e100 1e-300
end
parqit open _data
parqit gen double rx = round(x)
parqit gen double ru = round(x,u)
parqit gen double mu = mod(x,u)
parqit gen double literal = .90000000000000024
parqit gen double neg = -(1+x)
parqit gen str3 a = substr("abc",1,1e100)
parqit gen str3 b = substr("abc",3.9,1e100)
parqit gen str3 c = substr("abc",1e100,3)
parqit collect, clear
assert rx==x & ru==x
assert mu[3]==1
assert literal==.90000000000000024 & neg==-(1+x)
assert a=="abc" & b=="c" & c==""
parqit close _all

parqit sql "SELECT 9007199254740992::DOUBLE AS x, 9007199254740993::BIGINT AS y"
foreach mode in off on {
    parqit set statamissing `mode'
    quietly parqit count if x==y
    assert r(N)==0
    quietly parqit count if x<y
    assert r(N)==1
    quietly parqit count if inrange(x,y,y)
    assert r(N)==0
}
parqit close _all
parqit set statamissing off

parqit sql "SELECT 340282366920938463463374607431768211455::UHUGEINT AS u, CAST('-170141183460469231731687303715884105728' AS HUGEINT) AS s"
quietly parqit count if u<0
assert r(N)==0
quietly parqit count if u>-1
assert r(N)==1
quietly parqit count if s<170141183460469231731687303715884105728
assert r(N)==1
parqit close _all

clear
set obs 1
gen long id = 16777217
gen long common = 16777217
parqit open _data, name(using)
clear
set obs 1
gen float id = 16777216
gen float common = 1
parqit open _data, name(master)
parqit merge 1:1 id using view:using
parqit collect, clear
assert _N==2 & _merge!=3
assert id[2]==16777217 & common[2]==16777217
local type : type id
assert "`type'"=="double"
parqit close _all

parqit sql "SELECT 9007199254740993::BIGINT AS id", name(using)
parqit sql "SELECT 9007199254740992::DOUBLE AS id", name(master)
parqit joinby id using view:using
quietly parqit count
assert r(N)==0
parqit close _all

clear
set obs 1
gen byte id = 1
gen long x1 = 16777217
gen float x2 = 1
parqit open _data
parqit reshape long x, i(id) j(t)
parqit collect, clear
assert x[1]==16777217 & x[2]==1
parqit close _all

foreach percentage in .00015 .0015 12.3456 {
    parqit sql "SELECT i AS id FROM range(1234567) t(i)"
    parqit sample `percentage', seed(42)
    quietly parqit count
    assert r(N)==floor(1234567*`percentage'/100+.5)
    quietly parqit summarize id
    scalar mean = r(mean)
    quietly parqit summarize id
    assert r(mean)==mean
    parqit close _all
}
di "VERDICT(V98_EXACT_NUMERIC_REAUDIT): PASS"
