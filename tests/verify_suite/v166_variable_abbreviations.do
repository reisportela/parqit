* VARABBREV-1: names are resolved against the view/file, never against the
* unrelated data in memory. Native Stata is the numerical and naming oracle.
clear all
set more off
set linesize 255
set varabbrev on
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile stem native
set obs 12
gen int panel_id = ceil(_n/4)
gen int year_number = mod(_n-1,4) + 2020
gen float income = _n/10
gen double interest = _n/100
gen double interest_rate = _n/1000
gen int inc = _n*10
gen str5 industry = cond(panel_id==1, "A", "B")
gen int café_value = _n + 100
parqit save `"`stem'_source.parquet"', data
save `native'

parqit use using `"`stem'_source.parquet"', name(source)
* Canonical exact name wins over income; a shorter ambiguous prefix fails.
summarize inc, meanonly
local exact_mean = r(mean)
parqit summarize inc
assert r(mean) == `exact_mean'
summarize inco, meanonly
local income_mean = r(mean)
parqit su inco
assert r(mean) == `income_mean'
capture noisily parqit su inter
assert _rc == 111
capture noisily parqit gen bad = inter + 1
assert _rc == 198
parqit count
assert r(N) == 12

* Conditions, functions, subscripts, prefixes, and new names.
gen double literal = cond(indus == "inco", 0, sqrt(inco))
bysort pan (yea): gen double previous = inco[_n-1]
egen double grouped = mean(inco), by(pan)
parqit gen double literal = cond(indus == "inco", 0, sqrt(inco))
parqit bysort pan (yea): gen double previous = inco[_n-1]
parqit egen double grouped = mean(inco), by(pan)
replace inco = inco*2 if yea == 2021
parqit replace inco = inco*2 if yea == 2021
save `native', replace
parqit collect, clear
sort panel_id year_number
cf _all using `native', all
local abbreviated_type : type income
parqit use using `"`stem'_source.parquet"', name(full_type)
parqit replace income = income*2 if year_number == 2021
parqit collect, clear
assert "`: type income'" == "`abbreviated_type'"
parqit view source
parqit format year_n %ty
parqit list inco café_v if yea == 2021
parqit count if yea == 2021
assert r(N) == 3
capture noisily parqit replace inco = inco[_n-1]
assert _rc == 198
parqit unique pan yea
assert r(unique) == 12
parqit tabstat inco, by(indus) stats(mean) save
parqit ds
assert strpos(" `r(varlist)' ", " bad ") == 0

* Abbreviation follows each command's setting, also on an existing view.
set varabbrev off
capture noisily parqit summarize inco
assert _rc == 111
capture noisily parqit gen bad = inco
assert _rc == 198
capture noisily parqit count if inco > 0
assert _rc == 198
capture noisily parqit list inco
assert _rc == 111
capture noisily parqit format year_n %ty
assert _rc != 0
parqit summarize income
parqit use income* using view:source, name(wildcard)
parqit ds
assert "`r(varlist)'" == "income"
set varabbrev on
parqit view source

* Fresh view copies, rename and collapse resolve only existing names.
parqit use pan yea inco using view:source, name(copy)
parqit rename (pan yea) (panel year)
parqit collapse (mean) inco, by(pan)
parqit ds
assert "`r(varlist)'" == "panel income"
parqit collect, clear
assert _N == 3
parqit use using `"`stem'_source.parquet"', name(aggregate)
parqit collapse (sum) inco, by(pan)
parqit collect, clear
rename income sum_income
tempfile sumresult
save `sumresult'
parqit use using `"`stem'_source.parquet"', clear
collapse (sum) inco, by(pan)
rename income sum_income
cf _all using `sumresult', all

* Eager selection and filtering use the file's schema, independent of memory.
clear
set obs 1
gen str1 income = "x"
parqit use inco café_v if yea == 2021 using `"`stem'_source.parquet"', clear
assert _N == 3 & c(k) == 2
assert "`: type income'" == "float"
assert café_value == 102 + 4*(_n-1)
set varabbrev off
capture noisily parqit use inco using `"`stem'_source.parquet"', clear
assert _rc == 111
assert _N == 3 & c(k) == 2
set varabbrev on
capture noisily parqit use inter using `"`stem'_source.parquet"', clear
assert _rc == 111
assert _N == 3 & c(k) == 2

* Join keys, keepusing(), keep(), and partition_by().
clear
set obs 3
gen int panel_id = _n
gen double bonus_amount = _n*100
parqit save `"`stem'_lookup.parquet"', data
parqit save `"`stem'_memorytree"', data partition_by(pan)
set varabbrev off
capture noisily parqit save `"`stem'_refusedtree"', data partition_by(pan)
assert _rc == 111
set varabbrev on
parqit use using `"`stem'_source.parquet"', clear
parqit mergein m:1 pan using `"`stem'_lookup.parquet"', keepusing(bon) nogen
assert bonus_amount == panel_id*100
parqit use using `"`stem'_source.parquet"', name(joined)
parqit merge m:1 pan using `"`stem'_lookup.parquet"', keepusing(bon) nogen
parqit save `"`stem'_tree"', view partition_by(pan)
parqit collect, clear
assert _N == 12
assert bonus_amount == panel_id*100
parqit use using `"`stem'_source.parquet"', name(appended)
parqit append using `"`stem'_source.parquet"', keep(pan inco)
parqit collect, clear
assert _N == 24
count if missing(inc)
assert r(N) == 12
count if missing(income)
assert r(N) == 0

* Reshape input names and pivot sources/options are resolved before probes;
* reshape-long stubs and its newly created j() variable remain literal.
clear
set obs 3
gen int panel_id = _n
gen double income2020 = _n
gen double income2021 = _n + 10
save `native', replace
parqit open _data, name(shaped)
parqit reshape long income, i(pan) j(year_number)
parqit reshape wide inc, i(pan) j(yea)
parqit collect, clear
sort panel_id
cf _all using `native', all
parqit use using `"`stem'_source.parquet"', name(pivoted)
parqit pivot (mean) inco, rows(pan) cols(yea)
parqit collect, clear
assert _N == 3 & c(k) == 5
assert income2020 == float((4*panel_id-3)/10)
assert income2023 == float(4*panel_id/10)

* Case and Unicode remain significant, including the exposed alias of a
* column that DuckDB internally holds under a suffixed name.
clear
set obs 2
gen double value = _n
gen double VALUE = 10*_n
gen double café_value = 100*_n
parqit open _data, name(cases)
parqit gen double result = val + VAL + café_v
parqit format VAL %12.2f
parqit collect, clear
assert result == 111*_n
assert "`: format VALUE'" == "%12.2f"
assert "`: format value'" != "%12.2f"
parqit close _all
di "VERDICT(V166_VARIABLE_ABBREVIATIONS): PASS"
