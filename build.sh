#!/usr/bin/env bash
# Build and test the Linux plugin using the project's CMake presets.
set -euo pipefail

die() { printf 'parqit build: %s\n' "$*" >&2; exit 2; }

if [[ ${1:-} == --help || ${1:-} == -h ]]; then
    cat <<'HELP'
Usage: bash build.sh [parallel_jobs]

Linux build using GCC >= 10 and CMake >= 3.21. Defaults to two build jobs.
The first configure downloads DuckDB; Make (or the selected CMake build tool)
must be installed. Activate any required Red Hat compiler toolset first.

Optional environment settings:
  CC, CXX                  GCC compiler executables
  PARQIT_CMAKE              CMake executable (otherwise tries cmake and cmake3)
  PARQIT_DUCKDB_ARCHIVE     Local, pinned duckdb-1.5.3.tar.gz for offline builds

Output: ado/plus/p/parqit.plugin, alongside the matching ado/help files.
HELP
    exit 0
fi

[[ $# -le 1 ]] || die 'usage: bash build.sh [parallel_jobs]'
jobs=${1:-2}
[[ $jobs =~ ^[1-9][0-9]*$ ]] || die 'parallel_jobs must be a positive integer'
[[ $(uname -s) == Linux ]] || die 'this wrapper is for Linux; see BUILDING.md for other platforms'
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
[[ -f CMakePresets.json && -f CMakeLists.txt ]] || die 'place build.sh in the extracted parqit source folder'

cmake_supported() {
    local version
    command -v "$1" >/dev/null 2>&1 || return 1
    version=$("$1" --version) || return 1
    [[ $version =~ cmake\ version\ ([0-9]+)\.([0-9]+) ]] || return 1
    (( BASH_REMATCH[1] > 3 || (BASH_REMATCH[1] == 3 && BASH_REMATCH[2] >= 21) ))
}

cmake_bin=${PARQIT_CMAKE:-cmake}
if ! cmake_supported "$cmake_bin"; then
    if [[ -z ${PARQIT_CMAKE:-} ]] && cmake_supported cmake3; then
        cmake_bin=cmake3
    else
        die 'CMake >= 3.21 is required; activate it or set PARQIT_CMAKE to its executable'
    fi
fi

export CC=${CC:-gcc} CXX=${CXX:-g++}
for compiler in "$CC" "$CXX"; do
    command -v "$compiler" >/dev/null 2>&1 || die "compiler not found: $compiler; activate GCC >= 10"
    macros=$("$compiler" -dM -E -x c++ /dev/null) || die "cannot inspect compiler: $compiler"
    [[ $macros != *'#define __clang__ '* ]] || die 'this Linux wrapper expects GCC >= 10'
    major=$(printf '%s\n' "$macros" | awk '$2 == "__GNUC__" {print $3}')
    [[ $major =~ ^[0-9]+$ ]] && (( major >= 10 )) || \
        die "$compiler is too old; activate GCC >= 10 (gcc-toolset/devtoolset on Red Hat)"
done

# CMake ignores CC/CXX for an existing tree. Preserve equivalent cached aliases
# (cc/gcc, c++/g++); forcing a different path resets the cache and loses presets.
if [[ -f build/linux/CMakeCache.txt ]]; then
    for compiler_setting in "C=$CC" "CXX=$CXX"; do
        build_lang=${compiler_setting%%=*}
        requested_compiler=$(command -v "${compiler_setting#*=}")
        cached_compiler=$(awk -v key="CMAKE_${build_lang}_COMPILER:" \
            'index($0,key)==1 {sub(/^[^=]*=/, ""); print; exit}' build/linux/CMakeCache.txt)
        [[ -n $cached_compiler && $cached_compiler -ef $requested_compiler ]] || \
            die "cached $build_lang compiler differs; use the same CC/CXX or a fresh source tree"
    done
fi
configure=(--preset linux)
if [[ -n ${PARQIT_DUCKDB_ARCHIVE:-} ]]; then
    configure+=("-DPARQIT_DUCKDB_ARCHIVE=$PARQIT_DUCKDB_ARCHIVE")
fi
"$cmake_bin" "${configure[@]}"
"$cmake_bin" --build --preset linux --parallel "$jobs"
CTEST_OUTPUT_ON_FAILURE=1 "$cmake_bin" --build --preset linux --target test

printf '\nBuild and C++ checks passed. In a fresh Stata session, run:\n'
printf 'adopath ++ "%s/ado/plus/p"\n' "$PWD"
printf 'parqit version\nparqit selftest\n'
