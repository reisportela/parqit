#pragma once

#include <cstdlib>
#include <string>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace parqit_test {

inline long process_id() {
#ifdef _WIN32
    return static_cast<long>(_getpid());
#else
    return static_cast<long>(getpid());
#endif
}

/* Unit binaries may run concurrently under CTest, local agents, or a developer's
 * explicit stress run. Namespace every writable scratch path by PID so one
 * process can never truncate or overwrite another process's oracle. */
inline std::string tmp_path(const std::string &name) {
#ifdef _WIN32
    const char *base = std::getenv("TEMP");
    std::string dir = base ? base : ".";
    const char sep = '\\';
#else
    const char *base = std::getenv("TMPDIR");
    std::string dir = base ? base : "/tmp";
    const char sep = '/';
#endif
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += sep;
    return dir + name + "." + std::to_string(process_id());
}

} // namespace parqit_test
