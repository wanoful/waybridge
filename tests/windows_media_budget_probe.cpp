#include "../client/media_budget.h"
#include <cassert>
#include <thread>

int main() {
  using Cost = WindowMediaBudget::Cost;
  WindowMediaBudget budget({{200, 200, 3}, {120, 150, 2}, {80, 100, 1}});
  constexpr uint32_t front = 1, other = 2, newcomer = 3;
  budget.focus(front);
  auto frontDecoder = budget.acquire(front, {0, 50, 1});
  auto frontPicture = budget.acquire(front, {0, 50, 0});
  auto otherPicture = budget.acquire(other, {20, 20, 0});
  auto otherDecoder = budget.acquire(other, {0, 80, 1});
  assert(frontDecoder && frontPicture && otherPicture && otherDecoder);
  assert(!budget.acquire(newcomer, {0, 1, 0}));
  assert(!budget.acquire(front, {0, 1, 0}));
  assert(!budget.acquire(front, {121, 0, 0}));
  assert(!budget.acquire(front, {0, 0, 2}));

  // A sample can hold the old decoder's allocation after it leaves the cache.
  auto decoder = std::make_shared<WindowMediaBudget::Lease>(std::move(frontDecoder));
  auto sample = decoder;
  decoder.reset();
  const Cost nextPicture{0, 45, 0};
  budget.defer(other, nextPicture, 12);
  budget.defer(other, {0, 80, 0}, 10);
  budget.presented(other, 11);
  assert(!budget.takeRefresh(other));
  budget.focus(other);
  assert(!budget.takeRefresh(other));
  std::thread releaseSample([sample = std::move(sample)]() mutable { sample.reset(); });
  releaseSample.join();
  assert(budget.takeRefresh(other));
  assert(!budget.takeRefresh(other));
  auto resizedPicture = budget.acquire(other, nextPicture);
  assert(resizedPicture);

  // A newer picture or window close cancels a deferred refresh.
  budget.defer(other, {1, 0, 0}, 13);
  budget.presented(other, 13);
  assert(!budget.takeRefresh(other));
  budget.defer(newcomer, {1, 0, 0}, 14);
  budget.clearDeferred(newcomer);
  assert(!budget.takeRefresh(newcomer));

  otherDecoder.reset();
  otherPicture.reset();
  frontPicture.reset();
  resizedPicture.reset();
  const auto stats = budget.snapshot();
  assert(stats.used.empty());
  assert(stats.highWater.gpu == 200);
  assert(stats.highWater.cpu == 20);
  assert(stats.highWater.decoders == 2);
  assert(stats.denied == 4);

  // Switching focus preserves existing allocations while reserving new capacity.
  WindowMediaBudget switched({{20, 30, 3}, {20, 30, 3}, {10, 10, 1}});
  auto beforeFocus = switched.acquire(front, {13, 25, 2});
  assert(beforeFocus);
  switched.focus(other);
  assert(!switched.acquire(front, {1, 0, 0}));
  auto active = switched.acquire(other, {7, 5, 1});
  assert(active);
  assert(switched.snapshot().used.gpu == 30);

  // Another background picture and decoder stay resident across an HDR resize.
  WindowMediaBudget hdr;
  constexpr uint64_t pixels = uint64_t(4096) * 2304;
  hdr.focus(front);
  auto background = hdr.acquire(other, {0, 380 * WindowMediaBudget::MiB, 1});
  auto currentDecoder = hdr.acquire(front, WindowMediaBudget::decoderCost(pixels, true));
  auto displayedPair = hdr.acquire(front, WindowMediaBudget::swapchainCost(pixels, true));
  auto upload = hdr.acquire(front, WindowMediaBudget::videoTextureCost(pixels, true));
  auto mask = hdr.acquire(front, {0, pixels * 2, 0});
  auto nextPair = hdr.acquire(front, WindowMediaBudget::swapchainCost(pixels, true));
  assert(background && currentDecoder && displayedPair && upload && mask && nextPair);
  assert(hdr.snapshot().highWater.gpu == 1017 * WindowMediaBudget::MiB);
}
