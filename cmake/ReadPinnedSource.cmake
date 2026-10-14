# Hash pinned source text consistently after native Windows text writes.
# Accept only uniform LF or CRLF encodings; unknown edits and mixed endings
# remain refusals. Callers still compare the canonical hash to fixed pins.
include_guard(GLOBAL)
function(parqit_read_pinned_source path content_variable hash_variable)
    file(READ "${path}" contents)
    string(REPLACE "\r\n" "\n" canonical "${contents}")
    string(SHA256 canonical_hash "${canonical}")
    file(SHA256 "${path}" physical_hash)
    string(REPLACE "\n" "\r\n" windows "${canonical}")
    string(SHA256 windows_hash "${windows}")
    if(NOT physical_hash STREQUAL canonical_hash AND NOT physical_hash STREQUAL windows_hash)
        message(FATAL_ERROR "Unexpected DuckDB source line endings; preserving local changes: ${path}")
    endif()
    set(${content_variable} "${canonical}" PARENT_SCOPE)
    set(${hash_variable} "${canonical_hash}" PARENT_SCOPE)
endfunction()

function(parqit_verify_pinned_write path expected_hash)
    parqit_read_pinned_source("${path}" written written_hash)
    if(NOT written_hash STREQUAL expected_hash)
        message(FATAL_ERROR "DuckDB staging content failed verification; original source preserved: ${path}")
    endif()
endfunction()
