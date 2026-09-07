// Load the exact packaged plugin and exercise its public engine diagnostics.
#include "stplugin.h"
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {
std::map<std::string, std::string> locals;
int save_macro(char *name, char *value) {
    locals[name] = value;
    return 0;
}
int report_error(char *message) {
    std::cerr << message;
    return 0;
}
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3 || (argc == 3 && std::string(argv[2]) != "--distribution")) {
        std::cerr << "usage: parqit_runtime_probe /absolute/path/parqit.plugin [--distribution]\n";
        return 2;
    }
#ifdef _WIN32
    HMODULE library = LoadLibraryA(argv[1]);
    auto symbol = [library](const char *name) {
        return library ? GetProcAddress(library, name) : nullptr;
    };
#else
    void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    auto symbol = [library](const char *name) {
        return library ? dlsym(library, name) : nullptr;
    };
#endif
    if (!library) {
        std::cerr << "Plugin load failed";
#ifndef _WIN32
        std::cerr << ": " << dlerror();
#endif
        std::cerr << '\n';
        return 1;
    }
    const auto initialize = reinterpret_cast<ST_retcode (*)(ST_plugin *)>(symbol("pginit"));
    const auto call = reinterpret_cast<ST_retcode (*)(int, char **)>(symbol("stata_call"));
    if (!initialize || !call) return 1;
#ifdef __APPLE__
    if (argc == 3) {
        for (const char *name : {"__emutls_get_address", "__emutls_register_common",
                                 "__gcc_nested_func_ptr_created", "__gcc_nested_func_ptr_deleted"}) {
            if (symbol(name)) {
                std::cerr << "Private GCC helper remains dynamically exported: " << name << '\n';
                return 1;
            }
        }
    }
#endif
    ST_plugin stata{};
    stata.macresave = save_macro;
    stata.spouterr = report_error;
    if (initialize(&stata) != SD_PLUGINVER) return 1;
    char version[] = "version";
    char *version_args[] = {version};
    if (call(1, version_args) != 0 || locals["_parqit_openmp"] != "0" ||
        locals["_parqit_openmp_version"] != "0" ||
        locals["_parqit_openmp_max_threads"] != "0" ||
        locals["_parqit_parallel_backend"] != "duckdb") {
        std::cerr << "Plugin must use DuckDB's scheduler without OpenMP\n";
        return 1;
    }
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto temporary = std::filesystem::temp_directory_path() /
                           ("parqit-plugin-probe-" + std::to_string(nonce));
    if (!std::filesystem::create_directory(temporary)) return 1;
    std::string hex;
    constexpr char digits[] = "0123456789abcdef";
    for (unsigned char ch : temporary.string()) {
        hex.push_back(digits[ch >> 4]);
        hex.push_back(digits[ch & 15]);
    }
    char selftest[] = "selftest";
    char *selftest_args[] = {selftest, hex.data()};
    const bool engine_ok = call(2, selftest_args) == 0 && locals["_parqit_selftest"] == "ok" &&
                           locals["_parqit_openmp_threads"] == "0";
    std::error_code cleanup_error;
    std::filesystem::remove(temporary, cleanup_error);
    if (!engine_ok || cleanup_error) return 1;
    std::cout << "PLUGIN_RUNTIME_PASS version=" << locals["_parqit_plugin_version"]
              << " parallel_backend=duckdb openmp=0 engine=PASS\n";
    // Keep the plugin resident until process exit, as Stata normally does.
    return 0;
}
