add_executable(asyncio_tcp_client tcp/client.cpp)
target_link_libraries(asyncio_tcp_client PRIVATE asyncio-main)

add_executable(asyncio_tcp_server tcp/server.cpp)
target_link_libraries(asyncio_tcp_server PRIVATE asyncio-main)

add_executable(asyncio_tls_client tls/client.cpp)
target_link_libraries(asyncio_tls_client PRIVATE asyncio-main)

add_executable(asyncio_tls_server tls/server.cpp)
target_link_libraries(asyncio_tls_server PRIVATE asyncio-main)

add_executable(asyncio_http http/main.cpp)
target_link_libraries(asyncio_http PRIVATE asyncio-main)

add_executable(asyncio_ws ws/main.cpp)
target_link_libraries(asyncio_ws PRIVATE asyncio-main)

add_executable(asyncio_mqtt mqtt/main.cpp)
target_link_libraries(asyncio_mqtt PRIVATE asyncio-main)

add_executable(asyncio_candle candle/main.cpp)
target_link_libraries(asyncio_candle PRIVATE asyncio-main)

add_executable(asyncio_redis redis/main.cpp)
target_link_libraries(asyncio_redis PRIVATE asyncio-main)

add_executable(asyncio_memcached memcached/main.cpp)
target_link_libraries(asyncio_memcached PRIVATE asyncio-main)

add_executable(asyncio_perf_test perf_test/main.cpp)
target_link_libraries(asyncio_perf_test PRIVATE asyncio-main)

add_executable(asyncio_memory_test perf_test/memory_test.cpp)
target_link_libraries(asyncio_memory_test PRIVATE asyncio-main)

# Low-latency samples (header-only, no asyncio-main needed for simple tests)
add_executable(asyncio_udp_echo udp/echo.cpp)
target_link_libraries(asyncio_udp_echo PRIVATE asyncio)

add_executable(asyncio_shm_spsc shm/spsc.cpp)
target_link_libraries(asyncio_shm_spsc PRIVATE asyncio)

# KCP requires the kcp library (ikcp.c)
# add_executable(asyncio_kcp_echo kcp/echo.cpp)
# target_link_libraries(asyncio_kcp_echo PRIVATE asyncio kcp)
