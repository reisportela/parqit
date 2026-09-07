# GCC's Darwin driver exports runtime helpers when libgcc is embedded.
# Apple strip prunes both the symbol table and the dyld export trie.
execute_process(COMMAND "${STRIP}" -u -s "${EXPORTS}" -o "${OUTPUT}" "${INPUT}"
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Could not restrict the staged macOS plugin exports")
endif()
find_program(PARQIT_CODESIGN codesign)
if(NOT PARQIT_CODESIGN)
    message(FATAL_ERROR "codesign is required for the staged macOS plugin")
endif()
execute_process(COMMAND "${PARQIT_CODESIGN}" --force --sign - "${OUTPUT}"
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Could not ad-hoc sign the staged macOS plugin")
endif()
execute_process(COMMAND "${PARQIT_CODESIGN}" --verify --strict "${OUTPUT}"
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "The staged macOS plugin signature is invalid")
endif()
