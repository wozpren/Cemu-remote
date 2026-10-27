# cl can emit UTF-8 diagnostics while CMake decodes its compiler-id sample with
# the Windows ANSI code page. Probe the actual output so Ninja can remove the
# include prefix and retain correct incremental header dependencies.
if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja")
    get_property(_cgp_prefix GLOBAL PROPERTY CGP_MSVC_INCLUDE_PREFIX)
    if(NOT _cgp_prefix)
        set(_cgp_source "${CMAKE_BINARY_DIR}/CMakeFiles/GamePadMsvcIncludes.c")
        file(WRITE "${_cgp_source}" "#include <stddef.h>\n")
        execute_process(COMMAND "${CMAKE_C_COMPILER}" /nologo /showIncludes /c "${_cgp_source}"
            "/Fo${CMAKE_BINARY_DIR}/CMakeFiles/GamePadMsvcIncludes.obj"
            RESULT_VARIABLE _cgp_status OUTPUT_VARIABLE _cgp_output ERROR_VARIABLE _cgp_error ENCODING UTF-8)
        if(NOT _cgp_status EQUAL 0)
            message(FATAL_ERROR "MSVC include-prefix probe failed: ${_cgp_error}")
        endif()
        string(REGEX MATCH "(^|\n)([^\r\n]*: +)[A-Za-z]:[/\\\\][^\r\n]*stddef\\.h" _cgp_match "${_cgp_output}\n${_cgp_error}")
        if(NOT _cgp_match)
            message(FATAL_ERROR "Could not identify MSVC's include prefix: ${_cgp_output}\n${_cgp_error}")
        endif()
        set(_cgp_prefix "${CMAKE_MATCH_2}")
        set_property(GLOBAL PROPERTY CGP_MSVC_INCLUDE_PREFIX "${_cgp_prefix}")
    endif()
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "${_cgp_prefix}")
endif()
