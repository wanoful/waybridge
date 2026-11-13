// Exercise a stalled transport, FIFO delivery, bounded producers and cancellation.
#include "../client/ordered_sender.h"
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
using namespace std::chrono_literals;
static void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
int main() {
  try {
    OrderedSender sender(4, 3);
    std::mutex mutex;
    std::condition_variable wake;
    bool stalled = true, entered = false;
    std::vector<unsigned char> delivered;
    std::atomic<bool> cancelled{false};
    sender.start(
        [&](const auto &packet) {
          std::unique_lock<std::mutex> guard(mutex);
          entered = true;
          wake.notify_all();
          wake.wait(guard, [&] { return !stalled || cancelled.load(); });
          if (cancelled)
            return false;
          delivered.push_back(packet[0]);
          wake.notify_all();
          return true;
        },
        [&] {
          cancelled = true;
          wake.notify_all();
        });
    require(sender.enqueue({1, 1}, false), "first enqueue");
    {
      std::unique_lock<std::mutex> guard(mutex);
      require(wake.wait_for(guard, 2s, [&] { return entered; }), "writer did not start");
    }
    auto started = std::chrono::steady_clock::now();
    require(sender.enqueue({2, 2}, false), "second enqueue blocked behind transport");
    require(!sender.enqueue({3}, false), "nonblocking enqueue exceeded budget");
    require(std::chrono::steady_clock::now() - started < 100ms, "UI producer waited on writer");
    auto producer = std::async(std::launch::async, [&] { return sender.enqueue({3}, true); });
    require(producer.wait_for(30ms) == std::future_status::timeout,
            "worker producer ignored budget");
    {
      std::lock_guard<std::mutex> guard(mutex);
      stalled = false;
    }
    wake.notify_all();
    require(producer.wait_for(2s) == std::future_status::ready && producer.get(),
            "producer not resumed");
    {
      std::unique_lock<std::mutex> guard(mutex);
      require(wake.wait_for(guard, 2s, [&] { return delivered.size() == 3; }), "delivery stalled");
      require(delivered == std::vector<unsigned char>({1, 2, 3}), "wire order changed");
      stalled = true;
      entered = false;
    }
    require(sender.enqueue({4, 4, 4, 4}, false), "final enqueue");
    {
      std::unique_lock<std::mutex> guard(mutex);
      require(wake.wait_for(guard, 2s, [&] { return entered; }), "final writer not entered");
    }
    auto blocked = std::async(std::launch::async, [&] { return sender.enqueue({5}, true); });
    require(blocked.wait_for(30ms) == std::future_status::timeout, "final producer not bounded");
    sender.stop();
    require(blocked.wait_for(2s) == std::future_status::ready && !blocked.get(),
            "stop did not cancel producer");
    require(!sender.enqueue({6}, false), "stopped sender accepted work");
    std::cout << "PASS: nonblocking UI enqueue, bounded worker backpressure, FIFO and shutdown "
                 "cancellation\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
