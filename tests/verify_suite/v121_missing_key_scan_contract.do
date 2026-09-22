* MISSKEY-SCAN-1: note identity/order and deferred errors retain their contracts.
clear all
set more off
set varabbrev off
set linesize 244
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile stem

program define _v121_memory
    assert _N == 1 & sentinel == 1234567890123
    assert `"`: variable label sentinel'"' == "Current data"
    assert `"`: char sentinel[guard]'"' == "Preserve me"
    assert `"`: data label'"' == "Current metadata"
end

program define _v121_notes
    args logfile scenario reversed
    tempname fh
    local line_number 0
    local first 0
    local third 0
    file open `fh' using `"`logfile'"', read text
    file read `fh' line
    while (r(eof) == 0) {
        local ++line_number
        assert strpos(`"`line'"', "note: key k2:") == 0
        if (strpos(`"`line'"', "note: key k1:")) {
            assert "`scenario'" == "B" & `first' == 0
            assert strpos(`"`line'"', "key k1: 2 master and 1 using row(s)") > 0
            local first `line_number'
        }
        if (strpos(`"`line'"', "note: key k3:")) {
            assert `third' == 0
            assert strpos(`"`line'"', "key k3: 1 master and 2 using row(s)") > 0
            local third `line_number'
        }
        file read `fh' line
    }
    file close `fh'
    assert `third' > 0
    if ("`scenario'" == "A") assert `first' == 0
    else {
        assert `first' > 0
        assert (`first' > `third') == `reversed'
    }
end

program define _v121_case
    args stem scenario kind reversed
    tempfile expected notes
    local keys "k1 k2 k3"
    if (`reversed') local keys "k3 k2 k1"
    preserve
    use `"`stem'_master.dta"', clear
    if ("`kind'" == "joinby") joinby `keys' using `"`stem'_`scenario'.dta"'
    else merge `kind' `keys' using `"`stem'_`scenario'.dta"', nogenerate
    sort k1 k2 k3 x y
    save `"`expected'"'
    restore

    parqit use using `"`stem'_master.parquet"'
    log using `"`notes'"', text name(v121)
    if ("`kind'" == "joinby") parqit joinby `keys' using `"`stem'_`scenario'.parquet"'
    else parqit merge `kind' `keys' using `"`stem'_`scenario'.parquet"', nogenerate
    log close v121
    _v121_memory
    _v121_notes `"`notes'"' `scenario' `reversed'
    preserve
    parqit collect, clear
    sort k1 k2 k3 x y
    cf _all using `"`expected'"', all
    assert `"`: variable label x'"' == "Master value"
    assert `"`: variable label y'"' == "Using value"
    assert `"`: data label'"' == "Master metadata"
    restore
    parqit close _all
end

* A has missing k2/k3; B has missing k1/k3. Master has missing k1/k3.
input double k1 str1 k2 double k3 x
1 "a" 1 10
. "b" 2 20
. "c" 3 30
4 "d" . 40
5 "e" 5 50
end
label variable x "Master value"
label data "Master metadata"
save `"`stem'_master.dta"'
parqit save `"`stem'_master.parquet"', data
clear
input double k1 str1 k2 double k3 y
1 "a" 1 100
7 "" 8 700
4 "d" . 400
8 "f" . 800
end
label variable y "Using value"
save `"`stem'_A.dta"'
parqit save `"`stem'_A.parquet"', data
replace k1 = . in 2
replace k2 = "g" in 2
save `"`stem'_B.dta"'
parqit save `"`stem'_B.parquet"', data
keep in 1
parqit save `"`stem'_complete.parquet"', data
clear
input str4 s str1 k2 double k3 x
"1" "a" 1 10
"oops" "b" 2 20
end
parqit save `"`stem'_bad.parquet"', data

clear
set obs 1
gen double sentinel = 1234567890123
label variable sentinel "Current data"
char sentinel[guard] "Preserve me"
label data "Current metadata"
foreach kind in 1:1 m:1 1:m joinby {
    _v121_case `"`stem'"' A `kind' 0
    _v121_case `"`stem'"' A `kind' 1
    _v121_case `"`stem'"' B `kind' 0
    _v121_case `"`stem'"' B `kind' 1
}
di "VERDICT(V121_MISSING_KEY_NOTE_IDENTITY): PASS"

* A using source without missing keys leaves master execution to materialisers
* for m:1/joinby. The 1:1/1:m uniqueness contract still executes it immediately.
foreach kind in 1:1 m:1 1:m joinby {
    parqit sql "SELECT CAST(s AS INTEGER) AS k1, k2, k3, x FROM read_parquet('`stem'_bad.parquet')"
    local early = inlist("`kind'", "1:1", "1:m")
    if ("`kind'" == "joinby") capture noisily parqit joinby k1 k2 k3 using `"`stem'_complete.parquet"'
    else capture noisily parqit merge `kind' k1 k2 k3 using `"`stem'_complete.parquet"', nogenerate
    local rc = _rc
    assert `rc' == cond(`early', 920, 0)
    quietly parqit describe
    assert r(n_steps) == 1 - `early'
    _v121_memory
    if (!`early') {
        capture noisily parqit collect, clear
        assert _rc == 920
        _v121_memory
        tempfile output
        capture noisily parqit save `"`output'.parquet"'
        assert _rc == 920
        _v121_memory
        capture confirm file `"`output'.parquet"'
        assert _rc == 601
        quietly parqit describe
        assert r(n_steps) == 1
    }
    parqit close _all
}
di "VERDICT(V121_MISSING_KEY_ERROR_TIMING): PASS"
