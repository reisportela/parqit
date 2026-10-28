#pragma once

#include <optional>
#include <string>
#include <vector>

namespace parqit {

struct DuckDBSetting {
    std::string name;
    std::string canonical;
    std::string input_type;
    std::string scope;
    std::string description;
    std::vector<std::string> aliases;
    std::optional<std::string> value;
    bool explicit_override = false;
};

struct DuckDBSettingOverride {
    std::optional<std::string> value;
    std::string scope;
    bool reset = false;
};

} // namespace parqit
