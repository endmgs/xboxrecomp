# xbe_patches -- a title lifted from a patched XBE, loaded from the unpatched
# one (docs/technical/xbe-patches.md). The fixture and its
# recomp_xbe_patches.c come from tools/xbe_patch at build time, as a lift
# makes them, so this needs a Python 3 and skips without one. POSIX
# (setenv/_exit), like memory_layout; no game files.

find_package(Python3 COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND)
    message(STATUS "xbe_patches test skipped: no Python 3 to build its fixture")
    return()
endif()

get_filename_component(XBE_PATCHES_REPO "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(XBE_PATCHES_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/fixture)

add_custom_command(
    OUTPUT ${XBE_PATCHES_FIXTURE}/recomp_xbe_patches.c
           ${XBE_PATCHES_FIXTURE}/original.xbe
           ${XBE_PATCHES_FIXTURE}/patched.xbe
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/make_fixture.py
            ${XBE_PATCHES_FIXTURE}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/make_fixture.py
            ${XBE_PATCHES_REPO}/tools/xbe_patch/patcher.py
            ${XBE_PATCHES_REPO}/tools/xbe_patch/test_xbe_patch.py
            ${XBE_PATCHES_REPO}/scripts/make_test_xbe.py
    COMMENT "xbe_patches fixture (tools/xbe_patch)"
    VERBATIM)

add_executable(xbe_patches_test test_main.c ${XBE_PATCHES_FIXTURE}/recomp_xbe_patches.c)
target_link_libraries(xbe_patches_test PRIVATE xboxrecomp)

add_test(NAME xbe_patches
         COMMAND xbe_patches_test ${XBE_PATCHES_FIXTURE}/original.xbe
                                  ${XBE_PATCHES_FIXTURE}/patched.xbe)
