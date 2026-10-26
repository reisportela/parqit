* Exact scaled integer aggregation: real materialiser dispatch, native fallback,
* cancellation, subnormal double-rounding, signed zero and save/collect fidelity.
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
tempfile source reference late late_reference
python:
import os
from pathlib import Path
from fractions import Fraction
from sfi import Macro
import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq
groups=5000
k=1<<51
small=np.array([k,k+1,k+1],dtype=np.uint64).view(np.float64)
negative=(small.view(np.uint64)|(1<<63)).view(np.float64)
zero_values=[None,None,None]+[-0.,0.,None]*(groups-1)
table=pa.table({"g":np.repeat(np.arange(groups,dtype=np.int32),3),
 "strkey":pa.array([None if i%2==0 else "é" for i in np.repeat(np.arange(groups),3)]),
 "a":np.tile([1e16,1.,-1e16],groups),
 "q":np.tile([-2147483648.,0.,2147483647.],groups),
 "x":np.tile(small,groups),"negx":np.tile(negative,groups),
 "hi":np.tile([float(2**64),float(2**63),2049.],groups),
 "z":pa.array(zero_values,pa.float64()),
 "wide":np.tile([1e200,1.,-1e200],groups)})
pq.write_table(table,Macro.getLocal("source")+".parquet",row_group_size=8192)
values=np.ones(100000,dtype=np.float64);values[-1]=1e200
pq.write_table(pa.table({"g":np.arange(100000,dtype=np.int32)%groups,"late":values}),Macro.getLocal("late")+".parquet")
os.environ["PARQIT_TEST_COMPACT_AGGREGATES"]="1"
def verify_outputs(prefix,disabled,trace_required):
 text=Path(prefix+"_trace_"+disabled+".log").read_text()
 required=trace_required=="1" and disabled=="0"
 assert ("PARQIT_COMPACT_AGGREGATES|" in text)==required
 if required:
  assert "a:0|" in text and "q:integer32|" in text and "x:-1074|" in text and "z:0|" in text
 for mode in ["memory","view"]:
  result=pq.read_table(prefix+"_"+mode+"_"+disabled+".parquet")
  for column,bits in [("mean_x",0x0008000000000001),("sum_x",0x0018000000000002),
                      ("mean_negx",0x8008000000000001),("sum_negx",0x8018000000000002),("sum_z",0)]:
   assert np.all(result[column].to_numpy().view(np.uint64)==bits),(mode,column)
  assert result['mean_z'].null_count==1
  assert np.all(result['mean_z'].drop_null().to_numpy().view(np.uint64)==0)
  assert np.all(result['sum_hi'].to_numpy()==float(2**64+2**63+2049))
  assert np.all(result['mean_hi'].to_numpy()==float(Fraction(2**64+2**63+2049,3)))
  if disabled=="0":
   old=pq.read_table(prefix+"_"+mode+"_1.parquet")
   assert result.schema==old.schema and result.equals(old),(mode,"fallback parity")
end

foreach disabled in 1 0 {
    python: os.environ["PARQIT_DISABLE_COMPACT_AGGREGATES"]=Macro.getLocal("disabled")
    parqit use using "`source'.parquet"
    parqit format a %15.0g
    parqit collapse (mean) mean_a=a mean_q=q mean_x=x mean_negx=negx mean_hi=hi mean_z=z (sum) sum_a=a sum_q=q sum_x=x sum_negx=negx sum_hi=hi sum_z=z sum_wide=wide (count) count_z=z, by(g strkey)
    log using "`source'_trace_`disabled'.log", text name(v176)
    parqit collect, clear
    log close v176
    assert _N==5000 & c(k)==16
    assert mean_a==1/3 & mean_q==-1/3 & sum_a==1 & sum_q==-1
    assert sum_wide==1 & sum_z==0
    assert mean_z==0 & count_z==2 if g!=0
    assert missing(mean_z) & count_z==0 if g==0
    assert "`: type mean_a'"=="double" & "`: type sum_a'"=="double"
    assert "`: format mean_a'"=="%15.0g" & "`: variable label sum_a'"=="(sum) a"
    assert "`: sortedby'"=="g strkey"
    if `disabled'==1 save "`reference'"
    else cf _all using "`reference'", all
    parqit save "`source'_memory_`disabled'.parquet", data
    parqit save "`source'_view_`disabled'.parquet", view
    python: verify_outputs(Macro.getLocal("source"),Macro.getLocal("disabled"),Macro.getLocal("trace_required"))
}

* Mutations outside the captured first-collapse plan retain the general path.
parqit use using "`source'.parquet"
parqit rename a amount
parqit collapse (sum) total=amount, by(g)
parqit collect, clear
assert total==1 & _N==5000
parqit use using "`source'.parquet"
parqit collapse (sum) total=a other=q, by(g)
parqit drop other
parqit collect, clear
assert total==1 & c(k)==2
parqit save "`source'_drop.parquet", view

* Case-distinct names are resolved twice without changing their bindings.
tempfile aliases
python:
pq.write_table(pa.table({'g':np.repeat(np.arange(5000,dtype=np.int32),3),
 'x':np.tile([1.,2.,3.],5000),'X':np.tile([.25,.5,.75],5000)}),Macro.getLocal('aliases')+'.parquet')
end
parqit use using "`aliases'.parquet"
parqit collapse (mean) lower=x upper=X, by(g)
parqit collect, clear
assert lower==2 & upper==.5 & _N==5000

* The sample looks safe; a late wide exponent must disqualify the full proof.
foreach disabled in 1 0 {
    python: os.environ["PARQIT_DISABLE_COMPACT_AGGREGATES"]=Macro.getLocal("disabled")
    parqit use using "`late'.parquet"
    parqit collapse (sum) total=late, by(g)
    log using "`late'_trace_`disabled'.log", text name(v176late)
    parqit collect, clear
    log close v176late
    if `disabled'==1 save "`late_reference'"
    else cf _all using "`late_reference'", all
    python: assert "PARQIT_COMPACT_AGGREGATES|" not in Path(Macro.getLocal("late")+"_trace_"+Macro.getLocal("disabled")+".log").read_text()
}
python:
os.environ.pop("PARQIT_TEST_COMPACT_AGGREGATES",None)
os.environ.pop("PARQIT_DISABLE_COMPACT_AGGREGATES",None)
end
parqit close _all
di "VERDICT(V176_NATIVE_SCALED_AGGREGATES): PASS - actual dispatch, exact cancellation, subnormal single rounding, signed zero, native fallback, types/metadata and both materialisers"
