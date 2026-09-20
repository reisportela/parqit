* V107 — CSV-OPT-1: `parqit use ..., csv(...)` hands the embedded reader the
*   dialect and the types the data actually has. The integrity case is silent
*   type inference: a zip-code column of leading-zero text is inferred as an
*   integer and "01234" becomes 1234 — a value change with rc 0, which no
*   amount of downstream care can undo. types(zip:VARCHAR) and allvarchar both
*   stop it. This also pins the forced dialect (delim/quote/escape/header)
*   reaching BOTH the scan and the CSV-HEADER-1 raw-header probe (duplicate
*   header names must still be recovered with their src_name when a dialect is
*   forced), dateformat() producing exact %td day counts, nullstr() making a
*   sentinel missing where a plain load keeps it as text, the eager and lazy
*   forms, and loud refusals for an unknown sub-option, a type the engine does
*   not know, a bad sample() and csv() on a source that is not delimited text.
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

tempfile stem
local dir `"`stem'_d"'
mkdir `"`dir'"'
local fails 0

python:
from sfi import Macro
import csv, os
import pyarrow as pa, pyarrow.parquet as pq
d = Macro.getLocal("dir")
with open(os.path.join(d, "zip.csv"), "w", newline="") as fh:
    w = csv.writer(fh)
    w.writerow(["zip", "city"])
    w.writerows([["01234", "Ann"], ["00777", "Bob"], ["98765", "Cid"]])
# text that type inference silently turns into something else: scientific
# notation, a decimal with more digits than a double holds, and a word
with open(os.path.join(d, "codes.csv"), "w", newline="") as fh:
    w = csv.writer(fh)
    w.writerow(["code", "ratio", "flag"])
    w.writerows([["1e5", "3.14159265358979311600", "TRUE"],
                 ["2E3", "2.71828182845904509080", "FALSE"]])
with open(os.path.join(d, "dates.csv"), "w", newline="") as fh:
    w = csv.writer(fh)
    w.writerow(["d", "who", "amt"])
    w.writerows([["03/02/2021", "ann", "10"], ["31/12/1999", "NA", "20"]])
# dates that parse under BOTH readings: only dateformat() decides which
with open(os.path.join(d, "amb.csv"), "w", newline="") as fh:
    w = csv.writer(fh)
    w.writerow(["d"])
    w.writerows([["03/02/2021"], ["05/06/2020"]])
# a pipe-delimited file whose fields are quoted with @ and contain the
# delimiter: only the forced dialect reads it as two columns
with open(os.path.join(d, "pipe.csv"), "w", newline="") as fh:
    w = csv.writer(fh, delimiter="|", quotechar="@", quoting=csv.QUOTE_ALL)
    w.writerow(["k", "v"])
    w.writerows([["1", "a|b"], ["2", "c"]])
# duplicate and case-clashing header names, pipe-delimited
with open(os.path.join(d, "dup.csv"), "w", newline="") as fh:
    w = csv.writer(fh, delimiter="|")
    w.writerow(["a", "a", "b", "A"])
    w.writerow(["1", "2", "3", "4"])
pq.write_table(pa.table({"id": pa.array([1, 2], pa.int32())}),
               os.path.join(d, "one.parquet"))
end

capture log close v107
log using `"`dir'/v107.log"', replace name(v107) text

* ---------- 1. the integrity case: inference changes the value -------------
* Baseline: with the pinned engine's own inference "1e5" becomes the number
* 100000, a 21-digit decimal is rounded to what a double holds, and the word
* TRUE becomes 1 — a value change with rc 0. (Leading-zero digit strings are
* one case this engine's sniffer already protects; types()/allvarchar are the
* contract that does not depend on what the sniffer happens to decide.)
parqit use `"`dir'/codes.csv"', clear
assert strpos("`: type code'", "str") == 0
assert code[1] == 100000 & flag[1] == 1
* the source text is gone: the stored double does not print back as written
assert strpos("`: type ratio'", "str") == 0
assert strtrim(strofreal(ratio[1], "%21.18g")) != "3.14159265358979311600"

parqit use `"`dir'/codes.csv"', clear csv(allvarchar)
assert "`: type code'" == "str3" & "`: type ratio'" == "str22"
assert code[1] == "1e5" & code[2] == "2E3"
assert ratio[1] == "3.14159265358979311600"
assert flag[1] == "TRUE" & flag[2] == "FALSE"

parqit use `"`dir'/codes.csv"', clear csv(types(code:VARCHAR ratio:VARCHAR))
assert "`: type code'" == "str3" & "`: type ratio'" == "str22"
assert code[1] == "1e5" & ratio[2] == "2.71828182845904509080"
assert flag[1] == 1

parqit use `"`dir'/zip.csv"', clear csv(types(zip:VARCHAR))
assert "`: type zip'" == "str5"
assert zip[1] == "01234" & zip[2] == "00777" & zip[3] == "98765"
assert city[1] == "Ann" & city[3] == "Cid"
qui ds
assert "`r(varlist)'" == "zip city"

parqit use `"`dir'/zip.csv"', clear csv(allvarchar)
assert "`: type zip'" == "str5" & "`: type city'" == "str3"
assert zip[1] == "01234" & zip[2] == "00777" & zip[3] == "98765"

* the lazy form carries the same options into the view
parqit use using `"`dir'/zip.csv"', csv(types(zip:VARCHAR))
parqit keep if zip == "00777"
parqit collect, clear
assert _N == 1 & zip[1] == "00777"
parqit close _all

* ---------- 2. dateformat(): exact day counts ------------------------------
parqit use `"`dir'/dates.csv"', clear csv(dateformat("%d/%m/%Y"))
assert "`: format d'" == "%td"
assert d[1] == mdy(2, 3, 2021)
assert d[2] == mdy(12, 31, 1999)
assert amt[1] == 10 & amt[2] == 20
* the option decides the reading of an ambiguous date, deterministically
parqit use `"`dir'/amb.csv"', clear csv(dateformat("%d/%m/%Y"))
assert "`: format d'" == "%td"
assert d[1] == mdy(2, 3, 2021) & d[2] == mdy(6, 5, 2020)
parqit use `"`dir'/amb.csv"', clear csv(dateformat("%m/%d/%Y"))
assert "`: format d'" == "%td"
assert d[1] == mdy(3, 2, 2021) & d[2] == mdy(5, 6, 2020)

* ---------- 3. nullstr(): a sentinel becomes missing ------------------------
parqit use `"`dir'/dates.csv"', clear
assert who[2] == "NA"
parqit use `"`dir'/dates.csv"', clear csv(nullstr("NA"))
assert who[1] == "ann"
assert who[2] == ""
qui count if missing(who)
assert r(N) == 1

* ---------- 4. a forced delimiter and quote character ----------------------
parqit use `"`dir'/pipe.csv"', clear csv(delim("|") quote("@"))
qui ds
assert "`r(varlist)'" == "k v"
assert _N == 2
assert v[1] == "a|b" & v[2] == "c"
assert k[1] == 1 & k[2] == 2

* ---------- 5. header(off): the header line is data ------------------------
parqit use `"`dir'/zip.csv"', clear csv(header(off))
qui ds
assert "`r(varlist)'" == "column0 column1"
assert _N == 4
assert column0[1] == "zip" & column1[1] == "city"
assert column0[2] == "01234"

* ---------- 6. name recovery survives a forced dialect (CSV-HEADER-1) ------
parqit use `"`dir'/dup.csv"', clear csv(delim("|"))
qui ds
assert "`r(varlist)'" == "a a_1 b A"
assert "`: char a_1[src_name]'" == "a"
assert a[1] == 1 & a_1[1] == 2 & b[1] == 3 & A[1] == 4
* and with types() naming the recovered header name
parqit use `"`dir'/dup.csv"', clear csv(delim("|") types(b:VARCHAR))
assert "`: type b'" == "str1" & b[1] == "3"
qui ds
assert "`r(varlist)'" == "a a_1 b A"

* ---------- 7. sample() ----------------------------------------------------
parqit use `"`dir'/zip.csv"', clear csv(sample(-1) types(zip:VARCHAR))
assert zip[3] == "98765"
parqit use `"`dir'/zip.csv"', clear csv(sample(2048))
assert _N == 3

* ---------- 8. loud refusals ------------------------------------------------
* a type the engine DOES know, with a parenthesised argument, is accepted
parqit use `"`dir'/zip.csv"', clear csv(types(zip:DECIMAL(18,2)))
assert strpos("`: type zip'", "str") == 0
assert zip[1] == 1234

sysuse auto, clear
local nbefore = _N
capture noisily parqit use `"`dir'/zip.csv"', clear csv(bogus(1))
if (_rc != 198) {
    di as err "FAIL: an unknown csv() sub-option returned rc " _rc ", expected 198"
    local ++fails
}
capture noisily parqit use `"`dir'/zip.csv"', clear csv(header(maybe))
if (_rc != 198) {
    di as err "FAIL: csv(header(maybe)) returned rc " _rc ", expected 198"
    local ++fails
}
capture noisily parqit use `"`dir'/zip.csv"', clear csv(sample(0))
if (_rc != 198) {
    di as err "FAIL: csv(sample(0)) returned rc " _rc ", expected 198"
    local ++fails
}
capture noisily parqit use `"`dir'/zip.csv"', clear csv(types(zip))
if (_rc != 198) {
    di as err "FAIL: a types() item without a type returned rc " _rc ", expected 198"
    local ++fails
}
* an unknown type is parqit's own refusal, proved before any scan SQL is built
* (the engine reports it from the bind, with its own error class and a dump of
* the probe SQL and the user's path; the sweep at the end of this test proves
* none of that reaches the user)
capture noisily parqit use `"`dir'/zip.csv"', clear csv(types(zip:NOSUCHTYPE))
if (_rc != 198) {
    di as err "FAIL: an unknown type returned rc " _rc ", expected 198"
    local ++fails
}
capture noisily parqit use `"`dir'/zip.csv"', clear csv(types(zip:VAR'CHAR))
if (_rc != 198) {
    di as err "FAIL: a type token with a quote returned rc " _rc ", expected 198"
    local ++fails
}
capture noisily parqit use `"`dir'/one.parquet"', clear csv(delim("|"))
if (_rc != 198) {
    di as err "FAIL: csv() on a Parquet source returned rc " _rc ", expected 198"
    local ++fails
}
if (_N != `nbefore' | "`: type make'" == "") {
    di as err "FAIL: a refused csv() disturbed the dataset in memory"
    local ++fails
}
log close v107

python:
from sfi import Macro
import os
txt = open(os.path.join(Macro.getLocal("dir"), "v107.log"),
           encoding="utf-8", errors="replace").read().replace("\n> ", "")
ok = ("csv(): option bogus(1) not allowed" in txt
      and "header() takes on or off" in txt
      and "sample() takes the number of rows" in txt
      and "types() takes name:TYPE pairs" in txt
      and "unknown type NOSUCHTYPE for column zip" in txt
      and "is not a type name for column zip" in txt
      and "csv() applies to delimited text" in txt)
Macro.setLocal("msgs_ok", "1" if ok else "0")
# no engine SQL or engine error class may reach the user (v69's contract)
raw = [m for m in ("LINE 1:", "Catalog Error", "Binder Error", "Parser Error",
                   "read_csv_auto(") if m in txt]
Macro.setLocal("raw_seen", " ".join(raw))
end
if ("`msgs_ok'" != "1") {
    di as err "FAIL: a csv() refusal did not name its cause"
    local ++fails
}
if (`"`raw_seen'"' != "") {
    di as err `"FAIL: engine SQL/error text reached the user: `raw_seen'"'
    local ++fails
}

if (`fails' == 0) {
    di as result "VERDICT(V107_CSV_OPTIONS): PASS - csv() forces the reader's dialect and types (leading zeros preserved by types()/allvarchar, exact %td day counts, forced delim/quote, header(off), nullstr), the forced dialect also drives the header-name recovery, and every bad key, type, value or source refuses loudly with the dataset untouched"
}
else {
    di as err "VERDICT(V107_CSV_OPTIONS): FAIL - `fails' check(s)"
}
