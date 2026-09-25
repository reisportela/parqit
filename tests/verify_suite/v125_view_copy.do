* V125 — VIEW-COPY-1: `parqit use [varlist] using view:<source>, name(<new>)`
* copies the plan of an open view (never its data) into an independent view
* that shares the source's bridges. Oracles: native twins in memory, PyArrow.
* Also pins the audit fixes VIEWNAME-ALL-1 and PREFIX-RESTORE-1 (#165).
clear all
set more off
set varabbrev off
set linesize 255
set seed 125
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile stem

program define _v125_exists
    version 16.0
    args path expected
    capture confirm file `"`path'"'
    assert (_rc == 0) == `expected'
end

program define _v125_nbridges, rclass
    version 16.0
    local roots : dir `"`c(tmpdir)'"' dirs "_parqit_bridge_*"
    return scalar n = `: word count `roots''
end

* storage type, format, labels, notes and sort marker of the data in memory
program define _v125_meta, rclass
    version 16.0
    local sig
    foreach v of varlist _all {
        local sig `"`sig'|`v':`: type `v'':`: format `v'':`: value label `v'':`: variable label `v'':`: char `v'[note]':`: char `v'[note1]'"'
    }
    return local sig `"`sig'|sortedby:`: sortedby'"'
end

* does <needle> appear in <logfile>? (continuation lines are reassembled)
program define _v125_grep, rclass
    version 16.0
    args logfile needle
    tempname fh
    local found 0
    local cur ""
    file open `fh' using `"`logfile'"', read text
    file read `fh' line
    while (!r(eof)) {
        if (substr(`"`macval(line)'"', 1, 2) == "> ") {
            local cur `"`macval(cur)'`=substr(`"`macval(line)'"', 3, .)'"'
        }
        else {
            if (strpos(`"`macval(cur)'"', `"`needle'"')) local found 1
            local cur `"`macval(line)'"'
        }
        file read `fh' line
    }
    if (strpos(`"`macval(cur)'"', `"`needle'"')) local found 1
    file close `fh'
    return scalar found = `found'
end

* the parqit show text of the current view, without the log's own frame
program define _v125_show, rclass
    version 16.0
    tempfile lf
    tempname fh
    quietly log using `"`lf'"', text name(v125show) replace
    parqit show
    quietly log close v125show
    local sql
    local on 0
    file open `fh' using `"`lf'"', read text
    file read `fh' line
    while (!r(eof)) {
        if (substr(`"`macval(line)'"', 1, 10) == "-- source:") local on 1
        if (substr(`"`macval(line)'"', 1, 11) == "      name:") local on 0
        if (`on') local sql `"`macval(sql)'|`macval(line)'"'
        file read `fh' line
    }
    file close `fh'
    return local sql `"`macval(sql)'"'
end

* number of views, current view and plan of the current view
program define _v125_state, rclass
    version 16.0
    quietly parqit views
    local n = r(n_views)
    quietly parqit _dlgcontext parqit_views, report
    local cur "`r(view)'"
    _v125_show
    return local sig `"`n'|`cur'|`r(sql)'"'
end

* the command must fail with rc 198 and leave every view as it was
program define _v125_refuse
    version 16.0
    args cmd
    _v125_state
    local before `"`r(sig)'"'
    _v125_nbridges
    local nb = r(n)
    capture noisily `cmd'
    local rc = _rc
    _v125_state
    local after `"`r(sig)'"'
    _v125_nbridges
    assert `rc' == 198
    assert `"`after'"' == `"`before'"'
    assert r(n) == `nb'
end

* a 10 x 4 panel with UTF-8 text, labels, formats, notes and one missing wage
clear
set obs 40
gen long id = ceil(_n/4)
bysort id: gen int year = 2016 + _n
gen double wage = 10 + mod(_n * 7, 23)
replace wage = . in 5
gen str12 city = cond(mod(id, 2), "Braga", "Évora")
gen byte grp = mod(id, 3)
label define grpl 0 "zero" 1 "um" 2 "dois"
label values grp grpl
label variable wage "Salário"
format year %ty
gen long day = mdy(1, 1, year)
format day %td
char wage[note] "gross"
notes wage: hourly
sort id year
save `"`stem'_native.dta"'
parqit save `"`stem'_src.parquet"', data

* ---- 1. the copy and its source are independent, both ways -----------------
use `"`stem'_native.dta"', clear
keep if year >= 2018
sort id year
gen double w2 = wage * 2
save `"`stem'_A.dta"'
use `"`stem'_native.dta"', clear
keep if year >= 2018
sort id year
keep id year wage grp
keep if wage > 15
local nB = _N
save `"`stem'_B.dta"'

parqit use using `"`stem'_src.parquet"', name(A)
parqit keep if year >= 2018
parqit sort id year
quietly parqit views
local nviews = r(n_views)
parqit use id year wage grp using view:A, name(B)
assert r(k) == 4 & "`r(view)'" == "B" & "`r(source_view)'" == "A"
quietly parqit views
assert r(n_views) == `nviews' + 1
parqit keep if wage > 15
parqit view A: gen double w2 = wage * 2

parqit view A
parqit collect, clear
cf _all using `"`stem'_A.dta"'
parqit view B
parqit collect, clear
capture confirm variable w2
assert _rc == 111
assert c(k) == 4
cf _all using `"`stem'_B.dta"'
assert `"`: variable label wage'"' == "Salário"
assert "`: value label grp'" == "grpl" & "`: label grpl 2'" == "dois"

* ---- 2. metadata travel; an untouched copy reads like its source ------------
* an unsorted file, so the source is a pure passthrough (#38 direct read)
use `"`stem'_native.dta"', clear
gen double shuffle = runiform()
sort shuffle
drop shuffle
parqit save `"`stem'_unsorted.parquet"', data
parqit use using `"`stem'_unsorted.parquet"', name(P)
parqit use view:P, name(Q)
foreach v in P Q {
    parqit view `v'
    parqit describe
    assert r(n_steps) == 0
    _v125_show
    assert strpos(`"`r(sql)'"', "ORDER BY") == 0
}
parqit view P
parqit collect, clear
_v125_meta
local metaP `"`r(sig)'"'
save `"`stem'_P.dta"'
parqit view Q
parqit collect, clear
_v125_meta
assert `"`r(sig)'"' == `"`metaP'"'
cf _all using `"`stem'_P.dta"'

parqit view A
parqit use using view:A, name(A2)
parqit view A
parqit collect, clear
_v125_meta
local metaA `"`r(sig)'"'
parqit view A2
parqit collect, clear
_v125_meta
assert `"`r(sig)'"' == `"`metaA'"'

* ---- 3. closing the source leaves the copy usable ---------------------------
parqit close A
parqit view B
parqit collect, clear
cf _all using `"`stem'_B.dta"'

* ---- 4. bridges are shared and released by the last view --------------------
use `"`stem'_native.dta"', clear
parqit open _data, name(M)
local b1 `"`r(bridge)'"'
assert `"`b1'"' != ""
_v125_exists `"`b1'"' 1
parqit use using view:M, name(N)
parqit close M
_v125_exists `"`b1'"' 1
parqit view N
parqit collect, clear
cf _all using `"`stem'_native.dta"'
parqit close N
_v125_exists `"`b1'"' 0

* a chain M -> N -> O keeps the bridge until its last view, in any close order
foreach order in "M N O" "O N M" "N M O" {
    use `"`stem'_native.dta"', clear
    parqit open _data, name(M)
    local b `"`r(bridge)'"'
    parqit use using view:M, name(N)
    parqit use using view:N, name(O)
    local left : word count `order'
    foreach v of local order {
        local --left
        if (`left' == 0) {
            parqit view `v'
            parqit collect, clear
            cf _all using `"`stem'_native.dta"'
        }
        parqit close `v'
        _v125_exists `"`b'"' `=(`left' > 0)'
    }
}

* replacing the source keeps the copy on the old snapshot
use `"`stem'_native.dta"', clear
parqit open _data, name(M)
local b1 `"`r(bridge)'"'
parqit use using view:M, name(N)
replace wage = wage + 1000
parqit open _data, name(M)
local b2 `"`r(bridge)'"'
_v125_exists `"`b1'"' 1
parqit view N
parqit collect, clear
cf _all using `"`stem'_native.dta"'
parqit close N
_v125_exists `"`b1'"' 0
parqit close M
_v125_exists `"`b2'"' 0

* replacing a copy releases only the copy's references
use `"`stem'_native.dta"', clear
parqit open _data, name(M2)
local b2 `"`r(bridge)'"'
parqit use using view:M2, name(C)
parqit use using view:P, name(C)
_v125_exists `"`b2'"' 1
parqit view C
parqit collect, clear
cf _all using `"`stem'_P.dta"'
parqit close M2
_v125_exists `"`b2'"' 0

* an adapter (.dta) bridge follows the same rule
parqit use using `"`stem'_native.dta"', name(D)
local b3 `"`r(bridge)'"'
assert `"`b3'"' != ""
parqit use id wage using view:D, name(E)
parqit close D
_v125_exists `"`b3'"' 1
parqit view E
parqit collect, clear
assert _N == 40 & c(k) == 2
parqit close E
_v125_exists `"`b3'"' 0

* a source that embeds another view, and a copy used as a using side
clear
input long id double z
1 100
2 200
end
save `"`stem'_lk.dta"'
use `"`stem'_native.dta"', clear
merge m:1 id using `"`stem'_lk.dta"', keep(master match) nogenerate
sort id year
save `"`stem'_host.dta"'

use `"`stem'_lk.dta"', clear
parqit open _data, name(Lk)
local bl `"`r(bridge)'"'
parqit use using `"`stem'_src.parquet"', name(Host)
parqit merge m:1 id using view:Lk, keep(master match) nogenerate
parqit use using view:Host, name(HostCopy)
parqit close Lk
parqit close Host
_v125_exists `"`bl'"' 1
parqit view HostCopy
parqit collect, clear
sort id year
cf _all using `"`stem'_host.dta"'
parqit close HostCopy
_v125_exists `"`bl'"' 0

use `"`stem'_lk.dta"', clear
parqit open _data, name(Lk2)
local bl2 `"`r(bridge)'"'
parqit use using view:Lk2, name(LkCopy)
parqit close Lk2
parqit use using `"`stem'_src.parquet"', name(Host2)
parqit merge m:1 id using view:LkCopy, keep(master match) nogenerate
parqit close LkCopy
_v125_exists `"`bl2'"' 1
parqit view Host2
parqit collect, clear
sort id year
cf _all using `"`stem'_host.dta"'
parqit close Host2
_v125_exists `"`bl2'"' 0

* ---- 5. an unseeded sample keeps its draw in the copy -----------------------
parqit use using `"`stem'_src.parquet"', name(S)
parqit sample 30
parqit use using view:S, name(T)
parqit view S
parqit collect, clear
assert _N == 12
sort id year
save `"`stem'_S.dta"'
parqit view T
parqit collect, clear
sort id year
cf _all using `"`stem'_S.dta"'

* ---- 6. a pending keep in travels unvalidated and is checked at collect -----
use `"`stem'_native.dta"', clear
sort id year
keep in 5/10
save `"`stem'_K.dta"'
parqit use using `"`stem'_src.parquet"', name(K)
parqit sort id year
parqit keep in 5/10
parqit use using view:K, name(L)
parqit collect, clear
assert _N == 6
cf _all using `"`stem'_K.dta"'
parqit use using `"`stem'_src.parquet"', name(K2)
parqit keep in 1/100000
parqit use using view:K2, name(L2)
quietly datasignature
local sig0 `"`r(datasignature)'"'
capture noisily parqit collect, clear
local rc_copy = _rc
quietly datasignature
assert `rc_copy' != 0 & `"`r(datasignature)'"' == `"`sig0'"'
parqit view K2
capture noisily parqit collect, clear
local rc_source = _rc
quietly datasignature
assert `rc_source' == `rc_copy' & `"`r(datasignature)'"' == `"`sig0'"'

* ---- 7. refusals leave views, the current view, plans and bridges alone ----
parqit use using `"`stem'_src.parquet"', name(R)
parqit keep if id <= 3
_v125_refuse `"parqit use using view:R"'
_v125_refuse `"parqit use using view:R, name(R)"'
_v125_refuse `"parqit use using view:nosuch, name(X1)"'
_v125_refuse `"parqit use using view:, name(X2)"'
_v125_refuse `"parqit use using "view:R R", name(X3)"'
_v125_refuse `"parqit use nosuchvar using view:R, name(X4)"'
foreach opt in clear relaxed owned "encoding(latin1)" "int64(round)" "int64(nonsense)" ///
        "binary(hex)" "filename(src)" "csv(header=true)" {
    _v125_refuse `"parqit use using view:R, name(X5) `opt'"'
}
* VIEWNAME-ALL-1: _all is reserved by parqit close _all, for every opener
_v125_refuse `"parqit use using view:R, name(_all)"'
_v125_refuse `"parqit use using "`stem'_src.parquet", name(_all)"'
_v125_refuse `"parqit sql "select 1 as a", name(_all)"'
_v125_refuse `"parqit open _data, name(_all)"'
* mergein/appendin read a file: a view is refused by name, memory untouched
quietly datasignature
local sig0 `"`r(datasignature)'"'
tempfile lm
quietly log using `"`lm'"', text name(v125m) replace
_v125_refuse `"parqit mergein m:1 id using view:R"'
_v125_refuse `"parqit appendin using view:R"'
quietly log close v125m
quietly datasignature
assert `"`r(datasignature)'"' == `"`sig0'"'
_v125_grep `"`lm'"' "parqit mergein: the using side is a file on disk, not the open view R"
assert r(found)
_v125_grep `"`lm'"' "parqit appendin: the using side is a file on disk, not the open view R"
assert r(found)
quietly parqit count
assert r(N) == 12

* ---- 8. a Unicode view name crosses the hex wire -----------------------------
parqit use id year using view:R, name(médias)
assert "`r(view)'" == "médias"
parqit view médias
parqit collect, clear
assert _N == 12 & c(k) == 2
parqit close médias

* ---- 9. PREFIX-RESTORE-1: a prefixed copy restores the previous view -------
parqit use using `"`stem'_src.parquet"', name(PA)
parqit use using `"`stem'_src.parquet"', name(PZ)
parqit view PA: use id using view:PA, name(PC1)
quietly parqit _dlgcontext parqit_views, report
assert "`r(view)'" == "PZ"
parqit view PA
parqit view PA: use id using view:PA, name(PC2)
quietly parqit _dlgcontext parqit_views, report
assert "`r(view)'" == "PA"
* closing the prefixed view that was also the previous one restores nothing
parqit view PC2
parqit view PC2: close
quietly parqit _dlgcontext parqit_views, report
assert "`r(view)'" == ""

* ---- 10. the views listing keeps a copy's origin and its file name ---------
parqit use using view:PA, name(PL)
tempfile lv
quietly log using `"`lv'"', text name(v125v) replace
parqit views
quietly log close v125v
_v125_grep `"`lv'"' "view:PA ("
assert r(found)
_v125_grep `"`lv'"' "_src.parquet)"
assert r(found)

* ---- 11. a copy repeats the note on extended missing values -----------------
clear
input long id double v
1 1
2 .a
3 .b
end
parqit save `"`stem'_xm.parquet"', data xmissing
parqit use using `"`stem'_xm.parquet"', name(XM)
tempfile lx
quietly log using `"`lx'"', text name(v125x) replace
parqit use using view:XM, name(XC)
quietly log close v125x
_v125_grep `"`lx'"' "note: view:XM reads a file that keeps extended missing values (.a-.z) for v;"
assert r(found)
parqit view XC
parqit collect, clear
assert v[1] == 1 & v[2] == . & v[3] == .

* ---- 12. the manifest pattern: decide on a narrow copy, anti-join the source --
use `"`stem'_native.dta"', clear
replace id = . in 1/2
parqit save `"`stem'_aj.parquet"', data
drop if !missing(id) & mod(id, 3) != 0
sort id year wage
save `"`stem'_aj_expected.dta"'

parqit use using `"`stem'_aj.parquet"', name(panel)
parqit use id using view:panel, name(manifest)
parqit collect, clear
keep if !missing(id) & mod(id, 3) != 0
keep id
duplicates drop
parqit open _data, name(dropped)
parqit view panel: merge m:1 id using view:dropped, keep(master) nogenerate
parqit view panel
parqit collect, clear
sort id year wage
cf _all using `"`stem'_aj_expected.dta"'
assert missing(id) in -2/l

* ---- 13. a save from a copy, read back by PyArrow --------------------------
parqit view B
parqit save `"`stem'_Bsave.parquet"'
python:
import pyarrow.parquet as pq
from sfi import Macro
table = pq.read_table(Macro.getLocal('stem') + '_Bsave.parquet')
assert table.num_rows == int(Macro.getLocal('nB')), table.num_rows
assert table.column_names == ['id', 'year', 'wage', 'grp'], table.column_names
end

* ---- 14. close _all leaves no bridge behind ----------------------------------
use `"`stem'_native.dta"', clear
parqit open _data, name(Mz)
parqit use using view:Mz, name(Nz)
parqit use using `"`stem'_native.dta"', name(Dz)
parqit use using view:Dz, name(Ez)
parqit close _all
_v125_nbridges
assert r(n) == 0

* ---- 15. the count form repeats over row groups and threads -----------------
* The pinned engine runs a REPEATABLE reservoir sink serially (#165); a copy
* and repeated collects must draw the same rows.
clear
set obs 60000
gen long id = _n
gen double x = runiform()
parqit save `"`stem'_rg.parquet"', data chunk(2000)
python:
import pyarrow.parquet as pq
from sfi import Macro
assert pq.ParquetFile(Macro.getLocal('stem') + '_rg.parquet').num_row_groups == 30
end
parqit set threads 4
parqit use using `"`stem'_rg.parquet"', name(RG)
parqit sample 500, count
parqit use using view:RG, name(RGC)
parqit view RG
parqit collect, clear
sort id
save `"`stem'_rg1.dta"'
parqit collect, clear
sort id
cf _all using `"`stem'_rg1.dta"'
parqit view RGC
parqit collect, clear
sort id
cf _all using `"`stem'_rg1.dta"'
parqit close _all

di "VERDICT(V125_VIEW_COPY): PASS"
