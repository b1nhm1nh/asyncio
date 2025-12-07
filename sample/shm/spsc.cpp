// Shared Memory SPSC Queue Example
// Demonstrates asyncio::net::lowlatency::shm::AsyncSPSCQueue

#include <asyncio/net/lowlatency/shm.h>
#include <iostream>
#include <thread>
#include <chrono>

using namespace asyncio::net::lowlatency::shm;

// Message type (must be trivially copyable and standard layout)
struct alignas(64) Message {
    uint64_t sequence;
    uint64_t timestamp_ns;
    double value;
    char data[40];
};

static_assert(ShmCompatible<Message>, "Message must be SHM compatible");

void producer(const std::string& queue_name, int count) {
    auto queue = AsyncSPSCQueue<Message>::create(queue_name, 1024);
    std::cout << "[Producer] Created queue with capacity " << queue.capacity() << std::endl;

    for (int i = 0; i < count; ++i) {
        Message msg{};
        msg.sequence = static_cast<uint64_t>(i);
        msg.timestamp_ns = static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        msg.value = i * 1.5;
        std::snprintf(msg.data, sizeof(msg.data), "Message #%d", i);

        while (!queue.try_send(msg)) {
            std::this_thread::yield();
        }

        if (i % 1000 == 0) {
            std::cout << "[Producer] Sent " << i << " messages" << std::endl;
        }
    }

    std::cout << "[Producer] Done sending " << count << " messages" << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
}

void consumer(const std::string& queue_name, int expected_count) {
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    auto queue = AsyncSPSCQueue<Message>::open(queue_name);
    std::cout << "[Consumer] Opened queue" << std::endl;

    int received = 0;
    auto start = std::chrono::steady_clock::now();

    while (received < expected_count) {
        if (auto msg = queue.try_receive(); msg) {
            ++received;
            if (received % 1000 == 0 || received == expected_count) {
                std::cout << "[Consumer] Received " << received << " messages, "
                          << "last seq=" << msg->sequence << std::endl;
            }
        } else {
            std::this_thread::yield();
        }
    }

    auto end = std::chrono::steady_clock::now();
    double duration = std::chrono::duration<double>(end - start).count();
    double throughput = received / duration;

    std::cout << "[Consumer] Done: " << received << " messages in "
              << duration << "s (" << throughput << " msg/s)" << std::endl;
}

int main(int argc, char* argv[]) {
    int count = 10000;
    if (argc > 1) {
        count = std::atoi(argv[1]);
    }

    std::string queue_name = "asyncio-shm-example";

    std::cout << "SPSC Queue Example: " << count << " messages" << std::endl;

    std::thread prod_thread(producer, queue_name, count);
    std::thread cons_thread(consumer, queue_name, count);

    prod_thread.join();
    cons_thread.join();

    std::cout << "Done!" << std::endl;
    return 0;
}
