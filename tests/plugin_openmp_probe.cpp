// Run the exact plugin's OpenMP code without linking this verifier to OpenMP.
#include "stplugin.h"
#include <cstdlib>
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
    if (argc != 2) {
        std::cerr << "usage: parqit_openmp_probe /absolute/path/parqit.plugin\n";
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
        std::cerr << "OpenMP plugin load failed";
#ifndef _WIN32
        std::cerr << ": " << dlerror();
#endif
        std::cerr << '\n';
        return 1;
    }
    const auto initialize = reinterpret_cast<ST_retcode (*)(ST_plugin *)>(symbol("pginit"));
    const auto call = reinterpret_cast<ST_retcode (*)(int, char **)>(symbol("stata_call"));
    if (!initialize || !call) return 1;
    ST_plugin stata{};
    stata.macresave = save_macro;
    stata.spouterr = report_error;
    if (initialize(&stata) != SD_PLUGINVER) return 1;
    char version[] = "version";
    char *version_args[] = {version};
    if (call(1, version_args) != 0 || locals["_parqit_openmp"] != "1" ||
        std::atoi(locals["_parqit_openmp_version"].c_str()) < 200203)
        return 1;
    char probe[] = "openmp_probe";
    char *probe_args[] = {probe};
    if (call(1, probe_args) != 0 || locals["_parqit_openmp_threads"] != "2" ||
        locals["_parqit_openmp_checksum"] != "3") {
        std::cerr << "OpenMP must execute two workers: threads="
                  << locals["_parqit_openmp_threads"] << " checksum="
                  << locals["_parqit_openmp_checksum"] << '\n';
        return 1;
    }
    std::cout << "PLUGIN_OPENMP_PASS version=" << locals["_parqit_plugin_version"]
              << " standard=" << locals["_parqit_openmp_version"]
              << " threads=2 checksum=3\n";
    // Keep the runtime resident until process exit, as Stata normally does.
    return 0;
}
