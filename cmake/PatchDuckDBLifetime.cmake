# Preserve deterministic transaction defaults and safe process shutdown.
function(parqit_patch_duckdb_lifetime relative original_hash patched_hash before after)
    set(source "${duckdb_SOURCE_DIR}/${relative}")
    file(SHA256 "${source}" actual_hash)
    if(actual_hash STREQUAL patched_hash)
        return()
    endif()
    if(NOT actual_hash STREQUAL original_hash)
        message(FATAL_ERROR "Unexpected DuckDB lifetime source: ${relative}; preserving local changes")
    endif()
    file(READ "${source}" contents)
    string(REPLACE "${before}" "${after}" revised "${contents}")
    string(SHA256 revised_hash "${revised}")
    if(NOT revised_hash STREQUAL patched_hash)
        message(FATAL_ERROR "DuckDB lifetime patch did not match: ${relative}")
    endif()
    if(EXISTS "${source}.parqit-lifetime-patch")
        message(FATAL_ERROR "Existing DuckDB lifetime staging file preserved: ${relative}")
    endif()
    file(WRITE "${source}.parqit-lifetime-patch" "${revised}")
    file(RENAME "${source}.parqit-lifetime-patch" "${source}")
    message(STATUS "Applied parqit lifetime correction: ${relative}")
endfunction()

# Cached TLS vectors contain block IDs, not owning allocations. Marking the
# token dead and unmapping the pool suffices; accessing TLS here can resurrect
# an already destroyed cache, or discard another live allocator's cached IDs.
parqit_patch_duckdb_lifetime(
    src/storage/block_allocator.cpp
    e8db7fecc19cb0a0a180d4ed066b372edeb8a1a926d4217f4d46160162ecbceb
    70fb05c3c78bef69ae4f14595a7ad90bcbfdd70bee8c7bf1a6fe0ca3eec3ae05
    "BlockAllocator::~BlockAllocator() {\n\talive_token->store(false);\n\tGetBlockAllocatorThreadLocalState(*this).Clear();"
    "BlockAllocator::~BlockAllocator() {\n\talive_token->store(false);")

parqit_patch_duckdb_lifetime(
    src/transaction/transaction_context.cpp
    d2e30134b5be1bba1a67864c2b8ef294e5934bbac40dbee03d848d31ea277e1e
    14216595611a45db38b91aa548fde09eef433ff9dde69af529e1ef178414e684
    ": context(context), auto_commit(true), current_transaction(nullptr) {"
    ": context(context), auto_commit(true), invalidation_policy(TransactionInvalidationPolicy::STANDARD_POLICY),\n      auto_rollback(false), current_transaction(nullptr) {")
