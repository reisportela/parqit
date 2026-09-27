/* parqit — SPSS system files (.sav/.zsav) → Parquet (SPSS-READ-1). */
#pragma once

#include <string>
#include <vector>

#include "stplugin.h"

namespace parqit_plugin {

/* spss_convert <hex request path>: converts one SPSS file into one Parquet
 * file with parqit.* metadata, out of core; the dataset in memory and the
 * open views are never touched. */
ST_retcode cmd_spss_convert(const std::vector<std::string> &args);

} // namespace parqit_plugin
