#include "../client/audio_playout.h"

#include <array>
#include <cassert>
#include <iostream>

namespace {
using Block = std::array<int16_t, 10 * AudioPlayout::SamplesPerMs>;
constexpr int PrimeBlocks = AudioPlayout::PrimeSamples / Block{}.size();
constexpr int StartupBlocks = PrimeBlocks + 1;

void push(AudioPlayout &queue, int16_t sample) {
  Block block;
  block.fill(sample);
  queue.push(block.data(), block.size());
}
} // namespace

int main() {
  AudioPlayout queue;
  Block output{};
  int16_t expected = 1;
  for (int tick = 0; tick < 120; ++tick) {
    const int period = tick % 40;
    if (tick >= 40 && period == 5) {
      for (int held = tick - 5; held <= tick; ++held)
        push(queue, int16_t(held + 1));
    } else if (tick < 40 || period >= 5) {
      push(queue, int16_t(tick + 1));
    }
    if (tick < StartupBlocks - 1) {
      assert(queue.read(output.data(), output.size(), 1) == 0);
    } else {
      assert(queue.read(output.data(), output.size(), 1) == output.size());
      assert(output.front() == expected++);
    }
  }
  assert(queue.underrunCount() == 0);
  assert(queue.discardedSampleCount() == 0);

  while (queue.bufferedSamples())
    assert(queue.read(output.data(), output.size(), 1) == output.size());
  for (int tick = 0; tick < 4; ++tick)
    assert(queue.read(output.data(), output.size(), 1) == 0);
  assert(queue.underrunCount() == 1);
  for (int tick = 0; tick < StartupBlocks - 1; ++tick) {
    push(queue, 1000);
    assert(queue.read(output.data(), output.size(), 1) == 0);
  }
  push(queue, 1000);
  assert(queue.read(output.data(), output.size(), 0.5f) == output.size());
  assert(output.front() == 500);

  queue.clear();
  constexpr int OverflowBlocks = AudioPlayout::MaxSamples / Block{}.size() + 1;
  for (int tick = 0; tick < OverflowBlocks; ++tick)
    push(queue, int16_t(tick + 1));
  assert(queue.bufferedSamples() == AudioPlayout::PrimeSamples);
  assert(queue.discardedSampleCount() == (OverflowBlocks - PrimeBlocks) * output.size());
  push(queue, OverflowBlocks + 1);
  assert(queue.read(output.data(), output.size(), 1) == output.size());
  assert(output.front() == OverflowBlocks - PrimeBlocks + 1);
  std::cout << "PASS: 60 ms burst tolerance, underrun rebuffering, newest-audio trim\n";
}
