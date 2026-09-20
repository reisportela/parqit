* V104 — int64(refuse|round|string): the protective default for integers
* beyond 2^53 (INT64-PROTECT-1; finding T12 of the triple-precision audit,
* 2026-09-19, whose evidence is in audit_repro/triple_precision_20260919/).
*
* Stata's widest exact integer is 2^53. parqit used to load a BIGINT/UBIGINT
* column that goes beyond it as a rounded double with a note: two distinct
* keys could become one observation in memory. The default now REFUSES the
* read; the user chooses:
*
*   int64(refuse)  (default) rc 198 naming every offending column and both
*                  remedies; NOTHING is staged, the data in memory is intact
*   int64(round)   the historical behaviour — nearest double + the loud note
*   int64(string)  the affected columns arrive as EXACT decimal text; columns
*                  of the same family whose values all fit stay numeric
*
* Previews (parqit head/list) always show the exact digits — never a refusal,
* never a rounded number. `parqit set int64` moves the session default.
*
* The sources cannot be produced by Stata (its widest exact integer IS 2^53),
* so they are generated with parqit's own SQL, as v103 does, and every exact
* payload claim is checked against an INDEPENDENT oracle (pyarrow).
clear all
set more off
set varabbrev off
set linesize 244
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

capture program drop _v104_loghas
program define _v104_loghas, rclass
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

* a sentinel dataset: every refusal below must leave it exactly as it is
capture program drop _v104_sentinel
program define _v104_sentinel
    version 16.0
    clear
    quietly set obs 3
    quietly generate long sentinel = 100 * _n
end

tempfile t

* ---- the sources -----------------------------------------------------------
* bk: 2^53 and 2^53+1 (distinct on disk, one double in memory); n: an ordinary
* small integer that must stay numeric whatever happens to bk
capture quietly parqit close _all
parqit sql "SELECT * FROM (VALUES (9007199254740992::BIGINT, 'A', 1), (9007199254740993::BIGINT, 'B', 2)) t(bk, s, n)"
parqit save `"`t'_b.parquet"', replace
capture quietly parqit close _all
* the signed extremes: 19 digits, and 20 with the minus sign (INT64_MIN is
* written as -(INT64_MAX) because the engine parses the leading minus as
* negation of a literal that no longer fits INT64)
parqit sql "SELECT * FROM (VALUES (9223372036854775807::BIGINT), (-9223372036854775807::BIGINT)) t(ek)"
parqit save `"`t'_e.parquet"', replace
capture quietly parqit close _all
* unsigned 64-bit, the same family through __parqit_double
parqit sql "SELECT * FROM (VALUES (18446744073709551615::UBIGINT), (1::UBIGINT)) t(uk)"
parqit save `"`t'_u.parquet"', replace
capture quietly parqit close _all
* a wide DECIMAL: the same family (its integer part can exceed 2^53), and the
* one that arrives already carrying a "decimal converted to double" note
parqit sql "SELECT * FROM (VALUES (12345678901234567.89::DECIMAL(20,2)), (1.50::DECIMAL(20,2))) t(d)"
parqit save `"`t'_d.parquet"', replace
capture quietly parqit close _all
* the control: a BIGINT whose values all fit in 2^53 — nothing may change
parqit sql "SELECT * FROM (VALUES (9007199254740991::BIGINT, 7), (5::BIGINT, 8)) t(ck, m)"
parqit save `"`t'_c.parquet"', replace
capture quietly parqit close _all

* independent oracle: the exact digits and widths on disk
python:
import pyarrow.parquet as pq
from sfi import Macro
b = Macro.getLocal("t")
bk = pq.read_table(b + "_b.parquet").to_pydict()["bk"]
ek = pq.read_table(b + "_e.parquet").to_pydict()["ek"]
uk = pq.read_table(b + "_u.parquet").to_pydict()["uk"]
Macro.setLocal("Obk1", str(bk[0]))
Macro.setLocal("Obk2", str(bk[1]))
Macro.setLocal("Obkw", str(max(len(str(v)) for v in bk)))
Macro.setLocal("Oekw", str(max(len(str(v)) for v in ek)))
Macro.setLocal("Ouk1", str(max(uk)))
Macro.setLocal("Oukw", str(max(len(str(v)) for v in uk)))
Macro.setLocal("Oexact", "1" if (bk == [9007199254740992, 9007199254740993] and
                                 ek == [9223372036854775807, -9223372036854775807] and
                                 max(uk) == 18446744073709551615) else "0")
end
capture assert "`Oexact'" == "1"
if (_rc) di as err "VERDICT(v104_sources_exact_on_disk): FAIL"
else di as txt "VERDICT(v104_sources_exact_on_disk): PASS (bk width `Obkw', ek `Oekw', uk `Oukw')"

* =====================================================================
* A — the default refuses, names the column, and touches nothing
* =====================================================================
_v104_sentinel
log using `"`t'_A.log"', replace text name(v104A)
capture noisily parqit use `"`t'_b.parquet"', clear
local A_rc = _rc
log close v104A
capture quietly parqit close _all
_v104_loghas `"`t'_A.log"' hold integer values beyond 2^53
local A_msg = r(found)
_v104_loghas `"`t'_A.log"' column(s) bk
local A_names = r(found)
_v104_loghas `"`t'_A.log"' int64(string)
local A_rem1 = r(found)
_v104_loghas `"`t'_A.log"' int64(round)
local A_rem2 = r(found)
local A_intact = (_N == 3 & c(k) == 1 & sentinel[3] == 300)
capture assert `A_rc' == 198 & `A_msg' == 1 & `A_names' == 1 & ///
    `A_rem1' == 1 & `A_rem2' == 1 & `A_intact' == 1
if (_rc) {
    di as err "V104-A: rc=`A_rc' msg=`A_msg' names=`A_names'" ///
        " remedies=`A_rem1'/`A_rem2' intact=`A_intact'"
    di as err "VERDICT(v104_default_refuses_and_names_columns): FAIL"
}
else di as txt "VERDICT(v104_default_refuses_and_names_columns): PASS"

* the same protection on the lazy materialiser, with its own command name
_v104_sentinel
capture quietly parqit close _all
quietly parqit use using `"`t'_b.parquet"'
log using `"`t'_A2.log"', replace text name(v104A2)
capture noisily parqit collect, clear
local A2_rc = _rc
log close v104A2
capture quietly parqit close _all
_v104_loghas `"`t'_A2.log"' parqit collect: column(s) bk
local A2_msg = r(found)
local A2_intact = (_N == 3 & c(k) == 1 & sentinel[1] == 100)
capture assert `A2_rc' == 198 & `A2_msg' == 1 & `A2_intact' == 1
if (_rc) {
    di as err "V104-A2: rc=`A2_rc' msg=`A2_msg' intact=`A2_intact'"
    di as err "VERDICT(v104_collect_refuses_too): FAIL"
}
else di as txt "VERDICT(v104_collect_refuses_too): PASS"

* =====================================================================
* B — int64(round) is exactly the old behaviour: note + documented collapse
* =====================================================================
log using `"`t'_B.log"', replace text name(v104B)
parqit use `"`t'_b.parquet"', clear int64(round)
log close v104B
capture quietly parqit close _all
_v104_loghas `"`t'_B.log"' values beyond 2^53 rounded to nearest double
local B_note = r(found)
local B_type : type bk
quietly duplicates report bk
local B_distinct = r(unique_value)
capture assert `B_note' == 1 & `B_distinct' == 1 & _N == 2 & ///
    "`B_type'" == "double" & n[1] == 1 & n[2] == 2
if (_rc) {
    di as err "V104-B: note=`B_note' distinct=`B_distinct' type=`B_type' N=" _N
    di as err "VERDICT(v104_round_keeps_documented_behaviour): FAIL"
}
else di as txt "VERDICT(v104_round_keeps_documented_behaviour): PASS"

* =====================================================================
* C — int64(string): exact digits, exact width, neighbours untouched
* =====================================================================
log using `"`t'_C.log"', replace text name(v104C)
parqit use `"`t'_b.parquet"', clear int64(string)
log close v104C
capture quietly parqit close _all
_v104_loghas `"`t'_C.log"' loaded as text because values exceed 2^53
local C_note = r(found)
local C_type : type bk
local C_ntype : type n
local C_stype : type s
quietly count if bk == "`Obk1'" | bk == "`Obk2'"
local C_exact = r(N)
quietly duplicates report bk
local C_distinct = r(unique_value)
capture assert `C_note' == 1 & "`C_type'" == "str`Obkw'" & `C_exact' == 2 & ///
    `C_distinct' == 2 & "`C_ntype'" == "byte" & "`C_stype'" == "str1" & _N == 2
if (_rc) {
    di as err "V104-C: note=`C_note' type=`C_type' (expected str`Obkw')" ///
        " exact=`C_exact'/2 distinct=`C_distinct' n=`C_ntype' s=`C_stype'"
    di as err "VERDICT(v104_string_is_exact_text): FAIL"
}
else di as txt "VERDICT(v104_string_is_exact_text): PASS (`C_type')"

* the signed extremes: str19 for the max, str20 once the sign is there
parqit use `"`t'_e.parquet"', clear int64(string)
capture quietly parqit close _all
local E_type : type ek
quietly count if ek == "9223372036854775807" | ek == "-9223372036854775807"
local E_exact = r(N)
capture assert "`E_type'" == "str`Oekw'" & `E_exact' == 2 & _N == 2
if (_rc) {
    di as err "V104-E: type=`E_type' (expected str`Oekw') exact=`E_exact'/2"
    di as err "VERDICT(v104_int64_extremes_exact): FAIL"
}
else di as txt "VERDICT(v104_int64_extremes_exact): PASS (`E_type')"

* unsigned 64-bit takes the same three modes
_v104_sentinel
capture noisily parqit use `"`t'_u.parquet"', clear
local U_rc = _rc
local U_intact = (_N == 3 & c(k) == 1)
parqit use `"`t'_u.parquet"', clear int64(string)
capture quietly parqit close _all
local U_type : type uk
quietly count if uk == "`Ouk1'"
local U_exact = r(N)
capture assert `U_rc' == 198 & `U_intact' == 1 & "`U_type'" == "str`Oukw'" & ///
    `U_exact' == 1 & _N == 2
if (_rc) {
    di as err "V104-U: rc=`U_rc' intact=`U_intact' type=`U_type'" ///
        " (expected str`Oukw') exact=`U_exact'/1"
    di as err "VERDICT(v104_ubigint_same_contract): FAIL"
}
else di as txt "VERDICT(v104_ubigint_same_contract): PASS (`U_type')"

* a wide DECIMAL follows the same contract, and its note says ONE thing: the
* "decimal converted to double" it arrives with is replaced, not appended to
_v104_sentinel
capture noisily parqit use `"`t'_d.parquet"', clear
local W_rc = _rc
local W_intact = (_N == 3 & c(k) == 1)
log using `"`t'_W.log"', replace text name(v104W)
parqit use `"`t'_d.parquet"', clear int64(string)
log close v104W
capture quietly parqit close _all
_v104_loghas `"`t'_W.log"' loaded as text because values exceed 2^53
local W_text = r(found)
_v104_loghas `"`t'_W.log"' converted to double
local W_stale = r(found)
local W_type : type d
quietly count if d == "12345678901234567.89"
local W_exact = r(N)
capture assert `W_rc' == 198 & `W_intact' == 1 & `W_text' == 1 & ///
    `W_stale' == 0 & `W_exact' == 1 & _N == 2
if (_rc) {
    di as err "V104-W: rc=`W_rc' intact=`W_intact' text-note=`W_text'" ///
        " stale-note=`W_stale' type=`W_type' exact=`W_exact'/1"
    di as err "VERDICT(v104_wide_decimal_same_contract): FAIL"
}
else di as txt "VERDICT(v104_wide_decimal_same_contract): PASS (`W_type')"

* =====================================================================
* D — the lazy path: the option on collect, and carried from the open
* =====================================================================
capture quietly parqit close _all
quietly parqit use using `"`t'_b.parquet"'
parqit collect, clear int64(string)
capture quietly parqit close _all
local D_type : type bk
quietly count if bk == "`Obk1'" | bk == "`Obk2'"
local D_exact = r(N)

capture quietly parqit close _all
quietly parqit use using `"`t'_b.parquet"', int64(string)
parqit collect, clear
capture quietly parqit close _all
local D2_type : type bk
quietly count if bk == "`Obk1'" | bk == "`Obk2'"
local D2_exact = r(N)
capture assert "`D_type'" == "str`Obkw'" & `D_exact' == 2 & ///
    "`D2_type'" == "str`Obkw'" & `D2_exact' == 2
if (_rc) {
    di as err "V104-D: collect=`D_type'/`D_exact' carried=`D2_type'/`D2_exact'"
    di as err "VERDICT(v104_lazy_string_on_collect_and_open): FAIL"
}
else di as txt "VERDICT(v104_lazy_string_on_collect_and_open): PASS"

* =====================================================================
* E — a preview never refuses and never rounds: it shows the digits
* =====================================================================
capture quietly parqit close _all
quietly parqit use using `"`t'_b.parquet"'
log using `"`t'_H.log"', replace text name(v104H)
capture noisily parqit head 2
local H_rc = _rc
capture noisily parqit list
local L_rc = _rc
log close v104H
capture quietly parqit close _all
_v104_loghas `"`t'_H.log"' `Obk2'
local H_digits = r(found)
_v104_loghas `"`t'_H.log"' hold integer values beyond 2^53
local H_refused = r(found)
_v104_loghas `"`t'_H.log"' 9.007e+15
local H_rounded = r(found)
capture assert `H_rc' == 0 & `L_rc' == 0 & `H_digits' == 1 & ///
    `H_refused' == 0 & `H_rounded' == 0
if (_rc) {
    di as err "V104-H: head rc=`H_rc' list rc=`L_rc' digits=`H_digits'" ///
        " refused=`H_refused' rounded=`H_rounded'"
    di as err "VERDICT(v104_preview_shows_exact_digits): FAIL"
}
else di as txt "VERDICT(v104_preview_shows_exact_digits): PASS"

* =====================================================================
* F — parqit set int64 moves the default; an option still wins
* =====================================================================
parqit set int64 string
parqit use `"`t'_b.parquet"', clear
capture quietly parqit close _all
local F_type : type bk
parqit use `"`t'_b.parquet"', clear int64(round)
capture quietly parqit close _all
local F_optwins : type bk
parqit set int64 refuse
_v104_sentinel
capture parqit use `"`t'_b.parquet"', clear
local F_rc = _rc
capture quietly parqit close _all
capture parqit set int64 nonsense
local F_bad = _rc
capture assert "`F_type'" == "str`Obkw'" & "`F_optwins'" == "double" & ///
    `F_rc' == 198 & `F_bad' == 198 & _N == 3
if (_rc) {
    di as err "V104-F: set-string=`F_type' option-wins=`F_optwins'" ///
        " back-to-refuse rc=`F_rc' bad-value rc=`F_bad'"
    di as err "VERDICT(v104_session_default_and_precedence): FAIL"
}
else di as txt "VERDICT(v104_session_default_and_precedence): PASS"

* =====================================================================
* G — a column of the same family that FITS is untouched: no refusal,
*     no note, still numeric, with no option at all
* =====================================================================
log using `"`t'_G.log"', replace text name(v104G)
parqit use `"`t'_c.parquet"', clear
local G_rc = _rc
log close v104G
capture quietly parqit close _all
_v104_loghas `"`t'_G.log"' beyond 2^53
local G_any53 = r(found)
_v104_loghas `"`t'_G.log"' loaded as text
local G_text = r(found)
local G_type : type ck
capture assert `G_rc' == 0 & `G_any53' == 0 & `G_text' == 0 & ///
    ck[1] == 9007199254740991 & ck[2] == 5 & m[1] == 7 & _N == 2 & ///
    inlist("`G_type'", "double", "long")
if (_rc) {
    di as err "V104-G: rc=`G_rc' note53=`G_any53' text=`G_text' type=`G_type'"
    di as err "VERDICT(v104_control_column_unchanged): FAIL"
}
else di as txt "VERDICT(v104_control_column_unchanged): PASS (`G_type')"

capture quietly parqit close _all
di as txt "V104-DONE"
