* V105 — binary(drop|text|hex): a Parquet BINARY/BLOB column in Stata
* (BINARY-DECODE-1).
*
* Raw bytes have no Stata representation, so the DEFAULT still drops the
* column with a message — but the message now names the two ways to load it,
* and neither one can be silent about what it did:
*
*   binary(text)  decode(blob) -> UTF-8 text. DuckDB RAISES on an invalid
*                 byte sequence (it never substitutes replacement
*                 characters), so a column that is not text fails the read
*                 loudly and leaves the data in memory untouched.
*   binary(hex)   hex(blob) -> two UPPERCASE hex digits per byte: always
*                 valid, always exact, for bytes that are not text at all.
*
* The oracle is pyarrow + Python's own bytes.decode()/bytes.hex(): parqit is
* never its own witness for what the file holds.
clear all
set more off
set varabbrev off
set linesize 244
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

capture program drop _v105_loghas
program define _v105_loghas, rclass
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

capture program drop _v105_sentinel
program define _v105_sentinel
    version 16.0
    clear
    quietly set obs 3
    quietly generate long sentinel = 100 * _n
end

tempfile tb
local good `"`tb'_good.parquet"'
local bad  `"`tb'_bad.parquet"'

* ---- the sources, written by pyarrow (parqit cannot write a BLOB) ----------
python:
import pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
rows = ["olá".encode("utf-8"), "emoji ✈".encode("utf-8"), None, b""]
pq.write_table(pa.table({"blob": pa.array(rows, pa.binary()),
                         "n": pa.array([1, 2, 3, 4], pa.int32())}),
               Macro.getLocal("good"))
bad = rows + [b"\xff\xfe\x00abc"]
pq.write_table(pa.table({"blob": pa.array(bad, pa.binary()),
                         "n": pa.array([1, 2, 3, 4, 5], pa.int32())}),
               Macro.getLocal("bad"))
# the oracle: Python decodes/hexes the very bytes that are on disk
back = pq.read_table(Macro.getLocal("good")).to_pydict()["blob"]
Macro.setLocal("Otxt1", back[0].decode("utf-8"))
Macro.setLocal("Otxt2", back[1].decode("utf-8"))
Macro.setLocal("Ohex1", back[0].hex().upper())
Macro.setLocal("Ohex2", back[1].hex().upper())
Macro.setLocal("Owidth", str(max(len(b) for b in back if b is not None)))
Macro.setLocal("Obadhex", bad[4].hex().upper())
Macro.setLocal("Ophys", str(pq.read_table(Macro.getLocal("good")).schema.field("blob").type))
end
capture assert "`Ophys'" == "binary"
if (_rc) di as err "VERDICT(v105_source_is_binary_on_disk): FAIL (type=`Ophys')"
else di as txt "VERDICT(v105_source_is_binary_on_disk): PASS (`Ophys')"

* =====================================================================
* A — the default drops the column, says so, and loads everything else
* =====================================================================
capture quietly parqit close _all
log using `"`tb'_A.log"', replace text name(v105A)
parqit use `"`good'"', clear
local A_rc = _rc
log close v105A
capture quietly parqit close _all
_v105_loghas `"`tb'_A.log"' BLOB has no Stata representation
local A_said = r(found)
_v105_loghas `"`tb'_A.log"' add binary(text) or binary(hex) to load it
local A_remedy = r(found)
capture confirm variable blob
local A_gone = (_rc != 0)
capture assert `A_rc' == 0 & `A_said' == 1 & `A_remedy' == 1 & `A_gone' == 1 & ///
    _N == 4 & n[1] == 1 & n[4] == 4 & c(k) == 1
if (_rc) {
    di as err "V105-A: rc=`A_rc' said=`A_said' remedy=`A_remedy' dropped=`A_gone' k=" c(k)
    di as err "VERDICT(v105_default_drops_with_remedy): FAIL"
}
else di as txt "VERDICT(v105_default_drops_with_remedy): PASS"

* =====================================================================
* B — binary(text): the exact strings Python gets from the same bytes
* =====================================================================
log using `"`tb'_B.log"', replace text name(v105B)
parqit use `"`good'"', clear binary(text)
log close v105B
capture quietly parqit close _all
_v105_loghas `"`tb'_B.log"' binary column decoded as UTF-8 text
local B_note = r(found)
local B_type : type blob
quietly count if blob == `"`Otxt1'"' in 1
local B_1 = r(N)
quietly count if blob == `"`Otxt2'"' in 2
local B_2 = r(N)
capture assert `B_note' == 1 & "`B_type'" == "str`Owidth'" & `B_1' == 1 & ///
    `B_2' == 1 & blob[3] == "" & blob[4] == "" & _N == 4 & n[2] == 2
if (_rc) {
    di as err "V105-B: note=`B_note' type=`B_type' (expected str`Owidth')" ///
        " row1=`B_1' row2=`B_2'"
    di as err "VERDICT(v105_text_is_exact_utf8): FAIL"
}
else di as txt "VERDICT(v105_text_is_exact_utf8): PASS (`B_type')"

* =====================================================================
* C — binary(hex): Python's bytes.hex(), uppercase, digit for digit
* =====================================================================
parqit use `"`good'"', clear binary(hex)
capture quietly parqit close _all
local C_type : type blob
quietly count if upper(blob) == "`Ohex1'" in 1
local C_1 = r(N)
quietly count if upper(blob) == "`Ohex2'" in 2
local C_2 = r(N)
local C_upper = (blob[2] == upper(blob[2]))
capture assert `C_1' == 1 & `C_2' == 1 & `C_upper' == 1 & blob[3] == "" & _N == 4
if (_rc) {
    di as err "V105-C: type=`C_type' row1=`C_1' row2=`C_2' uppercase=`C_upper'"
    di as err "VERDICT(v105_hex_matches_python): FAIL"
}
else di as txt "VERDICT(v105_hex_matches_python): PASS (`C_type')"

* =====================================================================
* D — invalid UTF-8 under binary(text) is LOUD, names the column, and
*     leaves the data in memory exactly as it was; hex still reads it
* =====================================================================
_v105_sentinel
log using `"`tb'_D.log"', replace text name(v105D)
capture noisily parqit use `"`bad'"', clear binary(text)
local D_rc = _rc
log close v105D
capture quietly parqit close _all
_v105_loghas `"`tb'_D.log"' column blob holds bytes that are not valid UTF-8
local D_named = r(found)
_v105_loghas `"`tb'_D.log"' use binary(hex)
local D_remedy = r(found)
local D_intact = (_N == 3 & c(k) == 1 & sentinel[3] == 300)
parqit use `"`bad'"', clear binary(hex)
capture quietly parqit close _all
quietly count if upper(blob) == "`Obadhex'" in 5
local D_hex = r(N)
capture assert `D_rc' == 198 & `D_named' == 1 & `D_remedy' == 1 & ///
    `D_intact' == 1 & `D_hex' == 1 & _N == 5
if (_rc) {
    di as err "V105-D: rc=`D_rc' named=`D_named' remedy=`D_remedy'" ///
        " intact=`D_intact' hex-row=`D_hex'"
    di as err "VERDICT(v105_invalid_utf8_is_loud): FAIL"
}
else di as txt "VERDICT(v105_invalid_utf8_is_loud): PASS"

* =====================================================================
* E — the lazy path: binary() belongs to the OPEN (the boundary decides
*     a view's columns), and collect says so instead of ignoring it
* =====================================================================
capture quietly parqit close _all
quietly parqit use using `"`good'"', binary(hex)
parqit collect, clear
capture quietly parqit close _all
quietly count if upper(blob) == "`Ohex1'" in 1
local E_1 = r(N)

capture quietly parqit close _all
quietly parqit use using `"`good'"'
capture confirm variable blob
capture parqit collect, clear
local E_rc = _rc
capture confirm variable blob
local E_dropped = (_rc != 0)
capture parqit collect, clear binary(hex)
local E_late = _rc
capture quietly parqit close _all

_v105_sentinel
capture quietly parqit close _all
quietly parqit use using `"`bad'"', binary(text)
log using `"`tb'_E.log"', replace text name(v105E)
capture noisily parqit collect, clear
local E_bad = _rc
log close v105E
capture quietly parqit close _all
_v105_loghas `"`tb'_E.log"' not valid UTF-8
local E_said = r(found)
local E_intact = (_N == 3 & c(k) == 1)
capture assert `E_1' == 1 & `E_rc' == 0 & `E_dropped' == 1 & `E_late' == 198 & ///
    `E_bad' != 0 & `E_said' == 1 & `E_intact' == 1
if (_rc) {
    di as err "V105-E: hex-through-view=`E_1' plain-collect rc=`E_rc'" ///
        " dropped=`E_dropped' late-option rc=`E_late' invalid rc=`E_bad'" ///
        " said=`E_said' intact=`E_intact'"
    di as err "VERDICT(v105_lazy_binary_contract): FAIL"
}
else di as txt "VERDICT(v105_lazy_binary_contract): PASS"

* =====================================================================
* F — the SAME failure through a view that has STAGES: it is the temp
*     table that raises, before the planner runs, and the user must still
*     read parqit's remedy — never decode()'s SQL advice or the
*     generated query (v69's no-raw-engine-text contract)
* =====================================================================
_v105_sentinel
capture quietly parqit close _all
quietly parqit use using `"`bad'"', binary(text)
quietly parqit keep if n > 1
log using `"`tb'_F.log"', replace text name(v105F)
capture noisily parqit collect, clear
local F_rc = _rc
log close v105F
capture quietly parqit close _all
_v105_loghas `"`tb'_F.log"' not valid UTF-8
local F_said = r(found)
_v105_loghas `"`tb'_F.log"' binary(hex)
local F_remedy = r(found)
_v105_loghas `"`tb'_F.log"' LINE 1:
local F_sql = r(found)
_v105_loghas `"`tb'_F.log"' Conversion Error
local F_raw = r(found)
_v105_loghas `"`tb'_F.log"' try(decode
local F_advice = r(found)
local F_intact = (_N == 3 & c(k) == 1 & sentinel[2] == 200)
capture assert `F_rc' == 198 & `F_said' == 1 & `F_remedy' == 1 & ///
    `F_sql' == 0 & `F_raw' == 0 & `F_advice' == 0 & `F_intact' == 1
if (_rc) {
    di as err "V105-F: rc=`F_rc' said=`F_said' remedy=`F_remedy'" ///
        " raw-sql=`F_sql' raw-prefix=`F_raw' sql-advice=`F_advice'" ///
        " intact=`F_intact'"
    di as err "VERDICT(v105_staged_view_decode_is_parqits_own): FAIL"
}
else di as txt "VERDICT(v105_staged_view_decode_is_parqits_own): PASS"

capture quietly parqit close _all
di as txt "V105-DONE"
