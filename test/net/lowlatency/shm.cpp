#include <catch_extensions.h>
#include <asyncio/net/lowlatency/shm.h>
#include <thread>

using namespace asyncio::net::lowlatency::shm;

// Test message type
struct TestMessage {
    uint64_t sequence;
    uint64_t value;
};

static_assert(ShmCompatible<TestMessage>, "TestMessage must be SHM compatible");

TEST_CASE("SPSC Queue creation", "[net::lowlatency::shm]") {
    SECTION("create and destroy") {
        auto queue = AsyncSPSCQueue<TestMessage>::create("test-spsc-create", 64);
        REQUIRE(queue.capacity() >= 64);
    }
}

TEST_CASE("SPSC Queue operations", "[net::lowlatency::shm]") {
    auto queue = AsyncSPSCQueue<TestMessage>::create("test-spsc-ops", 64);

    SECTION("send and receive") {
        TestMessage msg{1, 100};
        auto result = queue.try_send(msg);
        REQUIRE(result.has_value());

        auto received = queue.try_receive();
        REQUIRE(received.has_value());
        REQUIRE(received->sequence == 1);
        REQUIRE(received->value == 100);
    }

    SECTION("empty queue returns error") {
        auto received = queue.try_receive();
        REQUIRE(!received.has_value());
        REQUIRE(received.error() == Error::QUEUE_EMPTY);
    }

    SECTION("full queue returns error") {
        // Fill the queue
        for (size_t i = 0; i < queue.capacity(); ++i) {
            TestMessage msg{i, i * 10};
            REQUIRE(queue.try_send(msg).has_value());
        }

        // Next send should fail
        TestMessage extra{999, 999};
        auto result = queue.try_send(extra);
        REQUIRE(!result.has_value());
        REQUIRE(result.error() == Error::QUEUE_FULL);
    }
}

TEST_CASE("SPSC Queue loan API", "[net::lowlatency::shm]") {
    auto queue = AsyncSPSCQueue<TestMessage>::create("test-spsc-loan", 64);

    SECTION("loan and commit") {
        auto loan = queue.try_loan();
        REQUIRE(loan.has_value());

        loan->get()->sequence = 42;
        loan->get()->value = 100;
        loan->commit();

        auto received = queue.try_receive();
        REQUIRE(received.has_value());
        REQUIRE(received->sequence == 42);
    }

    SECTION("borrow and release") {
        // First send something
        TestMessage msg{1, 50};
        REQUIRE(queue.try_send(msg).has_value());

        // Borrow it
        auto borrowed = queue.try_borrow();
        REQUIRE(borrowed.has_value());
        REQUIRE(borrowed->get()->sequence == 1);
        REQUIRE(borrowed->get()->value == 50);

        // Release is automatic on destruction
    }
}

TEST_CASE("SPSC Queue batch operations", "[net::lowlatency::shm]") {
    auto queue = AsyncSPSCQueue<TestMessage>::create("test-spsc-batch", 256);

    SECTION("batch send") {
        std::vector<TestMessage> batch(32);
        for (size_t i = 0; i < batch.size(); ++i) {
            batch[i] = {i, i * 10};
        }

        size_t sent = queue.try_send_batch(batch.data(), batch.size());
        REQUIRE(sent == 32);
    }

    SECTION("batch receive") {
        // Send some messages first
        for (uint64_t i = 0; i < 32; ++i) {
            queue.try_send({i, i * 10});
        }

        std::vector<TestMessage> batch(32);
        size_t received = queue.try_receive_batch(batch.data(), batch.size());
        REQUIRE(received == 32);
        REQUIRE(batch[0].sequence == 0);
        REQUIRE(batch[31].sequence == 31);
    }
}

TEST_CASE("SPSC Queue fill level", "[net::lowlatency::shm]") {
    auto queue = AsyncSPSCQueue<TestMessage>::create("test-spsc-fill", 64);

    SECTION("empty queue") {
        REQUIRE(queue.fill_level() == 0.0);
        REQUIRE(queue.available_slots() == queue.capacity());
    }

    SECTION("partially filled") {
        for (int i = 0; i < 32; ++i) {
            queue.try_send({static_cast<uint64_t>(i), 0});
        }
        REQUIRE(queue.fill_level() == Approx(0.5).margin(0.1));
    }

    SECTION("high water mark") {
        // Fill to 80%
        for (size_t i = 0; i < queue.capacity() * 80 / 100; ++i) {
            queue.try_send({i, 0});
        }
        REQUIRE(queue.is_high_water(0.75));
    }
}

TEST_CASE("Error codes", "[net::lowlatency::shm]") {
    REQUIRE(Error::QUEUE_FULL != Error::QUEUE_EMPTY);
    REQUIRE(Error::DISCONNECTED != Error::TIMEOUT);

    std::error_code ec = Error::QUEUE_FULL;
    REQUIRE(ec.message().find("full") != std::string::npos);
}
