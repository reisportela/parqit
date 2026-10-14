# Reproducible identity of the plugin sources and pinned dependency patches.
# Binary checksums remain platform/build-specific and belong to the release manifest.
set(PARQIT_NUMERIC_CONTRACT "4")
file(GLOB_RECURSE _parqit_identity_inputs CONFIGURE_DEPENDS
    RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
    "src/engine/*.cpp" "src/engine/*.hpp"
    "src/plugin/*.cpp" "src/plugin/*.hpp" "src/plugin/*.map" "src/plugin/*.def" "src/plugin/*.exports"
    "cmake/*.cmake" "cmake/*.in"
    "vendor/stata/*.c" "vendor/stata/*.h"
    "vendor/arrow/*.h" "vendor/json/*.hpp")
list(APPEND _parqit_identity_inputs "CMakeLists.txt" "CMakePresets.json")
list(SORT _parqit_identity_inputs)
set(_parqit_identity "parqit=${PROJECT_VERSION}\nduckdb=${PARQIT_DUCKDB_VERSION}:${PARQIT_DUCKDB_SHA256}\ncontract=${PARQIT_NUMERIC_CONTRACT}\n")
foreach(_file IN LISTS _parqit_identity_inputs)
    # A Windows checkout may use CRLF. Its line endings must not create a
    # different identity for otherwise identical release sources.
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${_file}" _text)
    string(REPLACE "\r\n" "\n" _text "${_text}")
    string(SHA256 _hash "${_text}")
    string(APPEND _parqit_identity "${_file}:${_hash}\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/${_file}")
endforeach()
string(SHA256 PARQIT_BUILD_ID "${_parqit_identity}")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_identity.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/parqit_build_identity.hpp" @ONLY)
