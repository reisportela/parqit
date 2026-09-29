/* parqit — R data files (.rds, .rda/.RData) → Parquet (R-READ-1). */
#pragma once

#include <string>
#include <vector>

#include "stplugin.h"

namespace parqit_plugin {

/* rdata_convert <hex request path>: converts the data frame an R data file
 * holds into one Parquet file with parqit.* metadata, out of core; the
 * dataset in memory and the open views are never touched. */
ST_retcode cmd_rdata_convert(const std::vector<std::string> &args);

} // namespace parqit_plugin
