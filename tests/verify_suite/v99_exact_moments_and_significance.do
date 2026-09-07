* Exact central moments, tiny nonzero shape/significance and machine boundaries.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
parqit set threads 1
tempfile tiny
python:
from sfi import Macro
import pyarrow as pa, pyarrow.parquet as pq
pq.write_table(pa.table({'x':pa.array([0.,5e-324,5e-324,5e-324,5e-324])}),Macro.getLocal('tiny'))
end
parqit use using "`tiny'"
quietly parqit summarize x, detail
assert r(N)==5 & r(sd)==0 & r(Var)==0
assert r(skewness)==-1.5 & r(kurtosis)==3.25
parqit close _all

parqit sql "SELECT * FROM (VALUES (-1e200::DOUBLE),(1::DOUBLE),(1e200::DOUBLE)) t(x)"
quietly parqit summarize x, detail
assert r(mean)==1/3 & r(sum)==1 & r(sd)==1e200 & r(Var)==.
assert reldif(r(skewness)/1e-200,-1.224744871391589)<1e-15
assert r(kurtosis)==1.5
parqit close _all

parqit sql "SELECT * FROM (VALUES (0::BIGINT),(9007199254740993::BIGINT),(18014398509481986::BIGINT)) t(x)"
quietly parqit summarize x, detail
assert r(mean)==9007199254740992 & r(sd)==9007199254740992
assert r(skewness)==0 & r(kurtosis)==1.5
scalar exact_sd = r(sd)
parqit egen double sd_x = sd(x)
quietly parqit summarize sd_x
assert r(mean)==exact_sd
parqit close _all

parqit sql "SELECT CAST(x AS DECIMAL(38,37)) AS x FROM (VALUES ('-1.0000000000000001110223024625156540424'),('0'),('1.0000000000000001110223024625156540424')) t(x)"
quietly parqit summarize x, detail
assert r(sd)==1+2^-52 & r(Var)==1+2^-52
assert r(skewness)==0 & r(kurtosis)==1.5
parqit close _all

clear
input double x double y
0 0
1 1
2 2.000000000001
end
parqit open _data
quietly parqit pwcorr x y, sig obs
assert r(C)[2,1]==1
assert abs(r(sig)[2,1]/1.8379263629379358e-13-1)<1e-14
parqit close _all
replace x = cond(_n==1,-1e200,cond(_n==2,1,1e200))
replace y = cond(_n==1,-1e200,cond(_n==2,2,1e200))
foreach threads in 1 4 {
    parqit set threads `threads'
    parqit open _data
    quietly parqit pwcorr x y, sig obs
    assert r(C)[2,1]==1
    assert abs(r(sig)[2,1]/3.6755259694786134e-201-1)<1e-14
    parqit close _all
}

* A true affine relation still has zero significance; constants are undefined.
replace y = x
parqit open _data
quietly parqit pwcorr x y, sig
assert r(C)[2,1]==1 & r(sig)[2,1]==0
parqit close _all
replace y = 1
parqit open _data
quietly parqit pwcorr x y, sig
assert r(C)[2,1]==. & r(sig)[2,1]==.
parqit close _all
di "VERDICT(V99_EXACT_MOMENTS_AND_SIGNIFICANCE): PASS"
