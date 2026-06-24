{smcl}
{* *! version 0.1.11 24jun2026}{...}
{vieweralsosee "[D] use" "help use"}{...}
{vieweralsosee "[D] save" "help save"}{...}
{vieweralsosee "[D] collapse" "help collapse"}{...}
{vieweralsosee "[D] merge" "help merge"}{...}
{viewerjumpto "Syntax" "parqit##syntax"}{...}
{viewerjumpto "Description" "parqit##description"}{...}
{viewerjumpto "The lazy view" "parqit##lazy"}{...}
{viewerjumpto "Input formats" "parqit##formats"}{...}
{viewerjumpto "Verbs" "parqit##verbs"}{...}
{viewerjumpto "Materialisers" "parqit##materialisers"}{...}
{viewerjumpto "Performance tips" "parqit##perf"}{...}
{viewerjumpto "Expressions" "parqit##expressions"}{...}
{viewerjumpto "Type mapping" "parqit##types"}{...}
{viewerjumpto "Options" "parqit##options"}{...}
{viewerjumpto "Examples" "parqit##examples"}{...}
{viewerjumpto "Limitations" "parqit##limitations"}{...}
{viewerjumpto "Stored results" "parqit##results"}{...}
{title:Title}

{phang}
{bf:parqit} {hline 2} a grammar of data manipulation for Stata, backed by
Parquet on an embedded DuckDB engine


{marker syntax}{...}
{title:Syntax}

{pstd}Open a lazy view (nothing is read) or read a file into memory:

{p 8 16 2}
{cmd:parqit use} [{it:namelist}] {cmd:using} {it:filename} [{cmd:,} {opt clear} {opt n:ame(viewname)} {opt relax:ed}]

{p 8 16 2}
{cmd:parqit use} {it:filename}{cmd:,} {opt clear}

{pstd}{it:filename} may be a Parquet file, a glob such as {it:data_*.parquet},
a Hive-partitioned directory, a delimited-text file ({cmd:.csv}/{cmd:.tsv}/
{cmd:.txt}), or a Stata {cmd:.dta} / Excel {cmd:.xls}/{cmd:.xlsx} file — see
{help parqit##formats:Input formats}. Without {opt clear} a lazy view opens over
the file(s); with {opt clear} the whole file is read into memory immediately.
{opt relaxed} reads a glob whose files have {it:different} schemas by union of
column names (columns absent from a file arrive missing); without it a schema
mismatch across the matched files is a loud error.

{pstd}Verbs on the open view (all lazy):

{p 8 16 2}{cmd:parqit keep} {it:varlist} | {cmd:parqit keep if} {it:exp} | {cmd:parqit keep in} {it:f}{cmd:/}{it:l}{p_end}
{p 8 16 2}{cmd:parqit drop} {it:varlist} | {cmd:parqit drop if} {it:exp}{p_end}
{p 8 16 2}{cmd:parqit gen} [{it:type}] {it:newvar} {cmd:=} {it:exp} [{cmd:if} {it:exp}]{p_end}
{p 8 16 2}{cmd:parqit replace} {it:var} {cmd:=} {it:exp} [{cmd:if} {it:exp}]{p_end}
{p 8 16 2}{cmd:parqit egen} [{it:type}] {it:newvar} {cmd:=} {it:fcn}{cmd:(}{it:exp}{cmd:)} [{cmd:,} {opt by(varlist)}]{p_end}
{p 8 16 2}{cmd:parqit rename} {it:old} {it:new}{p_end}
{p 8 16 2}{cmd:parqit rename} {cmd:(}{it:oldlist}{cmd:)} {cmd:(}{it:newlist}{cmd:)}{p_end}
{p 8 16 2}{cmd:parqit order} {it:varlist}{p_end}
{p 8 16 2}{cmd:parqit sort} {it:varlist} | {cmd:parqit gsort} [{cmd:+}|{cmd:-}]{it:varname} ...{p_end}
{p 8 16 2}{cmd:parqit collapse} {cmd:(}{it:stat}{cmd:)} [{it:tgt}{cmd:=}]{it:src} ... [{cmd:,} {opt by(varlist)}]{p_end}
{p 8 16 2}{cmd:parqit contract} {it:varlist} [{cmd:,} {opt f:req(newvar)}]{p_end}
{p 8 16 2}{cmd:parqit duplicates drop} [{it:varlist}{cmd:,} {opt force}]{p_end}
{p 8 16 2}{cmd:parqit sample} {it:#} [{cmd:,} {opt c:ount} {opt seed(#)}]{p_end}
{p 8 16 2}{cmd:parqit reshape} {cmd:long}|{cmd:wide} {it:stubs}{cmd:,} {opt i(varlist)} {opt j(name)}{p_end}
{p 8 16 2}{cmd:parqit merge} {cmd:1:1}|{cmd:m:1}|{cmd:1:m}|{cmd:m:m} {it:keys} {cmd:using} {it:source} [{cmd:,} {it:merge_options}]{p_end}
{p 8 16 2}{cmd:parqit append using} {it:source} [{it:source} ...] [{cmd:,} {opt gen:erate(newvar)}]{p_end}
{p 8 16 2}{cmd:parqit joinby} {it:keys} {cmd:using} {it:source}{p_end}

{p 8 16 2}{cmd:parqit mergein} {cmd:1:1}|{cmd:m:1}|{cmd:1:m}|{cmd:m:m} {it:keys} {cmd:using} {it:file} [{cmd:,} {it:merge_options}]{p_end}
{p 8 16 2}{cmd:parqit appendin using} {it:file} [{cmd:,} {opt keep(varlist)}]{p_end}

{pstd}{cmd:mergein}/{cmd:appendin} join the data {it:already in Stata's memory}
with a disk {it:file} via a {it:native} {help merge}/{help append}: the
in-memory dataset stays put (no DuckDB round-trip), and parqit reads only the
needed columns of the disk side. This is the fast route when the disk side is a
{it:small lookup}; for big-on-big use the out-of-core {cmd:parqit use … ; parqit
merge} path instead. {it:merge_options} are the native ones
({opt keepus:ing()}, {opt keep()}, {opt gen:erate()}, {opt nogen:erate},
{opt update}, {opt assert()}, …).

{pstd}where each {it:source} is a Parquet {it:filename} (file, glob or
Hive directory) or {cmd:view:}{it:viewname} — another open view, joined
without materialising either side.

{pstd}Materialisers (these execute the pipeline):

{p 8 16 2}{cmd:parqit collect} [{cmd:,} {opt clear}]{space 8}stream the result into memory (atomically){p_end}
{p 8 16 2}{cmd:parqit save} {it:filename} [{cmd:,} {opt replace} {opt d:ata} {opt comp:ression(codec)} {opt compression_level(#)} {opt part:ition_by(varlist)} {opt c:hunk(#)}]{p_end}
{p 8 16 2}{cmd:parqit count} | {cmd:parqit head} [{it:#}] | {cmd:parqit list} [{it:#}]{p_end}
{p 8 16 2}{cmd:parqit summarize} [{it:varlist}] [{cmd:,} {opt d:etail}]{p_end}
{p 8 16 2}{cmd:parqit tabulate} {it:varname} [{it:varname2}] [{cmd:,} {opt m:issing} {opt row} {opt col}]{p_end}
{p 8 16 2}{cmd:parqit misstable} [{cmd:summarize}|{cmd:patterns}] [{it:varlist}]{p_end}
{p 8 16 2}{cmd:parqit levelsof} {it:varname} [{cmd:,} {opt l:imit(#)}]{p_end}
{p 8 16 2}{cmd:parqit count} [{cmd:if} {it:exp}]{p_end}
{p 8 16 2}{cmd:parqit list} [{it:varlist}] [{cmd:if} {it:exp}] [{cmd:in} {it:f}{cmd:/}{it:l}]{p_end}
{p 8 16 2}{cmd:parqit ds} | {cmd:parqit lookfor} {it:word} [{it:word} ...]{p_end}
{p 8 16 2}{cmd:parqit codebook} [{it:varlist}]{p_end}
{p 8 16 2}{cmd:parqit distinct} [{it:varlist}] [{cmd:,} {opt j:oint}]{p_end}
{p 8 16 2}{cmd:parqit duplicates} {cmd:report}|{cmd:list} {it:varlist} [{cmd:,} {opt l:imit(#)}]{p_end}
{p 8 16 2}{cmd:parqit tabstat} {it:varlist} [{cmd:,} {opt s:tatistics(stats)} {opt by(varname)}]{p_end}
{p 8 16 2}{cmd:parqit correlate} {it:varlist} | {cmd:parqit pwcorr} {it:varlist} [{cmd:,} {opt obs} {opt sig}]{p_end}
{p 8 16 2}{cmd:parqit histogram} {it:varname} [{cmd:,} {opt b:ins(#)} {opt nodraw}]{p_end}
{p 8 16 2}{cmd:parqit describe} [{it:filename}] | {cmd:parqit glimpse} {it:filename}{p_end}

{pstd}Escape hatches and introspection:

{p 8 16 2}{cmd:parqit sql} {cmd:"}{it:DuckDB SQL}{cmd:"} [{cmd:,} {opt clear} {opt n:ame(viewname)}]{p_end}
{p 8 16 2}{cmd:parqit query} {cmd:"}{it:SQL fragment}{cmd:"}{p_end}
{p 8 16 2}{cmd:parqit show} | {cmd:parqit explain}{p_end}
{p 8 16 2}{cmd:parqit view} [{it:viewname}[{cmd::} {it:parqit_command}]] | {cmd:parqit views}{p_end}
{p 8 16 2}{cmd:parqit open _data} [{cmd:,} {opt n:ame(viewname)}] | {cmd:parqit close} [{it:viewname}|{cmd:_all}] | {cmd:parqit path} {it:filename}{p_end}
{p 8 16 2}{cmd:parqit set} {cmd:statamissing}|{cmd:threads}|{cmd:memory_limit}|{cmd:tempdir} {it:value}{p_end}
{p 8 16 2}{cmd:parqit version}{space 4}(plugin + engine versions){p_end}
{p 8 16 2}{cmd:parqit selftest}{space 3}(end-to-end engine and codec check, useful on new installs/HPC nodes){p_end}


{marker description}{...}
{title:Description}

{pstd}
{cmd:parqit} reads, writes, joins and manipulates columnar Parquet data with
ordinary Stata verbs that run {it:out of core} on an embedded
{browse "https://duckdb.org":DuckDB} engine. It is dbplyr's architecture
with Stata's vocabulary: verbs are lazy and build a logical plan; the plan
compiles to a single SQL query; the engine executes it on disk (datasets far
larger than memory; intermediate results spill to a temporary directory);
and only the final result is brought into Stata — or written straight back
to Parquet without ever touching Stata's memory.

{pstd}
Stata metadata survives: variable labels, value labels, notes, display
formats, characteristics and original column names are stored in standard
Parquet key-value metadata under a {cmd:parqit.*} namespace and restored on
read, while the file remains plain Parquet for pandas, polars, R and Spark.


{marker lazy}{...}
{title:The lazy view}

{pstd}
{cmd:parqit use using} {it:files} opens a {it:view}: a description of the
data plus a pipeline of verbs, like Stata's idea of "the current dataset"
but living on disk. Nothing is read until a materialiser runs. Views are
named (default name: {cmd:default}) and several can be open at once — the
vocabulary mirrors frames: {opt name()} opens under a name, {cmd:parqit view}
{it:name} switches the current view, {cmd:parqit view} {it:name}{cmd::}
{it:command} runs a one-off against another view, {cmd:parqit views} lists
them and {cmd:parqit close} [{it:name}|{cmd:_all}] closes. Verbs always act
on the current view. A view is a plan, not data: holding many costs
nothing.

{pstd}
{cmd:parqit collect} executes the pipeline once (into a spillable temporary
table), then loads the result atomically — your data is replaced only
after the new data is complete and valid — and the view {it:stays open}
for further exploration (collecting again re-executes). {cmd:parqit save}
executes the pipeline and writes Parquet directly, naming the view it
materialised; Stata memory is never touched. To export the {it:in-memory}
dataset while views are open, use {cmd:parqit save} {it:…}{cmd:, data}.
{cmd:parqit head} previews cheaply; {cmd:parqit show} prints the generated SQL
(a readable CTE pipeline, one stage per verb); {cmd:parqit explain} prints
the engine's plan.


{marker formats}{...}
{title:Input formats}

{pstd}
The engine scans {bf:Parquet} and {bf:delimited text} ({cmd:.csv}/{cmd:.tsv}/
{cmd:.txt}) directly on disk — both are read {it:out of core}, so a file may be
far larger than memory and only the columns and rows your verbs need are
touched. {bf:Stata} ({cmd:.dta}) and {bf:Excel} ({cmd:.xls}/{cmd:.xlsx}) inputs
are {it:not} engine-scannable, so parqit imports them into a throwaway frame —
your working dataset is left untouched — and snapshots them to a small Parquet
{it:bridge} the engine then scans; their variable/value labels and formats ride
along. parqit picks the path by file extension; the choice is the same whether
the file is a {cmd:parqit use} source or the {cmd:using} side of
{cmd:merge}/{cmd:joinby}/{cmd:append}.

{pstd}
{bf:When does the bridge make sense?} For a {it:small} side — a lookup
{cmd:.dta}, a hand-made {cmd:.xlsx} — it is ideal: the cost is one quick import.
A {it:large} {cmd:.dta} master gains nothing from it (you would have read the
whole file into Stata either way), so for that prefer Stata's {cmd:use} followed
by {cmd:parqit open _data}, which promotes the in-memory dataset to a view without
a second copy. Bridges live in the temp directory and are swept up by
{cmd:parqit close _all}.

{pstd}
This is exactly the shape that keeps a large master {it:out of} Stata while a
small file joins in — only the result is collected:

{phang2}{cmd:. parqit use using big.parquet}{space 22}({it:master view; nothing read yet}){p_end}
{phang2}{cmd:. parqit merge m:1 id using lookup.dta, keepusing(rate)}{space 3}({it:.dta bridged in}){p_end}
{phang2}{cmd:. parqit collect, clear}{space 27}({it:only the merged result enters Stata}){p_end}

{pstd}
A delimited file is scanned with DuckDB's {cmd:read_csv_auto} (schema and
delimiter auto-detected); add {opt relaxed} to {cmd:parqit use} to union a glob
whose files have different schemas. (SAS/SPSS are out of scope — parqit reads
Parquet, delimited text, Stata and Excel.)


{marker verbs}{...}
{title:Verbs}

{pstd}{cmd:collapse} statistics: {cmd:mean sum sd count min max median}
{cmd:p}{it:##} {cmd:first last firstnm lastnm}. Percentiles follow Stata's
{cmd:summarize} rule exactly. {cmd:first}/{cmd:last} are deterministic over
the declared {cmd:parqit sort} order and keep a missing first value missing.

{pstd}{cmd:egen} functions: {cmd:total mean sd min max count} with
{opt by()}.

{pstd}Two-table {cmd:using} sources may be {cmd:view:}{it:name}: the other
view's pipeline is embedded as a subquery, so filtered-view-to-
filtered-view joins run in one out-of-core query. All contracts below
apply to view sources too (a view may even be merged with itself).

{pstd}{cmd:merge} validates the uniqueness contract of its kind up front
({cmd:m:1} requires unique keys in using, etc.) and produces a
Stata-compatible {cmd:_merge}; missing keys match missing keys, as in
Stata. Options: {opt keep(match master using)}, {opt keepus:ing(varlist)},
{opt gen:erate(name)}, {opt nogen:erate}. {cmd:m:m} follows Stata's
sequential pairing — and, as in Stata, is almost never what you want;
prefer {cmd:parqit joinby} for pairwise combinations.

{pstd}{cmd:duplicates drop} with a {it:varlist} requires a previous
{cmd:parqit sort} so that "first occurrence" is well-defined on an engine
that runs in parallel.

{pstd}{cmd:keep in} {it:f}{cmd:/}{it:l} validates its range against the
real observation count when the pipeline runs; out-of-range is an error,
never a silent empty result.


{marker materialisers}{...}
{title:Materialisers}

{pstd}{cmd:parqit save} writes a single Parquet file (atomically: temp file,
payload verified by a fresh scan, then renamed into place) or a
Hive-partitioned tree with {opt partition_by()} (also staged and renamed
atomically). A partitioned target that already exists is overwritten only
with {opt replace} (the new tree is built and verified first, then the old
one is removed and the new one renamed into place); without {opt replace},
or when the path exists as a plain file, the save is refused. Codecs:
{cmd:snappy} (default) {cmd:zstd gzip lz4 lz4_raw brotli uncompressed};
unknown codecs are rejected, never silently substituted. {opt chunk(#)}
sets the target rows per Parquet row group (smaller groups = finer
pushdown granularity for later reads; larger = better compression); the
engine rounds it to its internal 2048-row vector multiples, so the
effective minimum is 2048.

{pstd}With no view open, {cmd:parqit save} writes the {it:in-memory} dataset
to Parquet, and {cmd:parqit use} {it:file}{cmd:, clear} reads whole files —
plain, fast I/O with the full type map.

{pstd}
{it:String encoding.} Parquet/Arrow strings must be valid UTF-8. A string
variable that carries raw Latin-1 or other legacy bytes (common in imported or
administrative data) is a {bf:loud per-cell error} at the offending
{it:var}{cmd:[}{it:obs}{cmd:]} — never a silently corrupted or unreadable file.
Run {helpb unicode:unicode translate} on the dataset first to convert it to
UTF-8, then save. Valid UTF-8 (ASCII, accented text, emoji, {cmd:strL}) is
unaffected.


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
{cmd:parqit appendin} run a {it:native} {help merge}/{help append}, reading only
the columns you ask for from the disk side — no round-trip through DuckDB.

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

{phang}o {bf:Write without loading.} {cmd:parqit save} runs the pipeline and
writes Parquet directly, never touching Stata's memory. Use it instead of
{cmd:parqit collect} followed by a native {cmd:save}/export when you only need the
file on disk.{p_end}

{phang}o {bf:Filter and project early.} Put {cmd:parqit keep}/{cmd:parqit keep if}
before a {cmd:collect}/{cmd:save} so the engine reads fewer columns and rows —
the pipeline is lazy, so order is just a hint to push work toward the scan.{p_end}

{phang}o {bf:Read into memory once.} If a workflow collects the same view more
than once, collect it once and work on the result; each {cmd:parqit collect}
re-executes the pipeline.{p_end}

{phang}o {bf:Force a serial fill if you need to.} Reads of 50,000+ rows fill
Stata's memory using up to {cmd:min(cores, 8)} worker threads (the per-cell
fill dominates the cost). To force the single-threaded path — for example on a
platform you have not yet verified — set the {it:operating-system} environment
variable {cmd:PARQIT_FILL_THREADS=0} {it:before launching Stata} (e.g.
{cmd:export PARQIT_FILL_THREADS=0} in your shell); {cmd:PARQIT_FILL_THREADS=}{it:n}
pins {it:n} workers for atypical very wide or string-heavy reads. It is read by
the plugin via {cmd:getenv}, so a Stata {cmd:global} does not reach it. The
parallel and serial fills are byte-identical.{p_end}


{marker explore}{...}
{title:Exploring a view (no materialisation)}

{pstd}
Everything in this group is computed by the engine as a push-down query —
only the summary numbers (or a few preview rows) reach Stata, and the
dataset in memory is never touched:

{p 8 12 2}{cmd:parqit count}{space 17}rows → {cmd:r(N)}{p_end}
{p 8 12 2}{cmd:parqit summarize} [{it:vars}]{space 6}obs/mean/sd/min/max per numeric variable{p_end}
{p 8 12 2}{cmd:parqit summarize} {it:v}{cmd:, detail}{space 3}adds variance, skewness, kurtosis and the
p1 p5 p10 p25 p50 p75 p90 p95 p99 percentiles, all with Stata's exact
definitions (population central moments; the {cmd:summarize} percentile
rule) → the full {cmd:r()} set{p_end}
{p 8 12 2}{cmd:parqit tabulate} {it:a}{space 14}one-way frequencies (freq/percent/cum){p_end}
{p 8 12 2}{cmd:parqit tabulate} {it:a b}{space 12}two-way cross-tabulation with row/column totals
(the column variable may have at most 30 distinct values){p_end}
{p 8 12 2}{cmd:parqit misstable} [{it:vars}]{space 6}missing count and share per variable (strings
count {cmd:""}){p_end}
{p 8 12 2}{cmd:parqit levelsof} {it:v}{space 12}sorted distinct values → {cmd:r(levels)}
(strings compound-quoted, like {helpb levelsof}); refuses beyond
{opt limit(#)} (default 5,000){p_end}
{p 8 12 2}{cmd:parqit head} [{it:#}]{space 13}materialises only {it:#} rows (default 5) into a
scratch frame, lists them, discards them{p_end}
{p 8 12 2}{cmd:parqit describe}{space 14}the view's schema and pipeline depth{p_end}
{p 8 12 2}{cmd:parqit count if} {it:exp}{space 8}filtered count {it:without touching the view's pipeline} (any parqit expression, including {cmd:missing(a,b,c)}){p_end}
{p 8 12 2}{cmd:parqit list} [{it:vars}] [{cmd:if}] [{cmd:in}]{space 2}non-mutating preview
with projection, filter and row-range (bare {cmd:if} caps at 200 rows){p_end}
{p 8 12 2}{cmd:parqit ds}{space 20}variable names → {cmd:r(varlist)}{p_end}
{p 8 12 2}{cmd:parqit lookfor} {it:words}{space 8}match names and labels{p_end}
{p 8 12 2}{cmd:parqit codebook} [{it:vars}]{space 6}per variable: kind, obs, missing,
distinct, min/max, label (one scan){p_end}
{p 8 12 2}{cmd:parqit distinct} [{it:vars}]{space 6}distinct counts per variable;
{opt joint} adds the distinct count of the tuple{p_end}
{p 8 12 2}{cmd:parqit duplicates report} {it:keys}{space 1}copies/observations/surplus
table; {cmd:duplicates list} shows the first offending rows{p_end}
{p 8 12 2}{cmd:parqit misstable patterns}{space 3}frequency of missing-data patterns
({cmd:+} observed, {cmd:.} missing; up to 14 variables){p_end}
{p 8 12 2}{cmd:parqit tabstat} {it:vars}{cmd:, s()}{space 5}statistics × variables table
({cmd:n mean sd var sum min max range median p##}); {opt by()} groups (≤200){p_end}
{p 8 12 2}{cmd:parqit correlate} {it:vars}{space 7}correlation matrix, listwise like
{helpb correlate}; {cmd:parqit pwcorr} is pairwise, with {opt obs} and {opt sig}
(two-sided p from the t distribution){p_end}
{p 8 12 2}{cmd:parqit histogram} {it:v}{space 9}bins computed by the engine; only the
bin table reaches Stata, drawn with {cmd:twoway bar} ({opt bins(#)},
{opt nodraw}) → {cmd:r(bins)}, {cmd:r(width)}, {cmd:r(start)}{p_end}

{pstd}
Each call re-executes the (lazy) pipeline; on Parquet this is fast because
filters and column selections are pushed into the scan. {cmd:parqit tabulate}
excludes missing values unless {opt missing} is given, like native
{helpb tabulate}; {opt row}/{opt col} add percentage panels to the two-way
form. Stata transforms that have no special command translate directly:
{cmd:destring} ≡ {cmd:parqit gen y = real(x)} (with
{cmd:subinstr(x, ",", "", .)} for thousands separators), string length ≡
{cmd:parqit gen n = strlen(s)}, {cmd:bysort g: gen n = _N} ≡
{cmd:parqit egen n = count(1), by(g)}, and a duplicates tag ≡ that count
minus one. There is no {cmd:browse} over a view — preview with {cmd:parqit list}/{cmd:head} or materialise a slice with {cmd:parqit list}'s {cmd:in}
ranges; {cmd:kdensity} and {cmd:graph box} need the data and are best run
after a {cmd:collect} of the variables involved.


{marker expressions}{...}
{title:Expressions}

{pstd}
{cmd:keep if}, {cmd:gen}, {cmd:replace} and friends translate Stata
expressions to SQL. Supported: arithmetic with Stata precedence ({cmd:^}
is power, {cmd:/} never integer-divides), comparisons, {cmd:& | !},
missing literals ({cmd:.} and {cmd:.a}-{cmd:.z}, which collapse to SQL
NULL), {cmd:_n}/{cmd:_N} (windows over the declared sort), string
literals, and functions including:

{p 8 8 2}{cmd:abs exp ln log log10 sqrt floor ceil int round mod min max}
{cmd:cond inrange inlist missing mi}{p_end}
{p 8 8 2}{cmd:strlen ustrlen upper lower trim ltrim rtrim substr strpos}
{cmd:subinstr string real regexm}{p_end}
{p 8 8 2}{cmd:year month day quarter dow doy mdy dofm mofd yofd} and the
date literals {cmd:td() tc() tC() tm() tq() th() tw() ty()} (impossible dates
like {cmd:td(31feb2020)} and a 60th second are rejected loudly){p_end}

{pstd}
{cmd:string()} and {cmd:strofreal()} use Stata's default {cmd:%9.0g}
format. {cmd:substr()} and {cmd:strpos()} index bytes, like Stata; if a
{cmd:substr()} slice splits a UTF-8 codepoint, parqit returns the replacement
character because DuckDB/Arrow strings must remain valid UTF-8.

{pstd}
{it:Missing-value semantics.} By default expressions use SQL semantics:
missing is NULL and any comparison involving a missing value is unknown
(NULL). For {cmd:keep if}/{cmd:drop if} this matches native Stata for the
lower-tail and equality idioms ({cmd:x < c}, {cmd:x <= c}, {cmd:x == c}),
but it differs for the upper tail and inequality ({cmd:x > c}, {cmd:x >= c},
{cmd:x != c}): native Stata treats missing as larger than every number and
so {it:keeps} those rows, whereas SQL drops them. Likewise
{cmd:gen y = x > c} yields system missing (not 0/1) for rows where {cmd:x}
is missing. Run {cmd:parqit set statamissing on} for full Stata ordering
("missing is greater than every number"): under it every comparison — in
filters and in assignments alike — reproduces Stata's result. The literal
idioms {cmd:x == .}, {cmd:x != .}, {cmd:x < .}, {cmd:x >= .} are translated
to IS NULL tests in either mode. Strings have no missing: NULL and
{cmd:""} are the same thing on read, write and compare.

{pstd}
An unsupported function is a loud, position-anchored error that names the
function — never a silent guess. {cmd:parqit sql} and {cmd:parqit query} are
the escape hatches.


{marker types}{...}
{title:Type mapping}

{pstd}On read, integer and string columns are sized from the observed
range using Stata's exact limits; when the file was written by parqit, the
original storage type wins (a {cmd:long} comes back {cmd:long}, a
{cmd:str8} keeps width 8). DECIMAL loads as {cmd:double}; UINT32 can carry
values above 2^31 (never silent missings); LIST/STRUCT and friends are
dropped with a message. {cmd:%td} variables are DATE on disk, {cmd:%tc}
TIMESTAMP, and {cmd:%tm %tq %th %tw %ty %tb} stay integer period counts —
never mis-scaled calendar dates. Inside a pipeline every date is its Stata
number (day or millisecond count), so date arithmetic is ordinary
arithmetic.

{pstd}IEEE specials: NaN loads as missing (it is how parquet encodes a
float NA); {cmd:±Inf} loads as missing {it:with a per-column note}.
float32 columns whose finite values exceed Stata's float ceiling
(±1.70e38) widen to {cmd:double} with a note — never a silent missing.
String values with an embedded NUL byte are truncated at the NUL, also
with a per-column note (Stata strings are C strings).


{marker options}{...}
{title:parqit set}

{p 8 12 2}{cmd:parqit set statamissing on}|{cmd:off}{space 4}expression missing-value mode{p_end}
{p 8 12 2}{cmd:parqit set threads} {it:#}{space 14}engine threads{p_end}
{p 8 12 2}{cmd:parqit set memory_limit} {it:value}{space 4}e.g. {cmd:8GB}{p_end}
{p 8 12 2}{cmd:parqit set tempdir} {it:path}{space 9}spill directory for out-of-core execution{p_end}


{marker examples}{...}
{title:Examples}

{pstd}Out-of-core firm-year panel from worker microdata:{p_end}
{phang2}{cmd:. parqit use using /data/qp_*.parquet}{p_end}
{phang2}{cmd:. parqit keep if year >= 2010 & inrange(age, 25, 64)}{p_end}
{phang2}{cmd:. parqit gen double lwage = ln(wage)}{p_end}
{phang2}{cmd:. parqit collapse (mean) lwage (count) n=lwage, by(firmid year)}{p_end}
{phang2}{cmd:. parqit show}{p_end}
{phang2}{cmd:. parqit collect, clear}{p_end}

{pstd}Parquet → Parquet without touching memory:{p_end}
{phang2}{cmd:. parqit use using /data/qp_*.parquet}{p_end}
{phang2}{cmd:. parqit keep if wage > 0 & !missing(firmid)}{p_end}
{phang2}{cmd:. parqit save firm_panel.parquet, replace partition_by(year)}{p_end}

{pstd}Join firm characteristics that live on disk:{p_end}
{phang2}{cmd:. parqit use using firm_panel.parquet}{p_end}
{phang2}{cmd:. parqit merge m:1 firmid year using /data/scie.parquet, keep(match) keepusing(tfp)}{p_end}
{phang2}{cmd:. parqit collect, clear}{p_end}

{pstd}Mixed formats — a large Parquet master, a CSV read out of core, and a
lookup that lives in a Stata {cmd:.dta} (bridged), all joined before anything
enters Stata:{p_end}
{phang2}{cmd:. parqit use using transactions_*.csv}{space 6}({it:delimited, scanned out of core}){p_end}
{phang2}{cmd:. parqit keep if amount > 0}{p_end}
{phang2}{cmd:. parqit merge m:1 client_id using clients.dta, keepusing(region segment)}{p_end}
{phang2}{cmd:. parqit collapse (sum) amount (count) n=amount, by(region segment)}{p_end}
{phang2}{cmd:. parqit collect, clear}{p_end}

{pstd}Read a single Excel sheet straight into Stata (small file, bridged):{p_end}
{phang2}{cmd:. parqit use using rates.xlsx, clear}{p_end}

{pstd}Drop to SQL when a window function is clearest:{p_end}
{phang2}{cmd:. parqit use using spells.parquet}{p_end}
{phang2}{cmd:. parqit sort id start}{p_end}
{phang2}{cmd:. parqit query "qualify row_number() over (partition by id order by start) = 1"}{p_end}
{phang2}{cmd:. parqit collect, clear}{p_end}

{pstd}First exploration of an unfamiliar file — nothing is materialised:{p_end}
{phang2}{cmd:. parqit use using /data/unknown.parquet}{p_end}
{phang2}{cmd:. parqit codebook}{p_end}
{phang2}{cmd:. parqit misstable}{p_end}
{phang2}{cmd:. parqit summarize wage, detail}{p_end}
{phang2}{cmd:. parqit tabulate region sector, row}{p_end}
{phang2}{cmd:. parqit count if missing(wage, age)}{p_end}
{phang2}{cmd:. parqit list id year wage if wage < 0 | wage > 10000}{p_end}
{phang2}{cmd:. parqit histogram wage, bins(30)}{p_end}

{pstd}Several named views, joined without materialising either side:{p_end}
{phang2}{cmd:. parqit use using qp_*.parquet, name(panel)}{p_end}
{phang2}{cmd:. parqit keep if year >= 2018}{p_end}
{phang2}{cmd:. parqit use using qp_*.parquet, name(stats)}{p_end}
{phang2}{cmd:. parqit collapse (mean) mw=wage (count) n=wage, by(firmid)}{p_end}
{phang2}{cmd:. parqit view panel}{p_end}
{phang2}{cmd:. parqit merge m:1 firmid using view:stats, keep(match)}{p_end}
{phang2}{cmd:. parqit collect, clear}{p_end}

{pstd}A complete, self-verifying tour over artificial data ships in the
repository's {cmd:examples/} directory ({cmd:parqit_tour.do}).{p_end}


{marker limitations}{...}
{title:Limitations}

{pstd}{cmd:•} Views are plans: re-collecting re-executes the pipeline
(results are not cached once loaded).{p_end}
{pstd}{cmd:•} Extended missings {cmd:.a}-{cmd:.z} become plain missing in
Parquet (the format has one missing concept); parqit warns when they are
written.{p_end}
{pstd}{cmd:•} Binary strLs are refused on save (text strLs round-trip
fine).{p_end}
{pstd}{cmd:•} {cmd:%tC} and {cmd:%tb} are stored as integer counts with
their format in metadata; third-party readers see the raw counts.{p_end}
{pstd}{cmd:•} {cmd:discard} unloads the plugin and forgets an un-collected
view (data on disk is never affected).{p_end}
{pstd}{cmd:•} A loaded result reports {cmd:c(filename)} empty and
{cmd:c(changed)} 0 — like an import, the data is not backed by a
.dta.{p_end}


{marker results}{...}
{title:Stored results}

{pstd}{cmd:parqit use, clear} and {cmd:parqit collect} return {cmd:r(N)} and
{cmd:r(k)}; a lazy {cmd:parqit use} (and {cmd:parqit sql}) returns {cmd:r(k)}
and {cmd:r(view)}. {cmd:parqit count} returns {cmd:r(N)}. {cmd:parqit describe} {it:file} returns scalars {cmd:r(n_rows)}, {cmd:r(n_cols)},
{cmd:r(n_row_groups)}, {cmd:r(n_files)}, {cmd:r(has_parqit_meta)} and locals
{cmd:r(name_}{it:i}{cmd:)}, {cmd:r(type_}{it:i}{cmd:)},
{cmd:r(stata_type_}{it:i}{cmd:)}; the view form returns {cmd:r(n_cols)}
and {cmd:r(n_steps)}. {cmd:parqit save} returns {cmd:r(N)}, {cmd:r(k)},
{cmd:r(filename)} and, when a view was materialised, {cmd:r(view)}.
{cmd:parqit summarize} returns {cmd:r(N)}, {cmd:r(mean)}, {cmd:r(sd)},
{cmd:r(min)}, {cmd:r(max)} of the last variable; with {cmd:detail} also
{cmd:r(Var)}, {cmd:r(skewness)}, {cmd:r(kurtosis)} and {cmd:r(p1)} …
{cmd:r(p99)}. {cmd:parqit tabulate} returns {cmd:r(N)} and {cmd:r(r)} (and
{cmd:r(c)} for the two-way form). {cmd:parqit misstable} returns {cmd:r(N)}
and {cmd:r(n_complete)}, the number of observations with no missing value
in any of the selected variables. {cmd:parqit levelsof} returns {cmd:r(levels)} and
{cmd:r(r)}. {cmd:parqit views} returns {cmd:r(n_views)}; {cmd:parqit view}
returns {cmd:r(view)}. {cmd:parqit list} returns {cmd:r(N)} (rows shown).
{cmd:parqit ds} and {cmd:parqit lookfor} return {cmd:r(varlist)}. {cmd:parqit distinct} returns {cmd:r(N)} and {cmd:r(ndistinct)} (last row of its
table). {cmd:parqit duplicates report} returns {cmd:r(N)},
{cmd:r(unique_value)} and {cmd:r(surplus)}; {cmd:misstable patterns}
returns {cmd:r(r)}. {cmd:parqit correlate}/{cmd:pwcorr} return {cmd:r(rho)}
(last off-diagonal) and {cmd:r(N)}. {cmd:parqit histogram} returns
{cmd:r(N)}, {cmd:r(bins)}, {cmd:r(width)}, {cmd:r(start)}. {cmd:parqit path}
returns {cmd:r(path)} and {cmd:r(exists)}. {cmd:parqit version} returns
{cmd:r(parqit_version)} and {cmd:r(duckdb_version)}; {cmd:parqit selftest}
returns {cmd:r(selftest)} ({cmd:ok}).


{marker author}{...}
{title:Author}

{pstd}Miguel Portela{break}
NIPE / Universidade do Minho and BPLIM / Banco de Portugal{break}
Email: {browse "mailto:miguel.portela@eeg.uminho.pt":miguel.portela@eeg.uminho.pt}{p_end}

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
author's.{p_end}
