* Native Arrow settings preserve values and metadata in serial and parallel fill.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
parqit set threads 2
set obs 4103
gen long id = _n
gen byte b = mod(_n,11)-5
gen int k = mod(_n,101)-50
gen long l = 2147000000-_n
gen float f = _n/11
gen double d = _n/7
gen double big = 2^53-mod(_n,4)
gen double day = mdy(1,1,2000)+_n
format day %td
gen double clock = clock("01jan2000 12:00:00","DMYhms")+_n
format clock %tc
gen str12 short = cond(mod(_n,5)==0,"","éA"+string(mod(_n,10)))
gen str60 text = "synthetic O'Brien é longer than twelve "+string(_n)
gen strL longtext = ""
mata: st_sstore(., "longtext", J(st_nobs(),1,invtokens(J(1,2100,"x"),"")))
replace longtext = longtext+" é "+string(_n)
replace b = . if mod(_n,19)==0
replace d = . if mod(_n,23)==0
replace longtext = "" if mod(_n,29)==0
label variable d "Synthetic precision label é"
label define idlabel 1 "first é" 2 "second"
label values id idlabel
char _dta[settings_test] "native transfer metadata"
notes: synthetic native settings transfer test
tempfile expected
save `"`expected'"'
parqit open _data, name(native_source)
parqit use using view:native_source, name(native_staged)
parqit view native_staged: gen double transformed = d*2

forvalues profile=1/4 {
    quietly parqit set duckdb arrow_large_buffer_size false
    quietly parqit set duckdb arrow_lossless_conversion false
    quietly parqit set duckdb arrow_output_list_view false
    quietly parqit set duckdb produce_arrow_string_view false
    quietly parqit set duckdb arrow_output_version "1.0"
    if `profile'==2 quietly parqit set duckdb arrow_large_buffer_size true
    if `profile'==3 {
        quietly parqit set duckdb arrow_output_version "1.4"
        quietly parqit set duckdb produce_arrow_string_view true
    }
    if `profile'==4 {
        quietly parqit set duckdb arrow_large_buffer_size true
        quietly parqit set duckdb arrow_lossless_conversion true
        quietly parqit set duckdb arrow_output_list_view true
        quietly parqit set duckdb arrow_output_version "1.4"
        quietly parqit set duckdb produce_arrow_string_view true
    }
    foreach workers in 1 2 {
        parqit set fill_threads `workers'
        parqit view native_source: collect, clear
        cf _all using `"`expected'"'
        assert "`: variable label d'"=="Synthetic precision label é"
        assert "`: value label id'"=="idlabel"
        assert "`: format day'"=="%td" & "`: format clock'"=="%tc"
        assert "`: char _dta[settings_test]'"=="native transfer metadata"
        assert "`: char _dta[note1]'"=="synthetic native settings transfer test"
        parqit view native_staged: collect, clear
        assert transformed==d*2
        drop transformed
        cf _all using `"`expected'"'
    }
}
quietly parqit set duckdb reset arrow_large_buffer_size
quietly parqit set duckdb reset arrow_lossless_conversion
quietly parqit set duckdb reset arrow_output_list_view
quietly parqit set duckdb reset produce_arrow_string_view
quietly parqit set duckdb reset arrow_output_version
parqit close _all
display "VERDICT(V199_DUCKDB_SETTINGS_TRANSFER): PASS"
