* V81 — CSV-HEADER-1 (audit 2026-09-01, F4): delimited-text header names get
*   the Parquet treatment. The CSV reader deduplicates duplicate and
*   case-clashing header names (a,a,b,A -> a, a_1, b, A_2) and, because the
*   footer-name recovery never ran for CSV, the renames were silent: no note,
*   no src_name, and the case-distinct `A` lost its exact name. Now the raw
*   header is read back (sniffed dialect, header=false) and aligned with the
*   scan, so: an exact duplicate keeps the engine's a_1 with char
*   a_1[src_name]="a", a case-distinct name is exact in Stata (alias only
*   inside the lazy view, with a note), an empty header cell becomes
*   v<position> with a note, and a headerless file is untouched.
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
mata:
f = fopen(st_local("dir") + "/dup.csv", "w")
fput(f, "a,a,b,A")
fput(f, "1,2,3,4")
fput(f, "5,6,7,8")
fclose(f)
f = fopen(st_local("dir") + "/dup.tsv", "w")
fput(f, "a" + char(9) + "a" + char(9) + "b")
fput(f, "1" + char(9) + "2" + char(9) + "3")
fclose(f)
f = fopen(st_local("dir") + "/blank.csv", "w")
fput(f, "id,,my col")
fput(f, "1,x,y")
fput(f, "2,z,w")
fclose(f)
f = fopen(st_local("dir") + "/nohead.csv", "w")
fput(f, "1,2,3")
fput(f, "4,5,6")
fclose(f)
end

capture log close v81
log using `"`dir'/v81.log"', replace name(v81) text

* ---------- exact duplicates and a case-distinct name ----------------------------
parqit use `"`dir'/dup.csv"', clear
assert "`: char a_1[src_name]'" == "a"
assert "`: char A[src_name]'" == ""
assert "`: char a[src_name]'" == ""
qui ds
assert "`r(varlist)'" == "a a_1 b A"
assert a[1] == 1 & a_1[1] == 2 & b[1] == 3 & A[1] == 4
assert a[2] == 5 & a_1[2] == 6 & b[2] == 7 & A[2] == 8
parqit use using `"`dir'/dup.csv"'
parqit describe
parqit gen double s = a + a_1 + A_2
parqit collect, clear
qui ds
assert "`r(varlist)'" == "a a_1 b A s"
assert s[1] == 7 & s[2] == 19
assert "`: char a_1[src_name]'" == "a"
parqit close _all
parqit use A using `"`dir'/dup.csv"', clear
qui ds
assert "`r(varlist)'" == "A"
assert A[2] == 8

* ---------- tab-delimited duplicates ---------------------------------------------
parqit use `"`dir'/dup.tsv"', clear
qui ds
assert "`r(varlist)'" == "a a_1 b"
assert "`: char a_1[src_name]'" == "a"
assert a_1[1] == 2

* ---------- an empty header cell and a name with a space --------------------------
parqit use `"`dir'/blank.csv"', clear
qui ds
assert "`r(varlist)'" == "id v2 my_col"
assert "`: char my_col[src_name]'" == "my col"
assert v2[1] == "x" & my_col[2] == "w"
parqit use using `"`dir'/blank.csv"'
parqit collect, clear
qui ds
assert "`r(varlist)'" == "id v2 my_col"
parqit close _all

* ---------- no header: the engine's column0.. names, no notes -------------------
parqit use `"`dir'/nohead.csv"', clear
qui ds
assert "`r(varlist)'" == "column0 column1 column2"
assert _N == 2
log close v81

python:
from sfi import Macro
import os
txt = open(os.path.join(Macro.getLocal("dir"), "v81.log"), encoding="utf-8", errors="replace").read().replace("\n> ", "")
ok = ('"A" differs only by case' in txt and "A_2 in lazy verbs" in txt
      and 'column "a" is a_1 in the view' in txt
      and "column 2 has an empty name; loaded as v2" in txt
      and 'column "my col" loaded as my_col' in txt)
Macro.setLocal("notes_ok", "1" if ok else "0")
end
assert "`notes_ok'" == "1"

di "VERDICT(V81_CSV_HEADER_NAMES): PASS - CSV duplicate, case-clashing, empty and space-bearing header names get the Parquet treatment (exact Stata names, src_name, alias note in the lazy view), headerless files untouched"
