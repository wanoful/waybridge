#pragma once
#include <windows.h>
#include <algorithm>
#include <cmath>

inline RECT fitImageRect(RECT client, int width, int height, bool fullscreen, UINT dpi) {
  if (width <= 0 || height <= 0)
    return client;
  // X11's integer logical root can round a fullscreen surface down by one
  // logical pixel. Fill that quantization gap, retaining letterboxing for
  // genuinely different game resolutions/aspect ratios.
  int tolerance = (int(dpi) + 95) / 96;
  if (fullscreen && std::abs(width - client.right) <= tolerance &&
      std::abs(height - client.bottom) <= tolerance)
    return client;
  double scale = std::min(double(client.right) / width, double(client.bottom) / height);
  int w = std::min(client.right, LONG(std::lround(width * scale))),
      h = std::min(client.bottom, LONG(std::lround(height * scale)));
  int left = (client.right - w) / 2, top = (client.bottom - h) / 2;
  return {left, top, left + w, top + h};
}
