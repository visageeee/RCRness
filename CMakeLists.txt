cmake_minimum_required(VERSION 3.20)
project(RCRness C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(NESRECOMP_ROOT "${CMAKE_SOURCE_DIR}/nesrecomp")
include("${NESRECOMP_ROOT}/runner/runner.cmake")

if(WIN32)
    list(APPEND CMAKE_PREFIX_PATH "${NESRECOMP_ROOT}/runner/external/SDL2/cmake")
endif()
find_package(SDL2 REQUIRED CONFIG)

add_executable(RCRness
    ${NESRECOMP_RUNNER_SOURCES}
    desktop_pause.c
    extras.c
    rcr_widescreen.c
    generated/rcr_full.c
    generated/rcr_dispatch.c
)

target_include_directories(RCRness PRIVATE
    ${NESRECOMP_RUNNER_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}
    ${SDL2_INCLUDE_DIRS}
)
target_link_libraries(RCRness PRIVATE ${SDL2_LIBRARIES})
if(UNIX)
    target_link_libraries(RCRness PRIVATE m)
endif()

if(WIN32)
    target_link_libraries(RCRness PRIVATE ws2_32)
    add_custom_command(TARGET RCRness POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${NESRECOMP_ROOT}/runner/external/SDL2/lib/x64/SDL2.dll"
            "$<TARGET_FILE_DIR:RCRness>"
    )
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Desktop Exec uses its own quoting, followed by desktop-string escaping.
    set(RCRNESS_DESKTOP_EXEC "${CMAKE_CURRENT_BINARY_DIR}/RCRness")
    foreach(char IN ITEMS "\\" "\"" "$" "`")
        string(REPLACE "${char}" "\\${char}" RCRNESS_DESKTOP_EXEC "${RCRNESS_DESKTOP_EXEC}")
    endforeach()
    string(REPLACE "\\" "\\\\" RCRNESS_DESKTOP_EXEC "${RCRNESS_DESKTOP_EXEC}")
    string(REPLACE "%" "%%" RCRNESS_DESKTOP_EXEC "${RCRNESS_DESKTOP_EXEC}")
    configure_file(RCRness.desktop.in RCRness.desktop @ONLY
        FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
endif()

# The runner seeds missing bindings. Relinking must preserve player edits.

if(MSVC)
    target_compile_options(RCRness PRIVATE /W3 /WX- /wd4102)
else()
    target_compile_options(RCRness PRIVATE -Wall -Wextra -Wno-unused-label)
endif()

target_compile_definitions(RCRness PRIVATE
    NESRECOMP_DESKTOP_PAUSE=1
    NESRECOMP_GAME_PLAYERS=2
    NESRECOMP_GAME_OAM_DMA_FILTER=1
    NESRECOMP_DEFAULT_REDUCE_SPRITE_FLICKER=1
    NESRECOMP_DEFAULT_CRT_FILTER=1
)
