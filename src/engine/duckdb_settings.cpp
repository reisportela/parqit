#include "session.hpp"
#include "abi.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#ifdef PARQIT_DUCKDB_200
#include "duckdb/main/connection.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/execution/operator/helper/physical_set.hpp"
#include "duckdb/common/error_data.hpp"
#endif

namespace parqit {
namespace {
std::string lower_name(std::string name) {
    for (auto &c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return name;
}

bool valid_name(const std::string &name) {
    if (name.empty()) return false;
    for (size_t i = 0; i < name.size(); ++i) {
        const auto c = static_cast<unsigned char>(name[i]);
        if (!(std::isalpha(c) || c == '_' || (i && std::isdigit(c)))) return false;
    }
    return true;
}

std::vector<std::string> aliases_from_text(const std::string &text) {
    std::vector<std::string> out;
    std::string name;
    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '_') name += static_cast<char>(c);
        else if (!name.empty()) {
            out.push_back(lower_name(name));
            name.clear();
        }
    }
    if (!name.empty()) out.push_back(lower_name(name));
    return out;
}

std::vector<std::string> local_keys(const std::map<std::string, DuckDBSettingOverride> &settings) {
    std::vector<std::string> keys;
    for (const auto &entry : settings)
        if (entry.second.scope == "LOCAL" || entry.second.scope == "SESSION")
            keys.push_back(entry.first);
    return keys;
}

#ifdef PARQIT_DUCKDB_200
duckdb::ClientContext &native_context(duckdb_connection connection) {
    return *reinterpret_cast<duckdb::Connection *>(connection)->context;
}

std::string mutation_scope(duckdb_connection connection, const std::string &name,
                           const std::string &fallback) {
    const duckdb::Identifier key(name);
    const auto option = duckdb::DBConfig::GetOptionByName(key);
    duckdb::SetScope scope;
    if (option) scope = duckdb::PhysicalSet::GetSettingScope(*option, duckdb::SetScope::AUTOMATIC);
    else {
        duckdb::ExtensionOption extension;
        if (!duckdb::DBConfig::GetConfig(native_context(connection)).TryGetExtensionOption(key, extension)) return fallback;
        scope = extension.ResolveScope(key, duckdb::SetScope::AUTOMATIC);
    }
    return scope == duckdb::SetScope::GLOBAL ? "GLOBAL" : "LOCAL";
}

bool apply_native(duckdb_connection connection, const std::string &name,
                  const DuckDBSettingOverride &setting, std::string *error) {
    try {
        auto &context = native_context(connection);
        const duckdb::Identifier key(name);
        context.RunFunctionInTransaction([&]() {
            if (!setting.reset) {
                const auto value = setting.value ? duckdb::Value(*setting.value) : duckdb::Value();
                duckdb::PhysicalSet::SetVariable(context, key, duckdb::SetScope::AUTOMATIC, value);
                return;
            }
            // Same native callbacks/default handling as PhysicalReset, without
            // needing to parse SQL under a user-restricted expression depth.
            auto &config = duckdb::DBConfig::GetConfig(context);
            config.CheckLock(key);
            const auto option = duckdb::DBConfig::GetOptionByName(key);
            if (!option) {
                duckdb::ExtensionOption extension;
                if (!config.TryGetExtensionOption(key, extension)) throw std::runtime_error("native setting is no longer registered");
                const auto scope = extension.ResolveScope(key, duckdb::SetScope::AUTOMATIC);
                if (extension.set_function) extension.set_function(context, scope, extension.default_value);
                if (extension.info) extension.info->OnSet(context, scope, extension.default_value, true);
                if (scope == duckdb::SetScope::GLOBAL) config.ResetOption(extension);
                else duckdb::ClientConfig::GetConfig(context).user_settings.SetUserSetting(
                    extension.setting_index.GetIndex(), extension.default_value);
                return;
            }
            const auto scope = duckdb::PhysicalSet::GetSettingScope(*option, duckdb::SetScope::AUTOMATIC);
            if (option->default_value) {
                if (option->set_callback) {
                    duckdb::SettingCallbackInfo info(context, scope);
                    info.is_reset = true;
                    auto value = duckdb::Value(option->default_value).CastAs(
                        context, duckdb::DBConfig::ParseLogicalType(option->parameter_type));
                    option->set_callback(info, value);
                }
                const auto index = option->setting_idx.GetIndex();
                if (scope == duckdb::SetScope::SESSION)
                    duckdb::ClientConfig::GetConfig(context).user_settings.ClearSetting(index);
                else config.ResetGenericOption(index);
            } else if (scope == duckdb::SetScope::GLOBAL) {
                if (!option->reset_global) throw std::runtime_error("native setting cannot be reset globally");
                config.ResetOption(&duckdb::DatabaseInstance::GetDatabase(context), *option);
            } else {
                if (!option->reset_local) throw std::runtime_error("native setting cannot be reset locally");
                option->reset_local(context);
            }
        });
        return true;
    } catch (const std::exception &failure) {
        if (error) *error = duckdb::ErrorData(failure).Message();
        return false;
    }
}

bool read_native(duckdb_connection connection, const std::string &name,
                 std::optional<std::string> *value, std::string *error) {
    try {
        auto &context = native_context(connection);
        duckdb::Value effective;
        const duckdb::Identifier key(name);
        context.RunFunctionInTransaction([&]() {
            const auto option = duckdb::DBConfig::GetOptionByName(key);
            if (option && option->get_setting) effective = option->get_setting(context);
            else if (!context.TryGetCurrentSetting(key, effective)) {
                if (option && option->default_value)
                    effective = duckdb::Value(option->default_value).CastAs(
                        context, duckdb::DBConfig::ParseLogicalType(option->parameter_type));
                else throw std::runtime_error("could not read the native setting");
            }
        });
        if (effective.IsNull()) value->reset();
        else *value = effective.ToString();
        return true;
    } catch (const std::exception &failure) {
        if (error) *error = duckdb::ErrorData(failure).Message();
        return false;
    }
}
#else
bool apply_native(duckdb_connection, const std::string &, const DuckDBSettingOverride &, std::string *error) {
    if (error) *error = "native settings require the DuckDB 2.0 engine";
    return false;
}
bool read_native(duckdb_connection, const std::string &, std::optional<std::string> *, std::string *error) {
    if (error) *error = "native settings require the DuckDB 2.0 engine";
    return false;
}
#endif

bool inspect_text_layout(duckdb_connection con, bool *nonstandard, std::string *err) {
    *nonstandard = true; // A failed inspection must never select the int32 walker.
    duckdb_arrow_options options = nullptr;
    duckdb_connection_get_arrow_options(con, &options);
    auto type = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
    ArrowSchema schema{};
    struct Release {
        duckdb_arrow_options &options;
        duckdb_logical_type &type;
        ArrowSchema &schema;
        ~Release() {
            if (schema.release) schema.release(&schema);
            if (type) duckdb_destroy_logical_type(&type);
            if (options) duckdb_destroy_arrow_options(&options);
        }
    } release{options, type, schema};
    if (!options || !type) {
        if (err) *err = "could not inspect the native Arrow output profile";
        return false;
    }
    const char *name = "text";
    auto failure = duckdb_to_arrow_schema(options, &type, &name, 1, &schema);
    if (failure) {
        if (err) *err = duckdb_error_data_message(failure);
        duckdb_destroy_error_data(&failure);
        return false;
    }
    if (schema.n_children != 1 || !schema.children || !schema.children[0] || !schema.children[0]->format) {
        if (err) *err = "invalid native Arrow text schema";
        return false;
    }
    *nonstandard = std::string(schema.children[0]->format) != "u";
    return true;
}
} // namespace

bool Session::native_settings(std::vector<DuckDBSetting> *out, std::string *err) {
    out->clear();
#ifndef PARQIT_DUCKDB_200
    if (err) *err = "native settings require the DuckDB 2.0 engine";
    return false;
#else
    duckdb_result result{};
    if (!query("SELECT name,input_type,scope,description,CAST(aliases AS VARCHAR) "
               "FROM duckdb_settings(include_all := true) ORDER BY name", &result, err)) return false;
    ResultGuard guard{&result};
    std::map<std::string, std::string> canonical;
    for (idx_t row = 0; row < duckdb_row_count(&result); ++row) {
        DuckDBSetting item;
        item.name = text_cell(result, 0, row, "");
        item.canonical = lower_name(item.name);
        item.input_type = text_cell(result, 1, row, "");
        item.scope = text_cell(result, 2, row, "");
        item.description = text_cell(result, 3, row, "");
        item.aliases = aliases_from_text(text_cell(result, 4, row, ""));
        for (const auto &alias : item.aliases) canonical[alias] = item.canonical;
        out->push_back(std::move(item));
    }
    for (auto &item : *out) {
        const auto alias = canonical.find(item.canonical);
        if (alias != canonical.end()) item.canonical = alias->second;
        item.scope = mutation_scope(con_, item.canonical, item.scope);
        item.explicit_override = native_overrides_.count(item.canonical) != 0;
    }
    native_metadata_ = *out;
    return true;
#endif
}

bool Session::native_setting_info(const std::string &name, DuckDBSetting *out, std::string *err) {
    if (!valid_name(name)) {
        if (err) *err = "a native setting name must contain letters, digits or underscores";
        return false;
    }
    std::vector<DuckDBSetting> refreshed;
    if (native_metadata_.empty() && !native_settings(&refreshed, err)) return false;
    const auto key = lower_name(name);
    auto find = [&]() { return std::find_if(native_metadata_.begin(), native_metadata_.end(), [&](const DuckDBSetting &item) {
        return lower_name(item.name) == key;
    }); };
    auto found = find();
    if (found == native_metadata_.end()) {
        // Extensions can register new settings after the initial discovery.
        if (!native_settings(&refreshed, err)) return false;
        found = find();
    }
    if (found == native_metadata_.end()) {
        if (err) *err = "DuckDB setting '" + name + "' is not registered in this embedded engine";
        return false;
    }
    *out = *found;
    out->explicit_override = native_overrides_.count(out->canonical) != 0;
    return true;
}

bool Session::native_setting_get(const std::string &name, DuckDBSetting *out, std::string *err) {
    if (!native_setting_info(name, out, err)) return false;
    if (!ensure_open()) {
        if (err) *err = last_error_;
        return false;
    }
    return read_native(con_, out->canonical, &out->value, err);
}

void Session::rebuild_native_local_sql() {
    native_local_keys_ = local_keys(native_overrides_);
    native_streaming_override_ = native_overrides_.count("max_streaming_buffer_size") != 0;
}

void Session::release_native_setting(const std::string &canonical) {
    if (native_overrides_.empty()) return;
    const auto key = lower_name(canonical);
    native_overrides_.erase(key);
    if (key == "disabled_optimizers") {
        native_disabled_optimizers_.clear();
        native_optimizer_dirty_ = true;
    }
    if (native_overrides_.empty()) native_arrow_layout_ = false;
    rebuild_native_local_sql();
}

bool Session::native_setting_set(const std::string &name, const std::optional<std::string> &value,
                                bool reset, DuckDBSetting *out, std::string *err) {
    DuckDBSetting before;
    if (!native_setting_info(name, &before, err)) return false;
    if (!ensure_open()) {
        if (err) *err = last_error_;
        return false;
    }
    DuckDBSettingOverride setting{value, before.scope, reset};
    // Keep the original literal, not a rounded human-readable memory value.
    auto pending = native_overrides_;
    pending[before.canonical] = std::move(setting);
    auto pending_local = local_keys(pending);
    std::string pending_disabled;
    if (before.canonical == "disabled_optimizers") pending_disabled = reset ? "" : value.value_or("");
    // Allocate replay state before applying an irreversible native setting.
    if (!apply_native(con_, before.canonical, pending.at(before.canonical), err)) return false;
    native_overrides_.swap(pending);
    native_local_keys_.swap(pending_local);
    native_streaming_override_ = native_overrides_.count("max_streaming_buffer_size") != 0;
    if (before.canonical == "disabled_optimizers") {
        native_disabled_optimizers_.swap(pending_disabled);
        native_optimizer_dirty_ = true;
    }
    if (!inspect_text_layout(con_, &native_arrow_layout_, err)) {
        if (err) *err = "DuckDB accepted the setting, but its Arrow profile could not be inspected: " + *err;
        return false;
    }
    DuckDBSetting after;
    if (!native_setting_get(before.canonical, &after, err)) {
        if (err) *err = "DuckDB accepted the setting, but its effective value could not be read: " + *err;
        return false;
    }
    std::optional<std::string> thread_count;
    if (!read_native(con_, "threads", &thread_count, err) || !thread_count) return false;
    threads_ = std::strtoll(thread_count->c_str(), nullptr, 10);
    if (before.canonical == "disabled_optimizers") {
        native_disabled_optimizers_ = after.value.value_or("");
        native_optimizer_dirty_ = true;
    }
    after.explicit_override = true;
    *out = std::move(after);
    return true;
}

bool Session::replay_native_settings(duckdb_connection con, bool local_only, std::string *err) {
    if (local_only) {
        for (const auto &key : native_local_keys_)
            if (!apply_native(con, key, native_overrides_.at(key), err)) return false;
    } else {
        for (const auto &entry : native_overrides_)
            if (!apply_native(con, entry.first, entry.second, err)) return false;
        if (!native_overrides_.empty()) {
            native_optimizer_dirty_ = true;
            if (!inspect_text_layout(con, &native_arrow_layout_, err)) return false;
        }
    }
    return true;
}

} // namespace parqit
