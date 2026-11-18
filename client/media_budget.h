#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

class WindowMediaBudget {
public:
  static constexpr uint64_t MiB = 1024 * 1024;
  struct Cost {
    uint64_t cpu = 0, gpu = 0, decoders = 0;

    bool fits(Cost cap) const {
      return cpu <= cap.cpu && gpu <= cap.gpu && decoders <= cap.decoders;
    }
    Cost operator+(Cost b) const { return {cpu + b.cpu, gpu + b.gpu, decoders + b.decoders}; }
    Cost operator-(Cost b) const { return {cpu - b.cpu, gpu - b.gpu, decoders - b.decoders}; }
    bool empty() const { return cpu == 0 && gpu == 0 && decoders == 0; }
  };
  static Cost decoderCost(uint64_t pixels, bool hdr) {
    return {0, pixels * (hdr ? 32 : 16) + 16 * MiB, 1};
  }
  static Cost swapchainCost(uint64_t pixels, bool hdr) {
    return {0, pixels * (hdr ? 16 : 8), 0};
  }
  static Cost videoTextureCost(uint64_t pixels, bool hdr) {
    return {0, pixels * (hdr ? 6 : 3) / 2, 0};
  }
  struct Limits {
    Cost total{512 * MiB, 1024 * MiB, 8};
    Cost window{256 * MiB, 720 * MiB, 2};
    Cost foregroundReserve{128 * MiB, 512 * MiB, 1};
  };
  struct Snapshot {
    Cost used, highWater;
    uint64_t denied = 0;
  };

private:
  struct Account {
    struct Demand {
      Cost cost;
      uint64_t sequence = 0;
    };
    explicit Account(Limits configured) : limits(configured) {}
    std::mutex mutex;
    Limits limits;
    Cost used, highWater;
    std::map<uint32_t, Cost> byWindow;
    std::map<uint32_t, Demand> deferred;
    uint32_t foreground = 0;
    uint64_t denied = 0;

    Cost window(uint32_t id) const {
      auto it = byWindow.find(id);
      return it == byWindow.end() ? Cost{} : it->second;
    }
    bool canAdmit(uint32_t id, Cost cost) const {
      if (!cost.fits(limits.total - used) || !cost.fits(limits.window - window(id)))
        return false;
      if (!foreground || id == foreground)
        return true;
      const auto backgroundCap = limits.total - limits.foregroundReserve;
      return (used - window(foreground) + cost).fits(backgroundCap);
    }
  };
  std::shared_ptr<Account> account;

public:
  class Lease {
    friend class WindowMediaBudget;
    std::shared_ptr<Account> account;
    uint32_t id = 0;
    Cost cost;

    Lease(std::shared_ptr<Account> owner, uint32_t window, Cost value)
        : account(std::move(owner)), id(window), cost(value) {}

  public:
    Lease() = default;
    Lease(const Lease &) = delete;
    Lease &operator=(const Lease &) = delete;
    Lease(Lease &&) noexcept = default;
    Lease &operator=(Lease &&next) noexcept {
      if (this != &next) {
        reset();
        account = std::move(next.account);
        id = next.id;
        cost = next.cost;
      }
      return *this;
    }
    ~Lease() { reset(); }
    explicit operator bool() const { return account != nullptr; }
    void reset() {
      auto owner = std::move(account);
      if (!owner) return;
      std::lock_guard<std::mutex> lock(owner->mutex);
      owner->used = owner->used - cost;
      auto it = owner->byWindow.find(id);
      it->second = it->second - cost;
      if (it->second.empty()) owner->byWindow.erase(it);
    }
  };

  WindowMediaBudget() : WindowMediaBudget(Limits{}) {}
  explicit WindowMediaBudget(Limits limits) : account(std::make_shared<Account>(limits)) {}

  Lease acquire(uint32_t id, Cost cost) {
    if (cost.empty()) return {};
    std::lock_guard<std::mutex> lock(account->mutex);
    if (!account->canAdmit(id, cost)) {
      ++account->denied;
      return {};
    }
    auto &local = account->byWindow[id];
    local = local + cost;
    auto &used = account->used;
    used = used + cost;
    auto &peak = account->highWater;
    peak = {std::max(peak.cpu, used.cpu), std::max(peak.gpu, used.gpu),
            std::max(peak.decoders, used.decoders)};
    return Lease(account, id, cost);
  }
  bool fitsLimits(Cost cost) const {
    return cost.fits(account->limits.total) && cost.fits(account->limits.window);
  }
  bool fitsRemainingWindow(uint32_t id, Cost cost) const {
    std::lock_guard<std::mutex> lock(account->mutex);
    return cost.fits(account->limits.window - account->window(id));
  }
  void focus(uint32_t id) {
    std::lock_guard<std::mutex> lock(account->mutex);
    account->foreground = id;
  }
  uint32_t focused() const {
    std::lock_guard<std::mutex> lock(account->mutex);
    return account->foreground;
  }
  void defer(uint32_t id, Cost cost, uint64_t sequence) {
    if (!fitsLimits(cost)) return;
    std::lock_guard<std::mutex> lock(account->mutex);
    auto &demand = account->deferred[id];
    if (sequence >= demand.sequence) demand = {cost, sequence};
  }
  void clearDeferred(uint32_t id) {
    std::lock_guard<std::mutex> lock(account->mutex);
    account->deferred.erase(id);
  }
  void presented(uint32_t id, uint64_t sequence) {
    std::lock_guard<std::mutex> lock(account->mutex);
    auto it = account->deferred.find(id);
    if (it != account->deferred.end() && it->second.sequence <= sequence)
      account->deferred.erase(it);
  }
  bool takeRefresh(uint32_t id) {
    std::lock_guard<std::mutex> lock(account->mutex);
    auto it = account->deferred.find(id);
    if (it == account->deferred.end() || !account->canAdmit(id, it->second.cost)) return false;
    account->deferred.erase(it);
    return true;
  }
  Snapshot snapshot() const {
    std::lock_guard<std::mutex> lock(account->mutex);
    return {account->used, account->highWater, account->denied};
  }
};
