#include "plugin/openmp_runtime.hpp"

#ifndef _OPENMP
#error "Every parqit plugin must be compiled with OpenMP enabled"
#endif

#include <omp.h>
#include <stdexcept>

#ifdef _MSC_VER
#include <windows.h>
#include <delayimp.h>
#include <cstring>
#include <string>
#include <vector>

namespace {
FARPROC WINAPI load_packaged_openmp(unsigned notification, PDelayLoadInfo info) {
    if (notification != dliNotePreLoadLibrary) return nullptr;
#ifdef _DEBUG
    constexpr const char *runtime_name = "vcomp140d.dll";
#else
    constexpr const char *runtime_name = "vcomp140.dll";
#endif
    if (_stricmp(info->szDll, runtime_name) != 0) return nullptr;
    HMODULE plugin = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&load_packaged_openmp), &plugin))
        throw std::runtime_error("cannot locate the parqit plugin for OpenMP");
    std::vector<wchar_t> buffer(32768);
    DWORD length = GetModuleFileNameW(plugin, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size())
        throw std::runtime_error("cannot resolve the parqit OpenMP runtime path");
    std::wstring path(buffer.data(), length);
    const auto separator = path.find_last_of(L"/\\");
    if (separator == std::wstring::npos)
        throw std::runtime_error("parqit plugin path has no parent directory");
    path.resize(separator + 1);
    path += L"parqit_vcomp140.dll";
    HMODULE runtime = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!runtime)
        throw std::runtime_error("cannot load parqit_vcomp140.dll; reinstall the complete parqit package");
    return reinterpret_cast<FARPROC>(runtime);
}
} // namespace

extern "C" const PfnDliHook __pfnDliNotifyHook2 = load_packaged_openmp;
#endif

namespace parqit_plugin {

int openmp_version() { return _OPENMP; }
int openmp_max_threads() { return omp_get_max_threads(); }

OpenMPProbe openmp_probe() {
    int threads = 0, checksum = 0;
    // No Stata API is called by a worker. Resource limits may reduce the team.
#pragma omp parallel num_threads(2) reduction(+:threads,checksum)
    {
        ++threads;
        checksum += omp_get_thread_num() + 1;
    }
    if (threads < 1 || threads > 2 || checksum != threads * (threads + 1) / 2)
        throw std::runtime_error("OpenMP runtime verification failed");
    return {threads, checksum};
}

} // namespace parqit_plugin
