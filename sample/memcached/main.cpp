#include <asyncio/memcached/client.h>
#include <asyncio/time.h>
#include <iostream>
#include <iomanip>

using namespace asyncio;
using namespace asyncio::memcached;

task::Task<void, std::error_code> asyncMain(int argc, char *argv[]) {
    std::string host = "127.0.0.1";
    std::uint16_t port = 11211;

    if (argc > 1) host = argv[1];
    if (argc > 2) port = static_cast<std::uint16_t>(std::stoi(argv[2]));

    std::cout << "Connecting to Memcached at " << host << ":" << port << "...\n";

    auto clientResult = co_await Client::connect(host, port);
    if (!clientResult) {
        std::cerr << "Failed to connect: " << clientResult.error().message() << "\n";
        co_return std::unexpected(clientResult.error());
    }

    auto& client = *clientResult;
    std::cout << "Connected!\n\n";

    // Version
    std::cout << "=== Server Version ===\n";
    auto versionResult = co_await client.version();
    if (versionResult) {
        for (const auto& [server, ver] : *versionResult) {
            std::cout << server << ": " << ver << "\n";
        }
    }
    std::cout << "\n";

    // Basic SET/GET
    std::cout << "=== Basic SET/GET ===\n";

    CO_EXPECT(co_await client.set("hello", "world"));
    std::cout << "SET hello 'world'\n";

    auto getValue = co_await client.getString("hello");
    if (getValue && *getValue) {
        std::cout << "GET hello: " << **getValue << "\n";
    }

    // Binary data
    std::cout << "\n=== Binary Data ===\n";
    std::vector<std::byte> binaryData = {
        std::byte{0x00}, std::byte{0x01}, std::byte{0x02},
        std::byte{0xFF}, std::byte{0xFE}, std::byte{0xFD}
    };

    CO_EXPECT(co_await client.set("binary_key", binaryData));
    std::cout << "SET binary_key (6 bytes of binary data)\n";

    auto binValue = co_await client.get("binary_key");
    if (binValue && *binValue) {
        std::cout << "GET binary_key: " << (*binValue)->data.size() << " bytes\n";
        std::cout << "  Hex:";
        for (auto b : (*binValue)->data) {
            std::cout << " " << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<int>(b);
        }
        std::cout << std::dec << "\n";
    }

    // Flags
    std::cout << "\n=== Custom Flags ===\n";
    std::uint32_t flags = 0xABCD;
    CO_EXPECT(co_await client.set("flagged", "data with flags", std::chrono::seconds{0}, flags));
    std::cout << "SET flagged with flags=0x" << std::hex << flags << std::dec << "\n";

    auto flaggedValue = co_await client.get("flagged");
    if (flaggedValue && *flaggedValue) {
        std::cout << "GET flagged: value='" << (*flaggedValue)->asString()
                  << "' flags=0x" << std::hex << (*flaggedValue)->flags << std::dec << "\n";
    }

    // ADD/REPLACE
    std::cout << "\n=== ADD/REPLACE ===\n";

    co_await client.del("add_test");
    auto addResult = co_await client.add("add_test", "initial value");
    if (addResult) {
        std::cout << "ADD add_test: " << (*addResult ? "success" : "failed") << "\n";
    }

    auto addResult2 = co_await client.add("add_test", "should fail");
    if (addResult2) {
        std::cout << "ADD add_test (again): " << (*addResult2 ? "success" : "failed - key exists") << "\n";
    }

    auto replaceResult = co_await client.replace("add_test", "replaced value");
    if (replaceResult) {
        std::cout << "REPLACE add_test: " << (*replaceResult ? "success" : "failed") << "\n";
    }

    auto addTestValue = co_await client.getString("add_test");
    if (addTestValue && *addTestValue) {
        std::cout << "GET add_test: " << **addTestValue << "\n";
    }

    // INCREMENT/DECREMENT
    std::cout << "\n=== INCREMENT/DECREMENT ===\n";

    CO_EXPECT(co_await client.set("counter", "100"));
    std::cout << "SET counter '100'\n";

    auto incrResult = co_await client.incr("counter", 1);
    if (incrResult) {
        std::cout << "INCR counter 1: " << *incrResult << "\n";
    }

    auto incrResult2 = co_await client.incr("counter", 10);
    if (incrResult2) {
        std::cout << "INCR counter 10: " << *incrResult2 << "\n";
    }

    auto decrResult = co_await client.decr("counter", 5);
    if (decrResult) {
        std::cout << "DECR counter 5: " << *decrResult << "\n";
    }

    // APPEND/PREPEND
    std::cout << "\n=== APPEND/PREPEND ===\n";

    CO_EXPECT(co_await client.set("message", "Hello"));
    std::cout << "SET message 'Hello'\n";

    CO_EXPECT(co_await client.append("message", " World"));
    std::cout << "APPEND message ' World'\n";

    CO_EXPECT(co_await client.prepend("message", "Say: "));
    std::cout << "PREPEND message 'Say: '\n";

    auto msgValue = co_await client.getString("message");
    if (msgValue && *msgValue) {
        std::cout << "GET message: " << **msgValue << "\n";
    }

    // TOUCH
    std::cout << "\n=== TOUCH ===\n";

    CO_EXPECT(co_await client.set("expire_test", "will expire", std::chrono::seconds{30}));
    std::cout << "SET expire_test with 30s expiration\n";

    auto touchResult = co_await client.touch("expire_test", std::chrono::seconds{60});
    if (touchResult) {
        std::cout << "TOUCH expire_test 60: " << (*touchResult ? "success" : "key not found") << "\n";
    }

    // GAT (Get And Touch)
    std::cout << "\n=== GAT (Get And Touch) ===\n";

    auto gatResult = co_await client.gat("expire_test", std::chrono::seconds{120});
    if (gatResult && *gatResult) {
        std::cout << "GAT expire_test 120: " << (*gatResult)->asString() << "\n";
    }

    // DELETE
    std::cout << "\n=== DELETE ===\n";

    auto delResult = co_await client.del("hello");
    if (delResult) {
        std::cout << "DELETE hello: " << (*delResult ? "deleted" : "not found") << "\n";
    }

    auto delResult2 = co_await client.del("nonexistent_key_12345");
    if (delResult2) {
        std::cout << "DELETE nonexistent_key_12345: " << (*delResult2 ? "deleted" : "not found") << "\n";
    }

    // Stats
    std::cout << "\n=== Server Stats ===\n";
    auto statsResult = co_await client.stats();
    if (statsResult && !statsResult->empty()) {
        const auto& serverStats = (*statsResult)[0];
        std::cout << "Stats from " << serverStats.server << ":\n";
        int count = 0;
        for (const auto& [key, value] : serverStats.stats) {
            if (count++ >= 10) {
                std::cout << "  ... (and more)\n";
                break;
            }
            std::cout << "  " << key << ": " << value << "\n";
        }
    }

    // Cleanup
    std::cout << "\n=== Cleanup ===\n";
    co_await client.del("binary_key");
    co_await client.del("flagged");
    co_await client.del("add_test");
    co_await client.del("counter");
    co_await client.del("message");
    co_await client.del("expire_test");
    std::cout << "Cleaned up test keys\n";

    CO_EXPECT(co_await client.disconnect());
    std::cout << "\nDisconnected from Memcached.\n";

    co_return {};
}
