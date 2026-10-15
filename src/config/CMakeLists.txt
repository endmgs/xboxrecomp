# Player settings: the file the launcher writes and the runtime reads.
#
# Deliberately depends on nothing. The d3d backend, the kernel and the HLE
# all read settings, so anything this linked would be linked by all three.

add_library(xbox_config STATIC
    recomp_config.c
)

target_include_directories(xbox_config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
    target_compile_options(xbox_config PRIVATE /W4 $<$<NOT:$<CONFIG:Debug>>:/O2>)
else()
    target_compile_options(xbox_config PRIVATE -Wall -Wextra -O2)
endif()

# What widescreen means for the title being built, said once in its
# CMakeLists, after the add_subdirectory that brings the toolkit in:
#
#     recomp_title_widescreen(NATIVE)                  # has a 16:9 mode
#     recomp_title_widescreen(HOR_PLUS REGISTER 60)    # has none
#     recomp_title_widescreen(NATIVE REGISTER 176 CENTRE_2D)
#
# CENTRE_2D: the game's 2D is drawn for 4:3 even in its 16:9 mode, so keep
# all of it at 4:3 but whole-screen flat-colour passes (fades, letterbox
# bars) -- the widescreen_2d = centre policy, as what widescreen_2d = auto
# means for this title.
#
# The player has one widescreen switch. Whether that is the console flag
# alone or the flag and a wider camera is a fact about the game, and so is
# the vertex constant register its projection is in (REGISTER, optional for
# either mode; without it the register is 60). See recomp_config.h,
# "widescreen", for how it is resolved and what a title that says nothing
# gets. It is compiled into this library because the game and its launcher
# both link it, so the two cannot be told different things; a title that
# would rather say it in code calls recomp_widescreen_title.
#
# A title where widescreen cannot be right at all says so to its launcher
# instead: recomp_add_launcher(... NO_WIDESCREEN).
function(recomp_title_widescreen MODE)
    cmake_parse_arguments(_wide "CENTRE_2D" "REGISTER" "" ${ARGN})
    if(MODE STREQUAL "NATIVE")
        set(_mode 1)                                # RECOMP_WIDE_NATIVE
    elseif(MODE STREQUAL "HOR_PLUS")
        set(_mode 2)                                # RECOMP_WIDE_HOR_PLUS
    else()
        message(FATAL_ERROR
            "recomp_title_widescreen: \"${MODE}\" is neither NATIVE nor HOR_PLUS")
    endif()
    if(_wide_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "recomp_title_widescreen: unexpected \"${_wide_UNPARSED_ARGUMENTS}\"")
    endif()
    target_compile_definitions(xbox_config PRIVATE RECOMP_TITLE_WIDESCREEN=${_mode})
    if(_wide_CENTRE_2D)
        target_compile_definitions(xbox_config PRIVATE RECOMP_TITLE_CENTRE_2D=1)
    endif()
    if(DEFINED _wide_REGISTER)
        if(NOT _wide_REGISTER MATCHES "^[0-9]+$" OR _wide_REGISTER GREATER 191)
            message(FATAL_ERROR
                "recomp_title_widescreen: REGISTER ${_wide_REGISTER} is not 0..191")
        endif()
        target_compile_definitions(xbox_config PRIVATE
            RECOMP_TITLE_PROJECTION_REGISTER=${_wide_REGISTER})
    endif()
endfunction()

# A setting that belongs to the title being built, with a row in its
# launcher (recomp_config.h, "title options"):
#
#     recomp_title_option(ts2_big_head
#         ENV    RECOMP_TS2_BIG_HEAD
#         LABEL  "Big heads"
#         HELP   "Every character's head drawn bigger."
#         VALUES 1 1.5 2 2.5 3
#         NAMES  Off 1.5x 2x 2.5x 3x)
#
# The key is what the settings file holds and what the title's code reads;
# ENV, optional, is the variable over it. VALUES are what the launcher's row
# steps through, the first being the default; NAMES, optional, are how each
# is shown (as many as VALUES). One call per option, in the order the rows
# should have; at most 8 options of at most 12 values each.
#
# Each call rewrites one generated file in the build tree with every option
# declared so far, and compiles it into xbox_config, which the game and the
# launcher both link, so the two cannot disagree about the list.
function(recomp_title_option KEY)
    cmake_parse_arguments(_opt "" "ENV;LABEL;HELP" "VALUES;NAMES" ${ARGN})
    if(_opt_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "recomp_title_option(${KEY}): unexpected \"${_opt_UNPARSED_ARGUMENTS}\"")
    endif()
    if(NOT KEY MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
        message(FATAL_ERROR "recomp_title_option: \"${KEY}\" is not a key (letters, digits, _)")
    endif()
    if(NOT _opt_VALUES)
        message(FATAL_ERROR "recomp_title_option(${KEY}): no VALUES")
    endif()
    list(LENGTH _opt_VALUES _nvalues)
    if(_nvalues GREATER 12)
        message(FATAL_ERROR "recomp_title_option(${KEY}): more than 12 VALUES")
    endif()
    if(_opt_NAMES)
        list(LENGTH _opt_NAMES _nnames)
        if(NOT _nnames EQUAL _nvalues)
            message(FATAL_ERROR
                "recomp_title_option(${KEY}): ${_nnames} NAMES for ${_nvalues} VALUES")
        endif()
    endif()
    if(NOT DEFINED _opt_LABEL)
        set(_opt_LABEL "${KEY}")
    endif()

    # As a C string literal.
    macro(_recomp_c_string OUT IN)
        string(REPLACE "\\" "\\\\" _s "${IN}")
        string(REPLACE "\"" "\\\"" _s "${_s}")
        string(REPLACE "\n" " " _s "${_s}")
        set(${OUT} "\"${_s}\"")
    endmacro()

    _recomp_c_string(_key "${KEY}")
    if(DEFINED _opt_ENV)
        _recomp_c_string(_env "${_opt_ENV}")
    else()
        set(_env "NULL")
    endif()
    _recomp_c_string(_label "${_opt_LABEL}")
    if(DEFINED _opt_HELP)
        _recomp_c_string(_help "${_opt_HELP}")
    else()
        set(_help "\"\"")
    endif()
    set(_values "")
    foreach(_v IN LISTS _opt_VALUES)
        _recomp_c_string(_c "${_v}")
        string(APPEND _values "${_c}, ")
    endforeach()
    set(_names "")
    foreach(_n IN LISTS _opt_NAMES)
        _recomp_c_string(_c "${_n}")
        string(APPEND _names "${_c}, ")
    endforeach()

    get_property(_keys GLOBAL PROPERTY RECOMP_TITLE_OPTION_KEYS)
    if(KEY IN_LIST _keys)
        message(FATAL_ERROR "recomp_title_option: ${KEY} declared twice")
    endif()
    list(LENGTH _keys _count)
    if(_count GREATER_EQUAL 8)
        message(FATAL_ERROR "recomp_title_option(${KEY}): more than 8 options")
    endif()
    set_property(GLOBAL APPEND PROPERTY RECOMP_TITLE_OPTION_KEYS "${KEY}")
    set_property(GLOBAL APPEND_STRING PROPERTY RECOMP_TITLE_OPTION_LINES
        "{ ${_key}, ${_env}, ${_label}, ${_help}, { ${_values}NULL }, { ${_names}NULL } },\n")

    get_property(_lines GLOBAL PROPERTY RECOMP_TITLE_OPTION_LINES)
    set(_file "${CMAKE_BINARY_DIR}/recomp_title_options.inc")
    # Only written when it changes, so a re-configure does not rebuild.
    file(CONFIGURE OUTPUT "${_file}"
         CONTENT "/* Generated by recomp_title_option (xboxrecomp src/config/CMakeLists.txt). */\n${_lines}"
         @ONLY)
    if(_count EQUAL 0)
        target_compile_definitions(xbox_config PRIVATE RECOMP_TITLE_OPTIONS_INC="${_file}")
    endif()
endfunction()

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/config_smoke
                 ${CMAKE_CURRENT_BINARY_DIR}/config_smoke)
