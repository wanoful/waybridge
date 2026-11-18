#pragma once
#include <windows.h>
#include <algorithm>
#include <array>
#include <cmath>

inline constexpr COLORREF ResizePreviewColor = RGB(40, 40, 40);

inline RECT resizePreviewImage(RECT client, int width, int height, UINT dpi) {
  return {client.left, client.top, client.left + MulDiv(width, dpi, 96),
          client.top + MulDiv(height, dpi, 96)};
}

inline std::array<RECT, 2> resizePreviewFill(RECT client, RECT image) {
  LONG right = std::clamp(image.right, client.left, client.right);
  LONG bottom = std::clamp(image.bottom, client.top, client.bottom);
  return {{{right, client.top, client.right, bottom},
           {client.left, bottom, client.right, client.bottom}}};
}

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
