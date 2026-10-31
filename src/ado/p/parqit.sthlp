{smcl}
{* *! version 0.8.3 31oct2026}{...}
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
{viewerjumpto "Syntax" "parqit##syntax"}{...}
{viewerjumpto "Menu" "parqit##menu"}{...}
{viewerjumpto "Description" "parqit##description"}{...}
{viewerjumpto "Options" "parqit##options"}{...}
{viewerjumpto "Views" "parqit##lazy"}{...}
{viewerjumpto "Lazy verbs and expressions" "parqit##verbs"}{...}
{viewerjumpto "Other file formats" "parqit##formats"}{...}
{viewerjumpto "Settings and diagnostics" "parqit##settings"}{...}
{viewerjumpto "Examples" "parqit##examples"}{...}
{viewerjumpto "Stored results" "parqit##results"}{...}
{viewerjumpto "Technical reference" "parqit##help"}{...}
{viewerjumpto "Authors" "parqit##author"}{...}
{p2colset 1 11 13 2}{...}
{p2col:{bf:parqit} {hline 2}}Read, write, and manipulate Parquet data from
Stata using an embedded DuckDB engine{p_end}
{p2colreset}{...}

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
{p 8 16 2}{cmd:parqit} {cmdab:g:enerate} [{it:type}] {it:newvar} {cmd:=} {it:{help parqit##expressions:exp}} [{cmd:if} {it:exp}]{p_end}
{p 8 16 2}{cmd:parqit replace} {it:var} {cmd:=} {it:exp} [{cmd:if} {it:exp}]{p_end}
{p 8 16 2}{cmd:parqit egen} [{it:type}] {it:newvar} {cmd:=} {it:{help parqit##egen:fcn}}{cmd:(}{it:arguments}{cmd:)} [{cmd:if} {it:exp}]
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
{p 8 16 2}{cmd:parqit collapse} {cmd:(}{it:stat}{cmd:)} [{it:tgt}{cmd:=}]{it:src} ...
[{cmd:if} {it:exp}] [{cmd:in} {it:range}] [{it:weight}]
[{cmd:,} {opt by(varlist)} {it:collapse_options}]{p_end}
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

{marker explore}{...}
{pstd}
Look at the view (engine-side; the dataset in memory is unchanged)

{p 8 16 2}{cmd:parqit} {cmdab:d:escribe} [{it:parquet_source} [{cmd:,} {opt lab:els} {opt not:es}]]{space 4}(the options need a file){p_end}
{p 8 16 2}{cmd:parqit} {cmdab:l:ist} [{it:varlist}] [{cmd:if} {it:exp}] [{cmd:in} {it:f}[{cmd:/}{it:l}]]
{space 2}({it:f} and {it:l} are observation numbers; the letters and negative bounds of {cmd:keep in} are not taken){p_end}
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
{p 8 16 2}{cmd:parqit set duckdb} [{it:name} [{it:value}]]{space 2}native catalog, effective value, or SET{p_end}
{p 8 16 2}{cmd:parqit set duckdb reset} {it:name}{space 2}native RESET{p_end}
{p 8 16 2}{cmd:parqit version}{space 4}(plugin + engine versions){p_end}
{p 8 16 2}{cmd:parqit selftest}{space 3}(end-to-end engine and codec check, useful on new installs/HPC nodes){p_end}
{p 8 16 2}{cmd:parqit menu}{space 8}(add parqit to the {bf:User} menu — GUI Stata only){p_end}

{pstd}
where {it:filename} is a Parquet file, a glob such as {it:data_*.parquet}, a
Hive-partitioned directory, or a delimited-text, Stata, Excel, SPSS or R file;
and each two-table {it:source} is such a file or {cmd:view:}{it:viewname},
another open view.

{pstd}
Local Parquet sources retain their absolute working-directory context at
open, including Hive partition columns, so a view keeps its source after
{cmd:cd}. See {help parqit_technical##reading:Reading data} for path rules.

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

{marker menu}{...}
{title:Menu}

{pstd}
In GUI Stata, {cmd:parqit menu} adds {bf:User > parqit} to the menu bar for
the current session. Each dialog builds and runs an ordinary {cmd:parqit}
command, echoed to the Results window for reuse in a do-file.

{pstd}
Dialogs that operate on a view offer a view selector. Commands change the
selected view's plan and restore which view was current; selecting a view
does not copy it. Use the view-copy mode to work on a separate plan.
The Viewer's {bf:Dialog} menu also opens the dialogs. See
{help parqit_technical##dialogs:Menus and dialogs} for their fields and tabs.

{marker description}{...}
{marker map}{...}
{title:Description}

{pstd}
{cmd:parqit} reads, writes, joins and reshapes Parquet data, either in Stata's
memory or out of core through a {it:view}. It is designed for datasets that
are too large or too slow to load into Stata.

{pstd}
A {bf:view} is a query plan managed by an embedded DuckDB engine. The source
data remain on disk; Stata-style {cmd:parqit} verbs build the plan, and
engine-side summaries let you explore it without replacing the dataset in
memory. {cmd:collect} loads the result into Stata; {cmd:save} writes it
directly to Parquet. Several named views can remain open simultaneously.

{pstd}
Variable and value labels, notes, formats, characteristics and original
column names travel in standard Parquet metadata. Other software can read
the files, but does not automatically restore Stata's labels and formats.
See {help parqit_technical##metadata:Stata metadata in Parquet}.

{pstd}
A typical session involves four steps, repeated as needed:

   {c TLC}{c -} {bf:1  OPEN} {c -} start a view
   {c |}{space 4}{cmd:parqit use} {it:file}
   {c |}{space 4}{cmd:parqit use view:}{it:name}{cmd:, name(copy)}
   {c |}{space 4}{cmd:parqit open _data}
   {c |}
   {c LT}{c -} {bf:2  SHAPE} {c -} filter, derive, aggregate or join
   {c |}{space 4}{cmd:keep} {cmd:drop} {cmd:gen} {cmd:egen} {cmd:replace}
   {c |}{space 4}{cmd:collapse} {cmd:reshape} {cmd:merge} {cmd:append} ...
   {c |}
   {c LT}{c -} {bf:3  LOOK} {c -} summaries computed by the engine
   {c |}{space 4}{cmd:describe} {cmd:count} {cmd:list} {cmd:summarize} {cmd:tabulate} ...
   {c |}
   {c BLC}{c -} {bf:4  LAND} {c -} run the plan and keep the result
   {space 5}{cmd:parqit collect}{space 5}load the result into Stata
   {space 5}{cmd:parqit save} {it:file}{space 3}write the result to Parquet

{pstd}
Look whenever you like, shape again after looking, and collect or save as
often as needed. A view stays open and runs its plan again each time.

{marker options}{...}
{title:Options}

{marker use_options}{...}
{dlgtab:use}

{phang}
{opt clear} reads the result into memory instead of opening a view. The data
in memory are replaced only when the new result is complete and valid.
An {cmd:if} before {cmd:using} filters rows; its condition may use file
variables outside the requested varlist. Open views remain unchanged.

{phang}
{opt name(viewname)} opens the view under {it:viewname}, replacing an open
view of that name; the default name is {cmd:default}.

{phang}
{opt compress}, with {opt clear}, narrows storage types losslessly as native
{helpb compress} does, before replacing the dataset in memory.

{phang}
{opt relaxed} combines files with different columns by name. A column absent
from a file is missing there; without this option, differing files are refused.

{phang}
{opt encoding(name[, all])} specifies the code page of text that is not UTF-8
in delimited text, Stata or Excel files, or overrides the encoding declared by
SPSS or R. The session default is set by {cmd:parqit set encoding}.
{cmd:all} also decodes text that is valid UTF-8. A note reports decoding.

{phang}
{opt int64(refuse|round|string)} controls integers beyond 2^53: {cmd:refuse}
(the default) stops rather than lose precision, {cmd:round} accepts the
nearest double, and {cmd:string} loads exact text.
{cmd:parqit set int64} changes the session default.

{phang}
{opt binary(text|hex)} loads Parquet {cmd:BINARY} columns as UTF-8 text or
hexadecimal digits; otherwise these columns are dropped with a message.

{phang}
{opt filename(newvar)} records the source file of each row. Local Parquet
paths are absolute; CSV paths retain their matched spelling.

{phang}
{opt csv(read_options)} controls delimited-text reading: {opt delim()},
{opt quote()}, {opt escape()}, {opt header(on|off)}, {opt dateformat()},
{opt timestampformat()}, {opt sample()}, {opt allvarchar}, {opt types()}
and {opt nullstr()}. Use {opt types()} or {opt allvarchar} to protect values
such as identifiers with leading zeros.

{phang}
{opt object(name)} selects the data frame from an R file containing several.

{marker materialisers}{...}
{dlgtab:collect}

{phang}
{opt clear} permits replacement of changed data in memory. Without it,
{cmd:collect} can load into empty or unchanged data. The swap is atomic and
the view stays open. {opt int64()} is as for {cmd:use}; {opt compress}
narrows storage types losslessly after loading and does not require {opt clear}.

{dlgtab:save}

{phang}
{opt replace} permits overwriting an existing file or tree.

{phang}
{opt view} explicitly saves the current view, the default when a view is open.
With no view, {cmd:save} writes the dataset in memory and {opt view} is refused.
{opt data} always saves the dataset in memory, even with an open view.

{phang}
{opt compression(codec)} selects {cmd:zstd} (the default), {cmd:snappy},
{cmd:gzip}, {cmd:lz4}, {cmd:lz4_raw}, {cmd:brotli} or {cmd:uncompressed}.
{opt compression_level(#)} sets the level for codecs that support it.

{phang}
{opt partition_by(varlist)} writes a Hive directory tree, with directories
for the combinations of partition-key values.

{phang}
{opt partitions(replace|append)} updates an existing tree. {cmd:replace}
replaces the partitions present in the result and keeps the others;
{cmd:append} adds files to existing or new partitions. Columns and metadata
must match, and {opt partition_by()} must name the tree's keys.

{phang}
{opt chunk(#)} sets rows per Parquet row group.

{phang}
{opt encoding(name)} decodes legacy text in the data being saved to UTF-8.

{phang}
{opt copysource}, with {opt data}, copies the original Parquet file loaded
by the last {cmd:parqit use ..., clear}. You assert that the data have not
changed; parqit performs partial checks, not a comparison of every value.
See {help parqit_technical##materialisers:Materialisers} before using it.

{phang}
{opt xmissing} preserves {cmd:.a}-{cmd:.z} in companion columns for eager
reads to restore. Lazy views still treat these categories as ordinary missing.

{phang}
{opt object(name)} selects the R data frame for {cmd:save ... using}.
That conversion form takes only the save options shown in its syntax line.

{dlgtab:collapse, egen, by, xtset}

{phang}
{cmd:collapse} accepts native statistics, additional aggregation statistics,
{cmd:if}, {cmd:in}, and {cmd:aweight}, {cmd:fweight}, {cmd:iweight} or
{cmd:pweight}, subject to the statistic's weight restrictions.
Options include {opt by()}, {cmd:cw}, {opt rawstat()}, {opt freq()},
{cmd:merge}, {cmd:replace}, {cmd:noinit}, {cmd:wildparse}, {cmd:unsorted},
{cmd:double}, {cmd:sumcheck}, {cmd:missing}, {opt labelformat()}, {opt labelprogram()},
{cmd:ds} and {cmd:nods}. {cmd:pivot} remains unweighted. The complete
statistics, option meanings and weight restrictions are in
{help parqit_technical##collapse:Aggregation contracts}.

{phang}
{cmd:egen} accepts the functions and options in
{help parqit_technical##egen:egen functions}. An unsupported function is an error.

{phang}
{cmd:by} and {cmd:bysort} prefix {cmd:gen}, {cmd:replace}, {cmd:egen},
{cmd:keep if} and {cmd:drop if}. Positions count within groups;
{cmd:by} requires the declared order, while {cmd:bysort} sorts first.

{phang}
{cmd:format} changes display formats without changing values or units.
{cmd:xtset} and {cmd:tsset} declare panel/time keys for {cmd:L.}, {cmd:F.},
{cmd:D.} and {cmd:S.}; they do not sort the view. Sort explicitly when row
positions matter. Time-series operators respect gaps in the time variable.

{dlgtab:merge, append, joinby}

{phang}
Lazy {cmd:merge} supports {opt keep()}, {opt keepusing()}, {opt generate()},
{opt nogenerate}, {opt nonotes} and {opt encoding()}. {cmd:append} supports
{opt keep(varlist)}, {opt generate(newvar)}, {opt nonotes} and {opt encoding()}.
{cmd:joinby} supports {opt encoding()}. Each takes a disk file or an open
{cmd:view:}{it:name} as its using source.

{dlgtab:mergein, appendin}

{phang}
{cmd:mergein} passes options to native {helpb merge}; {cmd:appendin} passes
{opt keep()}, {opt generate()}, {opt nolabel}, {opt nonotes} and {opt force}
to native {helpb append}. {opt int64()} and {opt encoding()} apply to the
using file. In {cmd:append} and {cmd:appendin}, {opt keep()} selects variables
from the using source, as in native {cmd:append}.

{marker sampledesign}{...}
{dlgtab:sample}

{phang}
{it:#} is a percentage, or a row count with {opt count}. {opt seed(#)} makes
the draw reproducible. {opt by()} draws within strata; {opt cluster()}
draws whole clusters, with {opt any} or {opt all} resolving a split
{cmd:if} frame. Rows outside the frame are kept and never drawn.
{opt generate()}, or {opt keep()}, flags the draw instead of dropping rows.
Designs follow {cmd:sample2}, but use different random draws. See
{help parqit_technical##sampledesign:Sampling designs}.

{dlgtab:Statistics}

{phang}
Statistics commands take the options shown in their syntax lines.
{cmd:unique} counts each variable and the varlist jointly; missing counts
as a value unless {opt nomissing} excludes it. {opt limit()} bounds the
values or rows displayed by {cmd:levelsof} and {cmd:duplicates list}.
See {help parqit_technical##explore:Exploration commands} for definitions
and output limits.

{marker lazy}{...}
{title:Views}

{pstd}
{cmd:parqit use} creates a named view ({cmd:default} unless {opt name()}
specifies otherwise). Commands operate on the current view by default.

{phang2}{cmd:parqit views}{space 6}list open views{p_end}
{phang2}{cmd:parqit view} {it:name}{space 2}switch the current view{p_end}
{phang2}{cmd:parqit view} {it:name}{cmd::} {it:command}{space 2}run one command on that view, then switch back{p_end}
{phang2}{cmd:parqit close} [{it:name}|{cmd:_all}]{space 2}close the current, named or all views{p_end}

{marker copy}{...}
{pstd}
{cmd:parqit use view:}{it:name}{cmd:, name(newview)} copies a plan and makes
the copy current. Both views read the same files; changes to the copy's plan
leave the original plan unchanged. Views are plans over live files, not
snapshots: keep the source files unchanged while working.

{pstd}
{cmd:merge}, {cmd:append} and {cmd:joinby} combine views. {cmd:mergein} and
{cmd:appendin} combine the dataset in Stata's memory with a disk file.
Use these for an in-memory master and a small lookup; use a view when both
sides are large. More: {help parqit_technical##views:Views}, including the
correspondence with {cmd:pq} commands.

{marker verbs}{...}
{title:Lazy verbs and expressions}

{pstd}
Lazy verbs build the view's plan without changing the dataset in memory.
Names and syntax are validated before the plan changes; a refused verb
leaves the view unchanged. Some checks require the actual data and occur
when the plan runs. Varlists accept wildcards and unambiguous abbreviations.

{marker expressions}{...}
{pstd}
Expressions support arithmetic, relational and logical operators, string
operations and the functions listed in
{help parqit_technical##expressions:Expression dialect}.
{cmd:_n}, {cmd:_N} and positional subscripts work in {cmd:keep if},
{cmd:drop if}, {cmd:gen}, {cmd:replace} and documented {cmd:egen} expressions,
including within {cmd:by} groups. Read-only statistics filters such as
{cmd:count if} and {cmd:list if} do not support positional references.
Date literals use forms such as {cmd:td(01jan2010)}, {cmd:tm(2010m1)} and
{cmd:tq(2010q1)}; the technical reference lists the supported date/time forms.

{pstd}
By default, numeric missing compares above every nonmissing number, as in
Stata: {cmd:keep if x > 5} also keeps missing values. To exclude them, use
{cmd:x > 5 & x < .} or {cmd:!missing(x)}. Explicitly select SQL comparisons
with {cmd:parqit set statamissing off}; comparisons with NULL then do not
evaluate to true. A lazy stage keeps the mode in which it was built.

{marker egen}{...}
{pstd}
Key differences from native Stata:

{phang2}{cmd:•} {cmd:replace} cannot read other rows of the variable it replaces,
in either its expression or its {cmd:if}; generate a new variable instead.{p_end}
{phang2}{cmd:•} {cmd:duplicates drop} with a varlist requires {opt force} and
a preceding {cmd:parqit sort}, retaining the first row in that order.{p_end}
{phang2}{cmd:•} {cmd:keep in} and {cmd:drop in} validate ranges when the plan runs.{p_end}
{phang2}{cmd:•} Lazy {cmd:merge m:m} is refused. {cmd:joinby} forms every pair
sharing a key; {cmd:mergein m:m} uses native's order-based pairing.{p_end}
{phang2}{cmd:•} {cmd:xtset}/{cmd:tsset} do not sort; {cmd:pivot} is unweighted;
{cmd:pivot}/{cmd:reshape wide} spread at most 2,000 distinct values.{p_end}
{phang2}{cmd:•} Untyped numeric {cmd:gen} results are {cmd:double}.
Use {cmd:collect, compress} to narrow result storage losslessly.{p_end}

{pstd}
The complete {help parqit_technical##egen:egen catalogue},
{help parqit_technical##verbs:verb contracts} and
{help parqit_technical##limitations:limitations} are in the technical help.

{marker formats}{...}
{title:Other file formats}

{pstd}
Besides Parquet, {cmd:parqit use} opens delimited text ({cmd:.csv},
{cmd:.tsv}, {cmd:.txt}, {cmd:.tab}), Stata ({cmd:.dta}), Excel
({cmd:.xls}, {cmd:.xlsx}), SPSS ({cmd:.sav}, {cmd:.zsav}) and R
({cmd:.rds}, {cmd:.rda}, {cmd:.RData}) files. These also work as the using
source for lazy combinations. Parquet and delimited text are read out of
core; other inputs are first converted to temporary Parquet.

{marker spss}{...}
{marker rdata}{...}
{pstd}
SPSS and R are read by parqit's own readers, without SPSS or R installed.
Labels, formats and missing-value codes are converted to Stata representations.
{cmd:parqit save new.parquet using old.sav} converts SPSS out of core;
the same form accepts R inputs. {cmd:spssencode} creates a labelled numeric
variable from SPSS string codes or R haven labels. See
{help parqit_technical##spss:SPSS files} and
{help parqit_technical##rdata:R data files} for mappings and restrictions.

{marker encoding}{...}
{pstd}
Text in other code pages is decoded to UTF-8 with {opt encoding()} or the
session default ({cmd:windows-1252} until changed). SPSS and R declare their
own encodings. See {help parqit_technical##encoding:Text encodings}.

{marker settings}{...}
{title:Settings and diagnostics}

{pstd}
{cmd:parqit set} {it:setting value} changes a session setting:

{p2colset 9 38 40 2}{...}
{p2col:{cmd:statamissing on}|{cmd:off}}Stata's default or SQL missing comparisons{p_end}
{p2col:{cmd:int64 refuse}|{cmd:round}|{cmd:string}}handling of large integers{p_end}
{p2col:{cmd:encoding} {it:name}}default code page of legacy text{p_end}
{p2col:{cmd:threads} {it:#}}engine threads; default, the CPUs available{p_end}
{p2col:{cmd:fill_threads auto}|{it:#}}threads transferring data to/from Stata{p_end}
{p2col:{cmd:stream_buffer_mb auto}|{it:#}}result buffered ahead of transfer{p_end}
{p2col:{cmd:memory_limit} {it:size}}engine memory budget, for example {cmd:8GB}{p_end}
{p2col:{cmd:tempdir} {it:path}}directory for disk spill{p_end}
{p2colreset}{...}

{pstd}
{cmd:parqit set duckdb} lists native engine setting names, types and scopes.
Add a name to inspect its value, a literal to set it, or {cmd:reset name}
to restore the engine default. For example:

{phang2}{cmd:. parqit set duckdb threads 4}{p_end}
{phang2}{cmd:. parqit set duckdb threads}{p_end}
{phang2}{cmd:. parqit set duckdb reset threads}{p_end}

{pstd}
Settings are validated by DuckDB; some cannot change during a session.
Keep {cmd:preserve_insertion_order true} when observation order matters.
Native streaming overrides, precedence and resource limits are explained in
{help parqit_technical##native_settings:Native settings reference}.

{pstd}
{cmd:show} prints the view's SQL and {cmd:explain} its execution plan.
{cmd:query} adds a raw SQL clause; {cmd:sql} opens a view over a SQL query
or loads its result with {opt clear}. These use native DuckDB SQL, separately
from parqit's Stata expression rules.

{pstd}
{cmd:version} reports the loaded package, engine and plugin identity;
{cmd:selftest} checks the installation using a separate engine, preserving
open views. {cmd:path} resolves a file path. Restart Stata after updating
parqit so it loads the matching plugin. See
{help parqit_technical##settings:Settings, raw SQL and diagnostics} and
{help parqit_technical##perf:Performance tips} for details.

{marker examples}{...}
{marker quickstart}{...}
{title:Examples}

{pstd}
The examples below assume the named files and variables are available in
the current working directory. Each example starts from its own source.
The shipped {cmd:parqit_basics.do} and {cmd:parqit_tour.do} instead create
synthetic data and provide runnable introductions without external inputs.

{pstd}{bf:Explore a Parquet file}{p_end}
{phang2}{cmd:. parqit describe data.parquet, labels notes}{p_end}
{phang2}{cmd:. parqit use data.parquet}{p_end}
{phang2}{cmd:. parqit list in 1/10}{p_end}
{phang2}{cmd:. parqit codebook}{p_end}
{phang2}{cmd:. parqit tabulate region sector, row}{p_end}
{phang2}{cmd:. parqit count if missing(wage, age)}{p_end}
{phang2}{cmd:. parqit close}{p_end}

{pstd}{bf:Filter, transform and aggregate out of core}{p_end}
{phang2}{cmd:. parqit use using workers_*.parquet, filename(source)}{p_end}
{phang2}{cmd:. parqit keep if year >= 2010 & year < . & inrange(age, 25, 64)}{p_end}
{phang2}{cmd:. parqit gen double lwage = ln(wage)}{p_end}
{phang2}{cmd:. parqit collapse (mean) lwage (count) n=lwage, by(firmid year)}{p_end}
{phang2}{cmd:. parqit collect, clear}{p_end}
{pstd}To keep the result on disk, use {cmd:parqit save result.parquet}
instead of {cmd:collect}. The view stays open in either case.{p_end}

{pstd}{bf:Compare populations using a copy of a view}{p_end}
{phang2}{cmd:. parqit use survey.parquet, name(full)}{p_end}
{phang2}{cmd:. parqit use view:full, name(adults)}{p_end}
{phang2}{cmd:. parqit keep if age >= 18 & age < .}{p_end}
{phang2}{cmd:. parqit summarize wage, detail}{p_end}
{phang2}{cmd:. parqit view full: summarize wage, detail}{p_end}
{phang2}{cmd:. parqit view full}{p_end}
{phang2}{cmd:. parqit views}{p_end}
{phang2}{cmd:. parqit close _all}{p_end}

{pstd}{bf:Join datasets on disk and save a partitioned result}{p_end}
{phang2}{cmd:. parqit use firm_panel.parquet}{p_end}
{phang2}{cmd:. parqit merge m:1 firmid year using firm_lookup.parquet, keep(match) keepusing(tfp)}{p_end}
{phang2}{cmd:. parqit save firm_panel_merge, partition_by(year)}{p_end}

{pstd}{bf:Append to the dataset in memory}{p_end}
{phang2}{cmd:. use master, clear}{p_end}
{phang2}{cmd:. parqit appendin using late_arrivals.parquet, keep(firmid wage)}{p_end}

{pstd}{bf:Merge two views}{p_end}
{phang2}{cmd:. parqit use workers.parquet, name(workers)}{p_end}
{phang2}{cmd:. parqit use firms.csv, name(firms)}{p_end}
{phang2}{cmd:. parqit view workers}{p_end}
{phang2}{cmd:. parqit merge m:1 firmid using view:firms}{p_end}
{phang2}{cmd:. parqit collect, clear}{p_end}

{pstd}{bf:Create variables with Stata expressions}{p_end}
{phang2}{cmd:. parqit use workers.parquet}{p_end}
{phang2}{cmd:. parqit gen byte prime = inrange(age, 25, 54)}{p_end}
{phang2}{cmd:. parqit gen hire_year = year(hire_date)}{p_end}
{phang2}{cmd:. parqit replace wage = . if wage <= 0}{p_end}
{phang2}{cmd:. parqit egen double fw = mean(wage), by(firmid)}{p_end}
{phang2}{cmd:. parqit keep if hire_date >= td(01jan2015) & hire_date < .}{p_end}

{pstd}{bf:Convert SPSS, or read an R data frame}{p_end}
{phang2}{cmd:. parqit save survey_converted.parquet using survey.sav}{p_end}
{phang2}{cmd:. parqit use survey_converted.parquet, clear}{p_end}
{phang2}{cmd:. parqit use workspace.RData, clear object(people)}{p_end}

{pstd}{bf:Use SQL within a pipeline or as a standalone query}{p_end}
{phang2}{cmd:. parqit use spells.parquet}{p_end}
{phang2}{cmd:. parqit sort id start}{p_end}
{phang2}{cmd:. parqit query "qualify row_number() over (partition by id order by start) = 1"}{p_end}
{phang2}{cmd:. parqit explain}{p_end}
{phang2}{cmd:. parqit sql "select year, count(*) n from read_parquet('spells.parquet') group by 1 order by 1", clear}{p_end}

{pstd}
Further examples of types, metadata, reshaping and sampling are in
{help parqit_technical##examples:Worked examples}. For a self-contained
introduction, run the shipped {cmd:parqit_basics.do}, followed by
{cmd:parqit_tour.do}.

{marker limitations}{...}
{pstd}
{bf:Practical limits.} Eager reads and {cmd:collect} are limited to
2,147,483,647 observations by the plugin transfer interface; larger lazy
results can be saved to Parquet. Lazy joins return rows ordered by key;
declare a complete sort before using row positions. Extended missing
categories are folded in lazy views, even with an {opt xmissing} source.
The full {help parqit_technical##limitations:list of limitations} is in
the technical reference.

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
{cmd:r(skewness)}, {cmd:r(kurtosis)} and
{cmd:r(p1) r(p5) r(p10) r(p25) r(p50) r(p75) r(p90) r(p95) r(p99)}{p_end}
{p2col:{cmd:tabulate}}{cmd:r(N)}, {cmd:r(r)}, and {cmd:r(c)} for two-way
tables{p_end}
{p2col:{cmd:tabstat}, {cmd:save}}{cmd:r(StatTotal)}, or {cmd:r(Stat1)}, ... with
{opt by()}{p_end}
{p2col:{cmd:correlate}, {cmd:pwcorr}}{cmd:r(rho)}, {cmd:r(N)}, matrix {cmd:r(C)};
{cmd:pwcorr} adds {cmd:r(Nobs)} and, with {opt sig}, {cmd:r(sig)}{p_end}
{p2col:{cmd:histogram}}{cmd:r(N)}, {cmd:r(bins)}, {cmd:r(width)},
{cmd:r(start)}{p_end}
{p2col:{cmd:levelsof}}{cmd:r(levels)}, {cmd:r(r)}{p_end}
{p2col:{cmd:unique}}{cmd:r(N)}, {cmd:r(N_used)}, {cmd:r(sum)}, {cmd:r(unique)};
{cmd:r(sum)} and {cmd:r(unique)} both hold the joint count{p_end}
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


{marker help}{...}
{title:Technical reference}

{pstd}
{help parqit_technical:{bf:help parqit_technical}} contains the complete
expression and {cmd:egen} catalogues, aggregation contracts, type mappings,
metadata layout, precision and atomicity rules, engine settings, dialog
fields, worked examples and limitations.

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
