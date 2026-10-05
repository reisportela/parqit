{smcl}
{* *! version 0.3.3 05oct2026}{...}
{vieweralsosee "[PARQIT] parqit" "help parqit"}{...}
{viewerjumpto "Description" "parqit_technical##description"}{...}
{viewerjumpto "Syntax conventions" "parqit_technical##conventions"}{...}
{viewerjumpto "Views" "parqit_technical##views"}{...}
{viewerjumpto "Reading data" "parqit_technical##reading"}{...}
{viewerjumpto "Input formats" "parqit_technical##formats"}{...}
{viewerjumpto "SPSS files" "parqit_technical##spss"}{...}
{viewerjumpto "R data files" "parqit_technical##rdata"}{...}
{viewerjumpto "Text encodings" "parqit_technical##encoding"}{...}
{viewerjumpto "Stata metadata in Parquet" "parqit_technical##metadata"}{...}
{viewerjumpto "Verb contracts" "parqit_technical##verbs"}{...}
{viewerjumpto "Sampling designs" "parqit_technical##sampledesign"}{...}
{viewerjumpto "Materialisers: atomicity, copysource, encoding, locks" "parqit_technical##materialisers"}{...}
{viewerjumpto "Exploration commands" "parqit_technical##explore"}{...}
{viewerjumpto "Expression dialect" "parqit_technical##expressions"}{...}
{viewerjumpto "Type mapping" "parqit_technical##types"}{...}
{viewerjumpto "Settings, raw SQL and diagnostics" "parqit_technical##settings"}{...}
{viewerjumpto "Environment" "parqit_technical##environment"}{...}
{viewerjumpto "Performance tips" "parqit_technical##perf"}{...}
{viewerjumpto "Menus and dialogs" "parqit_technical##dialogs"}{...}
{viewerjumpto "Stored results" "parqit_technical##results"}{...}
{viewerjumpto "Limitations" "parqit_technical##limitations"}{...}
{viewerjumpto "Authors" "parqit_technical##author"}{...}
{title:Title}

{phang}
{bf:parqit_technical} {hline 2} technical reference for {helpb parqit}: the
contracts behind every command — views, reading data, file formats and text
encodings, metadata, verbs, materialisers, the exploration statistics, the
expression dialect, the type mapping, settings, dialogs, stored results and
the complete list of limitations


{marker description}{...}
{title:Description}

{pstd}
{helpb parqit} is the user's guide: what parqit is for, its syntax and options,
the main behaviours and worked examples. This entry is its technical reference:
the rules parqit follows for compatibility with native Stata and standard
Parquet, with the supported subset and its exceptions, section by section in
the order of the list above. Use these contracts to assess a workflow's
resource needs and fidelity. The regression suites exercise them; passing
tests do not establish correctness for every possible input or platform.

{pstd}
DuckDB executes SQL using its own scheduler; {cmd:parqit set threads} controls
its worker limit. From 0.1.37, plugins do not link an OpenMP runtime or require
a companion Windows DLL. The OpenMP region in 0.1.36 was a diagnostic self-test,
not the execution mechanism for data calculations. Removing it does not change
the statistical algorithms or DuckDB's parallel execution.
{cmd:parqit version} returns {cmd:r(parallel_backend)} equal to {cmd:duckdb};
the legacy {cmd:r(openmp*)} diagnostics return zero.

{pstd}
Compiler-runtime notices remain in {cmd:parqit_openmp_license.txt}; its filename
is retained for package compatibility, although no OpenMP runtime is bundled.
The MIT license for parqit's own code does not replace third-party licenses.


{marker conventions}{...}
{title:Syntax conventions}

{pstd}
The rules shared by every subcommand: abbreviations and varlists.

{pstd}Subcommands and options take the abbreviations native Stata accepts for
the same commands; the underlined part is the shortest form, as in Stata's own
syntax diagrams. So {cmd:parqit su} is {cmd:parqit summarize}, {cmd:parqit ta}
is {cmd:parqit tabulate}, {cmd:parqit d} is {cmd:parqit describe},
{cmd:parqit g} is {cmd:parqit gen} ({cmd:gen}, itself Stata's short form of
{cmd:generate}, is the name the rest of this help uses), and
{cmd:parqit su price, d} asks for the detail. {cmd:hist} is a synonym for {cmd:histogram}, as in Stata, but
{cmd:histo} is not. The commands Stata spells out ({cmd:keep}, {cmd:drop},
{cmd:replace}, {cmd:egen}, {cmd:order}, {cmd:gsort}, {cmd:collapse},
{cmd:contract}, {cmd:duplicates}, {cmd:sample}, {cmd:joinby}, {cmd:reshape},
{cmd:misstable}, {cmd:levelsof}, {cmd:ds}, {cmd:lookfor}, {cmd:codebook},
{cmd:tabstat}, {cmd:pwcorr}) and parqit's own verbs are typed in full. In
{cmd:misstable}, {cmd:sum} and {cmd:pat} are the subcommands, as native, so a
variable named like one of them goes after the subcommand
({cmd:parqit misstable summarize sum}). The syntax below defines the supported
surface.

{pstd}Varlists expand Stata wildcards ({cmd:*} any run of characters,
{cmd:?} one Unicode character) against the exposed Stata column names, in
pattern order without duplicates. This applies to the namelist in eager or
lazy {cmd:parqit use}, the varlists of {cmd:keep}, {cmd:drop}, {cmd:order},
{cmd:contract} and {cmd:duplicates drop}, the {opt by()} of {cmd:egen} and
{cmd:collapse}, {cmd:pivot}'s {opt rows()}, {cmd:mergein}'s {opt keepusing()}
and {cmd:appendin}'s {opt keep()}. {cmd:sort}/{cmd:gsort} and
{cmd:reshape}'s {opt i()} take explicit names only.


{marker views}{...}
{title:Views}

{pstd}
The one idea to internalise: {bf:mutation verbs build a plan rather than materialising their result}.
A view is a plan — "the current dataset", except
that it lives on disk and may be far larger than memory. Opening a view probes
its schema. Verbs validate their names and stated contracts; commands such as
{cmd:keep}, {cmd:gen} and {cmd:collapse} also bind-check their candidate SQL.
Contract-sensitive verbs may run validation queries (for example merge-key uniqueness, reshape
cell uniqueness or pivot column discovery). These checks do not load the result
into Stata. {cmd:parqit collect} and {cmd:parqit save} execute the full result
plan. Projection pushdown can avoid reading unused columns; filter pushdown
can skip Parquet row groups whose statistics rule out a match. Execution may
also need validation, sizing and output-verification queries. The whole
{help parqit_technical##explore:exploration family} ({cmd:describe}, {cmd:head},
{cmd:summarize}, {cmd:tabulate}, {cmd:codebook}, {cmd:misstable}, …) is
computed by separate engine-side queries too, so a file can be profiled without
replacing or modifying the current dataset — explore first, load last. See
{help parqit_technical##views:The lazy view}.

{pstd}{bf:Why this matters for large data.} Keep the worker-level archive on
disk, build a firm-year table with filters, derived variables and aggregation,
join it to another view, and collect only the final research sample. Stata
does not need room for the source archive or each intermediate table. When even
the final table is too large, {cmd:parqit save} completes the transformation
directly to Parquet. Named views let several source plans coexist alongside
the dataset already in Stata, without loading a separate full copy of each
source. This moves the memory requirement from {it:the whole input} to
{it:the result you choose to collect}, plus execution buffers and scratch space.

{pstd}
{cmd:parqit use using} {it:files} opens a {it:view}: a description of the
data plus a pipeline of verbs, like Stata's idea of "the current dataset"
but living on disk. Opening probes source schema and metadata but does not
materialise its result into the current dataset; a delimited source may be
sampled for type inference, and adapter inputs are bridged as described below.
Views are
named (default name: {cmd:default}) and several can be open at once — the
vocabulary mirrors frames. {opt name()} opens under a name; opening a name that
already exists replaces that plan only and makes it current. {cmd:_all} is not a
view name: it is reserved by {cmd:parqit close _all}. {cmd:parqit view}
{it:name} switches the current view. {cmd:parqit view} {it:name}{cmd::}
{it:command} temporarily targets another view and then restores the previously
current view, even when the command fails or opens another view; a lazy verb
still changes the {it:target} view, so "temporary" describes the switch, not the
mutation.
{cmd:parqit views} lists them (bare {cmd:parqit view} does too) and
{cmd:parqit close}
[{it:name}|{cmd:_all}] closes a named view or every view — bare, the
current one. Verbs always act on the current view. A view holds its schema
and plan; an adapter or {cmd:open _data} may also own a temporary Parquet
snapshot. A {cmd:view:}{it:name} two-table source embeds
the source view's current compiled plan and retains any package-owned bridge it
needs; later changing or closing the source view does not invalidate the
derived plan.

{pstd}{bf:Copying a view.} {cmd:parqit use} [{it:varlist}] {cmd:using view:}{it:viewname}{cmd:,}
{opt name(newview)} opens {it:newview} as a copy of the plan of {it:viewname} as it stands and
makes it current; a {it:varlist} keeps only those variables, as {cmd:parqit keep} would. No rows
are read. The copy is a plan, not data: later verbs on either view do not reach the other,
closing either leaves the other usable, and both read the same files when they execute. A
{cmd:parqit sample} step keeps its seed, so both views draw the same sample, and a pending
{cmd:keep in} range is checked when the copy is collected or saved. {cmd:parqit view}
{it:name}{cmd::} {it:command} still changes {it:name}; copy it first to try verbs without
changing it. {opt name()} is required and must differ from {it:viewname}; the options that
describe how to read a file are refused. Under a {cmd:parqit view} {it:name}{cmd::} prefix the
previously current view is restored afterwards, as for any prefixed command. {cmd:parqit views}
and {cmd:parqit show} name the source as {cmd:view:}{it:viewname} followed by that view's own
source, and a copy of a view that reads extended missing values as {cmd:.} repeats that note.
{cmd:parqit mergein} and {cmd:parqit appendin} read a file, not a view: give them a
{cmd:view:} source and they refuse it and name the out-of-core alternative.{p_end}

{pstd}
A typical first session: open the view, explore it engine-side
({help parqit_technical##explore:describe, head, summarize, tabulate, …} — bounded output
may be staged or displayed, but the current dataset is unchanged), then filter
and derive lazily, and {cmd:collect} only the result — explore first, load last.

{pstd}
{cmd:parqit collect} executes the pipeline once, using a direct read for a pure
source view and a spillable temporary table for a transformed result, then
loads the result atomically — your data is replaced only after the new data is
complete and valid — and the view {it:stays open}
for further exploration (collecting again re-executes). {cmd:parqit save}
executes the pipeline and writes Parquet directly, naming the view it
materialised; the current dataset is never touched. To export the {it:in-memory}
dataset while views are open, use {cmd:parqit save} {it:…}{cmd:, data}.
{cmd:parqit head} requests a small preview; a declared or restored sort order
may still require a scan to find its first rows. {cmd:parqit show} prints the generated SQL
(a readable CTE pipeline, one stage per verb); {cmd:parqit explain} prints
the engine's plan.


{marker reading}{...}
{title:Reading data}

{pstd}
{cmd:parqit use} in full: its forms, its sources and each of its options;
{cmd:mergein}/{cmd:appendin} read their file the same way.

{pstd}Open a lazy view (no result rows are loaded into Stata) or read a file
into memory:

{p 8 16 2}
{cmd:parqit} {cmdab:u:se} [{it:varlist-patterns}] {cmd:using} {it:filename} [{cmd:,} {opt clear} {opt n:ame(viewname)}
{opt relax:ed} {opt enc:oding(name)} {opt int64(refuse|round|string)} {opt binary(text|hex)}
{opt file:name(newvar)} {opt csv(read_options)} {opt obj:ect(name)}]

{p 8 16 2}
{cmd:parqit} {cmdab:u:se} {it:filename} [{cmd:,} {opt clear} {opt n:ame(viewname)} {opt relax:ed} {opt enc:oding(name)}
{opt int64(refuse|round|string)} {opt binary(text|hex)} {opt file:name(newvar)} {opt csv(read_options)}
{opt obj:ect(name)}]

{p 8 16 2}
{cmd:parqit} {cmdab:u:se} [{it:varlist-patterns}] {cmd:using view:}{it:viewname}{cmd:,} {opt n:ame(newview)}

{p 8 16 2}
{cmd:parqit} {cmdab:u:se} {cmd:view:}{it:viewname}{cmd:,} {opt n:ame(newview)}

{pstd}The second form is the first without a {it:varlist}: {cmd:using} may be
omitted only when no variable list is given, and the two forms are otherwise
identical — {opt clear} reads into memory, its absence opens a lazy view.

{pstd}With {cmd:view:}{it:viewname} as the source (lowercase {cmd:view:}, as for the
two-table sources), {cmd:parqit use} copies the plan of an open view into a new view
instead of reading a file; see {help parqit_technical##views:Copying a view}.

{pstd}{it:filename} may be a Parquet file, a glob such as {it:data_*.parquet}
(wildcards are {cmd:*} and {cmd:?}; a {cmd:[} is a literal character, and a
filename that exists is always read as itself, never as a pattern),
a Hive-partitioned directory, a delimited-text file ({cmd:.csv}, {cmd:.tsv},
{cmd:.txt} or {cmd:.tab}), a Stata {cmd:.dta} / Excel {cmd:.xls}/{cmd:.xlsx} file, an
SPSS system file ({cmd:.sav} or {cmd:.zsav}; see {help parqit_technical##spss:SPSS files}), or an
R data file ({cmd:.rds}, {cmd:.rda} or {cmd:.RData}; see {help parqit_technical##rdata:R data files}) — see
{help parqit_technical##formats:Input formats}. Without {opt clear} a lazy view opens over
the file(s), replaces any existing view with the same name and becomes current;
the current in-memory dataset is unchanged. With {opt clear} the whole result is
read into memory atomically and every open view is left untouched; {opt name()}
is then invalid.
{opt relaxed} reads a glob whose files have {it:different} schemas by union of
column names (columns absent from a file arrive missing); without it a schema
mismatch across the matched files is a loud error. {opt encoding(name)} names
the encoding of text that is not UTF-8, in any language: of delimited text
(read as UTF-8 without it; a UTF-16 byte-order mark is recognised), of a
{cmd:.dta}/Excel source that must be bridged to Parquet (by default the
session code page, windows-1252 unless {cmd:parqit set encoding} named
another), for an SPSS file the code page that replaces the one the file
declares, and for an R data file the encoding of the strings R did not mark.
Any encoding parqit reads may be named: UTF-8, the Windows, ISO-8859, KOI8,
DOS and Mac code pages, Shift_JIS, EUC-JP, GBK, GB18030, Big5, EUC-KR and
UTF-16 (see {help parqit_technical##encoding:Text encodings}); {opt encoding(name, all)}
decodes from {it:name} also text that happens to be valid UTF-8. It is
ignored, with a note, for a Parquet source (UTF-8 by definition).
{opt object(name)} names the data frame to read from an R data file that holds
several: an {cmd:.RData} saved with several objects, or an {cmd:.rds} holding a
named list (see {help parqit_technical##rdata:R data files}); it is an error on any other source.

{pstd}{opt int64(refuse|round|string)} says what to do with a column whose
integers go beyond 2^53, outside the consecutively exact integer range of a
Stata {cmd:double}. The protection covers signed and unsigned 64-bit and 128-bit integers
and wide decimals. It is conservative, even if a particular larger integer
happens to be representable exactly.
{cmd:refuse} (the default) stops the read with {cmd:r(198)}, naming every such
column; nothing is staged and the data in memory is untouched. {cmd:round}
accepts the nearest double and says so per column — two distinct keys can then
be one observation. {cmd:string} loads only the affected columns as exact
decimal text (sized to the widest value; other columns are untouched).
On an eager read, preserved extended missings become the text {cmd:.a}-{cmd:.z}
in those converted columns; ordinary missing values become empty strings.
Companion codes are validated even when the primary column becomes text.
{cmd:parqit set int64} moves the default for the session; an option on the
command always wins. On a lazy {cmd:parqit use using} the value is carried by
the view and applied by {cmd:parqit collect}.
{cmd:parqit mergein} and {cmd:parqit appendin} read their disk side through
{cmd:parqit use} and take the same option. Merge keys must have compatible
types on both sides: {cmd:int64(string)} works with a matching string key in
memory, while a numeric master key and a using key converted to text give
native {cmd:r(106)}. {cmd:int64(round)} keeps a numeric key but may collapse
distinct values into a non-unique key ({cmd:r(459)}). Text conversion also
works for non-key payload/{opt keepus:ing()} columns.

{pstd}{opt binary(text|hex)} loads a Parquet {cmd:BINARY}/{cmd:BLOB} column,
which is otherwise dropped with a message (raw bytes have no Stata type).
{cmd:text} decodes the bytes as UTF-8 and refuses the read loudly, naming the
column, if any row is not valid UTF-8; {cmd:hex} loads two UPPERCASE hex digits
per byte, which always works. Either way the column arrives as text sized to
the widest value ({cmd:strL} beyond 2045 bytes), with a note naming the mode.
A lazy view's columns are decided when it is opened, so give the option to
{cmd:parqit use using}, not to {cmd:parqit collect}.

{pstd}{opt filename(newvar)} adds one string variable holding the path each
observation was read from — the path {it:as matched}, so an absolute pattern
gives absolute paths and a relative one gives relative paths. It is an ordinary
variable: lazy verbs can filter on it, {cmd:collect} and {cmd:save} carry it,
and a saved file holds it as plain text. It is added even when a
{it:varlist} selects only some columns; naming it in the {it:varlist} places it
where you put it. The name must not be one the source already loads (including
a Hive partition key) — that refuses, naming the clash — and a {cmd:.dta},
Excel, SPSS or R source refuses too, because such a source is read through a temporary
Parquet bridge whose path says nothing about your file. Delimited text that needs
decoding also refuses; a file already valid UTF-8 stays in place with a named
legacy encoding unless {cmd:all} is requested, and supports {opt filename()}.

{pstd}{opt csv(read_options)} hands the reader the dialect and the types of a
delimited-text source instead of letting it infer them; it is an error on any
other source. {it:read_options} are:

{p2colset 9 30 32 2}{...}
{p2col:{opt delim("c")}}field separator ({cmd:"\t"} is a tab){p_end}
{p2col:{opt quote("c")}}quote character{p_end}
{p2col:{opt esc:ape("c")}}escape character{p_end}
{p2col:{opt head:er(on|off)}}first line is names, or data{p_end}
{p2col:{opt date:format("...")}}date format, e.g. {cmd:"%d/%m/%Y"}{p_end}
{p2col:{opt timestamp:format("...")}}timestamp format{p_end}
{p2col:{opt sample(#)}}rows to sniff for types; {cmd:-1} = the whole file{p_end}
{p2col:{opt allvar:char}}no type inference: every column is text{p_end}
{p2col:{opt types(name:TYPE ...)}}force these columns' types ({cmd:VARCHAR},
{cmd:BIGINT}, {cmd:DOUBLE}, {cmd:DATE}, ...){p_end}
{p2col:{opt nullstr("...")}}text that means missing{p_end}
{p2colreset}{...}

{pstd}Use {opt types()} or {opt allvarchar} whenever inferred types would
change a value — an identifier written as {cmd:1e5}, a decimal with more digits
than a double holds, or a code with leading zeros is safest read as
{cmd:VARCHAR}. The names in {opt types()} are the file's own header names;
neither a name nor a type spelling may contain a space ({cmd:DECIMAL(18,2)}
works, {cmd:DECIMAL(18, 2)} does not), and a type the engine does not know is
refused by name before anything is read. {cmd:csv()} written with nothing in it
is treated as not given at all, as Stata treats every empty option argument.
{opt header(off)} means there are no names to recover, so the columns load
under the engine's {cmd:column0}, {cmd:column1}, ... names.

{pstd}{cmd:mergein}/{cmd:appendin} join the data {it:already in Stata's memory}
with a disk {it:file} via a {it:native} {help merge} / {help append}: the
in-memory dataset stays put (no DuckDB round-trip), and parqit reads only the
needed columns of the disk side. This is the fast route when the disk side is a
{it:small lookup}; for big-on-big use the out-of-core
{cmd:parqit use} + {cmd:parqit merge} path instead. {it:merge_options} belong to
{cmd:mergein} alone and are the native ones ({opt keepus:ing()}, {opt keep()},
{opt gen:erate()}, {opt nogen:erate},
{opt update}, {opt replace}, {opt assert()}, {opt force}, {opt nol:abel},
{opt non:otes}, {opt norep:ort}), forwarded verbatim to native {helpb merge};
{cmd:appendin} forwards {opt keep()}, {opt generate()} and {opt force} to native {helpb append}.
Lazy {cmd:parqit merge} is not a wrapper around native {cmd:merge} and takes
only the options shown in its own syntax line; any other native
{cmd:merge} option is rejected.

{pstd}where each {it:source} is any supported disk input (Parquet file, glob or
Hive directory; delimited text; Stata; Excel; SPSS; or R) or
{cmd:view:}{it:viewname} — another open view whose plan is embedded without
materialising either view. Non-Parquet file sources follow the adapter rules
in {help parqit_technical##formats:Input formats}.


{marker formats}{...}
{title:Input formats}

{pstd}
The engine scans {bf:Parquet} and {bf:delimited text} ({cmd:.csv}, {cmd:.tsv},
{cmd:.txt} or {cmd:.tab}) directly on disk when they are the main
{cmd:parqit use} source — both are read {it:out of core}, so a file may be
far larger than memory. Parquet can project columns and prune row groups;
delimited text must still be parsed as a stream and has no Parquet row-group
pruning. {bf:Stata} ({cmd:.dta}) and {bf:Excel} ({cmd:.xls}/{cmd:.xlsx}) inputs
are {it:not} engine-scannable, so parqit imports them into a throwaway frame —
your working dataset is left untouched — and snapshots them to a temporary Parquet
{it:bridge} the engine then scans; their variable/value labels and formats ride
along. parqit picks the path by the final file extension, case-insensitively.
On the {cmd:using} side of {cmd:merge}/{cmd:joinby}/{cmd:append}, Parquet stays
on disk, while delimited text, {cmd:.dta} and Excel are first imported to a
package-owned Parquet bridge (SPSS and R files are converted to one, as
described below); this keeps the engine's two-table input contract
uniform and is intended for a comparatively small using side.
The delimited using-side adapter uses Stata's {cmd:import delimited}, whose
type inference can differ from {cmd:read_csv_auto} on the main side (for
example, date text versus a parsed date). It reads the file as UTF-8 — after
parqit has decoded it when it is not (CSV-ENC-1), never with the encoding
{cmd:import delimited} would guess, which for Cyrillic, Chinese or Japanese
text is wrong without a word (observed: no observations, or mojibake). Excel uses the first worksheet and
its first row as variable names; there are no sheet/range/header options.
To retain the main-source CSV inference for a join, open that CSV as its own
named view and refer to {cmd:view:}{it:name}.
Delimited-text header names get the same treatment as Parquet column names
(see {it:Column names} under {help parqit_technical##types:Types and metadata}): a
repeated name keeps the engine's numbered form ({cmd:a_1}, with
{cmd:char a_1[src_name]} holding the original), a name that differs only by
case from another is exact in Stata (an alias inside the lazy view, with a
note), an empty header cell becomes {cmd:v}{it:#}, and a file without a header
keeps the engine's {cmd:column0}, {cmd:column1}, … names.

{pstd}
{bf:SPSS} system files ({cmd:.sav}, {cmd:.zsav}) are read by parqit's own reader
and never through a frame. The plugin reads the dictionary — every record of
the format, in either byte order — and decodes the data once (uncompressed,
bytecode or ZLIB) to check the whole file and settle what only the data can
settle: whether a date variable holds whole days, whether a time fits in a
day, the values observed inside a user-missing range, the widest string. It
then streams the cases through an internal DuckDB table function into the
verified Parquet writer, one engine vector at a time, and checks before
publishing that the rows written equal the cases counted and that the source
file did not change meanwhile. A malformed file is refused naming the record
or case and the byte offset; a truncated one is refused before any output
exists. The Parquet columns carry the SPSS names in dictionary order, followed
by the {cmd:_parqit_xm_}{it:name} companions of the variables whose
user-missing values occur; the characteristics are recorded under the Stata
names parqit's reader gives those columns. {cmd:parqit save} {it:file}
{cmd:using} {it:x.sav} writes that file; every other command that reads a
{cmd:.sav}/{cmd:.zsav} writes it as a package-owned bridge and reads it like
any bridge. The mapping, the user-missing codes and the encoding rules are in
{help parqit_technical##spss:SPSS files}. A string variable's SPSS value labels, which
Stata cannot attach to a string, stay in {cmd:char}
{it:var}{cmd:[spss_value_labels]}; {cmd:parqit spssencode} reads them with a
strict parser of the JSON that parqit writes, with no Python, and builds the
labelled numeric version of the variable.

{pstd}
{bf:R data files} ({cmd:.rds}, {cmd:.rda}, {cmd:.RData}) are read by parqit's own
reader of R's serialization format (R's {cmd:src/main/serialize.c}, versions 2
and 3, XDR), with no R. A gzip- or zstd-compressed file is first inflated into
a temporary file (each gzip member's CRC-32 and length checked), removed with
the conversion. A first pass parses the whole structure: attributes are loaded,
while every vector that may be a data frame column is only located — its
length and the offset of its first element — so the data are read later,
column by column, each column with its own cursor and buffer. Objects that are
not data are passed over without being evaluated or recursed into: a function
(byte-compiled or not), an environment that refers to itself, an external
pointer. R's references are followed through the file's own reference table,
and a malformed or truncated file is refused naming the byte offset. A profile
pass then reads every column once, to settle what only the data can settle —
a date holding a fraction of a day, a time beyond a day, the values inside a
user-missing range, the tagged missing values present, the widest string —
and an internal DuckDB table function streams the rows into the verified
Parquet writer, which checks the rows written and that the source file did
not change. ALTREP vectors R writes compactly are expanded as R would: integer
and real sequences, sorted/no-NA wrappers and deferred {cmd:as.character()} of
integers. Column names that repeat, or differ only by case, are written exactly
(footer rename, as for {cmd:parqit save}), after an empty or repeated name is
made unique. The mapping and the codes are in {help parqit_technical##rdata:R data files}.

{pstd}
The delimited-text dialect and column types are inferred from a sample of the
file. Inference is a guess, and a wrong guess changes values silently: text
written as {cmd:1e5} becomes the number 100000, a decimal with more digits than
a double holds is rounded, and {cmd:TRUE} becomes 1.
{opt csv()} on {cmd:parqit use} replaces the guess with what you know — the
dialect ({opt delim()}, {opt quote()}, {opt escape()}, {opt header()}), the
temporal formats ({opt dateformat()}, {opt timestampformat()}), the sniffing
sample ({opt sample()}), the missing-value text ({opt nullstr()}) and the
column types ({opt types()}, or {opt allvarchar} for "everything is text").
Only the names on the whitelist are accepted; anything else is refused by
name. A {opt types()} type is checked for shape and then proved against the
engine with one cast of a missing value, before any scan is built, so an
unknown type is parqit's own message naming the type and the column — never
the binder's error with its dump of the query. A
forced dialect drives the header-name recovery above as well as the scan, so
the names come back from the same split the data does; with {opt header(off)}
there is no header line to recover from. {opt csv()} applies to the main
{cmd:parqit use} source only — a {cmd:using}-side CSV is bridged through
{cmd:import delimited}, which has its own options.

{pstd}
{opt filename(newvar)} on {cmd:parqit use} adds the engine's provenance column:
one string variable holding the path each row was read from, as matched. In the
scan it sits after the files' own columns and before any Hive partition keys,
but it is not one of them — it takes no {cmd:parqit.*} metadata, is never
mistaken for a partition key, and carries a note saying what it is. A name the
source already loads is refused rather than quietly renamed, and a
{cmd:.dta}/Excel/SPSS/R source, or delimited text decoded from another
encoding, is refused because what it would report is the temporary bridge.

{pstd}
Because a bridge {it:is} a {cmd:parqit save} of the imported frame, the
write-side conversions apply to it and are now reported: extended missings
{cmd:.a}-{cmd:.z} collapse to {cmd:.}, fractional date/period counts round, and
legacy text is transcoded from the session code page ({cmd:windows-1252}
unless {cmd:parqit set encoding}; see {it:String encoding} under
Materialisers). The command that created the bridge prints those losses through a
{cmd:note:} naming the bridged file and returns them in
{cmd:r(ext_missing)}, {cmd:r(frac_dates)}, {cmd:r(transcoded_vars)},
{cmd:r(transcoded_cells)}, {cmd:r(transcoded_meta)}, {cmd:r(transcoded_revalid)},
{cmd:r(undecodable)}, {cmd:r(encoding_default)} and {cmd:r(encoding)} —
{cmd:parqit use} (lazy and eager), {cmd:merge}/{cmd:joinby}/{cmd:append} and
{cmd:open _data} alike. Choose another code page for a {cmd:.dta}/Excel bridge
with {opt encoding(name)} on any of those commands (any encoding parqit reads:
a Cyrillic, Greek, Chinese or Japanese {cmd:.dta} as much as a Latin-9 one).
With a named text encoding, checking UTF-8 may scan the entire file before
opening a view; a wholly UTF-8 file stays in place unless {cmd:all} is requested.
Delimited text that is not UTF-8 — by its byte-order mark, the patterns of
UTF-16 or {opt encoding()} — is decoded first into a package-owned UTF-8 copy,
under the file's own name and the {it:key}{cmd:=}{it:value} directories of its
path, that the engine scans (or {cmd:import delimited} reads, on the using
side) instead; see {help parqit_technical##encoding:Text encodings}. Every bridge lives in
the temporary directory ({cmd:c(tmpdir)}); if its path contains {cmd:=}, the
engine reads such a directory as a Hive partition column of the bridged data,
and parqit says so once a session — point TMPDIR (STATATMP on Windows) at a
directory without {cmd:=}.

{pstd}
{bf:When does the bridge make sense?} For a {it:small} side — a lookup
{cmd:.dta}, a hand-made {cmd:.xlsx} — it is ideal: the cost is one quick import.
A {it:large} {cmd:.dta} master gains nothing from it (you would have read the
whole file into Stata either way), so for that prefer Stata's {cmd:use} followed
by {cmd:parqit open _data}. That command writes one temporary Parquet snapshot
of the in-memory dataset and opens a lazy view over it; it does not clear or
otherwise change the in-memory dataset. The plugin atomically reserves every bridge, so concurrent Stata
processes sharing a temp directory cannot choose the same path. A failed
operation removes its package-owned bridge; after success, the bridge lives
until the last view whose plan references it is closed or replaced. A view
copied with {cmd:parqit use} ... {cmd:using view:}{it:name} references every
bridge of its source, so either view can be closed first.
{cmd:parqit close _all} remains the final package-owned cleanup sweep.

{pstd}
This is exactly the shape that keeps a large master {it:out of} Stata while a
small file joins in — only the result is collected:

{phang2}{cmd:. parqit use using big.parquet}{space 22}({it:master view; schema probed, no rows loaded}){p_end}
{phang2}{cmd:. parqit merge m:1 id using lookup.dta, keepusing(rate)}{space 3}({it:.dta bridged in}){p_end}
{phang2}{cmd:. parqit collect, clear}{space 27}({it:only the merged result replaces the current dataset}){p_end}

{pstd}
A delimited file is scanned with DuckDB's {cmd:read_csv_auto} (schema and
delimiter auto-detected); add {opt relaxed} to {cmd:parqit use} to union a glob
whose files have different schemas. (SAS files are out of scope — parqit reads
Parquet, delimited text, Stata, Excel, SPSS and R data files.)

{pstd}
{cmd:parqit describe} {it:source} / {cmd:glimpse} {it:source} is deliberately a
{bf:Parquet-only} footer inspection (file, glob or Hive directory): it does not
invoke the CSV, Stata, Excel, SPSS or R adapters. From the parqit metadata in that
footer it also shows each variable's value label and variable label, marks the
variables with notes and those whose SPSS or R labels are kept in characteristics,
and lists value labels and notes with its {opt labels} and {opt notes} options
(see {help parqit_technical##explore:parqit describe}). With no source argument it instead
describes the open view's carried schema and pipeline depth. A mixed-schema
Parquet glob is refused rather than displaying the first file as if it
represented the set; open it with {cmd:parqit use ..., relaxed} to inspect the
unioned view.


{marker spss}{...}
{title:SPSS files}

{pstd}parqit reads SPSS system files — {cmd:.sav}, uncompressed or
bytecode-compressed, and ZLIB-compressed {cmd:.zsav} — with its own reader and
out of core: the file is decoded straight into Parquet and never passes through
a Stata frame. To write the corresponding Parquet file, with every SPSS property
kept:

{phang2}{cmd:. parqit save survey.parquet using survey.sav, replace}{p_end}

{pstd}The whole file is decoded and checked first — a truncated or corrupt file
is refused, naming the case or record, before anything is written — then
written through the same staged, verified writer as every {cmd:parqit save}.
The dataset in memory and the open views stay as they were; {opt replace},
{opt compression()}, {opt compression_level()} and {opt encoding()} work as for
any save, and options that describe other saves ({opt data}, {opt xmissing},
{opt partition_by()}, ...) are refused by name. A {cmd:.sav}/{cmd:.zsav} is also
accepted wherever parqit reads a file — {cmd:parqit use} (lazy or with
{opt clear}), the {cmd:using} side of {cmd:merge}/{cmd:joinby}/{cmd:append}, and
{cmd:mergein}/{cmd:appendin}: parqit converts it the same way into a
package-owned temporary Parquet file (a bridge) and reads that.

{pstd}{bf:What is kept, and where.}

{p2colset 5 30 32 2}{...}
{p2col:{it:SPSS}}{it:Parquet file, and Stata after} {cmd:parqit use}{p_end}
{p2line}
{p2col:variable names}the SPSS names are the column names; a name Stata cannot
hold (longer than 32 characters, or with {cmd:.} {cmd:@} {cmd:#} {cmd:$}) loads
under a sanitised name, the SPSS name in {cmd:char} {it:var}{cmd:[src_name]}{p_end}
{p2col:numbers}{cmd:DOUBLE}; system-missing is missing{p_end}
{p2col:user-missing values}extended missing values {cmd:.a}-{cmd:.z} (below){p_end}
{p2col:strings}text, the blank padding removed; beyond 2045 bytes a {cmd:strL}{p_end}
{p2col:dates}{cmd:DATE}, {cmd:%td} with the SPSS look ({cmd:%tdDD-Mon-CCYY},
{cmd:%tdNN/DD/CCYY}, {cmd:%tdMon_CCYY}, {cmd:%tdq_!Q_CCYY}, ...); a date that holds a
time of day becomes a {cmd:TIMESTAMP} ({cmd:%tc}, same look, with a note){p_end}
{p2col:date-times}{cmd:TIMESTAMP}, {cmd:%tcDD-Mon-CCYY_HH:MM:SS} (DATETIME) or
{cmd:%tcCCYY-NN-DD_HH:MM:SS} (YMDHMS){p_end}
{p2col:times}{cmd:TIME} ({cmd:%tcHH:MM:SS}) when every value lies within a day;
{cmd:DTIME} and longer durations as seconds{p_end}
{p2col:display formats}{cmd:F} {it:w.d} as {cmd:%}{it:w.d}{cmd:f}, {cmd:COMMA}/{cmd:DOLLAR} as
{cmd:%}{it:w.d}{cmd:fc}, {cmd:DOT} as {cmd:%}{it:w}{cmd:,}{it:d}{cmd:fc}, {cmd:E} as
{cmd:%}{it:w.d}{cmd:e}, {cmd:N} as {cmd:%0}{it:w}{cmd:.0f}; the SPSS format itself in
{cmd:char} {it:var}{cmd:[spss_format]}{p_end}
{p2col:variable labels}variable labels; Stata keeps 80 characters, a longer
label is kept whole in {cmd:char} {it:var}{cmd:[spss_label]}{p_end}
{p2col:value labels}a value label named after the variable, with every integer
key; what Stata cannot hold (labels of strings, of non-integer values, of
dates) whole in {cmd:char} {it:var}{cmd:[spss_value_labels]} as JSON pairs,
which {cmd:parqit spssencode} turns into a labelled numeric variable (see
{bf:String codes} below){p_end}
{p2col:file label, documents}dataset label; notes on {cmd:_dta}{p_end}
{p2col:measure, width, ...}{cmd:char} {it:var}{cmd:[spss_measure]},
{cmd:[spss_display_width]}, {cmd:[spss_alignment]}, {cmd:[spss_role]},
{cmd:[spss_attributes]}{p_end}
{p2col:file properties}{cmd:char _dta[spss_weight]}, {cmd:[spss_encoding]},
{cmd:[spss_product]}, {cmd:[spss_creation]}, {cmd:[spss_attributes]},
{cmd:[spss_mrsets]} (multiple-response sets), {cmd:[spss_varsets]}{p_end}
{p2line}
{p2colreset}{...}

{pstd}{bf:User-missing values.} Every user-missing value becomes an extended
missing value, so it is missing in every Stata computation and keeps its
identity. The codes are assigned per variable in a fixed order: the discrete
missing values (ascending), then the labelled values inside the missing range
(ascending) — both taken from the SPSS dictionary, so a survey series with the
same definitions gets the same codes in every file — then any other value
observed inside the range (ascending). {cmd:char} {it:var}{cmd:[spss_missing]}
holds the SPSS definition (e.g. {cmd:LO THRU -1, 99}) and
{it:var}{cmd:[spss_missing_map]} the codes (e.g. {cmd:.a=99 .b=-9 .c=-8}); the
SPSS labels of those values are attached to their codes as well. In the Parquet
file the cell is null — every Parquet reader sees a missing value — and its code
sits in a companion column (the {help parqit_technical##materialisers:xmissing} layout),
which {cmd:parqit use}, {cmd:mergein} and {cmd:appendin} restore. A variable
with more than 26 distinct user-missing values gives the 26th and later ones a
shared {cmd:.z}, and says so. A lazy view reads the codes as plain {cmd:.} and
says so: to write a Parquet file that keeps them, convert the SPSS file with
{cmd:parqit save} ... {cmd:using}, not by saving a lazy view. String
user-missing values stay text (Stata has no missing strings), with their
definition in {cmd:char} {it:var}{cmd:[spss_missing]}. Before appending files
converted one by one, compare their {cmd:spss_missing_map}: a value observed in
only some files can get a different code in each.

{pstd}{bf:String codes.} Stata cannot attach a value label to a string, so a
string variable keeps its SPSS value labels in {cmd:char}
{it:var}{cmd:[spss_value_labels]}. With the data in memory,
{cmd:parqit spssencode} {it:var}{cmd:, generate(}{it:newvar}{cmd:)} creates the
labelled numeric version. When every SPSS code is a distinct integer, the
values are the codes themselves, as {cmd:destring} would give; otherwise the
codes are numbered 1, 2, ... in code order, as {cmd:encode} does, and values
the dictionary does not label follow, labelled by their own text.
{opt sequential} asks for that numbering even for integer codes. The
dictionary, not the data, chooses the numbering, so files that share a
dictionary share codes. SPSS user-missing codes become {cmd:.a}, {cmd:.b}, ...
with their labels, recorded in {cmd:char} {it:newvar}{cmd:[spss_missing_map]}.
The value label is named {it:newvar} unless {opt label()} names another; the
variable label is copied. The dictionary and the data are checked first:
integer codes mixed with other values, an existing label or a malformed
characteristic are refused before anything is created.

{phang2}{cmd:. parqit use using survey.parquet, clear}{p_end}
{phang2}{cmd:. parqit spssencode region, generate(region_num)}{p_end}
{phang2}{cmd:. tabulate region_num}{space 30}({it:labelled with the SPSS labels}){p_end}

{pstd}{cmd:parqit describe} {it:file}{cmd:.parquet} marks such variables with
{cmd:(spss)} ({cmd:(r)} for the labels of an R file), and its {opt labels}
option lists them without loading the file.

{pstd}{bf:Character encoding.} Text is decoded from the encoding the file
declares, by name (record 7, subtype 20) or code page number: UTF-8 or any code
page parqit reads (see {help parqit_technical##encoding:Text encodings}) — Windows 874 and
1250-1258, ISO-8859, KOI8, DOS and Mac, Shift_JIS, GBK, UHC, Big5, EUC-JP,
GB18030; a file that declares EBCDIC, UTF-16 or a code page parqit does not read
is refused with a message. A file without a declaration is decoded in the
session code page (windows-1252 unless {cmd:parqit set encoding}), with a note.
{opt encoding()} replaces a missing or wrong declaration. In a UTF-8 file, text
that is not valid UTF-8 is transcoded from the session code page (or the
{opt encoding()} one), item by item, and a note counts it; bytes the code page
does not define become U+FFFD, counted.

{pstd}{bf:Not read.} SPSS portable files ({cmd:.por}), encrypted
(password-protected) files, and EBCDIC or non-IEEE files are refused with a
message. The Data Editor's view settings and the obsolete date information
records are not carried, and the conversion says so. Compared with Stata's
{helpb import spss}, which also reads these files: user-missing values stay
distinct ({cmd:.a}-{cmd:.z}, not {cmd:.}), string user-missing values are kept,
dates are {cmd:%td} days rather than {cmd:%tc} milliseconds, a time of day is
counted from 01jan1960, and value labels Stata cannot hold are kept in
characteristics instead of being dropped. {cmd:parqit describe} reads Parquet
footers only; describe the converted file or a view opened over the SPSS file.
On the converted file it marks the string variables that carry SPSS labels,
and its {opt labels} option lists them.


{marker rdata}{...}
{title:R data files}

{pstd}parqit reads R data files — {cmd:.rds} (written by {cmd:saveRDS()}) and
{cmd:.rda}/{cmd:.RData} (written by {cmd:save()}) — with its own reader of R's
serialization format, out of core and without R: the data frame is decoded
straight into Parquet and never passes through a Stata frame. To write the
corresponding Parquet file, with every R property kept:

{phang2}{cmd:. parqit save households.parquet using households.rds, replace}{p_end}
{phang2}{cmd:. parqit save people.parquet using workspace.RData, replace object(people)}{p_end}

{pstd}As for an SPSS file, the whole file is parsed and every column read first,
so a truncated or corrupt file is refused before anything is written, and the
dataset in memory and the open views are untouched. {cmd:parqit save}
{it:file} {cmd:using} {it:Rfile} takes {opt replace}, {opt compression()},
{opt compression_level()}, {opt encoding()} and {opt object()}. An R data file is
also accepted wherever parqit reads a file — {cmd:parqit use} (lazy or with
{opt clear}), the {cmd:using} side of {cmd:merge}/{cmd:joinby}/{cmd:append}, and
{cmd:mergein}/{cmd:appendin} — through a temporary Parquet bridge.

{pstd}{bf:Which data frame.} An {cmd:.rds} holds one object: a data frame, or a
list whose data frames can be read by name. An {cmd:.RData} holds named objects;
functions (byte-compiled or not), environments and other objects are passed
over, never run. When the file holds exactly one data frame, that one is read,
with a note naming it when there are other objects; when it holds several,
{opt object(name)} names one (names are case-sensitive), and without it the
command stops, listing them. {opt object()} is an option of {cmd:parqit use} and
{cmd:parqit save ... using}; for the other commands, convert the file first.
Unnamed list elements use {cmd:[[1]]}, {cmd:[[2]]}, ...; a suffix is added when
that spelling is already a real name. Real names retain their own elements.

{pstd}{bf:Formats.} R's binary (XDR) serialization, versions 2 and 3, is read
uncompressed or compressed with gzip (R's default) or zstd. Files compressed
with bzip2 or xz, and files saved in R's ASCII format ({cmd:ascii = TRUE}), are
refused with a message: re-save them in R with {cmd:saveRDS(x, file)} or
{cmd:save(x, file = ...)}. (The data sets of R packages are often compressed
with xz or bzip2, because CRAN keeps whichever compresses best.) A compressed file is first decompressed into a
temporary file in {cmd:c(tmpdir)}, so that much free space is needed there.

{pstd}{bf:What is kept, and where.}

{p2colset 5 30 32 2}{...}
{p2col:{it:R}}{it:Parquet file, and Stata after} {cmd:parqit use}{p_end}
{p2line}
{p2col:column names}the R names; a name Stata cannot hold loads under a sanitised
name (the Parquet name in {cmd:char} {it:var}{cmd:[src_name]}); an empty or
{cmd:NA} name becomes {cmd:V}{it:#} (its position) and a repeated name gets
{cmd:_1}, {cmd:_2}, ... (the R name in {cmd:char} {it:var}{cmd:[r_name]}){p_end}
{p2col:logical}{cmd:BOOLEAN}, loaded as a {cmd:byte} 0/1{p_end}
{p2col:integer}{cmd:INTEGER}; {cmd:NA} is missing{p_end}
{p2col:double}{cmd:DOUBLE}; {cmd:NA} is missing; {cmd:NaN} and {cmd:Inf} stay in
the Parquet file and load as {cmd:.}, with a note{p_end}
{p2col:integer64 (bit64)}{cmd:BIGINT}, exact; values beyond 2^53 load only with
{opt int64()}, as for any Parquet file{p_end}
{p2col:character}UTF-8 text; {cmd:NA_character_} is a null in the Parquet file,
which Stata loads as {cmd:""}; beyond 2045 bytes a {cmd:strL}{p_end}
{p2col:factor, ordered}the integer codes, with a value label of the levels; with
more than 65,536 levels, or a level longer than 32,000 bytes, the text (with a
note){p_end}
{p2col:Date, IDate}{cmd:DATE} ({cmd:%td}); a date holding a fraction of a day
becomes a {cmd:TIMESTAMP} ({cmd:%tc}), with a note{p_end}
{p2col:POSIXct}{cmd:TIMESTAMP} ({cmd:%tc}), the UTC clock time of the instant R
stores; the time zone R displayed it in, in {cmd:char} {it:var}{cmd:[r_tzone]},
with a note{p_end}
{p2col:hms, ITime}{cmd:TIME} ({cmd:%tcHH:MM:SS}) when every value remains within
a day after microsecond rounding, otherwise seconds{p_end}
{p2col:difftime}its number, the unit in {cmd:char} {it:var}{cmd:[r_units]}{p_end}
{p2col:label (haven)}the variable label; Stata keeps 80 characters, a longer
label is kept whole in {cmd:char} {it:var}{cmd:[r_label]}{p_end}
{p2col:labels (haven)}a value label with every integer key; what Stata cannot
hold (labels of text, of non-integer values) whole in {cmd:char}
{it:var}{cmd:[r_value_labels]} as JSON pairs, which {cmd:parqit spssencode}
turns into a labelled numeric variable{p_end}
{p2col:format.stata (haven)}a display format compatible with the converted units;
an incompatible format is kept in {cmd:r_attributes}, with a note{p_end}
{p2col:comment()}notes, on the variable or on {cmd:_dta}{p_end}
{p2col:the data frame's label}the dataset label{p_end}
{p2col:character row names}a first variable, {cmd:rowname}; integer row names
are not kept (a note says so when they are not 1 to N){p_end}
{p2col:other attributes}{cmd:char} {it:var}{cmd:[r_class]} and
{it:var}{cmd:[r_attributes]} (JSON); {cmd:char _dta[r_class]},
{cmd:[r_attributes]}, {cmd:[r_object]}, {cmd:[r_written_by]},
{cmd:[r_encoding]}{p_end}
{p2line}
{p2colreset}{...}

{pstd}A fractional {cmd:Date} uses {cmd:%tc} even if its original
{cmd:format.stata} was {cmd:%td}. Finite dates that coincide with the engine's
infinity sentinels are kept as numbers of days since 1970, with a note.
Times that round to 24:00 are kept as seconds.

{pstd}Other attributes exceeding 1,000 elements, 20 levels of JSON nesting,
or the parser's size/depth limits are summarized. A JSON characteristic over
60,000 bytes is reduced to the attribute names, with a note. These summaries
do not retain the full attribute values.

{pstd}{bf:Missing-value codes.} haven's tagged missing values
({cmd:tagged_na("a")}, as haven reads the {cmd:.a}-{cmd:.z} of a Stata or SAS
file) become the extended missing values of the same letter. SPSS
user-missing values carried by haven ({cmd:na_values}, {cmd:na_range}) become
extended missing values as for an SPSS file (see {help parqit_technical##spss:SPSS files}),
taking the letters the tagged values of that variable do not use;
{cmd:char} {it:var}{cmd:[r_missing]} holds the definition and
{it:var}{cmd:[r_missing_map]} the codes. Their labels are attached to their
codes. In the Parquet file the cell is null and its code sits in a companion
column, which {cmd:parqit use}, {cmd:mergein} and {cmd:appendin} restore; a lazy
view reads them as {cmd:.} and says so.

{pstd}{bf:Not carried.} Columns Stata has no type for — lists, nested data
frames, matrices, complex numbers, raw bytes, {cmd:POSIXlt} (convert it with
{cmd:as.POSIXct()} in R) and S4 objects — are left out, each named in a note, as
is a column that only its own package can expand (an ALTREP class of a package).
A character column that R still holds as the numbers of an {cmd:as.character()}
it has not carried out — R writes them as text only when they are used, with the
formatting of the computer that reads them — is stored as those numbers, with a
note and {cmd:char} {it:var}{cmd:[r_deferred]}; to keep R's text, run
{cmd:x <- paste0(x)} on the column in R before saving. The distinction between
{cmd:NA_character_} and {cmd:""} survives in the Parquet file only.

{pstd}{bf:Character encoding.} Each string is decoded by the encoding R marked
on it (UTF-8, latin1, bytes), and an unmarked one by the encoding R recorded
for its session (a version-3 file records it — any code page parqit reads, see
{help parqit_technical##encoding:Text encodings}; UTF-8 is assumed for a version-2 file).
{opt encoding()} replaces the recorded one. A string that is not valid in its
encoding is read in the session code page (windows-1252 unless
{cmd:parqit set encoding}) or the {opt encoding()} one, and a note counts it.

{pstd}{cmd:parqit describe} reads Parquet footers only; describe the converted
file, where {cmd:(r)} marks the variables whose labels are in {cmd:char}
{it:var}{cmd:[r_value_labels]}.


{marker encoding}{...}
{title:Text encodings}

{pstd}Parquet text is UTF-8, as is text in Stata 14 and later, and names,
values, labels, notes and characteristics in any script travel unchanged (no
normalisation: {cmd:é} and {cmd:e} followed by a combining accent stay
different). Text in another encoding, in any language, is decoded to UTF-8 on
its way in — never refused, never guessed in silence: a {cmd:note:} says what
was decoded, from what, and how much.

{pstd}{bf:The encodings.} {opt encoding()} and {cmd:parqit set encoding} take
any of these (case, {cmd:-}, {cmd:_}, {cmd:.} and blanks do not matter, and the
usual aliases work: {cmd:cp1251}, {cmd:Shift_JIS}, {cmd:sjis}, {cmd:GB2312},
{cmd:EUC-KR}, {cmd:UHC}, {cmd:Big5}, {cmd:latin2}, {cmd:ISO-8859-5}, ...):

{p2colset 9 32 34 2}{...}
{p2col:UTF-8}{cmd:utf-8}{p_end}
{p2col:Western European}{cmd:windows-1252} (the default), {cmd:latin1},
{cmd:latin9}, {cmd:macroman}, {cmd:ibm850}, {cmd:ibm437}, {cmd:ibm858},
{cmd:ibm860}, {cmd:ibm861}, {cmd:ibm863}, {cmd:ibm865}, {cmd:mac-iceland},
{cmd:iso-8859-3}, {cmd:iso-8859-10}, {cmd:iso-8859-14}{p_end}
{p2col:Central European}{cmd:windows-1250}, {cmd:iso-8859-2},
{cmd:iso-8859-16}, {cmd:ibm852}, {cmd:mac-centraleurope}{p_end}
{p2col:Cyrillic}{cmd:windows-1251}, {cmd:koi8-r}, {cmd:koi8-u},
{cmd:iso-8859-5}, {cmd:ibm866}, {cmd:ibm855}, {cmd:mac-cyrillic}{p_end}
{p2col:Greek}{cmd:windows-1253}, {cmd:iso-8859-7}, {cmd:ibm737},
{cmd:ibm869}, {cmd:mac-greek}{p_end}
{p2col:Turkish}{cmd:windows-1254}, {cmd:iso-8859-9}, {cmd:ibm857},
{cmd:mac-turkish}{p_end}
{p2col:Hebrew}{cmd:windows-1255}, {cmd:iso-8859-8}, {cmd:ibm862}{p_end}
{p2col:Arabic}{cmd:windows-1256}, {cmd:iso-8859-6}, {cmd:ibm864}{p_end}
{p2col:Baltic}{cmd:windows-1257}, {cmd:iso-8859-4}, {cmd:iso-8859-13},
{cmd:ibm775}{p_end}
{p2col:Vietnamese}{cmd:windows-1258}{p_end}
{p2col:Thai}{cmd:windows-874}, {cmd:iso-8859-11} ({cmd:tis-620}){p_end}
{p2col:Japanese}{cmd:shift_jis} (Windows code page 932), {cmd:euc-jp}{p_end}
{p2col:Chinese}{cmd:gbk} (936, {cmd:gb2312}), {cmd:gb18030},
{cmd:big5} (950: Windows' Big5, whose ETEN area C6A1-C8FE is read as Private
Use characters; not Big5-HKSCS){p_end}
{p2col:Korean}{cmd:euc-kr} (949, UHC){p_end}
{p2col:UTF-16}{cmd:utf-16le}, {cmd:utf-16be}, {cmd:utf-16} (delimited text only){p_end}
{p2colreset}{...}

{pstd}{cmd:r(encoding)} reports the canonical name ({cmd:windows-932},
{cmd:windows-936}, {cmd:windows-949} and {cmd:windows-950} for the four
Windows double-byte code pages). The code pages are read as Windows reads them
— the characters users defined in Shift_JIS, GBK, UHC and Big5 become the
Unicode Private Use characters Windows gives them — and GB18030 as its 2005
edition. The tests compare mappings with ICU and independent oracles, with
explicit vendor differences: CP864's byte 0x25 is Arabic {cmd:٪}; some ICU
variants give ASCII {cmd:%}. ISO-8859-16 and Mac Icelandic, absent from Stata's
ICU, are pinned by unit tests against published tables. A byte sequence that a
code page does not define becomes U+FFFD (the replacement character), counted
in {cmd:r(undecodable)} and said in a note.

{pstd}{bf:The dataset in memory, .dta and Excel files.} Text that is valid
UTF-8 is kept; any other text is decoded from the code page item by item — a
cell, a label, a note — as {helpb unicode:unicode translate} would, without
changing the data in memory. The code page is {opt encoding()}, or else the
session's ({cmd:windows-1252} unless {cmd:parqit set encoding} named another).
Text in a multibyte code page (Shift_JIS, EUC-JP, GBK, GB18030, Big5, EUC-KR)
is often valid UTF-8 by accident (GBK's {cmd:女} is also UTF-8's {cmd:Ů}), so
with such a code page a string variable with any text that is not UTF-8 is
decoded as a whole, and so are all labels, notes and characteristics once any
text is legacy; a note counts the cells that were valid UTF-8
({cmd:r(transcoded_revalid)}). {opt encoding(name, all)} decodes every text
that needs decoding, including CP864's byte 0x25 (Arabic {cmd:٪}), whose meaning
differs from ASCII. {opt copysource} refuses {opt encoding(name, all)}: omit
{opt copysource} to decode text through the normal memory writer.
A {cmd:.dta} file of format 117 or older (Stata 13 and
earlier, which predate Unicode) is read as {opt encoding(name, all)} whenever
{opt encoding()} names its code page. Stata decodes an Excel file's text
itself, so {opt encoding(name, all)} is not applied to it (a note says so).

{pstd}{bf:Delimited text.} A {cmd:.csv}, {cmd:.tsv}, {cmd:.txt} or
{cmd:.tab} file is read as UTF-8. Its byte-order mark is recognised (UTF-8 or
UTF-16; a mark, which the file itself declares, overrides a contradicting
{opt encoding()}, with a note), and so is UTF-16 without one — by the NUL byte
in every other byte of its ASCII text, or by its line ends when its text is in
another script. UTF-32 is refused; the NUL bytes of a fixed-width export's
padding are read as before (Stata cuts a cell at a NUL, with a note). An
{opt encoding()} given is followed even when the bytes look like UTF-16 or
UTF-32, and a note says what they looked like. A file valid UTF-8 throughout
stays in place with a named legacy encoding unless {cmd:all} is requested;
{opt filename()} continues to report its original path. Checking a named
encoding may scan the whole file before opening the view. Files needing decoding
are read through a temporary UTF-8 copy — line by line with
a single-byte code page (a line already valid UTF-8 is kept), all of it with a
multibyte one; decoded lines that were valid UTF-8 are counted.
Lines may end in LF, CRLF or CR alone. {opt encoding(utf-8)} reads a
UTF-8 file whose broken bytes then become U+FFFD. A glob ({cmd:data/*.csv},
{cmd:data/**/*.csv}) is decoded file by file, each by its own mark, and the copy
keeps the file names and the {it:key}{cmd:=}{it:value} directories that the
engine reads as Hive partition columns. Without {opt encoding()}, text that is
not UTF-8 in a file scanned in place ({cmd:parqit use}) is refused, and the
message names {opt encoding()}; the using file of {cmd:merge}, {cmd:joinby},
{cmd:append}, {cmd:mergein} or {cmd:appendin} is decoded from the session code
page, with a note — never from Stata's own guess of its encoding, which is
wrong for most scripts.

{pstd}{bf:SPSS and R files} declare their encoding; see
{help parqit_technical##spss:SPSS files} and {help parqit_technical##rdata:R data files}.
{bf:Parquet} text is UTF-8 by definition and is never transcoded.

{pstd}{bf:The default and the locale.} The default code page is
{cmd:windows-1252} wherever parqit runs, so a do-file gives the same result on
every computer; {cmd:parqit set encoding} {it:name} changes it for the session.
When text that declares no encoding was decoded from the default and the
locale ({cmd:c(locale_functions)}) suggests another code page, a note names it —
in a Russian locale, {cmd:encoding(windows-1251)}, or once per session
{cmd:parqit set encoding windows-1251}.

{pstd}{bf:Stata's limits.} A value-label text longer than Stata's 32,000 bytes,
a characteristic longer than 67,783 bytes and a variable or data label longer
than 80 characters are cut at a character boundary, never inside one.


{marker metadata}{...}
{title:Stata metadata in Parquet}

{pstd}
A file written by {cmd:parqit save} is an ordinary Parquet file: Python,
R, Spark, DuckDB and other readers see the data columns normally. Stata-only
metadata is stored in the Parquet footer as file-level key-value metadata.
The keys are {cmd:parqit.schema}, {cmd:parqit.vallabs}, {cmd:parqit.chars}
and {cmd:parqit.dtalabel}. {cmd:parqit.schema} carries Stata storage types,
display formats, variable labels, attached value-label names, original
source names and the dataset's sort-order marker
({cmd:sortedby}, restored on read as far as Stata accepts it);
{cmd:parqit.vallabs} carries the value-label definitions;
{cmd:parqit.chars} carries characteristics and notes; and
{cmd:parqit.dtalabel} carries the Stata data label. A file written with
{cmd:parqit save ..., xmissing} also carries {cmd:parqit.xmissing}, a JSON
object mapping each variable that held an extended missing value to its
companion column {cmd:_parqit_xm_}{it:var}: an {cmd:int8} data column with the
code of the cell (0 = not an extended missing, 1-26 = {cmd:.a}-{cmd:.z}; the
variable's own cell is null in both cases). Only variables that actually held
such a value get a companion. parqit's readers hide the companions and the
eager ones ({cmd:use}, {cmd:mergein}, {cmd:appendin}) restore the cells from
the codes; a companion whose code is outside 0-26, or that marks a cell
holding a value, makes the load fail rather than guess. Other readers see the
companions as ordinary columns.

{pstd}A column cannot be declared both as a primary and as a companion.
Sources with differing {cmd:parqit.*} metadata are refused when an
extended-missing channel is present: discarding it would change values,
not just labels or formats. This also applies to {opt relaxed} globs.
Read the files individually and combine them with {cmd:parqit appendin}.

{pstd}
Third-party readers usually do not apply Stata labels automatically. For
example, {cmd:pandas.read_parquet()} will read a labelled numeric variable as
its numeric codes; the label definitions remain available in the footer. In
Python, inspect them with {cmd:pyarrow}:

{phang2}{cmd:import json, pyarrow.parquet as pq}{p_end}
{phang2}{cmd:md = pq.read_metadata("file.parquet").metadata or dict()}{p_end}
{phang2}{cmd:schema = json.loads(md[b"parqit.schema"].decode())}{p_end}
{phang2}{cmd:vallabs = json.loads(md[b"parqit.vallabs"].decode())}{p_end}
{phang2}{cmd:chars = json.loads(md[b"parqit.chars"].decode())}{p_end}
{phang2}{cmd:dtalabel = json.loads(md[b"parqit.dtalabel"].decode())}{p_end}
{phang2}{cmd:xmissing = json.loads(md[b"parqit.xmissing"].decode()) if b"parqit.xmissing" in md else dict()}{p_end}

{pstd}
When the file is read back with {cmd:parqit use} or materialised with
{cmd:parqit collect}, parqit restores the metadata to Stata. Extended missing
categories {cmd:.a}-{cmd:.z} become plain missing values without {opt xmissing};
that opt-in preserves their codes in companion columns for eager reads.
Lazy views fold the categories to ordinary missing and announce it.
Their value-label definitions still survive
in {cmd:parqit.vallabs}. Value labels that are defined but attached to no
variable ({cmd:label define} orphans) are written and restored too, like native
{cmd:save}.

{pstd}
Restoration of presentation metadata is best-effort and loud: an item Stata cannot accept is skipped
or trimmed with a {cmd:note:} and never aborts the load — a display format Stata
rejects, a value-label name or key that is not a legal Stata name/integer,
value-label text over 32,000 bytes, a characteristic name that is not legal, a
characteristic value over Stata's 67,783-byte limit (truncated), or a note/char
whose variable is not in the result (dropped). Without an extended-missing
channel, a glob with {it:different} {cmd:parqit.*} metadata restores no
labels/formats and says so; with that channel it is refused, as described above.
Malformed JSON metadata is skipped with a note. The validated companion graph
and cell codes are value-integrity contracts and can refuse the load.
In {cmd:merge}/{cmd:append}/{cmd:joinby}
a value label defined differently on both sides keeps the master definition with
a note.


{marker verbs}{...}
{title:Verb contracts}

{pstd}Lazy verbs validate names and their stated contracts before changing the
plan; a refused verb leaves the current view usable at its previous state.
Binding checks on ordinary single-table operations do not replace the
data-dependent checks performed when the result runs.
No lazy verb changes Stata's in-memory dataset. {cmd:keep}/{cmd:drop} project
columns; {cmd:order} moves the requested columns to the front and retains the
relative order of the rest. Grouped {cmd:rename (oldlist) (newlist)} is one
atomic mapping, so equal-length lists may contain swaps such as
{cmd:(a b) (b a)}; labels, notes, characteristics and declared sort keys follow
the renamed column. {cmd:sort} is ascending, while {cmd:gsort} accepts a
{cmd:+}/{cmd:-} prefix per key. Sorting records plan order and is applied when
the plan runs; it does not scan the source when typed.

{pstd}{cmd:gen} accepts {cmd:byte int long float double str# strL};
{cmd:egen} accepts numeric storage types. The declared type is value semantics:
numeric narrowing truncates toward zero and makes out-of-range values missing,
and {cmd:str#} enforces its byte width. An untyped numeric {cmd:gen} result is
{cmd:double}; an untyped {cmd:egen} retains the aggregate's engine type until
materialisation, including exact integer or decimal totals. Use
{cmd:egen double} to request a double result. In {cmd:gen ... if}, observations
outside the qualifier receive missing; in {cmd:replace ... if}, they retain the
old value. {cmd:replace} preserves a contractual {cmd:float}/{cmd:double} storage
type when possible and otherwise re-infers it safely. {cmd:egen} functions are
{cmd:total mean sd min max count}, optionally within {opt by()}; these functions
are numeric, so a string result type is refused.

{pstd}{cmd:collapse} statistics: {cmd:mean sum sd count min max median}
{cmd:p}{it:##} {cmd:first last firstnm lastnm}. Percentiles follow Stata's
{cmd:summarize} rule exactly and are computed out of core, by rank, so a
huge group needs no in-memory list. {cmd:first}/{cmd:last} are deterministic over
the declared {cmd:parqit sort} order and keep a missing first value missing.
Weights ({cmd:[fweight=}{it:exp}{cmd:]}, …) are not supported on
{cmd:collapse}/{cmd:pivot} and are refused loudly.

{pstd}{cmd:collapse} counts nonmissing values; parqit also permits
{cmd:(count)} on a string and excludes both {cmd:""} and SQL NULL. {cmd:(sum)}
of an all-missing group is zero. A collapse without {opt by()} over an empty
view yields zero observations rather than fabricating one aggregate row.
{cmd:first}/{cmd:last} include missing; {cmd:firstnm}/{cmd:lastnm} skip it. When
no {cmd:parqit sort} was declared, the four order-sensitive statistics use a
reproducible total order over all columns; declare the intended sort whenever
"first" means the source's substantive order.

{pstd}{cmd:contract} produces one row per distinct key tuple, calls the frequency
variable {cmd:_freq} by default, accepts another noncolliding name through
{opt freq()}, and leaves the result ordered by the contracted keys. A
{cmd:contract} that would overwrite an existing {cmd:_freq} column is refused
(name it with {opt freq()}), matching native Stata's {cmd:r(110)}.

{pstd}{cmd:sample} draws an engine-side random sample: {it:#} is a
percentage in (0,100]; with {opt count}, {it:#} is a number of rows.
The count must be a nonnegative integer below 2^63 (zero is allowed). A
{opt seed(#)} from 0 to 2,147,483,647 makes the draw reproducible with unchanged
input order, engine and execution settings. The percentage form selects the nearest whole number
to N times the stored percentage divided by 100, with half ties rounded upward;
it computes that count globally. The count form uses a reservoir. If the seed is omitted
or negative, parqit chooses one when the sample step is added to the plan;
{cmd:parqit show} displays it. Re-executing the unchanged plan does not request
a fresh draw. Sampling remains lazy, and statistical commands that need
several passes use one realization of a sampled input.
Percentage sampling can require a source-sized engine temporary table and a
spillable sort; a small fixed-count sample is usually cheaper.

{pstd}{cmd:reshape long} requires {opt i()} to identify wide rows uniquely. For
each stub it discovers columns named {it:stub}{it:suffix}; if any suffix is
numeric, {opt j()} is numeric and nonnumeric prefix matches are carried as
ordinary columns, otherwise {opt j()} is string. Stubs must be balanced and
must not mix string and numeric source columns. Native Stata's leading-zero
rule is preserved: {cmd:inc01} signals that numeric {cmd:j=1} exists but is
carried as an ordinary column; {cmd:inc1}, when present, supplies the long
value, and otherwise that value is missing. Other columns are carried.
The long stubs and {opt j()} must not collide with carried columns or with
each other, either under the engine's case-insensitive identifiers or under
the exact Stata names exposed by aliases. A collision is refused before the
view changes; rename the conflicting column first.
{cmd:reshape wide} requires unique ({opt i()},{opt j()}) cells, refuses missing
{opt j()} values, and requires every other column to be an {opt i()} variable,
the {opt j()} variable or a listed stub. Generated {it:stub}{it:jvalue} names
must be valid, noncolliding Stata names; a generated name that differs only by
case from a live or another generated name ({cmd:x1} beside {cmd:X1}) is
refused loudly rather than written as a duplicate column (the engine cannot
hold both). Both wide reshape and {cmd:pivot}
refuse more than 2,000 distinct {opt j()}/{opt cols()} values. Successful
reshapes leave the result ordered by {opt i()} (and then {opt j()} for long).

{pstd}{cmd:pivot} is Excel's pivot table as one lazy verb: it aggregates
the {cmd:(}{it:stat}{cmd:)} specs by ({opt rows()}, {opt cols()}) — exactly
{cmd:collapse}'s statistics and contracts — and then spreads each distinct
{opt cols()} value into its own column ({cmd:reshape wide}), so the result
has one row per {opt rows()} combination and one column per {opt cols()}
value, named {it:tgt}{it:value} (e.g. {cmd:wage2019}, {cmd:nNorth}).
{opt rows()} accepts wildcards. Both stages appear in {cmd:parqit show},
and their contracts apply: a missing {opt cols()} value is a loud error
(as in native {cmd:reshape wide} — {cmd:parqit replace} or filter it
first), generated names must be valid variable names, and more than 2,000
distinct {opt cols()} values refuse to run. A refused pivot leaves the
view exactly as it was.

{pstd}Two-table {cmd:using} sources may be {cmd:view:}{it:name}: the other
view's pipeline is embedded as a subquery, so filtered-view-to-
filtered-view joins run in one out-of-core query. All contracts below
apply to view sources too (a view may even be merged with itself).

{pstd}{cmd:merge} validates the uniqueness contract of its kind up front
({cmd:m:1} requires unique keys in using, etc.) and produces a
Stata-compatible {cmd:_merge}; missing keys match missing keys, as in
Stata. Options: {opt keep(match master using)}, {opt keepus:ing(varlist)},
{opt gen:erate(name)}, {opt nogen:erate}. Lazy {cmd:parqit merge m:m} is
refused before importing a using-side adapter or changing the current view: a
lazy plan does not retain the physical within-key row order required by native
Stata's sequential reuse rule. Use {cmd:parqit joinby} for Cartesian
many-to-many matches, or {cmd:parqit mergein m:m} when native Stata's
order-dependent sequential behaviour is deliberately required.

{pstd}The default merge marker is {cmd:_merge}, with byte values and labels
1 master only, 2 using only and 3 matched; {opt generate()} renames it and
{opt nogenerate} omits it. Its name must be absent from the master and the
using columns retained in the result, including their exposed Stata names.
A collision is refused with {cmd:r(198)} before changing the view. Choose a
fresh {opt generate()} name, omit the marker, or exclude the conflicting
using column with {opt keepusing()}.
{opt keep()} accepts names or codes
({cmd:master}/{cmd:1}, {cmd:using}/{cmd:2}, {cmd:match}/{cmd:matched}/{cmd:3})
and repeated tokens do not change their meaning. {opt keepusing()} accepts
wildcards. A nonkey name present on both sides keeps the master column and
prints a note. Empty-string keys and numeric NULL, NaN, infinities and
magnitudes at or above 2^1023 are folded to the same ordinary missing key
before uniqueness tests and matching. The result
is ordered by the merge keys. Missing-key disclosure checks the using side
first. With no missing using keys, {cmd:merge m:1} and {cmd:joinby} need not
execute the master plan at this step; data-dependent master errors may then
surface when {cmd:collect}, {cmd:save} or an exploratory query executes it.
The uniqueness checks required by {cmd:merge 1:1} and {cmd:merge 1:m} still
execute the master before the merge is accepted.

{pstd}{cmd:append} accepts one or more file or {cmd:view:}{it:name} sources and
performs a union by column name in the stated source order. Columns absent from
a source are missing; a same-named string/numeric conflict is a loud error.
Numeric types are reconciled before the union: float with long or double
is widened to double so values survive both {cmd:collect} and {cmd:save}.
Combining a wider integer/decimal with floating-point values is refused at
execution if the conversion would lose its original precision; make an
explicit conversion first when that loss is intended. Derived non-finite
values are normalized to missing before the appended column is used.
With {opt generate(newvar)}, master rows receive 0 and each using source receives
1, 2, ..., labelled as native {helpb append} labels them: value label {cmd:_append}
(0 Master, {it:k} Appended dataset {it:k}; {cmd:__append1} and so on when that name is
taken) and variable label Dataset source. The marker must not collide on any side, including an exposed
Stata name carried under a different engine alias. A collision is refused
before changing the view. {cmd:joinby} is an inner
Cartesian match within each key tuple; same-named nonkey using columns are not
added and produce a note. Append clears the declared sort; merge and joinby
declare their keys as the result order.

{pstd}Cross-source name matching must be unambiguous. Unaligned view inputs,
such as a master {cmd:a} and a using view exposing {cmd:A} under that spelling,
are refused by {cmd:append}. File-backed using inputs are aligned to the master
when opened; {cmd:view:} inputs retain their existing aliases. All three
two-table verbs refuse an engine alias that identifies
different Stata columns on the two sides (for example {cmd:A_1} as an alias for
{cmd:A} on one side and an original {cmd:A_1} on the other). Rename the columns
consistently in the source views first. Case-distinct datasets with already
aligned names and aliases remain supported.

{pstd}Notes and characteristics of the using data follow native {helpb merge},
{helpb append} and {helpb joinby}, as read off StataNow 19.5. For the dataset
({cmd:_dta}) and for every variable the verb keeps from the using data (the keys,
the variables on both sides and the new ones; with {opt keepusing()} or
{opt keep()}, only those named), each characteristic the master lacks comes
across, and the master's wins on a name clash. The notes come across too, unless
{opt nonotes} ({cmd:merge} and {cmd:append}; {cmd:joinby} has no such option):
they follow the master's in their order, numbered on from the master's
{cmd:note0}, which {cmd:notes drop} leaves where it was; a note whose text the
master already has, compared exactly, is skipped, and one repeated within the
using data comes across each time. {cmd:append} takes its sources one after
another, so a later file skips a note that an earlier one brought. A using
{cmd:view:}{it:name} carries its own. {cmd:mergein} and {cmd:appendin} run the
native commands, so the same holds there.

{pstd}{cmd:duplicates drop} with no varlist deduplicates on every column and
needs neither ordering nor {opt force}. With a {it:varlist}, it requires both
{opt force} and a previous {cmd:parqit sort}; it keeps the first row in that
declared order. {cmd:duplicates report}/{cmd:list} are read-only diagnostics
and require an explicit key varlist.

{pstd}{cmd:keep in} {it:f}{cmd:/}{it:l} validates its range against the
real observation count when the pipeline runs; out-of-range is an error,
never a silent empty result. As in native {helpb keep}, the bounds may be the
letters {cmd:f} (first) and {cmd:l} (last) and negative counts from the end
({cmd:-1} is the last observation); {cmd:l} and negative bounds are resolved
from the view's current row count. {cmd:keep in} {it:#} keeps exactly
observation {it:#}; a reversed range is refused. {cmd:drop in} {it:f}{cmd:/}{it:l}
is the complement: it removes observations {it:f} to {it:l} of the same order
and keeps every other row in place, with the same bounds grammar and the same
validation against the real count.

{pstd}Result metadata follows native Stata where it is unambiguous. A
{cmd:collapse} target is labelled {cmd:(}{it:stat}{cmd:)} {it:source} and keeps
the source variable's display format (a {cmd:(count)} of a string source
carries {cmd:%8.0g}); a {cmd:(count)} target is stored {cmd:long}. The {cmd:merge} marker keeps native's {cmd:%23.0g} format with its
{cmd:_merge} value label. A {cmd:reshape wide} spread column is labelled
{it:jvalue} {it:stub} and keeps the stub's format. Because these travel into the
saved file's {cmd:parqit.*} metadata, third-party readers see the same
labels/formats; the data values are unchanged.

{pstd}Aggregation storage need not equal native {cmd:collapse}'s storage.
Means and standard deviations use the engine's double results, which can
retain more precision than native output stored as float. Percentile
interpolation retains the source endpoints until their exact midpoint is
rounded to double; {cmd:collapse} preserves a float
source's native value rounding, although the result may be stored wider.
{cmd:tabstat} uses the unrounded double statistic. Distinguish agreement of
statistical definitions from byte-identical storage types.

{pstd}A sampled or staged input to {cmd:summarize, detail} is materialized once
inside DuckDB before its moments and spillable rank sorts. Histogram range and
bin counts likewise share one realization. A bin boundary is compared exactly
as {it:bins*x >= (bins-k)*min+k*max}, avoiding division by an already rounded
width. Centers and width are rounded for Stata; indistinguishable centers are
refused. These temporary projections never enter
Stata's dataset and are released on completion or error. The embedded engine
includes pinned corrections for uniform reservoir inclusion and the C-API
window-aggregate state bridge. A missing/negative sample seed is chosen once
per plan and appears in {cmd:show}; it is not redrawn between statistical passes.
The count form uses a reservoir. Percentage sampling captures the source once
inside DuckDB, rounds the exact binary64 proportion {it:N*p/100} to the nearest
row count (half ties upward), and selects that many rows by a seeded hash
priority with row-index tie-breaking. Its materialization and sort can spill;
it may need substantially more time and scratch than a small fixed-count sample.
Reproducibility requires unchanged input order, engine and execution settings.
A sampling design with {opt cluster()} does not materialize its input: it
aggregates one row per cluster, ranks the clusters within each stratum and
joins the decision back to the rows. It therefore reads its input twice, and
repeats any work in the view's plan. The rank is
{it:mix(bits(x) ^ mix(seed))} for a number {it:x} (its binary64 bits, with
-0 read as +0) and {it:mix(fnv1a64(s) ^ mix(seed ^ 0x5bd1e9955bd1e995))} for
text {it:s}, where {it:mix} is splitmix64's finalizer; ties are broken by the
cluster's value. Integers beyond 2^53 are ranked by their binary64
approximation, so two that round to the same double share a key and are
ordered by value. A design without clusters materializes its input once, like the
percentage form. The strata and split-cluster checks run one query over the
current plan when {cmd:sample} is issued, and so validate a pending
{cmd:keep in} range at that point; a design without clusters leaves that range
to materialization, like every other verb.


{marker sampledesign}{...}
{title:Sampling designs}

{pstd}{bf:Sampling designs}, after Weesie's {cmd:sample2} (STB-37 dm46). {cmd:if} {it:exp}
defines the sampling frame: rows outside it are kept and never drawn. Under the default
SQL missing semantics a row whose {it:exp} is missing is outside the frame; with
{cmd:parqit set statamissing on} comparisons follow Stata, so {cmd:x > 60} holds for a
missing {cmd:x}, as in {cmd:sample2}. {opt by()} draws # percent (or, with {opt count}, # units)
within each stratum; missing values form their own stratum. {opt cluster()} draws
whole clusters instead of rows: all rows of a drawn cluster are kept, the others are
dropped. Rows with a missing cluster are outside the frame and kept; a note says how
many. The strata must be constant within clusters. A cluster that {cmd:if} splits is
an error, unless {opt any} places it in the frame when any of its rows satisfies
{it:exp}, or {opt all} only when every row does; without {opt cluster()}, {opt any}
and {opt all} are ignored with a note, as in {cmd:sample2}. {opt generate()} (or its synonym
{opt keep()}, as in {cmd:sample2}) adds a 0/1 variable, 1 for the rows that would be
kept, instead of dropping rows.{p_end}

{pstd}Each stratum draws the nearest whole number to n times the stored percentage
divided by 100, with half ties rounded upward, as the plain percentage form does:
for a whole-number percentage this is {cmd:sample}'s and {cmd:sample2}'s
{cmd:int(n*#/100+.5)}, and for a fractional one it can differ from them by one unit
at a tie (0.3 percent of 500 draws 1, where they draw 2). A cluster's rank comes from a
parqit function of the seed and the cluster's value only, so the drawn clusters do
not depend on row order, on how the source is split into files, on the number of
threads or on the DuckDB version; a number hashes its binary64 value, so 1 stored as
an integer or as a double is one cluster. Without {opt cluster()}, rows use the plain form's priority, so
{opt generate()} alone flags exactly the rows the plain percentage form keeps; with
{opt count}, a design keeps # rows per stratum by that priority, while the plain
count form keeps its reservoir. The draw never matches {cmd:sample2}'s, which uses
Stata's random numbers. The checks on clusters (constant strata, split clusters)
read the current plan once when {cmd:sample} is issued, like {cmd:merge}'s key
checks. A design without {opt cluster()} numbers rows in the view's sort order, or in
input order when there is none, and its draw depends on that numbering, as the plain
form's does. {cmd:in} is not supported.{p_end}


{marker materialisers}{...}
{title:Materialisers: atomicity, copysource, encoding, locks}

{pstd}{cmd:parqit collect} replaces Stata's current dataset only after the
engine result has been computed, typed, filled and decorated successfully in a
staging frame. Without {opt clear}, changed nonempty data in memory trigger
Stata error 4; {opt clear} explicitly authorises replacement. The lazy view is
not closed or reset. Until the successful swap, the old dataset and the staged
result can both occupy memory. Consequently a second collect re-runs the source and every
pipeline stage. Open views are likewise untouched by eager
{cmd:parqit use ..., clear}.

{pstd}{cmd:parqit save} writes a single Parquet file (atomically: an exclusively
owned same-filesystem staging file, validated before publication) or a
Hive-partitioned tree with {opt partition_by()} (staged and validated before
publication). A partitioned target that already exists is overwritten only
with {opt replace} (the new tree is built and verified first, then the old
one is set aside until the new tree is in place); without {opt replace},
or when the path exists as a plain file, the save is refused. Codecs:
{cmd:zstd} (default) {cmd:snappy gzip lz4 lz4_raw brotli uncompressed};
unknown codecs are rejected, never silently substituted. {opt chunk(#)}
sets the target rows per Parquet row group (smaller groups = finer
pushdown granularity for later reads; larger = better compression); the
engine rounds it to its internal 2048-row vector multiples, so the
effective minimum is 2048.

{pstd}{opt compression_level(#)} is a codec-specific DuckDB setting: a
nonnegative integer is forwarded to the chosen codec; omitted (or a negative
value) keeps the engine default. {opt partition_by(varlist)} names columns in
the result and writes a directory tree rather than a single file; a partition
key is restored to its recorded Stata type on read (a float/double/{cmd:%tc}
key too), and a zero-observation partitioned save writes an empty tree that
reads back as 0 observations with every variable. A string partition key
whose value is the text {cmd:NULL} or {cmd:__HIVE_DEFAULT_PARTITION__} is
refused before the tree is published: the engine names the directory of a
{it:missing} partition that way and would read the value back as missing
(empty). A foreign tree carrying such a directory under a string key loads
those rows with the key empty, and says so in a {cmd:note:}; every other
value — the empty string, {cmd:=}, {cmd:/}, spaces, {cmd:%}, Unicode,
names differing only by case, numeric-looking text — round-trips exactly,
as does a missing numeric or date key. A save is
refused if its destination is the current view's own source file, matches one
of its source-glob paths, or lies inside (or would replace a directory
containing) a directory the view scans; collect first or choose a
nonoverlapping destination. A destination that is a symbolic link is written
through to its target (native {cmd:save, replace} semantics); a read-only
existing destination refuses {opt replace} with {cmd:r(608)}, as native does.
A valid destination name is accepted up to the filesystem limit
({cmd:NAME_MAX}, 255 bytes on the usual systems); the package lock and staging
siblings fall back to short digest-keyed names when the destination basename is
long.

{pstd}{opt xmissing} (a save of the dataset in memory) preserves extended
missing values {cmd:.a}-{cmd:.z}, which Parquet's single null otherwise
collapses to {cmd:.}: every variable that holds one gets an {cmd:int8}
companion column {cmd:_parqit_xm_}{it:var} carrying the code of each cell
(0 = none, 1-26 = {cmd:.a}-{cmd:.z}; the variable's own cell stays null) and
the pairs are recorded under the {cmd:parqit.xmissing} footer key. The file is
still ordinary Parquet — other readers see the companions as columns.
{cmd:parqit use}, {cmd:mergein} and {cmd:appendin} hide the companions and
restore the cells in every numeric storage type (a byte {cmd:.a} comes back a
byte {cmd:.a}), printing a {cmd:note:} that names the variables; a companion
whose codes are outside 0-26, or that marks a cell holding a value, makes the
load fail rather than guess, and a variable named like a companion is refused
at save time. A column declared both as a primary and a companion is refused
on read. A multi-file source with differing {cmd:parqit.*} metadata and an
extended-missing channel is also refused, including with {opt relaxed}; read
the files separately and use {cmd:parqit appendin} to preserve the codes.
A lazy view over a compatible source hides the companions and reads
those cells as plain {cmd:.} — the open says so — so a {cmd:collect} or a view
{cmd:save} from it carries no extended missings. Not available with
{opt copysource} (the file is copied as it is; a source that carries companions
is refused there) or with {opt partitions()} (every file of a tree must carry
the same metadata); a whole-tree {opt partition_by()} save is fine. Without the
option nothing changes: the loss is announced as before, and the note now
names the option.

{pstd}{opt partitions(replace)} and {opt partitions(append)} update an
{it:existing} partitioned tree partition by partition instead of rewriting it
(they need {opt partition_by()} and exclude {opt replace}). With
{cmd:replace}, every partition present in the result replaces its namesake in
the tree — staged, verified, then swapped directory by directory, the old
directory set aside until the new one is in place — while the partitions
absent from the result stay byte-identical and a partition the tree does not
have yet is added: the monthly "add or replace one month" update. With
{cmd:append}, the result's files are added into the partitions under unique
names and nothing is removed. The tree must be a Hive tree over the same keys
in the same order, and the result must read as one dataset with it: the same
columns and engine types, and the same {cmd:parqit.*} metadata (labels,
formats, value labels, notes, characteristics, Stata storage types including
{cmd:str#} widths, and the {cmd:sortedby} marker) — a difference is refused before anything is
published, with the tree untouched, because files that disagree lose their
labels on read. The refusal names what differs, variable by variable (for
example {cmd:cae: storage type str5 in the tree, str8 in the result}). A tree
written by another tool, without {cmd:parqit.*}
metadata, receives its new partitions without it, with a {cmd:note:}; a tree
whose files store the partition key inside is refused. A zero-row result
touches nothing. A note reports how many partitions were replaced, added,
and, with {cmd:append}, extended with a new file;
handled publication errors trigger rollback of the touched partitions. Several
directory exchanges do not form one atomic snapshot for concurrent readers;
see {help parqit_technical##materialisers:atomicity and locks}.

{pstd}The current partition-update validator requires a regular Hive directory
layout: non-hidden marker files such as {cmd:_SUCCESS} at partition-directory
levels are refused. The empty tree written by a zero-row save contains a root
Parquet schema file and cannot yet be populated with {opt partitions()}; use
an initial full-tree {opt replace} when creating its first populated version.

{pstd}With a view open, {cmd:parqit save} materialises that view and leaves the
current Stata dataset untouched; {opt data} instead writes the in-memory
dataset. With no view open, save writes memory and {opt data} is redundant.
Thus selection is explicit and never guessed from which dataset was most
recently changed.
{cmd:parqit use} {it:file}{cmd:, clear} is the corresponding eager read path.

{pstd}The output format is always Parquet, irrespective of the filename
extension. To produce a Stata {cmd:.dta}, collect a result that fits in Stata
and then use native {cmd:save}. {cmd:parqit save output.dta} does not convert
Parquet to a Stata file.

{pstd}Stata's plugin observation index is signed 32-bit. Eager
{cmd:parqit use ..., clear} and {cmd:collect} therefore refuse a result above
2,147,483,647 observations with error 901 before filling memory. The lazy view
and disk-to-disk path remain valid: filter or aggregate first, or write the
large result with {cmd:parqit save}.

{pstd}The contracts behind {cmd:collect} and {cmd:save} — the {opt copysource}
opt-in, {opt encoding()} and the transcoding of legacy text, the lock file that
serializes writers, and the full string-encoding rules — are in
{help parqit_technical##materialisers:the technical reference}.

{pstd}The default memory-to-Parquet writer assembles complete Arrow column
buffers before writing; this can require substantial memory in addition to
the dataset already in Stata. {cmd:open _data} and in-memory input adapters
use that writer too. They do not have the same memory profile as a direct
lazy Parquet-to-Parquet save. Set the operating-system environment variable
{cmd:PARQIT_SAVE_NOARROW=1} before launching Stata to use the existing batched
writer through a spillable DuckDB staging table. Any presence of this variable
(even {cmd:0}) selects that path; unset it to restore the default. This trades
additional staging work for avoiding the complete Arrow buffer assembly.

{pstd}{opt copysource} is an explicit, hardened opt-in for
{cmd:parqit save} {it:…}{cmd:, data}: instead of reading the dataset in memory,
it copies the unchanged Parquet file loaded by the last
{cmd:parqit use} {it:file}{cmd:, clear} — you assert nothing has changed. The
default memory-save path reads the dataset in memory; with a view open,
request that path with {opt data}. Stata's
{cmd:c(changed)} cannot prove the dataset still equals the file: it stays 0
after {cmd:sort}/{cmd:gsort} and after Mata {cmd:st_store}/{cmd:st_sstore}/
{cmd:st_view} writes, which reorder or edit the data. {opt copysource} therefore
verifies, and refuses loudly when any check fails: the source's full identity
must still match (size, mtime, ctime, inode and a Parquet-footer digest,
re-checked immediately before and after the copy); the in-memory variable
names/kinds, observation count and {cmd:sortedby} must equal the file's; the
first and last 64 observations of every variable must equal the file's rows;
and the dataset must be reproducible by copy (case-distinct names, sanitised
names, {cmd:%tc} and binary {cmd:strL} are refused with the remedy). Those
checks catch a {cmd:sort}, a {cmd:gsort} and any edit that touches either end of
the data; they do {bf:not} compare the observations in between — an edit
confined to the middle rows (a Mata {cmd:st_store} on observation 1,000 of
2,003, say) is not detected, and the copy then carries the source file's
content, not memory: with {opt copysource} you assert that nothing has changed.
The copied file is the source file's content with the source file's own
{cmd:sortedby} claim (copied as is), and {cmd:r(copysource)} reports the file
copied. Eager {cmd:parqit use} {it:file}{cmd:, clear} records
a private characteristic {cmd:char _dta[_parqit_fast_source_nonce]} that ties the
dataset to that source so {opt copysource} can verify provenance; it is harmless,
travels with a saved {cmd:.dta}, is never written into a parqit Parquet file, and
may be removed with {cmd:char _dta[_parqit_fast_source_nonce]}.

{pstd}{opt encoding(name)} names the code page used to transcode text that is
not valid UTF-8 (see {it:String encoding} below): any of the encodings listed
under {help parqit_technical##encoding:Text encodings} — UTF-8, 49 single-byte code
pages (Windows 874 and 1250-1258, ISO-8859, KOI8, DOS, Mac), Shift_JIS (932),
GBK (936), UHC (949), Big5 (950), EUC-JP and GB18030 — by its name or a usual
alias, case-insensitively and ignoring {cmd:-}, {cmd:_}, {cmd:.} and blanks.
Without it the session code page applies ({cmd:windows-1252}, or the one
{cmd:parqit set encoding} named). {opt encoding(name, all)} decodes every
text from {it:name}, also text that happens to be valid UTF-8. ASCII bytes are
unchanged only when that encoding gives them their ASCII meaning; CP864's
0x25 becomes Arabic {cmd:٪}. {opt copysource} refuses {cmd:all} before writing:
omit {opt copysource} to decode text through the normal memory writer.
{cmd:r(encoding)} reports the canonical name ({cmd:windows-1252}, {cmd:latin1},
{cmd:latin9}, {cmd:macroman}, {cmd:windows-1251}, {cmd:windows-936}, ...)
whatever spelling was typed. Any other name is refused before anything is
written — on {bf:both} the memory-save and the lazy view-save paths — with the
list of the families parqit reads; UTF-16 applies to delimited text only. It has
an effect only for a save of the dataset in memory; a lazy Parquet-to-Parquet
save carries UTF-8 already, so a valid name is accepted with no effect there.

{pstd}
Writers for the same destination are serialized by
{it:filename}{cmd:.parqit_lock}. parqit removes that lock only when the current
process created it. A pre-existing or crash-stale lock therefore causes a loud,
fail-closed refusal; after confirming that no writer is alive, the user may
remove that stale lock explicitly. Historical sibling names such as
{cmd:.parqit_tmp}/{cmd:.parqit_old} are never treated as package-owned.

{pstd}Atomicity has a scope. Single-file publication uses a same-filesystem
rename when replacing; creating a new file on POSIX uses a hard link followed
by removal of the staging name, so an existing destination cannot be
overwritten in a race. Filesystems without hard-link support can refuse that
new-file path. Replacing a tree and updating several partitions
involve multiple directory exchanges, with rollback on handled publication
errors; they are not one transaction visible atomically to concurrent readers,
nor a crash-recovery guarantee. The writer lock covers the resolved destination,
not every ancestor or descendant path. Coordinate readers and writers of a
partitioned tree, and do not concurrently write overlapping destinations.

{pstd}
{it:String encoding.} Parquet/Arrow strings must be valid UTF-8. Text that is
already valid UTF-8 (text in any script, emoji, {cmd:strL}) is written
byte-exact. A string cell, variable or data label, value-label text, note or
characteristic that carries the raw bytes of a legacy code page (common in
administrative data saved by Stata 13 and earlier, or loaded into a Unicode
Stata without {helpb unicode:unicode translate}) is
{bf:transcoded to UTF-8 on the way out}, item by item — what
{cmd:unicode translate} would do, with no
translate step on your side and without touching the dataset in memory. The
source code page is {opt encoding()}, or else the session's:
{cmd:windows-1252} (identical to Latin-1 for the accented letters, and covering
the euro sign and typographic quotes in 0x80-0x9F) unless
{cmd:parqit set encoding} named another. A byte an 8-bit code page leaves
undefined becomes, as in the WHATWG decoders, the C1 control of the same value
(0x80-0x9F) or U+FFFD; an invalid multibyte sequence becomes U+FFFD (a trailing
ASCII byte is kept); each text with a U+FFFD is counted in
{cmd:r(undecodable)} and said. A {cmd:str#} whose transcoded values are longer is recorded
wider, exactly as {cmd:unicode translate} widens it, and past 2,045 bytes the
recorded type becomes {cmd:strL} (the {cmd:parqit.*} metadata is built after the
data pass, so the recorded type always matches the written values). Every save
that transcodes anything prints a {cmd:note:} with counts and returns
{cmd:r(transcoded_cells)}, {cmd:r(transcoded_meta)}, {cmd:r(transcoded_vars)},
{cmd:r(transcoded_revalid)}, {cmd:r(undecodable)}, {cmd:r(encoding_default)}
and {cmd:r(encoding)}. The limitation shared with {cmd:unicode translate} — a
legacy string that happens to be well-formed UTF-8 cannot be told apart — is
narrowed three ways (ENC-3): with a multibyte code page, whose text is often
valid UTF-8 by accident, a string variable with any cell that is not UTF-8 is
decoded as a whole, and so is all the metadata once any column or item is
legacy (a pre-scan finds them, stopping per variable at its first such cell);
{opt encoding(name, all)} decodes all text, including a code page's non-ASCII
meaning for a byte below 128 (CP864's 0x25); and a
{cmd:.dta} of format 117 or older ({cmd:dtaversion}), which predates Unicode,
is read with {cmd:all} whenever {opt encoding()} is given. Otherwise a single
legacy string that is well-formed UTF-8 is kept as is. A Parquet file is never
transcoded on read (delimited text, {cmd:.dta}, Excel, SPSS and R files are
decoded on their way in; see {help parqit_technical##encoding:Text encodings}): a foreign Parquet file whose
string payload is not valid UTF-8 is refused by the engine with a loud error
naming the column — rewrite it as UTF-8 at the source. A binary {cmd:strL} containing an embedded NUL cannot be represented
through the Stata plugin's text interface, so a direct memory-to-Parquet save
refuses the offending cell before publishing any output. A lazy
Parquet-to-Parquet save does not cross that interface and preserves the bytes.

{pstd}
{it:Encoding tables.} The code-page tables are generated
({cmd:tools/gen_encoding_tables.py}) from the standard mapping files as Python's
codecs carry them, corrected where Windows reads its own code pages
differently: the end-user-defined areas of 932, 936, 949 and 950 map to the
Private Use Area as Windows maps them, 950's C6A1-C8FE is such an area (not the
ETEN extensions), 936's reserved cells keep the Private Use code points
Windows gives them where GB18030 later assigned real characters, EUC-JP's JIS
X 0208 is read through the 932 table by pointer (as WHATWG builds it: the NEC
and IBM extensions, and Microsoft's mappings of the few characters vendors
disagree on), and GB18030 follows its 2005 edition (as ICU and glibc do). No
multibyte sequence decodes to an ASCII character, so decoding cannot create a
delimiter, a quote or a line break. The verify suite compares every byte
sequence of every code page — about 195,000 — with ICU ({cmd:ustrfrom()}); the
few that differ are listed there with the reason (IBM's extensions in ICU,
vendor tables of the DOS code pages 864 and 869).

{pstd}
{it:Delimited text} (CSV-ENC-1). The engine's CSV reader reads UTF-8 only, and
reads UTF-16 without a byte-order mark as one column of garbage without an
error, so parqit reads the first 64 KiB of each file first: a UTF-8, UTF-16 or
UTF-32 byte-order mark; UTF-16 without one — NUL bytes in at least a tenth of
the byte pairs with at least 95% at one parity, or (fewer NUL bytes: text in
other scripts) a sample that is not UTF-8 whose line feeds are UTF-16 line
feeds at even offsets, at most one in ten at the other parity, and which read
as UTF-16 is text (no unpaired surrogate, no control character but tab, LF and
CR: random bytes fail at once); UTF-32 without
one — the two high bytes of every four NUL (95%), the low one not (5%), refused
unless {opt encoding()} names an encoding. Any other NUL bytes (the padding of
fixed-width exports) are left to the reader, as before. An {opt encoding()}
given is followed over these patterns, with a note; a byte-order mark is not a
pattern but the file's own declaration, and overrides {opt encoding()}. A file
that is not UTF-8 is decoded, streaming, in 4 MiB blocks cut after a CR or LF
(bytes no supported encoding uses inside a character; UTF-16 blocks keep a split
surrogate pair together); a line longer than 64 MiB is cut into pieces that
depend on the text alone — anywhere in a single-byte code page, at a character
boundary in UTF-8, after a byte below 0x30 in a multibyte code page, and
refused when a multibyte one has none. The copy goes to a package-owned
directory, erased with the view or once imported, under the file's own name
and the {it:key}{cmd:=}{it:value} directories of its path (the engine reads
them as Hive partition columns); a glob whose files need it becomes the same
tree of copies, read through the same pattern (files that need no decoding are
copied as they are; with a single-byte code page every file is read line by
line, its lines that are valid UTF-8 kept). The path is used as given, never
normalized: the system resolves {it:link}{cmd:/..} physically, and the engine
reads Hive keys from the path it is given. A path that names a file is that
file, even with {cmd:*} or {cmd:?} in it; otherwise {cmd:*} and {cmd:?} are live
and {cmd:[} is literal (GLOB-2). A missing file or an empty glob is left for the reader to report, as
before. With a single-byte code page each line is decoded unless it is valid
UTF-8; with a multibyte one the file is first checked for UTF-8, stopping at the
first line that is not, and then decoded whole. The notes are written by the
plugin, file by file where files differ. The engine's own CSV encodings
(latin-1, UTF-16) are not used: they reject half of windows-1252, and its
extension hook copies multibyte text wrongly (DuckDB 1.5.3, {cmd:csv_encoder}).
The engine's refusal of text that is not UTF-8 names parqit's {opt encoding()}
in place of the engine's own advice.


{marker explore}{...}
{title:Exploration commands}

{pstd}
These commands inspect the carried schema or request engine-side queries —
only summary output or the requested preview rows reach Stata, and the
current dataset is never replaced or modified:

{pstd}Results follow native Stata presentation conventions: variable labels
in headings, bordered tables, Stata numeric formats and correlation panels
that fit {cmd:linesize}. {cmd:summarize, detail} places percentiles beside the
four smallest and largest values, with moments on the right. Numeric output
for calculated statistics uses numeric formats; returned numbers retain their
precision. Long category labels and preview cells may be shortened with
{cmd:~}; braces and control characters in data are displayed as text.
{cmd:codebook} and {cmd:misstable} retain the compact information described
below, including parqit's string-missing and complete-observation counts.

{pstd}Statistics are unweighted. Their varlists take explicit view names,
including an exposed Stata name or its reported engine alias; wildcards,
variable ranges and native {cmd:if}/{cmd:in} qualifiers are not supported here.
{cmd:count if} and {cmd:list if/in} have their own syntax. For a filtered
statistical comparison, prepare a second named view and apply its extra
{cmd:keep if} before computing the summary; switching back preserves the first
view. A lazy filter changes its target plan and is not automatically undone.

{p 8 12 2}{cmd:parqit count}{space 17}rows → {cmd:r(N)}{p_end}
{p 8 12 2}{cmd:parqit summarize} [{it:vars}]{space 6}obs/mean/sd/min/max per numeric variable{p_end}
{p 8 12 2}{cmd:parqit summarize} {it:v}{cmd:, detail}{space 3}adds variance, skewness, kurtosis and the
p1 p5 p10 p25 p50 p75 p90 p95 p99 percentiles, all with Stata's exact
definitions (population central moments; the {cmd:summarize} percentile
rule); stored results are listed {help parqit_technical##results:below}{p_end}

{pstd}{cmd:summarize} reports numeric variables. The ordinary form omits
strings and refuses a request with no numeric variables; {opt detail} refuses
an explicitly named string. Variance and standard deviation are sample
statistics (denominator N-1; missing for N below 2). Skewness and kurtosis use
population central moments; kurtosis is Pearson's coefficient, with 3 for a
normal distribution, not excess kurtosis. Sums and means accumulate exactly
before one rounding to the nearest Stata
double. Percentile endpoints retain their source precision until interpolation;
amplitudes subtract before conversion. Dispersion, shape and correlations use
exact power sums/cross-products and certified rounding, including the
same calculations inside {cmd:egen}, {cmd:collapse} and {cmd:pivot}. A finite
standard deviation can be reported even when its variance is too large to
store. Undefined or out-of-range results are ordinary missing; variance may
round to zero below double's smallest positive value. Even sd may round to
zero for a nonconstant sample at that boundary; skewness and kurtosis still use
the exact variance numerator. Corrected results can
differ from native Stata when its own arithmetic loses precision at extreme
offsets or scales. See {help parqit_technical##types:the technical reference}
for numerical and storage limits.

{pstd}For unchanged inputs, the algebraic summary results do not depend on
row order or thread count. Correlation p-values use an independently computed
complement, so a rounded correlation of 1 need not have p-value zero. Their
transcendental tail evaluation has ordinary floating-point/conditioning error
and a documented beta-function domain; see the technical reference. Exact
accumulators use bounded engine memory per group, with increased scratch and
computation possible for many groups. Raw SQL uses DuckDB's function semantics.

{pstd}Numerical correctness concerns the values actually stored. In expressions,
the double value written as .1 is slightly greater than an exact decimal tenth.
Accordingly, {cmd:round(.25,.1)} gives .2 and {cmd:mod(1,.1)} is approximately
.09999999999999995. The implementation avoids native failures such as changing
the integer in {cmd:round(4503599627370497)} or returning 0 for
{cmd:mod(1e100,3)} (the correct remainder is 1). See
{help parqit_technical##expressions:Expression dialect} for domains and tie rules.

{p 8 12 2}{cmd:parqit tabulate} {it:a}{space 14}one-way frequencies (freq/percent/cum){p_end}
{p 8 12 2}{cmd:parqit tabulate} {it:a b}{space 12}two-way cross-tabulation with row/column totals
(the column variable may have at most 30 distinct values; the table at most
10,000 occupied cells). Both forms are laid out as native {cmd:tabulate} lays
them out: a long variable label wraps over several lines instead of widening
the table, long values are cut, and a table wider than the line is split into
panels{p_end}
{p 8 12 2}{cmd:parqit misstable} [{it:vars}]{space 6}missing count and share per variable (strings
count {cmd:""}){p_end}
{p 8 12 2}{cmd:parqit levelsof} {it:v}{space 12}sorted distinct values → {cmd:r(levels)}
(strings compound-quoted, like {helpb levelsof}); refuses beyond
{opt limit(#)} (default 5,000){p_end}
{p 8 12 2}{cmd:parqit head} [{it:#}]{space 13}materialises only {it:#} rows (default 5) into a
scratch frame, lists them, discards them; {it:#} must be positive{p_end}
{p 8 12 2}{cmd:parqit describe}{space 14}the view's schema and pipeline depth
({cmd:parqit glimpse} is a synonym).
The Stata types shown are the honest display of the file's declared/saved
types {it:without} a data scan; {cmd:collect} additionally sizes integers and
strings from the observed range, so a foreign file's column can arrive
narrower than {cmd:describe} showed{p_end}
{p 8 12 2}{cmd:parqit describe} {it:parquet_source} [{cmd:,} {opt labels} {opt notes}]{space 1}the
file's footer, without reading column values: rows, columns and row groups, and for each
variable its Parquet and Stata types, format, value label and variable label, as
{cmd:describe using} shows a Stata dataset. {cmd:*} marks a variable with notes,
{cmd:(_dta has notes)} a dataset note, and {cmd:(spss)} a string variable whose SPSS
value labels are kept in {cmd:char} {it:var}{cmd:[spss_value_labels]} (see
{cmd:parqit spssencode}); closing lines count the labels and notes. {opt labels} lists
the value-label sets as {cmd:label list} does, then those SPSS labels; {opt notes}
lists the notes as {cmd:notes list} does{p_end}
{p 8 12 2}{cmd:parqit count if} {it:exp}{space 8}filtered count {it:without touching the view's pipeline}
(any parqit expression except {cmd:_n}/{cmd:_N} — see
{help parqit_technical##expressions:Expressions} — including {cmd:missing(a,b,c)}){p_end}
{p 8 12 2}{cmd:parqit list} [{it:vars}] [{cmd:if}] [{cmd:in}]{space 2}non-mutating preview
with projection, filter and row-range (bare {cmd:parqit list} shows rows
1-20; a bare {cmd:if} caps at 200 rows){p_end}
{p 8 12 2}{cmd:parqit ds}{space 20}variable names → {cmd:r(varlist)}{p_end}
{p 8 12 2}{cmd:parqit lookfor} {it:words}{space 8}match names and labels{p_end}
{p 8 12 2}{cmd:parqit codebook} [{it:vars}]{space 6}per variable: kind, obs, missing,
distinct, min/max, label (one scan){p_end}
{p 8 12 2}{cmd:parqit distinct} [{it:vars}]{space 6}the number of distinct values of each
variable, as the community-contributed {cmd:distinct} (Cox and Longton, SSC) reports
it: {bf:Obs} counts the observations used, the nonmissing ones, and missing values
are not a distinct value; {opt missing} counts every observation and missing as
one more value. {opt joint} adds one line, {bf:(joint)}: the number of distinct
combinations of the variables, the groups they define together, counted over the
observations with no missing value among them (all observations with
{opt missing}). {cmd:r(N)} and {cmd:r(ndistinct)} hold the last line{p_end}
{p 8 12 2}{cmd:parqit duplicates report} [{it:vars}]{space 1}copies/observations/surplus
table; {cmd:duplicates list} lists the observations of every duplicated group as
native {helpb duplicates} {cmd:list} does: {bf:Group}, {bf:Obs} (the row's {cmd:_n} in
the view's current order) and the variables, up to {opt limit(#)} rows (default 20).
With no {it:vars}, all variables are used, as in native {cmd:duplicates}{p_end}
{p 8 12 2}{cmd:parqit misstable patterns}{space 3}frequencies and percentages of missing-data
patterns ({cmd:1} observed, {cmd:0} missing; up to 14 variables and the
100 most frequent patterns). Percentages use the full view; when patterns are
omitted, the displayed share and total observation count make this explicit{p_end}

{pstd}With no varlist, {cmd:misstable patterns} selects every view variable,
including fully observed variables and strings, so that form requires at most
14 variables. On a wider file, first use {cmd:misstable} and then name the
variables relevant to the missing-data check.

{p 8 12 2}{cmd:parqit tabstat} {it:vars}{cmd:, s()}{space 5}statistics × variables table
({cmd:n mean sd var sum min max range median p##}; {cmd:count} ≡ {cmd:n});
{opt by()} groups (≤200); {opt save} returns the calculated tables in {cmd:r()}{p_end}
{p 8 12 2}{cmd:parqit correlate} [{it:vars}]{space 5}correlation matrix, listwise like
{helpb correlate}; {cmd:parqit pwcorr} is pairwise, with {opt obs} and {opt sig}
(two-sided p from the t distribution). With no {it:vars}, both use every numeric
variable and name each string variable they skip, as native does{p_end}
{p 8 12 2}{cmd:parqit histogram} {it:v}{space 9}bins computed by the engine; only the
bin table reaches Stata, drawn with {cmd:twoway bar} ({opt bins(#)},
{opt nodraw}) → {cmd:r(bins)}, {cmd:r(width)}, {cmd:r(start)}{p_end}

{pstd}
Each data-query call re-executes the lazy pipeline; schema-only commands use
the carried schema. Filters and column selections can be pushed into a Parquet scan. Execution cost
depends on the plan and the source; an exact statistic may require a full scan
or sort. {cmd:parqit tabulate}
excludes missing values unless {opt missing} is given, like native
{helpb tabulate}; {opt row}/{opt col} add percentage panels to the two-way
form; a labelled numeric variable is displayed through its value labels, as
native does, and {opt nolabel} shows the codes instead. {cmd:codebook}'s unique count and {cmd:distinct} exclude missing values;
{cmd:tabstat, by()} omits a missing by-group, matching native Stata. SQL NULL,
empty-string and NaN encodings of the same Stata missing value are folded before
grouping. Stata transforms that have no special command translate directly:
{cmd:destring} ≡ {cmd:parqit gen y = real(x)} (with
{cmd:subinstr(x, ",", "", .)} for thousands separators), string length ≡
{cmd:parqit gen n = strlen(s)}, {cmd:bysort g: gen n = _N} ≡
{cmd:parqit egen n = count(1), by(g)}, and a duplicates tag ≡ that count
minus one. There is no {cmd:browse} over a view — preview with {cmd:parqit list}/{cmd:head} or materialise a slice with {cmd:parqit list}'s {cmd:in}
ranges; {cmd:kdensity} and {cmd:graph box} need the data and are best run
after a {cmd:collect} of the variables involved.

{pstd}
Additional display bounds are deliberate safeguards, not partial silent
results. A one-way {cmd:tabulate} refuses more than 10,000 levels. A two-way
table also caps its column dimension at 30. {cmd:tabstat, by()} permits at most
200 nonmissing groups. {cmd:histogram} defaults to ceil(sqrt(N)) bins capped at
50; an explicit request is capped at 1,000, and a constant variable uses one
bin. {cmd:bins(0)} selects the automatic rule; negative values are refused.
A constant variable returns width 0 and is drawn as a bar of width 1 at its
value. A positive width that underflows to zero, or a width outside Stata's
numeric range, is refused with a message to change {cmd:bins()} or rescale.
Bins are left-closed and right-open, with the maximum included in the last
bin. Boundaries use the exact stored endpoints and requested number of bins;
the displayed width is rounded to double. Bin centers that collapse to the
same Stata value are refused; reduce bins or center/rescale the variable.
{cmd:levelsof} excludes missing and fails if its limit would be exceeded.
{cmd:lookfor} is case-insensitive and returns variables whose name or label
contains any supplied word. These commands do not mutate the view; neither do
{cmd:count if}, {cmd:list}, {cmd:duplicates report/list}, {cmd:show},
{cmd:explain} or either form of {cmd:describe}.

{pstd}{cmd:tabstat} uses the native default orientation: variables in columns
for a multi-variable request, statistics in columns for a single variable.
With {opt by()}, parqit reports group summaries only, corresponding to native
{cmd:tabstat, nototal}; it does not add an overall aggregate query. The
{opt save} option stores those same results for reuse without another scan.
It writes no data file. Correlation commands return the full matrices as well
as the existing scalar results, subject to Stata's matrix dimension limit.

{pstd}An invalid deferred {cmd:keep in}/{cmd:drop in} is refused by these
execution commands just as by {cmd:collect}/{cmd:save}. Explicit names in
{cmd:codebook} and both {cmd:misstable} forms must all exist. Preview defaults
are small, but an explicit large {cmd:head} or {cmd:list in} request must fit
in a scratch Stata frame; a row limit does not bound the work of an upstream
join, aggregation or sort.

{pstd}Exploratory output uses native Stata numeric formats and table layouts;
labels come from the view's carried metadata. The smallest/largest values in
{cmd:summarize, detail} are fetched from the existing percentile sort, adding
no source scan. Printed numbers are rounded for display; returned matrices
retain the calculated precision. {cmd:tabstat, save} and correlation matrices
are built from the already computed summary response, not from another query
or a reconstruction of the raw dataset. Missing-pattern percentages use a
total computed before the 100-pattern display cap.

{pstd}Statistics cross into Stata as binary64 values before text formatting,
including extrema from a FLOAT source. Non-finite values and magnitudes outside
Stata's numeric range are returned as ordinary missing, and numeric response
text is parsed as a number rather than evaluated as an expression.
Floating sums use a fixed integer accumulator in units of the smallest
binary64 subnormal. Integer/decimal inputs use fixed 256-bit integer states.
Sums and means are rounded once to the nearest binary64 value, with ties to
even; means divide the exact total before rounding. Integer/decimal conversions
use exact midpoint comparisons, including collect, extrema and percentiles.
Dispersion and shape use exact integer power sums. Central moments are formed
before division; variance and Pearson kurtosis are exact rational values
rounded to binary64, while sd and skewness certify the rounding of their square
roots against exact midpoint squares. Correlation likewise forms exact sums
and cross-products and independently rounds the coefficient and its geometric
complement. Results for the same stored inputs are invariant to order,
partitioning and thread count. No clipping or approximate retry is used;
an internal mathematical invariant failure is an error.

{pstd}Standard deviation is rounded from the exact variance independently of
the returned variance. Thus sd can be finite when variance overflows, or nonzero when
variance underflows to zero. A constant with a very large level still has
zero sd and its finite mean. Conversely, a nonconstant sample can have sd
rounded to zero at the subnormal boundary; shape uses the exact variance
numerator and remains defined. These algorithms do not reproduce native
Stata's numerical failures. Counts, missingness and rank definitions
remain unchanged; unrelated SQL, source and resource failures remain errors.
Exact integer sums in a lazy table retain the engine's integer/decimal result
type and still refuse a true overflow of that type. For an explicitly double
total of wide values, {cmd:egen double newvar = total(x)} accumulates before
conversion. Returned {cmd:tabstat, s(sum)} statistics are double values too.

{pstd}The states are bounded and remain inside DuckDB's grouped/window/spill
execution: 288 bytes for a total, 832 for dispersion, 2,736 for detail and
2,184 per correlation pair. Many groups can require more time and scratch
than approximate accumulation. The arithmetic bound admits fewer than 2^64
values per state, finite doubles with magnitude below 2^1023, and the engine's
128-bit integer/decimal coefficients. No raw floating powers are formed.

{pstd}Correlation significance uses the independently calculated complement,
not {it:1 - rounded_rho^2}. A coefficient displayed or stored as 1 can therefore
have a small positive p-value. The two-sided Student tail uses stable analytic
or beta-function forms; a scaled series recovers subnormal probabilities that
Mata's exponential implementation would otherwise turn into zero. Unlike the
algebraic statistics, these transcendental tails are subject to floating-point
library and conditioning error; correctly rounded final bits are not promised.
The beta-function shape domain ends at 1e17: outside it, nontrivial p-values
are missing with a note, while the correlation remains available.
Raw {cmd:parqit sql}/{cmd:query} fragments retain DuckDB's own function semantics;
user-written SQL aggregates are not silently replaced by parqit's routines.

{pstd}Statistics read complete response records even when a string or label
exceeds 32 KiB. Each duplicate-preview cell has its own encoded field, and
string keys retain embedded NUL bytes during grouping and table construction.
Display escapes NUL, line breaks and unit separators as {cmd:\0}, {cmd:\n},
{cmd:\r} and {cmd:\x1f}; these escapes describe characters in the data.


{marker expressions}{...}
{title:Expression dialect}

{pstd}
{cmd:keep if}, {cmd:drop if}, {cmd:count if}, {cmd:gen}, {cmd:replace} and
{cmd:egen} translate Stata expressions to SQL. Supported operators are
{cmd:+ - * / ^} (with Stata precedence; {cmd:^} is left-associative power and
{cmd:/} never integer-divides), {cmd:== != ~= < <= > >=}, {cmd:& |}, unary
{cmd:!}/{cmd:~} and parentheses. Relational chains are left-associative, as in
Stata: {cmd:1 < x < 10} means {cmd:(1 < x) < 10}. {cmd:+} also concatenates two
strings. Ordinary and compound string literals and the ordinary missing
literal {cmd:.} are supported. The complete function list is:

{pstd}
The date literals are constants, not functions of a variable. Seven use
Stata's own notation: {cmd:td(}{it:ddmonyyyy}{cmd:)},
{cmd:tm(}{it:yyyy}{cmd:m}{it:#}{cmd:)}, {cmd:tq(}{it:yyyy}{cmd:q}{it:#}{cmd:)},
{cmd:th(}{it:yyyy}{cmd:h}{it:#}{cmd:)}, {cmd:tw(}{it:yyyy}{cmd:w}{it:#}{cmd:)}
and {cmd:tc()}/{cmd:tC(}{it:ddmonyyyy hh:mm}[{cmd::}{it:ss}[{cmd:.}{it:fff}]]{cmd:)}
— for example {cmd:td(01jan2015)}, {cmd:tq(2015q1)} and
{cmd:tc(01jan2015 09:30:00)}. {cmd:ty(}{it:yyyy}{cmd:)} is a parqit extension
accepted for symmetry: native Stata has no {cmd:ty()} function; a yearly
{cmd:%ty} value is written as the bare year, for example {cmd:2026}. An
impossible date such as {cmd:td(31feb2020)} or a 60th second is rejected
loudly. {cmd:tC()} yields the same count as {cmd:tc()}: parqit does not add
leap seconds.

{pstd}
{cmd:_n}/{cmd:_N} are supported in {cmd:keep if}/{cmd:drop if} and in the
main expression of {cmd:parqit gen}; they are windows over the declared
{cmd:parqit sort} order (or engine scan order when no sort was declared, which
is not a reproducibility guarantee). Everywhere else they are unavailable:
{cmd:egen} refuses them in its argument, {cmd:replace} refuses them in either
half of the command, {cmd:gen} refuses
them inside its {cmd:if} qualifier (the {it:main} expression of
{cmd:gen ... if} may still use them), and the read-only
{cmd:count if}/{cmd:list if} filters do not implement them at all. Every one of
those forms fails loudly and leaves the view unchanged.

{pstd}
{it:Missing-value semantics.} By default expressions use SQL semantics:
numeric missing is NULL and comparisons with nonmissing constants are unknown
(NULL) when the variable is missing. For {cmd:keep if} this matches native Stata for the
lower-tail and equality idioms ({cmd:x < c}, {cmd:x <= c}, {cmd:x == c}),
but it differs for the upper tail and inequality ({cmd:x > c}, {cmd:x >= c},
{cmd:x != c}): native Stata treats missing as larger than every number and
so {it:keeps} those rows, whereas SQL drops them. {cmd:drop if} removes only
rows whose condition is true; an unknown comparison leaves the row in place.
Likewise
{cmd:gen y = x > c} yields system missing (not 0/1) for rows where {cmd:x}
is missing. The {cmd:if} qualifier of {cmd:gen} and {cmd:replace} is a filter
and follows the same missing-value mode: under the default SQL semantics a
missing comparison excludes the row; under {cmd:statamissing on} it reproduces
native Stata. A bare numeric condition still uses Stata truth in either mode:
zero is false and every nonzero value, including missing, is true. Numeric
operands of {cmd:&}/{cmd:|}/{cmd:!} use the same coercion; a comparison operand
retains the result implied by the selected missing-value mode. Run
{cmd:parqit set statamissing on} to emulate Stata's ordinary numeric missing
ordering ("missing is greater than every number") in filters and assignments;
the documented precision and expression-dialect limits still apply. The literal
idioms {cmd:x == .}, {cmd:x != .}, {cmd:x < .}, {cmd:x >= .} are translated
to IS NULL tests in either mode. Stata represents string missing by
{cmd:""}; SQL NULL and {cmd:""} compare alike in lazy string expressions.

{pstd}{bf:The missing-value warning.} Until you choose a mode, parqit names, in
red, each comparison whose result can differ from native Stata's because a
compared value is missing: {cmd:parqit keep if x > 5} prints
{cmd:warning: x > 5: ...}. In a condition a comparison is named only where
native Stata would make it true for a missing value ({cmd:>}, {cmd:>=} and
{cmd:!=}, or {cmd:<} and {cmd:<=} with the variable on the right; the reverse
under {cmd:!}). An assigned value such as {cmd:gen y = x > 5} is named whenever
a compared value can be missing, since SQL then gives {cmd:.} where Stata gives
0 or 1. The idioms that settle the missing rows stay silent:
{cmd:x > 5 & x < .}, {cmd:x > 5 & !missing(x)}, {cmd:missing(x) | x > 5} and
{cmd:gen y = x > 5 if !missing(x)}. The check reads no data, so it can name a
comparison on a variable that happens to have no missing values.
{cmd:parqit set statamissing on} (Stata's rule) or
{cmd:parqit set statamissing off} (SQL's rule, knowingly) silences it for the
session. It is shown after the command's output and, like any output, is
silenced by {cmd:quietly} and {cmd:capture}. It covers {cmd:keep if},
{cmd:drop if}, {cmd:gen}, {cmd:replace}, {cmd:egen}, {cmd:sample if},
{cmd:count if}, {cmd:list if} and the {cmd:if} of every statistics command.

{pstd}
An unsupported function is a loud, position-anchored error that names the
function — never a silent guess; syntax native Stata rejects ({cmd:||},
{cmd:&&}, uppercase extended missings like {cmd:.A}, malformed numbers) is
rejected here too, with one lenience: a unary plus ({cmd:+x}) is accepted.
Every value whose magnitude reaches 2^1023 is normalized to missing in either
sign; Stata's largest finite value, {cmd:maxdouble()}, remains a value.
{cmd:parqit sql} and {cmd:parqit query} are the escape
hatches.

{pstd}
{cmd:parqit set statamissing} affects expressions translated {it:after} the
setting changes, including read-only {cmd:count if}/{cmd:list if} calls. Lazy
stages already appended retain the SQL semantics under which they were built;
change the setting before adding the relevant filter or assignment if the
pipeline must use Stata missing ordering throughout.

{pstd}The rest of the dialect — the numeric edge contracts, string-function
details, ties in sort keys, extended missings and double-precision evaluation
— is in {help parqit_technical##expressions:the technical reference}.

{pstd}
The numeric edge contracts follow Stata rather than DuckDB defaults. Division
by zero, an invalid power, overflow, {cmd:ln()}/{cmd:log10()} of a nonpositive
value and {cmd:sqrt()} of a negative value produce missing. {cmd:round(x)} and
{cmd:round(x,u)} break exact halves toward +infinity for positive units (so
{cmd:round(-2.5)=-2}); negative units reverse the tie direction and {cmd:u=0}
returns {cmd:x}. {cmd:mod(x,y)} is the
nonnegative remainder and is missing when {cmd:y<=0}. {cmd:min()}/{cmd:max()}
take 2–64 numeric arguments, ignore missing arguments and return missing only
when all are missing. {cmd:missing()}/{cmd:mi()} accept one or more arguments;
{cmd:inlist()} accepts 2–255 same-family arguments. Numeric
{cmd:inrange(x,lo,hi)} treats missing {cmd:x} as outside the range and missing
bounds as unbounded. Three-argument {cmd:cond()} treats a missing numeric
condition as true; its four-argument form selects the fourth branch instead.
Branches must be all numeric or all string.

{pstd}
An order with tied keys is not a total order, so parqit completes it as native
Stata does: by the rows' physical order. A view over Parquet files carries each
row's position in its source (the file, then the row within the file) through
the verbs that keep rows — {cmd:keep}, {cmd:drop}, {cmd:gen}, {cmd:replace},
{cmd:egen}, {cmd:rename}, {cmd:order}, {cmd:sort}/{cmd:gsort}, {cmd:sample}
and {cmd:duplicates drop} with a varlist — and a declared sort breaks its ties
by it. Within a tie, rows come in their order in the files, the same at every
evaluation: {cmd:_n}, {cmd:keep in}, {cmd:list in}, {cmd:collect},
{cmd:save} and the {bf:Obs} of {cmd:duplicates list} agree with each other and
with native Stata after {cmd:use}. A file saved from Stata while sorted on a
non-unique key (auto's {cmd:foreign}) reopens with that sort declared and its
ties in the saved order. A new {cmd:parqit sort} breaks its ties in file
order, which is native {cmd:sort ..., stable} on the data as loaded; when a
sort variable is dropped or replaced, the order it gave is kept, as native.
{cmd:merge}, {cmd:append}, {cmd:joinby}, {cmd:collapse}, {cmd:contract},
{cmd:reshape}, {cmd:pivot}, {cmd:query}, the {cmd:sample} designs and
{cmd:duplicates drop} without a varlist build new rows: after them, and in
views over CSV files or {cmd:parqit sql}, the order within a tie is the
engine's and may differ between evaluations, so add a unique key to
{cmd:parqit sort}/{cmd:gsort} before {cmd:keep in}, {cmd:_n} or a sliced
preview when row identity matters. The same holds for a Parquet source with a
column named {cmd:file_row_number} (or {cmd:file_index}, over several files),
which hides the engine's row positions; {cmd:parqit use} says so.

{pstd}
{cmd:string()} and {cmd:strofreal()} accept one numeric argument and use
Stata's default {cmd:%9.0g} format. {cmd:strlen()}/{cmd:length()} are string
byte lengths here, whereas {cmd:ustrlen()} counts Unicode characters; unlike
native Stata's {cmd:length()}, the numeric-display-width form is not
implemented. {cmd:real()} returns missing for invalid or nonfinite text.
{cmd:upper()}/{cmd:lower()} and their
{cmd:strupper()}/{cmd:strlower()} aliases fold ASCII only, while
{cmd:ustrupper()}/{cmd:ustrlower()} are Unicode-aware. {cmd:subinstr()} supports
the replace-all form whose fourth argument is {cmd:.}. {cmd:substr()} and
{cmd:strpos()} index bytes, like Stata; if a
{cmd:substr()} slice splits a UTF-8 codepoint, parqit returns the replacement
character because DuckDB/Arrow strings must remain valid UTF-8.
Unicode-indexed {cmd:usubstr()} and {cmd:ustrpos()} are not implemented and
fail loudly rather than silently using byte positions.
{cmd:ustrupper()}/{cmd:ustrlower()} apply the engine's simple one-to-one
Unicode case mapping, not ICU's full mapping: {cmd:ustrupper("straße")} is
{cmd:STRAẞE} where native gives {cmd:STRASSE}, and {cmd:ustrlower("İ")} is
a plain {cmd:i} where native keeps a combining dot. {cmd:regexm()} has no
multiline mode: {cmd:^} and {cmd:$} anchor only at the ends of the whole
value and {cmd:.} does not match a newline, whereas native matches
{cmd:"^line1$"} and {cmd:"1.l"} inside {cmd:"line1"+char(10)+"line2"}.

{pstd}{cmd:round()} and {cmd:mod()} operate on the actual binary64 inputs,
without overflowing or rounding a quotient before finding its integer part.
This corrects native numerical failures: {cmd:round(4503599627370497)} keeps
that integer, and {cmd:mod(1e100,3)} is 1. Decimal-looking binary64 values are
not exact decimal fractions: {cmd:round(.25,.1)} is .2, and {cmd:mod(1,.1)}
is approximately .09999999999999995, not zero. The rounded exact remainder of
{cmd:mod(7,.00001)} is approximately 9.99999999942738e-06. These intentional
mathematical corrections can differ from native Stata's results.

{pstd}
Extended-missing literals {cmd:.a}-{cmd:.z} are rejected in lazy expressions.
At the Parquet boundary their category identity has already collapsed to the
single ordinary missing value, so accepting them would fabricate a distinction
the view cannot observe. Use {cmd:missing(x)} or compare with {cmd:.}.

{pstd}
Arithmetic operators evaluate in double precision, and guarded out-of-range
values become missing: an overflowing
result ({cmd:exp(800)}, {cmd:1e300*1e300}) or an out-of-range literal
({cmd:1e309}) is {cmd:.} in filters, assignments and aggregates alike —
never an IEEE infinity. Untyped numeric {cmd:gen} results are double;
control their storage with a typed {cmd:parqit gen} (e.g.
{cmd:parqit gen byte flag = ...}); native Stata's untyped {cmd:gen} default
is {cmd:float}. For an explicit {cmd:float} target, a finite value outside
Stata's ±1.70e38 storage range becomes missing, as in native assignment.
A {cmd:float} operand is promoted when needed for numeric evaluation, including
comparisons with integer columns/literals above 2^24 and the alternatives of
{cmd:min()}, {cmd:max()} and {cmd:cond()}. Float-exact comparisons and
integer-only key filters retain their exact, narrow predicates. In particular:
{cmd:x == 0.1} is false for a float {cmd:x} holding 0.1, and
{cmd:x == float(0.1)} is the native idiom. {cmd:float(x)} rounds {cmd:x} to
float precision (a value beyond ±1.70e38 is missing). {cmd:round()} and
{cmd:mod()} normalize non-finite results before another expression uses them,
so {cmd:missing(round(x,u))} and {cmd:round(x,u)==.} agree.
Numeric literals first take their Stata binary64 value; fractional SQL
literals are typed directly as DOUBLE to avoid an intermediate decimal cast.
Bare integer/decimal columns retain their stored precision in comparisons,
including against a floating column. Thus DOUBLE 2^53 does not equal BIGINT
2^53+1. A DECIMAL just above .5 does not equal the literal .5, even if both
would round to .5 upon collection. Generate an explicit double copy when that
rounded representation is the intended comparison domain.
Date functions floor a fractional day count (like Stata:
{cmd:day(-0.5)} is 31) and an out-of-range argument is row-local missing.
One documented dialect difference: {cmd:regexm()} runs on DuckDB's RE2
engine, which understands {cmd:\d \w \s}, {cmd:{c -(}n,m{c )-}} and
non-greedy quantifiers that Stata's own {cmd:regexm} treats as literals —
patterns restricted to the shared syntax normally agree on single-line text.
The newline and Unicode differences above remain relevant.


{marker types}{...}
{title:Type mapping}

{pstd}{it:Integers and floating point.} At the Stata-memory boundary,
{cmd:BOOLEAN} becomes {cmd:byte} 0/1. Signed and unsigned integers use the
smallest exact Stata integer storage that contains the observed range and
otherwise {cmd:double}; an all-missing integer column becomes an all-missing
{cmd:byte} with a note. {cmd:UINT32} values above Stata {cmd:long}'s ceiling
survive as {cmd:double}. {cmd:BIGINT}/{cmd:UINT64}/{cmd:HUGEINT}/{cmd:UHUGEINT}
values beyond 2^53 and wide {cmd:DECIMAL} values are outside the protected
consecutively exact integer range and are {bf:refused} by default,
even when a particular larger integer is exactly representable
({cmd:r(198)}, naming every such column, nothing staged).
{opt int64(round)} loads them as {cmd:double} with the explicit precision note
(never as silent missing), {opt int64(string)} loads only those columns as
exact decimal text sized by the observed maximum, and
{cmd:parqit set int64 refuse|round|string} moves the session default. The
eager text conversion keeps preserved extended-missing codes as the strings
{cmd:.a}-{cmd:.z}, with ordinary nulls becoming empty strings; corrupt codes
still refuse the load. The
trigger is the observed data, not the declared type: a 64-bit or 128-bit
integer column whose values all fit is read exactly as before, with no option
and no precision-loss note. The full signed and unsigned 128-bit ranges are
accepted with {opt int64(string)} or {opt int64(round)}; protective refusal
also handles their extreme values without an arithmetic overflow.
For wide decimals, the threshold test rounds the observed extrema to integers
using the engine's DECIMAL-to-HUGEINT cast. Thus a magnitude of 2^53+0.4 does
not trigger this guard, while 2^53+0.5 does. Decimal-to-double conversion
can still round fractional values below the threshold and carries its own
conversion note. A
preview ({cmd:parqit head}, {cmd:parqit list}) always shows the exact digits.
A lazy plan keeps these source numerics in DuckDB until a Stata boundary is
actually crossed, so join keys, {cmd:parqit save} and engine-side statistics
are exact whatever the option says.

{pstd}{it:Round-trip storage.} When a file was written by parqit, its metadata
preserves the original storage floor (a {cmd:byte} comes back {cmd:byte}, a
{cmd:long} comes back {cmd:long}, and a {cmd:str8} keeps width 8) unless the
observed values require a wider safe type. A plain display format
({cmd:%9.2f}, {cmd:%8.0g}) never widens storage; only a genuine date/period
format keeps integer storage at {cmd:int} or wider so its count fits.
Foreign strings are sized by maximum UTF-8 byte length: up to 2,045 bytes use
{cmd:str#}, longer values use {cmd:strL}, and empty/all-null columns use
{cmd:str1}. {cmd:ENUM}, {cmd:UUID} and logical {cmd:JSON} load as text.

{pstd}{it:Dates and times.} {cmd:%td} variables (and the old-style {cmd:%d}
synonyms) are {cmd:DATE} on disk,
{cmd:%tc} variables are {cmd:TIMESTAMP}, and {cmd:%tm %tq %th %tw %ty %tb}
stay integer period counts — never mis-scaled calendar dates. A parqit-written
{cmd:%td} or {cmd:%tc} column restores its recorded storage type on both the
eager and lazy paths (an {cmd:int} {cmd:%td} comes back {cmd:int}; a {cmd:float}
{cmd:%tc} comes back {cmd:float} when a scan proves every value exactly
representable as a float — on eager, lazy and view-save reads — and
{cmd:double} otherwise) unless the observed values require wider. Foreign
{cmd:TIME} values become milliseconds since midnight with
{cmd:%tcHH:MM:SS}; nanosecond time/timestamps are truncated (toward the earlier
millisecond, including before 1970) with a note. This includes finite
nanosecond timestamps near the signed 64-bit limits; negative values use
floor, not truncation toward zero. A timezone-aware timestamp keeps its UTC instant;
a time-of-day offset is discarded with a note. Inside a pipeline dates are
their Stata day or millisecond counts, so date arithmetic is ordinary
arithmetic. Saving a fractional day, millisecond or period count rounds to the
nearest integer using native Stata's exact-half rule (toward +infinity), on
both memory and lazy paths, and names the affected column.

{pstd}Finite microsecond timestamps near the signed 64-bit limits also use
integer flooring without an intermediate subtraction overflow. Their Stata
millisecond counts can exceed 2^53: a lazy {cmd:collect} may require
{opt int64(round)} even when the particular count is exactly representable.
A count that is not exactly representable in binary64 is always refused.
If flooring the earliest instant to milliseconds puts it outside the
timestamp writer's range, {cmd:save} refuses and preserves an existing target.

{pstd}{it:Special and unsupported values.} IEEE NaN loads as missing;
{cmd:±Inf}, and any finite magnitude at or above Stata's missing sentinel
(≈ 8.99e307), load as missing with a per-column note. Source DATE/TIMESTAMP
infinities, including nanosecond timestamps, have no Stata representation and
are refused explicitly on eager and lazy paths. Convert them to text or map
them to ordinary missing explicitly in SQL if that is the intended meaning.
A foreign float32 column
whose finite range exceeds Stata float's ±1.70e38 ceiling widens to
{cmd:double}. String values containing NUL are truncated at the first NUL when
loaded into Stata, with a per-column note; a lazy Parquet-to-Parquet save does
not cross that boundary. Types with no Stata representation — {cmd:NULL},
{cmd:BLOB}, {cmd:BIT}, {cmd:INTERVAL}, {cmd:LIST}/{cmd:ARRAY},
{cmd:STRUCT}/{cmd:MAP}/{cmd:UNION}, {cmd:BIGNUM}, {cmd:GEOMETRY} and
{cmd:VARIANT} — are dropped with a reason; a result containing no loadable
columns is refused. A {cmd:BLOB} can be loaded on request:
{opt binary(text)} decodes the bytes as UTF-8 (and refuses the read loudly,
naming the column, if any row is not valid UTF-8 — never a replacement
character), {opt binary(hex)} loads two UPPERCASE hex digits per byte. The
column is then text sized like any other ({cmd:strL} beyond 2,045 bytes) and
carries a note naming the mode. A lazy view's columns are fixed when it is
opened, so the option belongs to {cmd:parqit use using}; giving it to
{cmd:parqit collect} is refused with a message saying so.

{pstd}{it:Column names.} At the Stata boundary, invalid name characters become
underscores, a leading digit or reserved word gains an underscore (only
{cmd:strL} and the {cmd:str#} family are reserved — a plain {cmd:str} is a legal
name), names are
limited to 32 Unicode code points, empty names become {cmd:v}{it:position}
(with a note; there is no source name to keep), and collisions gain
deterministic numbered suffixes. The original column name is
retained in {cmd:char var[src_name]} and in the {cmd:parqit.*} metadata; a later
{cmd:parqit save} writes the Stata names (the original stays recoverable from
{cmd:parqit.chars}). This recovery works for a single file, a glob, a Hive tree
and a {opt relaxed} union (parqit predicts the engine's union of the files'
columns exactly and maps every engine name back to the true one), and for
DuckDB's nested dedup shapes (a file carrying {cmd:a}, {cmd:a_1} and {cmd:A} is
read back as those three names). Under {opt relaxed} the engine matches the
files' column names case-insensitively: a later file's column that differs
only by case from an earlier file's ({cmd:NUEMP} after {cmd:nuemp}) is unioned
into that column and a {cmd:note:} says so; when such a match would split one
name across two columns (one file carrying both {cmd:nuemp} and {cmd:NUEMP},
another only {cmd:NUEMP}) the read is refused — read the files separately or
rename the columns upstream. A Hive tree whose partition key differs only by
case from a column inside the files ({cmd:g=} directories over a file column
{cmd:G}) is refused on every path, because the engine would replace that
column's values with the key; a key that exactly duplicates a file column is
read with a {cmd:note:} (the directory value is used).
Names that differ only by case ({cmd:nuemp} and {cmd:NUEMP}) are distinct
variables in Stata and distinct columns in Parquet, but not in the engine, whose
identifiers are case-insensitive: parqit keeps them exact at both boundaries —
a save writes both names into the file, {cmd:parqit use ..., clear} and
{cmd:parqit collect} restore both — while inside a lazy view the second is
addressed by a numbered alias ({cmd:NUEMP_1}, reported when the view opens and
by {cmd:parqit describe}) that {cmd:collect} and {cmd:save} translate back — a
selection varlist ({cmd:parqit use} {it:varlist}, {cmd:keep}/{cmd:drop}/
{cmd:order}, {opt partition_by()}) accepts either the alias or the exact name.
Creating a lazy name that differs only by case from a live one is refused, and
{opt partition_by()} is not available for such datasets. A {cmd:parqit sql}
result with case-clashing output names ({cmd:SELECT 1 AS a, 2 AS A}) is handled
the same way and reported with a {cmd:note:}; a raw {cmd:SELECT *} over a
case-clashing file arrives with DuckDB's own dedup names (a note flags them) —
open the file with {cmd:parqit use} to keep the exact names.
A source column name containing a NUL byte is refused on every input surface;
truncating it could select the wrong column and is never allowed.


{marker settings}{...}
{title:Settings, raw SQL and diagnostics}

{pstd}{bf:Engine settings.} {cmd:parqit set} takes one of eight names and a value:

{p 8 12 2}{cmd:parqit set statamissing on}|{cmd:off}{space 4}expression missing-value mode{p_end}
{p 8 12 2}{cmd:parqit set int64} {it:mode}{space 12}refuse|round|string for integers beyond 2^53{p_end}
{p 8 12 2}{cmd:parqit set encoding} {it:name}{space 9}the code page of legacy text that declares none, for the
session (windows-1252 until set; an {opt encoding()} option still wins; see
{help parqit_technical##encoding:Text encodings}){p_end}
{p 8 12 2}{cmd:parqit set fill_threads} {cmd:auto}|{it:#}{space 3}workers that fill Stata's memory on
{cmd:use}/{cmd:collect} ({cmd:auto} = environment override, otherwise one per available CPU
on reads with at least 50,000 rows or 2 million cells;
{cmd:1} = serial; {cmd:0} aliases {cmd:auto}; any positive count up to those CPUs,
a larger one is clamped and said; an explicit positive count outranks
{cmd:PARQIT_FILL_THREADS}; reported by {cmd:parqit version} as
{cmd:r(fill_threads)}){p_end}
{p 8 12 2}{cmd:parqit set stream_buffer_mb} {cmd:auto}|{it:#}{space 1}cap in megabytes on the
engine result buffered ahead of that fill ({cmd:auto} = environment override, otherwise sized per read;
{cmd:0} restores the engine's own default before fetching; an explicit number
outranks {cmd:PARQIT_STREAM_BUFFER_MB}; reported as
{cmd:r(stream_buffer_mb)}). Malformed, negative or overflowing environment
values are ignored, leaving automatic per-read sizing; explicit session
values are validated by {cmd:parqit set}.{p_end}
{p 8 12 2}{cmd:parqit set threads} {it:#}{space 14}engine threads, 1 up to the CPUs available to this
process (the default; a larger number is clamped to them and said){p_end}
{p 8 12 2}{cmd:parqit set memory_limit} {it:value}{space 4}e.g. {cmd:8GB}{p_end}
{p 8 12 2}{cmd:parqit set tempdir} {it:path}{space 9}spill directory for out-of-core
execution (warns if the directory does not exist yet){p_end}

{pstd}
{cmd:statamissing} defaults to {cmd:off}. {cmd:threads} controls DuckDB query
execution, not the separate fill pool: it defaults to the CPUs available to
this process (on Linux the affinity mask, so a cluster job's allocation is
honoured — the engine's own default would ignore it) and accepts any whole
number from 1 up to that count; a larger number is clamped to it with a
{cmd:note:}, never refused, so a script written for a bigger machine still
runs. {cmd:parqit version} reports the count as {cmd:r(cpus)} and the engine
threads in force as {cmd:r(threads)}. {cmd:memory_limit} accepts DuckDB size
strings such as {cmd:8GB}; {cmd:tempdir} accepts a literal path (quote paths
containing spaces). The settings apply to this loaded plugin session and survive view changes.
{cmd:discard} refreshes ado programs but does not guarantee a reset of the loaded plugin.
Restart Stata to load a rebuilt plugin or restore fresh-session defaults.
Close views explicitly with {cmd:parqit close _all}. A nonexistent temp directory
is warned about immediately but not forbidden, because it may be created before
the first spill.

{pstd}{bf:Out-of-core execution still uses resources.} {cmd:memory_limit}
budgets DuckDB's buffer manager; it is not a ceiling for the complete Stata
process. Reserve memory for Stata, collected or preview results, and other
engine allocations, plus disk space for spill, bridges and staged output.
Not every query can spill all its state. Use {cmd:show}/{cmd:explain} to inspect
the plan, reduce unnecessary columns and rows before expensive operations
when that preserves the intended result, and collect only what fits.

{pstd}{bf:Raw SQL.} {cmd:parqit sql} accepts a DuckDB {it:query} that returns a
table; it is nested as a subquery, so DDL/DML statements are not this command's
contract. Without {opt clear}, it opens or replaces {cmd:default} (or
{opt name()}) as a lazy view and leaves the current dataset untouched. With
{opt clear}, {opt name()} is invalid: the query is staged, collected atomically, and the
{cmd:default} view is committed only after the load succeeds. Result names and
types cross the same Stata boundary as file input; unsupported columns are
dropped with a warning and a query with no loadable columns is refused.
{cmd:parqit query} instead appends a raw DuckDB clause after the current view's
{cmd:SELECT ... FROM ...}; use it for {cmd:WHERE}, {cmd:QUALIFY}, {cmd:ORDER BY}
or {cmd:LIMIT} constructs that are awkward in the Stata grammar. It does not
translate Stata expressions or change the view's declared projection, and it
bind-validates the candidate before changing the plan.

{pstd}{bf:View and installation diagnostics.} {cmd:parqit open _data} snapshots
memory to a package-owned temporary Parquet bridge, opens/replaces the named
view (default {cmd:default}), leaves memory in place, and reports any extended-
missing collapse or fractional-date rounding caused by that snapshot.
{cmd:close} releases a view and deletes a bridge only after its last dependent
view closes, and a copied view depends on every bridge of its source;
{cmd:close _all} closes every view and performs the final owned-
bridge sweep. {cmd:show} prints compiled SQL; {cmd:explain} asks DuckDB for its
plan. {cmd:path} resolves a path to an absolute spelling and reports whether it
exists, without creating it. {cmd:version} reports the parqit and embedded
DuckDB versions and identifies DuckDB as the parallel execution backend.
{cmd:selftest} checks the ado/plugin codec, opens the engine, and writes/reads a
small metadata-bearing Parquet file in process. No separate OpenMP runtime or
Windows OpenMP DLL is required. {cmd:parqit set threads} controls DuckDB's workers.
{cmd:menu} adds the reproducible dialogs to {bf:User > parqit} once per GUI
session and refuses console/batch sessions.

{pstd}The environment knobs outside {cmd:parqit set} ({cmd:PARQIT_PLUGIN_PATH},
{cmd:PARQIT_NOTIPS}, {cmd:PARQIT_FILL_THREADS},
{cmd:PARQIT_STREAM_BUFFER_MB}, {cmd:PARQIT_FETCH_MATERIALIZED}) are described in
{help parqit_technical##environment:the technical reference}.

{pstd}Use matching ado and plugin files and restart Stata after an update.
Before every command the ado-files check that the plugin is of their own
release and numerical protocol; a plugin of another release is refused with
{cmd:r(498)} and a message that names both releases. A
{cmd:PARQIT_PLUGIN_PATH} global is unnecessary when the matching plugin is
already found through the adopath.

{pstd}For {cmd:parqit sql}, trailing statement terminators ({cmd:;}) are
optional and ignored; semicolons inside the SQL text are preserved.{p_end}

{pstd}In SQL, names go in double quotes and text values in single quotes. A
column whose name is a reserved SQL word must be quoted to be read: auto.dta's
{cmd:foreign} is the classic case, so write
{cmd:parqit sql `"SELECT make, "foreign" FROM read_parquet('auto.parquet')"'}.
Other reserved words that are plausible variable names include {cmd:order},
{cmd:group}, {cmd:end}, {cmd:default}, {cmd:check}, {cmd:case}, {cmd:unique},
{cmd:table}, {cmd:window}, {cmd:limit}, {cmd:offset} and {cmd:both}. When the
engine stops at such a word, {cmd:parqit sql} and {cmd:parqit query} say so and
show the quoting. The lazy verbs quote every name themselves.{p_end}


{marker environment}{...}
{title:Environment}

{pstd}
The following knobs live outside {cmd:parqit set}. The Stata global
{cmd:PARQIT_PLUGIN_PATH} points the loader at a locally built plugin and
takes precedence over the adopath search for {cmd:parqit.plugin};
{cmd:global PARQIT_NOTIPS 1} mutes the one-line performance tips; and the
operating-system environment variable {cmd:PARQIT_FILL_THREADS} controls
the parallel memory fill (overridden by a positive {cmd:parqit set fill_threads} count),
{cmd:PARQIT_STREAM_BUFFER_MB} caps how much result
the engine keeps buffered ahead of that fill (overridden by an explicit numeric
{cmd:parqit set stream_buffer_mb}) and {cmd:PARQIT_FETCH_MATERIALIZED=1}
restores the earlier materialised fetch (see
{help parqit_technical##perf:Performance tips}).
The operating-system variable {cmd:PARQIT_SAVE_NOARROW} selects the batched
memory writer (see {help parqit_technical##materialisers:Materialisers}).

{pstd}No plugin-path global is needed when matching package files are on the
adopath. The ado and plugin verify their release and numerical protocol before
operating; files of another release are refused. After updating a loaded
package, restart Stata so cached programs and the loaded binary belong to the
same release: a running Stata can keep the plugin it loaded first, even after
{cmd:discard} or {cmd:clear all}.


{marker perf}{...}
{title:Performance tips}

{pstd}
parqit is fastest when data stays on disk and only the final result moves into
Stata. The biggest single cost in any Stata↔columnar bridge is moving rows in
and out of Stata's memory through the plugin interface, so the patterns below
pay off most on large data. parqit prints a one-line {it:tip} when it detects one
of these (e.g. a large {cmd:mergein}); {cmd:global PARQIT_NOTIPS 1} silences them.

{dlgtab:Joining in-memory data with a disk file}

{pstd}
If your data is already in Stata's memory and you want to merge or append a
{it:small} lookup that lives on disk, keep your data put: {cmd:parqit mergein} /
{cmd:parqit appendin} run a {it:native} {help merge} / {help append}, reading only
the columns you ask for from the disk side. The engine still reads that disk
side, but your in-memory data never crosses into DuckDB and back.

{phang2}{cmd:. parqit mergein m:1 firm_id using firms.parquet, keepusing(tfp)}{p_end}
{phang2}{cmd:. parqit appendin using more_rows.parquet}{p_end}

{pstd}
When {it:both} sides are large, it is often faster to let DuckDB do the join
out of core and bring back only the result. DuckDB's hash join avoids sorting
either dataset, so on big-on-big it can beat Stata's native sort-merge even
after the cost of moving the in-memory side across. If both files are on disk:

{phang2}{cmd:. parqit use using big_master.parquet}{p_end}
{phang2}{cmd:. parqit merge m:1 id using big_using.parquet, keepusing(...)}{p_end}
{phang2}{cmd:. parqit collect, clear}{space 20}({it:only the joined result enters Stata}){p_end}

{pstd}
If the large side you want to join is in Stata's memory (not on disk), promote
it once with {cmd:parqit open _data} and join out of core, then collect:

{phang2}{cmd:. parqit open _data}{space 27}({it:snapshots the in-memory data to a view}){p_end}
{phang2}{cmd:. parqit merge m:1 id using big_using.parquet, keepusing(...)}{p_end}
{phang2}{cmd:. parqit collect, clear}{p_end}

{pstd}
The trade-off: {cmd:parqit open _data} writes a temporary bridge first (about the
cost of one {cmd:parqit save}), so for a {it:small} lookup the native
{cmd:parqit mergein} is usually faster, while for {it:big-on-big} the out-of-core
join usually wins.

{dlgtab:Other patterns}

{phang}o {bf:Write without loading.} With a view open, {cmd:parqit save} runs the
pipeline and writes Parquet directly without loading the result into the current
dataset. Use it instead of
{cmd:parqit collect} followed by a native {cmd:save}/export when you only need the
file on disk.{p_end}

{phang}o {bf:Filter and project early.} Put {cmd:parqit keep}/{cmd:parqit keep if}
before expensive operations when that preserves the intended result. Verb
order is semantically significant: filtering before and after {cmd:collapse},
{cmd:sample} or a row-number calculation can select different data. DuckDB
pushes work toward the scan only when its optimizer can preserve meaning.{p_end}

{phang}o {bf:Distinguish filtering from row-group pruning.} Floating-point
normalization and date conversion can leave expression filters that the pinned
engine evaluates during the scan but cannot use to prune row groups from their
statistics. Simpler predicates, including string predicates the optimizer can
simplify, may prune. {cmd:parqit explain} shows which form reaches the scan;
the presence of a filter alone does not prove that row groups were skipped.{p_end}

{phang}o {bf:Read into memory once.} If a workflow collects the same view more
than once, collect it once and work on the result; each {cmd:parqit collect}
re-executes the pipeline.{p_end}

{phang}o {bf:Set a shared-machine memory budget.} The pinned DuckDB engine's
default buffer-manager limit is 80% of the memory it detects. On a shared server
or scheduler allocation, set an explicit engine budget with
{cmd:parqit set memory_limit} (for example {cmd:8GB}) and, when useful, a spill
location with {cmd:parqit set tempdir}. This does not cap the complete process:
Stata frames, collected results and some engine allocations are outside this
budget. Sorting, joining and exact percentiles may need substantial scratch
space; complex SQL aggregate states may not spill. Smaller result output alone
does not guarantee a cheap query.{p_end}

{phang}o {bf:Choose the fill workers, or force a serial fill.} Reads of 50,000+
rows, or of 2 million+ cells, fill Stata's memory with one worker thread per
CPU available to this process (on Linux the affinity mask — a SLURM/cgroup
allocation or {cmd:taskset} — elsewhere the hardware count; nothing is
hard-coded); each worker converts the engine chunk it fills and stores it cell
by cell. {cmd:parqit set fill_threads} {it:#} chooses the count for the session
({cmd:1} = the single-threaded path, for example on a platform you have not
yet verified; any number up to the available CPUs; a larger number is clamped
to them and said; {cmd:auto} returns to the environment override, if set,
otherwise the size/CPU rule), and {cmd:parqit version}
reports it as {cmd:r(fill_threads)} next to {cmd:r(cpus)}. The
{it:operating-system} variable {cmd:PARQIT_FILL_THREADS} does the same from
outside Stata ({cmd:export PARQIT_FILL_THREADS=0} before launching; read via
{cmd:getenv}, so a Stata {cmd:global} does not reach it) and is outranked by the
explicit positive session count. A session value of {cmd:0} is an alias for
{cmd:auto}, whereas environment {cmd:PARQIT_FILL_THREADS=0} forces serial.
Historical measurements on a 48-core machine as a matrix of engine threads
(16/24/48) by fill workers, a 58.8M x 9 numeric read takes 1.2-1.3 s with 8
workers, 0.9-1.0 s with 16, 0.7-0.8 s with 24-32 and 0.62-0.73 s with 48 at
every tested engine thread count; no oversubscription penalty was observed
in that workload, while a string-heavy read was scan-bound and flat. The
parallel and serial fills are byte-identical.{p_end}

{phang}o {bf:The fill reads a streamed result.} The engine's chunks are handed
to the fill directly, instead of first being collected into a materialised
result and copied out again. The buffer that holds them is sized for each read
from the result's estimated size to avoid repeated buffer stalls. This is a
soft engine-buffer cap, not a limit on the complete process; underestimated
variable-width payloads and execution state can require additional memory.
{cmd:parqit set stream_buffer_mb} {it:n} (in-session; reported by
{cmd:parqit version} as {cmd:r(stream_buffer_mb)}) or the operating-system
variable {cmd:PARQIT_STREAM_BUFFER_MB=}{it:n} caps that buffer at {it:n}
megabytes: on plain reads peak memory drops by up to about half, at the price
of a slower, stop-and-go fill once the cap falls below roughly half the result;
{cmd:0} restores the engine's own default before each fetch (normally 1 MB),
including after a previous larger buffer. In the measured wide numeric reads,
that setting used less memory and was slower. {cmd:auto} defers to the environment
override, if set, otherwise per-read sizing. An explicit numeric session setting
outranks the variable. A malformed or negative environment value, or one
whose megabyte-to-byte conversion would overflow, is ignored and automatic
per-read sizing is used. {cmd:PARQIT_FETCH_MATERIALIZED=1}
restores the earlier materialised fetch. The variables are read via
{cmd:getenv}, like {cmd:PARQIT_FILL_THREADS}, and none of these changes any
value: the fetches are byte-identical under every cap.{p_end}


{marker dialogs}{...}
{title:Menus and dialogs}

{pstd}{bf:Point and click.} The dialogs cover every public subcommand listed
above (apart from {cmd:parqit menu}, which installs the menu itself); see
{help parqit_technical##dialogs:Menu}.

{pstd}In GUI Stata, {cmd:parqit menu} adds one submenu to Stata's {bf:User}
menu for the session; add that line to your {help profile}.do to keep it
across sessions. Repeating {cmd:parqit menu} is idempotent. If another command
explicitly runs {cmd:window menu clear}, Stata removes {it:every} package's
menu additions and exposes no package-local query/removal API; restore parqit's
entry with {cmd:global PARQIT_MENU_ON} followed by {cmd:parqit menu}. parqit
never calls {cmd:window menu clear} itself. Each entry opens a dialog that
builds and runs an ordinary {cmd:parqit} command, echoed to the Results and
Review windows like a typed
command, so every click is reproducible in a do-file. The dialogs are also
listed in the Viewer's {bf:Dialog} menu of this help file, and each can be
launched directly with {cmd:db} {it:name}.

{phang2}{bf:User > parqit > Read data (lazy view or into memory)...}{p_end}
{p 12 12 2}({cmd:db parqit_read}) {cmd:use} — a lazy view, or the data into
memory with {opt clear} — {cmd:open _data}, and {cmd:path}; {bf:Populate}
lists the variables recorded in the Parquet footer of the source, and
{bf:Describe} runs {cmd:describe} on it. These two footer-inspection buttons
are disabled for recognized delimited-text, Stata, Excel, SPSS and R inputs; those
sources can still be opened, and variable names can be typed. {bf:Browse}
lists every supported input type together, or one type at a time. The
{opt int64()}, {opt binary()}, {opt filename()}, {opt csv()} and {opt object()}
options have their own fields ({opt csv()} a free-text field for the
delimited-text reader's sub-options, such as {cmd:delim(;) header(off)};
{opt object()}, enabled for an R file, names the data frame to read). The integer selector's
{bf:default} choice inherits the session setting; selecting {bf:refuse},
{bf:round} or {bf:string} emits that explicit option. Likewise the encoding
selector's {bf:default} emits no {opt encoding()}: delimited text is read as
UTF-8 (UTF-16 by its byte-order mark or its pattern), {cmd:.dta} text in the session
code page (windows-1252 unless {cmd:parqit set encoding}), an SPSS file in the
code page it declares, and an R file in the encoding R recorded. The field lists
the common code pages of every script and takes any other name typed in,
{it:name}{cmd:, all} included.{p_end}

{phang2}{bf:User > parqit > Describe and explore data...}{p_end}
{p 12 12 2}({cmd:db parqit_explore}) {cmd:describe}/{cmd:glimpse} of the view
or of a file, {cmd:ds}, {cmd:lookfor}, {cmd:codebook}, {cmd:head}, {cmd:list},
{cmd:count}, {cmd:misstable} [{cmd:patterns}], {cmd:levelsof}, {cmd:distinct}
and {cmd:duplicates report}/{cmd:list} (both with every variable when the
variables are left empty): one list of operations, every one an
engine-side query; {cmd:distinct} offers {opt joint} and {opt missing},
{cmd:describe} of a file offers {opt labels} and {opt notes}, and the
{cmd:if} expression field serves every operation that takes one.{p_end}

{phang2}{bf:User > parqit > Summary statistics, tables, and correlations...}{p_end}
{p 12 12 2}({cmd:db parqit_stats}) {cmd:summarize} [{opt detail}],
{cmd:tabulate} one- and two-way, {cmd:tabstat} with the statistics chosen by
check boxes, {opt by()} and {opt save}, {cmd:correlate}/{cmd:pwcorr} and
{cmd:histogram}. Tabulation has separate row and optional column fields,
{opt nolabel} for numeric codes, and row/column percentages only when the
column field is filled; {cmd:correlate}/{cmd:pwcorr} use every numeric variable
when the field is left empty, and one {cmd:if} expression applies to
whichever statistic is chosen. Calculation pickers show numeric view variables;
the tabulation and {opt by()} pickers also show strings. Tooltips explain
the group/bin limits and that {cmd:tabstat, save} returns matrices.{p_end}

{phang2}{bf:User > parqit > Keep or drop observations, or draw a sample...}{p_end}
{p 12 12 2}({cmd:db parqit_filter}) {cmd:keep if}, {cmd:drop if}, {cmd:keep in}, {cmd:drop in}
({cmd:f}, {cmd:l} and negative bounds accepted) and {cmd:sample}, with its
{cmd:if} frame, strata, whole clusters and indicator; the
{bf:Create...} button opens Stata's expression builder.{p_end}

{phang2}{bf:User > parqit > Keep, drop, order, sort, or rename variables...}{p_end}
{p 12 12 2}({cmd:db parqit_vars}) {cmd:keep}, {cmd:drop}, {cmd:order},
{cmd:sort}, {cmd:gsort}, {cmd:rename (oldlist) (newlist)} and
{cmd:duplicates drop}.{p_end}

{phang2}{bf:User > parqit > Create or change variables...}{p_end}
{p 12 12 2}({cmd:db parqit_gen}) {cmd:gen} (with a storage type and an
{cmd:if} qualifier), {cmd:egen} (function and {opt by()}) and
{cmd:replace} on the view, and {cmd:spssencode} on the data in memory (the
labelled numeric version of a string variable read from an SPSS file, or from
an R file with haven labels).{p_end}

{phang2}{bf:User > parqit > Collapse, contract, pivot table, or reshape...}{p_end}
{p 12 12 2}({cmd:db parqit_pivot}) {cmd:collapse} and {cmd:pivot} share two
statistic rows plus free additional specifications; {cmd:contract};
{cmd:reshape long}|{cmd:wide}.{p_end}

{phang2}{bf:User > parqit > Combine datasets (merge, append, joinby)...}{p_end}
{p 12 12 2}({cmd:db parqit_combine}) lazy {cmd:merge}, {cmd:append} (several
sources) and {cmd:joinby} over files, globs, Hive directories or
{cmd:view:}{it:name}; native {cmd:mergein}/{cmd:appendin} for data already
in memory, with native Stata's merge and append options, an {opt int64()}
selector and an {opt encoding()} field on the {bf:Options} tab. Each option is
enabled where the command takes it: {opt nonotes} for every merge and append,
{opt nolabel} for {cmd:mergein} and {cmd:appendin}, the other merge options for
{cmd:mergein}. The integer selector applies
only to the memory routes; the encoding field, which names the encoding of the
using file's text that is not UTF-8, applies to every route. The
additional-sources field is raw Stata source-list syntax: compound-quote each
path that contains spaces or commas. {bf:Browse} offers the same file types
as the read dialog.{p_end}

{phang2}{bf:User > parqit > Save as Parquet or collect into memory...}{p_end}
{p 12 12 2}({cmd:db parqit_write}) four explicit choices: save the selected
view to Parquet (the initial choice), save Stata memory with {opt data},
convert an SPSS or R file with {cmd:parqit save} {it:newfile} {cmd:using}
{it:file} [{opt replace} {opt compression()} {opt compression_level()}
{opt encoding()} {opt object()}] (see {help parqit_technical##spss:SPSS files} and
{help parqit_technical##rdata:R data files}; the code page {bf:declared} keeps the one the
file records, and {bf:R object} names the data frame of an {cmd:.RData} that
holds several), or
{cmd:collect} [{opt clear} {opt int64()}]. The integer selector is enabled
for collect; {bf:default} inherits the view's option or the session setting.
View save and collect use
{cmd:parqit view} {it:name}{cmd::} to name the view shown in the context line;
closing it cannot silently redirect a save to memory. Refresh after switching
views to select a different target. Saving offers
{opt replace} (an existing file asks first, as in Stata's save dialog),
{opt compression()}, {opt compression_level()}, {opt partition_by()},
{opt partitions()}, {opt chunk()}, {opt encoding()}, {opt data}, {opt copysource}
and {opt xmissing};
{bf:Populate} lists variables from the view, or from the dataset in memory
when {opt data} is selected.{p_end}

{phang2}{bf:User > parqit > Views, SQL, and engine settings...}{p_end}
{p 12 12 2}({cmd:db parqit_views}) buttons that report on the current view at
once ({cmd:views}, {cmd:show}, {cmd:explain}, {cmd:describe}, {cmd:ds},
{cmd:version}, {cmd:selftest}); the actions {cmd:view}, {cmd:close},
{cmd:view} {it:name}{cmd::} {it:command}, a copy of a view ({cmd:use} ...
{cmd:using view:}{it:name}), {cmd:sql}, {cmd:query} and all eight
{cmd:set} options: {cmd:statamissing}, {cmd:int64}, {cmd:encoding}, {cmd:threads},
{cmd:fill_threads}, {cmd:stream_buffer_mb}, {cmd:memory_limit}, {cmd:tempdir}.{p_end}

{phang2}{bf:User > parqit > Version}, {bf:Self-test}, {bf:Help on parqit}, {bf:Technical reference}{p_end}
{p 12 12 2}run {cmd:parqit version}, {cmd:parqit selftest} and
{cmd:help parqit} and {cmd:help parqit_technical} directly.{p_end}

{pstd}Conventions shared by the dialogs, following Stata's own: a
context line identifies the view or Stata memory. {bf:Refresh} updates that
context and the relevant variable pickers after a view switch; the write dialog
uses the selected view shown there. Context updates are queued until Stata is
ready and preserve the existing {cmd:r()} results. Help buttons open the relevant
manual section. A
{bf:Populate} button fills the variable pickers on demand from the current
view, from the dataset in memory when the write dialog selects {opt data}, or
from the Parquet footer of the file named in the dialog, exactly as Stata's
{cmd:use}, {cmd:describe} and {cmd:merge} dialogs populate from a dataset on
disk (the pickers are editable, so names may also be typed); the
{bf:Create...} buttons open Stata's expression builder, whose variable list is
that of the data in memory, so type the view's variable names into the
expression; a dialog remembers the last settings submitted with {bf:OK} or
{bf:Submit} for the session ({bf:Cancel} discards changes), and the {bf:R}
button resets them.{p_end}


{marker results}{...}
{title:Stored results}

{pstd}{it:Opening and materialising.} Eager {cmd:parqit use ..., clear} and
{cmd:collect} return scalars {cmd:r(N)} and {cmd:r(k)}. Lazy {cmd:use} returns
{cmd:r(k)} and local {cmd:r(view)}; a copy ({cmd:using view:}) also returns the
source view's name in {cmd:r(source_view)}. When a {cmd:.dta}, Excel or SPSS adapter was
needed it also returns the package-owned temporary path in {cmd:r(bridge)}.
{cmd:open _data} returns its snapshot path in {cmd:r(bridge)}. Lazy
{cmd:sql} returns {cmd:r(k)} and {cmd:r(view)}; {cmd:sql ..., clear} returns
{cmd:r(N)}, {cmd:r(k)} and {cmd:r(view)}. Any command that bridges a
source through a temporary snapshot ({cmd:use} lazy or eager,
{cmd:merge}/{cmd:joinby}/{cmd:append}, {cmd:open _data}) additionally returns
the snapshot's losses — {cmd:r(ext_missing)}, {cmd:r(frac_dates)},
{cmd:r(transcoded_vars)}, {cmd:r(transcoded_cells)}, {cmd:r(transcoded_meta)},
{cmd:r(transcoded_revalid)}, {cmd:r(transcoded_revalid_vars)},
{cmd:r(undecodable)}, {cmd:r(encoding_default)}, {cmd:r(encoding)} — only when
the corresponding loss occurred; delimited text decoded to UTF-8 on the way in
returns {cmd:r(transcoded_lines)}, {cmd:r(transcoded_revalid_lines)} (decoded
lines that were valid UTF-8), {cmd:r(undecodable)} and {cmd:r(encoding)}
(see {help parqit_technical##encoding:Text encodings}). This bridge
reporting differs from the zero-valued counters returned by a direct memory save.

{pstd}{it:Writing.} {cmd:parqit save} always returns scalars {cmd:r(N)} and
{cmd:r(k)} and local {cmd:r(filename)}. Locals {cmd:r(ext_missing)} and
{cmd:r(frac_dates)} list the variables whose extended missings became null or
whose fractional date/period counts were rounded, and are stored
{it:only when such a loss occurred}: with nothing lost they are not set at all, so they are
absent from {helpb return list} and both references expand to nothing. A view
save also returns {cmd:r(view)}; a memory save does not. A memory save also
returns scalars {cmd:r(transcoded_cells)}, {cmd:r(transcoded_meta)},
{cmd:r(transcoded_revalid)} (cells that were valid UTF-8 and were decoded
with their column, or under {cmd:all}), {cmd:r(undecodable)} (texts with bytes
the code page does not define, now U+FFFD) and {cmd:r(encoding_default)} (1
when no {opt encoding()} was given) and locals {cmd:r(transcoded_vars)},
{cmd:r(transcoded_revalid_vars)} and {cmd:r(encoding)} (see
{help parqit_technical##encoding:Text encodings}); the counts are zero, the lists are empty,
and {cmd:r(encoding)} still names the selected code page when nothing needed
transcoding. A lazy view save does not return these transcoding counters.
{cmd:parqit save} {it:…}{cmd:, data copysource} additionally returns local
{cmd:r(copysource)}, the source file it copied. A memory save with
{opt xmissing} returns local {cmd:r(xmissing_vars)}, the variables whose
extended missings were preserved in companion columns (empty when none had
any); those variables are then absent from {cmd:r(ext_missing)}.

{pstd}{it:Converting an SPSS file.} {cmd:parqit save} {it:file} {cmd:using}
{it:spssfile} returns scalars {cmd:r(N)} (cases) and {cmd:r(k)} (SPSS
variables) and locals {cmd:r(filename)}, {cmd:r(source)}, {cmd:r(xmissing_vars)}
(the variables whose user-missing values occur, stored as {cmd:.a}-{cmd:.z}),
{cmd:r(encoding)} (the encoding used), {cmd:r(spss_encoding)} (the one the file
declares) and {cmd:r(spss_compression)} ({cmd:none}, {cmd:bytecode} or
{cmd:zlib}); scalars {cmd:r(transcoded_cells)} and {cmd:r(transcoded_meta)}
only when text had to be transcoded.

{pstd}{it:Converting an R data file.} {cmd:parqit save} {it:file} {cmd:using}
{it:Rfile} returns scalars {cmd:r(N)} (rows), {cmd:r(k)} (columns written) and
{cmd:r(k_dropped)} (columns Stata has no type for) and locals
{cmd:r(filename)}, {cmd:r(source)}, {cmd:r(xmissing_vars)} (the variables whose
tagged or user-missing values occur, stored as {cmd:.a}-{cmd:.z}),
{cmd:r(r_object)} (the object read, when the file holds several),
{cmd:r(r_format)} ({cmd:rds} or {cmd:RData}), {cmd:r(r_version)} (the R that
wrote it), {cmd:r(r_encoding)} and {cmd:r(r_compression)} ({cmd:none},
{cmd:gzip} or {cmd:zstd}); scalars {cmd:r(transcoded_cells)} and
{cmd:r(transcoded_meta)} only when text had to be read in the session code page.
{cmd:parqit use} of an R file returns {cmd:r(r_object)} as well.

{pstd}{it:Encoding SPSS string codes.} {cmd:parqit spssencode} returns scalars
{cmd:r(N_labels)} (SPSS labels carried) and {cmd:r(N_unlabeled)} (distinct
values without an SPSS label) and locals {cmd:r(mode)} ({cmd:codes} or
{cmd:sequential}), {cmd:r(label)} and, when user-missing codes exist,
{cmd:r(missing_map)}.

{pstd}{it:Sources and views.} Lazy {cmd:merge}/{cmd:joinby} return
{cmd:r(bridge)} only when their using source needed an adapter. {cmd:append}
returns {cmd:r(n_bridges)} and, for each adapter-created bridge,
{cmd:r(bridge_1)}, …, {cmd:r(bridge_}{it:n}{cmd:)}. {cmd:views} and bare
{cmd:view} return {cmd:r(n_views)}; {cmd:view} {it:name} returns
{cmd:r(view)}. The prefix form {cmd:view} {it:name}{cmd::} {it:command}
returns the wrapped command's stored results after restoring the previous
current view.

{pstd}{it:Description.} {cmd:describe}/{cmd:glimpse} {it:parquet_source}
return scalars {cmd:r(n_rows)}, {cmd:r(n_cols)} (alias
{cmd:r(n_columns)}), {cmd:r(n_row_groups)}, {cmd:r(n_files)} and
{cmd:r(has_parqit_meta)}, plus locals {cmd:r(name_}{it:i}{cmd:)},
{cmd:r(type_}{it:i}{cmd:)}, {cmd:r(stata_type_}{it:i}{cmd:)},
{cmd:r(varlab_}{it:i}{cmd:)} (variable label) and {cmd:r(vallab_}{it:i}{cmd:)}
(value label) for each column; also scalars {cmd:r(n_value_labels)}
(value-label sets) and {cmd:r(n_notes)}, and locals {cmd:r(label)} (the
dataset label), {cmd:r(spss_labels)} and {cmd:r(r_labels)} (the variables whose
SPSS or R labels are kept in characteristics). The no-argument view form returns {cmd:r(n_cols)}
(alias {cmd:r(n_columns)}) and {cmd:r(n_steps)}.

{pstd}{it:Statistics and previews.} {cmd:count} returns {cmd:r(N)}.
{cmd:head}/{cmd:list} return {cmd:r(N)}, the number of rows shown.
{cmd:summarize} returns {cmd:r(N)}, {cmd:r(sum_w)} (equal to {cmd:r(N)} for
these unweighted summaries), {cmd:r(sum)}, {cmd:r(mean)}, {cmd:r(sd)},
{cmd:r(Var)}, {cmd:r(min)} and {cmd:r(max)} for the last displayed variable;
{opt detail} also returns {cmd:r(skewness)}, {cmd:r(kurtosis)} and
{cmd:r(p1) r(p5) r(p10) r(p25) r(p50) r(p75) r(p90) r(p95) r(p99)}.
{cmd:tabulate} returns {cmd:r(N)} and {cmd:r(r)}, plus {cmd:r(c)} for two-way
tables. {cmd:misstable} returns {cmd:r(N)} and {cmd:r(n_complete)}; its
{cmd:patterns} form returns {cmd:r(N)} for the full view and {cmd:r(r)}, the
number of displayed patterns. {cmd:levelsof} returns local {cmd:r(levels)} and scalar {cmd:r(r)}.

{pstd}{it:Other exploration.} {cmd:ds}/{cmd:lookfor} return local
{cmd:r(varlist)}. {cmd:distinct} returns {cmd:r(N)} and
{cmd:r(ndistinct)} for the last row of its displayed table (the joint tuple
when {opt joint} was requested). {cmd:duplicates report} returns
{cmd:r(N)}, {cmd:r(unique_value)} and {cmd:r(surplus)}.
{cmd:correlate}/{cmd:pwcorr} return {cmd:r(rho)}, the last off-diagonal
coefficient, and {cmd:r(N)}, the minimum diagonal nonmissing count.
These existing scalar conventions are retained for compatibility; native
{cmd:r(rho)} refers to the first two variables. Use {cmd:r(C)} and
{cmd:r(Nobs)} when a particular pair is intended. Both also
return matrix {cmd:r(C)}; {cmd:pwcorr} adds matrix {cmd:r(Nobs)} with pairwise
counts and, with {opt sig}, matrix {cmd:r(sig)} (its diagonal is missing).
If the matrix dimension exceeds Stata's limit, the table and scalars are
returned with a note instead of attempting to create an oversized matrix.
{cmd:tabstat, save} returns matrix {cmd:r(StatTotal)} without {opt by()}, or
matrices {cmd:r(Stat1)}, {cmd:r(Stat2)}, ... and group-label locals
{cmd:r(name1)}, {cmd:r(name2)}, ... with {opt by()}. Rows are statistics and
columns are the requested variables; the group-only form has no {cmd:r(StatTotal)}.
Row names follow native's stored labels:
{cmd:N Mean SD Variance Sum Min Max Range} and the requested {cmd:p##};
{cmd:median} is named {cmd:p50}.
If {opt by()} leaves no nonmissing groups, parqit prints {cmd:no observations},
returns success and creates no group matrices or group-name locals.
Native {cmd:tabstat} instead reports error 2000 for that empty-group case.
{cmd:histogram} returns {cmd:r(N)}, {cmd:r(bins)}, {cmd:r(width)} and
{cmd:r(start)}.

{pstd}{it:Diagnostics.} {cmd:path} returns local {cmd:r(path)} and scalar
{cmd:r(exists)}. {cmd:version} returns locals {cmd:r(parqit_version)},
{cmd:r(duckdb_version)}, {cmd:r(parallel_backend)} equal to {cmd:duckdb} and
{cmd:r(fill_threads)} ({cmd:auto}, or the number chosen with {cmd:parqit set fill_threads}),
and {cmd:r(stream_buffer_mb)} ({cmd:auto}, {cmd:0}, or the chosen MB cap).
Scalars {cmd:r(cpus)} and {cmd:r(threads)} report the available CPU count and
the active engine thread count. The two setting locals report the session
choice; {cmd:auto} can still inherit an operating-system environment override.
For compatibility, scalars {cmd:r(openmp)}, {cmd:r(openmp_version)} and
{cmd:r(openmp_max_threads)} remain available and equal zero: the plugin has no
OpenMP dependency. These are not DuckDB worker counts. {cmd:selftest} returns
local {cmd:r(selftest)} equal to {cmd:ok} and legacy scalar
{cmd:r(openmp_threads)} equal to zero.
Commands not listed in this section do not promise parqit-specific
stored results; in particular the lazy mutation verbs normally change only the
view plan, while {cmd:codebook}, {cmd:tabstat} without {opt save}, {cmd:duplicates list},
{cmd:show} and {cmd:explain} are display commands.


{marker limitations}{...}
{title:Limitations}

{pstd}{cmd:•} Views are plans over live sources, not snapshots: re-collecting
re-executes the pipeline and can observe a source file that changed meanwhile.
Results are not cached. A {cmd:view:}{it:name} input captures that view's plan at
the time it is embedded, but its underlying files remain live.{p_end}
{pstd}{cmd:•} Cross-source case or alias collisions must be resolved before
{cmd:append}/{cmd:merge}/{cmd:joinby}; a shared engine identifier must name the
same Stata column on both sides. Ambiguous combinations are refused before
plan mutation (see {help parqit_technical##verbs:Verbs}).{p_end}
{pstd}{cmd:•} Eager {cmd:use, clear} and direct {cmd:collect} reads check each
matched file's identity (size, mtime, ctime, inode) before planning and before
and after the fetch. Fetched column types are compared with the plan; a change fails with
{cmd:r(920)} and the dataset in memory is untouched — retry when the file is
stable. These checks are not a cross-query snapshot protocol for transformed
collect, view save or engine-side statistics. A transformed pipeline's result
is built by an engine query over the sources at execution time; a command
may also run validation queries or several statistical passes. Its sources
must remain stable throughout the command.{p_end}
{pstd}{cmd:•} {cmd:parqit save ..., data copysource} verifies identity, names,
kinds, count, {cmd:sortedby} and the first and last 64 observations only; an
edit confined to the middle rows is not detected and the copy carries the
source file's content (see {help parqit_technical##materialisers:Materialisers}).{p_end}
{pstd}{cmd:•} {cmd:reshape long} refuses output names that collide with
carried columns or each other, checking both the engine's case-insensitive
names and exact exposed Stata names before changing the view.
{cmd:reshape wide}/{cmd:pivot} refuse a generated name that
differs only by case from a live or another generated name ({cmd:x1} beside
{cmd:X1}); a {opt relaxed} union refuses a name the engine's case-insensitive
union would split across two columns, and a Hive tree whose partition key
differs only by case from a file column is refused (see {it:Column names} under
{help parqit_technical##types:Types and metadata}).{p_end}
{pstd}{cmd:•} Stata's plugin observation index is signed 32-bit. Eager
{cmd:use ..., clear} and {cmd:collect} refuse more than 2,147,483,647 rows with
error 901; filter, aggregate or {cmd:save} the lazy result instead.{p_end}
{pstd}{cmd:•} Main-source Parquet and delimited text are engine-scanned, but
{cmd:.dta}/{cmd:.xls}/{cmd:.xlsx} require a full temporary Parquet bridge, and
SPSS and R files are converted, out of core, into one (a compressed R file is
first decompressed into {cmd:c(tmpdir)}). Delimited text on a two-table
{cmd:using} side is bridged too, and delimited text that is not UTF-8 is first
decoded into a temporary UTF-8 copy, which needs room in {cmd:c(tmpdir)} (UTF-8
can take more bytes than the code page did). {cmd:describe} with a source
argument is Parquet-only.{p_end}
{pstd}{cmd:•} Bridges belong to the Stata session that created them and are
removed when the last view that uses them is closed or replaced (an eager read
removes its bridge at once); a session that ends with views open, or crashes,
leaves its {cmd:_parqit_bridge_}{it:kind}{cmd:_}{it:pid}{cmd:_}... directories
in {cmd:c(tmpdir)}. Close views with {cmd:parqit close _all} before exit; such
a directory can be deleted once no Stata session uses it (its name carries the
id of the process that made it). If the path of {cmd:c(tmpdir)} contains
{cmd:=}, the engine reads the bridge directory as a Hive partition column;
parqit says so once a session.{p_end}
{pstd}{cmd:•} Encoding limits: ISO-2022-JP and other stateful encodings,
EUC-TW, Big5-HKSCS, EBCDIC and UTF-32 are refused with a message. GB18030 is
read as its 2005 edition (as ICU and glibc read it), without the 2022
edition's changes, and {cmd:big5} is Windows' code page 950, whose area C6A1-C8FE is read as Private
Use characters, so the ETEN extensions of a Unix Big5 file (circled digits,
kana) come out as those. No legacy code page is guessed from the text: only
byte-order marks and the patterns of UTF-16 and UTF-32 are recognised, and
otherwise {opt encoding()} or the session code page applies (see
{help parqit_technical##encoding:Text encodings}).{p_end}
{pstd}{cmd:•} SPSS user-missing values are {cmd:.a}-{cmd:.z} after
{cmd:parqit use} but plain {cmd:.} in a lazy view (convert with
{cmd:parqit save} ... {cmd:using} to keep them in Parquet); more than 26 in one
variable share {cmd:.z}; string user-missing values stay text. Portable
({cmd:.por}) and encrypted SPSS files are not read; see
{help parqit_technical##spss:SPSS files}.{p_end}
{pstd}{cmd:•} R files compressed with bzip2 or xz, and files in R's ASCII
format, are refused with a message (re-save them with R's default); lists,
nested data frames, matrices, complex, raw, {cmd:POSIXlt} and S4 columns are
left out, each named in a note; a compressed file needs its uncompressed size
free in {cmd:c(tmpdir)}. See {help parqit_technical##rdata:R data files}.{p_end}
{pstd}{cmd:•} Without {opt xmissing}, extended missings {cmd:.a}-{cmd:.z}
become plain missing in Parquet, with a write-time note. With it, eager reads
restore the codes; lazy views still fold them and announce that loss.
Their literals are therefore rejected in lazy expressions; use
{cmd:missing()} or the ordinary {cmd:.} value. Merge/joinby disclose missing
keys on both sides. They inspect using keys first and scan master keys only
where using has missing values; {cmd:merge m:1}/{cmd:joinby} can consequently
defer a data-dependent master failure until execution. The uniqueness checks
of {cmd:merge 1:1}/{cmd:merge 1:m} still inspect the master.{p_end}
{pstd}{cmd:•} After a verb that builds new rows ({cmd:merge}, {cmd:append},
{cmd:collapse} and the others listed under the tie rule above), and in views
over CSV files, a slice over tied sort keys has no defined within-tie order.
Add a unique key to {cmd:sort}/{cmd:gsort} before {cmd:keep in} or a sliced
preview when row identity must be reproducible.{p_end}
{pstd}{cmd:•} A direct memory-to-Parquet save refuses a binary {cmd:strL}
containing NUL; a lazy Parquet-to-Parquet save preserves it, and text
{cmd:strL}s round-trip. Unsupported DuckDB types are dropped with a reason,
and an input with no representable columns is refused. A NUL in a source
column name is always refused; a NUL in a string value is truncated only when
crossing into Stata, with a note.{p_end}
{pstd}{cmd:•} Lazy {cmd:parqit merge m:m} is refused before adapter import or
view mutation because a lazy plan lacks native physical within-key order. Use
{cmd:joinby} for Cartesian matches or native {cmd:mergein m:m} for Stata's
sequential behaviour.{p_end}
{pstd}{cmd:•} {cmd:reshape wide} and {cmd:pivot} cap the spread dimension at
2,000 values. {cmd:collapse}/{cmd:pivot} do not implement weights. Lazy
expressions are the documented subset, not arbitrary Stata syntax; in
particular {cmd:_n}/{cmd:_N} are unavailable in {cmd:replace}, in the
{cmd:if} qualifier of {cmd:gen}, and in the read-only {cmd:count if} and
{cmd:list if} filters; {cmd:egen} also refuses them.{p_end}
{pstd}{cmd:•} {cmd:%tC} and {cmd:%tb} are stored as integer counts with
their format in metadata; third-party readers see the raw counts.{p_end}
{pstd}{cmd:•} {cmd:discard} refreshes ado programs but does not guarantee a reset
of the loaded plugin. Its views and settings may persist; close views explicitly
with {cmd:parqit close _all}. Restart Stata after rebuilding the plugin or to
restore fresh-session defaults.{p_end}
{pstd}{cmd:•} A loaded result reports {cmd:c(filename)} empty and
{cmd:c(changed)} 0 — like an import, the data is not backed by a
.dta.{p_end}

{pstd}{cmd:•} {help parqit_technical##sampledesign:Sampling designs} use parqit's own random
numbers, so they follow {cmd:sample2}'s rules but never reproduce its draw. A
fractional percentage can draw one unit fewer or more than {cmd:sample2} at a
tie. A missing cluster is outside the frame and kept, where {cmd:sample2} treats
it as one more cluster. With {opt count}, a design ranks rows by priority rather
than by the plain count form's reservoir. {cmd:in} is not supported.{p_end}

{pstd}{cmd:•} Extended missings {cmd:.a}-{cmd:.z} become plain missing in
Parquet (their labels survive) unless the file is written with
{cmd:parqit save ..., xmissing}, which keeps their codes in companion columns
that {cmd:parqit use}, {cmd:mergein} and {cmd:appendin} restore (see
{it:xmissing} under {it:Materialisers}); a lazy view reads such a file with
plain {cmd:.} in those cells and says so when opened. Their literals are
refused in lazy expressions. In a {cmd:merge}/{cmd:joinby} {it:key} the
collapse matches rows native Stata kept apart, because {cmd:.a} and {cmd:.} are
then the same missing and Stata matches missing with missing; both verbs print
a {cmd:note:} naming the key and the counts when the same key has missing
values on {it:both} sides.{p_end}
{pstd}{cmd:•} A lazy {cmd:merge}/{cmd:joinby} returns its result grouped by the
key, with a true {cmd:sortedby} marker, in an order that is not native Stata's.
Guaranteed are the content (the same rows and cells as native {cmd:merge}, as a
multiset); order within tied keys is not guaranteed, including on repeated
execution. Native {cmd:merge}'s own within-key order also changes
with the physical order of the {it:using} file. Sort explicitly after
collecting if {cmd:_n} or {cmd:by:} depends on it.{p_end}
{pstd}{cmd:•} Infinite source dates and timestamps, including nanosecond
timestamps, are refused because Stata cannot represent them. Eager and lazy
failures preserve the current data. A save whose temporal result is outside
the writer's range also refuses, preserving an existing destination. See
{help parqit_technical##types:Type fidelity} for extreme finite values.{p_end}
{pstd}{cmd:•} Signed and unsigned 64-bit and 128-bit integers outside +/-2^53
are conservatively
{bf:refused} by default, even if a particular larger integer is exactly
representable ({cmd:r(198)}, naming
every such column; nothing is staged). {opt int64(string)} loads them as exact
text and {opt int64(round)} accepts the nearest {cmd:double} with a
{cmd:note:} — after which two distinct keys can collide.
{cmd:parqit set int64} moves the session default; {cmd:parqit head} and
{cmd:parqit list} always preview the exact digits. A {it:lazy} join over such a
key is exact regardless: it runs in the engine before any Stata {cmd:double}
exists.{p_end}
{pstd}{cmd:•} {cmd:BINARY}/{cmd:BLOB} columns are dropped with a message;
{opt binary(text)} loads them as UTF-8 text (loud refusal, naming the column,
on invalid UTF-8) and {opt binary(hex)} as uppercase hex digits. Give the
option to {cmd:parqit use}, which is where a view's columns are decided.{p_end}


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
