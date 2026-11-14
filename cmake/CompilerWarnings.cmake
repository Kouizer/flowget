# Applies the project warning policy to a target.
#
# Rationale: warnings are not style preferences here. A silently truncated size or a signed/
# unsigned mixup in a download engine corrupts files, so they fail the build.

function(flowget_set_warnings target)
    if(MSVC)
        target_compile_options(
            ${target}
            PRIVATE /W4
                    /permissive-
                    /utf-8
                    /Zc:__cplusplus
                    /Zc:preprocessor
                    /EHsc)
        if(FLOWGET_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(
            ${target}
            PRIVATE -Wall
                    -Wextra
                    -Wpedantic
                    -Wshadow
                    -Wconversion
                    -Wsign-conversion
                    -Wold-style-cast
                    -Wnon-virtual-dtor
                    -Wcast-align
                    -Wunused
                    -Woverloaded-virtual
                    -Wdouble-promotion
                    -Wformat=2)
        if(FLOWGET_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
