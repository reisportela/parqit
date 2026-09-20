* V103 — int64/uint64 above 2^53: what is lost, what is exact, and where.
* Finding T12 (and T09) of the triple-precision audit (2026-09-19); evidence in
* audit_repro/triple_precision_20260919/ (a05_keys §4, a06_keys2 §2, a04 P4).
*
*   A  READING into Stata rounds to the nearest double and SAYS SO: 2^53 and
*      2^53+1 are one value afterwards. Announced loss, never silent.
*   B  the LOSSLESS path is exact: parqit sql with CAST(... AS VARCHAR) brings
*      9007199254740993 and 9223372036854775807 back as text, digit for digit.
*   C  a LAZY join over such a key is EXACT, because it runs in the engine
*      before any Stata double exists: keys 2^53 and 2^53+1 pair A<->10 and
*      B<->20, never A<->20. No path through memory — native or otherwise —
*      can do this.
*
* The BIGINT source cannot be produced by Stata (its widest exact integer is
* 2^53), so it is generated here with parqit's own SQL, as the audit did, and
* the on-disk payload is then checked with an INDEPENDENT oracle (pyarrow):
* the physical type must be int64 and the three values exact on disk.
clear all
set more off
set varabbrev off
set linesize 244
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

capture program drop _v103_loghas
program define _v103_loghas, rclass
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

tempfile t

* ---- the source: three BIGINTs Stata cannot hold exactly --------------------
capture quietly parqit close _all
parqit sql "SELECT * FROM (VALUES (9007199254740992::BIGINT, 'A'), (9007199254740993::BIGINT, 'B'), (9223372036854775807::BIGINT, 'C')) t(bk, side_m)"
parqit save `"`t'_m.parquet"', replace
capture quietly parqit close _all
parqit sql "SELECT * FROM (VALUES (9007199254740992::BIGINT, 10.0), (9007199254740993::BIGINT, 20.0)) t(bk, val_u)"
parqit save `"`t'_u.parquet"', replace
capture quietly parqit close _all

* independent oracle: is the file really int64, with the exact values?
python:
import pyarrow.parquet as pq
from sfi import Macro
b = Macro.getLocal("t")
tb = pq.read_table(b + "_m.parquet")
phys = str(tb.schema.field("bk").type)
vals = tb.to_pydict()["bk"]
ok = (phys == "int64" and
      vals == [9007199254740992, 9007199254740993, 9223372036854775807])
Macro.setLocal("Ophys", phys)
Macro.setLocal("Ook", "1" if ok else "0")
end
capture assert "`Ook'" == "1"
if (_rc) di as err "VERDICT(v103_source_is_exact_int64_on_disk): FAIL (type=`Ophys')"
else di as txt "VERDICT(v103_source_is_exact_int64_on_disk): PASS (`Ophys')"

* =====================================================================
* A — reading into Stata rounds, and announces the rounding
* =====================================================================
* INT64-PROTECT-1: reading such a column into memory now REFUSES by default
* (v104); int64(round) is exactly this documented rounding path, and asking
* for it explicitly is what makes the loss a choice instead of a surprise.
log using `"`t'_A.log"', replace text name(v103A)
parqit use `"`t'_m.parquet"', clear int64(round)
log close v103A
capture quietly parqit close _all
_v103_loghas `"`t'_A.log"' values beyond 2^53 rounded to nearest double
local A_note = r(found)
quietly duplicates report bk
local A_distinct = r(unique_value)
local A_collapsed = (bk[1] == bk[2])
capture assert `A_note' == 1 & `A_distinct' == 2 & `A_collapsed' == 1 & _N == 3
if (_rc) {
    di as err "V103-A: note=`A_note' distinct=`A_distinct' (expected 2)" ///
        " collapsed=`A_collapsed' N=" _N
    di as err "VERDICT(v103_read_rounds_and_says_so): FAIL"
}
else di as txt "VERDICT(v103_read_rounds_and_says_so): PASS"

* =====================================================================
* B — the lossless recipe: CAST(... AS VARCHAR) through parqit sql
* =====================================================================
capture quietly parqit close _all
parqit sql `"SELECT side_m, CAST(bk AS VARCHAR) AS s64 FROM read_parquet('`t'_m.parquet')"'
parqit collect, clear
capture quietly parqit close _all
local B_type : type s64
quietly count if (side_m == "A" & s64 == "9007199254740992") | ///
    (side_m == "B" & s64 == "9007199254740993") | ///
    (side_m == "C" & s64 == "9223372036854775807")
local B_exact = r(N)
capture assert `B_exact' == 3 & _N == 3 & substr("`B_type'", 1, 3) == "str"
if (_rc) {
    di as err "V103-B: exact=`B_exact'/3 N=" _N " type=`B_type'"
    di as err "VERDICT(v103_varchar_cast_is_exact): FAIL"
}
else di as txt "VERDICT(v103_varchar_cast_is_exact): PASS (`B_type')"

* =====================================================================
* C — the lazy join over a BIGINT key is exact (it never becomes a double)
* =====================================================================
capture quietly parqit close _all
parqit use using `"`t'_m.parquet"'
parqit merge 1:1 bk using `"`t'_u.parquet"'
* the JOIN is exact in the engine; only the trip into Stata memory rounds,
* which int64(round) now asks for explicitly (INT64-PROTECT-1)
parqit collect, clear int64(round)
capture quietly parqit close _all
quietly count if side_m == "A" & val_u == 10
local C_A = r(N)
quietly count if side_m == "B" & val_u == 20
local C_B = r(N)
quietly count if (side_m == "A" & val_u == 20) | (side_m == "B" & val_u == 10)
local C_cross = r(N)
quietly count if side_m == "C" & _merge == 1
local C_unmatched = r(N)
capture assert `C_A' == 1 & `C_B' == 1 & `C_cross' == 0 & `C_unmatched' == 1 & _N == 3
if (_rc) {
    di as err "V103-C: A<->10=`C_A' B<->20=`C_B' crossed=`C_cross'" ///
        " C unmatched=`C_unmatched' N=" _N
    di as err "VERDICT(v103_lazy_bigint_join_is_exact): FAIL"
}
else di as txt "VERDICT(v103_lazy_bigint_join_is_exact): PASS"

* The same keys read into memory first DO collapse — the contrast that makes
* the lazy join's exactness a property of the architecture, not of the data.
capture quietly parqit close _all
parqit use `"`t'_m.parquet"', clear int64(round)
quietly duplicates report bk
local C_mem = r(unique_value)
capture assert `C_mem' == 2
if (_rc) di as err "VERDICT(v103_memory_path_collapses_as_documented): FAIL (distinct=`C_mem')"
else di as txt "VERDICT(v103_memory_path_collapses_as_documented): PASS"

capture quietly parqit close _all
di as txt "V103-DONE"
