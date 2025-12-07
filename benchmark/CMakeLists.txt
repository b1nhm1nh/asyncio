find_package(benchmark CONFIG REQUIRED)
find_package(asio CONFIG REQUIRED)
find_package(Drogon CONFIG REQUIRED)

# TCP Echo Benchmarks
add_executable(
        bench_tcp_echo
        tcp_echo_asyncio.cpp
        tcp_echo_asio.cpp
        tcp_echo_asio_async.cpp
        tcp_echo_trantor.cpp
        tcp_echo_main.cpp
)

target_link_libraries(
        bench_tcp_echo
        PRIVATE
        asyncio
        asio::asio
        Drogon::Drogon
        benchmark::benchmark
)

# WebSocket Benchmarks (external server)
add_executable(
        bench_websocket
        ws_asyncio.cpp
        ws_asyncio_optimized.cpp
        ws_drogon.cpp
        ws_main.cpp
)

# WebSocket Benchmarks (internal server)
add_executable(
        bench_websocket_internal
        ws_server_asyncio.cpp
        ws_server_drogon.cpp
        ws_main.cpp
)

target_link_libraries(
        bench_websocket
        PRIVATE
        asyncio
        Drogon::Drogon
        benchmark::benchmark
)

target_link_libraries(
        bench_websocket_internal
        PRIVATE
        asyncio
        Drogon::Drogon
        benchmark::benchmark
)
