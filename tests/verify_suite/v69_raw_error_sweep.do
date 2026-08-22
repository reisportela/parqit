* V69 — every public variable-name surface refuses unknown names before a
* generated query reaches DuckDB.  The contract is nonzero rc + parqit's own
* message, never Binder/Parser/Catalog text, generated SQL or internal names.
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/ado/plus/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

local fails 0
tempfile stem
global V69_SRC `"`stem'_src.parquet"'
global V69_USING `"`stem'_using.parquet"'
global V69_OUT `"`stem'_out.parquet"'
global V69_LOG `"`stem'_messages.log"'

clear
set obs 4
gen long id = _n
gen double x = _n * 2
gen int year = 2019 + mod(_n, 2)
gen byte j = mod(_n, 2) + 1
gen double x1 = _n * 10
gen double x2 = _n * 20
parqit save "$V69_SRC", replace data

clear
set obs 4
gen long id = _n
gen double u = _n / 2
parqit save "$V69_USING", replace data

capture program drop _v69_grep
program define _v69_grep, rclass
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

capture program drop _v69_probe
program define _v69_probe, rclass
    gettoken label 0 : 0, parse(" ")
    capture parqit close _all
    quietly parqit use using "$V69_SRC"
    quietly log using "$V69_LOG", text name(v69probe) append
    capture noisily `0'
    local cmdrc = _rc
    di as txt "CASE(`label'): RC=`cmdrc'"
    quietly log close v69probe
    capture quietly parqit count
    local alive_rc = _rc
    local alive_n = cond(`alive_rc' == 0, r(N), -1)
    capture parqit close _all
    return scalar rc = `cmdrc'
    return scalar intact = (`alive_rc' == 0 & `alive_n' == 4)
end

capture erase "$V69_LOG"

* One invocation per variable-bearing public form. Every failure must be
* atomic; _v69_probe therefore also counts the original four-row view.
local ncases 0
foreach spec in ///
    `"keep              parqit keep nosuchvar"' ///
    `"drop              parqit drop nosuchvar"' ///
    `"order             parqit order nosuchvar"' ///
    `"sort              parqit sort nosuchvar"' ///
    `"gsort             parqit gsort -nosuchvar"' ///
    `"rename_old        parqit rename nosuchvar newvar"' ///
    `"gen_expr          parqit gen double z = nosuchvar + 1"' ///
    `"replace_target    parqit replace nosuchvar = id"' ///
    `"replace_expr      parqit replace id = nosuchvar"' ///
    `"egen_expr         parqit egen double z = mean(nosuchvar), by(id)"' ///
    `"egen_by           parqit egen double z = mean(x), by(nosuchvar)"' ///
    `"keep_if           parqit keep if nosuchvar > 0"' ///
    `"count_if          parqit count if nosuchvar > 0"' ///
    `"list_var          parqit list nosuchvar"' ///
    `"list_if           parqit list if nosuchvar > 0"' ///
    `"collapse_src      parqit collapse (mean) nosuchvar"' ///
    `"collapse_by       parqit collapse (mean) x, by(nosuchvar)"' ///
    `"contract          parqit contract nosuchvar"' ///
    `"dup_report        parqit duplicates report nosuchvar"' ///
    `"dup_list          parqit duplicates list nosuchvar"' ///
    `"dup_drop          parqit duplicates drop nosuchvar, force"' ///
    `"reshape_stub      parqit reshape long nosuch, i(id) j(k)"' ///
    `"reshape_long_i    parqit reshape long x, i(nosuchvar) j(k)"' ///
    `"reshape_long_i2   parqit reshape long x, i(id nosuchvar) j(k)"' ///
    `"reshape_wide_j    parqit reshape wide x, i(id) j(nosuchvar)"' ///
    `"reshape_wide_i    parqit reshape wide x, i(nosuchvar) j(j)"' ///
    `"reshape_wide_stub parqit reshape wide nosuchvar, i(id) j(j)"' ///
    `"pivot_src         parqit pivot (sum) nosuchvar, rows(id) cols(year)"' ///
    `"pivot_rows        parqit pivot (sum) x, rows(nosuchvar) cols(year)"' ///
    `"pivot_cols        parqit pivot (sum) x, rows(id) cols(nosuchvar)"' ///
    `"merge_key         parqit merge 1:1 nosuchvar using $V69_USING"' ///
    `"merge_keepusing   parqit merge 1:1 id using $V69_USING, keepusing(nosuchvar)"' ///
    `"joinby_key        parqit joinby nosuchvar using $V69_USING"' ///
    `"append_collision  parqit append using $V69_USING, generate(id)"' ///
    `"summarize         parqit summarize nosuchvar"' ///
    `"tabulate1         parqit tabulate nosuchvar"' ///
    `"tabulate2         parqit tabulate id nosuchvar"' ///
    `"tabstat           parqit tabstat nosuchvar, statistics(mean)"' ///
    `"tabstat_by        parqit tabstat x, statistics(mean) by(nosuchvar)"' ///
    `"levelsof          parqit levelsof nosuchvar"' ///
    `"misstable         parqit misstable nosuchvar"' ///
    `"codebook          parqit codebook nosuchvar"' ///
    `"distinct          parqit distinct nosuchvar"' ///
    `"correlate         parqit correlate id nosuchvar"' ///
    `"pwcorr            parqit pwcorr id nosuchvar"' ///
    `"histogram         parqit histogram nosuchvar, nodraw"' ///
    `"save_partition    parqit save $V69_OUT, replace partition_by(nosuchvar)"' ///
    `"use_lazy          parqit use nosuchvar using $V69_SRC"' ///
    `"use_clear         parqit use nosuchvar using $V69_SRC, clear"' {

    local ++ncases
    _v69_probe `spec'
    if (r(rc) == 0) {
        di as err "FAIL `: word 1 of `spec'': unknown name was accepted"
        local ++fails
    }
    if (!r(intact)) {
        di as err "FAIL `: word 1 of `spec'': refusal changed or destroyed the live view"
        local ++fails
    }
}

* A rename destination is supposed not to exist; over-validating both sides
* would be a regression, so this is the positive control for the sweep.
parqit use using "$V69_SRC"
capture noisily parqit rename id brandnew
if (_rc) {
    di as err "FAIL rename_new: a valid new destination name was refused"
    local ++fails
}
capture quietly parqit count
if (_rc | r(N) != 4) {
    di as err "FAIL rename_new: successful rename damaged the view"
    local ++fails
}
parqit close _all

* Native merge/append wrappers validate the projected disk-side names through
* parqit's eager read. They must be equally loud and engine-internal-free.
foreach op in mergein appendin {
    clear
    set obs 4
    gen long id = _n
    gen double u = _n / 2
    quietly log using "$V69_LOG", text name(v69probe) append
    if ("`op'" == "mergein") ///
        capture noisily parqit mergein 1:1 id using "$V69_SRC", keepusing(nosuchvar)
    else ///
        capture noisily parqit appendin using "$V69_SRC", keep(nosuchvar)
    local cmdrc = _rc
    di as txt "CASE(`op'): RC=`cmdrc'"
    quietly log close v69probe
    if (`cmdrc' == 0) {
        di as err "FAIL `op': unknown using-side name was accepted"
        local ++fails
    }
    local ++ncases
}

* Every pre-query reshape name guard uses native variable-not-found rc.
foreach lab in reshape_stub reshape_long_i reshape_long_i2 reshape_wide_j ///
               reshape_wide_i reshape_wide_stub {
    _v69_grep "$V69_LOG" "CASE(`lab'): RC=111"
    if (!r(found)) {
        di as err "FAIL `lab': expected rc 111 before any engine query"
        local ++fails
    }
}
_v69_grep "$V69_LOG" "parqit reshape long: no variables match stub(s) nosuch…"
if (!r(found)) {
    di as err "FAIL reshape long: did not name the unmatched stub"
    local ++fails
}
_v69_grep "$V69_LOG" "parqit reshape long: i() variable nosuchvar not found in the view"
if (!r(found)) {
    di as err "FAIL reshape long: did not name the missing i() variable"
    local ++fails
}
_v69_grep "$V69_LOG" "parqit reshape wide: j() variable nosuchvar not found in the view"
if (!r(found)) {
    di as err "FAIL reshape wide j(): did not name the missing variable"
    local ++fails
}
_v69_grep "$V69_LOG" "parqit reshape wide: i() variable nosuchvar not found in the view"
if (!r(found)) {
    di as err "FAIL reshape wide i(): did not name the missing variable"
    local ++fails
}
_v69_grep "$V69_LOG" "parqit reshape wide: stub variable nosuchvar not found in the view"
if (!r(found)) {
    di as err "FAIL reshape wide: did not name the missing stub variable"
    local ++fails
}

foreach forbidden in "Binder Error" "Parser Error" "Catalog Error" ///
                     "Invalid Input Error" "__parqit_s" "__PARQIT_" "LINE 1:" {
    _v69_grep "$V69_LOG" `"`forbidden'"'
    if (r(found)) {
        di as err `"FAIL RAW-ERROR: forbidden engine/internal text reached the user: `forbidden'"'
        local ++fails
    }
}

global V69_SRC
global V69_USING
global V69_OUT
global V69_LOG

if (`fails' == 0) {
    di as result "VERDICT(V69_RAW_ERROR_SWEEP): PASS - `ncases' unknown-name cases refuse atomically with no engine SQL or internal names"
}
else {
    di as err "VERDICT(V69_RAW_ERROR_SWEEP): FAIL - `fails' check(s) across `ncases' cases"
}
