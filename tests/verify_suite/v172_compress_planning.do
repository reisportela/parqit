* Native compress is the independent storage/value/metadata oracle. Exercise
* multiple vectors, a late fractional value, missing companions and both paths.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
parqit set threads 4
set obs 70001
gen double b = cond(mod(_n,2),-127,100)
gen double i = cond(mod(_n,2),-128,32740)
gen double l = cond(mod(_n,2),-32768,2147483620)
gen double outside = 2147483621
gen double fraction = 1.25
gen double late = 1
replace late = .5 in L
gen double missing = .
gen double extended = cond(mod(_n,2),.a,.z)
gen float original_float = 100000000
gen double day = td(24oct2026)
format day %td
gen str20 text = cond(mod(_n,2),"ação","text")
gen strL longtext = "abc"
forvalues repeat=1/10 {
    quietly replace longtext=longtext+longtext
}
replace longtext="same" if mod(_n,2)
label variable b "Native compression boundary"
label define codes -127 "negative" 100 "positive"
label values b codes
char b[meaning] "preserved"
label data "Compression native oracle"
tempfile source native
parqit save "`source'.parquet", data xmissing
compress
foreach v of varlist _all {
    local want_`v' : type `v'
    local format_`v' : format `v'
}
save "`native'"
foreach route in eager lazy staged {
    parqit close _all
    if "`route'"=="eager" parqit use using "`source'.parquet", clear compress
    else {
        parqit use using "`source'.parquet"
        * Lazy views intentionally normalise extended missing codes to '.',
        * so test those only on the eager lossless read path.
        if "`route'"=="staged" parqit keep if b<=100
        parqit collect, clear compress
    }
    foreach v of varlist _all {
        if "`: type `v''"!="`want_`v''" error 9
        if "`: format `v''"!="`format_`v''" error 9
    }
    if "`route'"=="eager" cf _all using "`native'"
    else cf b i l outside fraction late missing original_float day text longtext using "`native'"
    if "`: variable label b'"!="Native compression boundary" error 9
    if "`: label (b) -127'"!="negative" error 9
    if "`: char b[meaning]'"!="preserved" error 9
    if "`: format day'"!="%td" error 9
}

* Foreign Parquet has no stored Stata format. Demotion must retain the default
* format of the original planned type, just as load followed by compress does.
tempfile plain plain_native
python:
from sfi import Macro
import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq
pq.write_table(pa.table({'x': np.arange(70001,dtype=np.float64)%101,
                        'fraction': np.full(70001,1.25),
                        'tiny': np.full(70001,np.nextafter(0.,1.)),
                        'wide': np.full(70001,2147483621.),
                        'empty_value': pa.array([None]*70001, type=pa.float64())}), Macro.getLocal('plain')+'.parquet')
end
parqit use using "`plain'.parquet", clear
compress
local native_x_format : format x
foreach v of varlist _all {
    local plain_type_`v' : type `v'
    local plain_format_`v' : format `v'
}
save "`plain_native'"
parqit use using "`plain'.parquet", clear compress
cf _all using "`plain_native'"
if "`:type x'"!="byte" error 9
if "`:format x'"!="`native_x_format'" error 9
foreach v of varlist _all {
    if "`:type `v''"!="`plain_type_`v''" error 9
    if "`:format `v''"!="`plain_format_`v''" error 9
}

* Arrow Date32 uses 1970; Stata uses 1960: add 3653, not subtract it. These
* independent fixtures straddle byte/int boundaries in the converted units.
tempfile dates dates_native
python:
raw=[0,32740,-3653,32740-3653,32741-3653,100-3653,101-3653,-127-3653,-128-3653,-32767-3653,-32768-3653]
columns={'d'+str(i):pa.array(np.full(70001,v,dtype=np.int32)).cast(pa.date32()) for i,v in enumerate(raw)}
pq.write_table(pa.table(columns),Macro.getLocal('dates')+'.parquet')
end
parqit use using "`dates'.parquet"
parqit collect, clear
if d0[1]!=td(01jan1970) | d2[1]!=0 error 9
compress
foreach v of varlist _all {
    local date_type_`v' : type `v'
    local date_format_`v' : format `v'
}
save "`dates_native'"
foreach route in eager lazy {
    if "`route'"=="eager" parqit use using "`dates'.parquet", clear compress
    else {
        parqit use using "`dates'.parquet"
        parqit collect, clear compress
    }
    cf _all using "`dates_native'"
    foreach v of varlist _all {
        if "`:type `v''"!="`date_type_`v''" error 9
        if "`:format `v''"!="`date_format_`v''" error 9
    }
}

tempfile infinite_date
python:
pq.write_table(pa.table({'bad_date':pa.array([2147483647],type=pa.int32()).cast(pa.date32())}),Macro.getLocal('infinite_date')+'.parquet')
end
clear
set obs 1
gen double sentinel=12345
capture noisily parqit use using "`infinite_date'.parquet", clear compress
if _rc==0 error 9
assert _N==1 & sentinel==12345

* A foreign column's default display format also follows the user's dp setting.
foreach decimal in period comma {
    set dp `decimal'
    parqit use using "`plain'.parquet", clear
    compress
    local wanted_format : format x
    parqit use using "`plain'.parquet", clear compress
    if "`:format x'"!="`wanted_format'" error 9
}
set dp period

* Empty/raw SQL stays native; an unproven variable never changes its type.
parqit sql "SELECT NULL::DOUBLE AS x WHERE false"
parqit collect, clear compress
if _N!=0 | "`:type x'"!="double" error 9
parqit sql "SELECT random()::DOUBLE AS x FROM range(70001)"
parqit collect, clear compress
if _N!=70001 | "`:type x'"!="double" error 9
assert x>=0 & x<1

* A fallible staged query must leave the live frame and its values untouched.
clear
set obs 1
gen double sentinel = 12345
parqit sql "SELECT i::DOUBLE AS x FROM range(70001) t(i)"
parqit query "WHERE CAST(CASE WHEN x=70000 THEN 'invalid' ELSE '1' END AS INTEGER)=1"
capture noisily parqit collect, clear compress
if _rc==0 error 9
assert _N==1 & sentinel==12345
parqit close _all
di "VERDICT(V172_COMPRESS_PLANNING): PASS - native integer demotions, fractional/empty fallbacks, metadata, extended missings and atomic failures"
