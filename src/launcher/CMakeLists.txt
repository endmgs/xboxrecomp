# The launcher a recompiled title ships beside its game.
#
# Not a library and not built here: each title makes its own, because the
# executable it starts is the title's. A title's CMakeLists calls
#
#     recomp_add_launcher(timesplitters2_launcher timesplitters2_recomp.exe)
#
# (add NO_WIDESCREEN for a title where widescreen must not be offered; see
# below. What the one Widescreen switch does for a title that does offer it
# is said with recomp_title_widescreen, src/config/CMakeLists.txt)
#
# and gets <name>.exe beside the game, reading the game's own XBE for the
# name it shows and the settings file it writes.
#
# Windows only for now. The drawing is behind launcher_theme.h so the day
# this needs to run elsewhere it is that file that is replaced, not the
# launcher.

if(WIN32)
    function(recomp_add_launcher TARGET GAME_EXE)
        add_executable(${TARGET} WIN32
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/launcher.c
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/launcher_theme.c
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/launcher_bindings.c
        )
        target_include_directories(${TARGET} PRIVATE
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}
        )
        target_compile_definitions(${TARGET} PRIVATE
            LAUNCHER_GAME_EXE="${GAME_EXE}"
        )
        # The launcher has to find the game's XBE where the game does, or it
        # names the wrong settings file and every choice made in it is lost.
        # The game looks beside itself in game/, then in YOUR_GAME_DIR from
        # its main.c; read that define rather than repeat it here, so the
        # two cannot drift. It is a C string literal, so its separators are
        # doubled backslashes: made forward slashes, which Windows accepts.
        set(_main_c "${CMAKE_CURRENT_SOURCE_DIR}/src/main.c")
        if(EXISTS "${_main_c}")
            file(STRINGS "${_main_c}" _dir_line
                 REGEX "^#define[ \t]+YOUR_GAME_DIR[ \t]+\"")
            if(_dir_line MATCHES "\"([^\"]*)\"")
                string(REPLACE "\\\\" "/" _game_dir "${CMAKE_MATCH_1}")
                target_compile_definitions(${TARGET} PRIVATE
                    LAUNCHER_GAME_DIR="${_game_dir}")
            endif()
            # Re-configure when main.c changes, e.g. after regen_title_main.py.
            set_property(DIRECTORY APPEND PROPERTY
                CMAKE_CONFIGURE_DEPENDS "${_main_c}")
        endif()
        # xbox_config carries the settings file's format, so the launcher
        # and the runtime cannot disagree about what they are writing and
        # reading. xbox_input is the pads (recomp_pad.h: SDL3, or XInput),
        # the same code the game reads them with; the rest is in every Windows.
        # comdlg32: the Select ISO dialog. shell32: folders made and removed
        # by the install. advapi32: the SHA-1 of the installed XBE.
        target_link_libraries(${TARGET} PRIVATE
            xbox_config xbox_input user32 gdi32 comdlg32 shell32 advapi32
        )
        # recomp_add_launcher(<target> <exe> NO_WIDESCREEN): a title where
        # widescreen cannot be right hides the Widescreen row, and the
        # launcher writes it off. A 2D fighter is the case: its stage
        # bounds and walls are built for 4:3, so a wider view shows space the
        # fighters cannot reach.
        #
        # TITLE "<name>": what the window says before the game is installed.
        # XBE_SHA1 <hex>: the default.xbe this build was made for; an install
        # from another disc warns. DEV_FILE <path in the game folder> and
        # DEV_HELP "<text>": a Developer mode row that makes or removes that
        # file. Keep double quotes out of TITLE and DEV_HELP.
        cmake_parse_arguments(_launcher "NO_WIDESCREEN"
                              "TITLE;XBE_SHA1;DEV_FILE;DEV_HELP" "" ${ARGN})
        if(_launcher_NO_WIDESCREEN)
            target_compile_definitions(${TARGET} PRIVATE LAUNCHER_NO_WIDESCREEN=1)
        endif()
        foreach(_opt TITLE XBE_SHA1 DEV_FILE DEV_HELP)
            if(DEFINED _launcher_${_opt})
                target_compile_definitions(${TARGET} PRIVATE
                    "LAUNCHER_${_opt}=\"${_launcher_${_opt}}\"")
            endif()
        endforeach()
        if(MSVC)
            target_compile_options(${TARGET} PRIVATE
                /W4 $<$<NOT:$<CONFIG:Debug>>:/O2>)
        endif()
    endfunction()
endif()
