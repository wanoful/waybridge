#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>

// Called under the playback device's queue lock. Counts are interleaved stereo samples.
class AudioPlayout {
public:
  static constexpr std::size_t SamplesPerMs = 48 * 2;
  static constexpr std::size_t PrimeSamples = 60 * SamplesPerMs;
  static constexpr std::size_t MaxSamples = 100 * SamplesPerMs;

  void push(const int16_t *samples, std::size_t count) {
    if (!count || count % 2 || count > MaxSamples)
      return;
    if (queued.size() + count > MaxSamples) {
      const auto excess = queued.size() + count - std::max(PrimeSamples, count);
      discardedSamples += excess;
      queued.erase(queued.begin(), queued.begin() + excess);
    }
    queued.insert(queued.end(), samples, samples + count);
  }

  std::size_t read(int16_t *target, std::size_t count, float gain) {
    // Leave the jitter reserve in the queue after filling the first device buffer.
    if (!started && queued.size() < std::min(MaxSamples, PrimeSamples + count))
      return 0;
    started = true;
    const auto available = std::min(count, queued.size());
    for (std::size_t i = 0; i < available; ++i) {
      target[i] = int16_t(float(queued.front()) * gain);
      queued.pop_front();
    }
    if (available < count) {
      ++underruns;
      started = false;
    }
    return available;
  }

  void rebuffer() { started = false; }
  void clear() {
    queued.clear();
    rebuffer();
  }
  std::size_t bufferedSamples() const { return queued.size(); }
  uint64_t underrunCount() const { return underruns; }
  uint64_t discardedSampleCount() const { return discardedSamples; }

private:
  std::deque<int16_t> queued;
  bool started = false;
  uint64_t underruns = 0, discardedSamples = 0;
};
