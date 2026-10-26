/* An environment variable set for one test's scope (the engine's test hooks
 * read the environment), on POSIX and Windows. */
#pragma once

#include <cstdlib>
#include <string>

namespace parqit_test {

inline void set_env(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

struct ScopedEnv {
    const char *name;
    ScopedEnv(const char *n, const std::string &v) : name(n) { set_env(n, v.empty() ? nullptr : v.c_str()); }
    ~ScopedEnv() { set_env(name, nullptr); }
    ScopedEnv(const ScopedEnv &) = delete;
    ScopedEnv &operator=(const ScopedEnv &) = delete;
};

} // namespace parqit_test
