# The System Link switch on 127.0.0.1: a host and several joining links in
# one process, no game, no router. Portable.
add_executable(test_syslink_hub
    test_syslink_hub.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/kernel/xbox_net_udp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/kernel/xbox_net_nat.c
)
target_include_directories(test_syslink_hub PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/kernel
)
target_link_libraries(test_syslink_hub PRIVATE xbox_config xbox_net)
if(WIN32)
    target_link_libraries(test_syslink_hub PRIVATE ws2_32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(test_syslink_hub PRIVATE Threads::Threads)
endif()
add_test(NAME syslink_hub COMMAND test_syslink_hub)
