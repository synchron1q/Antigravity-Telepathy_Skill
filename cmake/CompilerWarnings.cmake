option(AGMESH_PEDANTIC_CONVERSIONS "Enable extra -Wconversion/-Wsign-conversion/-Wshadow warnings" OFF)

function(agmesh_apply_compiler_warnings target_name)
    if(MSVC)
        target_compile_definitions(${target_name} PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(${target_name} PRIVATE
            /W4
            /permissive-
            /utf-8
            /Zc:__cplusplus
            /wd4100
            $<$<BOOL:${AGMESH_WARNINGS_AS_ERRORS}>:/WX>
        )
    else()
        target_compile_options(${target_name} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wnon-virtual-dtor
            $<$<BOOL:${AGMESH_PEDANTIC_CONVERSIONS}>:-Wconversion>
            $<$<BOOL:${AGMESH_PEDANTIC_CONVERSIONS}>:-Wsign-conversion>
            $<$<BOOL:${AGMESH_PEDANTIC_CONVERSIONS}>:-Wshadow>
            $<$<BOOL:${AGMESH_PEDANTIC_CONVERSIONS}>:-Wold-style-cast>
            $<$<BOOL:${AGMESH_WARNINGS_AS_ERRORS}>:-Werror>
        )
    endif()
endfunction()
