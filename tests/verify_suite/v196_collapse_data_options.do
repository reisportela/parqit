* gcollapse data options, independent Parquet payloads and explicit oracles.
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
parqit set threads 4
tempfile stem native
clear
input byte g byte t double x int y double w
1 1 0 1 1
1 2 1 2 2
1 3 2 . 3
1 4 4 4 4
1 5 . 5 5
2 6 . 6 1
2 7 . 7 2
3 8 -2 8 1
3 9 1 9 2
3 10 3 10 3
end
label variable x "Source #STAT#"
label variable y "Árvore #STAT#"
gen double z=100+t
parqit save `"`stem'.parquet"', data
save `"`native'"'

* Explicit source patterns, wild target names, range, cw, aliases and frequency.
parqit use using `"`stem'.parquet"'
parqit collapse (sum) total_x*=x* (var) v=x if t<6 in 2/5, by(g) cw wildparse freq(f)
parqit collect, clear
assert _N==1 & g==1 & total_x==7 & f==3
assert abs(v-7/3)<1e-14
parqit close _all

* Digit captures and raw target patterns use the view's names.
parqit use using `"`stem'.parquet"'
parqit gen double x1=x
parqit gen double x2=x*2
parqit collapse (sum) total#=x#, by(g) wildparse
parqit collect, clear
assert total1==7 & total2==14 if g==1
parqit close _all
parqit use using `"`stem'.parquet"'
parqit collapse (mean) mx=x my=y, by(-g) cw
parqit collect, clear
assert g[1]==3 & g[_N]==1
assert mx==5/3 & my==7/3 if g==1
parqit close _all

* Weights, rawstat and stored group frequency count actual admitted rows.
foreach weight in fw aw iw pw {
    parqit use using `"`stem'.parquet"'
    parqit collapse (mean) m=x raw=x (sum) s=x (rawsum) rs=x (count) n=x ///
        (nmissing) nm=x (nunique) nu=x (percent) pc=x (geomean) gm=x ///
        (gini) gi=x (gini dropneg) gd=x (gini keepneg) gk=x ///
        (p25) p=x (select2) sel=x (select-2) sn=x (rawselect2) rawsel=x ///
        [`weight'=w], by(g) freq(f) rawstat(raw*) double
    parqit save `"`stem'_`weight'.parquet"'
    parqit collect, clear
    assert m==2.4 & raw==1.75 & rs==7 & nu==5 & f==5 & p==1 & sel==1 & rawsel==1 if g==1
    assert s==cond("`weight'"=="aw",9.6,24) if g==1
    assert n==cond("`weight'"=="aw",4,10) if g==1
    assert nm==cond("`weight'"=="aw",1,5) if g==1
    assert abs(gi-.325)<1e-14 & gm==0 if g==1
    assert n==0 & s==0 & nu==1 & missing(m) & missing(gm) if g==2
    assert abs(gk-11/18)<1e-14 if g==3
    assert "`: type n'"=="double"
    parqit close _all
}

* Exclusions apply to raw statistics too; Boolean weight expressions are legal.
parqit use using `"`stem'.parquet"'
parqit replace w=0 if t==1
parqit replace w=. if t==2
parqit collapse (mean) raw=x (count) n=x [fw=w], by(g) rawstat(raw) freq(f)
parqit collect, clear
assert raw==3 & n==7 & f==3 if g==1
parqit close _all
parqit use using `"`stem'.parquet"'
parqit collapse (mean) m=x [fw=(t>4)], by(g) freq(f)
parqit collect, clear
assert f==1 & missing(m) if g==1
assert f==3 & m==2/3 if g==3
parqit close _all

* merge updates only the sample; noinit preserves previous outside values.
foreach mode in plain noinit {
    parqit use using `"`stem'.parquet"'
    local extra ""
    if ("`mode'"=="noinit") local extra noinit
    parqit collapse (mean) z=x if inlist(t,2,4), by(g) merge replace `extra'
    parqit collect, clear
    assert _N==10 & t==_n
    assert z==2.5 if inlist(t,2,4)
    if ("`mode'"=="plain") assert missing(z) if !inlist(t,2,4)
    else assert z==100+t if !inlist(t,2,4)
    parqit close _all
}

* sumcheck is proved from the materialised target, never a narrowing cast.
parqit use using `"`stem'.parquet"'
parqit collapse (sum) small=y big=y, by(g) sumcheck
parqit save `"`stem'_packed.parquet"'
parqit close _all
parqit use using `"`stem'_packed.parquet"'
parqit collect, clear
assert "`: type small'"=="byte" & small==12 if g==1
parqit close _all
parqit use using `"`stem'.parquet"'
parqit gen long large_y=1000000000
parqit collapse (sum) s=large_y, by(g) sumcheck
parqit collect, clear
assert "`: type s'"=="double" & s==5000000000 if g==1
parqit close _all
parqit use using `"`stem'.parquet"'
parqit collapse (sum) s=y, by(g) sumcheck double
parqit collect, clear
assert "`: type s'"=="double"
parqit close _all

* Labels are derived from the selected view's metadata, including a hook.
program define _v196_pretty, rclass
    args stat
    clear
    if ("`stat'"=="mean") return local prettystat "Average"
    else return local prettystat "#default#"
end
parqit use using `"`stem'.parquet"'
quietly datasignature
local before_label `"`r(datasignature)'"'
parqit collapse (mean) m=x (skew) sk=x, by(g) labelformat("#stat:pretty#: #sourcelabel#") labelprogram(_v196_pretty)
quietly datasignature
assert `"`r(datasignature)'"'==`"`before_label'"'
parqit collect, clear
if `"`: variable label m'"'!="Average: Source MEAN" display `"LABEL_OBSERVED: `: variable label m'"'
assert "`: variable label m'"=="Average: Source MEAN"
assert "`: variable label sk'"=="Skewness: Source SKEWNESS"
parqit close _all
parqit use using `"`stem'.parquet"'
parqit collapse (mean) m=y, by(g) labelformat("#sourcelabel:1:6# #STAT#")
parqit collect, clear
assert "`: variable label m'"=="Árvore MEAN"
parqit close _all

* A volatile source is shared by the original rows and grouped branch.
parqit sql "SELECT 1+i//5 AS g, i+1 AS t, random() AS draw FROM range(10) d(i)"
parqit collapse (mean) grouped=draw, by(g) merge
parqit collect, clear
egen double native_grouped=mean(draw), by(g)
assert reldif(grouped,native_grouped)<1e-14 & t==_n
parqit close _all

* A weight-looking literal in if is data, not a weight clause.
use `"`native'"', clear
gen str20 message=cond(t<=4,"[fw=w]","")
parqit open _data
parqit collapse (count) n=x if message=="[fw=w]", by(g) freq(f)
parqit collect, clear
assert _N==1 & g==1 & n==4 & f==4
parqit close _all

* Every failure is atomic, including failures that occur at materialisation.
parqit use using `"`stem'.parquet"'
quietly datasignature
local before_bad_weight `"`r(datasignature)'"'
capture noisily parqit collapse (mean) m=x [fw=w/2], by(g)
local request_rc=_rc
if (`request_rc'==0) {
    capture noisily parqit collect, clear
    assert _rc!=0
}
else assert `request_rc'!=0
quietly datasignature
assert `"`r(datasignature)'"'==`"`before_bad_weight'"'
* The original named view is a pipeline; a data-dependent refusal leaves it
* uncollected and never swaps a partially filled dataset into memory.
parqit close _all
parqit use using `"`stem'.parquet"'
foreach bad in "replace" "noinit" "rawstat(absent)" "merge" {
    capture noisily parqit collapse (sum) x=x, by(g) `bad'
    assert _rc!=0
    parqit count
    assert r(N)==10
}
capture noisily parqit collapse (sd) s=x [pw=w], by(g)
assert _rc!=0
parqit count
assert r(N)==10
parqit close _all

python:
from collections import defaultdict
from decimal import Decimal, localcontext
from sfi import Macro
import math, pyarrow.parquet as pq
stem=Macro.getLocal("stem")
source=pq.read_table(stem+".parquet").to_pylist()
groups=defaultdict(list)
for r in source: groups[r["g"]].append(r)
for kind in ["fw","aw","iw","pw"]:
    output=pq.read_table(stem+"_"+kind+".parquet").to_pylist()
    assert len(output)==len(groups)
    with localcontext() as ctx:
        ctx.prec=100
        for row in output:
            data=groups[row["g"]]
            nonmissing=[r for r in data if r["x"] is not None]
            mass=sum(Decimal.from_float(r["w"]) for r in nonmissing)
            total=sum(Decimal.from_float(r["w"])*Decimal.from_float(r["x"]) for r in nonmissing)
            assert row["nu"]==len({r["x"] for r in data})
            assert row["f"]==len(data)
            if not nonmissing:
                assert row["m"] is None
                continue
            mean=total/mass
            assert abs(row["m"]-float(mean))<=math.ulp(float(mean))
            want=float(total*len(nonmissing)/mass if kind=="aw" else total)
            assert abs(row["s"]-want)<=2*math.ulp(want)
            vals=sorted(nonmissing,key=lambda r:r["x"])
            # Pairwise definition of the weighted Gini, independent of ranks.
            for target,mode in [("gi","clamp"),("gd","drop"),("gk","keep")]:
                pairs=[(Decimal.from_float(r["w"]),Decimal.from_float(max(r["x"],0) if mode=="clamp" else r["x"])) for r in nonmissing if mode!="drop" or r["x"]>=0]
                W=sum(w for w,x in pairs)
                WX=sum(w*x for w,x in pairs)
                if W and WX:
                    truth=sum(wa*wb*abs(xa-xb) for wa,xa in pairs for wb,xb in pairs)/(2*W*WX)
                    assert abs(row[target]-float(truth))<2e-14,(kind,target,row[target],truth)
print("Independent Decimal-100/PyArrow oracles: PASS")
end
display "VERDICT(V196_COLLAPSE_DATA_OPTIONS): PASS"
