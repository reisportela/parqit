* V158: reproduced findings from the Opus 5.5 debate, independently checked
* against native Stata. Inputs are synthetic; failed use must preserve bridges.
clear all
set more off
set linesize 255
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
parqit set statamissing on
tempfile a b nat master

* Nullable strings introduced by append sort before nonmissing strings.
input id str1 s
1 "b"
2 "a"
end
save "`master'", replace
parqit save "`a'.parquet", data replace
clear
input id
3
end
parqit save "`b'.parquet", data replace
append using "`master'"
sort s id
gen double pos = _n
keep in 1/2
save "`nat'", replace
parqit use using "`b'.parquet"
parqit append using "`a'.parquet"
parqit sort s id
parqit gen double pos = _n
parqit keep in 1/2
parqit collect, clear
cf _all using "`nat'"
parqit close _all

* String missing group codes and labels; no gaps without missing.
clear
input id str1 s
1 "b"
2 ""
3 "a"
4 ""
end
egen double expected = group(s), missing label
decode expected, gen(expected_text)
egen double complete = group(s)
parqit save "`a'.parquet", data replace
parqit use using "`a'.parquet"
parqit egen double actual = group(s), missing label
parqit egen double ac = group(s)
parqit collect, clear
decode actual, gen(actual_text)
assert actual == expected & ac == complete
assert actual_text == expected_text
parqit close _all

* DOUBLE keys retain their value labels and integer spelling.
clear
input double region
1
2
3
.
end
label define reg 1 "North" 2 "South"
label values region reg
egen double expected = group(region), missing label
decode expected, gen(expected_text)
parqit save "`a'.parquet", data replace
parqit use using "`a'.parquet"
parqit egen double actual = group(region), missing label
parqit collect, clear
decode actual, gen(actual_text)
assert actual == expected & actual_text == expected_text
parqit close _all

* A rejected use leaves the active view, named target and owned bridge intact.
save "`master'.dta", replace
parqit use using "`master'.dta", name(old)
parqit gen double extra = 7
parqit use using "`a'.parquet", name(other)
capture noisily parqit use if nosuchvar > 1 using "`master'.dta", name(old)
assert _rc == 198
parqit collect, clear
capture confirm variable extra
assert _rc == 111
parqit view old
parqit collect, clear
assert extra == 7
capture noisily parqit use nosuchvar if region == 1 using "`master'.dta", name(old)
assert _rc == 198
parqit collect, clear
assert extra == 7
capture noisily parqit use if nosuchvar using "`a'.parquet", name(absent)
assert _rc == 198
capture noisily parqit view absent
assert _rc == 198
parqit view old
parqit collect, clear
assert extra == 7
parqit use region if region == 1 using "`a'.parquet", name(old)
parqit collect, clear
assert _N == 1 & c(k) == 1 & region == 1
parqit close _all

* FLOAT window results still evaluate against LONG/literals in double.
clear
input float x long y
16777216 16777217
16777216 16777217
end
gen double t = _n
tsset t
label define big 16777217 "wrong"
label values x big
egen expected_label = concat(x), decode
gen double e1 = x[_n-1] == y
gen double e2 = x[_n-1] == 16777217
gen double e3 = x[_n-1]+1 == 16777216
gen double e4 = L.x == y
parqit save "`a'.parquet", data replace
parqit use using "`a'.parquet"
parqit sort t
parqit tsset t
parqit gen double a1 = x[_n-1] == y
parqit gen double a2 = x[_n-1] == 16777217
parqit gen double a3 = x[_n-1]+1 == 16777216
parqit gen double a4 = L.x == y
parqit egen actual_label = concat(x), decode
parqit collect, clear
assert e1 == a1 & e2 == a2 & e3 == a3 & e4 == a4
assert actual_label == expected_label
parqit close _all

* The group-relative qualifier follows native total() on an explicit order.
clear
input id g x
1 1 10
2 2 20
3 1 30
4 2 40
5 1 90
end
sort g id
egen double expected = total(x) if _n <= 2, by(g)
egen double expected_mean = mean(x), by(g)
parqit save "`a'.parquet", data replace
parqit use using "`a'.parquet"
parqit sort g id
parqit egen double actual = total(x) if _n <= 2, by(g)
parqit collect, clear
assert actual == expected
parqit sort id
capture noisily parqit by g: egen bad = mean(x)
assert _rc == 198
parqit egen double byoption = mean(x), by(g)
parqit collect, clear
assert byoption == expected_mean
parqit bysort g (id): egen double accepted = mean(x)
parqit close _all

* Decode tolerates .a-.z labels even after values have folded to ordinary .
clear
input double x
1
.
end
label define xl 1 "one" .a "refused"
label values x xl
gen double y = .
label define yl .a "only extended label"
label values y yl
egen expected = concat(x), decode
egen expected_y = concat(y), decode
parqit save "`a'.parquet", data replace
parqit use using "`a'.parquet"
parqit egen actual = concat(x), decode
parqit egen actual_y = concat(y), decode
parqit collect, clear
assert actual == expected & actual_y == expected_y
parqit close _all

* Fractions survive JSON; real/numlist parsing retains native Stata semantics.
clear
set obs 5
gen double x = (_n-1)/4
egen double expected = cut(x), at(0 .5 1)
egen double es = std(x), mean(.25) sd(.5)
egen double ep = pctile(x), p(.5)
egen double expected_exact = std(x), mean(.30000000000000004)
parqit save "`a'.parquet", data replace
parqit use using "`a'.parquet"
parqit egen double actual = cut(x), at(0 .5 1)
parqit egen double as = std(x), mean(.25) sd(.5)
parqit egen double ap = pctile(x), p(.5)
parqit egen double exact = std(x), mean(.30000000000000004)
parqit collect, clear
assert actual == expected & as == es & ap == ep
assert exact == expected_exact if x == .5
parqit close _all

* Calendar domain checks are row-local, including extreme period counts.
clear
input double q
0
240
1e10
-1e10
1e100
end
foreach f in dofq dofh dofw {
    gen double e_`f' = `f'(q)
}
parqit save "`a'.parquet", data replace
parqit use using "`a'.parquet"
foreach f in dofq dofh dofw {
    parqit gen double a_`f' = `f'(q)
}
parqit collect, clear
foreach f in dofq dofh dofw {
    assert a_`f' == e_`f'
}
parqit close _all
clear
input double h double m double s double d
0 0 0 0
1.5 2.5 3.3333 .5
23.9 59.9 59.9999 2936549
24 0 0 0
-.1 0 0 0
0 60 0 0
0 0 60 0
0 0 0 2936550
0 0 0 -679351
. 0 0 0
0 . 0 0
0 0 . 0
0 0 0 .
end
gen double eh = hms(h,m,s)
gen double ed = dhms(d,h,m,s)
gen double em = mdyhms(1,1,2020,h,m,s)
gen double ec = clock("15jan2020 10:30:00", "DMY hms")
parqit save "`a'.parquet", data replace
parqit use using "`a'.parquet"
parqit gen double ah = hms(h,m,s)
parqit gen double ad = dhms(d,h,m,s)
parqit gen double am = mdyhms(1,1,2020,h,m,s)
parqit gen double ac = clock("15jan2020 10:30:00", "DMY hms")
parqit collect, clear
assert ah == eh & ad == ed & am == em & ac == ec
capture noisily parqit gen bad = td(4294967297jan2020)
assert _rc == 198
capture noisily parqit gen bad = tc(01jan2020 4294967296:00:00)
assert _rc == 198
parqit close _all
display "VERDICT(V158_OPUS_ADVERSARIAL): PASS - atomic use, string order/group labels, float windows, by validation, decode, numeric JSON and date domains match native oracles"
