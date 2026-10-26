* Native-domain checks plus exact rational oracles for count rounding and
* square roots. ASSUMPTIONS #134 takes precedence over native arithmetic bugs.
clear all
set more off
set linesize 255
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
parqit set threads 4
input byte g double x
1 0
1 0
1 0
2 1
2 1
2 1
3 .5
3 .5
3 .5
4 0
4 .5
4 1
5 -1
5 1
5 1
6 0
6 2
6 1
7 .2
7 .7
7 .9
8 .25
8 1.25
8 2.25
9 -1
9 2
9 5
10 1.1
10 1.1
10 1.1
11 0
11 .49
11 .49
12 0
12 .5
12 0
13 0
13 1e-300
13 0
14 .
14 .
14 .
15 -1
15 0
15 1
16 .49999999999999994
16 .
16 .
17 .5
17 .
17 .
18 .5000000000000001
18 .
18 .
19 1.4999999999999998
19 .
19 .
20 1.5
20 .
20 .
21 1.5000000000000002
21 .
21 .
22 8e307
22 8e307
22 .
23 8e307
23 8e307
23 8e307
24 -.0
24 0
24 1
25 .9999999999999998
25 0
25 .
26 .49999999999999994
26 1.3877787807814457e-17
26 1.3877787807814457e-17
end
tempfile original native stem
save "`original'"
parqit save "`stem'_source.parquet", data
collapse (sebinomial) native_sb=x (sepoisson) native_sp=x, by(g)
save "`native'"
parqit use using "`stem'_source.parquet"
parqit collapse (sebinomial) sb=x (sepoisson) sp=x, by(g)
parqit save "`stem'_result.parquet", view
parqit collect, clear
merge 1:1 g using "`native'", assert(match) nogen
assert missing(sb)==missing(native_sb)
assert reldif(sb,native_sb)<1e-15 if !missing(sb)
* Native intermediate rounding/overflow differs in these four pinned cases.
assert missing(sp)==missing(native_sp) if !inlist(g,16,22,23,26)
assert reldif(sp,native_sp)<1e-15 if !missing(sp) & !inlist(g,16,22,23,26)
assert sp==0 if inlist(g,16,26)
assert sp<. if inlist(g,22,23)
python:
from fractions import Fraction as F
import math, struct
import pyarrow.parquet as pq
from sfi import Macro, Data
source=pq.read_table(Macro.getLocal('stem')+'_source.parquet').to_pydict()
saved=pq.read_table(Macro.getLocal('stem')+'_result.parquet').to_pydict()
groups={}
for g,x in zip(source['g'],source['x']):
    groups.setdefault(g,[])
    if x is not None:groups[g].append(F(x))
def expected(xs):
    n=len(xs)
    if not n:return None,None
    sb=None
    if all(x in (0,1) for x in xs):
        successes=sum(xs)
        sb=successes*(n-successes)/n**3
    sp=None
    if all(x>=0 for x in xs):
        total=sum(xs,F(0))
        whole,remainder=divmod(total.numerator,total.denominator)
        whole+=2*remainder>=total.denominator
        sp=F(whole,n*n)
    return sb,sp
def check(got,squared):
    if squared is None:
        assert got is None
        return
    assert got is not None and math.isfinite(got) and got>=0
    if not squared:
        assert got==0 and math.copysign(1,got)==1
        return
    value=F(got)
    lower=(F(math.nextafter(got,0))+value)/2
    upper=(F(math.nextafter(got,math.inf))+value)/2
    bits=struct.unpack('<Q',struct.pack('<d',got))[0]
    assert lower*lower<=squared<=upper*upper
    if squared in (lower*lower,upper*upper):assert bits%2==0
for g,b,p in zip(saved['g'],saved['sb'],saved['sp']):
    eb,ep=expected(groups[g]);check(b,eb);check(p,ep)
for g,b,p in Data.get(var=['g','sb','sp'],missingval=None):
    eb,ep=expected(groups[g]);check(b,eb);check(p,ep)
end
parqit close _all

* The qualifier changes the validation sample; pivot applies the same rules
* separately to every cell. These are ordinary native cases, no overflow.
use "`original'", clear
keep if g<=15
expand 2, generate(j)
tempfile panel
parqit save "`panel'.parquet", data
collapse (sebinomial) sb=x (sepoisson) sp=x if x>=0, by(g j)
reshape wide sb sp, i(g) j(j)
save "`native'", replace
parqit use using "`panel'.parquet"
parqit pivot (sebinomial) sb=x (sepoisson) sp=x if x>=0, rows(g) cols(j)
parqit collect, clear
rename (sb0 sb1 sp0 sp1) (got_sb0 got_sb1 got_sp0 got_sp1)
merge 1:1 g using "`native'", assert(match) nogen
foreach v in sb0 sb1 sp0 sp1 {
    assert missing(got_`v')==missing(`v')
    assert reldif(got_`v',`v')<1e-15 if !missing(`v')
}
parqit close _all
di "VERDICT(V171_STANDARD_ERRORS): PASS - native domains and exact rounded totals, roots, save/collect and pivot"
