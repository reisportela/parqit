* ADVERSARIAL: join-key missing semantics. SQL joins never match NULLs;
* Stata's merge/joinby treat missing (and "") as ordinary values that DO
* match each other. parqit must follow Stata. Covers numeric missing keys,
* empty-string keys (NULL≡"" contract), multi-key with partial missing,
* m:1, joinby, and the documented extended-missing collapse on save.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

local fails 0
tempfile b1 b2 b3 b4 b5 b6
local mq  `"`b1'.parquet"'
local uq  `"`b2'.parquet"'
local msq `"`b3'.parquet"'
local usq `"`b4'.parquet"'
local mkq `"`b5'.parquet"'
local ukq `"`b6'.parquet"'

* ---------- numeric missing keys, 1:1 --------------------------------------
clear
input double k double xm
1 10
. 20
2 30
end
tempfile nm
qui save `"`nm'"'
parqit save `"`mq'"', replace
clear
input double k double xu
1 100
. 200
3 300
end
tempfile nu
qui save `"`nu'"'
parqit save `"`uq'"', replace

use `"`nm'"', clear
qui merge 1:1 k using `"`nu'"'
sort k xm
tempfile oracle
qui save `"`oracle'"'

parqit use using `"`mq'"'
parqit merge 1:1 k using `"`uq'"'
parqit sort k xm
parqit collect, clear
sort k xm
capture cf _all using `"`oracle'"'
if (_rc) {
    di as err "FAIL: numeric missing-key 1:1 merge differs from native"
    local ++fails
}
parqit close _all

* ---------- empty-string keys, m:1 -----------------------------------------
clear
input str4 g double xm
"a" 1
""  2
"a" 3
""  4
"b" 5
end
tempfile sm
qui save `"`sm'"'
parqit save `"`msq'"', replace
clear
input str4 g double w
"a" 10
""  20
end
tempfile su
qui save `"`su'"'
parqit save `"`usq'"', replace

use `"`sm'"', clear
qui merge m:1 g using `"`su'"'
sort g xm
qui save `"`oracle'"', replace

parqit use using `"`msq'"'
parqit merge m:1 g using `"`usq'"'
parqit sort g xm
parqit collect, clear
sort g xm
capture cf _all using `"`oracle'"'
if (_rc) {
    di as err "FAIL: empty-string-key m:1 merge differs from native"
    local ++fails
}
parqit close _all

* ---------- multi-key, partially missing ------------------------------------
clear
input double(k1 k2 xm)
1 1 10
1 . 20
. . 30
2 1 40
end
tempfile km
qui save `"`km'"'
parqit save `"`mkq'"', replace
clear
input double(k1 k2 xu)
1 1 100
1 . 200
. . 300
2 2 400
end
tempfile ku
qui save `"`ku'"'
parqit save `"`ukq'"', replace

use `"`km'"', clear
qui merge 1:1 k1 k2 using `"`ku'"'
sort k1 k2
qui save `"`oracle'"', replace

parqit use using `"`mkq'"'
parqit merge 1:1 k1 k2 using `"`ukq'"'
parqit sort k1 k2
parqit collect, clear
sort k1 k2
capture cf _all using `"`oracle'"'
if (_rc) {
    di as err "FAIL: multi-key partial-missing merge differs from native"
    local ++fails
}
parqit close _all

* ---------- joinby with missing keys ----------------------------------------
use `"`km'"', clear
qui joinby k1 using `"`ku'"', unmatched(none)
sort k1 k2 xm xu
qui save `"`oracle'"', replace

parqit use using `"`mkq'"'
parqit joinby k1 using `"`ukq'"'
parqit sort k1 k2 xm xu
parqit collect, clear
sort k1 k2 xm xu
* joinby brings both k2 columns? native renames; align variable sets
capture cf k1 xm xu using `"`oracle'"'
if (_rc) {
    di as err "FAIL: joinby with missing keys differs from native"
    local ++fails
}
parqit close _all

* ---------- extended missings: collapse on save must be LOUD ----------------
clear
input double k double x
1 10
2 20
end
replace k = .a in 1
replace k = .b in 2
tempname lg
local plog "`c(tmpdir)'/_parqit_v14_ext.log"
capture erase `"`plog'"'
log using `"`plog'"', text name(`lg')
parqit save `"`mq'"', replace
log close `lg'
mata: st_local("savetxt", invtokens(cat(st_local("plog"))', char(10)))
if (strpos(`"`savetxt'"', "extended missing") == 0) {
    di as err "FAIL: extended-missing collapse on save was silent"
    local ++fails
}
capture erase `"`plog'"'
* after the documented collapse both rows have k==. (one missing concept)
parqit use using `"`mq'"', clear
qui count if k == .
if (r(N) != 2) {
    di as err "FAIL: extended missings did not collapse to ."
    local ++fails
}

* ---------------------------------------------------------------------------
if (`fails' == 0) di "VERDICT(V14_JOIN_MISSING_KEYS): PASS - missing/empty keys join exactly like native Stata; extended-missing collapse is loud"
else {
    di as err "VERDICT(V14_JOIN_MISSING_KEYS): FAIL - `fails' check(s)"
    exit 9
}
