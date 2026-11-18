#pragma once
#include "h264_decoder.h"
#include <algorithm>
#include <memory>
#include <vector>

// The decode thread shares a small pool across windows. Keep common restore /
// maximized sizes warm without accumulating a decoder for every drag position.
class VideoDecoderCache {
  std::vector<std::unique_ptr<H264Decoder>> idle;

public:
  void recycle(std::unique_ptr<H264Decoder> decoder) {
    if (!decoder || decoder->pixels() > 8 * 1024 * 1024)
      return;
    idle.push_back(std::move(decoder));
    uint64_t pixels = 0;
    for (const auto &entry : idle)
      pixels += entry->pixels();
    while (idle.size() > 4 || pixels > 8 * 1024 * 1024) {
      pixels -= idle.front()->pixels();
      idle.erase(idle.begin());
    }
  }
  std::unique_ptr<H264Decoder> acquire(UINT width, UINT height, bool &reused, uint8_t codec = 2,
                                      uint32_t owner = 0, WindowMediaBudget *budget = nullptr) {
    for (auto it = idle.begin(); it != idle.end(); ++it) {
      if ((*it)->owner() != owner || !(*it)->matches(width, height, codec))
        continue;
      auto decoder = std::move(*it);
      idle.erase(it);
      // Each epoch starts with codec headers and an independent keyframe. Discard old state and
      // timestamps while retaining the decoder's allocated hardware surfaces.
      decoder->restart();
      reused = true;
      return decoder;
    }
    reused = false;
    std::shared_ptr<WindowMediaBudget::Lease> lease;
    if (budget) {
      const auto cost = VideoDecoder::estimatedCost(width, height, codec);
      auto reservation = budget->acquire(owner, cost);
      if (!reservation && !idle.empty()) {
        clear();
        reservation = budget->acquire(owner, cost);
      }
      if (!reservation) return {};
      lease = std::make_shared<WindowMediaBudget::Lease>(std::move(reservation));
    }
    return std::make_unique<H264Decoder>(width, height, codec, owner, std::move(lease));
  }
  void clear() { idle.clear(); }
  void forget(uint32_t owner) {
    idle.erase(std::remove_if(idle.begin(), idle.end(), [owner](const auto &entry) {
                 return entry->owner() == owner;
               }), idle.end());
  }
};

using H264DecoderCache = VideoDecoderCache;
