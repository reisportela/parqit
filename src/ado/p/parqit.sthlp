{smcl}
{* *! version 0.5.0 16oct2026}{...}
{viewerdialog "parqit use" "dialog parqit_read"}{...}
{viewerdialog "parqit describe" "dialog parqit_explore"}{...}
{viewerdialog "parqit summarize" "dialog parqit_stats"}{...}
{viewerdialog "parqit keep if/in, sample" "dialog parqit_filter"}{...}
{viewerdialog "parqit keep/drop/order/sort/rename" "dialog parqit_vars"}{...}
{viewerdialog "parqit generate/spssencode" "dialog parqit_gen"}{...}
{viewerdialog "parqit collapse/pivot/contract/reshape" "dialog parqit_pivot"}{...}
{viewerdialog "parqit merge/append/joinby" "dialog parqit_combine"}{...}
{viewerdialog "parqit collect/save" "dialog parqit_write"}{...}
{viewerdialog "parqit view/sql/set" "dialog parqit_views"}{...}
{vieweralsosee "[PARQIT] parqit_technical" "help parqit_technical"}{...}
{vieweralsosee "" "--"}{...}
{vieweralsosee "[D] use" "help use"}{...}
{vieweralsosee "[D] save" "help save"}{...}
{vieweralsosee "[D] collapse" "help collapse"}{...}
{vieweralsosee "[D] merge" "help merge"}{...}
{vieweralsosee "[D] frames" "help frames"}{...}
{viewerjumpto "At a glance" "parqit##map"}{...}
{viewerjumpto "Syntax" "parqit##syntax"}{...}
{viewerjumpto "Menu" "parqit##menu"}{...}
{viewerjumpto "Description" "parqit##description"}{...}
{viewerjumpto "Quick start" "parqit##quickstart"}{...}
{viewerjumpto "Options" "parqit##options"}{...}
{viewerjumpto "Views" "parqit##lazy"}{...}
{viewerjumpto "Lazy verbs" "parqit##verbs"}{...}
{viewerjumpto "Getting the result" "parqit##materialisers"}{...}
{viewerjumpto "Exploring a view" "parqit##explore"}{...}
{viewerjumpto "Other file formats and encodings" "parqit##formats"}{...}
{viewerjumpto "Expressions and missing values" "parqit##expressions"}{...}
{viewerjumpto "Settings and diagnostics" "parqit##settings"}{...}
{viewerjumpto "Examples" "parqit##examples"}{...}
{viewerjumpto "Limitations" "parqit##limitations"}{...}
{viewerjumpto "Stored results" "parqit##results"}{...}
{viewerjumpto "Authors" "parqit##author"}{...}
{p2colset 1 11 13 2}{...}
{p2col:{bf:parqit} {hline 2}}A grammar of data manipulation for Stata, backed by
Parquet on an embedded DuckDB engine{p_end}
{p2colreset}{...}

{pstd}
{bf:This is the user's guide.} The technical reference,
{help parqit_technical:{bf:help parqit_technical}}, holds the contracts behind
every command: the Parquet metadata layout, the type mapping, the file formats
and text encodings, precision, atomicity, performance and the complete list of
limitations.


{marker map}{...}
{title:At a glance}

{pstd}
{cmd:parqit} is for data too large, or too slow, to load into Stata. It opens a
file as a {it:view}: a plan held by the engine, with the source data on disk. You shape
the view with the Stata commands you already know, look at it through summaries
computed by the engine, and bring into Stata only the result you ask for, or
write it straight to a Parquet file. An embedded DuckDB engine runs the plan out
of core, and variable and value labels, notes and formats travel with the data.

{pstd}
A session has four moves; only the last one produces the full result:

   {c TLC}{c -} {bf:1  OPEN} {c -} start a view {c -} {help parqit##lazy:[more]}
   {c |}
   {c |}{space 4}{cmd:parqit use} {it:file}{space 11}a Parquet file, glob or Hive directory,
   {c |}{space 30}or delimited text, Stata, Excel, SPSS or R
   {c |}{space 4}{cmd:parqit use view:}{it:name}{space 6}a copy of an open view, with {opt name()}
   {c |}{space 4}{cmd:parqit open _data}{space 9}the dataset already in Stata's memory
   {c |}{space 4}{cmd:parqit sql} {cmd:"}{it:SELECT ...}{cmd:"}{space 3}any DuckDB query
   {c |}
   {c LT}{c -} {bf:2  SHAPE} {c -} lazy verbs; each one extends the plan {c -} {help parqit##verbs:[more]}
   {c |}
   {c |}{space 4}rows{space 9}{cmd:keep} {cmd:drop} {cmd:sample} {cmd:duplicates drop}
   {c |}{space 4}columns{space 6}{cmd:gen} {cmd:egen} {cmd:replace} {cmd:rename} {cmd:order}
   {c |}{space 4}order{space 8}{cmd:sort} {cmd:gsort}
   {c |}{space 4}aggregate{space 4}{cmd:collapse} {cmd:contract} {cmd:pivot}
   {c |}{space 4}restructure{space 2}{cmd:reshape long} {cmd:reshape wide}
   {c |}{space 4}two tables{space 3}{cmd:merge} {cmd:append} {cmd:joinby}
   {c |}
   {c LT}{c -} {bf:3  LOOK} {c -} engine-side summaries; memory unchanged {c -} {help parqit##explore:[more]}
   {c |}
   {c |}{space 4}shape{space 8}{cmd:describe} {cmd:ds} {cmd:lookfor} {cmd:codebook}
   {c |}{space 4}rows{space 9}{cmd:count} {cmd:list} {cmd:levelsof} {cmd:unique}
   {c |}{space 4}statistics{space 3}{cmd:summarize} {cmd:tabstat} {cmd:tabulate} {cmd:histogram}
   {c |}{space 17}{cmd:correlate} {cmd:pwcorr}
   {c |}{space 4}quality{space 6}{cmd:misstable} {cmd:duplicates report} {cmd:duplicates list}
   {c |}
   {c BLC}{c -} {bf:4  LAND} {c -} run the plan and keep the result {c -} {help parqit##materialisers:[more]}

   {space 5}{cmd:parqit collect}{space 12}load the result into Stata, atomically
   {space 5}{cmd:parqit save} {it:file}{space 10}write the result to a Parquet file

{pstd}
The order is a habit, not a rule: look whenever you like, shape again after
looking, and collect or save as often as you need {c -} the view stays open and
re-executes each time.

{pstd}
Alongside the four moves, at any point in the session:

   {space 5}the plan{space 5}{cmd:parqit show} {cmd:parqit explain}
   {space 5}views{space 8}{cmd:parqit views} {cmd:parqit view} {cmd:parqit close}
   {space 18}a copy: {cmd:parqit use view:}{it:name}
   {space 5}in memory{space 4}{cmd:parqit mergein} {cmd:parqit appendin} (native merge/append with a file parqit reads)
   {space 5}engine{space 7}{cmd:parqit set} {cmd:parqit path}
   {space 5}install{space 6}{cmd:parqit version} {cmd:parqit selftest} {cmd:parqit menu}

{pstd}
The rest of this entry gives the syntax, the options and worked examples;
{help parqit_technical} gives the contracts behind them.


{marker syntax}{...}
{title:Syntax}

{pstd}
Open a view over a file, or read a file into memory

{p 8 16 2}
{cmd:parqit} {cmdab:u:se} [{it:varlist}] [{cmd:if} {it:exp}] {cmd:using} {it:filename} [{cmd:,}
{it:{help parqit##use_options:use_options}}]

{p 8 16 2}
{cmd:parqit} {cmdab:u:se} {it:filename} [{cmd:,} {it:use_options}]

{p 8 16 2}
{cmd:parqit} {cmdab:u:se} [{it:varlist}] {cmd:using view:}{it:viewname}{cmd:,}
{opt n:ame(newview)}

{p 8 16 2}
{cmd:parqit open _data} [{cmd:,} {opt n:ame(viewname)} {opt enc:oding(name)}]

{p 8 16 2}{cmd:parqit sql} {cmd:"}{it:DuckDB SQL}{cmd:"} [{cmd:,} {opt clear} {opt n:ame(viewname)}]{p_end}

{pstd}
Shape the view (lazy: each verb extends the plan)

{p 8 16 2}{cmd:parqit keep} {it:varlist} | {cmd:parqit keep if} {it:exp} | {cmd:parqit keep in} {it:f}[{cmd:/}{it:l}]{p_end}
{p 8 16 2}{cmd:parqit drop} {it:varlist} | {cmd:parqit drop if} {it:exp} | {cmd:parqit drop in} {it:f}[{cmd:/}{it:l}]{p_end}
{p 8 16 2}{cmd:parqit} {cmdab:g:enerate} [{it:type}] {it:newvar} {cmd:=} {it:exp} [{cmd:if} {it:exp}]{p_end}
{p 8 16 2}{cmd:parqit replace} {it:var} {cmd:=} {it:exp} [{cmd:if} {it:exp}]{p_end}
{p 8 16 2}{cmd:parqit egen} [{it:type}] {it:newvar} {cmd:=} {it:fcn}{cmd:(}{it:arguments}{cmd:)} [{cmd:if} {it:exp}]
[{cmd:,} {opt by(varlist)} {it:fcn_options}]{p_end}
{p 8 16 2}{cmd:parqit by} {it:varlist} [{cmd:(}{it:sortvars}{cmd:)}]{cmd::} {it:parqit_command}{space 4}(gen, replace, egen, keep if, drop if){p_end}
{p 8 16 2}{cmd:parqit} {cmdab:bys:ort} {it:varlist} [{cmd:(}{it:sortvars}{cmd:)}]{cmd::} {it:parqit_command}{p_end}
{p 8 16 2}{cmd:parqit} {cmdab:ren:ame} {it:old} {it:new}{p_end}
{p 8 16 2}{cmd:parqit} {cmdab:ren:ame} {cmd:(}{it:oldlist}{cmd:)} {cmd:(}{it:newlist}{cmd:)}{p_end}
{p 8 16 2}{cmd:parqit order} {it:varlist}{p_end}
{p 8 16 2}{cmd:parqit} {cmdab:form:at} {it:varlist} {cmd:%}{it:fmt}{p_end}
{p 8 16 2}{cmd:parqit} {cmdab:form:at} {cmd:%}{it:fmt} {it:varlist}{p_end}
{p 8 16 2}{cmd:parqit} {cmdab:form:at} [{it:varlist}]{space 4}(display formats){p_end}
{p 8 16 2}{cmd:parqit} {cmdab:so:rt} {it:varlist} | {cmd:parqit gsort} [{cmd:+}|{cmd:-}]{it:varname} ...{p_end}
{p 8 16 2}{cmd:parqit collapse} {cmd:(}{it:stat}{cmd:)} [{it:tgt}{cmd:=}]{it:src} ... [{cmd:if} {it:exp}] [{cmd:,} {opt by(varlist)}]{p_end}
{p 8 16 2}{cmd:parqit contract} {it:varlist} [{cmd:if} {it:exp}] [{cmd:,} {opt f:req(newvar)}]{p_end}
{p 8 16 2}{cmd:parqit duplicates drop} [{it:varlist}] [{cmd:if} {it:exp}] [{cmd:,} {opt force}]{p_end}
{p 8 16 2}{cmd:parqit sample} {it:#} [{cmd:if} {it:exp}] [{cmd:,} {opt c:ount} {opt seed(#)} {opt by(varlist)}
{opt cl:uster(varname)} {opt any} {opt all} {opt gen:erate(newvar)} {opt keep(newvar)}]{p_end}
{p 8 16 2}{cmd:parqit reshape} {cmd:long}|{cmd:wide} {it:stubs}{cmd:,} {opt i(varlist)} {opt j(name)}{p_end}
{p 8 16 2}{cmd:parqit pivot} {cmd:(}{it:stat}{cmd:)} [{it:tgt}{cmd:=}]{it:src} ... [{cmd:if} {it:exp}]{cmd:,} {opt r:ows(varlist)} {opt c:ols(varname)}{p_end}
{p 8 16 2}{cmd:parqit xtset} {it:panelvar} {it:timevar} [{cmd:,} {opt d:elta(#)}]
| {cmd:parqit tsset} [{it:panelvar}] {it:timevar} [{cmd:,} {opt d:elta(#)}]{p_end}
{p 8 16 2}{cmd:parqit xtset} | {cmd:parqit xtset, clear}{space 4}(show, or remove, the declaration; {cmd:tsset} likewise){p_end}
{p 8 16 2}{cmd:parqit} {cmdab:mer:ge} {cmd:1:1}|{cmd:m:1}|{cmd:1:m} {it:keys} {cmd:using} {it:source}
[{cmd:,} {opt keep(spec)} {opt keepus:ing(varlist)} {opt gen:erate(newvar)}
{opt nogen:erate} {opt nonote:s} {opt enc:oding(name)}]{p_end}
{p 8 16 2}{cmd:parqit} {cmdab:ap:pend} {cmd:using} {it:source} [{it:source} ...] [{cmd:,} {opt gen:erate(newvar)} {opt keep(varlist)}
{opt nonote:s} {opt enc:oding(name)}]{p_end}
{p 8 16 2}{cmd:parqit joinby} {it:keys} {cmd:using} {it:source} [{cmd:,} {opt enc:oding(name)}]{p_end}
{p 8 16 2}{cmd:parqit query} {cmd:"}{it:SQL fragment}{cmd:"}{p_end}

{pstd}
Look at the view (engine-side; the dataset in memory is unchanged)

{p 8 16 2}{cmd:parqit} {cmdab:d:escribe} [{it:parquet_source}] [{cmd:,} {opt lab:els} {opt not:es}]{p_end}
{p 8 16 2}{cmd:parqit} {cmdab:l:ist} [{it:varlist}] [{cmd:if} {it:exp}] [{cmd:in} {it:f}[{cmd:/}{it:l}]]{p_end}
{p 8 16 2}{cmd:parqit ds} | {cmd:parqit lookfor} {it:word} [{it:word} ...]{p_end}
{p 8 16 2}{cmd:parqit codebook} [{it:varlist}] [{cmd:if} {it:exp}]{p_end}
{p 8 16 2}{cmd:parqit} {cmdab:su:mmarize} [{it:varlist}] [{cmd:if} {it:exp}] [{cmd:,} {opt d:etail}]{p_end}
{p 8 16 2}{cmd:parqit} {cmdab:ta:bulate} {it:varname} [{it:varname2}] [{cmd:if} {it:exp}] [{cmd:,} {opt m:issing} {opt r:ow} {opt co:lumn}
{opt nol:abel}]{space 2}({opt row}/{opt column} apply to the two-way form; the one-way form
ignores them; {opt nolabel} shows codes instead of value labels){p_end}
{p 8 16 2}{cmd:parqit misstable} [{cmdab:sum:marize}|{cmdab:pat:terns}] [{it:varlist}] [{cmd:if} {it:exp}]{p_end}
{p 8 16 2}{cmd:parqit levelsof} {it:varname} [{cmd:if} {it:exp}] [{cmd:,} {opt l:imit(#)}]{p_end}
{p 8 16 2}{cmd:parqit} {cmdab:cou:nt} [{cmd:if} {it:exp}]{p_end}
{p 8 16 2}{cmd:parqit unique} [{it:varlist}] [{cmd:if} {it:exp}] [{cmd:,} {opt by(varlist)} {opt nomiss:ing}]{p_end}
{p 8 16 2}{cmd:parqit duplicates} {cmdab:r:eport}|{cmdab:l:ist} [{it:varlist}] [{cmd:if} {it:exp}] [{cmd:,} {opt l:imit(#)}]{p_end}
{p 8 16 2}{cmd:parqit tabstat} {it:varlist} [{cmd:if} {it:exp}] [{cmd:,} {opt s:tatistics(stats)} {opt by(varname)} {opt save}]
{space 2}({opt stats()} is a synonym for {opt statistics()}, as in Stata){p_end}
{p 8 16 2}{cmd:parqit} {cmdab:cor:relate} [{it:varlist}] [{cmd:if} {it:exp}]{space 6}(listwise; takes no options){p_end}
{p 8 16 2}{cmd:parqit pwcorr} [{it:varlist}] [{cmd:if} {it:exp}] [{cmd:,} {opt o:bs} {opt sig}]{p_end}
{p 8 16 2}{cmd:parqit histogram} {it:varname} [{cmd:if} {it:exp}] [{cmd:,} {opt b:ins(#)} {opt nodraw}]
{space 2}({cmd:hist} is a synonym for {cmd:histogram}){p_end}

{pstd}The {cmd:if} {it:exp} of these commands restricts the rows the statistic reads,
as in native Stata, and leaves the view unchanged; {it:exp} is a parqit
expression over the view ({help parqit##expressions:Expressions}). In
{cmd:duplicates list}, {bf:Obs} still numbers the rows of the whole view, as
native does.{p_end}

{pstd}
Get the result

{p 8 16 2}{cmd:parqit collect} [{cmd:,} {opt clear} {opt int64(refuse|round|string)} {opt comp:ress}]{space 2}stream the result into memory (atomically){p_end}
{p 8 16 2}{cmd:parqit} {cmdab:sa:ve} {it:filename} [{cmd:,} {opt replace} {opt d:ata} {opt view}
{opt comp:ression(codec)} {opt compression_level(#)} {opt part:ition_by(varlist)}
{opt partitions(replace|append)} {opt c:hunk(#)} {opt enc:oding(name)} {opt copy:source}
{opt xmiss:ing}]{p_end}
{p 8 16 2}{cmd:parqit} {cmdab:sa:ve} {it:filename} {cmd:using} {it:spssfile}|{it:Rfile} [{cmd:,} {opt replace}
{opt comp:ression(codec)} {opt compression_level(#)} {opt enc:oding(name)} {opt obj:ect(name)}]{space 2}convert an
SPSS {cmd:.sav}/{cmd:.zsav} file or an R {cmd:.rds}/{cmd:.rda}/{cmd:.RData} file to Parquet
({help parqit##spss:SPSS files}, {help parqit##rdata:R data files}){p_end}

{pstd}
Work with the dataset in memory

{p 8 16 2}{cmd:parqit mergein} {cmd:1:1}|{cmd:m:1}|{cmd:1:m}|{cmd:m:m} {it:keys} {cmd:using} {it:file}
[{cmd:,} {it:merge_options} {opt int64(refuse|round|string)} {opt enc:oding(name)}]{p_end}
{p 8 16 2}{cmd:parqit appendin using} {it:file}
[{cmd:,} {opt keep(varlist)} {opt gen:erate(newvar)} {opt nol:abel} {opt nonote:s} {opt force}
{opt int64(refuse|round|string)} {opt enc:oding(name)}]{space 3}({opt keep()} names variables
{it:of the file}, as in native {helpb append}){p_end}
{p 8 16 2}{cmd:parqit spssencode} {it:strvar}{cmd:,} {opt g:enerate(newvar)} [{opt l:abel(name)}
{opt seq:uential}]{space 2}labelled numeric version of a string variable read from an
SPSS file, or from an R file with haven labels ({help parqit##spss:SPSS files}){p_end}

{pstd}
Manage views and the engine

{p 8 16 2}{cmd:parqit view} [{it:viewname}[{cmd::} {it:parqit_command}]] | {cmd:parqit views}{p_end}
{p 8 16 2}{cmd:parqit show} | {cmd:parqit explain}{p_end}
{p 8 16 2}{cmd:parqit close} [{it:viewname}|{cmd:_all}]{p_end}
{p 8 16 2}{cmd:parqit path} {it:filename}{p_end}
{p 8 16 2}{cmd:parqit} {cmdab:se:t} {it:setting} {it:value}{space 2}where {it:setting} is
{cmd:statamissing}, {cmd:int64}, {cmd:encoding}, {cmd:fill_threads},
{cmd:stream_buffer_mb}, {cmd:threads}, {cmd:memory_limit} or {cmd:tempdir}{p_end}
{p 8 16 2}{cmd:parqit version}{space 4}(plugin + engine versions){p_end}
{p 8 16 2}{cmd:parqit selftest}{space 3}(end-to-end engine and codec check, useful on new installs/HPC nodes){p_end}
{p 8 16 2}{cmd:parqit menu}{space 8}(add parqit to the {bf:User} menu — GUI Stata only){p_end}

{pstd}
where {it:filename} is a Parquet file, a glob such as {it:data_*.parquet}, a
Hive-partitioned directory, or a delimited-text, Stata, Excel, SPSS or R file;
and each two-table {it:source} is such a file or {cmd:view:}{it:viewname},
another open view.

{pstd}
Subcommands and options take the abbreviations native Stata accepts for the same
commands; the underlined part is the shortest form: {cmd:parqit su} is
{cmd:parqit summarize}, {cmd:parqit ta} is {cmd:parqit tabulate}, {cmd:parqit g}
is {cmd:parqit gen}, and {cmd:hist} is {cmd:histogram}. The commands Stata spells
out, and parqit's own verbs, are typed in full; see
{help parqit_technical##conventions:Syntax conventions}.

{pstd}
Existing variable names accept unique, case-sensitive prefixes in varlists,
expressions and variable-valued options: {cmd:parqit su wag} and
{cmd:parqit gen lwage = log(wag)} may use {cmd:wag} for {cmd:wage}. Exact names
take priority; ambiguous prefixes are refused. Names are resolved against the
view or input file, independently of the data in memory. {cmd:set varabbrev off}
requires full names; wildcards still work. New names, reshape-long stubs and
raw SQL are literal.

{marker use_options}{...}
{synoptset 28 tabbed}{...}
{synopthdr:use_options}
{synoptline}
{synopt:{opt clear}}read the result into memory instead of opening a view{p_end}
{synopt:{opt n:ame(viewname)}}name the view; default {cmd:default}{p_end}
{synopt:{opt relax:ed}}combine files whose columns differ, by name{p_end}
{synopt:{opt enc:oding(name[, all])}}code page of text that is not UTF-8{p_end}
{synopt:{opt int64(refuse|round|string)}}integers beyond 2^53{p_end}
{synopt:{opt binary(text|hex)}}load Parquet {cmd:BINARY} columns{p_end}
{synopt:{opt file:name(newvar)}}the file each row came from{p_end}
{synopt:{opt csv(read_options)}}dialect and column types of delimited text{p_end}
{synopt:{opt obj:ect(name)}}the data frame to read from an R file{p_end}
{synoptline}

{synoptset 28 tabbed}{...}
{synopthdr:save_options}
{synoptline}
{synopt:{opt replace}}overwrite an existing file or tree{p_end}
{synopt:{opt d:ata}}save the dataset in memory, even with a view open{p_end}
{synopt:{opt comp:ression(codec)}}{cmd:zstd} (default), {cmd:snappy}, {cmd:gzip}, ...{p_end}
{synopt:{opt compression_level(#)}}the codec's level{p_end}
{synopt:{opt part:ition_by(varlist)}}write a Hive directory tree{p_end}
{synopt:{opt partitions(replace|append)}}update an existing tree partition by partition{p_end}
{synopt:{opt c:hunk(#)}}rows per Parquet row group{p_end}
{synopt:{opt enc:oding(name)}}code page of legacy text in the data saved{p_end}
{synopt:{opt copy:source}}with {opt data}, copy the file the data were read from{p_end}
{synopt:{opt xmiss:ing}}keep extended missing values {cmd:.a}-{cmd:.z}{p_end}
{synopt:{opt obj:ect(name)}}the R data frame to convert{p_end}
{synoptline}


{marker menu}{...}
{title:Menu}

{pstd}
In GUI Stata, {cmd:parqit menu} adds {bf:User > parqit} for the session; put it
in your {cmd:profile.do} to keep it. Each dialog builds and runs an ordinary
{cmd:parqit} command, echoed to the Results window, so every click can be
repeated in a do-file:

{p2colset 9 56 58 2}{...}
{p2col:{bf:Read data (lazy view or into memory)}}{cmd:db parqit_read}{p_end}
{p2col:{bf:Describe and explore data}}{cmd:db parqit_explore}{p_end}
{p2col:{bf:Summary statistics, tables, and correlations}}{cmd:db parqit_stats}{p_end}
{p2col:{bf:Keep or drop observations, or draw a sample}}{cmd:db parqit_filter}{p_end}
{p2col:{bf:Keep, drop, order, sort, or rename variables}}{cmd:db parqit_vars}{p_end}
{p2col:{bf:Create or change variables}}{cmd:db parqit_gen}{p_end}
{p2col:{bf:Collapse, contract, pivot table, or reshape}}{cmd:db parqit_pivot}{p_end}
{p2col:{bf:Combine datasets (merge, append, joinby)}}{cmd:db parqit_combine}{p_end}
{p2col:{bf:Save as Parquet or collect into memory}}{cmd:db parqit_write}{p_end}
{p2col:{bf:Views, SQL, and engine settings}}{cmd:db parqit_views}{p_end}
{p2colreset}{...}

{pstd}
The same menu runs {cmd:parqit version}, {cmd:parqit selftest} and opens both
help files. The dialogs are also listed in this Viewer's {bf:Dialog} menu; see
{help parqit_technical##dialogs:Menus and dialogs} for what each one offers.


{marker description}{...}
{title:Description}

{pstd}
{cmd:parqit} reads, writes, joins and reshapes Parquet data with ordinary Stata
verbs that run out of core on an embedded DuckDB engine. It is dbplyr's
architecture with Stata's vocabulary: the verbs build a plan, the plan compiles
to one SQL query, and the engine reads the files directly and can spill to disk.
The result enters Stata only when you collect it, or goes straight back to
Parquet.

{pstd}
{bf:Views.} A view is "the current dataset" kept on disk, possibly far larger
than memory. Opening one probes the file's schema; no result rows are loaded.
Each verb checks its names and contract when you type it, and the whole
exploration family ({cmd:describe}, {cmd:summarize}, {cmd:tabulate}, ...)
answers with separate engine-side queries, so a file can be profiled without
replacing the dataset in memory: explore first, load last. Several named views
can be open at once, like frames.

{pstd}
{bf:Metadata.} Variable and value labels, notes, formats, characteristics and
original column names are stored in standard Parquet key-value metadata and
restored on read; the files stay plain Parquet for pandas, polars, R and Spark.

{pstd}
{bf:Formats and languages.} Delimited text, Stata and Excel files open the same
way. SPSS and R files are read by parqit's own readers, without SPSS or R, with
their labels and missing-value codes. Text in any language and code page is
decoded to UTF-8, with a note saying what was decoded.

{pstd}
{bf:Statistics.} The summaries follow the definitions of the native commands and
are computed with exact arithmetic before one final rounding, so for the same
data they do not depend on row order or the number of threads.

{pstd}
StataNow (Stata 19.5) ships a native {cmd:import parquet} that reads a Parquet
file into memory ({bf:File > Import > Parquet data}). {cmd:parqit} is
complementary to it, not a replacement: its contribution is the lazy verb
grammar that filters, derives, aggregates and joins data far larger than
memory before anything is loaded, the Parquet writer ({cmd:parqit save}) and
the Stata-metadata round-trip. When reading a whole file into memory is all
that is needed, either command serves. {cmd:parqit}'s menu and dialogs live
under {bf:User > parqit} and never alter Stata's own menus.


{marker quickstart}{...}
{title:Quick start}

{pstd}Explore first, load last. Open a file as a lazy view, look at it with
engine-side commands, build the pipeline with ordinary verbs, and materialise
only the result:

{phang2}{cmd:. parqit describe /data/big.parquet}{space 8}({it:the footer: rows, types, labels}){p_end}
{phang2}{cmd:. parqit use using /data/big.parquet}{space 6}({it:a lazy view}){p_end}
{phang2}{cmd:. parqit summarize wage age}{space 15}({it:computed by the engine}){p_end}
{phang2}{cmd:. parqit keep if year >= 2019 & !missing(wage)}{p_end}
{phang2}{cmd:. parqit gen double lwage = ln(wage)}{p_end}
{phang2}{cmd:. parqit collapse (mean) lwage (count) n=lwage, by(firm year)}{p_end}
{phang2}{cmd:. parqit show}{space 28}({it:the SQL the plan compiles to}){p_end}
{phang2}{cmd:. parqit collect, clear}{space 18}({it:only the result enters memory}){p_end}

{pstd}When the result should stay on disk, replace the last line with
{cmd:parqit save result.parquet, replace}: the pipeline runs and writes Parquet
without loading the result into the current dataset. {cmd:parqit close}
discards the view. Files written by parqit keep variable and value labels,
notes, formats and characteristics, and stay plain Parquet for other tools.
Two runnable courses ship with the package, {cmd:parqit_basics.do} and
{cmd:parqit_tour.do} (see {help parqit##examples:Examples}).

{pstd}To load only a sample, draw it in the view: {cmd:parqit sample 10, cluster(hh) by(region)}
keeps a tenth of the households of each region, with all their members
({help parqit##sampledesign:Sampling designs}). To try verbs without changing a
view, copy its plan first: {cmd:parqit use view:default, name(try)}
({help parqit##copy:Copying a view}).

{pstd}Other files open the same way, by their extension: delimited text,
{cmd:.dta}, Excel, SPSS ({cmd:parqit use using survey.sav}) and R
({cmd:parqit use using people.rds}). Text in another code page is decoded with
{opt encoding()}: {cmd:parqit use using prices.csv, encoding(windows-1251)}
({help parqit##encoding:Text encodings}).

{pstd}Three rules of thumb: put {cmd:keep}/{cmd:keep if} early so the engine
reads less; never expect a lazy verb to change the data in memory (only
{cmd:collect} does, and only with a complete result); and read
{help parqit##expressions:Expressions} once for the missing-value rules:
the default is {cmd:statamissing on}, so {cmd:keep if x > 5} keeps a numeric
missing {cmd:x}, as native Stata does. Use {cmd:x > 5 & x < .} to exclude it,
or explicitly select SQL comparisons with {cmd:parqit set statamissing off}.


{marker options}{...}
{title:Options}

{dlgtab:use}

{phang}
{opt clear} reads the whole result into memory, atomically, instead of opening a
view. As with {helpb use}, changed data in memory need {opt clear}; open views
are left as they were. An {cmd:if} {it:exp} before {cmd:using} reads the rows it
selects, as native {cmd:use}: the condition may use any variable of the file,
also one outside {it:varlist}; with {opt clear} it runs in the engine while the
file is read, and on a view it is the view's first verb.

{phang}
{opt compress}, with {opt clear}, runs native {helpb compress} on the data
once loaded and before they replace the dataset in memory: a storage type is
narrowed only when every value fits it exactly, so an integral total becomes
{cmd:long}, {cmd:int} or {cmd:byte} and a double mean stays {cmd:double}.

{phang}
{opt name(viewname)} opens the view under {it:viewname}, replacing an open view
of that name; the default name is {cmd:default}.

{phang}
{opt relaxed} reads a glob whose files have different columns, combining them by
name; a column absent from a file is missing there. Without it, files that
differ are refused.

{phang}
{opt encoding(name[, all])} names the code page of text that is not UTF-8: of
delimited text, of a {cmd:.dta} or Excel file, or instead of the one an SPSS or R
file declares. {cmd:all} also decodes text that happens to be valid UTF-8. See
{help parqit##encoding:Text encodings}.

{phang}
{opt int64(refuse|round|string)} says what to do with integers beyond 2^53, which
a {cmd:double} cannot hold exactly: {cmd:refuse} (the default) stops and names
the columns, {cmd:round} accepts the nearest double, and {cmd:string} loads them
as exact text. {cmd:parqit set int64} changes the default for the session.

{phang}
{opt binary(text|hex)} loads Parquet {cmd:BINARY} columns, which are otherwise
dropped with a message, as UTF-8 text or as hexadecimal digits.

{phang}
{opt filename(newvar)} adds a variable holding the file each row was read from.

{phang}
{opt csv(read_options)} states the dialect and the column types of delimited
text instead of letting the reader guess them: {opt delim()}, {opt quote()},
{opt escape()}, {opt header(on|off)}, {opt dateformat()},
{opt timestampformat()}, {opt sample()}, {opt allvarchar}, {opt types()} and
{opt nullstr()}. Use {opt types()} or {opt allvarchar} when a guessed type would
change a value, such as an identifier with leading zeros. See
{help parqit_technical##reading:Reading data}.

{phang}
{opt object(name)} names the data frame to read from an R file that holds
several.

{dlgtab:save}

{phang}
{opt replace} permits {cmd:parqit save} to overwrite an existing file or tree.

{phang}
{opt data} saves the dataset in memory even when a view is open; without a view,
{cmd:parqit save} saves memory anyway. {opt view} says explicitly that the
current view is what is written, and is refused when no view is open.

{phang}
{opt compression(codec)} chooses {cmd:zstd} (the default), {cmd:snappy},
{cmd:gzip}, {cmd:lz4}, {cmd:lz4_raw}, {cmd:brotli} or {cmd:uncompressed};
{opt compression_level(#)} sets the codec's level.

{phang}
{opt partition_by(varlist)} writes a Hive directory tree with one directory per
value of {it:varlist}, which later reads can skip.

{phang}
{opt partitions(replace|append)} updates an existing tree partition by partition
instead of rewriting it: {cmd:replace} swaps the partitions present in the
result and keeps the others, {cmd:append} adds files to them. The tree's columns
and parqit metadata must match the result, and {opt partition_by()} must name
the tree's keys: left out, the refusal names them.

{phang}
{opt chunk(#)} sets the number of rows per Parquet row group.

{phang}
{opt encoding(name)} names the code page of text that is not UTF-8 in the data
saved; it is decoded to UTF-8.

{phang}
{opt copysource}, with {opt data}, copies the Parquet file that the last
{cmd:parqit use} ...{cmd:, clear} loaded instead of writing the data in memory:
you assert that nothing has changed, and parqit checks the file, the variables
and the first and last rows before copying. See
{help parqit_technical##materialisers:Materialisers}.

{phang}
{opt xmissing} keeps extended missing values {cmd:.a}-{cmd:.z}, which Parquet
cannot represent, in companion columns that {cmd:parqit use} restores.

{phang}
{opt object(name)} names the R data frame to convert with
{cmd:parqit save} ... {cmd:using}.

{dlgtab:collect}

{phang}
{opt clear} permits {cmd:parqit collect} to replace changed data in memory.
{opt int64()} and {opt compress} are as for {cmd:parqit use}: {opt compress}
narrows the storage types losslessly after the result is loaded, which is
where the {cmd:double} results of {cmd:collapse} and {cmd:egen} become
{cmd:long}, {cmd:int} or {cmd:byte} when their values allow it.

{dlgtab:collapse, egen, by, xtset}

{phang}
{cmd:collapse} and {cmd:pivot} take every native statistic: {cmd:mean},
{cmd:sum}, {cmd:rawsum}, {cmd:sd}, {cmd:semean}, {cmd:sebinomial},
{cmd:sepoisson}, {cmd:median}, {cmd:p}{it:##}, {cmd:iqr}, {cmd:count},
{cmd:percent}, {cmd:min}, {cmd:max}, {cmd:first}, {cmd:last}, {cmd:firstnm} and
{cmd:lastnm}, without weights.

{phang}
{cmd:egen} takes the native functions and their options. With an expression:
{cmd:total} ({opt missing}), {cmd:mean}, {cmd:sd}, {cmd:min}, {cmd:max}
({opt missing}), {cmd:count}, {cmd:median}, {cmd:pctile} ({opt p(#)}),
{cmd:iqr}, {cmd:kurt}, {cmd:skew}, {cmd:mad}, {cmd:mdev}, {cmd:std}
({opt mean(#)} {opt sd(#)}), {cmd:pc} ({opt prop}), {cmd:rank} ({opt field}
{opt track} {opt unique}). With a varlist: {cmd:group} ({opt missing}
{opt label} {opt lname(name)} {opt truncate(#)}), {cmd:tag} ({opt missing}),
{cmd:anycount}, {cmd:anymatch}, {cmd:anyvalue} ({opt values(numlist)}),
{cmd:concat} ({opt punct(chars)} {opt format(%fmt)} {opt decode}
{opt maxlength(#)}), {cmd:diff}, {cmd:rowtotal} ({opt missing}), {cmd:rowmean},
{cmd:rowmin}, {cmd:rowmax}, {cmd:rowmiss}, {cmd:rownonmiss} ({opt strok}),
{cmd:rowsd}, {cmd:rowfirst}, {cmd:rowlast}, {cmd:rowmedian}, {cmd:rowpctile}
({opt p(#)}). With one variable: {cmd:mode} ({opt minmode} {opt maxmode}
{opt nummode(#)} {opt missing}), {cmd:cut} ({opt at(numlist)} or
{opt group(#)}, {opt icodes}, {opt label}), {cmd:ends} ({opt punct(chars)}
{opt head} {opt last} {opt tail} {opt trim}). With no argument: {cmd:seq}
({opt from(#)} {opt to(#)} {opt block(#)}). {opt by()} is allowed where native
allows it. {cmd:fill()} and {cmd:mtr()} are not available.

{phang}
{cmd:by} and {cmd:bysort} prefix {cmd:gen}, {cmd:replace}, {cmd:egen},
{cmd:keep if} and {cmd:drop if}: {cmd:_n}, {cmd:_N} and the subscripts count
within each group of {it:varlist}, ordered by {it:sortvars}. {cmd:bysort} sorts
the view by {it:varlist} and {it:sortvars} first and leaves it sorted; {cmd:by}
requires that order already ("not sorted" otherwise). {cmd:bys} is {cmd:bysort}.

{phang}
{cmd:format} changes the view's display formats without reading observations
or changing values. For example, {cmd:parqit format event_date %tdCCYY-NN-DD}
displays a numeric day count as a date. Numeric, date/time and string formats
are validated by native Stata on an empty frame, then applied together; an
invalid format leaves the view unchanged. {cmd:collect} and {cmd:save} preserve
them. With only a varlist, formats are displayed; with no arguments, only long
formats are displayed, as in Stata. Changing a format does not convert units:
use a date function to turn days into milliseconds before applying {cmd:%tc}.

{phang}
{cmd:xtset} and {cmd:tsset} declare the panel and time variables of the view,
as their native namesakes do for the data in memory: the time variable is
integer-valued and unique within each panel, no two consecutive times closer
than {opt delta(#)}, a whole number of periods; a missing time is accepted, and
the operators are missing on that row. The operators {cmd:L.}, {cmd:F.},
{cmd:D.} and {cmd:S.} and the functions {cmd:tin()} and {cmd:twithin()} then
work in expressions, by time. Unlike native, the declaration does not sort
the view: run {cmd:parqit sort} {it:panelvar timevar} before {cmd:_n},
subscripts or {cmd:keep in} (parqit's note names it); the operators follow
time whatever the sort. The operators also accept the time and panel variables,
as native Stata does; a gap gives missing, not the previous available row.

{phang}
{cmd:unique} reports each variable and their combination, without {cmd:joint}.
An omitted varlist selects all variables. Missing values count as values;
{opt nomiss:ing} excludes them separately for each variable and excludes any
incomplete row from the joint count. {opt by(varlist)} prints the counts per
group; {cmd:if} restricts the observations used by all counts.

{dlgtab:merge, append, joinby}

{phang}
{opt keep()}, {opt keepusing()}, {opt generate()} and {opt nogenerate} are as in
{helpb merge}; lazy {cmd:merge} takes only these and {opt nonotes}. {cmd:append}
takes {opt keep(varlist)}, the variables taken from the using sources, and
{opt generate(newvar)}, which marks the master 0 and each source 1, 2, ..., as
{helpb append} does. {opt encoding()} decodes a using file's legacy text.

{phang}
As the native commands do, {cmd:merge}, {cmd:append} and {cmd:joinby} take from
the using data the value labels, notes and characteristics of the variables they
keep from it and of the dataset; where the master already has one, the master's
is kept, and a note the master already has is not repeated. {opt nonotes}, with
{cmd:merge} and {cmd:append}, leaves the using data's notes out.

{dlgtab:mergein, appendin}

{phang}
{cmd:mergein} passes the options of native {helpb merge} ({opt keepusing()},
{opt keep()}, {opt generate()}, {opt nogenerate}, {opt update}, {opt replace},
{opt assert()}, {opt force}, {opt nolabel}, {opt nonotes}, {opt noreport});
{cmd:appendin} passes {opt keep()}, {opt generate()}, {opt nolabel},
{opt nonotes} and {opt force} to {helpb append}. {opt int64()} and
{opt encoding()} apply to the file on disk.

{dlgtab:sample}

{phang}
{it:#} is a percentage, or with {opt count} a number of rows. {opt seed(#)} makes
the draw reproducible. {opt by(varlist)} draws within strata,
{opt cluster(varname)} draws whole clusters, and {opt any} or {opt all} decide
whether a cluster that an {cmd:if} frame splits is included.
{opt generate(newvar)}, or its synonym {opt keep()}, flags the drawn rows
instead of dropping the others. See {help parqit##sampledesign:Sampling designs}.

{dlgtab:Statistics}

{phang}
The statistics commands take the options of their native namesakes that parqit
implements: {cmd:summarize}, {opt detail}; {cmd:tabulate}, {opt missing},
{opt row}, {opt column}, {opt nolabel}; {cmd:tabstat}, {opt statistics()} (or
{opt stats()}), {opt by()}, {opt save}; {cmd:pwcorr}, {opt obs}, {opt sig};
{cmd:histogram}, {opt bins(#)}, {opt nodraw}; {cmd:unique}, {opt by()},
{opt nomissing}; {cmd:describe} of a file, {opt labels}, {opt notes}.
{cmd:levelsof} and {cmd:duplicates list} take {opt limit(#)}, a bound on the
values or rows shown.


{marker lazy}{...}
{title:Views}

{pstd}
{cmd:parqit use using} {it:file} opens a view: the file's schema and a plan of
verbs. Views are named ({cmd:default} unless {opt name()} says otherwise) and
several can be open at once, like frames. Verbs act on the current view;
{cmd:parqit view} {it:name} switches, {cmd:parqit view} {it:name}{cmd::}
{it:command} runs one command against another view and switches back,
{cmd:parqit views} lists them, and {cmd:parqit close} closes one or all.

{marker copy}{...}
{pstd}
{bf:Copying a view.} {cmd:parqit use} [{it:varlist}] {cmd:using view:}{it:name}{cmd:,}
{opt name(newview)} copies a view's plan, so that verbs can be tried on the copy
while the original stays as it was. The copy is a plan, not data: both views
read the same files.

{pstd}
Two commands are deliberately {it:not} view verbs: {cmd:parqit mergein} and
{cmd:parqit appendin} join the dataset {it:already in Stata's memory} with a disk
file through a {it:native} {helpb merge} / {helpb append}, reading only the
columns of the file they need. Use them when the disk side is a small lookup;
use {cmd:parqit use} + {cmd:parqit merge} when both sides are big.
Think of {cmd:mergein} as merge {it:into} the data in memory.

{pstd}
{bf:Coming from pq.} These basic commands correspond as follows; options
specific to {cmd:pq} are not implied by the correspondence.

{p2colset 8 59 61 2}{...}
{p2col:{bf:pq}}{bf:parqit}{p_end}
{p2col:{cmd:pq use [varlist] using f.parquet, clear}}{cmd:parqit use [varlist] using f.parquet, clear}{p_end}
{p2col:{cmd:pq merge m:1 keys using f.parquet}}{cmd:parqit mergein m:1 keys using f.parquet}{p_end}
{p2col:{cmd:pq append using f.parquet}}{cmd:parqit appendin using f.parquet}{p_end}
{p2col:{cmd:pq save using f.parquet, replace}}{cmd:parqit save f.parquet, replace}{p_end}
{p2colreset}{...}

{pstd}
The last line saves memory when no view is open. When a view is open, add
{opt data}: {cmd:parqit save f.parquet, replace data} saves memory explicitly.
{cmd:mergein} and {cmd:appendin} each read one file at a time;
{cmd:merge}, {cmd:append} and {cmd:joinby} continue to act on the current view.

{pstd}
Without a current view, these lazy verbs refuse with {cmd:r(198)} before
importing a disk file. If data are already in memory, {cmd:merge} and
{cmd:append} show a copyable {cmd:mergein} or {cmd:appendin} command with the
options you typed. For multiple append files, the suggestion starts with the
first file. The message also explains how to open a view for an out-of-core
join; it never changes routes automatically. A {cmd:view:} source keeps the
existing view diagnostic.

{pstd}
More: {help parqit_technical##views:Views} in the technical reference.


{marker verbs}{...}
{title:Lazy verbs}

{pstd}
The verbs take the syntax of their native namesakes. Each one is checked when you
type it: names, types and the verb's contract, such as the unique keys a
{cmd:merge m:1} requires, are validated before the plan changes, and a refused
verb leaves the view as it was. No verb changes the dataset in memory. Varlists
accept the wildcards {cmd:*} and {cmd:?}.

{pstd}
Where parqit differs from native Stata:

{phang2}{cmd:•} {cmd:collapse} and {cmd:pivot} take every native statistic,
without weights; {cmd:egen} takes every native function except {cmd:fill()} and
{cmd:mtr()} (see {help parqit##options:Options}). Means, sums and the like are
kept in {cmd:double}; {cmd:collect, compress} narrows what fits.{p_end}
{phang2}{cmd:•} {cmd:xtset} and {cmd:tsset} declare the panel and time
without sorting the view; sort it with {cmd:parqit sort} when row positions
matter.{p_end}
{phang2}{cmd:•} A {cmd:replace} that reads the replaced variable's other rows,
in its expression or in its {cmd:if}
({cmd:replace x = x[_n-1] if missing(x)}), is refused: native {cmd:replace} is
sequential, each new value feeding the next row, and a lazy plan computes every
row from the values before the command. Generate a new variable instead.{p_end}
{phang2}{cmd:•} {cmd:pivot} is a pivot table in one verb: {cmd:collapse} by rows
and columns, then {cmd:reshape wide}. {cmd:reshape wide} and {cmd:pivot} spread at
most 2,000 values.{p_end}
{phang2}{cmd:•} {cmd:duplicates drop} {it:varlist} needs {opt force} and a
previous {cmd:parqit sort}, and keeps the first row in that order.{p_end}
{phang2}{cmd:•} {cmd:keep in} and {cmd:drop in} are checked against the real
number of rows when the plan runs; a range out of bounds is an error.{p_end}
{phang2}{cmd:•} Lazy {cmd:merge m:m} is refused; use {cmd:joinby}, or
{cmd:mergein m:m} when native's order-based pairing is intended.{p_end}

{pstd}{bf:merge m:m or joinby?} Both combine the rows that share a key value,
but differently. Take a firm with 2 rows in the master (a1, a2) and 3 in the
using file (b1, b2, b3). {cmd:joinby} forms every pair: 2 x 3 = 6 rows (a1-b1,
a1-b2, a1-b3, a2-b1, a2-b2, a2-b3). That is what "many to many" usually means,
and the result does not depend on the order of the rows. {cmd:merge m:m}
instead pairs the rows one by one in their current order (a1-b1, a2-b2) and,
when one side runs out, repeats its last row (a2-b3): 3 rows, and a different
sort order gives different pairs, which is why the Stata manual ({bf:[D] merge})
calls m:m a bad idea. {cmd:joinby} keeps only the keys present on both sides,
as native {cmd:joinby} does by default. Use {cmd:parqit joinby} for all
combinations, and {cmd:parqit mergein m:m} only when native Stata's
order-based pairing is what you need.

{pstd}
The contract of each verb is in
{help parqit_technical##verbs:Verb contracts}.


{marker sampledesign}{...}
{title:Sampling designs}

{pstd}
{cmd:parqit sample} draws an engine-side random sample and follows the designs of
Weesie's {cmd:sample2}: {opt by()} draws within strata, {opt cluster()} draws
whole clusters, an {cmd:if} expression restricts the sampling frame (rows outside
it are kept and never drawn), and {opt generate()} flags the drawn rows instead
of dropping the others. {opt seed()} makes a draw reproducible; the draw never
matches {cmd:sample2}'s, which uses Stata's random numbers. More:
{help parqit_technical##sampledesign:Sampling designs}.


{marker materialisers}{...}
{title:Getting the result}

{pstd}
{cmd:parqit collect} runs the plan and loads the result into Stata. The data in
memory are replaced only once the new data are complete and valid, and the view
stays open: collecting again runs the plan again.

{pstd}
{cmd:parqit save} runs the plan and writes the result to a Parquet file, or with
{opt partition_by()} to a Hive directory tree, without loading it into Stata;
with {opt data} it writes the dataset in memory instead. Files are staged and
checked before they replace anything. {opt partitions(replace)} adds or replaces
a month of a monthly tree, say, without rewriting the rest.

{pstd}
Stata holds at most 2,147,483,647 observations; a larger result can still be
saved to Parquet. The output is always Parquet: to get a {cmd:.dta}, collect the
result and use {helpb save}. More:
{help parqit_technical##materialisers:Materialisers}.


{marker explore}{...}
{title:Exploring a view}

{pstd}
These commands answer with engine-side queries: only their results reach Stata,
and the dataset in memory is never replaced. Their output looks like the native
commands', and their statistics use the native definitions with exact arithmetic.
Their {cmd:if} {it:exp} restricts the rows they read and leaves the view as it
is.

{p2colset 9 34 36 2}{...}
{p2col:{cmd:describe}}the view's variables; with a file, its
Parquet footer: rows, types and labels{p_end}
{p2col:{cmd:list}}the first rows, or a filtered or ranged preview{p_end}
{p2col:{cmd:count}}the number of rows, or of rows satisfying {cmd:if}{p_end}
{p2col:{cmd:ds}, {cmd:lookfor}}variable names, or those matching words{p_end}
{p2col:{cmd:codebook}}per variable: type, missing and distinct values, range{p_end}
{p2col:{cmd:summarize}}summary statistics; {opt detail} adds percentiles and
moments{p_end}
{p2col:{cmd:tabulate}}one- and two-way frequency tables{p_end}
{p2col:{cmd:tabstat}}a table of statistics, optionally by group{p_end}
{p2col:{cmd:correlate}, {cmd:pwcorr}}correlations, listwise or pairwise{p_end}
{p2col:{cmd:histogram}}a histogram whose bins the engine computes{p_end}
{p2col:{cmd:misstable}}missing values by variable, or their patterns{p_end}
{p2col:{cmd:levelsof}}the distinct values of a variable{p_end}
{p2col:{cmd:unique}}the number of unique values of a varlist taken jointly,
missing counting as a value, as the SSC {cmd:unique}; per variable and per
group too{p_end}
{p2col:{cmd:duplicates}}a report or a list of duplicated observations{p_end}
{p2colreset}{...}

{pstd}
Some outputs are bounded on purpose, and say so rather than cut silently: a
one-way table stops beyond 10,000 values, a two-way table beyond 30 columns, and
{cmd:levelsof} beyond {opt limit()} values. More:
{help parqit_technical##explore:Exploration commands}.


{marker formats}{...}
{title:Other file formats and text encodings}

{pstd}
Besides Parquet, {cmd:parqit use} opens delimited text ({cmd:.csv}, {cmd:.tsv},
{cmd:.txt}, {cmd:.tab}), Stata ({cmd:.dta}), Excel ({cmd:.xls}, {cmd:.xlsx}),
SPSS and R files, chosen by the extension. Parquet and delimited text are read
out of core; the others are first converted to a temporary Parquet file. All of
them also work as the {cmd:using} file of {cmd:merge}, {cmd:append} and
{cmd:joinby}. More: {help parqit_technical##formats:Input formats}.

{marker spss}{...}
{pstd}
{bf:SPSS files} ({cmd:.sav}, {cmd:.zsav}) are read by parqit's own reader with
their dictionary: variable and value labels, user-missing values (as
{cmd:.a}-{cmd:.z}), formats, documents and the SPSS properties.
{cmd:parqit save} {it:new}{cmd:.parquet using} {it:old}{cmd:.sav} converts a file
out of core, and {cmd:parqit spssencode} turns a string variable with SPSS value
labels into a labelled numeric one. More:
{help parqit_technical##spss:SPSS files}.

{marker rdata}{...}
{pstd}
{bf:R data files} ({cmd:.rds}, {cmd:.rda}, {cmd:.RData}) are read without R:
factors, dates, times, {cmd:integer64}, haven labels and missing-value codes
become Stata value labels, formats and extended missing values, and
{opt object()} picks a data frame. More:
{help parqit_technical##rdata:R data files}.

{marker encoding}{...}
{pstd}
{bf:Text encodings.} Parquet text is UTF-8, as is Stata's. Text in another code
page, in any language, is decoded on the way in with {opt encoding(name)} or the
session default set by {cmd:parqit set encoding} ({cmd:windows-1252} until set),
and a note says what was decoded; SPSS and R files declare their own. More:
{help parqit_technical##encoding:Text encodings}.


{marker expressions}{...}
{title:Expressions and missing values}

{pstd}
{cmd:keep if}, {cmd:drop if}, {cmd:gen}, {cmd:replace}, {cmd:egen} and the
{cmd:if} of every command take Stata expressions, translated for the engine: the
arithmetic, relational and logical operators with Stata's precedence, {cmd:+} for
string concatenation, and these functions:

{* parqit-lint: expression-function-list begin. Every name in this block must}{...}
{* be implemented by src/engine/exprtrans.cpp, and every implemented function}{...}
{* keep this list synchronized with exprtrans.cpp.}{...}
{p 8 8 2}{cmd:abs exp ln log log10 sqrt floor ceil int trunc round mod min max}
{cmd:float cond inrange inlist missing mi}{p_end}
{p 8 8 2}{cmd:strlen length ustrlen upper strupper ustrupper lower strlower}
{cmd:ustrlower trim strtrim ltrim rtrim substr strpos subinstr string strofreal}
{cmd:real regexm}{p_end}
{p 8 8 2}{cmd:year month day quarter dow doy week halfyear mdy dofm mofd yofd ym yq yh yw}
{cmd:dofq dofh dofy dofw qofd hofd wofd dofc dofC cofd hh mm ss hms dhms mdyhms}
{cmd:hours minutes seconds msofhours msofminutes msofseconds datediff age}{p_end}
{p 8 8 2}{cmd:date daily clock monthly quarterly halfyearly weekly yearly}
(a string and a literal mask){p_end}
{p 8 8 2}{cmd:tin twithin} (after a panel and time declaration) and the
date literals {cmd:td tc tC tm tq th tw ty}{p_end}
{* parqit-lint: expression-function-list end}{...}

{pstd}
The text readers take the string and a mask, written as in Stata:
{cmd:date(s, "DMY")}, {cmd:clock(s, "YMDhms")}, {cmd:date(s, "MD20Y")},
{cmd:date(s, "MDY", 2050)}, {cmd:monthly(s, "YM")}.

{pstd}
Dates are their Stata numbers, and the date literals ({cmd:td(01jan2015)},
{cmd:tm(2015m1)}, {cmd:tc(01jan2015 09:30:00)}, ...) are constants. The text
readers follow Stata's rules: the mask letters take the fields of the string in
order, a two-digit year needs {cmd:20Y}/{cmd:19Y} or a topyear, month names and
am/pm are read, a {cmd:#} in the mask skips a field (as the last letter, the
rest of the string), and a string that does not fit the mask is missing.
{cmd:_n}, {cmd:_N} and Stata's subscripts — {cmd:x[_n-1]}, {cmd:x[_n+1]},
{cmd:x[1]}, {cmd:x[_N]}, {cmd:x[_N-1]}, positional, within the groups of a
{cmd:by} prefix — work in {cmd:keep if}, {cmd:drop if}, {cmd:gen}, {cmd:replace}
and {cmd:egen}. After {cmd:parqit xtset}, {cmd:L.x}, {cmd:L2.x}, {cmd:F.x},
{cmd:D.x}, {cmd:S2.x}, {cmd:LD.x} ... are by time: a gap in the panel gives
missing, as native, where {cmd:x[_n-1]} would give the previous row. An
unsupported function is an error that names it; {cmd:parqit query}
and {cmd:parqit sql} are the escape hatches.

{pstd}
{bf:Missing values.} By default comparisons follow Stata's ordering: numeric
missing is larger than every nonmissing number, so {cmd:keep if x > 5} keeps
a missing {cmd:x}. {cmd:parqit set statamissing off} explicitly selects SQL
comparisons, where that comparison is unknown and the row is excluded.
Idioms such as {cmd:x > 5 & x < .} or {cmd:!missing(x)} work in either mode.
The preservation of extended missing categories is a separate storage contract.
More: {help parqit_technical##expressions:Expression dialect}.

{pstd}{bf:Migrating to 0.5.0.} Earlier releases defaulted to SQL comparisons;
start an older do-file with {cmd:parqit set statamissing off} if it relies on
that rule. The former missing-comparison warning is no longer emitted:
Stata mode is the default, and SQL mode is chosen explicitly.
{cmd:distinct}, {cmd:head} and {cmd:glimpse} are retired and give migration
messages. Use {cmd:unique} (with {opt nomissing} for the old exclusion rule),
{cmd:list} and {cmd:describe}. Plain {cmd:list} is a bounded preview;
{cmd:list in f/l} requires a valid range within the view.


{marker settings}{...}
{title:Settings and diagnostics}

{pstd}
{cmd:parqit set} {it:setting} {it:value} changes a setting for the session:

{p2colset 9 38 40 2}{...}
{p2col:{cmd:statamissing on}|{cmd:off}}Stata's (default) or SQL's missing-value rule{p_end}
{p2col:{cmd:int64} {it:mode}}{cmd:refuse}, {cmd:round} or {cmd:string} for
integers beyond 2^53{p_end}
{p2col:{cmd:encoding} {it:name}}the default code page of legacy text{p_end}
{p2col:{cmd:threads} {it:#}}engine threads; default, the CPUs available{p_end}
{p2col:{cmd:fill_threads} {cmd:auto}|{it:#}}threads that fill Stata's memory{p_end}
{p2col:{cmd:stream_buffer_mb} {cmd:auto}|{it:#}}result buffered ahead of that
fill{p_end}
{p2col:{cmd:memory_limit} {it:size}}the engine's memory budget, e.g. {cmd:8GB}{p_end}
{p2col:{cmd:tempdir} {it:path}}where the engine spills to disk{p_end}
{p2colreset}{...}

{pstd}
{cmd:parqit show} prints the SQL the plan compiles to, and {cmd:parqit explain}
the engine's plan. {cmd:parqit query} appends a raw SQL clause to the plan, and
{cmd:parqit sql} opens a view over any DuckDB query; in SQL, quote a column named
like a reserved word, such as {cmd:"foreign"}. {cmd:parqit version} and
{cmd:parqit selftest} check an installation, and {cmd:parqit path} resolves a
path. Restart Stata after updating parqit: while Stata still holds the plugin of
another release or numerical revision, parqit refuses the mismatch. Version
output also identifies the loaded plugin's sources and pinned patches; the
64-digit source ID is returned in {cmd:r(build_id)}, and the required numerical
revision in {cmd:r(numeric_contract)}. {cmd:selftest} checks deterministic
window and lambda calculations in a separate engine, preserving your views.
More:
{help parqit_technical##settings:Settings, raw SQL and diagnostics}.

{pstd}
For performance, filter and select columns where the intended calculation
allows it, then collect once. Consecutive independent {cmd:egen} aggregates
over the same groups can share a window automatically; expressions that
depend on earlier commands keep their sequential meaning.
{cmd:parqit version} reports the package and DuckDB versions separately;
an engine update alone does not guarantee a faster workflow. Compare repeated
runs with the same result and thread settings, recording server load. See
{help parqit_technical##perf:Performance tips} for the optimisation's scope
and how to distinguish query execution from transfer into Stata.


{marker examples}{...}
{title:Examples}

{pstd}{bf:First contact with an unknown file.} {cmd:describe} reads only Parquet
footer metadata, not column values. The other commands below may scan relevant
data engine-side and stage bounded output, but do not replace the current
dataset:{p_end}
{phang2}{cmd:. parqit describe /data/unknown.parquet}{space 4}({it:rows, columns, types, labels, row groups}){p_end}
{phang2}{cmd:. parqit describe /data/unknown.parquet, labels notes}{space 1}({it:value labels and notes too}){p_end}
{phang2}{cmd:. parqit use using /data/unknown.parquet}{space 2}({it:lazy view; schema probed, no rows loaded}){p_end}
{phang2}{cmd:. parqit list in 1/10}{p_end}
{phang2}{cmd:. parqit codebook}{p_end}
{phang2}{cmd:. parqit misstable}{p_end}
{phang2}{cmd:. parqit summarize wage, detail}{p_end}
{phang2}{cmd:. parqit tabulate region sector, row}{p_end}
{phang2}{cmd:. parqit count if missing(wage, age)}{p_end}
{phang2}{cmd:. parqit list id year wage if wage < 0 | wage > 10000}{p_end}
{phang2}{cmd:. parqit histogram wage, bins(30)}{p_end}
{phang2}{cmd:. parqit close}{p_end}

{pstd}{bf:Compare a filtered population while keeping the full view.}
These are two plans over the same file, not two full copies in Stata:{p_end}
{phang2}{cmd:. parqit use using survey.parquet, name(full)}{p_end}
{phang2}{cmd:. parqit use using survey.parquet, name(adults)}{p_end}
{phang2}{cmd:. parqit keep if age >= 18 & !missing(age)}{p_end}
{phang2}{cmd:. parqit summarize wage, detail}{p_end}
{phang2}{cmd:. parqit view full: summarize wage, detail}{p_end}
{phang2}{cmd:. parqit view full}{p_end}

{pstd}{bf:Whole-file I/O and the metadata round-trip.} Labels, value labels,
notes, formats and storage types are carried through save → use, subject to
the documented conversion and restoration rules; the file stays
plain Parquet for Python/R/Spark (see
{help parqit_technical##metadata:Stata metadata in Parquet}):{p_end}
{phang2}{cmd:. sysuse auto, clear}{p_end}
{phang2}{cmd:. parqit save auto.parquet, replace data}{p_end}
{phang2}{cmd:. parqit use using auto.parquet, clear}{p_end}
{phang2}{cmd:. describe}{space 15}({it:same types, labels and formats as before}){p_end}

{pstd}{bf:Convert an archive once, work out of core forever.} A {cmd:.dta} (or
{cmd:.xlsx}/{cmd:.csv}) source can be a {cmd:parqit use} input directly — so
conversion is two lines, metadata included:{p_end}
{phang2}{cmd:. parqit use using big_archive.dta, clear}{p_end}
{phang2}{cmd:. parqit save big_archive.parquet, replace data compression(zstd)}{p_end}

{pstd}{bf:An SPSS survey to Parquet, with its dictionary.} One command, out of
core; the labels, the user-missing codes and the SPSS properties travel with
the file ({help parqit##spss:SPSS files}):{p_end}
{phang2}{cmd:. parqit save ess_round10.parquet using ess_round10.sav, replace}{p_end}
{phang2}{cmd:. parqit use using ess_round10.parquet, clear}{p_end}
{phang2}{cmd:. tabulate trstprl, missing}{space 10}({it:refusals and don't-knows are .a, .b, … with their labels}){p_end}
{phang2}{cmd:. char list trstprl[]}{space 16}({it:the SPSS definition, codes, format and measurement level}){p_end}

{pstd}{bf:An R data frame without R.} Factors, dates, haven labels and
missing-value codes travel the same way ({help parqit##rdata:R data files}):{p_end}
{phang2}{cmd:. parqit save households.parquet using households.rds, replace}{p_end}
{phang2}{cmd:. parqit use using workspace.RData, clear object(people)}{space 2}({it:one data frame of an .RData}){p_end}

{pstd}{bf:Text in other code pages} — Cyrillic CSV files decoded as they are
read, a Chinese {cmd:.dta} from Stata 13, and a session default
({help parqit##encoding:Text encodings}):{p_end}
{phang2}{cmd:. parqit use using prices_ru_*.csv, encoding(windows-1251)}{space 2}({it:a note counts the decoded lines}){p_end}
{phang2}{cmd:. parqit collect, clear}{p_end}
{phang2}{cmd:. parqit use using survey_zh.dta, clear encoding(gbk)}{p_end}
{phang2}{cmd:. parqit set encoding windows-1251}{space 4}({it:undeclared text, for the rest of the session}){p_end}

{pstd}{bf:Out-of-core panel build} — filter, derive, aggregate on disk; only
the firm-year result enters Stata:{p_end}
{phang2}{cmd:. parqit use using /data/qp_*.parquet}{p_end}
{phang2}{cmd:. parqit keep if year >= 2010 & inrange(age, 25, 64)}{p_end}
{phang2}{cmd:. parqit gen double lwage = ln(wage)}{p_end}
{phang2}{cmd:. parqit collapse (mean) lwage (sd) sd_lw=lwage (p50) med=lwage (count) n=lwage, by(firmid year)}{p_end}
{phang2}{cmd:. parqit show}{space 22}({it:print the SQL the pipeline compiled to}){p_end}
{phang2}{cmd:. parqit collect, clear}{p_end}

{pstd}{bf:Parquet → Parquet without loading the result into Stata} —
{cmd:save} materialises the view straight to disk; add {opt partition_by()}
for a Hive tree that later reads can prune:{p_end}
{phang2}{cmd:. parqit use using /data/qp_*.parquet}{p_end}
{phang2}{cmd:. parqit keep if wage > 0 & !missing(firmid)}{p_end}
{phang2}{cmd:. parqit save firm_panel.parquet, replace partition_by(year)}{p_end}

{pstd}{bf:Disk-to-disk joins.} The {cmd:using} side stays on disk; contracts
({cmd:m:1} unique keys, …) are validated up front and {cmd:_merge} is
Stata-compatible:{p_end}
{phang2}{cmd:. parqit use using firm_panel.parquet}{p_end}
{phang2}{cmd:. parqit merge m:1 firmid year using /data/scie.parquet, keep(match) keepusing(tfp)}{p_end}
{phang2}{cmd:. parqit collect, clear}{p_end}

{pstd}{bf:Pairwise combinations} use {cmd:joinby}, as in native Stata.
Lazy {cmd:merge m:m} is refused because its order-dependent sequential pairing
cannot be reproduced from a lazy plan; native {cmd:mergein m:m} remains
available when that behaviour is intentional:{p_end}
{phang2}{cmd:. parqit use using workers.parquet}{p_end}
{phang2}{cmd:. parqit joinby firmid using patents.parquet}{p_end}
{phang2}{cmd:. parqit collect, clear}{p_end}

{pstd}{bf:Mixed formats in one pipeline} — a CSV scanned out of core and a
{cmd:.dta} lookup bridged in, joined before the result replaces the current
dataset:{p_end}
{phang2}{cmd:. parqit use using transactions_*.csv}{p_end}
{phang2}{cmd:. parqit keep if amount > 0}{p_end}
{phang2}{cmd:. parqit merge m:1 client_id using clients.dta, keepusing(region segment)}{p_end}
{phang2}{cmd:. parqit collapse (sum) amount (count) n=amount, by(region segment)}{p_end}
{phang2}{cmd:. parqit collect, clear}{p_end}

{pstd}{bf:Data already in memory, lookup on disk} — keep your data put and
join natively, reading only the needed columns of the file
({cmd:mergein}/{cmd:appendin}); or promote memory to a view for big-on-big:{p_end}
{phang2}{cmd:. use master, clear}{p_end}
{phang2}{cmd:. parqit mergein m:1 firmid using firms.parquet, keepusing(tfp) nogen}{p_end}
{phang2}{cmd:. parqit appendin using late_arrivals.parquet, keep(firmid wage)}{p_end}
{phang2}{cmd:. parqit open _data}{space 18}({it:big-on-big: promote and join out of core}){p_end}
{phang2}{cmd:. parqit merge m:1 id using big_using.parquet, keepusing(x y)}{p_end}
{phang2}{cmd:. parqit collect, clear}{p_end}

{pstd}{bf:Reshape on disk} — a billion-row long↔wide can be written without
loading the result into Stata's current dataset:{p_end}
{phang2}{cmd:. parqit use using wide_income.parquet}{p_end}
{phang2}{cmd:. parqit reshape long inc, i(pid) j(year)}{p_end}
{phang2}{cmd:. parqit save long_income.parquet, replace}{p_end}

{pstd}{bf:Pivot table (Excel-style)} — mean wage and a count by region × year:{p_end}
{phang2}{cmd:. parqit use using panel.parquet}{p_end}
{phang2}{cmd:. parqit pivot (mean) wage (count) n=wage, rows(region) cols(year)}{p_end}
{phang2}{cmd:. parqit collect, clear}{space 5}({it:columns wage2019 n2019 wage2020 n2020 ...}){p_end}

{pstd}{bf:Dedup, frequency tables, samples}:{p_end}
{phang2}{cmd:. parqit use using events.parquet}{p_end}
{phang2}{cmd:. parqit duplicates report id date}{space 5}({it:copies/surplus table, no materialisation}){p_end}
{phang2}{cmd:. parqit sort id date}{p_end}
{phang2}{cmd:. parqit duplicates drop id date, force}{space 2}({it:first occurrence in the declared order}){p_end}
{phang2}{cmd:. parqit contract region sector, freq(n)}{p_end}
{phang2}{cmd:. parqit collect, clear}{p_end}
{phang2}{cmd:. parqit use using events.parquet}{p_end}
{phang2}{cmd:. parqit sample 1, seed(42)}{space 13}({it:1% engine-side sample; count for # of rows}){p_end}
{phang2}{cmd:. parqit collect, clear}{p_end}

{pstd}{bf:Sampling designs} — whole households within regions; then half of the
households with someone over 60, flagged instead of dropped (see
{help parqit##sampledesign:Sampling designs}):{p_end}
{phang2}{cmd:. parqit use using households.parquet}{p_end}
{phang2}{cmd:. parqit sample 10, cluster(hh) by(region) seed(42)}{space 2}({it:10% of each region's households, all members}){p_end}
{phang2}{cmd:. parqit collect, clear}{p_end}
{phang2}{cmd:. parqit use using households.parquet}{p_end}
{phang2}{cmd:. parqit sample 50 if age > 60, cluster(hh) any generate(pick)}{space 2}({it:pick = 0: not drawn}){p_end}
{phang2}{cmd:. parqit collect, clear}{p_end}

{pstd}{bf:Expressions, types and dates.} Untyped results are double (like
Stata's evaluator); type the {cmd:gen} to control storage. Dates are their
Stata numbers inside the pipeline:{p_end}
{phang2}{cmd:. parqit use using workers.parquet}{p_end}
{phang2}{cmd:. parqit gen byte prime = inrange(age, 25, 54)}{p_end}
{phang2}{cmd:. parqit gen hire_year = year(hire_date)}{p_end}
{phang2}{cmd:. parqit gen str1 ini = substr(name, 1, 1)}{p_end}
{phang2}{cmd:. parqit replace wage = . if wage <= 0}{p_end}
{phang2}{cmd:. parqit egen double fw = mean(wage), by(firmid)}{p_end}
{phang2}{cmd:. parqit keep if hire_date >= td(01jan2015)}{p_end}
{phang2}{cmd:. parqit collect, clear}{p_end}

{pstd}{bf:Missing-value semantics, explicitly.} Stata mode (the default) keeps
numeric missings on {cmd:>} filters; SQL mode drops them:{p_end}
{phang2}{cmd:. parqit use using workers.parquet}{p_end}
{phang2}{cmd:. parqit count if wage > 5000}{space 12}({it:Stata mode: missing wage counted, as native}){p_end}
{phang2}{cmd:. parqit set statamissing off}{p_end}
{phang2}{cmd:. parqit count if wage > 5000}{space 12}({it:SQL mode: missing wage NOT counted}){p_end}
{phang2}{cmd:. parqit set statamissing on}{p_end}

{pstd}{bf:Several named views}, switched like frames and joined without
materialising either side ({cmd:view:}{it:name} as a {cmd:using} source):{p_end}
{phang2}{cmd:. parqit use using qp_*.parquet, name(panel)}{p_end}
{phang2}{cmd:. parqit keep if year >= 2018}{p_end}
{phang2}{cmd:. parqit use using qp_*.parquet, name(stats)}{p_end}
{phang2}{cmd:. parqit collapse (mean) mw=wage (count) n=wage, by(firmid)}{p_end}
{phang2}{cmd:. parqit views}{p_end}
{phang2}{cmd:. parqit view stats: count}{space 6}({it:one-off against another view}){p_end}
{phang2}{cmd:. parqit view panel}{p_end}
{phang2}{cmd:. parqit merge m:1 firmid using view:stats, keep(match)}{p_end}
{phang2}{cmd:. parqit collect, clear}{p_end}
{phang2}{cmd:. parqit close _all}{p_end}

{pstd}{bf:Copying a view} to branch a pipeline without changing it:{p_end}
{phang2}{cmd:. parqit use using qp_*.parquet, name(panel)}{p_end}
{phang2}{cmd:. parqit keep if year >= 2018}{p_end}
{phang2}{cmd:. parqit use firmid wage using view:panel, name(firms)}{space 2}({it:copy; panel is unchanged}){p_end}
{phang2}{cmd:. parqit collapse (mean) mw=wage, by(firmid)}{p_end}
{phang2}{cmd:. parqit collect, clear}{p_end}
{phang2}{cmd:. parqit view panel}{space 25}({it:all columns, 2018 on}){p_end}

{pstd}{bf:SQL escape hatches} — inject a fragment into the pipeline
({cmd:query}), or run a standalone statement ({cmd:sql}); {cmd:show} and
{cmd:explain} print what will run:{p_end}
{phang2}{cmd:. parqit use using spells.parquet}{p_end}
{phang2}{cmd:. parqit sort id start}{p_end}
{phang2}{cmd:. parqit query "qualify row_number() over (partition by id order by start) = 1"}{p_end}
{phang2}{cmd:. parqit explain}{p_end}
{phang2}{cmd:. parqit collect, clear}{p_end}
{phang2}{cmd:. parqit sql "select year, count(*) n from read_parquet('spells.parquet') group by 1 order by 1", clear}{p_end}

{pstd}{bf:Housekeeping} — engine settings, environment checks:{p_end}
{phang2}{cmd:. parqit set threads 8}{p_end}
{phang2}{cmd:. parqit set memory_limit 8GB}{p_end}
{phang2}{cmd:. parqit set tempdir "/scratch/$USER"}{space 4}({it:spill directory for out-of-core runs}){p_end}
{phang2}{cmd:. parqit version}{p_end}
{phang2}{cmd:. parqit selftest}{space 17}({it:end-to-end engine/codec check on a new machine}){p_end}


{pstd}Two runnable companions, {cmd:parqit_basics.do} and
{cmd:parqit_tour.do}, ship with parqit as ancillary files
({cmd:net get parqit} from the installation source copies them into the current
directory) and live in the source repository{c 39}s {cmd:examples/} directory.
Both create small artificial NLS-style labour-panel data under Stata{c 39}s
temporary directory, so they require no data download. {bf:Start with}
{cmd:parqit_basics.do}: a gentle course in Parquet I/O and metadata, lazy
views and sampling, {cmd:collect} versus {cmd:save} (including a partitioned
directory and a multi-file glob), lazy and in-memory merge and append, and
CSV-to-Parquet conversion. {cmd:parqit_tour.do} then covers engine-side
statistics and missing-value modes, richer lazy transformations, collapse,
pivot, contract and reshape, named views, view-to-view merge, joinby, raw SQL
and engine settings. Comments identify the matching {bf:User > parqit}
dialogs; neither file is exhaustive.{p_end}


{pmore}{cmd:. net get parqit, from("https://github.com/reisportela/parqit/releases/latest/download")}{p_end}
{phang2}{cmd:. do parqit_basics.do}{p_end}
{phang2}{cmd:. do parqit_tour.do}{p_end}


{marker limitations}{...}
{title:Limitations}

{p 4 6 2}{cmd:•} A view is a plan over live files, not a snapshot: collecting again
runs it again, so keep the source files unchanged while you work.{p_end}
{p 4 6 2}{cmd:•} Expressions use Stata's missing comparisons by default;
{cmd:parqit set statamissing off} selects SQL comparisons. {cmd:_n} and {cmd:_N} work in
{cmd:keep if}, {cmd:drop if} and {cmd:gen}.{p_end}
{p 4 6 2}{cmd:•} Lazy {cmd:merge m:m} is refused; {cmd:collapse} and {cmd:pivot}
take no weights; {cmd:reshape wide} and {cmd:pivot} spread at most 2,000
values.{p_end}
{p 4 6 2}{cmd:•} A lazy {cmd:merge} or {cmd:joinby} returns its rows ordered by the
key, not in native Stata's order; sort after collecting if {cmd:_n} matters.{p_end}
{p 4 6 2}{cmd:•} Stata holds at most 2,147,483,647 observations; the lazy path and
{cmd:save} are not so bounded.{p_end}
{p 4 6 2}{cmd:•} Extended missing values {cmd:.a}-{cmd:.z} become plain missing in
Parquet unless saved with {opt xmissing}, and a lazy view reads them as plain
missing values.{p_end}
{p 4 6 2}{cmd:•} Integers beyond 2^53 are refused unless {opt int64()} says to round
them or load them as text; {cmd:BINARY} columns load only with
{opt binary()}.{p_end}
{p 4 6 2}{cmd:•} Sampling designs follow {cmd:sample2}'s rules but not its random
draw.{p_end}

{pstd}
The complete list, with the contract behind each item, is in
{help parqit_technical##limitations:Limitations}.


{marker results}{...}
{title:Stored results}

{pstd}
The main commands store the following in {cmd:r()}:

{p2colset 5 30 32 2}{...}
{p2col:{cmd:use}, {cmd:clear}; {cmd:collect}}{cmd:r(N)}, {cmd:r(k)}{p_end}
{p2col:{cmd:use} (lazy)}{cmd:r(k)}, {cmd:r(view)}{p_end}
{p2col:{cmd:save}}{cmd:r(N)}, {cmd:r(k)}, {cmd:r(filename)}; {cmd:r(ext_missing)} and
{cmd:r(frac_dates)} name variables whose extended missing values or fractional
dates were lost{p_end}
{p2col:{cmd:count}, {cmd:list}}{cmd:r(N)}{p_end}
{p2col:{cmd:summarize}}{cmd:r(N)}, {cmd:r(mean)}, {cmd:r(sd)}, {cmd:r(Var)},
{cmd:r(min)}, {cmd:r(max)}, {cmd:r(sum)}; with {opt detail} also
{cmd:r(skewness)}, {cmd:r(kurtosis)} and {cmd:r(p1)} to {cmd:r(p99)}{p_end}
{p2col:{cmd:tabulate}}{cmd:r(N)}, {cmd:r(r)}, and {cmd:r(c)} for two-way
tables{p_end}
{p2col:{cmd:tabstat}, {cmd:save}}{cmd:r(StatTotal)}, or {cmd:r(Stat1)}, ... with
{opt by()}{p_end}
{p2col:{cmd:correlate}, {cmd:pwcorr}}{cmd:r(rho)}, {cmd:r(N)}, matrix {cmd:r(C)};
{cmd:pwcorr} adds {cmd:r(Nobs)} and, with {opt sig}, {cmd:r(sig)}{p_end}
{p2col:{cmd:histogram}}{cmd:r(N)}, {cmd:r(bins)}, {cmd:r(width)},
{cmd:r(start)}{p_end}
{p2col:{cmd:levelsof}}{cmd:r(levels)}, {cmd:r(r)}{p_end}
{p2col:{cmd:unique}}{cmd:r(N)}, {cmd:r(N_used)}, {cmd:r(sum)}, {cmd:r(unique)}{p_end}
{p2col:{cmd:xtset}, {cmd:tsset}}{cmd:r(panelvar)}, {cmd:r(timevar)},
{cmd:r(delta)}; when declaring also {cmd:r(tmin)}, {cmd:r(tmax)} and
{cmd:r(gaps)} (1 when a panel has gaps){p_end}
{p2col:{cmd:duplicates report}}{cmd:r(N)}, {cmd:r(unique_value)},
{cmd:r(surplus)}{p_end}
{p2col:{cmd:misstable}}{cmd:r(N)}, {cmd:r(n_complete)}; with {cmd:patterns},
{cmd:r(r)}{p_end}
{p2col:{cmd:ds}, {cmd:lookfor}}{cmd:r(varlist)}{p_end}
{p2col:{cmd:describe} {it:file}}{cmd:r(n_rows)}, {cmd:r(n_cols)} and, per
variable, its name, types and labels{p_end}
{p2col:{cmd:version}}{cmd:r(parqit_version)}, {cmd:r(duckdb_version)},
{cmd:r(build_id)}, {cmd:r(numeric_contract)}, {cmd:r(cpus)}, {cmd:r(threads)}{p_end}
{p2colreset}{...}

{pstd}
The complete list, including the SPSS and R conversions, the text-decoding
counts and the views, is in
{help parqit_technical##results:Stored results}.


{marker author}{...}
{title:Authors}

{pstd}Miguel Portela{break}
NIPE / Universidade do Minho and BPLIM / Banco de Portugal{break}
Email: {browse "mailto:miguel.portela@eeg.uminho.pt":miguel.portela@eeg.uminho.pt}{p_end}

{pstd}Rute Costa{break}
BPLIM / Banco de Portugal{break}
Email: {browse "mailto:ricosta@bportugal.pt":ricosta@bportugal.pt}{p_end}

{pstd}Paulo Guimarães{break}
BPLIM / Banco de Portugal{break}
Email: {browse "mailto:pfguimaraes@bportugal.pt":pfguimaraes@bportugal.pt}{p_end}

{pstd}Marta Silva{break}
BPLIM / Banco de Portugal{break}
Email: {browse "mailto:msilva@bportugal.pt":msilva@bportugal.pt}{p_end}

{pstd}Only the listed human authors are authors or co-authors of {cmd:parqit}. No
software tool or AI system is credited as an author or co-author.{p_end}

{pstd}Issues and source:
{browse "https://github.com/reisportela/parqit":github.com/reisportela/parqit}.{p_end}


{marker acknowledgements}{...}
{title:Acknowledgements}

{pstd}
{cmd:parqit} takes {bf:pq} by Jon Rothbaum as its starting point -- the work from
which the {cmd:parqit} solution was designed -- and re-bases the manipulation
layer on an embedded engine. Full credit and thanks to:{p_end}
{phang2}{bf:pq} by Jon Rothbaum (Stata) -
{browse "https://github.com/jrothbaum/stata_parquet_io":github.com/jrothbaum/stata_parquet_io}{p_end}
{phang2}{bf:DuckDB} - {browse "https://duckdb.org":duckdb.org}{p_end}
{phang2}{bf:Apache Arrow C Data Interface} -
{browse "https://arrow.apache.org/docs/format/CDataInterface.html":arrow.apache.org}{p_end}

{pstd}
Jon Rothbaum's package, and the care he puts into its correctness, directly shaped
{cmd:parqit}'s design and its test suite; the debt is gratefully acknowledged.{p_end}

{pstd}
We warmly thank the {bf:BPLIM} team at {bf:Banco de Portugal}
({browse "https://bplim.bportugal.pt/":bplim.bportugal.pt}), whose interaction
throughout greatly benefited the development of {cmd:parqit}.{p_end}

{pstd}
{cmd:parqit} embeds {browse "https://duckdb.org":DuckDB} and uses the Apache Arrow
C Data Interface; it is not affiliated with StataCorp. All remaining errors are the
authors'.{p_end}
