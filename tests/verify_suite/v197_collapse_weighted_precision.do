* Weighted moments, exact cumulative selections and large counts: independent
* Decimal/Fraction and PyArrow oracles, including wide integer/decimal input.
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
parqit set threads 4
tempfile stem
python:
from decimal import Decimal
from sfi import Macro
import pyarrow as pa, pyarrow.parquet as pq
stem=Macro.getLocal("stem")
pq.write_table(pa.table({"g":[1,1],"x":pa.array([2**53,2**53+1],pa.int64()),"w":[1.,2.]}),stem+"_int.parquet")
pq.write_table(pa.table({"g":[1,1],"x":pa.array([Decimal("9007199254740992.1250"),Decimal("9007199254740992.3750")],pa.decimal128(24,4)),"w":[1.,3.]}),stem+"_dec.parquet")
pq.write_table(pa.table({"x":[float(i) for i in range(1,22)],"w":[1./7]*21}),stem+"_rank.parquet")
pq.write_table(pa.table({"g":[1,1,1,2,2,3,3],"x":[8e307,-8e307,1.,5e-324,1e-323,1.,0.],"w":[4.,4.,1.,1.,1.,float(2**54),1.]}),stem+"_extreme.parquet")
pq.write_table(pa.table({"g":[1,1,2,2],"x":[1.,2.,3.,4.],"w":[8e307]*4}),stem+"_huge_weights.parquet")
pq.write_table(pa.table({"g":[1]*21+[2]*21,"x":[0.]+[1.]*20+[0.]*20+[1.],"w":[8e307]+[1e-16]*20+[1e-16]*20+[8e307]}),stem+"_gini_tail.parquet")
pq.write_table(pa.table({"g":[1]*20+[2],"x":[1.]*21,"w":[1e-16]*20+[8e307]}),stem+"_percent_tail.parquet")
end

foreach family in int dec {
    parqit use using `"`stem'_`family'.parquet"'
    parqit collapse (mean) m=x (sum) s=x (sd) sd=x (variance) v=x (skewness) sk=x (kurtosis) ku=x (gini) gi=x [fw=w], by(g)
    parqit save `"`stem'_`family'_result.parquet"'
    parqit collect, clear
    if ("`family'"=="int") assert abs(v-1/3)<1e-16
    else assert v==.015625 & sd==.125
    parqit close _all
}

* Fourteen binary64 1/7 weights sum to just below 2, although rounding the
* cumulative sum to double gives 2. The fifteenth value must be selected.
parqit use using `"`stem'_rank.parquet"'
parqit collapse (select2) lo=x (select-2) hi=x [iw=w]
parqit collect, clear
assert _N==1 & lo==15 & hi==7
parqit close _all

* Decimal values that round to 1 in double are still not binary inputs.
parqit sql "SELECT * FROM (VALUES ('0.99999999999999999999999999999999999999'::DECIMAL(38,38),1.0::DOUBLE),('0'::DECIMAL(38,38),1.0)) t(x,w)"
parqit collapse (sebinomial) sb=x [fw=w]
parqit collect, clear
assert missing(sb)
parqit close _all

* Intermediate weight totals may exceed the Stata/double range while the
* percentages, quantiles and Gini remain finite and well-defined.
parqit use using `"`stem'_huge_weights.parquet"'
parqit collapse (mean) m=x (count) n=x (percent) pc=x (p50) md=x (gini) gi=x (select2) lo=x [pw=w], by(g)
parqit save `"`stem'_huge_result.parquet"'
parqit collect, clear
assert pc==50 & missing(n)
assert m==1.5 & md==1.5 & lo==1 & abs(gi-1/6)<1e-16 if g==1
assert m==3.5 & md==3.5 & lo==3 & abs(gi-1/14)<1e-16 if g==2
parqit close _all
parqit use using `"`stem'_gini_tail.parquet"'
parqit collapse (gini) gi=x [pw=w], by(g)
parqit save `"`stem'_gini_tail_result.parquet"'
parqit collect, clear
assert gi==1 if g==1
assert gi>0 & gi<1e-320 if g==2
parqit close _all
parqit use using `"`stem'_percent_tail.parquet"'
parqit collapse (percent) pc=x [pw=w], by(g)
parqit save `"`stem'_percent_tail_result.parquet"'
parqit collect, clear
assert pc>0 & pc<1e-319 if g==1
parqit close _all

* Unweighted pivots share the new statistics.
parqit use using `"`stem'_rank.parquet"'
parqit gen byte r=ceil(x/7)
parqit gen byte c=mod(x,2)
parqit pivot (variance) v=x (nmissing) nm=x, rows(r) cols(c)
parqit collect, clear
assert _N==3 & nm0==0 & nm1==0
assert v0==4 & abs(v1-20/3)<1e-14 if r==1
parqit close _all

parqit use using `"`stem'_extreme.parquet"'
parqit collapse (mean) m=x (sum) s=x (sebinomial) se=x [fw=w], by(g)
parqit collect, clear
assert m==1/9 & s==1 if g==1
assert m==1e-323 if g==2
assert m==1 & se>0 & abs(se-2^-54)<1e-31 if g==3
parqit close _all

* double storage does not convert an exact wide frequency count before the
* integer precision policy is applied. Parquet retains its exact payload.
parqit use using `"`stem'_extreme.parquet"'
parqit collapse (count) n=x [fw=w] if g==3, double
parqit save `"`stem'_count.parquet"'
capture noisily parqit collect, clear
assert _rc==198
parqit collect, clear int64(string)
assert _N==1 & n=="18014398509481985"
parqit close _all

python:
from decimal import Decimal,localcontext
from fractions import Fraction
from sfi import Macro
import math,pyarrow.parquet as pq
stem=Macro.getLocal("stem")
with localcontext() as ctx:
    ctx.prec=120
    for family in ["int","dec"]:
        rows=pq.read_table(stem+"_"+family+".parquet").to_pylist()
        actual=pq.read_table(stem+"_"+family+"_result.parquet").to_pylist()[0]
        x=[Decimal(r["x"]) for r in rows]; w=[Decimal.from_float(r["w"]) for r in rows]
        W=sum(w); mean=sum(a*b for a,b in zip(x,w))/W
        m2=sum(b*(a-mean)**2 for a,b in zip(x,w))/W
        variance=m2*W/(W-1)
        m3=sum(b*(a-mean)**3 for a,b in zip(x,w))/W
        m4=sum(b*(a-mean)**4 for a,b in zip(x,w))/W
        gi=sum(wa*wb*abs(xa-xb) for wa,xa in zip(w,x) for wb,xb in zip(w,x))/(2*W*sum(a*b for a,b in zip(x,w)))
        truths={"m":mean,"s":sum(a*b for a,b in zip(x,w)),"v":variance,"sd":variance.sqrt(),"sk":m3/(m2*m2.sqrt()),"ku":m4/(m2*m2),"gi":gi}
        for key,truth in truths.items():
            rounded=float(truth)
            assert abs(actual[key]-rounded)<=2*math.ulp(rounded),(family,key,actual[key],rounded)
    weights=[Fraction.from_float(1./7)]*21
    assert sum(weights[:14])<2<=sum(weights[:15])
count=pq.read_table(stem+"_count.parquet").column("n").to_pylist()[0]
assert int(count)==2**54+1,count
for fixture,target,field in [("gini_tail","gini_tail_result","gi"),("percent_tail","percent_tail_result","pc")]:
    rows=pq.read_table(stem+"_"+fixture+".parquet").to_pylist()
    results=pq.read_table(stem+"_"+target+".parquet").to_pylist()
    weights={g:sum(Fraction.from_float(r["w"]) for r in rows if r["g"]==g) for g in [1,2]}
    for row in results:
        g=row["g"]
        group=[r for r in rows if r["g"]==g]
        if field=="gi":
            numerator=sum(Fraction.from_float(a["w"])*Fraction.from_float(b["w"])*abs(Fraction.from_float(a["x"])-Fraction.from_float(b["x"])) for a in group for b in group)
            denominator=2*weights[g]*sum(Fraction.from_float(r["w"])*Fraction.from_float(r["x"]) for r in group)
            truth=float(numerator/denominator)
        else: truth=float(100*weights[g]/sum(weights.values()))
        assert abs(row[field]-truth)<=2*math.ulp(truth),(fixture,g,row[field],truth)
print("Decimal-120, exact Fraction ranks and Parquet count payload: PASS")
end
display "VERDICT(V197_COLLAPSE_WEIGHTED_PRECISION): PASS"
