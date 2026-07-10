* M1: varlist selection on parqit use (named columns, named order — charter
* §6.1), parqit describe r() contract (scalars are scalars — audit S4-12),
* multi-file globs, and shape edges (0 rows, 1 var, wide).
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

* ---------- fixture: two files for a glob + one wide + one empty ----------
* Keep directory fixtures in the runner's unique working directory and clean
* them explicitly. A tempfile-derived directory is not removed by Stata and can
* collide when the OS later reuses a process/temp-name prefix.
local dir `"`c(pwd)'/_t02_use_options_fixture"'
foreach f in part1.parquet part2.parquet empty.parquet one.parquet wide.parquet {
    capture erase `"`dir'/`f'"'
}
capture rmdir `"`dir'"'
mkdir `"`dir'"'

clear
set obs 3
gen long id = _n
gen double w = _n * 1.5
gen str4 s = "a" + string(_n)
parqit save `"`dir'/part1.parquet"'

clear
set obs 2
gen long id = 100 + _n
gen double w = -_n
gen str4 s = "b" + string(_n)
parqit save `"`dir'/part2.parquet"'

* ---------- glob read: file order, 5 rows ----------
parqit use using `"`dir'/part*.parquet"', clear
assert _N == 5 & c(k) == 3
assert id[1] == 1 & id[4] == 101 & id[5] == 102
assert s[5] == "b2"

* ---------- varlist selection: named columns, named order ----------
parqit use s id using `"`dir'/part1.parquet"', clear
assert c(k) == 2
unab order : _all
assert "`order'" == "s id"
assert s[3] == "a3" & id[3] == 3

* unknown variable is a loud 111
capture parqit use nope using `"`dir'/part1.parquet"', clear
assert _rc == 111

* ---------- describe: r() scalars are scalars (S4-12) ----------
parqit describe `"`dir'/part1.parquet"'
assert r(n_rows) == 3
assert r(n_cols) == 3
assert r(n_row_groups) >= 1
assert r(n_files) == 1
assert r(has_parqit_meta) == 1
assert "`r(name_1)'" == "id"
assert "`r(stata_type_1)'" == "long"

* describe leaves data + changed-flag untouched (state hygiene)
clear
set obs 2
gen x = _n
datasignature set, reset
parqit describe `"`dir'/part1.parquet"'
datasignature confirm

* ---------- 0-row file round-trip ----------
clear
set obs 0
gen long a = .
gen str3 sz = ""
parqit save `"`dir'/empty.parquet"', replace
parqit use using `"`dir'/empty.parquet"', clear
assert _N == 0 & c(k) == 2
confirm long variable a
confirm str variable sz

* ---------- single-variable file ----------
clear
set obs 4
gen byte only = _n
parqit save `"`dir'/one.parquet"', replace
parqit use using `"`dir'/one.parquet"', clear
assert c(k) == 1 & _N == 4 & only[4] == 4

* ---------- wide file (500 vars; CI-friendly slice of the 2500 target) ----------
clear
set obs 2
forvalues i = 1/500 {
    qui gen int v`i' = `i' * 10 + _n
}
parqit save `"`dir'/wide.parquet"', replace
parqit use using `"`dir'/wide.parquet"', clear
assert c(k) == 500 & _N == 2
assert v1[1] == 11 & v500[2] == 5002

foreach f in part1.parquet part2.parquet empty.parquet one.parquet wide.parquet {
    capture erase `"`dir'/`f'"'
}
capture rmdir `"`dir'"'
local cleanup_rc = _rc
assert `cleanup_rc' == 0

di "VERDICT(T02_USE_OPTIONS): PASS - varlist order, describe scalars, globs, shape edges"
