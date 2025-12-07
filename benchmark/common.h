#ifndef ASYNCIO_BENCHMARK_COMMON_H
#define ASYNCIO_BENCHMARK_COMMON_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <chrono>

namespace bench {
    constexpr std::uint16_t TCP_PORT = 18080;
    constexpr std::uint16_t WS_PORT = 18081;
    constexpr const char* LOCALHOST = "127.0.0.1";

    inline std::vector<std::byte> makePayload(std::size_t size) {
        std::vector<std::byte> payload(size);
        for (std::size_t i = 0; i < size; ++i) {
            payload[i] = static_cast<std::byte>(i % 256);
        }
        return payload;
    }

    inline std::string makeStringPayload(std::size_t size) {
        std::string payload(size, 'x');
        for (std::size_t i = 0; i < size; ++i) {
            payload[i] = 'a' + static_cast<char>(i % 26);
        }
        return payload;
    }
}

#endif //ASYNCIO_BENCHMARK_COMMON_H
