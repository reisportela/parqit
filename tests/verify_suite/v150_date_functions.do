* V150 — BPLIM feedback, third round (2026-10-13): Stata's date and time
*   functions in lazy expressions (DATEFN-1): the period constructors and
*   converters (ym yq yh yw dofq dofh dofy dofw qofd hofd wofd week halfyear),
*   the clock pieces (dofc cofd hh mm ss hms dhms mdyhms hours minutes
*   seconds msofhours msofminutes msofseconds), the calendar differences
*   (datediff age) and the text readers with a mask (date daily clock monthly
*   quarterly halfyearly weekly yearly), with native's rules: extra or missing
*   fields are missing, run-together digits split by the mask, a two-digit
*   year needs 20Y/19Y or topyear, am/pm, a fraction of a second beyond three
*   decimals is missing, and an impossible calendar date is missing.
* Oracle: native Stata's own functions on the same strings and numbers.
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile stem nat
parqit set statamissing off

* ---------- numbers: periods, clock pieces, differences --------------------
clear
input double d double c double y double m double k
21929 1894703445678 2020 7 2
21974 1898553599999 2020 12 4
0 0 1960 1 1
100 8678445000 2021 13 0
. . . . .
22272 -1894703445678 1999 4 53
end
tempfile num
parqit save `"`num'.parquet"', data replace
gen double ymv = ym(y, m)
gen double yqv = yq(y, k)
gen double yhv = yh(y, k)
gen double ywv = yw(y, k)
gen double dq = dofq(k + 240)
gen double dh = dofh(k + 120)
gen double dy = dofy(y)
gen double dw = dofw(k + 3120)
gen double qd = qofd(d)
gen double hd = hofd(d)
gen double wd = wofd(d)
gen double wk = week(d)
gen double hy = halfyear(d)
gen double mo = mofd(d)
gen double dm = dofm(mo)
gen double dc = dofc(c)
gen double cd = cofd(d)
gen double h = hh(c)
gen double mi = mm(c)
gen double se = ss(c)
gen double hmsv = hms(10, 30, 45)
gen double dhmsv = dhms(d, 10, 30, 45)
gen double mdyhmsv = mdyhms(m, k, y, 10, 30, 45)
gen double hrs = hours(c)
gen double mins = minutes(c)
gen double secs = seconds(c)
gen double msh = msofhours(k)
gen double msm = msofminutes(k)
gen double mss = msofseconds(k)
gen double ddm = datediff(d, 22000, "month")
gen double ddy = datediff(d, 22000, "year")
gen double ddd = datediff(d, 22000, "day")
gen double ag = age(d, 22000)
gen double dd2 = datediff(21945, d, "month")
save `"`nat'"', replace
parqit use using `"`num'.parquet"'
parqit gen ymv = ym(y, m)
parqit gen yqv = yq(y, k)
parqit gen yhv = yh(y, k)
parqit gen ywv = yw(y, k)
parqit gen dq = dofq(k + 240)
parqit gen dh = dofh(k + 120)
parqit gen dy = dofy(y)
parqit gen dw = dofw(k + 3120)
parqit gen qd = qofd(d)
parqit gen hd = hofd(d)
parqit gen wd = wofd(d)
parqit gen wk = week(d)
parqit gen hy = halfyear(d)
parqit gen mo = mofd(d)
parqit gen dm = dofm(mo)
parqit gen dc = dofc(c)
parqit gen cd = cofd(d)
parqit gen h = hh(c)
parqit gen mi = mm(c)
parqit gen se = ss(c)
parqit gen hmsv = hms(10, 30, 45)
parqit gen dhmsv = dhms(d, 10, 30, 45)
parqit gen mdyhmsv = mdyhms(m, k, y, 10, 30, 45)
parqit gen hrs = hours(c)
parqit gen mins = minutes(c)
parqit gen secs = seconds(c)
parqit gen msh = msofhours(k)
parqit gen msm = msofminutes(k)
parqit gen mss = msofseconds(k)
parqit gen ddm = datediff(d, 22000, "month")
parqit gen ddy = datediff(d, 22000, "year")
parqit gen ddd = datediff(d, 22000, "day")
parqit gen ag = age(d, 22000)
parqit gen dd2 = datediff(21945, d, "month")
parqit collect, clear
cf _all using `"`nat'"', verbose
parqit close _all

* ---------- text readers with masks -----------------------------------------
clear
input str40 s
"15jan2020 10:30"
"20200115"
"2006125"
"1/15/08"
"15 January 2020"
"jan 15, 2020"
"2020-13-01"
"29feb2019"
"29feb2020"
"15jan99"
"15jan0099"
"15jan100"
"  15jan2020  "
"15-jan-2020 extra"
"15jan2020 10:30:45.678"
"15jan2020 10:30:45 pm"
"15jan2020 10:30:60"
"15jan2020 24:00:00"
"15jan2020 10"
"20200115103045"
"15jan2020 10:30:45.6789"
"2020m7"
"2020-7"
"7/2020"
"2020q3"
"2020h2"
"2020w10"
"2020w53"
"2020"
"July 2020"
"2020m13"
""
end
tempfile txt
parqit save `"`txt'.parquet"', data replace
gen double d_dmy = date(s, "DMY")
gen double d_ymd = date(s, "YMD")
gen double d_mdy = date(s, "MDY")
gen double d_md20y = date(s, "MD20Y")
gen double d_mdy_top = date(s, "MDY", 2050)
gen double d_dm19y = date(s, "DM19Y")
gen double d_dmy_top = date(s, "DMY", 2020)
gen double dl = daily(s, "DMY")
gen double c_dmyhm = clock(s, "DMYhm")
gen double c_dmyhms = clock(s, "DMYhms")
gen double c_ymdhms = clock(s, "YMDhms")
gen double c_dmy = clock(s, "DMY")
gen double c_dmyh = clock(s, "DMYh")
gen double c_top = clock(s, "MDYhm", 2050)
gen double mo = monthly(s, "YM")
gen double mo2 = monthly(s, "MY")
gen double q = quarterly(s, "YQ")
gen double h = halfyearly(s, "YH")
gen double w = weekly(s, "YW")
gen double y = yearly(s, "Y")
save `"`nat'"', replace
parqit use using `"`txt'.parquet"'
parqit gen d_dmy = date(s, "DMY")
parqit gen d_ymd = date(s, "YMD")
parqit gen d_mdy = date(s, "MDY")
parqit gen d_md20y = date(s, "MD20Y")
parqit gen d_mdy_top = date(s, "MDY", 2050)
parqit gen d_dm19y = date(s, "DM19Y")
parqit gen d_dmy_top = date(s, "DMY", 2020)
parqit gen dl = daily(s, "DMY")
parqit gen c_dmyhm = clock(s, "DMYhm")
parqit gen c_dmyhms = clock(s, "DMYhms")
parqit gen c_ymdhms = clock(s, "YMDhms")
parqit gen c_dmy = clock(s, "DMY")
parqit gen c_dmyh = clock(s, "DMYh")
parqit gen c_top = clock(s, "MDYhm", 2050)
parqit gen mo = monthly(s, "YM")
parqit gen mo2 = monthly(s, "MY")
parqit gen q = quarterly(s, "YQ")
parqit gen h = halfyearly(s, "YH")
parqit gen w = weekly(s, "YW")
parqit gen y = yearly(s, "Y")
parqit collect, clear
cf _all using `"`nat'"', verbose
* the # of a mask, am/pm twice, dotted dates, a leading unit letter and the
* fraction of a second: native's rules on a third set of strings
clear
input str40 s
"15jan2020 extra"
"extra 15jan2020"
"15jan2020"
"15 jan 2020 foo bar"
"15jan2020 10:30 extra"
"15jan2020 extra 10:30"
"15jan2020 10:30 am pm"
"15jan2020 10:30 am am"
"15.01.2020 10:30:45"
"2020.01.15"
"2020.01.15 10:30"
"20200115103045.678"
"15jan2020 10:30:45.678"
"15jan2020 10:30.5"
"15jan2020 10.30"
"x2020m7"
"2020m7 extra"
"m7 2020"
"15jan2020 1030"
"15jan2020 103045.5"
end
tempfile txt3
parqit save `"`txt3'.parquet"', data replace
gen double d_dmy = date(s, "DMY")
gen double d_dmyh = date(s, "DMY#")
gen double d_hdmy = date(s, "#DMY")
gen double c_dmyhm = clock(s, "DMYhm")
gen double c_dmyhmh = clock(s, "DMYhm#")
gen double c_dmyhhm = clock(s, "DMY#hm")
gen double c_dmyhms = clock(s, "DMYhms")
gen double c_ymd = clock(s, "YMD")
gen double c_ymdhm = clock(s, "YMDhm")
gen double c_ymdhms = clock(s, "YMDhms")
gen double mo = monthly(s, "YM")
gen double mo2 = monthly(s, "MY")
save `"`nat'"', replace
parqit use using `"`txt3'.parquet"'
parqit gen d_dmy = date(s, "DMY")
parqit gen d_dmyh = date(s, "DMY#")
parqit gen d_hdmy = date(s, "#DMY")
parqit gen c_dmyhm = clock(s, "DMYhm")
parqit gen c_dmyhmh = clock(s, "DMYhm#")
parqit gen c_dmyhhm = clock(s, "DMY#hm")
parqit gen c_dmyhms = clock(s, "DMYhms")
parqit gen c_ymd = clock(s, "YMD")
parqit gen c_ymdhm = clock(s, "YMDhm")
parqit gen c_ymdhms = clock(s, "YMDhms")
parqit gen mo = monthly(s, "YM")
parqit gen mo2 = monthly(s, "MY")
parqit collect, clear
cf _all using `"`nat'"', verbose
parqit close _all

* the readers also serve a filter, a date literal and a Stata date format
parqit use using `"`txt'.parquet"'
parqit keep if date(s, "DMY") >= td(01jan2020)
parqit count
* "15 January 2020", "29feb2020" and "  15jan2020  "; "15-jan-2020 extra" is
* missing in native (a stray word), as are the strings with a time
assert r(N) == 3
* refusals: a mask that is not a literal, an unknown mask letter, a letter twice
capture noisily parqit gen z = date(s, s)
assert _rc == 198
capture noisily parqit gen z = date(s, "DMX")
assert _rc == 198
capture noisily parqit gen z = clock(s, "DMYYhm")
assert _rc == 198
capture noisily parqit gen z = monthly(s, "YQ")
assert _rc == 198
capture noisily parqit gen z = datediff(td(01jan2020), td(01feb2020), "week")
assert _rc == 198
parqit close _all

di "VERDICT(V150_DATE_FUNCTIONS): PASS - the period, clock, difference and text-reading date functions equal native Stata on the same inputs, missing where native is missing (extra or missing fields, stray words, a second am/pm, bad years, impossible dates, 24:00, a 60th second, four decimals), # skipping a field or the rest, dots separating dates and the fraction of a second as native reads them; a non-literal or malformed mask is refused"
