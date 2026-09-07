# OpenMP is required; GNU's runtime coexists with Stata's private Intel runtime.
add_library(parqit_openmp INTERFACE)

if(UNIX)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        message(FATAL_ERROR "OpenMP plugin builds on Linux/macOS require GCC (macOS: gcc@14)")
    endif()
    if(APPLE AND PARQIT_TARGET_ARCH)
        execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -dumpmachine
            OUTPUT_VARIABLE _gcc_machine OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(PARQIT_TARGET_ARCH STREQUAL "arm64")
            set(_arch_pattern "^aarch64-")
        else()
            set(_arch_pattern "^x86_64-")
        endif()
        if(NOT _gcc_machine MATCHES "${_arch_pattern}")
            message(FATAL_ERROR "The selected GCC does not target ${PARQIT_TARGET_ARCH}: ${_gcc_machine}")
        endif()
    endif()
    include(FetchContent)
    include(ExternalProject)
    set(PARQIT_GCC_RUNTIME_VERSION "14.3.0")
    set(PARQIT_GCC_RUNTIME_ARCHIVE "" CACHE FILEPATH "Optional GCC source archive for PIC libgomp")
    set(_gcc_url "https://ftp.gnu.org/gnu/gcc/gcc-${PARQIT_GCC_RUNTIME_VERSION}/gcc-${PARQIT_GCC_RUNTIME_VERSION}.tar.xz")
    if(PARQIT_GCC_RUNTIME_ARCHIVE)
        set(_gcc_url "${PARQIT_GCC_RUNTIME_ARCHIVE}")
    endif()
    FetchContent_Declare(parqit_gcc
        URL "${_gcc_url}"
        URL_HASH SHA256=e0dc77297625631ac8e50fa92fffefe899a4eb702592da5c32ef04e2293aca3a)
    FetchContent_GetProperties(parqit_gcc)
    if(NOT parqit_gcc_POPULATED)
        FetchContent_Populate(parqit_gcc)
    endif()
    find_program(PARQIT_GNU_MAKE NAMES gmake make)
    if(NOT PARQIT_GNU_MAKE)
        message(FATAL_ERROR "GNU make is required to build the OpenMP runtime")
    endif()
    set(_gomp_build "${CMAKE_BINARY_DIR}/_deps/parqit-libgomp-build")
    file(MAKE_DIRECTORY "${_gomp_build}")
    set(_gomp_cflags "-O2 -fPIC")
    if(APPLE AND CMAKE_OSX_DEPLOYMENT_TARGET)
        string(APPEND _gomp_cflags " -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
    endif()
    ExternalProject_Add(parqit_libgomp_build
        SOURCE_DIR "${parqit_gcc_SOURCE_DIR}/libgomp"
        BINARY_DIR "${_gomp_build}"
        DOWNLOAD_COMMAND ""
        UPDATE_COMMAND ""
        CONFIGURE_COMMAND "${CMAKE_COMMAND}" -E env
            "CC=${CMAKE_C_COMPILER}" "CFLAGS=${_gomp_cflags}" "GFORTRAN=no"
            <SOURCE_DIR>/configure
            --disable-shared --enable-static --with-pic
            --disable-multilib --disable-werror --disable-tls
            --disable-libgomp-plugin --enable-offload-targets=
        BUILD_COMMAND "${PARQIT_GNU_MAKE}" -j 4 libgomp.la
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS "${_gomp_build}/.libs/libgomp.a" "${_gomp_build}/omp.h")
    add_library(parqit_libgomp STATIC IMPORTED GLOBAL)
    set_target_properties(parqit_libgomp PROPERTIES
        IMPORTED_LOCATION "${_gomp_build}/.libs/libgomp.a"
        INTERFACE_INCLUDE_DIRECTORIES "${_gomp_build}")
    add_dependencies(parqit_libgomp parqit_libgomp_build)
    target_compile_options(parqit_openmp INTERFACE "$<$<COMPILE_LANGUAGE:CXX>:-fopenmp>")
    target_link_libraries(parqit_openmp INTERFACE parqit_libgomp Threads::Threads ${CMAKE_DL_LIBS})
    message(STATUS "parqit OpenMP: required; PIC GNU libgomp ${PARQIT_GCC_RUNTIME_VERSION}")
else()
    find_package(OpenMP REQUIRED COMPONENTS CXX)
    target_link_libraries(parqit_openmp INTERFACE OpenMP::OpenMP_CXX)
    message(STATUS "parqit OpenMP: required; ${OpenMP_CXX_FLAGS}; ${OpenMP_CXX_LIBRARIES}")
endif()

if(MSVC)
    # MSVC's OpenMP runtime is a DLL even with /MT. The delay-load hook resolves
    # the redistributable installed beside the plugin, including Unicode paths.
    set(PARQIT_WINDOWS_OPENMP_DLL "" CACHE FILEPATH "Redistributable x64 vcomp140.dll")
    if(NOT PARQIT_WINDOWS_OPENMP_DLL)
        string(REGEX REPLACE "/VC/Tools/MSVC/.*$" "/VC/Redist/MSVC"
            _vc_redist "${CMAKE_CXX_COMPILER}")
        file(GLOB _vcomp_candidates "${_vc_redist}/*/x64/Microsoft.VC*.OpenMP/vcomp140.dll")
        list(SORT _vcomp_candidates ORDER DESCENDING)
        if(_vcomp_candidates)
            list(GET _vcomp_candidates 0 PARQIT_WINDOWS_OPENMP_DLL)
        endif()
    endif()
    if(NOT EXISTS "${PARQIT_WINDOWS_OPENMP_DLL}")
        message(FATAL_ERROR "Set PARQIT_WINDOWS_OPENMP_DLL to the x64 redistributable vcomp140.dll")
    endif()
    string(REGEX REPLACE "/VC/Tools/MSVC/.*$" "/VC/Redist/MSVC"
        _vc_redist "${CMAKE_CXX_COMPILER}")
    file(GLOB _vcomp_debug_candidates
        "${_vc_redist}/*/debug_nonredist/x64/Microsoft.VC*.DebugOpenMP/vcomp140d.dll")
    list(SORT _vcomp_debug_candidates ORDER DESCENDING)
    if(_vcomp_debug_candidates)
        list(GET _vcomp_debug_candidates 0 _parqit_vcomp_debug)
    else()
        set(_parqit_vcomp_debug "${CMAKE_BINARY_DIR}/missing-vcomp140d.dll")
    endif()
    set(PARQIT_OPENMP_DLL_SOURCE
        "$<IF:$<CONFIG:Debug>,${_parqit_vcomp_debug},${PARQIT_WINDOWS_OPENMP_DLL}>")
endif()
