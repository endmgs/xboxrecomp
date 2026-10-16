# The console identity loader on a made-up EEPROM: checksums, fields, the
# HDD key from EvoX's hddinfo.txt. Portable.
add_executable(test_console_id
    test_console_id.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/kernel/xbox_console_id.c
)
target_include_directories(test_console_id PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/kernel
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
)
target_link_libraries(test_console_id PRIVATE xbox_config)
if(NOT WIN32)
    target_link_libraries(test_console_id PRIVATE platform)
endif()
add_test(NAME console_id COMMAND test_console_id ${CMAKE_CURRENT_BINARY_DIR}/console_id_test)
