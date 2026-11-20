#pragma once
// Serialized filesystem work; clipboard/STA work returns through a separate UI inbox.
#include <atomic>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <thread>

namespace FileIo {
inline std::atomic<bool> active{false};
inline thread_local bool onWorker = false;
inline std::mutex mutex;
inline std::condition_variable wake;
struct Task {
  std::function<void()> run;
  size_t bytes;
  bool cleanup;
  const void *key;
};
inline std::deque<Task> tasks;
inline size_t queuedBytes = 0;
inline bool stopping = false;
inline std::thread worker;
inline std::mutex uiMutex;
inline std::deque<std::function<void()>> completions;
inline std::function<void()> notifyUi;
inline std::function<void()> notifyFailure;
inline std::atomic<bool> failed{false};

inline void fail() {
  if (!failed.exchange(true) && notifyFailure)
    notifyFailure();
  wake.notify_all();
}

inline void start(std::function<void()> notify, std::function<void()> failure = {}) {
  notifyUi = std::move(notify);
  notifyFailure = std::move(failure);
  failed = false;
  stopping = false;
  active = true;
  worker = std::thread([] {
    onWorker = true;
    for (;;) {
      Task task;
      bool execute;
      {
        std::unique_lock<std::mutex> lock(mutex);
        wake.wait(lock, [] { return stopping || !tasks.empty(); });
        if (tasks.empty() && stopping)
          break;
        task = std::move(tasks.front());
        tasks.pop_front();
        queuedBytes -= task.bytes;
        execute = (!stopping && !failed) || task.cleanup;
      }
      wake.notify_all();
      if (execute) {
        try {
          task.run();
        } catch (const std::exception &error) {
          std::cerr << "File worker: " << error.what() << '\n';
          fail();
        }
      }
    }
    onWorker = false;
  });
}
inline bool submit(std::function<void()> task, size_t bytes = 0, bool mayWait = false) {
  if (active && failed)
    return false;
  if (!active || onWorker) {
    task();
    return true;
  }
  {
    std::unique_lock<std::mutex> lock(mutex);
    constexpr size_t Budget = 32 * 1024 * 1024;
    if (bytes > Budget) return false;
    auto room = [&] { return tasks.size() < 512 && bytes <= Budget - queuedBytes; };
    while (mayWait && !stopping && !failed && running && !room())
      wake.wait_for(lock, std::chrono::milliseconds(100));
    if (stopping || failed || !running || !room())
      return false;
    queuedBytes += bytes;
    tasks.push_back({std::move(task), bytes, false, nullptr});
  }
  wake.notify_all();
  return true;
}
inline void cleanup(std::function<void()> task, const void *key = nullptr) {
  if (!active || onWorker) {
    task();
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex);
    // Repeated local/remote selection invalidations are the unbounded producer:
    // coalesce them by cancellation target. Distinct cleanup objects originate
    // only from the 512-job work inbox, 128-result UI inbox, or active batch.
    if (key && std::any_of(tasks.begin(), tasks.end(),
                           [key](const Task &queued) { return queued.key == key; }))
      return;
    tasks.push_back({std::move(task), 0, true, key});
  }
  wake.notify_one();
}
inline void postUi(std::function<void()> task) {
  if (!active) {
    task();
    return;
  }
  if (!running || failed)
    return;
  {
    std::lock_guard<std::mutex> lock(uiMutex);
    if (!running || failed)
      return;
    if (completions.size() >= 128) {
      // Failure is made visible and stops new work. Destruction of the rejected
      // completion releases staging on this worker, not on the STA.
      fail();
      return;
    }
    completions.push_back(std::move(task));
  }
  if (notifyUi)
    notifyUi();
}
inline void drain() {
  std::deque<std::function<void()>> ready;
  {
    std::lock_guard<std::mutex> lock(uiMutex);
    ready.swap(completions);
  }
  for (auto &task : ready)
    task();
}
inline void stop() {
  if (!active)
    return;
  std::deque<std::function<void()>> abandoned;
  {
    std::lock_guard<std::mutex> lock(uiMutex);
    abandoned.swap(completions);
  }
  abandoned.clear(); // release staged results while cleanup is still accepted
  {
    std::lock_guard<std::mutex> lock(mutex);
    stopping = true;
  }
  wake.notify_all();
  if (worker.joinable())
    worker.join();
  active = false;
}
} // namespace FileIo
