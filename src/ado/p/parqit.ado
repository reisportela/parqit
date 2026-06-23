*! version 0.1.7 16jun2026
*! parqit — a grammar of data manipulation for Stata, backed by Parquet (embedded DuckDB engine)
*! Author: Miguel Portela, Universidade do Minho & NIPE
*! License: MIT (see LICENSE in the parqit repository)

program define parqit, rclass
    version 16.0
    gettoken todo 0 : 0, parse(" ,")
    if `"`todo'"' == "" {
        di as err "parqit: subcommand required; see {help parqit}"
        exit 198
    }

    local cmds version selftest use save describe glimpse open close       ///
        keep drop gen egen replace rename order sort gsort collapse        ///
        contract duplicates sample collect count head list show explain    ///
        set merge append joinby reshape sql query summarize tabulate path   ///
        view views misstable levelsof ds lookfor codebook distinct      ///
        tabstat correlate pwcorr histogram mergein appendin
    local k : list posof `"`todo'"' in cmds
    if (`k' == 0) {
        di as err `"parqit: unknown subcommand `"`todo'"'"'
        exit 198
    }
    _parqit_`todo' `0'
    return add
end

* ----------------------------------------------------------------------------
* plugin management
* ----------------------------------------------------------------------------

program define _parqit_ensure_plugin
    version 16.0
    capture plugin call parqit_plugin, ping
    if (_rc == 0) exit

    local pp
    if `"$PARQIT_PLUGIN_PATH"' != "" {
        capture confirm file `"$PARQIT_PLUGIN_PATH"'
        if (_rc) {
            di as err `"parqit: \$PARQIT_PLUGIN_PATH is set but no file exists at `"$PARQIT_PLUGIN_PATH"'"'
            exit 601
        }
        local pp `"$PARQIT_PLUGIN_PATH"'
    }
    else {
        capture findfile parqit.plugin
        if (_rc) {
            di as err "parqit: could not find {bf:parqit.plugin} along the adopath"
            di as err "install the platform binary for your OS, or point the global {bf:PARQIT_PLUGIN_PATH} at a locally built plugin"
            exit 601
        }
        local pp `"`r(fn)'"'
        * Stata hands this path raw to dlopen, which does not expand ~ —
        * sysdir-based results like ~/ado/plus/p/parqit.plugin must be expanded
        if (substr(`"`pp'"', 1, 2) == "~/") {
            local home : env HOME
            if (`"`home'"' != "") {
                local pp `"`home'`=substr(`"`pp'"', 2, .)'"'
            }
        }
    }

    capture program parqit_plugin, plugin using(`"`pp'"')
    if (_rc != 0 & _rc != 110) {
        di as err `"parqit: failed to load the compiled plugin from `"`pp'"' (rc = `=_rc')"'
        di as err "the binary may be for a different platform; see BUILDING.md to build from source"
        exit 498
    }

    plugin call parqit_plugin, ping
end

* ----------------------------------------------------------------------------
* version / selftest
* ----------------------------------------------------------------------------

program define _parqit_version, rclass
    version 16.0
    syntax
    _parqit_ensure_plugin
    plugin call parqit_plugin, version
    di as txt "parqit version " as res "`parqit_plugin_version'" ///
        as txt "  (engine: DuckDB " as res "`parqit_duckdb_version'" ///
        as txt ", Stata Plugin Interface " as res "`parqit_spi_version'" as txt ")"
    return local parqit_version `"`parqit_plugin_version'"'
    return local duckdb_version `"`parqit_duckdb_version'"'
end

program define _parqit_selftest, rclass
    version 16.0
    syntax
    _parqit_ensure_plugin

    mata: assert(_parqit_hex("") == "")
    mata: assert(_parqit_hex("parqit") == "706172716974")
    mata: assert(_parqit_hex("Olá 🦆") == "4f6cc3a120f09fa686")
    mata: assert(_parqit_unhex("706172716974") == "parqit")
    mata: assert(_parqit_unhex(_parqit_hex(`"pa"th's\bad"')) == `"pa"th's\bad"')

    mata: st_local("echo_in", _parqit_hex("Olá 🦆 / path with 'quotes'"))
    plugin call parqit_plugin, echo `echo_in'
    if `"`parqit_echo'"' != `"`echo_in'"' {
        di as err "parqit selftest: hex codec mismatch between ado and plugin"
        exit 920
    }

    mata: st_local("tdirhex", _parqit_hex(st_global("c(tmpdir)")))
    plugin call parqit_plugin, selftest `tdirhex'
    if `"`parqit_selftest'"' != "ok" {
        di as err "parqit selftest: failed (no result from plugin)"
        exit 920
    }
    di as txt "parqit selftest: " as res "ok" ///
        as txt "  (codecs agree; engine opened; Parquet write/read and parqit metadata verified in-process)"
    return local selftest "ok"
end

* ----------------------------------------------------------------------------
* source adapters — let parqit read non-Parquet inputs
*   parquet/dir/glob : scanned in place (out-of-core)
*   csv/tsv/txt      : scanned in place via DuckDB read_csv_auto (out-of-core)
*   dta / xls / xlsx : NOT engine-scannable, so the ado imports them into a
*                      throwaway frame (the working dataset is untouched) and
*                      snapshots to a Parquet bridge the engine then scans. Best
*                      for small inputs (a lookup .dta, an .xlsx) — for a large
*                      master prefer `use file.dta` + `parqit open _data`.
* The raw path travels through the global PARQIT_RS_IN to survive spaces/quotes.
* ----------------------------------------------------------------------------

* One-line, truthful performance hint shown when parqit spots a faster path for a
* large operation (e.g. a big native mergein DuckDB could join out of core).
* Muted by `global PARQIT_NOTIPS 1`. Researcher-facing text is English.
program define _parqit_tip
    version 16.0
    args msg
    if ("${PARQIT_NOTIPS}" != "") exit
    di as txt `"(tip: `msg' — see {help parqit##perf:performance tips}; "' ///
        as txt `"{bf:global PARQIT_NOTIPS 1} mutes these)"'
end

program define _parqit_import_to_bridge, rclass
    version 16.0
    args kind                                /* dta | excel | csv */
    local src `"${PARQIT_RS_IN}"'
    _parqit_ensure_plugin
    if ("${PARQIT_IMPORT_SEQ}" == "") global PARQIT_IMPORT_SEQ = 0
    global PARQIT_IMPORT_SEQ = ${PARQIT_IMPORT_SEQ} + 1
    local bridge `"`c(tmpdir)'/_parqit_imp_`c(pid)'_${PARQIT_IMPORT_SEQ}.parquet"'
    tempname fr
    frame create `fr'
    frame `fr' {
        if ("`kind'" == "dta")        use `"`src'"', clear
        else if ("`kind'" == "excel") import excel `"`src'"', firstrow clear
        else                          import delimited `"`src'"', clear
        if (c(k) == 0 | _N == 0) {
            * still snapshot an empty schema so downstream errors are about the
            * data, not a missing file
        }
        parqit save `"`bridge'"', replace data
    }
    frame drop `fr'
    * track for cleanup at `parqit close _all`
    global PARQIT_IMPORT_BRIDGES `"${PARQIT_IMPORT_BRIDGES} `bridge'"'
    return local bridge `"`bridge'"'
end

program define _parqit_resolve_source, rclass
    version 16.0
    args mode                                /* source | using */
    local raw `"${PARQIT_RS_IN}"'
    * extension of the final path component (basename), case-insensitive
    local base = substr(`"`raw'"', strrpos(`"`raw'"', "/") + 1, .)
    local ext ""
    if (strpos("`base'", ".") > 0) ///
        local ext = lower(substr("`base'", strrpos("`base'", ".") + 1, .))
    local fmt "parquet"
    local bridge ""
    if (inlist("`ext'", "csv", "tsv", "txt", "tab")) {
        if ("`mode'" == "using") {
            * bridge small CSV lookups so the two-table path stays Parquet-only
            _parqit_import_to_bridge csv
            local raw `"`r(bridge)'"'
            local bridge `"`raw'"'
        }
        else local fmt "csv"                 /* big side: scan out-of-core */
    }
    else if ("`ext'" == "dta") {
        _parqit_import_to_bridge dta
        local raw `"`r(bridge)'"'
        local bridge `"`raw'"'
    }
    else if (inlist("`ext'", "xls", "xlsx")) {
        _parqit_import_to_bridge excel
        local raw `"`r(bridge)'"'
        local bridge `"`raw'"'
    }
    return local path `"`raw'"'
    return local fmt "`fmt'"
    return local bridge `"`bridge'"'
end

* ----------------------------------------------------------------------------
* parqit use — lazy view by default; , clear = read into memory now
* ----------------------------------------------------------------------------

program define _parqit_use, rclass
    version 16.0
    * owned is INTERNAL (not in the help): the view takes ownership of the
    * backing file and the plugin erases it on close/replace — only
    * parqit open _data passes it for its per-promotion bridge snapshots.
    capture syntax [namelist] using/ [, clear Name(name) OWNed RELAXed]
    if (_rc) {
        syntax anything(name=fileraw id="filename") [, clear Name(name) OWNed RELAXed]
        local using `fileraw'
        local namelist
    }
    local _sq_relaxed = ("`relaxed'" != "")
    _parqit_ensure_plugin

    * resolve the input: parquet/csv scan in place; dta/xls/xlsx -> Parquet
    * bridge (the working dataset is left untouched)
    global PARQIT_RS_IN `"`using'"'
    _parqit_resolve_source source
    local using `"`r(path)'"'
    local _sq_fmt "`r(fmt)'"
    local _sq_bridge `"`r(bridge)'"'

    if ("`clear'" == "") {
        * open (or replace) the named lazy view — nothing is read
        if ("`name'" == "") local name "default"
        tempfile req
        local _sq_file `"`using'"'
        local _sq_namelist `"`namelist'"'
        local _sq_vname "`name'"
        * a dta/xls/xlsx bridge is owned by this view: erased on close/replace
        if (`"`_sq_bridge'"' != "") local owned "owned"
        local _sq_owned = ("`owned'" != "")
        mata: _parqit_wr_view_open_request("`req'")
        capture noisily plugin call parqit_plugin, view_open `reqhex'
        if (_rc) exit _rc
        mata: st_local("vname", _parqit_unhex(st_local("parqit_view_name")))
        di as txt "(lazy view " as res "`vname'" as txt " opened over " ///
            as res `"`using'"' as txt ": " as res "`parqit_view_k'" ///
            as txt " columns; nothing read — use {bf:parqit collect} or {bf:parqit save})"
        return scalar k = `parqit_view_k'
        return local view "`vname'"
        exit
    }
    if ("`name'" != "") {
        di as err "parqit use: name() applies to lazy views; omit clear"
        exit 198
    }

    * materialise now; open views are untouched (a plain read is just a read)

    tempfile req resp strl
    local _sq_file `"`using'"'
    local _sq_namelist `"`namelist'"'
    mata: _parqit_wr_use_request("`req'", "`resp'", "`strl'")
    capture noisily plugin call parqit_plugin, use_prepare `reqhex'
    if (_rc) exit _rc

    _parqit_load_core, resp(`"`resp'"') strl(`"`strl'"') tag("`parqit_tag'") ///
        n(`parqit_n') names("`parqit_names'")

    global PARQIT_FAST_SOURCE_NONCE
    global PARQIT_FAST_SOURCE_PATH
    global PARQIT_FAST_SOURCE_SIZE
    global PARQIT_FAST_SOURCE_MTIME
    if ("`parqit_fast_source_ok'" == "1") {
        if ("${PARQIT_FAST_SOURCE_SEQ}" == "") global PARQIT_FAST_SOURCE_SEQ = 0
        global PARQIT_FAST_SOURCE_SEQ = ${PARQIT_FAST_SOURCE_SEQ} + 1
        local _parqit_fast_nonce "`c(pid)'_${PARQIT_FAST_SOURCE_SEQ}"
        mata: st_local("_parqit_fast_path", _parqit_unhex(st_local("parqit_fast_source_path")))
        char _dta[_parqit_fast_source_nonce] "`_parqit_fast_nonce'"
        global PARQIT_FAST_SOURCE_NONCE "`_parqit_fast_nonce'"
        global PARQIT_FAST_SOURCE_PATH `"`_parqit_fast_path'"'
        global PARQIT_FAST_SOURCE_SIZE "`parqit_fast_source_size'"
        global PARQIT_FAST_SOURCE_MTIME "`parqit_fast_source_mtime'"
        mata: (void) st_updata(0)
    }

    * a dta/xls/xlsx bridge has been consumed into memory — drop it now
    if (`"`_sq_bridge'"' != "") {
        capture erase `"`_sq_bridge'"'
        global PARQIT_IMPORT_BRIDGES `"`=subinstr(`" ${PARQIT_IMPORT_BRIDGES} "', `" `_sq_bridge' "', " ", .)'"'
    }

    di as txt "(" as res "`parqit_k'" as txt " vars, " as res "`parqit_n'" ///
        as txt `" obs read from `_sq_file')"'
    return scalar N = `parqit_n'
    return scalar k = `parqit_k'
end

* shared staging: create vars in a tempframe, fetch, decorate, atomic swap
program define _parqit_load_core
    version 16.0
    syntax, resp(string) strl(string) tag(string) n(integer) [names(string)]

    tempname stage
    local curframe = c(frame)
    local loadrc = 0
    frame create `stage'
    frame `stage' {
        capture noisily {
            mata: _parqit_resp_create(`"`resp'"', `n')
            if (`n' > 0) {
                plugin call parqit_plugin `names' in 1/`n', use_fetch `tag'
            }
            mata: _parqit_apply_strl(`"`strl'"')
            mata: _parqit_resp_decorate(`"`resp'"')
            if (`"`parqit_dtalabel'"' != "") {
                mata: st_local("dl", _parqit_unhex(st_local("parqit_dtalabel")))
                label data `"`dl'"'
            }
        }
        local loadrc = _rc
    }
    if (`loadrc') {
        frame drop `stage'
        exit `loadrc'
    }

    * Atomic swap by adopting the staged frame under the live name, rather
    * than deep-copying the (possibly multi-GB) result into `curframe`. The
    * stage is known-good at this point and the old data is discarded only
    * after the new frame is complete, so the validate-then-mutate guarantee
    * (charter §6.9) holds: if the fill above had failed we exited before here
    * with `curframe` untouched. This makes the swap O(1) in the data size and
    * avoids a transient second copy of the result in memory.
    frame change `stage'
    frame drop `curframe'
    frame rename `stage' `curframe'
    global S_FN
    global S_FNDATE
    mata: (void) st_updata(0)
end

* ----------------------------------------------------------------------------
* verbs on the lazy view
* ----------------------------------------------------------------------------

program define _parqit_keep
    version 16.0
    gettoken first : 0, parse(" ")
    if (`"`first'"' == "if") {
        gettoken first 0 : 0, parse(" ")
        _parqit_op_filter keep_if `0'
        exit
    }
    if (`"`first'"' == "in") {
        gettoken first 0 : 0, parse(" ")
        _parqit_op_keepin `0'
        exit
    }
    _parqit_op_names keep_vars `0'
end

program define _parqit_drop
    version 16.0
    gettoken first : 0, parse(" ")
    if (`"`first'"' == "if") {
        gettoken first 0 : 0, parse(" ")
        _parqit_op_filter drop_if `0'
        exit
    }
    _parqit_op_names drop_vars `0'
end

program define _parqit_op_filter
    version 16.0
    gettoken op 0 : 0, parse(" ")
    if (strtrim(`"`0'"') == "") {
        di as err "parqit: expression required"
        exit 198
    }
    _parqit_ensure_plugin
    tempfile req
    local _sq_op "`op'"
    local _sq_expr `"`0'"'
    mata: _parqit_wr_op_expr_request("`req'")
    capture noisily plugin call parqit_plugin, view_op `reqhex'
    if (_rc) exit _rc
end

program define _parqit_op_names
    version 16.0
    gettoken op 0 : 0, parse(" ")
    if (strtrim(`"`0'"') == "") {
        di as err "parqit: variable list required"
        exit 198
    }
    _parqit_ensure_plugin
    tempfile req
    local _sq_op "`op'"
    local _sq_names `"`0'"'
    mata: _parqit_wr_op_names_request("`req'")
    capture noisily plugin call parqit_plugin, view_op `reqhex'
    if (_rc) exit _rc
end

program define _parqit_op_keepin
    version 16.0
    * forms: f/l  |  l (means 1/l)
    local range `"`0'"'
    local f 1
    local l .
    if (strpos(`"`range'"', "/")) {
        local f = substr(`"`range'"', 1, strpos(`"`range'"', "/") - 1)
        local l = substr(`"`range'"', strpos(`"`range'"', "/") + 1, .)
    }
    else {
        * Stata: `keep in #' keeps exactly that observation
        local f `range'
        local l `range'
    }
    capture confirm integer number `f'
    local bad = _rc
    capture confirm integer number `l'
    if (`bad' | _rc) {
        di as err "parqit keep in: range must be f/l with integer f and l (negative forms are not supported on a lazy view)"
        exit 198
    }
    _parqit_ensure_plugin
    tempfile req
    local _sq_f `f'
    local _sq_l `l'
    mata: _parqit_wr_op_keepin_request("`req'")
    capture noisily plugin call parqit_plugin, view_op `reqhex'
    if (_rc) exit _rc
end

program define _parqit_gen
    version 16.0
    * parqit gen [type] name = expr [if cond]
    local vtype
    gettoken t1 rest : 0, parse(" =")
    local isty 0
    if inlist("`t1'", "byte", "int", "long", "float", "double", "strL") local isty 1
    if (substr("`t1'", 1, 3) == "str" & !`isty') {
        capture confirm integer number `=substr("`t1'", 4, .)'
        if (!_rc) local isty 1
    }
    if (`isty') {
        local vtype `t1'
        local 0 `"`rest'"'
    }
    gettoken name 0 : 0, parse(" =")
    gettoken eq 0 : 0, parse(" =")
    if (`"`eq'"' != "=") {
        di as err "parqit gen: expected name = expression"
        exit 198
    }
    confirm name `name'
    mata: _parqit_split_if(st_local("0"))
    _parqit_ensure_plugin
    tempfile req
    local _sq_name "`name'"
    local _sq_type "`vtype'"
    mata: _parqit_wr_op_gen_request("`req'", "gen")
    capture noisily plugin call parqit_plugin, view_op `reqhex'
    if (_rc) exit _rc
end

program define _parqit_replace
    version 16.0
    gettoken name 0 : 0, parse(" =")
    gettoken eq 0 : 0, parse(" =")
    if (`"`eq'"' != "=") {
        di as err "parqit replace: expected name = expression"
        exit 198
    }
    confirm name `name'
    mata: _parqit_split_if(st_local("0"))
    _parqit_ensure_plugin
    tempfile req
    local _sq_name "`name'"
    local _sq_type ""
    mata: _parqit_wr_op_gen_request("`req'", "replace")
    capture noisily plugin call parqit_plugin, view_op `reqhex'
    if (_rc) exit _rc
end

program define _parqit_egen
    version 16.0
    * parqit egen [type] name = fcn(expr) [, by(varlist)]
    local vtype
    gettoken t1 rest : 0, parse(" =")
    local isty 0
    if inlist("`t1'", "byte", "int", "long", "float", "double", "strL") local isty 1
    if (substr("`t1'", 1, 3) == "str" & !`isty') {
        capture confirm integer number `=substr("`t1'", 4, .)'
        if (!_rc) local isty 1
    }
    if (`isty') {
        local vtype `t1'
        local 0 `"`rest'"'
    }
    gettoken name 0 : 0, parse(" =")
    gettoken eq 0 : 0, parse(" =")
    if (`"`eq'"' != "=") {
        di as err "parqit egen: expected name = fcn(expression)"
        exit 198
    }
    confirm name `name'
    * extract fcn(...) by matching parentheses, not by cutting at the first comma
    * (which split cond(x>0, y, .) mid-expression) — EGEN-1. The remainder after
    * the matching ")" is the option list ([, by(...)]).
    local rest0 = strtrim(`"`0'"')
    local p = strpos(`"`rest0'"', "(")
    if (`p' == 0) {
        di as err "parqit egen: expected fcn(expression)"
        exit 198
    }
    local fcn = strtrim(substr(`"`rest0'"', 1, `p' - 1))
    local n = strlen(`"`rest0'"')
    local depth 0
    local close 0
    local i = `p'
    while (`i' <= `n') {
        local ch = substr(`"`rest0'"', `i', 1)
        if ("`ch'" == "(")      local depth = `depth' + 1
        else if ("`ch'" == ")") {
            local depth = `depth' - 1
            if (`depth' == 0) {
                local close = `i'
                continue, break
            }
        }
        local i = `i' + 1
    }
    if (`close' == 0) {
        di as err "parqit egen: expected fcn(expression)"
        exit 198
    }
    local fexpr = substr(`"`rest0'"', `p' + 1, `close' - `p' - 1)
    local 0 = substr(`"`rest0'"', `close' + 1, .)
    syntax [, by(string)]
    _parqit_ensure_plugin
    tempfile req
    local _sq_name "`name'"
    local _sq_type "`vtype'"
    local _sq_fcn "`fcn'"
    local _sq_expr `"`fexpr'"'
    local _sq_by `"`by'"'
    mata: _parqit_wr_op_egen_request("`req'")
    capture noisily plugin call parqit_plugin, view_op `reqhex'
    if (_rc) exit _rc
end

program define _parqit_rename
    version 16.0
    * Two documented forms (RENAME-1):
    *   parqit rename oldname newname
    *   parqit rename (oldlist) (newlist)   [equal lengths, renamed pairwise]
    local in = strtrim(`"`0'"')
    if (substr(`"`in'"', 1, 1) == "(") {
        local c1 = strpos(`"`in'"', ")")
        if (`c1' == 0) {
            di as err "parqit rename: expected (oldlist) (newlist)"
            exit 198
        }
        local oldlist = strtrim(substr(`"`in'"', 2, `c1' - 2))
        local rest = strtrim(substr(`"`in'"', `c1' + 1, .))
        local c2 = strpos(`"`rest'"', ")")
        if (substr(`"`rest'"', 1, 1) != "(" | `c2' == 0) {
            di as err "parqit rename: expected (oldlist) (newlist)"
            exit 198
        }
        local newlist = strtrim(substr(`"`rest'"', 2, `c2' - 2))
        local tail = strtrim(substr(`"`rest'"', `c2' + 1, .))
        if (`"`tail'"' != "") {
            di as err "parqit rename: unexpected text after (newlist)"
            exit 198
        }
        local nold : word count `oldlist'
        local nnew : word count `newlist'
        if (`nold' != `nnew' | `nold' == 0) {
            di as err "parqit rename: oldlist and newlist must have the same (nonzero) length"
            exit 198
        }
        * Renamed pairwise in order; the plugin refuses a new name that already
        * exists, so an overlapping/cyclic rename fails loudly rather than
        * corrupting (rename through an intermediate name for those).
        forvalues k = 1/`nold' {
            local o : word `k' of `oldlist'
            local nn : word `k' of `newlist'
            _parqit_rename_one `"`o'"' `"`nn'"'
        }
        exit
    }
    gettoken oldn 0 : 0, parse(" ")
    gettoken newn 0 : 0, parse(" ")
    if (`"`oldn'"' == "" | `"`newn'"' == "" | strtrim(`"`0'"') != "") {
        di as err "parqit rename: syntax is parqit rename oldname newname  or  parqit rename (oldlist) (newlist)"
        exit 198
    }
    _parqit_rename_one `"`oldn'"' `"`newn'"'
end

program define _parqit_rename_one
    version 16.0
    args oldn newn
    confirm name `newn'
    _parqit_ensure_plugin
    tempfile req
    local _sq_old `"`oldn'"'
    local _sq_new `"`newn'"'
    mata: _parqit_wr_op_rename_request("`req'")
    capture noisily plugin call parqit_plugin, view_op `reqhex'
    if (_rc) exit _rc
end

program define _parqit_order
    version 16.0
    _parqit_op_names order `0'
end

program define _parqit_sort
    version 16.0
    if (strtrim(`"`0'"') == "") {
        di as err "parqit sort: variable list required"
        exit 198
    }
    _parqit_ensure_plugin
    tempfile req
    local _sq_keys `"`0'"'
    local _sq_desc ""
    foreach t of local 0 {
        local _sq_desc "`_sq_desc' 0"
    }
    mata: _parqit_wr_op_sort_request("`req'")
    capture noisily plugin call parqit_plugin, view_op `reqhex'
    if (_rc) exit _rc
end

program define _parqit_gsort
    version 16.0
    * tokens like -wage +id
    local keys
    local desc
    foreach t of local 0 {
        local s = substr(`"`t'"', 1, 1)
        if ("`s'" == "-") {
            local keys `"`keys' `=substr(`"`t'"', 2, .)'"'
            local desc "`desc' 1"
        }
        else if ("`s'" == "+") {
            local keys `"`keys' `=substr(`"`t'"', 2, .)'"'
            local desc "`desc' 0"
        }
        else {
            local keys `"`keys' `t'"'
            local desc "`desc' 0"
        }
    }
    if (strtrim(`"`keys'"') == "") {
        di as err "parqit gsort: variable list required"
        exit 198
    }
    _parqit_ensure_plugin
    tempfile req
    local _sq_keys `"`keys'"'
    local _sq_desc "`desc'"
    mata: _parqit_wr_op_sort_request("`req'")
    capture noisily plugin call parqit_plugin, view_op `reqhex'
    if (_rc) exit _rc
end

program define _parqit_collapse
    version 16.0
    * (stat) [tgt=]src ... [(stat) ...] , by(varlist)
    local stat "mean"
    local specs
    local pend
    local pendtgt 0
    local parsing 1
    while (`parsing') {
        gettoken tok 0 : 0, parse(" ,()=")
        if (`"`tok'"' == "" | `"`tok'"' == ",") {
            if ("`pend'" != "") local specs `"`specs' `stat'||`pend'"'
            local pend
            if (`"`tok'"' == ",") local 0 `", `0'"'
            local parsing 0
            continue
        }
        if (`"`tok'"' == "(") {
            if ("`pend'" != "") local specs `"`specs' `stat'||`pend'"'
            local pend
            gettoken stat 0 : 0, parse(" ()")
            gettoken close 0 : 0, parse(" ()")
            if (`"`close'"' != ")") {
                di as err "parqit collapse: malformed (statistic)"
                exit 198
            }
            continue
        }
        if (`"`tok'"' == "=") {
            if ("`pend'" == "" | `pendtgt') {
                di as err "parqit collapse: misplaced ="
                exit 198
            }
            local pendtgt 1
            continue
        }
        if (`pendtgt') {
            local specs `"`specs' `stat'|`pend'|`tok'"'
            local pend
            local pendtgt 0
            continue
        }
        if ("`pend'" != "") local specs `"`specs' `stat'||`pend'"'
        local pend `"`tok'"'
    }
    if (strtrim(`"`specs'"') == "") {
        di as err "parqit collapse: nothing to compute"
        exit 198
    }
    syntax [, by(string)]
    _parqit_ensure_plugin
    tempfile req
    local _sq_specs `"`specs'"'
    local _sq_by `"`by'"'
    mata: _parqit_wr_op_collapse_request("`req'")
    capture noisily plugin call parqit_plugin, view_op `reqhex'
    if (_rc) exit _rc
end

program define _parqit_contract
    version 16.0
    syntax anything(name=vars) [, Freq(name)]
    _parqit_ensure_plugin
    tempfile req
    local _sq_names `"`vars'"'
    local _sq_freq "`freq'"
    mata: _parqit_wr_op_contract_request("`req'")
    capture noisily plugin call parqit_plugin, view_op `reqhex'
    if (_rc) exit _rc
end

program define _parqit_duplicates, rclass
    version 16.0
    gettoken sub 0 : 0, parse(" ,")
    if (`"`sub'"' == "report" | `"`sub'"' == "list") {
        syntax anything(name=vars) [, Limit(integer 20)]
        _parqit_ensure_plugin
        tempfile req resp
        local _sq_what = cond("`sub'" == "report", "dupreport", "duplist")
        local _sq_vars `"`vars'"'
        local _sq_limit `limit'
        mata: _parqit_wr_stats_request("`req'", "`resp'")
        capture noisily plugin call parqit_plugin, view_stats `reqhex'
        if (_rc) exit _rc
        if ("`sub'" == "report") {
            mata: _parqit_print_dupreport("`resp'")
            return scalar unique_value = `parqit_dup_unique'
            return scalar surplus = `parqit_dup_surplus'
            return scalar N = `parqit_dup_total'
        }
        else {
            mata: _parqit_print_duplist("`resp'")
        }
        exit
    }
    if (`"`sub'"' != "drop") {
        di as err "parqit duplicates: drop, report or list"
        exit 198
    }
    syntax [anything(name=vars)] [, force]
    _parqit_ensure_plugin
    tempfile req
    local _sq_names `"`vars'"'
    local _sq_force = ("`force'" != "")
    mata: _parqit_wr_op_dupdrop_request("`req'")
    capture noisily plugin call parqit_plugin, view_op `reqhex'
    if (_rc) exit _rc
end

program define _parqit_sample
    version 16.0
    syntax anything(name=amount) [, Count seed(integer -1)]
    confirm number `amount'
    _parqit_ensure_plugin
    tempfile req
    local _sq_amount `amount'
    local _sq_count = cond("`count'" != "", "true", "false")
    local _sq_seed `seed'
    mata: _parqit_wr_op_sample_request("`req'")
    capture noisily plugin call parqit_plugin, view_op `reqhex'
    if (_rc) exit _rc
end

* ----------------------------------------------------------------------------
* materialisers and introspection
* ----------------------------------------------------------------------------

program define _parqit_collect, rclass
    version 16.0
    syntax [, clear]
    if ("`clear'" == "" & c(changed) & (c(N) > 0 | c(k) > 0)) {
        error 4
    }
    _parqit_ensure_plugin
    tempfile req resp strl
    local _sq_limit -1
    mata: _parqit_wr_collect_request("`req'", "`resp'", "`strl'")
    capture noisily plugin call parqit_plugin, view_collect_prepare `reqhex'
    if (_rc) exit _rc

    _parqit_load_core, resp(`"`resp'"') strl(`"`strl'"') tag("`parqit_tag'") ///
        n(`parqit_n') names("`parqit_names'")

    plugin call parqit_plugin, view_alive
    mata: st_local("vname", _parqit_unhex(st_local("parqit_view_current")))
    di as txt "(" as res "`parqit_k'" as txt " vars, " as res "`parqit_n'" ///
        as txt " obs collected; view " as res "`vname'" as txt " remains open)"
    return scalar N = `parqit_n'
    return scalar k = `parqit_k'
end

program define _parqit_count, rclass
    version 16.0
    gettoken first : 0, parse(" ")
    if (`"`first'"' == "if") {
        gettoken first 0 : 0, parse(" ")
        if (strtrim(`"`0'"') == "") {
            di as err "parqit count: expression required after if"
            exit 198
        }
        _parqit_ensure_plugin
        tempfile req resp
        local _sq_what "countif"
        local _sq_vars ""
        local _sq_expr `"`0'"'
        mata: _parqit_wr_stats_request("`req'", "`resp'")
        capture noisily plugin call parqit_plugin, view_stats `reqhex'
        if (_rc) exit _rc
        di as txt "  " as res %21.0fc `parqit_n'
        return scalar N = `parqit_n'
        exit
    }
    syntax
    _parqit_ensure_plugin
    mata: st_local("whathex", _parqit_hex("count"))
    capture noisily plugin call parqit_plugin, view_info `whathex'
    if (_rc) exit _rc
    di as txt "  " as res %21.0fc `parqit_n'
    return scalar N = `parqit_n'
end

program define _parqit_head, rclass
    version 16.0
    syntax [anything(name=nrows)]
    if (`"`nrows'"' == "") local nrows 5
    confirm integer number `nrows'
    _parqit_ensure_plugin
    tempfile req resp strl
    local _sq_limit `nrows'
    mata: _parqit_wr_collect_request("`req'", "`resp'", "`strl'")
    capture noisily plugin call parqit_plugin, view_collect_prepare `reqhex'
    if (_rc) exit _rc

    tempname stage
    local rc = 0
    frame create `stage'
    frame `stage' {
        capture noisily {
            mata: _parqit_resp_create("`resp'", `parqit_n')
            if (`parqit_n' > 0) {
                plugin call parqit_plugin `parqit_names' in 1/`parqit_n', use_fetch `parqit_tag'
            }
            mata: _parqit_apply_strl("`strl'")
            mata: _parqit_resp_decorate("`resp'")
            list, abbreviate(12)
        }
        local rc = _rc
    }
    frame drop `stage'
    if (`rc') exit `rc'
    return scalar N = `parqit_n'
end

program define _parqit_list, rclass
    version 16.0
    * parqit list [varlist] [if exp] [in f/l]   (non-mutating preview)
    local vars
    local ifexp
    local f 0
    local l 0
    local limit 20
    local parsing 1
    while (`parsing') {
        gettoken tok : 0, parse(" ")
        if (`"`tok'"' == "") {
            local parsing 0
            continue
        }
        if (`"`tok'"' == "if") {
            gettoken tok 0 : 0, parse(" ")
            mata: _parqit_split_in(st_local("0"))
            local ifexp `"`parqit_inexpr'"'
            if ("`parqit_inrange'" != "") {
                local f = real(word("`parqit_inrange'", 1))
                local l = real(word("`parqit_inrange'", 2))
            }
            local parsing 0
            continue
        }
        if (`"`tok'"' == "in") {
            gettoken tok 0 : 0, parse(" ")
            gettoken rng 0 : 0, parse(" ")
            if (strpos(`"`rng'"', "/")) {
                local f = real(substr(`"`rng'"', 1, strpos(`"`rng'"', "/") - 1))
                local l = real(substr(`"`rng'"', strpos(`"`rng'"', "/") + 1, .))
            }
            else {
                local f = real(`"`rng'"')
                local l = `f'
            }
            continue
        }
        gettoken tok 0 : 0, parse(" ")
        local vars `vars' `tok'
    }
    if (`f' < 0 | `l' < `f' | (`f' == 0 & `l' != 0) | `f' == . | `l' == .) {
        di as err "parqit list: invalid in range"
        exit 198
    }
    if (`f' == 0 & `"`ifexp'"' == "") {
        local f 1
        local l `limit'
    }
    _parqit_ensure_plugin
    tempfile req resp strl
    local _sq_limit = cond(`"`ifexp'"' != "" & `f' == 0, 200, -1)
    local _sq_pvars `"`vars'"'
    local _sq_pfilter `"`ifexp'"'
    local _sq_pf `f'
    local _sq_pl `l'
    mata: _parqit_wr_collect_request("`req'", "`resp'", "`strl'")
    capture noisily plugin call parqit_plugin, view_collect_prepare `reqhex'
    if (_rc) exit _rc

    tempname stage
    local rc = 0
    frame create `stage'
    frame `stage' {
        capture noisily {
            mata: _parqit_resp_create("`resp'", `parqit_n')
            if (`parqit_n' > 0) {
                plugin call parqit_plugin `parqit_names' in 1/`parqit_n', use_fetch `parqit_tag'
            }
            mata: _parqit_apply_strl("`strl'")
            mata: _parqit_resp_decorate("`resp'")
            list, abbreviate(12)
        }
        local rc = _rc
    }
    frame drop `stage'
    if (`rc') exit `rc'
    if (`"`ifexp'"' != "" & `f' == 0 & `parqit_n' == 200) {
        di as txt "(showing the first 200 matching rows; add {bf:in f/l} to page)"
    }
    return scalar N = `parqit_n'
end

program define _parqit_ds, rclass
    version 16.0
    syntax
    _parqit_ensure_plugin
    tempfile resp
    mata: st_local("whathex", _parqit_hex("describe"))
    mata: st_local("resphex", _parqit_hex(st_local("resp")))
    capture noisily plugin call parqit_plugin, view_info `whathex' `resphex'
    if (_rc) exit _rc
    mata: _parqit_collect_names("`resp'", "")
    di as txt `"`parqit_dsnames'"'
    return local varlist `"`parqit_dsnames'"'
end

program define _parqit_lookfor, rclass
    version 16.0
    syntax anything(name=terms)
    _parqit_ensure_plugin
    tempfile resp
    mata: st_local("whathex", _parqit_hex("describe"))
    mata: st_local("resphex", _parqit_hex(st_local("resp")))
    capture noisily plugin call parqit_plugin, view_info `whathex' `resphex'
    if (_rc) exit _rc
    local _sq_terms `"`terms'"'
    mata: _parqit_lookfor_resp("`resp'")
    return local varlist `"`parqit_dsnames'"'
end

program define _parqit_codebook, rclass
    version 16.0
    syntax [anything(name=vars)]
    _parqit_ensure_plugin
    tempfile req resp
    local _sq_what "codebook"
    local _sq_vars `"`vars'"'
    mata: _parqit_wr_stats_request("`req'", "`resp'")
    capture noisily plugin call parqit_plugin, view_stats `reqhex'
    if (_rc) exit _rc
    mata: _parqit_print_codebook("`resp'")
end

program define _parqit_distinct, rclass
    version 16.0
    syntax [anything(name=vars)] [, Joint]
    _parqit_ensure_plugin
    tempfile req resp
    local _sq_what "distinct"
    local _sq_vars `"`vars'"'
    local _sq_joint = ("`joint'" != "")
    mata: _parqit_wr_stats_request("`req'", "`resp'")
    capture noisily plugin call parqit_plugin, view_stats `reqhex'
    if (_rc) exit _rc
    mata: _parqit_print_distinct("`resp'")
    return scalar N = `parqit_n'
    return scalar ndistinct = `parqit_ndistinct'
end

program define _parqit_tabstat, rclass
    version 16.0
    gettoken vars 0 : 0, parse(",")
    local vars = strtrim(`"`vars'"')
    if ("`vars'" == "") {
        di as err "parqit tabstat: a numeric varlist is required"
        exit 198
    }
    syntax [, Statistics(string) by(name)]
    if (`"`statistics'"' == "") local statistics "mean"
    _parqit_ensure_plugin
    tempfile req resp
    local _sq_what "tabstat"
    local _sq_vars `"`vars'"'
    local _sq_stats = strlower(`"`statistics'"')
    local _sq_by "`by'"
    mata: _parqit_wr_stats_request("`req'", "`resp'")
    capture noisily plugin call parqit_plugin, view_stats `reqhex'
    if (_rc) exit _rc
    mata: _parqit_print_tabstat("`resp'")
end

program define _parqit_correlate, rclass
    version 16.0
    syntax anything(name=vars)
    _parqit_ensure_plugin
    tempfile req resp
    local _sq_what "corr"
    local _sq_vars `"`vars'"'
    local _sq_pairwise "false"
    mata: _parqit_wr_stats_request("`req'", "`resp'")
    capture noisily plugin call parqit_plugin, view_stats `reqhex'
    if (_rc) exit _rc
    local _sq_sig ""
    local _sq_obs ""
    mata: _parqit_print_corr("`resp'")
    return scalar N = `parqit_corr_n'
    return scalar rho = `parqit_corr_last'
end

program define _parqit_pwcorr, rclass
    version 16.0
    gettoken vars 0 : 0, parse(",")
    local vars = strtrim(`"`vars'"')
    syntax [, obs sig]
    _parqit_ensure_plugin
    tempfile req resp
    local _sq_what "corr"
    local _sq_vars `"`vars'"'
    local _sq_pairwise "true"
    mata: _parqit_wr_stats_request("`req'", "`resp'")
    capture noisily plugin call parqit_plugin, view_stats `reqhex'
    if (_rc) exit _rc
    local _sq_sig "`sig'"
    local _sq_obs "`obs'"
    mata: _parqit_print_corr("`resp'")
    return scalar N = `parqit_corr_n'
    return scalar rho = `parqit_corr_last'
end

program define _parqit_histogram, rclass
    version 16.0
    syntax anything(name=var) [, Bins(integer 0) NODRAW]
    _parqit_ensure_plugin
    tempfile req resp
    local _sq_what "hist"
    local _sq_vars `"`var'"'
    local _sq_bins `bins'
    mata: _parqit_wr_stats_request("`req'", "`resp'")
    capture noisily plugin call parqit_plugin, view_stats `reqhex'
    if (_rc) exit _rc

    * tiny bin table → scratch frame → bar chart; the data never loads
    tempname hf
    local nb = `parqit_hist_bins'
    frame create `hf'
    frame `hf' {
        qui set obs `nb'
        qui gen double __mid = `parqit_hist_lo' + (_n - 0.5) * `parqit_hist_width'
        qui gen double __freq = 0
        mata: _parqit_fill_hist("`resp'")
        if ("`nodraw'" == "") {
            twoway bar __freq __mid, barwidth(`parqit_hist_width') ///
                xtitle(`"`var'"') ytitle("frequency") name(parqit_hist, replace)
        }
    }
    frame drop `hf'
    return scalar N = `parqit_n'
    return scalar bins = `parqit_hist_bins'
    return scalar width = `parqit_hist_width'
    return scalar start = `parqit_hist_lo'
end

program define _parqit_show
    version 16.0
    syntax
    _parqit_ensure_plugin
    tempfile resp
    mata: st_local("whathex", _parqit_hex("show"))
    mata: st_local("resphex", _parqit_hex(st_local("resp")))
    capture noisily plugin call parqit_plugin, view_info `whathex' `resphex'
    if (_rc) exit _rc
    mata: _parqit_print_resp("`resp'", "sql")
end

program define _parqit_explain
    version 16.0
    syntax
    _parqit_ensure_plugin
    tempfile resp
    mata: st_local("whathex", _parqit_hex("explain"))
    mata: st_local("resphex", _parqit_hex(st_local("resp")))
    capture noisily plugin call parqit_plugin, view_info `whathex' `resphex'
    if (_rc) exit _rc
    mata: _parqit_print_resp("`resp'", "plan")
end

program define _parqit_glimpse
    version 16.0
    _parqit_describe `0'
end

program define _parqit_describe, rclass
    version 16.0
    syntax [anything(name=target)]
    _parqit_ensure_plugin

    if (`"`target'"' == "") {
        * describe the open view
        tempfile resp
        mata: st_local("whathex", _parqit_hex("describe"))
        mata: st_local("resphex", _parqit_hex(st_local("resp")))
        capture noisily plugin call parqit_plugin, view_info `whathex' `resphex'
        if (_rc) exit _rc
        mata: st_local("vsrc", _parqit_unhex(st_local("parqit_view_src")))
        di as txt ""
        di as txt "  lazy view over " as res `"`vsrc'"'
        di as txt "  columns: " as res "`parqit_view_k'" ///
            as txt "   pipeline steps: " as res "`parqit_view_stages'"
        di as txt ""
        mata: _parqit_print_view_describe("`resp'")
        return scalar n_cols = `parqit_view_k'
        return scalar n_columns = `parqit_view_k'   /* pq-compatible alias */
        return scalar n_steps = `parqit_view_stages'
        exit
    }

    local file `target'
    tempfile req resp
    local _sq_file `"`file'"'
    mata: _parqit_wr_describe_request("`req'", "`resp'")
    capture noisily plugin call parqit_plugin, describe `reqhex'
    if (_rc) exit _rc

    di as txt ""
    di as txt `"  `_sq_file'"'
    di as txt "  rows: " as res %20.0fc `parqit_n' ///
        as txt "   columns: " as res "`parqit_k'" ///
        as txt "   row groups: " as res "`parqit_row_groups'" ///
        as txt "   files: " as res "`parqit_n_files'" ///
        as txt "   parqit metadata: " as res cond("`parqit_has_meta'" == "1", "yes", "no")
    di as txt ""
    mata: _parqit_resp_describe("`resp'")

    return scalar n_rows = `parqit_n'
    return scalar n_cols = `parqit_k'
    return scalar n_columns = `parqit_k'   /* pq-compatible alias of n_cols */
    return scalar n_row_groups = `parqit_row_groups'
    return scalar n_files = `parqit_n_files'
    return scalar has_parqit_meta = ("`parqit_has_meta'" == "1")
    forvalues i = 1/`parqit_k' {
        return local name_`i' `"`parqit_dname_`i''"'
        return local type_`i' `"`parqit_dtype_`i''"'
        return local stata_type_`i' `"`parqit_dstype_`i''"'
    }
end

* ----------------------------------------------------------------------------
* parqit save — view → parquet when a view is open; else in-memory → parquet
* ----------------------------------------------------------------------------

* Lossy-conversion notes shared by every Parquet-write path (in-memory save,
* view save, and the parqit open _data bridge) so the warnings never depend on
* which path produced the loss (ATOM-2). Each list is space-separated variable
* names; an empty list prints nothing.
program define _parqit_lossy_notes
    version 16.0
    syntax [, ext(string) frac(string)]
    if (`"`ext'"' != "") {
        di as txt "note: extended missing values (.a-.z) in " ///
            as res `"`ext'"' ///
            as txt " were written as nulls (Parquet has a single missing concept)"
    }
    if (`"`frac'"' != "") {
        di as txt "note: non-integer date/period values in " ///
            as res `"`frac'"' as txt " were rounded to the nearest unit"
    }
end

program define _parqit_save, rclass
    version 16.0
    syntax anything(name=target id="filename") [, replace Data ///
        COMPression(string) compression_level(integer -1) PARTition_by(string) ///
        Chunk(integer -1)]

    local dest `target'
    _parqit_ensure_plugin
    plugin call parqit_plugin, view_alive

    if ("`parqit_view_alive'" == "1" & "`data'" == "") {
        mata: st_local("vname", _parqit_unhex(st_local("parqit_view_current")))
        di as txt "(materialising view " as res "`vname'" ///
            as txt " — the dataset in memory is untouched; use the " ///
            as txt "{bf:data} option to export memory instead)"
        tempfile req
        local _sq_dest `"`dest'"'
        local _sq_replace = ("`replace'" != "")
        local _sq_comp `"`compression'"'
        local _sq_complevel = `compression_level'
        local _sq_partition `"`partition_by'"'
        local _sq_chunk = `chunk'
        mata: _parqit_wr_view_save_request("`req'")
        capture noisily plugin call parqit_plugin, view_save `reqhex'
        if (_rc) exit _rc
        mata: st_local("destabs", _parqit_unhex(st_local("parqit_dest")))
        * A view over a Parquet source carries no Stata extended missings, so
        * only the date/period rounding note can fire here (parqit_frac_dates is
        * set by view_save; parqit_ext_missing stays empty) — ATOM-2.
        _parqit_lossy_notes, ext(`"`parqit_ext_missing'"') frac(`"`parqit_frac_dates'"')
        di as txt "(" as res "`parqit_written_n'" as txt " obs, " ///
            as res "`parqit_written_k'" as txt `" vars written to `destabs')"'
        return local filename `"`destabs'"'
        return local view "`vname'"
        return scalar N = `parqit_written_n'
        return scalar k = `parqit_written_k'
        return local ext_missing `"`parqit_ext_missing'"'
        return local frac_dates `"`parqit_frac_dates'"'
        exit
    }

    if (c(k) == 0) {
        di as err "no variables defined"
        exit 111
    }
    qui ds
    local allvars `r(varlist)'

    tempfile req
    local _sq_dest `"`dest'"'
    local _sq_replace = ("`replace'" != "")
    local _sq_comp `"`compression'"'
    local _sq_complevel = `compression_level'
    local _sq_partition `"`partition_by'"'
    local _sq_chunk = `chunk'
    local _sq_dtalabel `: data label'
    local _sq_direct = 0
    local _fast_nonce : char _dta[_parqit_fast_source_nonce]
    local _fast_global `"${PARQIT_FAST_SOURCE_NONCE}"'
    if (`"`_fast_global'"' != "" & ///
        `"`_fast_nonce'"' == `"`_fast_global'"' & ///
        c(changed) == 0 & `"`c(filename)'"' == "") {
        local _sq_direct = 1
        local _sq_source `"$PARQIT_FAST_SOURCE_PATH"'
        local _sq_source_size `"$PARQIT_FAST_SOURCE_SIZE"'
        local _sq_source_mtime `"$PARQIT_FAST_SOURCE_MTIME"'
        local _sq_nobs = _N
    }
    mata: _parqit_wr_save_request("`req'")

    if (`_sq_direct') {
        capture noisily plugin call parqit_plugin `allvars', save_data_direct `reqhex'
        if (_rc) exit _rc
        if ("`parqit_direct_done'" == "1") {
            mata: st_local("destabs", _parqit_unhex(st_local("parqit_dest")))
            _parqit_lossy_notes, ext(`"`parqit_ext_missing'"') frac(`"`parqit_frac_dates'"')
            di as txt "(" as res "`parqit_written_n'" as txt " obs, " ///
                as res "`parqit_written_k'" as txt `" vars written to `destabs')"'
            return local filename `"`destabs'"'
            return scalar N = `parqit_written_n'
            return scalar k = `parqit_written_k'
            return local ext_missing `"`parqit_ext_missing'"'
            return local frac_dates `"`parqit_frac_dates'"'
            exit
        }
    }

    capture noisily plugin call parqit_plugin `allvars', save_data `reqhex'
    if (_rc) exit _rc

    mata: st_local("destabs", _parqit_unhex(st_local("parqit_dest")))
    _parqit_lossy_notes, ext(`"`parqit_ext_missing'"') frac(`"`parqit_frac_dates'"')
    di as txt "(" as res "`parqit_written_n'" as txt " obs, " ///
        as res "`parqit_written_k'" as txt `" vars written to `destabs')"'
    return local filename `"`destabs'"'
    return scalar N = `parqit_written_n'
    return scalar k = `parqit_written_k'
    return local ext_missing `"`parqit_ext_missing'"'
    return local frac_dates `"`parqit_frac_dates'"'
end

* ----------------------------------------------------------------------------
* parqit open _data — promote the in-memory dataset to a lazy view
* ----------------------------------------------------------------------------

program define _parqit_open, rclass
    version 16.0
    syntax anything(name=what) [, Name(name)]
    if (`"`what'"' != "_data") {
        di as err "parqit open: only {bf:parqit open _data} is supported"
        exit 198
    }
    if (c(k) == 0) {
        di as err "no variables defined"
        exit 111
    }
    local nobs = _N
    _parqit_ensure_plugin
    * bridge: snapshot the dataset to a parquet unique to THIS promotion —
    * a shared path would let a later open _data silently rebind every
    * earlier named view to the newest data. The view owns the file: the
    * plugin erases it when the view is closed or replaced.
    if ("${PARQIT_OPENDATA_SEQ}" == "") global PARQIT_OPENDATA_SEQ = 0
    global PARQIT_OPENDATA_SEQ = ${PARQIT_OPENDATA_SEQ} + 1
    local bridge "`c(tmpdir)'/_parqit_opendata_`c(pid)'_${PARQIT_OPENDATA_SEQ}.parquet"
    capture erase `"`bridge'"'
    qui _parqit_save `"`bridge'"', replace data
    * The bridge snapshot applies the same lossy conversions as any in-memory
    * save (extended missings -> null, fractional dates rounded). _parqit_save's
    * own notes were suppressed by `qui'; surface them here so the loss is not
    * silent when the dataset is later collected/saved through the view (ATOM-2).
    local _parqit_open_ext  `"`r(ext_missing)'"'
    local _parqit_open_frac `"`r(frac_dates)'"'
    if ("`name'" == "") qui _parqit_use using `"`bridge'"', owned
    else                qui _parqit_use using `"`bridge'"', name(`name') owned
    di as txt "(in-memory dataset promoted to a lazy view; " ///
        as txt "manipulate with parqit verbs, then parqit collect or parqit save)"
    _parqit_lossy_notes, ext(`"`_parqit_open_ext'"') frac(`"`_parqit_open_frac'"')
    if (`nobs' >= 1000000) {
        local nstr : di %15.0fc `nobs'
        _parqit_tip `"promoting `=trim("`nstr'")' obs writes a temporary bridge; if you only need to merge/append a small disk lookup, {bf:parqit mergein}/{bf:parqit appendin} keeps the data in Stata and skips it"'
    }
end

program define _parqit_close
    version 16.0
    syntax [anything(name=which)]
    _parqit_ensure_plugin
    if (`"`which'"' == "") {
        plugin call parqit_plugin, view_close
        di as txt "(current view closed)"
        exit
    }
    if (`"`which'"' == "_all") {
        mata: st_local("whex", _parqit_hex("_all"))
    }
    else {
        confirm name `which'
        mata: st_local("whex", _parqit_hex(st_local("which")))
    }
    capture noisily plugin call parqit_plugin, view_close `whex'
    if (_rc) exit _rc
    * sweep up any Parquet bridges made for dta/xls/xlsx/csv using sides
    if (`"`which'"' == "_all" & `"${PARQIT_IMPORT_BRIDGES}"' != "") {
        foreach b of global PARQIT_IMPORT_BRIDGES {
            capture erase `"`b'"'
        }
        global PARQIT_IMPORT_BRIDGES ""
    }
    di as txt "(view`=cond("`which'"=="_all","s","")' closed)"
end

* parqit view             -> list open views (same as parqit views)
* parqit view <name>      -> make <name> the current view
* parqit view <name>: cmd -> run one parqit command against <name>, then restore
program define _parqit_view, rclass
    version 16.0
    if (strtrim(`"`0'"') == "") {
        _parqit_views
        return add
        exit
    }
    gettoken name 0 : 0, parse(" :")
    confirm name `name'
    gettoken colon : 0, parse(" :")
    _parqit_ensure_plugin

    if (`"`colon'"' != ":") {
        mata: st_local("nhex", _parqit_hex(st_local("name")))
        capture noisily plugin call parqit_plugin, view_switch `nhex'
        if (_rc) exit _rc
        di as txt "(current view: " as res "`name'" as txt ")"
        return local view "`name'"
        exit
    }

    * prefix form: remember current, switch, run, switch back
    gettoken colon 0 : 0, parse(" :")
    plugin call parqit_plugin, view_alive
    mata: st_local("prev", _parqit_unhex(st_local("parqit_view_current")))
    mata: st_local("nhex", _parqit_hex(st_local("name")))
    capture noisily plugin call parqit_plugin, view_switch `nhex'
    if (_rc) exit _rc
    capture noisily parqit `0'
    local rc = _rc
    if ("`prev'" != "" & "`prev'" != "`name'") {
        mata: st_local("phex", _parqit_hex(st_local("prev")))
        capture plugin call parqit_plugin, view_switch `phex'
    }
    if (`rc') exit `rc'
    return add
end

program define _parqit_views, rclass
    version 16.0
    syntax
    _parqit_ensure_plugin
    tempfile resp
    mata: st_local("rhex", _parqit_hex(st_local("resp")))
    capture noisily plugin call parqit_plugin, view_list `rhex'
    if (_rc) exit _rc
    if (`parqit_n_views' == 0) {
        di as txt "(no views open)"
        return scalar n_views = 0
        exit
    }
    mata: _parqit_print_views("`resp'")
    return scalar n_views = `parqit_n_views'
end

program define _parqit_set
    version 16.0
    gettoken what 0 : 0, parse(" ")
    local value = strtrim(`"`0'"')
    if !inlist("`what'", "statamissing", "threads", "memory_limit", "tempdir") {
        di as err "parqit set: expected statamissing|threads|memory_limit|tempdir <value>"
        exit 198
    }
    if ("`what'" == "statamissing" & !inlist("`value'", "on", "off")) {
        di as err "parqit set statamissing: on or off"
        exit 198
    }
    _parqit_ensure_plugin
    mata: st_local("whathex", _parqit_hex(st_local("what")))
    mata: st_local("valhex", _parqit_hex(st_local("value")))
    capture noisily plugin call parqit_plugin, set `whathex' `valhex'
    if (_rc) exit _rc
end

* ----------------------------------------------------------------------------
* two-table verbs: merge / append / joinby (using side stays on disk)
* ----------------------------------------------------------------------------

program define _parqit_merge
    version 16.0
    * parqit merge 1:1|m:1|1:m|m:m keys using <file> [, keep() keepusing() gen() nogen]
    gettoken kind 0 : 0, parse(" ")
    if !inlist("`kind'", "1:1", "m:1", "1:m", "m:m") {
        di as err "parqit merge: kind must be 1:1, m:1, 1:m or m:m"
        exit 198
    }
    local keys
    gettoken tok : 0, parse(" ")
    while (`"`tok'"' != "using" & `"`tok'"' != "") {
        gettoken tok 0 : 0, parse(" ")
        local keys `keys' `tok'
        gettoken tok : 0, parse(" ")
    }
    if (strtrim("`keys'") == "") {
        di as err "parqit merge: key varlist required"
        exit 198
    }
    syntax using/ [, keep(string) KEEPUSing(string) GENerate(name) NOGENerate]
    if ("`generate'" != "" & "`nogenerate'" != "") {
        di as err "parqit merge: generate() and nogenerate are mutually exclusive"
        exit 198
    }
    * keep() is a set of master/using/match: build it from idempotent flags so a
    * repeated token (keep(master master)) cannot flip the mask to another subset
    * the way additive bits did (MERGE-2).
    local m_master 0
    local m_using  0
    local m_match  0
    if ("`keep'" != "") {
        foreach w of local keep {
            if inlist("`w'", "master", "1") local m_master 1
            else if inlist("`w'", "using", "2") local m_using 1
            else if inlist("`w'", "match", "matched", "3") local m_match 1
            else {
                di as err "parqit merge: keep() takes master, using and/or match"
                exit 198
            }
        }
    }
    local mask = `m_master' + 2 * `m_using' + 4 * `m_match'
    _parqit_ensure_plugin
    * a dta/xls/xlsx/csv using side is imported to a small Parquet bridge
    global PARQIT_RS_IN `"`using'"'
    _parqit_resolve_source using
    local using `"`r(path)'"'
    tempfile req
    local _sq_op "merge"
    local _sq_kind "`kind'"
    local _sq_keys "`keys'"
    local _sq_file `"`using'"'
    local _sq_keepusing `"`keepusing'"'
    local _sq_gen "`generate'"
    local _sq_nogen = ("`nogenerate'" != "")
    local _sq_mask `mask'
    mata: _parqit_wr_twotable_request("`req'")
    capture noisily plugin call parqit_plugin, view_twotable `reqhex'
    if (_rc) exit _rc
end

program define _parqit_append
    version 16.0
    * parqit append using <file> [<file> ...] [, generate(name)]
    gettoken usingtok 0 : 0, parse(" ")
    if (`"`usingtok'"' != "using") {
        di as err "parqit append: syntax is parqit append using <files> [, generate()]"
        exit 198
    }
    local nf 0
    local parsing 1
    while (`parsing') {
        gettoken f 0 : 0, parse(" ,")
        if (`"`f'"' == "" | `"`f'"' == ",") {
            if (`"`f'"' == ",") local 0 `", `0'"'
            local parsing 0
            continue
        }
        local ++nf
        local _sq_file_`nf' `"`f'"'
    }
    if (`nf' == 0) {
        di as err "parqit append: at least one using file required"
        exit 198
    }
    syntax [, GENerate(name)]
    _parqit_ensure_plugin
    * import any dta/xls/xlsx/csv source to a Parquet bridge
    forvalues i = 1/`nf' {
        global PARQIT_RS_IN `"`_sq_file_`i''"'
        _parqit_resolve_source using
        local _sq_file_`i' `"`r(path)'"'
    }
    tempfile req
    local _sq_op "append"
    local _sq_nfiles `nf'
    local _sq_gen "`generate'"
    mata: _parqit_wr_append_request("`req'")
    capture noisily plugin call parqit_plugin, view_twotable `reqhex'
    if (_rc) exit _rc
end

program define _parqit_joinby
    version 16.0
    * parqit joinby keys using <file>
    local keys
    gettoken tok : 0, parse(" ")
    while (`"`tok'"' != "using" & `"`tok'"' != "") {
        gettoken tok 0 : 0, parse(" ")
        local keys `keys' `tok'
        gettoken tok : 0, parse(" ")
    }
    if (strtrim("`keys'") == "") {
        di as err "parqit joinby: key varlist required"
        exit 198
    }
    syntax using/
    _parqit_ensure_plugin
    global PARQIT_RS_IN `"`using'"'
    _parqit_resolve_source using
    local using `"`r(path)'"'
    tempfile req
    local _sq_op "joinby"
    local _sq_keys "`keys'"
    local _sq_file `"`using'"'
    mata: _parqit_wr_twotable_request("`req'")
    capture noisily plugin call parqit_plugin, view_twotable `reqhex'
    if (_rc) exit _rc
end

* ----------------------------------------------------------------------------
* mergein / appendin — join the IN-MEMORY dataset with a disk file, fast.
*   The in-memory data stays put (no bridge round-trip through DuckDB); parqit
*   reads only the needed columns of the disk side (Parquet/CSV/dta/xlsx, with
*   projection pushdown) into a throwaway frame, then a NATIVE merge/append
*   runs. This is the fast route when the DISK side is the smaller (lookup) one;
*   for big-on-big prefer the out-of-core `parqit use … ; parqit merge` path.
* ----------------------------------------------------------------------------

program define _parqit_mergein, rclass
    version 16.0
    if (c(k) == 0) {
        di as err "parqit mergein: no data in memory (it joins the in-memory "  ///
            "dataset with a disk file; load data first)"
        exit 111
    }
    gettoken mtype 0 : 0, parse(" ")
    if !inlist("`mtype'", "1:1", "m:1", "1:m", "m:m") {
        di as err "parqit mergein: merge type must be 1:1, m:1, 1:m or m:m"
        exit 198
    }
    local keys
    gettoken tok : 0, parse(" ")
    while (`"`tok'"' != "using" & `"`tok'"' != "") {
        gettoken tok 0 : 0, parse(" ")
        local keys `keys' `tok'
        gettoken tok : 0, parse(" ")
    }
    if (strtrim("`keys'") == "") {
        di as err "parqit mergein: key varlist required"
        exit 198
    }
    syntax using/ [, KEEPUSing(string) keep(string) GENerate(name)       ///
        NOGENerate ASSERT(string) UPDATE replace NOLabel NONotes FORCE       ///
        NOREPort]

    * read only keys + keepusing of the disk side (projection pushdown)
    tempname fr
    tempfile tmp
    frame create `fr'
    frame `fr' {
        if ("`keepusing'" != "") qui parqit use `keys' `keepusing' using `"`using'"', clear
        else                     qui parqit use using `"`using'"', clear
        local disk_n = _N
        qui save `"`tmp'"', replace
    }
    frame drop `fr'

    if (`disk_n' >= 1000000) {
        local dstr : di %15.0fc `disk_n'
        _parqit_tip `"the disk side has `=trim("`dstr'")' obs; for a large two-table join it can be faster to do it out of core in parqit ({bf:parqit open _data} ; {bf:parqit merge} {it:...} {bf:using} {it:`using'} ; {bf:parqit collect}) and bring back only the result"'
    }

    * forward every native-merge option that was given
    local opts
    if ("`keepusing'" != "") local opts `opts' keepusing(`keepusing')
    if ("`keep'"      != "") local opts `opts' keep(`keep')
    if ("`generate'"  != "") local opts `opts' generate(`generate')
    if ("`nogenerate'"!= "") local opts `opts' nogenerate
    if (`"`assert'"'  != "") local opts `opts' assert(`assert')
    if ("`update'"    != "") local opts `opts' update
    if ("`replace'"   != "") local opts `opts' replace
    if ("`nolabel'"   != "") local opts `opts' nolabel
    if ("`nonotes'"   != "") local opts `opts' nonotes
    if ("`force'"     != "") local opts `opts' force
    if ("`noreport'"  != "") local opts `opts' noreport
    merge `mtype' `keys' using `"`tmp'"', `opts'
end

program define _parqit_appendin
    version 16.0
    syntax using/ [, KEEP(varlist) FORCE]
    tempname fr
    tempfile tmp
    frame create `fr'
    frame `fr' {
        qui parqit use using `"`using'"', clear
        local disk_n = _N
        qui save `"`tmp'"', replace
    }
    frame drop `fr'
    if (`disk_n' >= 1000000) {
        local dstr : di %15.0fc `disk_n'
        _parqit_tip `"appending `=trim("`dstr'")' obs into Stata; for a large append you only need on disk, {bf:parqit use} {it:A} ; {bf:parqit append using} {it:B} ; {bf:parqit save} stays out of core"'
    }
    local opts
    if ("`keep'"  != "") local opts `opts' keep(`keep')
    if ("`force'" != "") local opts `opts' force
    append using `"`tmp'"', `opts'
end

* ----------------------------------------------------------------------------
* M4: reshape, sql/query escape hatches, summaries, path
* ----------------------------------------------------------------------------

program define _parqit_reshape
    version 16.0
    gettoken dir 0 : 0, parse(" ,")
    if !inlist("`dir'", "long", "wide") {
        di as err "parqit reshape: direction must be long or wide"
        exit 198
    }
    * stubs up to the comma
    local stubs
    local parsing 1
    while (`parsing') {
        gettoken tok 0 : 0, parse(" ,")
        if (`"`tok'"' == "" | `"`tok'"' == ",") {
            if (`"`tok'"' == ",") local 0 `", `0'"'
            local parsing 0
            continue
        }
        local stubs `stubs' `tok'
    }
    if (strtrim("`stubs'") == "") {
        di as err "parqit reshape: stub varlist required"
        exit 198
    }
    syntax, i(string) j(name)
    _parqit_ensure_plugin
    tempfile req
    local _sq_dir "`dir'"
    local _sq_stubs "`stubs'"
    local _sq_i `"`i'"'
    local _sq_j "`j'"
    mata: _parqit_wr_reshape_request("`req'")
    capture noisily plugin call parqit_plugin, view_reshape `reqhex'
    if (_rc) exit _rc
end

program define _parqit_sql, rclass
    version 16.0
    * parqit sql "<DuckDB SQL>" [, clear]
    gettoken q 0 : 0, parse(",")
    local q = strtrim(`"`q'"')
    local q `q'
    if (`"`q'"' == "") {
        di as err `"parqit sql: a quoted SQL query is required"'
        exit 198
    }
    syntax [, clear Name(name)]
    if ("`name'" != "" & "`clear'" != "") {
        di as err "parqit sql: name() applies to lazy views; omit clear"
        exit 198
    }
    if ("`name'" == "") local name "default"
    _parqit_ensure_plugin
    tempfile req
    local _sq_sql `"`q'"'
    local _sq_vname "`name'"
    mata: _parqit_wr_sql_request("`req'")
    capture noisily plugin call parqit_plugin, view_sql `reqhex'
    if (_rc) exit _rc
    mata: st_local("vname", _parqit_unhex(st_local("parqit_view_name")))
    di as txt "(view " as res "`vname'" as txt " opened over the SQL result: " ///
        as res "`parqit_view_k'" as txt " columns)"
    return local view "`vname'"
    if ("`clear'" != "") {
        _parqit_collect, clear
        return add
    }
end

program define _parqit_query
    version 16.0
    gettoken frag 0 : 0, parse(",")
    local frag = strtrim(`"`frag'"')
    local frag `frag'
    if (`"`frag'"' == "") {
        di as err `"parqit query: a quoted SQL fragment is required (e.g. "qualify row_number() over (...) = 1")"'
        exit 198
    }
    _parqit_ensure_plugin
    tempfile req
    local _sq_frag `"`frag'"'
    mata: _parqit_wr_query_request("`req'")
    capture noisily plugin call parqit_plugin, view_query `reqhex'
    if (_rc) exit _rc
end

program define _parqit_summarize, rclass
    version 16.0
    syntax [anything(name=vars)] [, Detail]
    _parqit_ensure_plugin
    tempfile req resp
    local _sq_what = cond("`detail'" != "", "detail", "summarize")
    local _sq_vars `"`vars'"'
    mata: _parqit_wr_stats_request("`req'", "`resp'")
    capture noisily plugin call parqit_plugin, view_stats `reqhex'
    if (_rc) exit _rc
    if ("`detail'" == "") {
        mata: _parqit_print_summarize("`resp'")
        return scalar N    = `parqit_sum_n'
        return scalar mean = `parqit_sum_mean'
        return scalar sd   = `parqit_sum_sd'
        return scalar min  = `parqit_sum_min'
        return scalar max  = `parqit_sum_max'
        exit
    }
    mata: _parqit_print_detail("`resp'")
    return scalar N        = `parqit_det_n'
    return scalar mean     = `parqit_det_mean'
    return scalar sd       = `parqit_det_sd'
    return scalar Var      = `parqit_det_var'
    return scalar skewness = `parqit_det_skew'
    return scalar kurtosis = `parqit_det_kurt'
    return scalar min      = `parqit_det_min'
    return scalar max      = `parqit_det_max'
    foreach p in 1 5 10 25 50 75 90 95 99 {
        return scalar p`p' = `parqit_det_p`p''
    }
end

program define _parqit_tabulate, rclass
    version 16.0
    syntax anything(name=vars) [, Missing ROW COL]
    local nv : word count `vars'
    if (`nv' < 1 | `nv' > 2) {
        di as err "parqit tabulate: one variable (oneway) or two (twoway)"
        exit 198
    }
    _parqit_ensure_plugin
    tempfile req resp
    local _sq_what = cond(`nv' == 2, "tab2", "tabulate")
    local _sq_vars `"`vars'"'
    local _sq_missing = ("`missing'" != "")
    mata: _parqit_wr_stats_request("`req'", "`resp'")
    capture noisily plugin call parqit_plugin, view_stats `reqhex'
    if (_rc) exit _rc
    if (`nv' == 1) {
        mata: _parqit_print_tabulate("`resp'")
        return scalar N = `parqit_tab_n'
        return scalar r = `parqit_tab_r'
        exit
    }
    local parqit_tab2_row = ("`row'" != "")
    local parqit_tab2_col = ("`col'" != "")
    mata: _parqit_print_tab2("`resp'")
    return scalar N = `parqit_tab_n'
    return scalar r = `parqit_tab_r'
    return scalar c = `parqit_tab_c'
end

program define _parqit_misstable, rclass
    version 16.0
    gettoken maybe : 0, parse(" ,")
    local what "misstable"
    if (`"`maybe'"' == "patterns") {
        gettoken maybe 0 : 0, parse(" ,")
        local what "misspatterns"
    }
    else if (`"`maybe'"' == "summarize") {
        gettoken maybe 0 : 0, parse(" ,")
    }
    syntax [anything(name=vars)]
    _parqit_ensure_plugin
    tempfile req resp
    local _sq_what "`what'"
    local _sq_vars `"`vars'"'
    mata: _parqit_wr_stats_request("`req'", "`resp'")
    capture noisily plugin call parqit_plugin, view_stats `reqhex'
    if (_rc) exit _rc
    if ("`what'" == "misspatterns") {
        mata: _parqit_print_misspatterns("`resp'")
        return scalar r = `parqit_mp_r'
        exit
    }
    mata: _parqit_print_misstable("`resp'")
    return scalar N = `parqit_n'
    return scalar n_complete = `parqit_n_complete'
end

program define _parqit_levelsof, rclass
    version 16.0
    syntax anything(name=var) [, Limit(integer 5000)]
    _parqit_ensure_plugin
    tempfile req resp
    local _sq_what "levelsof"
    local _sq_vars `"`var'"'
    local _sq_limit `limit'
    mata: _parqit_wr_stats_request("`req'", "`resp'")
    capture noisily plugin call parqit_plugin, view_stats `reqhex'
    if (_rc) exit _rc
    mata: _parqit_build_levels("`resp'")
    di as txt `"`parqit_levels'"'
    return local levels `"`parqit_levels'"'
    return scalar r = `parqit_n_levels'
end

program define _parqit_path, rclass
    version 16.0
    syntax anything(name=target id="filename")
    local file `target'
    _parqit_ensure_plugin
    mata: st_local("phex", _parqit_hex(st_local("file")))
    capture noisily plugin call parqit_plugin, path `phex'
    if (_rc) exit _rc
    mata: st_local("pabs", _parqit_unhex(st_local("parqit_path")))
    di as txt `"  `pabs'"' as txt cond("`parqit_path_exists'" == "1", "", "  (does not exist)")
    return local path `"`pabs'"'
    return scalar exists = ("`parqit_path_exists'" == "1")
end

* ----------------------------------------------------------------------------
* Mata: hex codec twin + protocol writers/readers
* ----------------------------------------------------------------------------


version 16.0
mata:
mata set matastrict on

string scalar _parqit_hex(string scalar s)
{
    string scalar    d
    real rowvector   b
    string rowvector h
    real scalar      i, n

    d = "0123456789abcdef"
    n = strlen(s)
    if (n == 0) return("")
    b = ascii(s)
    h = J(1, n, "")
    for (i = 1; i <= n; i++) {
        h[i] = substr(d, floor(b[i] / 16) + 1, 1) + substr(d, mod(b[i], 16) + 1, 1)
    }
    return(invtokens(h, ""))
}

string scalar _parqit_unhex(string scalar x0)
{
    string scalar    d, x
    string rowvector parts
    real scalar      i, n, hi, lo

    d = "0123456789abcdef"
    x = strlower(x0)
    n = strlen(x)
    if (n == 0) return("")
    if (mod(n, 2)) {
        _error(3300, "parqit: malformed hex payload")
    }
    parts = J(1, n / 2, "")
    for (i = 1; i <= n / 2; i++) {
        hi = strpos(d, substr(x, 2 * i - 1, 1)) - 1
        lo = strpos(d, substr(x, 2 * i, 1)) - 1
        if (hi < 0 | lo < 0) {
            _error(3300, "parqit: malformed hex payload")
        }
        parts[i] = char(hi * 16 + lo)
    }
    return(invtokens(parts, ""))
}

// ---- JSON building: every double quote comes from char(34), so no Mata
// ---- compound-literal delimiter ambiguity can ever corrupt a request.

string scalar _parqit_jq(string scalar s)
{
    return(char(34) + s + char(34))
}

string scalar _parqit_jstr(string scalar s)
{
    return(_parqit_jq(_parqit_hex(s)))
}

string scalar _parqit_jpair(string scalar key, string scalar rawjson)
{
    return(_parqit_jq(key) + ":" + rawjson)
}

string scalar _parqit_jtext(string scalar key, string scalar value)
{
    return(_parqit_jpair(key, _parqit_jstr(value)))
}

string scalar _parqit_jlist(string rowvector items)
{
    string scalar out
    real scalar   i

    out = "["
    for (i = 1; i <= cols(items); i++) {
        if (i > 1) out = out + ","
        out = out + _parqit_jstr(items[i])
    }
    return(out + "]")
}

string scalar _parqit_jobj(string rowvector pairs)
{
    string scalar out
    real scalar   i

    out = "{"
    for (i = 1; i <= cols(pairs); i++) {
        if (i > 1) out = out + ","
        out = out + pairs[i]
    }
    return(out + "}")
}

void _parqit_write_file(string scalar path, string scalar content)
{
    real scalar fh

    fh = fopen(path, "w")
    fwrite(fh, content)
    fclose(fh)
}

void _parqit_emit(string scalar req, string scalar payload)
{
    _parqit_write_file(req, payload)
    st_local("reqhex", _parqit_hex(req))
}

// ---- request writers --------------------------------------------------

void _parqit_wr_use_request(string scalar req, string scalar resp,
                             string scalar strl)
{
    string rowvector p
    string scalar    vl

    p = (_parqit_jtext("cmd", "use_prepare"),
         _parqit_jpair("files", "[" + _parqit_jstr(st_local("_sq_file")) + "]"))
    vl = st_local("_sq_namelist")
    if (strtrim(vl) != "") {
        p = (p, _parqit_jpair("varlist", _parqit_jlist(tokens(vl))))
    }
    if (st_local("_sq_relaxed") == "1") {
        p = (p, _parqit_jpair("relaxed", "true"))
    }
    if (st_local("_sq_fmt") == "csv") {
        p = (p, _parqit_jpair("csv", "true"))
    }
    p = (p, _parqit_jtext("respfile", resp),
            _parqit_jtext("strlfile", strl),
            _parqit_jtext("tmpdir", st_global("c(tmpdir)")))
    _parqit_emit(req, _parqit_jobj(p))
}

void _parqit_wr_view_open_request(string scalar req)
{
    string rowvector p
    string scalar    vl

    p = (_parqit_jtext("cmd", "view_open"),
         _parqit_jtext("name", st_local("_sq_vname")),
         _parqit_jpair("files", "[" + _parqit_jstr(st_local("_sq_file")) + "]"))
    vl = st_local("_sq_namelist")
    if (strtrim(vl) != "") {
        p = (p, _parqit_jpair("varlist", _parqit_jlist(tokens(vl))))
    }
    if (st_local("_sq_owned") == "1") {
        p = (p, _parqit_jpair("owned", "true"))
    }
    if (st_local("_sq_relaxed") == "1") {
        p = (p, _parqit_jpair("relaxed", "true"))
    }
    if (st_local("_sq_fmt") == "csv") {
        p = (p, _parqit_jpair("csv", "true"))
    }
    p = (p, _parqit_jtext("tmpdir", st_global("c(tmpdir)")))
    _parqit_emit(req, _parqit_jobj(p))
}

void _parqit_wr_describe_request(string scalar req, string scalar resp)
{
    _parqit_emit(req, _parqit_jobj((
        _parqit_jtext("cmd", "describe"),
        _parqit_jpair("files", "[" + _parqit_jstr(st_local("_sq_file")) + "]"),
        _parqit_jtext("respfile", resp),
        _parqit_jtext("tmpdir", st_global("c(tmpdir)")))))
}

void _parqit_wr_collect_request(string scalar req, string scalar resp,
                                 string scalar strl)
{
    string rowvector p

    p = (_parqit_jtext("cmd", "view_collect_prepare"),
         _parqit_jtext("respfile", resp),
         _parqit_jtext("strlfile", strl),
         _parqit_jpair("limit", st_local("_sq_limit")),
         _parqit_jtext("tmpdir", st_global("c(tmpdir)")))
    if (st_local("_sq_pvars") != "") {
        p = (p, _parqit_jpair("vars", _parqit_jlist(tokens(st_local("_sq_pvars")))))
    }
    if (st_local("_sq_pfilter") != "") {
        p = (p, _parqit_jtext("filter", st_local("_sq_pfilter")))
    }
    if (st_local("_sq_pf") != "" & st_local("_sq_pf") != "0") {
        p = (p, _parqit_jpair("f", st_local("_sq_pf")),
                _parqit_jpair("l", st_local("_sq_pl")))
    }
    _parqit_emit(req, _parqit_jobj(p))
}

void _parqit_wr_op_expr_request(string scalar req)
{
    _parqit_emit(req, _parqit_jobj((
        _parqit_jtext("cmd", "view_op"),
        _parqit_jtext("op", st_local("_sq_op")),
        _parqit_jtext("expr", st_local("_sq_expr")))))
}

void _parqit_wr_op_names_request(string scalar req)
{
    _parqit_emit(req, _parqit_jobj((
        _parqit_jtext("cmd", "view_op"),
        _parqit_jtext("op", st_local("_sq_op")),
        _parqit_jpair("names", _parqit_jlist(tokens(st_local("_sq_names")))))))
}

void _parqit_wr_op_keepin_request(string scalar req)
{
    _parqit_emit(req, _parqit_jobj((
        _parqit_jtext("cmd", "view_op"),
        _parqit_jtext("op", "keep_in"),
        _parqit_jpair("f", st_local("_sq_f")),
        _parqit_jpair("l", st_local("_sq_l")))))
}

void _parqit_wr_op_rename_request(string scalar req)
{
    _parqit_emit(req, _parqit_jobj((
        _parqit_jtext("cmd", "view_op"),
        _parqit_jtext("op", "rename"),
        _parqit_jtext("old", st_local("_sq_old")),
        _parqit_jtext("new", st_local("_sq_new")))))
}

void _parqit_wr_op_sample_request(string scalar req)
{
    _parqit_emit(req, _parqit_jobj((
        _parqit_jtext("cmd", "view_op"),
        _parqit_jtext("op", "sample"),
        _parqit_jpair("amount", st_local("_sq_amount")),
        _parqit_jpair("count", st_local("_sq_count")),
        _parqit_jpair("seed", st_local("_sq_seed")))))
}

void _parqit_wr_op_gen_request(string scalar req, string scalar op)
{
    _parqit_emit(req, _parqit_jobj((
        _parqit_jtext("cmd", "view_op"),
        _parqit_jtext("op", op),
        _parqit_jtext("name", st_local("_sq_name")),
        _parqit_jtext("type", st_local("_sq_type")),
        _parqit_jtext("expr", st_local("parqit_expr")),
        _parqit_jtext("ifexpr", st_local("parqit_ifexpr")))))
}

void _parqit_wr_op_egen_request(string scalar req)
{
    _parqit_emit(req, _parqit_jobj((
        _parqit_jtext("cmd", "view_op"),
        _parqit_jtext("op", "egen"),
        _parqit_jtext("name", st_local("_sq_name")),
        _parqit_jtext("type", st_local("_sq_type")),
        _parqit_jtext("fcn", st_local("_sq_fcn")),
        _parqit_jtext("expr", st_local("_sq_expr")),
        _parqit_jpair("by", _parqit_jlist(tokens(st_local("_sq_by")))))))
}

void _parqit_wr_op_sort_request(string scalar req)
{
    string rowvector keys, descs
    string scalar    dj
    real scalar      i

    keys = tokens(st_local("_sq_keys"))
    descs = tokens(st_local("_sq_desc"))
    dj = "["
    for (i = 1; i <= cols(keys); i++) {
        if (i > 1) dj = dj + ","
        dj = dj + (i <= cols(descs) & descs[i] == "1" ? "true" : "false")
    }
    dj = dj + "]"
    _parqit_emit(req, _parqit_jobj((
        _parqit_jtext("cmd", "view_op"),
        _parqit_jtext("op", "sort"),
        _parqit_jpair("keys", _parqit_jlist(keys)),
        _parqit_jpair("desc", dj))))
}

void _parqit_wr_op_collapse_request(string scalar req)
{
    string rowvector items, parts
    string scalar    sj
    real scalar      i

    items = tokens(st_local("_sq_specs"))
    sj = "["
    for (i = 1; i <= cols(items); i++) {
        parts = _parqit_fields(items[i], 3)
        if (i > 1) sj = sj + ","
        sj = sj + _parqit_jobj((
            _parqit_jtext("stat", parts[1]),
            _parqit_jtext("target", parts[2]),
            _parqit_jtext("source", parts[3])))
    }
    sj = sj + "]"
    _parqit_emit(req, _parqit_jobj((
        _parqit_jtext("cmd", "view_op"),
        _parqit_jtext("op", "collapse"),
        _parqit_jpair("specs", sj),
        _parqit_jpair("by", _parqit_jlist(tokens(st_local("_sq_by")))))))
}

void _parqit_wr_op_contract_request(string scalar req)
{
    _parqit_emit(req, _parqit_jobj((
        _parqit_jtext("cmd", "view_op"),
        _parqit_jtext("op", "contract"),
        _parqit_jpair("names", _parqit_jlist(tokens(st_local("_sq_names")))),
        _parqit_jtext("freq", st_local("_sq_freq")))))
}

void _parqit_wr_op_dupdrop_request(string scalar req)
{
    _parqit_emit(req, _parqit_jobj((
        _parqit_jtext("cmd", "view_op"),
        _parqit_jtext("op", "dupdrop"),
        _parqit_jpair("names", _parqit_jlist(tokens(st_local("_sq_names")))),
        _parqit_jpair("force", st_local("_sq_force") == "1" ? "true" : "false"))))
}

void _parqit_wr_view_save_request(string scalar req)
{
    _parqit_emit(req, _parqit_jobj((
        _parqit_jtext("cmd", "view_save"),
        _parqit_jtext("dest", st_local("_sq_dest")),
        _parqit_jtext("tmpdir", st_global("c(tmpdir)")),
        _parqit_jpair("replace", st_local("_sq_replace") == "1" ? "true" : "false"),
        _parqit_jtext("compression", strlower(strtrim(st_local("_sq_comp")))),
        _parqit_jpair("compression_level", st_local("_sq_complevel")),
        _parqit_jpair("chunk", st_local("_sq_chunk")),
        _parqit_jpair("partition_by", _parqit_jlist(tokens(st_local("_sq_partition")))))))
}

void _parqit_wr_save_request(string scalar req)
{
    string scalar    j, name, src, vlname, ent
    string rowvector partv
    real scalar      i, k, lv, nlab, c
    string colvector labnames, texts, charnames
    real colvector   values

    k = st_nvar()
    j = "{" + _parqit_jtext("cmd", "save_data")
    j = j + "," + _parqit_jtext("dest", st_local("_sq_dest"))
    j = j + "," + _parqit_jtext("tmpdir", st_global("c(tmpdir)"))
    j = j + "," + _parqit_jtext("dtalabel", st_local("_sq_dtalabel"))
    j = j + "," + _parqit_jpair("replace",
                              st_local("_sq_replace") == "1" ? "true" : "false")
    j = j + "," + _parqit_jtext("compression",
                              strlower(strtrim(st_local("_sq_comp"))))
    j = j + "," + _parqit_jpair("compression_level", st_local("_sq_complevel"))
    j = j + "," + _parqit_jpair("chunk", st_local("_sq_chunk"))
    if (st_local("_sq_direct") == "1") {
        j = j + "," + _parqit_jtext("source_file", st_local("_sq_source"))
        j = j + "," + _parqit_jtext("source_size", st_local("_sq_source_size"))
        j = j + "," + _parqit_jtext("source_mtime", st_local("_sq_source_mtime"))
        j = j + "," + _parqit_jpair("nobs", st_local("_sq_nobs"))
    }
    partv = (strtrim(st_local("_sq_partition")) == "" ? J(1, 0, "")
             : tokens(st_local("_sq_partition")))
    j = j + "," + _parqit_jpair("partition_by", _parqit_jlist(partv))

    j = j + "," + _parqit_jq("vars") + ":["
    labnames = J(0, 1, "")
    for (i = 1; i <= k; i++) {
        if (i > 1) j = j + ","
        name = st_varname(i)
        src = st_global(name + "[src_name]")
        if (src == "") src = name
        vlname = st_varvaluelabel(i)
        if (vlname != "") labnames = labnames \ vlname
        j = j + _parqit_jobj((
            _parqit_jtext("name", name),
            _parqit_jtext("source", src),
            _parqit_jtext("type", st_vartype(i)),
            _parqit_jtext("fmt", st_varformat(i)),
            _parqit_jtext("varlab", st_varlabel(i)),
            _parqit_jtext("vallab", vlname)))
    }
    j = j + "]"

    labnames = uniqrows(labnames)
    j = j + "," + _parqit_jq("vallabs") + ":["
    nlab = 0
    for (lv = 1; lv <= rows(labnames); lv++) {
        if (!st_vlexists(labnames[lv])) continue
        st_vlload(labnames[lv], values, texts)
        if (nlab++ > 0) j = j + ","
        j = j + "{" + _parqit_jtext("name", labnames[lv]) + ","
        j = j + _parqit_jq("entries") + ":["
        for (i = 1; i <= rows(values); i++) {
            if (i > 1) j = j + ","
            ent = strtrim(strofreal(values[i], "%21.0g"))
            j = j + "[" + _parqit_jq(ent) + "," + _parqit_jstr(texts[i]) + "]"
        }
        j = j + "]}"
    }
    j = j + "]"

    j = j + "," + _parqit_jq("chars") + ":["
    nlab = 0
    charnames = st_dir("char", "_dta", "*")
    for (c = 1; c <= rows(charnames); c++) {
        if (charnames[c] == "_parqit_fast_source_nonce") continue
        if (nlab++ > 0) j = j + ","
        j = j + "[" + _parqit_jstr("_dta") + "," + _parqit_jstr(charnames[c]) + ","
        j = j + _parqit_jstr(st_global("_dta[" + charnames[c] + "]")) + "]"
    }
    for (i = 1; i <= k; i++) {
        name = st_varname(i)
        charnames = st_dir("char", name, "*")
        for (c = 1; c <= rows(charnames); c++) {
            if (nlab++ > 0) j = j + ","
            j = j + "[" + _parqit_jstr(name) + "," + _parqit_jstr(charnames[c]) + ","
            j = j + _parqit_jstr(st_global(name + "[" + charnames[c] + "]")) + "]"
        }
    }
    j = j + "]}"

    _parqit_emit(req, j)
}

// split "expr [if cond]" at the first top-level bare `if'
void _parqit_split_if(string scalar src)
{
    real scalar      i, n, depth, instr
    string scalar    c, q

    n = strlen(src)
    depth = 0
    instr = 0
    q = ""
    for (i = 1; i <= n; i++) {
        c = substr(src, i, 1)
        if (instr) {
            if (c == q) instr = 0
            continue
        }
        if (c == char(34)) {
            instr = 1
            q = char(34)
            continue
        }
        if (c == "(") depth++
        else if (c == ")") depth--
        else if (depth == 0 & c == "i" & substr(src, i, 3) == "if ") {
            if (i == 1 | substr(src, i - 1, 1) == " ") {
                st_local("parqit_expr", strtrim(substr(src, 1, i - 1)))
                st_local("parqit_ifexpr", strtrim(substr(src, i + 3, .)))
                return
            }
        }
    }
    st_local("parqit_expr", strtrim(src))
    st_local("parqit_ifexpr", "")
}

string rowvector _parqit_fields(string scalar line, real scalar n)
{
    string rowvector t, out
    real scalar      i

    t = ustrsplit(line, "\|")
    out = J(1, n, "")
    for (i = 1; i <= min((n, cols(t))); i++) out[i] = t[i]
    return(out)
}

/* Read the whole response file and split into records on the newline. Mata's
 * fget() caps a line at 32768 bytes and continues the remainder as a bogus
 * record, which truncates/corrupts a long hex field (a 32000-byte value label,
 * a long note/characteristic) — META-1. fread()+split has no line-length limit;
 * a hex/ASCII record never contains a newline, so the split is exact. */
string colvector _parqit_resp_lines(string scalar resp)
{
    real scalar      fh, i
    string scalar    buf, chunk
    string colvector all, out

    fh = fopen(resp, "r")
    buf = ""
    while ((chunk = fread(fh, 1048576)) != J(0, 0, "")) buf = buf + chunk
    fclose(fh)
    if (buf == "") return(J(0, 1, ""))
    all = ustrsplit(buf, char(10))'
    out = J(0, 1, "")
    for (i = 1; i <= rows(all); i++)
        if (all[i] != "") out = out \ all[i]
    return(out)
}

void _parqit_resp_create(string scalar resp, real scalar n)
{
    real scalar      fh, idx
    string scalar    line, name, code, fmt
    string rowvector f

    fh = fopen(resp, "r")
    while ((line = fget(fh)) != J(0, 0, "")) {
        f = _parqit_fields(line, 8)
        if (f[1] != "var") continue
        name = _parqit_unhex(f[3])
        code = _parqit_unhex(f[5])
        idx = st_addvar(code, name)
        fmt = _parqit_unhex(f[6])
        if (fmt != "") st_varformat(idx, fmt)
    }
    fclose(fh)
    if (n > 0) st_addobs(n)
}

void _parqit_apply_strl(string scalar path)
{
    real scalar   fh, v, o, len
    string scalar hdr, payload

    if (!fileexists(path)) return
    fh = fopen(path, "r")
    /* fixed 35-byte header: var(10) + obs(13) + len(12) — must match the
     * writer in plugin_io.cpp fill_column */
    while ((hdr = fread(fh, 35)) != J(0, 0, "")) {
        if (strlen(hdr) < 35) break
        v   = strtoreal(substr(hdr, 1, 10))
        o   = strtoreal(substr(hdr, 11, 13))
        len = strtoreal(substr(hdr, 24, 12))
        payload = (len > 0 ? fread(fh, len) : "")
        st_sstore(o, v, payload)
    }
    fclose(fh)
}

void _parqit_resp_decorate(string scalar resp)
{
    real scalar      nv, i, v, _li, _nl
    string scalar    line, name, varlab, vallab, labname, txt, tgt, cname, vraw
    string rowvector f
    string colvector vl_names, vl_texts_all, vl_owner, _lines
    real colvector   vl_vals_all, sel

    _lines = _parqit_resp_lines(resp)
    _nl = rows(_lines)
    vl_owner = J(0, 1, "")
    vl_vals_all = J(0, 1, .)
    vl_texts_all = J(0, 1, "")
    for (_li = 1; _li <= _nl; _li++) {
        line = _lines[_li]
        f = _parqit_fields(line, 8)
        if (f[1] == "var") {
            name = _parqit_unhex(f[3])
            varlab = _parqit_unhex(f[7])
            vallab = _parqit_unhex(f[8])
            if (varlab != "") st_varlabel(name, varlab)
            /* a foreign value-label NAME that is not a legal Stata name would
             * abort st_varvaluelabel — warn and skip, never fail the load */
            if (vallab != "" & st_isnumvar(name)) {
                if (st_isname(vallab)) st_varvaluelabel(name, vallab)
                else printf("note: %s: skipping value label with invalid name %s\n",
                            name, vallab)
            }
            if (_parqit_unhex(f[4]) != name) {
                st_global(name + "[src_name]", _parqit_unhex(f[4]))
            }
        }
        else if (f[1] == "vlab") {
            labname = _parqit_unhex(f[3])
            vraw = _parqit_unhex(f[2])
            v = strtoreal(vraw)
            txt = _parqit_unhex(f[4])
            /* Stata value-label keys must be integers and the label a legal
             * name; a foreign/corrupt file can carry neither. Skip loudly so a
             * non-integer key (1.5, "abc") can never silently overwrite a real
             * key or abort the load. */
            if (!st_isname(labname))
                printf("note: skipping value label with invalid name %s\n", labname)
            else if (v < . & (v != trunc(v) | abs(v) >= 2147483648))
                printf("note: value label %s: skipping non-integer/out-of-range key %s\n", labname, vraw)
            else {
                vl_owner = vl_owner \ labname
                vl_vals_all = vl_vals_all \ v
                vl_texts_all = vl_texts_all \ txt
            }
        }
        else if (f[1] == "char") {
            tgt = _parqit_unhex(f[2])
            cname = _parqit_unhex(f[3])
            /* a char whose target/name is not a legal Stata name would abort
             * st_global; warn and skip the characteristic instead */
            if (st_isname(cname) & (tgt == "_dta" | st_isname(tgt)))
                st_global(tgt + "[" + cname + "]", _parqit_unhex(f[4]))
            else
                printf("note: skipping characteristic %s[%s] (invalid name)\n",
                       tgt, cname)
        }
        else if (f[1] == "dlabel") {
            st_local("parqit_dtalabel", f[2])
        }
        else if (f[1] == "drop") {
            displayas("error")
            printf("warning: column %s dropped: %s\n",
                   _parqit_unhex(f[2]), _parqit_unhex(f[3]))
        }
        else if (f[1] == "warn") {
            displayas("text")
            printf("note: %s\n", _parqit_unhex(f[2]))
        }
    }
    vl_names = uniqrows(vl_owner)
    nv = rows(vl_names)
    for (i = 1; i <= nv; i++) {
        sel = selectindex(vl_owner :== vl_names[i])
        st_vlmodify(vl_names[i], vl_vals_all[sel], vl_texts_all[sel])
    }
}

void _parqit_resp_describe(string scalar resp)
{
    real scalar      fh, i, k
    string scalar    line
    string rowvector f
    string colvector dnames, dtypes, snames, stypes, sfmts

    fh = fopen(resp, "r")
    dnames = dtypes = snames = stypes = sfmts = J(0, 1, "")
    while ((line = fget(fh)) != J(0, 0, "")) {
        f = _parqit_fields(line, 8)
        if (f[1] == "dtype") {
            dnames = dnames \ _parqit_unhex(f[2])
            dtypes = dtypes \ _parqit_unhex(f[3])
        }
        else if (f[1] == "var") {
            snames = snames \ _parqit_unhex(f[3])
            stypes = stypes \ _parqit_unhex(f[5])
            sfmts  = sfmts  \ _parqit_unhex(f[6])
        }
        else if (f[1] == "drop") {
            displayas("error")
            printf("  (column %s not loadable: %s)\n",
                   _parqit_unhex(f[2]), _parqit_unhex(f[3]))
        }
    }
    fclose(fh)

    displayas("text")
    printf("  %-32s %-18s %-10s %s\n", "variable", "parquet type", "stata type", "format")
    printf("  %s\n", 72 * "-")
    k = rows(snames)
    for (i = 1; i <= k; i++) {
        printf("  %-32s %-18s %-10s %s\n",
               snames[i],
               (i <= rows(dtypes) ? dtypes[i] : ""),
               stypes[i], sfmts[i])
        st_local("parqit_dname_" + strofreal(i), snames[i])
        st_local("parqit_dtype_" + strofreal(i),
                 (i <= rows(dtypes) ? dtypes[i] : ""))
        st_local("parqit_dstype_" + strofreal(i), stypes[i])
    }
    printf("\n")
}

void _parqit_print_resp(string scalar resp, string scalar kind)
{
    real scalar      _li, _nl
    string scalar    line
    string rowvector f
    string colvector _lines

    _lines = _parqit_resp_lines(resp)
    _nl = rows(_lines)
    displayas("text")
    for (_li = 1; _li <= _nl; _li++) {
        line = _lines[_li]
        f = _parqit_fields(line, 3)
        if (f[1] == kind & kind == "sql") {
            printf("%s\n", _parqit_unhex(f[2]))
        }
        else if (f[1] == kind) {
            printf("%s\n%s\n", _parqit_unhex(f[2]), _parqit_unhex(f[3]))
        }
    }
}

void _parqit_print_view_describe(string scalar resp)
{
    real scalar      fh
    string scalar    line, kind
    string rowvector f

    fh = fopen(resp, "r")
    displayas("text")
    printf("  %-32s %-8s %-12s %s\n", "variable", "kind", "format", "label")
    printf("  %s\n", 72 * "-")
    while ((line = fget(fh)) != J(0, 0, "")) {
        f = _parqit_fields(line, 6)
        if (f[1] != "vcol") continue
        kind = (_parqit_unhex(f[4]) == "s" ? "string" : "numeric")
        printf("  %-32s %-8s %-12s %s\n", _parqit_unhex(f[3]), kind,
               _parqit_unhex(f[5]), _parqit_unhex(f[6]))
    }
    fclose(fh)
    printf("\n")
}
end

version 16.0
mata:

void _parqit_wr_twotable_request(string scalar req)
{
    string rowvector p

    p = (_parqit_jtext("cmd", "view_twotable"),
         _parqit_jtext("op", st_local("_sq_op")),
         _parqit_jpair("files", "[" + _parqit_jstr(st_local("_sq_file")) + "]"),
         _parqit_jpair("keys", _parqit_jlist(tokens(st_local("_sq_keys")))),
         _parqit_jtext("tmpdir", st_global("c(tmpdir)")))
    if (st_local("_sq_op") == "merge") {
        p = (p, _parqit_jtext("kind", st_local("_sq_kind")),
                _parqit_jpair("keepusing",
                            _parqit_jlist(tokens(st_local("_sq_keepusing")))),
                _parqit_jtext("gen", st_local("_sq_gen")),
                _parqit_jpair("nogen",
                            st_local("_sq_nogen") == "1" ? "true" : "false"),
                _parqit_jpair("keep_mask", st_local("_sq_mask")))
    }
    _parqit_emit(req, _parqit_jobj(p))
}

void _parqit_wr_append_request(string scalar req)
{
    real scalar      i, nf
    string scalar    flist

    nf = strtoreal(st_local("_sq_nfiles"))
    flist = "["
    for (i = 1; i <= nf; i++) {
        if (i > 1) flist = flist + ","
        flist = flist + _parqit_jstr(st_local("_sq_file_" + strofreal(i)))
    }
    flist = flist + "]"
    _parqit_emit(req, _parqit_jobj((
        _parqit_jtext("cmd", "view_twotable"),
        _parqit_jtext("op", "append"),
        _parqit_jpair("files", flist),
        _parqit_jpair("keys", "[]"),
        _parqit_jtext("gen", st_local("_sq_gen")),
        _parqit_jtext("tmpdir", st_global("c(tmpdir)")))))
}
end

version 16.0
mata:

void _parqit_wr_reshape_request(string scalar req)
{
    _parqit_emit(req, _parqit_jobj((
        _parqit_jtext("cmd", "view_reshape"),
        _parqit_jtext("dir", st_local("_sq_dir")),
        _parqit_jpair("stubs", _parqit_jlist(tokens(st_local("_sq_stubs")))),
        _parqit_jpair("i", _parqit_jlist(tokens(st_local("_sq_i")))),
        _parqit_jtext("j", st_local("_sq_j")))))
}

void _parqit_wr_sql_request(string scalar req)
{
    _parqit_emit(req, _parqit_jobj((
        _parqit_jtext("cmd", "view_sql"),
        _parqit_jtext("name", st_local("_sq_vname")),
        _parqit_jtext("sql", st_local("_sq_sql")),
        _parqit_jtext("tmpdir", st_global("c(tmpdir)")))))
}

void _parqit_wr_query_request(string scalar req)
{
    _parqit_emit(req, _parqit_jobj((
        _parqit_jtext("cmd", "view_query"),
        _parqit_jtext("fragment", st_local("_sq_frag")))))
}

void _parqit_wr_stats_request(string scalar req, string scalar resp)
{
    string rowvector p

    p = (_parqit_jtext("cmd", "view_stats"),
         _parqit_jtext("what", st_local("_sq_what")),
         _parqit_jpair("vars", _parqit_jlist(tokens(st_local("_sq_vars")))),
         _parqit_jtext("respfile", resp))
    if (st_local("_sq_limit") != "") {
        p = (p, _parqit_jpair("limit", st_local("_sq_limit")))
    }
    if (st_local("_sq_expr") != "") {
        p = (p, _parqit_jtext("expr", st_local("_sq_expr")))
    }
    if (st_local("_sq_joint") == "1") {
        p = (p, _parqit_jpair("joint", "true"))
    }
    if (st_local("_sq_pairwise") != "") {
        p = (p, _parqit_jpair("pairwise", st_local("_sq_pairwise")))
    }
    if (st_local("_sq_missing") == "1") {
        p = (p, _parqit_jpair("missing", "true"))
    }
    if (st_local("_sq_stats") != "") {
        p = (p, _parqit_jpair("stats", _parqit_jlist(tokens(st_local("_sq_stats")))))
    }
    if (st_local("_sq_by") != "") {
        p = (p, _parqit_jtext("by", st_local("_sq_by")))
    }
    if (st_local("_sq_bins") != "" & st_local("_sq_bins") != "0") {
        p = (p, _parqit_jpair("bins", st_local("_sq_bins")))
    }
    _parqit_emit(req, _parqit_jobj(p))
}

void _parqit_print_summarize(string scalar resp)
{
    real scalar      fh
    string scalar    line
    string rowvector f

    fh = fopen(resp, "r")
    displayas("text")
    printf("\n  %-24s %10s %12s %12s %12s %12s\n",
           "variable", "obs", "mean", "sd", "min", "max")
    printf("  %s\n", 86 * "-")
    while ((line = fget(fh)) != J(0, 0, "")) {
        f = _parqit_fields(line, 7)
        if (f[1] != "stat") continue
        printf("  %-24s %10s %12s %12s %12s %12s\n",
               _parqit_unhex(f[7]),
               f[2],
               substr(f[3], 1, 12), substr(f[4], 1, 12),
               substr(f[5], 1, 12), substr(f[6], 1, 12))
        st_local("parqit_sum_n", f[2] == "." ? "0" : f[2])
        st_local("parqit_sum_mean", f[3])
        st_local("parqit_sum_sd", f[4])
        st_local("parqit_sum_min", f[5])
        st_local("parqit_sum_max", f[6])
    }
    fclose(fh)
    printf("\n")
    if (st_local("parqit_sum_n") == "") st_local("parqit_sum_n", "0")
    if (st_local("parqit_sum_mean") == "") st_local("parqit_sum_mean", ".")
    if (st_local("parqit_sum_sd") == "") st_local("parqit_sum_sd", ".")
    if (st_local("parqit_sum_min") == "") st_local("parqit_sum_min", ".")
    if (st_local("parqit_sum_max") == "") st_local("parqit_sum_max", ".")
}

void _parqit_print_tabulate(string scalar resp)
{
    real scalar      fh, total, rows, n, i
    string scalar    line
    string rowvector f
    string colvector vals
    real colvector   counts

    fh = fopen(resp, "r")
    vals = J(0, 1, "")
    counts = J(0, 1, .)
    while ((line = fget(fh)) != J(0, 0, "")) {
        f = _parqit_fields(line, 3)
        if (f[1] != "tab") continue
        counts = counts \ strtoreal(f[2])
        vals = vals \ _parqit_unhex(f[3])
    }
    fclose(fh)
    total = sum(counts)
    rows = rows(vals)
    displayas("text")
    printf("\n  %-32s %12s %9s %9s\n", "value", "freq.", "percent", "cum.")
    printf("  %s\n", 66 * "-")
    n = 0
    for (i = 1; i <= rows; i++) {
        n = n + counts[i]
        printf("  %-32s %12.0f %8.2f%% %8.2f%%\n", vals[i], counts[i],
               100 * counts[i] / total, 100 * n / total)
    }
    printf("  %s\n", 66 * "-")
    printf("  %-32s %12.0f\n\n", "total", total)
    st_local("parqit_tab_n", strofreal(total, "%21.0g"))
    st_local("parqit_tab_r", strofreal(rows, "%21.0g"))
}
end

version 16.0
mata:

void _parqit_print_views(string scalar resp)
{
    real scalar      fh
    string scalar    line, cur
    string rowvector f

    fh = fopen(resp, "r")
    displayas("text")
    printf("\n    %-20s %8s %8s   %s\n", "view", "columns", "steps", "source")
    printf("    %s\n", 70 * "-")
    while ((line = fget(fh)) != J(0, 0, "")) {
        f = _parqit_fields(line, 6)
        if (f[1] != "view") continue
        cur = (f[2] == "1" ? "* " : "  ")
        printf("  %s%-20s %8s %8s   %s\n", cur, _parqit_unhex(f[5]), f[3], f[4],
               abbrev(_parqit_unhex(f[6]), 40))
    }
    fclose(fh)
    printf("    (* = current)\n\n")
}
end

version 16.0
mata:

void _parqit_print_misstable(string scalar resp)
{
    real scalar      fh, nm, nt, ncomp
    string scalar    line
    string rowvector f

    fh = fopen(resp, "r")
    displayas("text")
    printf("\n  %-32s %12s %12s %9s\n", "variable", "missing", "obs", "share")
    printf("  %s\n", 70 * "-")
    nt = 0
    while ((line = fget(fh)) != J(0, 0, "")) {
        f = _parqit_fields(line, 4)
        if (f[1] != "miss") continue
        nm = strtoreal(f[2])
        nt = strtoreal(f[3])
        printf("  %-32s %12.0f %12.0f %8.2f%%\n", _parqit_unhex(f[4]), nm, nt,
               nt > 0 ? 100 * nm / nt : 0)
    }
    fclose(fh)
    /* the plugin computed complete observations row-wise over the
     * selected variables (count of rows with no missing in any of them) */
    ncomp = strtoreal(st_local("parqit_n_complete"))
    printf("  %s\n", 70 * "-")
    printf("  complete observations: %12.0f of %12.0f (%5.2f%%)\n\n",
           ncomp, nt, nt > 0 ? 100 * ncomp / nt : 0)
}

void _parqit_print_detail(string scalar resp)
{
    real scalar      fh, i, p
    string scalar    line, name
    string rowvector f, pl

    pl = ("1", "5", "10", "25", "50", "75", "90", "95", "99")
    fh = fopen(resp, "r")
    displayas("text")
    while ((line = fget(fh)) != J(0, 0, "")) {
        f = _parqit_fields(line, 19)
        if (f[1] != "det") continue
        name = _parqit_unhex(f[19])
        printf("\n  {bf:%s}\n", name)
        printf("  %s\n", 60 * "-")
        printf("  %12s %-14s %14s %-12s\n", "obs", f[2], "mean", substr(f[3], 1, 12))
        printf("  %12s %-14s %14s %-12s\n", "sd", substr(f[4], 1, 12),
               "variance", substr(f[5], 1, 12))
        printf("  %12s %-14s %14s %-12s\n", "skewness", substr(f[6], 1, 12),
               "kurtosis", substr(f[7], 1, 12))
        printf("  %12s %-14s %14s %-12s\n", "min", substr(f[8], 1, 12),
               "max", substr(f[9], 1, 12))
        for (i = 1; i <= 9; i = i + 2) {
            printf("  %11s%% %-14s %13s%% %-12s\n", pl[i], substr(f[9 + i], 1, 12),
                   (i < 9 ? pl[i + 1] : ""), (i < 9 ? substr(f[10 + i], 1, 12) : ""))
        }
        st_local("parqit_det_n", f[2])
        st_local("parqit_det_mean", f[3])
        st_local("parqit_det_sd", f[4])
        st_local("parqit_det_var", f[5])
        st_local("parqit_det_skew", f[6])
        st_local("parqit_det_kurt", f[7])
        st_local("parqit_det_min", f[8])
        st_local("parqit_det_max", f[9])
        for (p = 1; p <= 9; p++) {
            st_local("parqit_det_p" + pl[p], f[9 + p])
        }
    }
    fclose(fh)
    printf("\n")
}

void _parqit_print_tab2(string scalar resp)
{
    real scalar      fh, i, j, r, c, n, total
    string scalar    line
    string rowvector f
    string colvector rv, cv, cells_r, cells_c
    real colvector   cells_n
    real matrix      M
    real colvector   rowtot
    real rowvector   coltot

    fh = fopen(resp, "r")
    cells_r = cells_c = J(0, 1, "")
    cells_n = J(0, 1, .)
    while ((line = fget(fh)) != J(0, 0, "")) {
        f = _parqit_fields(line, 4)
        if (f[1] != "t2") continue
        cells_n = cells_n \ strtoreal(f[2])
        cells_r = cells_r \ _parqit_unhex(f[3])
        cells_c = cells_c \ _parqit_unhex(f[4])
    }
    fclose(fh)
    rv = uniqrows(cells_r)
    cv = uniqrows(cells_c)
    r = rows(rv)
    c = rows(cv)
    M = J(r, c, 0)
    n = rows(cells_n)
    for (i = 1; i <= n; i++) {
        M[selectindex(rv :== cells_r[i]), selectindex(cv :== cells_c[i])] =
            cells_n[i]
    }
    rowtot = rowsum(M)
    coltot = colsum(M)
    total = sum(M)

    displayas("text")
    printf("\n  %-20s", "")
    for (j = 1; j <= c; j++) printf(" %10s", abbrev(cv[j], 10))
    printf(" | %10s\n", "total")
    printf("  %s\n", (22 + 11 * (c + 1)) * "-")
    for (i = 1; i <= r; i++) {
        printf("  %-20s", abbrev(rv[i], 20))
        for (j = 1; j <= c; j++) printf(" %10.0f", M[i, j])
        printf(" | %10.0f\n", rowtot[i])
        if (st_local("parqit_tab2_row") == "1") {
            printf("  %-20s", "")
            for (j = 1; j <= c; j++) printf(" %9.2f%%",
                rowtot[i] > 0 ? 100 * M[i, j] / rowtot[i] : 0)
            printf(" | %9.2f%%\n", 100)
        }
        if (st_local("parqit_tab2_col") == "1") {
            printf("  %-20s", "")
            for (j = 1; j <= c; j++) printf(" %9.2f%%",
                coltot[j] > 0 ? 100 * M[i, j] / coltot[j] : 0)
            printf(" | %9.2f%%\n", total > 0 ? 100 * rowtot[i] / total : 0)
        }
    }
    printf("  %s\n", (22 + 11 * (c + 1)) * "-")
    printf("  %-20s", "total")
    for (j = 1; j <= c; j++) printf(" %10.0f", coltot[j])
    printf(" | %10.0f\n", total)
    if (st_local("parqit_tab2_row") == "1") {
        printf("  %-20s", "")
        for (j = 1; j <= c; j++) printf(" %9.2f%%",
            total > 0 ? 100 * coltot[j] / total : 0)
        printf(" | %9.2f%%\n", 100)
    }
    if (st_local("parqit_tab2_col") == "1") {
        printf("  %-20s", "")
        for (j = 1; j <= c; j++) printf(" %9.2f%%", 100)
        printf(" | %9.2f%%\n", 100)
    }
    printf("\n")

    st_local("parqit_tab_n", strofreal(total, "%21.0g"))
    st_local("parqit_tab_r", strofreal(r, "%21.0g"))
    st_local("parqit_tab_c", strofreal(c, "%21.0g"))
}

void _parqit_build_levels(string scalar resp)
{
    real scalar      fh, n
    string scalar    line, out, v, kind
    string rowvector f

    kind = st_local("parqit_lvl_kind")
    fh = fopen(resp, "r")
    out = ""
    n = 0
    while ((line = fget(fh)) != J(0, 0, "")) {
        f = _parqit_fields(line, 2)
        if (f[1] != "lvl") continue
        v = _parqit_unhex(f[2])
        n++
        if (n > 1) out = out + " "
        if (kind == "s") out = out + "`" + char(34) + v + char(34) + "'"
        else             out = out + v
    }
    fclose(fh)
    st_local("parqit_levels", out)
}
end

version 16.0
mata:

// split "expr [in f/l]" at a trailing ` in f/l'
void _parqit_split_in(string scalar src)
{
    real scalar      p
    string scalar    tailpart
    string rowvector t

    st_local("parqit_inexpr", strtrim(src))
    st_local("parqit_inrange", "")
    p = strrpos(src, " in ")
    if (p == 0) return
    tailpart = strtrim(substr(src, p + 4, .))
    t = ustrsplit(tailpart, "/")
    if (cols(t) == 2 & strtoreal(t[1]) != . & strtoreal(t[2]) != .) {
        st_local("parqit_inexpr", strtrim(substr(src, 1, p - 1)))
        st_local("parqit_inrange", strtrim(t[1]) + " " + strtrim(t[2]))
    }
    else if (cols(t) == 1 & strtoreal(t[1]) != .) {
        st_local("parqit_inexpr", strtrim(substr(src, 1, p - 1)))
        st_local("parqit_inrange", strtrim(t[1]) + " " + strtrim(t[1]))
    }
}

void _parqit_collect_names(string scalar resp, string scalar unused)
{
    real scalar      fh
    string scalar    line, out
    string rowvector f

    fh = fopen(resp, "r")
    out = ""
    while ((line = fget(fh)) != J(0, 0, "")) {
        f = _parqit_fields(line, 6)
        if (f[1] != "vcol") continue
        out = out + (out == "" ? "" : " ") + _parqit_unhex(f[3])
    }
    fclose(fh)
    st_local("parqit_dsnames", out)
}

void _parqit_lookfor_resp(string scalar resp)
{
    real scalar      fh, i, hit
    string scalar    line, name, lab, out
    string rowvector f, terms

    terms = tokens(strlower(st_local("_sq_terms")))
    fh = fopen(resp, "r")
    out = ""
    displayas("text")
    while ((line = fget(fh)) != J(0, 0, "")) {
        f = _parqit_fields(line, 6)
        if (f[1] != "vcol") continue
        name = _parqit_unhex(f[3])
        lab = _parqit_unhex(f[6])
        hit = 0
        for (i = 1; i <= cols(terms); i++) {
            if (strpos(strlower(name), terms[i]) | strpos(strlower(lab), terms[i])) {
                hit = 1
            }
        }
        if (hit) {
            printf("  %-32s %s\n", name, lab)
            out = out + (out == "" ? "" : " ") + name
        }
    }
    fclose(fh)
    if (out == "") printf("  (nothing matched)\n")
    st_local("parqit_dsnames", out)
}

void _parqit_print_codebook(string scalar resp)
{
    real scalar      fh
    string scalar    line
    string rowvector f

    fh = fopen(resp, "r")
    displayas("text")
    printf("\n  %-24s %-7s %10s %9s %10s  %-12s %-12s %s\n",
           "variable", "kind", "obs", "missing", "distinct", "min", "max", "label")
    printf("  %s\n", 104 * "-")
    while ((line = fget(fh)) != J(0, 0, "")) {
        f = _parqit_fields(line, 9)
        if (f[1] != "cb") continue
        printf("  %-24s %-7s %10s %9s %10s  %-12s %-12s %s\n",
               _parqit_unhex(f[6]),
               (f[5] == "s" ? "string" : "numeric"),
               f[2], f[3], f[4],
               abbrev(_parqit_unhex(f[7]), 12), abbrev(_parqit_unhex(f[8]), 12),
               abbrev(_parqit_unhex(f[9]), 24))
    }
    fclose(fh)
    printf("\n")
}

void _parqit_print_distinct(string scalar resp)
{
    real scalar      fh, lastd
    string scalar    line
    string rowvector f

    fh = fopen(resp, "r")
    displayas("text")
    printf("\n  %-32s %12s %12s\n", "variable", "distinct", "obs")
    printf("  %s\n", 60 * "-")
    lastd = .
    while ((line = fget(fh)) != J(0, 0, "")) {
        f = _parqit_fields(line, 4)
        if (f[1] == "dst") {
            printf("  %-32s %12s %12s\n", _parqit_unhex(f[4]), f[2], f[3])
            lastd = strtoreal(f[2])
        }
        else if (f[1] == "dstj") {
            printf("  %-32s %12s %12s\n", "(joint)", f[2], f[3])
            lastd = strtoreal(f[2])
        }
    }
    fclose(fh)
    printf("\n")
    st_local("parqit_ndistinct", strofreal(lastd, "%21.0g"))
}

void _parqit_print_dupreport(string scalar resp)
{
    real scalar      fh, copies, groups, uniq, surplus, total
    string scalar    line
    string rowvector f

    fh = fopen(resp, "r")
    displayas("text")
    printf("\n  %10s %14s %12s\n", "copies", "observations", "surplus")
    printf("  %s\n", 40 * "-")
    uniq = 0
    surplus = 0
    total = 0
    while ((line = fget(fh)) != J(0, 0, "")) {
        f = _parqit_fields(line, 3)
        if (f[1] != "dupr") continue
        copies = strtoreal(f[2])
        groups = strtoreal(f[3])
        printf("  %10.0f %14.0f %12.0f\n", copies, copies * groups,
               (copies - 1) * groups)
        uniq = uniq + groups
        surplus = surplus + (copies - 1) * groups
        total = total + copies * groups
    }
    fclose(fh)
    printf("\n")
    st_local("parqit_dup_unique", strofreal(uniq, "%21.0g"))
    st_local("parqit_dup_surplus", strofreal(surplus, "%21.0g"))
    st_local("parqit_dup_total", strofreal(total, "%21.0g"))
}

void _parqit_print_duplist(string scalar resp)
{
    real scalar      fh, i
    string scalar    line
    string rowvector f, parts

    fh = fopen(resp, "r")
    displayas("text")
    while ((line = fget(fh)) != J(0, 0, "")) {
        f = _parqit_fields(line, 2)
        if (f[1] == "duph") {
            parts = ustrsplit(_parqit_unhex(f[2]), "\t")
            printf("\n  ")
            for (i = 1; i <= cols(parts); i++) printf("%-14s", abbrev(parts[i], 13))
            printf("\n  %s\n", (14 * cols(parts)) * "-")
        }
        else if (f[1] == "dupl") {
            parts = ustrsplit(_parqit_unhex(f[2]), "\t")
            printf("  ")
            for (i = 1; i <= cols(parts); i++) printf("%-14s", abbrev(parts[i], 13))
            printf("\n")
        }
    }
    fclose(fh)
    printf("\n")
}

void _parqit_print_misspatterns(string scalar resp)
{
    real scalar      fh, n
    string scalar    line
    string rowvector f

    fh = fopen(resp, "r")
    displayas("text")
    n = 0
    while ((line = fget(fh)) != J(0, 0, "")) {
        f = _parqit_fields(line, 3)
        if (f[1] == "mph") {
            printf("\n  pattern key (+ observed, . missing), variables in order:\n")
            printf("    %s\n", _parqit_unhex(f[2]))
            printf("\n  %-20s %12s\n", "pattern", "freq.")
            printf("  %s\n", 36 * "-")
        }
        else if (f[1] == "mpat") {
            printf("  %-20s %12s\n", _parqit_unhex(f[3]), f[2])
            n++
        }
    }
    fclose(fh)
    printf("\n")
    st_local("parqit_mp_r", strofreal(n, "%21.0g"))
}

void _parqit_print_tabstat(string scalar resp)
{
    real scalar      fh, i, ns
    string scalar    line, g, lastg
    string rowvector f, stats

    stats = tokens(st_local("_sq_stats"))
    ns = cols(stats)
    fh = fopen(resp, "r")
    displayas("text")
    printf("\n  %-20s %-14s", "variable", "group")
    for (i = 1; i <= ns; i++) printf(" %12s", stats[i])
    printf("\n  %s\n", (36 + 13 * ns) * "-")
    lastg = ""
    while ((line = fget(fh)) != J(0, 0, "")) {
        f = _parqit_fields(line, 2 + ns + 1)
        if (f[1] != "ts") continue
        g = _parqit_unhex(f[2 + ns + 1])
        printf("  %-20s %-14s", abbrev(_parqit_unhex(f[2 + ns]), 20),
               abbrev(g == "" ? "(all)" : g, 14))
        for (i = 1; i <= ns; i++) printf(" %12s", substr(f[1 + i], 1, 12))
        printf("\n")
    }
    fclose(fh)
    printf("\n")
}

void _parqit_print_corr(string scalar resp)
{
    real scalar      fh, i, j, k, n, minn, lastr, tstat, pval
    string scalar    line
    string rowvector f
    string colvector names
    real matrix      R, Nm

    fh = fopen(resp, "r")
    names = J(0, 1, "")
    while ((line = fget(fh)) != J(0, 0, "")) {
        f = _parqit_fields(line, 7)
        if (f[1] != "cor") continue
        i = strtoreal(f[2])
        if (i > rows(names)) names = names \ _parqit_unhex(f[6])
    }
    fclose(fh)
    k = rows(names)
    R = J(k, k, .)
    Nm = J(k, k, .)
    fh = fopen(resp, "r")
    while ((line = fget(fh)) != J(0, 0, "")) {
        f = _parqit_fields(line, 7)
        if (f[1] != "cor") continue
        i = strtoreal(f[2])
        j = strtoreal(f[3])
        R[i, j] = strtoreal(f[4])
        Nm[i, j] = strtoreal(f[5])
    }
    fclose(fh)

    displayas("text")
    printf("\n  %-14s", "")
    for (j = 1; j <= k; j++) printf(" %12s", abbrev(names[j], 12))
    printf("\n")
    minn = .
    lastr = .
    for (i = 1; i <= k; i++) {
        printf("  %-14s", abbrev(names[i], 12))
        for (j = 1; j <= i; j++) {
            printf(" %12.4f", R[i, j])
            if (i != j) lastr = R[i, j]
        }
        printf("\n")
        if (st_local("_sq_sig") != "") {
            printf("  %-14s", "")
            for (j = 1; j <= i; j++) {
                if (i == j | R[i, j] == . | Nm[i, j] < 3) printf(" %12s", "")
                else {
                    tstat = R[i, j] * sqrt((Nm[i, j] - 2) / (1 - R[i, j]^2))
                    pval = 2 * ttail(Nm[i, j] - 2, abs(tstat))
                    printf(" %12.4f", pval)
                }
            }
            printf("\n")
        }
        if (st_local("_sq_obs") != "") {
            printf("  %-14s", "")
            for (j = 1; j <= i; j++) printf(" %12.0f", Nm[i, j])
            printf("\n")
        }
        if (Nm[i, i] < minn) minn = Nm[i, i]
    }
    printf("\n")
    st_local("parqit_corr_n", strofreal(minn, "%21.0g"))
    st_local("parqit_corr_last", strofreal(lastr, "%21.0g"))
}

void _parqit_fill_hist(string scalar resp)
{
    real scalar      fh, b
    string scalar    line
    string rowvector f

    fh = fopen(resp, "r")
    while ((line = fget(fh)) != J(0, 0, "")) {
        f = _parqit_fields(line, 3)
        if (f[1] != "hb") continue
        b = strtoreal(f[2]) + 1
        if (b < 1 | b > st_nobs() | b == .) {
            displayas("error")
            printf("parqit histogram: internal bin record out of range: %s (frame obs %f)\n",
                   line, st_nobs())
            _error(3300, "parqit histogram: bin/frame mismatch")
        }
        st_store(b, "__freq", strtoreal(f[3]))
    }
    fclose(fh)
}
end
