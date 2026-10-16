* FORMAT-1 / TS-KEY-LAG-1: native formats and time/panel lags, plus a
* PyArrow oracle for physical date units, metadata and unchanged values.
clear all
set more off
set varabbrev on
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile stem original expected
set obs 6
gen long row_id = _n
gen long daily = td(15oct2026) + _n - 1
gen double clocktime = clock("15oct2026 12:34:56", "DMYhms") + _n*1234
gen int monthly = ym(2020,1) + _n
gen int quarterly = yq(2020,1) + _n
gen int halfyear = yh(2020,1) + _n
gen int weekly = yw(2020,1) + _n
gen int yearly = 2020 + _n
gen double amount = _n/8
gen str12 caption = "record " + string(_n)
label var daily "Event date"
char daily[origin] "synthetic"
notes daily: retained through format
save `original'
parqit open _data
parqit format daily %tdCCYY-NN-DD
parqit format %tcCCYY-NN-DD_HH:MM:SS.sss clocktime
parqit form monthly %tm
parqit format quarterly %tq
parqit format halfyear %th
parqit format weekly %tw
parqit format yearly %ty
parqit format amount %12,3f
parqit format caption %-12s
parqit format daily clocktime
parqit format
* In-memory values and formats have not changed.
cf _all using `original', all
assert "`: format daily'" == "%12.0g"
assert c(changed) == 0
local original_frame `c(frame)'
capture noisily parqit format amount caption %9.2f
assert _rc == 109
capture noisily parqit format daily %not_a_format
assert _rc == 120
assert "`c(frame)'" == "`original_frame'"
parqit save `"`stem'_formatted.parquet"', view
shell python3 "`repo'/tests/verify_suite/v165_format_oracle.py" "`stem'_formatted.parquet" "`stem'_oracle.ok" "`stem'_bad_format.parquet"
confirm file `"`stem'_oracle.ok"'
parqit collect, clear
cf _all using `original', all
assert "`: format daily'" == "%tdCCYY-NN-DD"
assert "`: format clocktime'" == "%tcCCYY-NN-DD_HH:MM:SS.sss"
assert "`: format monthly'" == "%tm"
assert "`: format amount'" == "%12,3f"
assert "`: format caption'" == "%-12s"
assert "`: variable label daily'" == "Event date"
assert "`: char daily[origin]'" == "synthetic"
assert "`: char daily[note1]'" == "retained through format"
save `expected'
parqit use using `"`stem'_formatted.parquet"', clear
cf _all using `expected', all
assert "`: format clocktime'" == "%tcCCYY-NN-DD_HH:MM:SS.sss"
assert "`: format daily'" == "%tdCCYY-NN-DD"
* Changing a physical DATE's display format back to numeric retains day counts.
parqit use using `"`stem'_formatted.parquet"'
parqit format daily %12.0g
parqit use using view:default, name(format_copy)
parqit format daily %td
parqit view default
parqit collect, clear
cf _all using `original', all
assert "`: format daily'" == "%12.0g"
parqit view format_copy
parqit collect, clear
assert "`: format daily'" == "%td"
parqit close _all

* An explicit format can repair an invalid format in a foreign footer.
parqit use using `"`stem'_bad_format.parquet"'
parqit format daily %td
parqit collect, clear
assert daily == td(15oct2026) + row_id - 1
assert "`: format daily'" == "%td"
parqit close _all

* A case alias may exceed Stata's 32-character name limit internally.
clear
set obs 2
local lower aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
local upper AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA
gen double `lower' = _n
gen double `upper' = 10*_n
parqit open _data
parqit format `upper' %12.2f
parqit format `lower' `upper'
parqit collect, clear
assert `lower' == _n & `upper' == 10*_n
assert "`: format `upper''" == "%12.2f"
assert "`: format `lower''" != "%12.2f"
parqit close _all

* The native lag oracle includes gaps, delta(2), panels and a missing time.
clear
set obs 8
gen int panel_id = ceil(_n/4)
gen int timevar = 2*mod(_n-1,4)
replace timevar = timevar + 2 if mod(_n,4)>=3 | mod(_n,4)==0
replace timevar = . in 8
gen double outcome = _n/10
xtset panel_id timevar, delta(2)
local k 0
foreach expr in L.timevar F.timevar D.timevar S2.timevar L.panel_id F.panel_id L.outcome L2.outcome {
    local ++k
    gen double native`k' = `expr'
}
parqit open _data
parqit xtset pan timev, delta(2)
local k 0
foreach expr in L.timev F.timev D.timev S2.timev L.pan F.pan L.out L2.out {
    local ++k
    parqit gen double result`k' = `expr'
}
parqit sort panel_id timevar
parqit collect, clear
forvalues k=1/8 {
    assert result`k' == native`k' if !missing(timevar)
    * Existing contract: every nonzero shift at a missing time is missing.
    * Native F.panel_id may return the same panel there (known divergence).
    assert missing(result`k') if missing(timevar)
}
parqit close _all
di "VERDICT(V165_FORMAT_KEY_LAGS): PASS"
