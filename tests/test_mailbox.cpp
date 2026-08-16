#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "common/gate.h"
#include "common/mailbox.h"

using namespace krg;
using namespace std::chrono_literals;

TEST_CASE("an item pushed is the item popped") {
    Mailbox<int> m;
    m.push(7);
    auto got = m.pop();
    REQUIRE(got.has_value());
    CHECK(*got == 7);
}

TEST_CASE("a consumer that fell behind gets the newest item, not the oldest") {
    Mailbox<int> m;
    m.push(1);
    m.push(2);
    m.push(3);
    auto got = m.pop();
    REQUIRE(got.has_value());
    // The whole point of the pipeline: stale frames are dropped before
    // encoding, never after.
    CHECK(*got == 3);
    CHECK_FALSE(m.pop_for(1ms).has_value());
}

TEST_CASE("the merge folds what was dropped into what replaced it") {
    // The LZ4 path's rule: a dropped frame's pixels are already contained in
    // the frame that replaced it, but its dirty rects are not, so they are
    // carried across or the receiver never repaints those regions.
    Mailbox<std::vector<int>> m([](std::vector<int>& incoming, std::vector<int>& pending) {
        incoming.insert(incoming.begin(), pending.begin(), pending.end());
    });
    m.push({1, 2});
    m.push({3});
    auto got = m.pop();
    REQUIRE(got.has_value());
    CHECK(*got == std::vector<int>{1, 2, 3});
}

TEST_CASE("the merge runs for every dropped item, not just the last") {
    Mailbox<std::string> m([](std::string& incoming, std::string& pending) {
        incoming = pending + incoming;
    });
    m.push("a");
    m.push("b");
    m.push("c");
    auto got = m.pop();
    REQUIRE(got.has_value());
    CHECK(*got == "abc");
}

TEST_CASE("clear throws away what is waiting") {
    // The sender rebuilding its encoder for a new display mode, with a frame
    // cut to the old one still in the slot.
    Mailbox<int> m;
    m.push(1);
    m.clear();
    CHECK_FALSE(m.pop_for(1ms).has_value());
}

TEST_CASE("a timed pop on an empty mailbox reports nothing rather than blocking") {
    Mailbox<int> m;
    const auto t0 = std::chrono::steady_clock::now();
    CHECK_FALSE(m.pop_for(20ms).has_value());
    CHECK(std::chrono::steady_clock::now() - t0 >= 15ms);
}

TEST_CASE("stop releases a blocked consumer with nothing") {
    Mailbox<int> m;
    std::atomic<bool> returned{false};
    std::thread consumer([&] {
        auto got = m.pop();
        CHECK_FALSE(got.has_value());
        returned = true;
    });
    std::this_thread::sleep_for(10ms);
    CHECK_FALSE(returned.load());
    m.stop();
    consumer.join();
    CHECK(returned.load());
}

TEST_CASE("a blocked consumer wakes on a push") {
    Mailbox<int> m;
    std::thread producer([&] {
        std::this_thread::sleep_for(10ms);
        m.push(42);
    });
    auto got = m.pop(); // blocks until the push above
    producer.join();
    REQUIRE(got.has_value());
    CHECK(*got == 42);
}

TEST_CASE("a gate starts closed and opens once") {
    Gate g;
    CHECK_FALSE(g.is_set());
    g.set(true);
    CHECK(g.is_set());
    g.wait(); // already open: returns without blocking
    g.set(false);
    CHECK_FALSE(g.is_set());
}

TEST_CASE("a thread parked on a closed gate resumes when it opens") {
    // This is what stops the sender's capture thread between clients: acquiring
    // and copying a full desktop image costs the same whether or not anyone is
    // watching it.
    Gate g;
    std::atomic<bool> past{false};
    std::thread worker([&] {
        g.wait();
        past = true;
    });
    std::this_thread::sleep_for(10ms);
    CHECK_FALSE(past.load());
    g.set(true);
    worker.join();
    CHECK(past.load());
}
