* Real file-backed fused aggregates: null-safe numeric/string keys, duplicates,
* singleton sd, exact cancellation and declared order under the retained policy.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
parqit set threads 4
tempfile source
python:
from sfi import Macro
import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq
b=8192
g=[0.,-0.,float('nan'),None,1.,1.,2.,3.,3.]*b+[4.,5.,5.]
x=[1.,3.,5.,7.,None,None,42.,1.,3.]*b+[1.,1.,3.]
s=['a','a','',None,'b','b','c','d','d']*b+['e','f','f']
pq.write_table(pa.table({'id':np.arange(1,len(g)+1,dtype=np.int64),
                        'g':pa.array(g,pa.float64()),'x':pa.array(x,pa.float64()),'s':s}),Macro.getLocal('source')+'.parquet')
end
foreach key in g s {
    parqit use using "`source'.parquet"
    parqit sort id
    parqit egen double m=mean(x), by(`key')
    parqit egen double total=total(x), by(`key')
    parqit egen double n=count(x), by(`key')
    parqit egen double sd=sd(x), by(`key')
    parqit collect, clear
    assert _N==73731 & id==_n
    assert m==2 & total==32768 & n==16384 if g==0 | g==3
    assert reldif(sd,sqrt(16384/16383))<1e-15 if g==0 | g==3
    assert m==6 & total==98304 & n==16384 if missing(g)
    assert missing(m) & total==0 & n==0 & missing(sd) if g==1
    assert m==42 & total==344064 & n==8192 & sd==0 if g==2
    assert m==1 & n==1 & missing(sd) if g==4
    assert m==2 & n==2 & reldif(sd,sqrt(2))<1e-15 if g==5
}

* DOUBLE cancellation must retain the exact sum, not a rounded native SUM.
clear
set obs 120000
gen long id=_n
gen byte g=mod(floor((id-1)/3),17)
gen double x=cond(mod(id,3)==1,1e16,cond(mod(id,3)==2,1,-1e16))
tempfile cancel
parqit save "`cancel'.parquet", data
parqit use using "`cancel'.parquet"
parqit sort id
parqit egen double m=mean(x), by(g)
parqit egen double total=total(x), by(g)
parqit egen double n=count(x), by(g)
parqit collect, clear
assert id==_n & m==1/3 & total==n/3

* A dependent/qualified aggregate is sequential and must not enter the shortcut.
parqit use using "`source'.parquet"
parqit egen double m=mean(x), by(g)
parqit egen double n=count(x), by(g)
parqit egen double dependent=total(m) if g==0, by(g)
parqit keep if n>100
parqit collect, clear
assert dependent==32768 if g==0
assert missing(dependent) if g!=0
parqit close _all
di "VERDICT(V174_FUSED_FILE_AGGREGATES): PASS - numeric/string missing keys, signed zero, duplicates, singleton sd, exact cancellation, order and dependent qualifiers"
