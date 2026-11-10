set(HVNETPP_EXAMPLES
    unpack_selftest
    loop_selftest
    net_selftest
    net_bench
    tcp_echo_server
    tcp_chat_server
    tcp_client_test
    udp_echo_server
    udp_client_test
    timer_demo
)

foreach(example ${HVNETPP_EXAMPLES})
    add_executable(${example} ${example}.cpp)
    target_link_libraries(${example} PRIVATE hvnetpp)
endforeach()
