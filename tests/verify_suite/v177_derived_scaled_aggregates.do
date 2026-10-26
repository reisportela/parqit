* DERIVED-COLLAPSE-1: a certified row-local gen prefix keeps the exact scaled
* integer aggregation; division by zero, if, all-missing groups, float storage,
* subnormals, cancellation, a late wide exponent and both materialisers.
clear all
set more off
set linesize 255
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
parqit set threads 4
parqit version
local trace_required=regexm(`"`r(duckdb_version)'"',"^v?2[.]") & c(os)=="Unix"
tempfile source reference
python:
import os
from pathlib import Path
from sfi import Macro
import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq
groups=5000
k=1<<51
small=np.array([k,k+1,k+1],dtype=np.uint64).view(np.float64)
negative=(small.view(np.uint64)|(1<<63)).view(np.float64)
den=np.ones(3*groups);den[:3]=0.
table=pa.table({"g":np.repeat(np.arange(groups,dtype=np.int32),3),
 "strkey":pa.array([None if i%2==0 else "é" for i in np.repeat(np.arange(groups),3)]),
 "a":np.tile([1e16,1.,-1e16],groups),"b":den,
 "num":np.tile([3.,6.,9.],groups),"den4":np.full(3*groups,4.),
 "x":np.tile(small,groups),"negx":np.tile(negative,groups),
 "wide":np.tile([1e200,1.,-1e200],groups)})
pq.write_table(table,Macro.getLocal("source")+".parquet",row_group_size=8192)
os.environ["PARQIT_TEST_COMPACT_AGGREGATES"]="1"
def verify(prefix,disabled,trace_required):
 text=Path(prefix+"_trace_"+disabled+".log").read_text()
 required=trace_required=="1" and disabled=="0"
 assert ("PARQIT_COMPACT_AGGREGATES|" in text)==required
 if required:
  for token in ["r:0|","q:-2|","sx:-1074|","snegx:-1074|","qi:-2|"]:
   assert token in text,token
  assert "|w:" not in text and "|f:" not in text and "|wide:" not in text
 for mode in ["memory","view"]:
  result=pq.read_table(prefix+"_"+mode+"_"+disabled+".parquet")
  for column,bits in [("mean_sx",0x0008000000000001),("sum_sx",0x0018000000000002),
                      ("mean_negx",0x8008000000000001),("sum_negx",0x8018000000000002)]:
   assert np.all(result[column].to_numpy().view(np.uint64)==bits),(mode,column)
  if disabled=="0":
   old=pq.read_table(prefix+"_"+mode+"_1.parquet")
   assert result.schema==old.schema and result.equals(old),(mode,"fallback parity")
end

foreach disabled in 1 0 {
    python: os.environ["PARQIT_DISABLE_COMPACT_AGGREGATES"]=Macro.getLocal("disabled")
    parqit use using "`source'.parquet"
    parqit gen double r = a/b
    parqit gen double q = num/den4
    parqit gen double sx = x*1
    parqit gen double snegx = -(-negx)
    parqit gen double w = wide/1
    parqit gen double qi = num/den4 if num > 3
    parqit gen float f = num/den4
    parqit collapse (mean) mean_r=r mean_q=q mean_sx=sx mean_negx=snegx mean_qi=qi mean_f=f (sum) sum_r=r sum_q=q sum_sx=sx sum_negx=snegx sum_w=w sum_qi=qi (count) n_r=r n_qi=qi, by(g strkey)
    log using "`source'_trace_`disabled'.log", text name(v177)
    parqit collect, clear
    log close v177
    assert _N==5000 & c(k)==16
    assert mean_r==1/3 & sum_r==1 & n_r==3 if g!=0
    assert missing(mean_r) & sum_r==0 & n_r==0 if g==0
    assert mean_q==1.5 & sum_q==4.5 & sum_w==1
    assert mean_qi==1.875 & sum_qi==3.75 & n_qi==2
    assert mean_f==1.5
    assert "`: type mean_r'"=="double" & "`: type sum_q'"=="double" & "`: type n_r'"=="long"
    assert "`: variable label sum_r'"=="(sum) r" & "`: sortedby'"=="g strkey"
    if `disabled'==1 save "`reference'"
    else cf _all using "`reference'", all
    parqit save "`source'_memory_`disabled'.parquet", data
    parqit save "`source'_view_`disabled'.parquet", view
    python: verify(Macro.getLocal("source"),Macro.getLocal("disabled"),Macro.getLocal("trace_required"))
}

* A stage outside the certified prefix keeps the original accumulator and the same values.
python: os.environ["PARQIT_DISABLE_COMPACT_AGGREGATES"]="0"
foreach variant in rename rowctx keepif text {
    parqit use using "`source'.parquet"
    if "`variant'"=="keepif" parqit keep if num > 0
    if "`variant'"=="rowctx" parqit gen double r = a/b + 0*_n
    else parqit gen double r = a/b
    if "`variant'"=="rename" parqit rename r rr
    if "`variant'"=="text" parqit gen double t = strlen(strkey)
    local target = cond("`variant'"=="rename","rr","r")
    parqit collapse (mean) mean_r=`target' (sum) sum_r=`target', by(g strkey)
    log using "`source'_mut_`variant'.log", text name(v177m)
    parqit collect, clear
    log close v177m
    assert mean_r==1/3 & sum_r==1 if g!=0
    assert missing(mean_r) & sum_r==0 if g==0
    python: assert "PARQIT_COMPACT_AGGREGATES|" not in Path(Macro.getLocal("source")+"_mut_"+Macro.getLocal("variant")+".log").read_text()
}
* Case-distinct source names keep their bindings through the certified prefix,
* and a step after the collapse returns the whole plan to the general path.
tempfile aliases
python:
pq.write_table(pa.table({'g':np.repeat(np.arange(5000,dtype=np.int32),3),
 'x':np.tile([1.,2.,3.],5000),'X':np.tile([.25,.5,.75],5000)}),Macro.getLocal('aliases')+'.parquet')
end
foreach disabled in 1 0 {
    python: os.environ["PARQIT_DISABLE_COMPACT_AGGREGATES"]=Macro.getLocal("disabled")
    parqit use using "`aliases'.parquet"
    parqit gen double both = x + 4*X
    parqit collapse (mean) lower=x upper=X mboth=both (sum) sboth=both, by(g)
    log using "`aliases'_trace_`disabled'.log", text name(v177a)
    parqit collect, clear
    log close v177a
    assert lower==2 & upper==.5 & mboth==4 & sboth==12 & _N==5000
    python: assert ("PARQIT_COMPACT_AGGREGATES|" in Path(Macro.getLocal("aliases")+"_trace_"+Macro.getLocal("disabled")+".log").read_text())==(Macro.getLocal("trace_required")=="1" and Macro.getLocal("disabled")=="0")
    parqit use using "`source'.parquet"
    parqit gen double q = num/den4
    parqit collapse (mean) mean_q=q (sum) sum_q=q, by(g)
    parqit gen double twice = 2*sum_q
    log using "`source'_later_`disabled'.log", text name(v177l)
    parqit collect, clear
    log close v177l
    assert mean_q==1.5 & sum_q==4.5 & twice==9 & _N==5000
    python: assert "PARQIT_COMPACT_AGGREGATES|" not in Path(Macro.getLocal("source")+"_later_"+Macro.getLocal("disabled")+".log").read_text()
}
python:
os.environ.pop("PARQIT_TEST_COMPACT_AGGREGATES",None)
os.environ.pop("PARQIT_DISABLE_COMPACT_AGGREGATES",None)
end
parqit close _all
di "VERDICT(V177_DERIVED_SCALED_AGGREGATES): PASS - certified gen prefix dispatch, division by zero, if, all-missing group, float storage, subnormal single rounding, cancellation, late wide fallback, mutations and both materialisers"
