#pragma once
// A single ordered writer. No transport I/O runs while the queue mutex is held.
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class OrderedSender {
public:
  using Packet = std::vector<unsigned char>;
  using Write = std::function<bool(const Packet &)>;
  using Cancel = std::function<void()>;

private:
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<Packet> packets;
  size_t bytes = 0;
  const size_t byteLimit, countLimit;
  bool stopped = true;
  std::thread worker;
  Write write;
  Cancel cancel;

public:
  explicit OrderedSender(size_t byteLimit = 4 * 1024 * 1024, size_t countLimit = 512)
      : byteLimit(byteLimit), countLimit(countLimit) {}
  OrderedSender(const OrderedSender &) = delete;
  OrderedSender &operator=(const OrderedSender &) = delete;
  ~OrderedSender() { stop(); }
  void start(Write send, Cancel abort) {
    stop();
    write = std::move(send);
    cancel = std::move(abort);
    {
      std::lock_guard<std::mutex> guard(mutex);
      stopped = false;
    }
    worker = std::thread([this] {
      for (;;) {
        Packet packet;
        {
          std::unique_lock<std::mutex> guard(mutex);
          changed.wait(guard, [&] { return stopped || !packets.empty(); });
          if (stopped)
            break;
          packet = std::move(packets.front());
          packets.pop_front();
          // Keep the in-progress write in the memory budget until it completes.
        }
        bool ok = false;
        try {
          ok = write(packet);
        } catch (...) {
        }
        {
          std::lock_guard<std::mutex> guard(mutex);
          bytes -= packet.size();
          if (!ok) {
            stopped = true;
            packets.clear();
            bytes = 0;
          }
        }
        changed.notify_all();
        if (!ok) {
          cancel();
          break;
        }
      }
    });
  }
  bool enqueue(Packet packet, bool mayWait) {
    std::unique_lock<std::mutex> guard(mutex);
    auto room = [&] {
      // One large clipboard packet is permitted only with an otherwise empty
      // writer. The caller also enforces the protocol's absolute packet limit.
      return !bytes || (packet.size() <= byteLimit && bytes <= byteLimit - packet.size() &&
                        packets.size() < countLimit);
    };
    if (mayWait)
      changed.wait(guard, [&] { return stopped || room(); });
    if (stopped || !room())
      return false;
    bytes += packet.size();
    packets.push_back(std::move(packet));
    guard.unlock();
    changed.notify_all();
    return true;
  }
  void stop() {
    bool abort = false;
    {
      std::lock_guard<std::mutex> guard(mutex);
      abort = !stopped;
      stopped = true;
      // A worker may still hold the in-progress packet; do not adjust its bytes.
      for (const auto &packet : packets)
        bytes -= packet.size();
      packets.clear();
    }
    changed.notify_all();
    if (abort && cancel)
      cancel();
    if (worker.joinable())
      worker.join();
  }
};
