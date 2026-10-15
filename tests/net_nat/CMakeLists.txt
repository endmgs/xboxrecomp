# The router behind syslink = nat, on 127.0.0.1: DHCP, ARP, UDP both ways,
# fragments both ways, TCP refused, ping. No game. Portable.
add_executable(test_net_nat
    test_net_nat.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/kernel/xbox_net_nat.c
)
target_include_directories(test_net_nat PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/kernel
)
if(WIN32)
    target_link_libraries(test_net_nat PRIVATE ws2_32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(test_net_nat PRIVATE Threads::Threads)
endif()
add_test(NAME net_nat COMMAND test_net_nat)
