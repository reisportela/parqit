/* parqit — M2 lazy-view subcommands. */
#pragma once

#include <string>
#include <vector>

#include "stplugin.h"

namespace parqit_plugin {

/* WINDOW-NEST-1: the eager subcommands (use, describe, save data, the bridge,
 * the text/SPSS/R converters) plan nothing of the current view, so they take
 * the full optimizer set whatever view is current — a no-op unless the state
 * changed. The dispatcher calls it before those subcommands. */
void eager_optimizers();
/* WINDOW-NEST-1: forgets the using view a previous two-table call took into
 * the optimizer sync; the dispatcher calls it first on every call */
void begin_call();

ST_retcode cmd_view_open(const std::vector<std::string> &args);
/* VIEW-COPY-1: `parqit use [varlist] using view:<source>, name(<target>)` */
ST_retcode cmd_view_copy(const std::vector<std::string> &args);
ST_retcode cmd_view_op(const std::vector<std::string> &args);
ST_retcode cmd_view_twotable(const std::vector<std::string> &args);
ST_retcode cmd_view_reshape(const std::vector<std::string> &args);
ST_retcode cmd_view_pivot(const std::vector<std::string> &args);
ST_retcode cmd_view_sql(const std::vector<std::string> &args);
ST_retcode cmd_view_query(const std::vector<std::string> &args);
ST_retcode cmd_view_stats(const std::vector<std::string> &args);
ST_retcode cmd_path(const std::vector<std::string> &args);
ST_retcode cmd_view_info(const std::vector<std::string> &args); /* show/explain/describe/count */
ST_retcode cmd_view_collect_prepare(const std::vector<std::string> &args);
ST_retcode cmd_view_save(const std::vector<std::string> &args);
ST_retcode cmd_view_close(const std::vector<std::string> &args);
ST_retcode cmd_view_switch(const std::vector<std::string> &args);
ST_retcode cmd_view_commit(const std::vector<std::string> &args);
ST_retcode cmd_view_list(const std::vector<std::string> &args);
/* Internal persistent-bridge lifecycle.  bridge_new reserves a process- and
 * operation-unique package-owned path; bridge_discard can erase only a path
 * created by bridge_new in this plugin session. */
ST_retcode cmd_bridge_new(const std::vector<std::string> &args);
ST_retcode cmd_bridge_discard(const std::vector<std::string> &args);
/* CSV-ENC-1: decode delimited text that is not UTF-8 into a UTF-8 bridge */
ST_retcode cmd_text_prepare(const std::vector<std::string> &args);
/* USE-IF-1: the WHERE clause of an eager `parqit use ... if exp`: the Stata
 * expression translated over the columns of `scan_sql` (a read_parquet/CSV
 * scan), each bare reference replaced by its boundary cast — a DATE as a day
 * count, a BOOLEAN as 0/1, a FLOAT/DOUBLE guarded like the lazy view — so the
 * condition sees the same values a view verb would, while the fetch keeps
 * reading the raw columns (the .a-.z companions included). Announces the
 * missing-value rule like a view verb (STATAMISS-WARN-1). Returns false and
 * *err on a translation error; _n/_N are refused (no row order to count). */
bool eager_filter_sql(const std::string &scan_sql, const std::string &expr,
                      std::string *where_sql, std::string *err);
/* name of the current view ("" when none is live) */
std::string view_current_name();
ST_retcode cmd_set(const std::vector<std::string> &args);

bool view_is_live();

} // namespace parqit_plugin
