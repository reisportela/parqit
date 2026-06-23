/* parqit — M2 lazy-view subcommands. */
#pragma once

#include <string>
#include <vector>

#include "stplugin.h"

namespace parqit_plugin {

ST_retcode cmd_view_open(const std::vector<std::string> &args);
ST_retcode cmd_view_op(const std::vector<std::string> &args);
ST_retcode cmd_view_twotable(const std::vector<std::string> &args);
ST_retcode cmd_view_reshape(const std::vector<std::string> &args);
ST_retcode cmd_view_sql(const std::vector<std::string> &args);
ST_retcode cmd_view_query(const std::vector<std::string> &args);
ST_retcode cmd_view_stats(const std::vector<std::string> &args);
ST_retcode cmd_path(const std::vector<std::string> &args);
ST_retcode cmd_view_info(const std::vector<std::string> &args); /* show/explain/describe/count */
ST_retcode cmd_view_collect_prepare(const std::vector<std::string> &args);
ST_retcode cmd_view_save(const std::vector<std::string> &args);
ST_retcode cmd_view_close(const std::vector<std::string> &args);
ST_retcode cmd_view_switch(const std::vector<std::string> &args);
ST_retcode cmd_view_list(const std::vector<std::string> &args);
/* name of the current view ("" when none is live) */
std::string view_current_name();
ST_retcode cmd_set(const std::vector<std::string> &args);

bool view_is_live();

} // namespace parqit_plugin
