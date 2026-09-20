* V102 — MISSKEY-NOTE-1 / MERGE-ORDER-1 / r(459): what a lazy merge promises
* about its KEYS, pinned against native Stata run inside this very test.
* Findings T10, T11 and T15 of the triple-precision audit (2026-09-19);
* evidence in audit_repro/triple_precision_20260919/.
*
*   A  a missing key matches a missing key, exactly as native Stata does
*      ({1, ., 2} x {1, ., 3} -> matched 2), and the whole result is the
*      native result cell for cell. Guards against a regression to SQL
*      semantics, where NULL = NULL is false and the pair would vanish.
*   B  the FALSE match the documented .a-.z -> . collapse creates in a key,
*      and the note that now discloses it at join time. EXPECTED AND
*      ANNOUNCED, not a latent bug: a master keyed .a and a using keyed .
*      give parqit matched=2 / N=2 where native Stata (no bridge, .a intact)
*      gives matched=1 / N=3 — verified natively below. Parquet has one
*      missing concept, so the .a was already a plain . when it reached disk;
*      pq 4.0.2 does the same when both sides cross the bridge (a06_keys2).
*      What parqit adds is that the JOIN says so, to the reader, who may be
*      in another session and may not have written the file.
*   C  the order contract: the result equals native as a MULTISET, the
*      sortedby marker parqit declares is TRUE, and the same plan run twice
*      gives the same order. Order equality WITH native is deliberately NOT
*      asserted: the audit showed native merge's own within-key order changes
*      with the physical order of the using file, so it cannot be a contract.
*   D  a duplicated key in a 1:1 leaves _rc 459 — native Stata's own code for
*      this contract, confirmed natively in this test.
*   E  joinby honours the same missing-key rule (native: missing rows join
*      Cartesian-style) and gets the same note.
*
* Every native-parity claim runs native Stata here; nothing is taken on trust.
clear all
set more off
set varabbrev off
* Stata wraps output at linesize; the note is long, so keep the grepped
* fragment well inside the first line (run_stata.sh warns about long TMPDIRs
* for the same reason).
set linesize 244
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

* Does a text log contain a literal (non-regex) substring? r(found)=1/0.
capture program drop _v102_loghas
program define _v102_loghas, rclass
    version 16.0
    gettoken lf 0 : 0
    local pat = strtrim(`"`0'"')
    tempname fh
    local found 0
    file open `fh' using `"`lf'"', read text
    file read `fh' line
    while (r(eof)==0) {
        if (strpos(`"`line'"', `"`pat'"')) local found 1
        file read `fh' line
    }
    file close `fh'
    return scalar found = `found'
end

* Cell-exact MULTISET comparison of two .dta files. Numerics are taken to
* double (float->double is exact) and compared with ==, which in Stata is
* bit-exact and distinguishes . from .a; strings compare byte for byte. The
* canonical sort over every column makes it a multiset comparison, never an
* order assertion. Logic copied from the audit's _cmp.do so this test depends
* on nothing under audit_repro/. Returns r(ok) and r(msg).
capture program drop _v102_cmp
program define _v102_cmp, rclass
    version 16.0
    args fa fb
    tempfile pa pb
    quietly use `"`fa'"', clear
    local na = _N
    unab va : _all
    local sa : list sort va
    quietly use `"`fb'"', clear
    local nb = _N
    unab vb : _all
    local sb : list sort vb
    if (`na' != `nb') {
        return scalar ok = 0
        return local msg "rows `na' vs `nb'"
        exit
    }
    if (`"`sa'"' != `"`sb'"') {
        return scalar ok = 0
        return local msg "names (`sa') vs (`sb')"
        exit
    }
    foreach s in a b {
        quietly use `"`f`s''"', clear
        quietly ds, has(type numeric)
        if "`r(varlist)'" != "" quietly recast double `r(varlist)'
        sort `sa'
        quietly gen long __ord = _n
        foreach v of local sa {
            quietly rename `v' `s'_`v'
        }
        quietly save `"`p`s''"', replace
    }
    quietly use `"`pa'"', clear
    quietly merge 1:1 __ord using `"`pb'"', nogenerate
    local bad ""
    foreach v of local sa {
        quietly count if !(a_`v' == b_`v')
        if (r(N) > 0) local bad `"`bad' `v'(`r(N)')"'
    }
    return scalar ok = (`"`bad'"' == "")
    return local msg `"`bad'"'
end

tempfile t mdta udta Lnat Lpar Lpar2

* =====================================================================
* A — a missing key matches a missing key, as native Stata does
* =====================================================================
clear
input double k double xm
1 10
. 20
2 30
end
quietly save `"`mdta'"', replace
capture quietly parqit close _all
parqit save `"`t'_A_m.parquet"', replace data
clear
input double k double xu
1 100
. 200
3 300
end
quietly save `"`udta'"', replace
parqit save `"`t'_A_u.parquet"', replace data

* the oracle: native Stata on the equivalent .dta pair
use `"`mdta'"', clear
quietly merge 1:1 k using `"`udta'"'
quietly count if _merge == 3
local A_nat = r(N)
local A_natN = _N
quietly save `"`Lnat'"', replace

parqit use using `"`t'_A_m.parquet"'
parqit merge 1:1 k using `"`t'_A_u.parquet"'
parqit collect, clear
capture quietly parqit close _all
quietly count if _merge == 3
local A_par = r(N)
local A_parN = _N
quietly save `"`Lpar'"', replace

_v102_cmp `"`Lnat'"' `"`Lpar'"'
local A_cells = r(ok)
local A_msg `"`r(msg)'"'
capture assert `A_nat' == 2 & `A_par' == 2 & `A_natN' == `A_parN' & `A_cells' == 1
if (_rc) {
    di as err "V102-A: native matched=`A_nat' N=`A_natN'; parqit matched=`A_par'" ///
        " N=`A_parN'; cells ok=`A_cells'`A_msg'"
    di as err "VERDICT(v102_missing_key_matches_native): FAIL"
}
else di as txt "VERDICT(v102_missing_key_matches_native): PASS"

* =====================================================================
* B — the .a -> . collapse FALSELY matches in a key, and the join says so
* =====================================================================
clear
input double k double xm
1 10
2 30
end
quietly replace k = .a in 2
quietly save `"`mdta'"', replace
capture quietly parqit close _all
parqit save `"`t'_B_m.parquet"', replace data
clear
input double k double xu
1 100
2 200
end
quietly replace k = . in 2
quietly save `"`udta'"', replace
parqit save `"`t'_B_u.parquet"', replace data

* the oracle: native Stata, no bridge, so .a is still .a and does NOT match .
use `"`mdta'"', clear
quietly merge 1:1 k using `"`udta'"'
quietly count if _merge == 3
local B_nat = r(N)
local B_natN = _N

parqit use using `"`t'_B_m.parquet"'
log using `"`t'_B.log"', replace text name(v102B)
parqit merge 1:1 k using `"`t'_B_u.parquet"'
log close v102B
parqit collect, clear
capture quietly parqit close _all
quietly count if _merge == 3
local B_par = r(N)
local B_parN = _N
_v102_loghas `"`t'_B.log"' have a missing value in this key; missing matches missing
local B_note = r(found)
_v102_loghas `"`t'_B.log"' key k: 1 master and 1 using row(s)
local B_names = r(found)

capture assert `B_nat' == 1 & `B_natN' == 3 & `B_par' == 2 & `B_parN' == 2 ///
    & `B_note' == 1 & `B_names' == 1
if (_rc) {
    di as err "V102-B: native matched=`B_nat' N=`B_natN' (expected 1/3);" ///
        " parqit matched=`B_par' N=`B_parN' (expected 2/2);" ///
        " note=`B_note' names=`B_names'"
    di as err "VERDICT(v102_ext_missing_key_false_match_announced): FAIL"
}
else di as txt "VERDICT(v102_ext_missing_key_false_match_announced): PASS"

* No missing on the using side means no pairing is possible, so no note.
clear
input double k double xu
1 100
2 200
end
quietly save `"`udta'"', replace
capture quietly parqit close _all
parqit save `"`t'_B_u2.parquet"', replace data
parqit use using `"`t'_B_m.parquet"'
log using `"`t'_B2.log"', replace text name(v102B2)
parqit merge 1:1 k using `"`t'_B_u2.parquet"'
log close v102B2
capture quietly parqit close _all
_v102_loghas `"`t'_B2.log"' have a missing value in this key
local B2_note = r(found)
capture assert `B2_note' == 0
if (_rc) di as err "VERDICT(v102_note_only_when_both_sides_missing): FAIL (note=`B2_note')"
else di as txt "VERDICT(v102_note_only_when_both_sides_missing): PASS"

* =====================================================================
* C — order: multiset equality with native, a TRUE sortedby marker,
*     and the same order from the same plan twice
* =====================================================================
clear
input double firm double x
2 1
1 2
2 3
3 4
1 5
2 6
4 7
3 8
end
quietly save `"`mdta'"', replace
capture quietly parqit close _all
parqit save `"`t'_C_m.parquet"', replace data
clear
input double firm double w
1 10
2 20
3 30
4 40
end
quietly save `"`udta'"', replace
parqit save `"`t'_C_u.parquet"', replace data

use `"`mdta'"', clear
quietly merge m:1 firm using `"`udta'"'
quietly save `"`Lnat'"', replace

parqit use using `"`t'_C_m.parquet"'
parqit merge m:1 firm using `"`t'_C_u.parquet"'
parqit collect, clear
capture quietly parqit close _all
local C_sorted : sortedby
quietly save `"`Lpar'"', replace
* The declared marker must be TRUE. Sorting by the marker (with the physical
* position as a stable tiebreaker) must be a no-op; this holds for a compound
* marker too, which a per-variable monotonicity walk would not catch.
local C_marker_ok 1
if ("`C_sorted'" == "") local C_marker_ok 0
else {
    preserve
    quietly gen long __o = _n
    sort `C_sorted' __o
    capture assert __o == _n
    if (_rc) local C_marker_ok 0
    restore
}
* the same plan a second time must give the same order, cell for cell
parqit use using `"`t'_C_m.parquet"'
parqit merge m:1 firm using `"`t'_C_u.parquet"'
parqit collect, clear
capture quietly parqit close _all
quietly save `"`Lpar2'"', replace
quietly use `"`Lpar'"', clear
quietly gen long __ord = _n
quietly rename (firm x w _merge) (a_firm a_x a_w a_merge)
tempfile Lp1
quietly save `"`Lp1'"', replace
quietly use `"`Lpar2'"', clear
quietly gen long __ord = _n
quietly merge 1:1 __ord using `"`Lp1'"', nogenerate
quietly count if !(firm == a_firm) | !(x == a_x) | !(w == a_w) | !(_merge == a_merge)
local C_determ = (r(N) == 0)

_v102_cmp `"`Lnat'"' `"`Lpar'"'
local C_cells = r(ok)
local C_msg `"`r(msg)'"'
capture assert `C_cells' == 1 & `C_marker_ok' == 1 & `C_determ' == 1
if (_rc) {
    di as err "V102-C: multiset ok=`C_cells'`C_msg'; sortedby=(`C_sorted')" ///
        " true=`C_marker_ok'; deterministic=`C_determ'"
    di as err "VERDICT(v102_merge_order_contract): FAIL"
}
else di as txt "VERDICT(v102_merge_order_contract): PASS (sortedby `C_sorted')"

* =====================================================================
* D — a duplicated key in a 1:1 is r(459), native Stata's own code
* =====================================================================
clear
input double k double xm
1 10
1 11
end
quietly save `"`mdta'"', replace
capture quietly parqit close _all
parqit save `"`t'_D_m.parquet"', replace data
clear
input double k double xu
1 100
end
quietly save `"`udta'"', replace
parqit save `"`t'_D_u.parquet"', replace data

use `"`mdta'"', clear
capture merge 1:1 k using `"`udta'"'
local D_nat = _rc

parqit use using `"`t'_D_m.parquet"'
capture noisily parqit merge 1:1 k using `"`t'_D_u.parquet"'
local D_par = _rc
capture quietly parqit close _all
* the using side too: m:1 requires a unique using
clear
input double k double xu
1 100
1 101
end
quietly save `"`udta'"', replace
capture quietly parqit close _all
parqit save `"`t'_D_u2.parquet"', replace data
parqit use using `"`t'_D_m.parquet"'
capture noisily parqit merge m:1 k using `"`t'_D_u2.parquet"'
local D_using = _rc
capture quietly parqit close _all

capture assert `D_nat' == 459 & `D_par' == 459 & `D_using' == 459
if (_rc) {
    di as err "V102-D: native rc=`D_nat'; parqit master rc=`D_par'; parqit using rc=`D_using'"
    di as err "VERDICT(v102_not_unique_is_rc459): FAIL"
}
else di as txt "VERDICT(v102_not_unique_is_rc459): PASS"

* =====================================================================
* E — joinby follows the same missing-key rule and gets the same note
* =====================================================================
clear
input double k double xm
1 10
. 20
. 21
end
quietly save `"`mdta'"', replace
capture quietly parqit close _all
parqit save `"`t'_E_m.parquet"', replace data
clear
input double k double xu
1 100
. 200
end
quietly save `"`udta'"', replace
parqit save `"`t'_E_u.parquet"', replace data

* the oracle: native joinby pairs missing with missing, Cartesian-style
use `"`mdta'"', clear
quietly joinby k using `"`udta'"'
local E_natN = _N
quietly count if missing(k)
local E_natmiss = r(N)
quietly save `"`Lnat'"', replace

parqit use using `"`t'_E_m.parquet"'
log using `"`t'_E.log"', replace text name(v102E)
parqit joinby k using `"`t'_E_u.parquet"'
log close v102E
parqit collect, clear
capture quietly parqit close _all
local E_parN = _N
quietly count if missing(k)
local E_parmiss = r(N)
quietly save `"`Lpar'"', replace
_v102_loghas `"`t'_E.log"' key k: 2 master and 1 using row(s)
local E_note = r(found)

_v102_cmp `"`Lnat'"' `"`Lpar'"'
local E_cells = r(ok)
local E_msg `"`r(msg)'"'
capture assert `E_natN' == 3 & `E_parN' == 3 & `E_natmiss' == 2 & ///
    `E_parmiss' == 2 & `E_cells' == 1 & `E_note' == 1
if (_rc) {
    di as err "V102-E: native N=`E_natN' miss=`E_natmiss'; parqit N=`E_parN'" ///
        " miss=`E_parmiss'; cells ok=`E_cells'`E_msg'; note=`E_note'"
    di as err "VERDICT(v102_joinby_missing_key_matches_native): FAIL"
}
else di as txt "VERDICT(v102_joinby_missing_key_matches_native): PASS"

* =====================================================================
* F — KEYFOLD-1: the uniqueness contract and the missing-key note fold a
*     key EXACTLY as the join does. A third-party using file whose numeric
*     key holds NULL, inf and 1e308 — values Stata cannot hold, which the
*     join reads as ONE missing key — is not unique for m:1: r(459), never a
*     silent duplication of the master's missing-key row; the note counts
*     all three; a master view over such a file is not unique for 1:1; and
*     m:m (no contract) pairs the master's . with every one of them. The
*     contract used to fold only NaN, so inf and 1e308 passed it while the
*     join matched them (found 2026-09-20, reading the two predicates).
* =====================================================================
python:
import pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
t = Macro.getLocal("t")
pq.write_table(pa.table({
    "k": pa.array([1.0, None, float("inf"), 1e308], pa.float64()),
    "xu": pa.array([100.0, 200.0, 300.0, 400.0], pa.float64())}),
    t + "_F_u.parquet")
pq.write_table(pa.table({
    "k": pa.array([1.0, None, float("inf")], pa.float64()),
    "xm": pa.array([10.0, 20.0, 30.0], pa.float64())}),
    t + "_F_m.parquet")
end
clear
input double k double xm
1 10
. 20
2 30
end
capture quietly parqit close _all
parqit save `"`t'_F_m2.parquet"', replace data

parqit use using `"`t'_F_m2.parquet"'
capture noisily parqit merge m:1 k using `"`t'_F_u.parquet"'
local F_using = _rc
capture quietly parqit close _all

parqit use using `"`t'_F_m.parquet"'
capture noisily parqit merge 1:1 k using `"`t'_F_m2.parquet"'
local F_master = _rc
capture quietly parqit close _all

* the join's own rule, materialised: joinby (no uniqueness contract, native
* Cartesian pairing) pairs the master's . with NULL, inf and 1e308 alike —
* every using row is delivered, the three land under a missing key, and the
* disclosure counts all three (a refused merge prints no notes, so the count
* is read here)
parqit use using `"`t'_F_m2.parquet"'
log using `"`t'_F.log"', replace text name(v102F)
parqit joinby k using `"`t'_F_u.parquet"'
log close v102F
parqit collect, clear
capture quietly parqit close _all
_v102_loghas `"`t'_F.log"' key k: 1 master and 3 using row(s)
local F_note = r(found)
local F_jb_N = _N
quietly count if missing(k)
local F_jb_miss = r(N)
quietly summarize xu
local F_jb_sum = r(sum)

capture assert `F_using' == 459 & `F_master' == 459 & `F_note' == 1 ///
    & `F_jb_N' == 4 & `F_jb_miss' == 3 & `F_jb_sum' == 1000
if (_rc) {
    di as err "V102-F: using-side m:1 rc=`F_using' (expected 459); master-side 1:1" ///
        " rc=`F_master' (expected 459); note=`F_note'; joinby N=`F_jb_N'" ///
        " missing=`F_jb_miss' sum(xu)=`F_jb_sum' (expected 4/3/1000)"
    di as err "VERDICT(v102_contract_folds_keys_as_the_join): FAIL"
}
else di as txt "VERDICT(v102_contract_folds_keys_as_the_join): PASS"

capture quietly parqit close _all
di as txt "V102-DONE"
