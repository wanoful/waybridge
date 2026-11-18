#pragma once
#include "branding_assets.h"
#include "image_decoder.h"
#include <algorithm>
#include <cmath>

// Badges are composed for remote Windows window and tray icons. Masters remain
// unmodified; all composition happens at the final destination's pixel size.
namespace IconBadge {
struct State {
  std::vector<uint8_t> custom;
  uint64_t revision = 1;
};
inline State &state() { static State value; return value; }
inline const std::vector<uint8_t> &defaultPixels(bool placeholder = false) {
  static std::vector<uint8_t> badge, fallback;
  auto &value = placeholder ? fallback : badge;
  if (value.empty()) {
    ImageDecoder decoder;
    if (placeholder)
      decoder.decode(BrandingAssets::placeholder, sizeof(BrandingAssets::placeholder), 128, 128, false, value);
    else
      decoder.decode(BrandingAssets::badge, sizeof(BrandingAssets::badge), 64, 64, false, value);
  }
  return value;
}
inline bool configure(const std::vector<uint8_t> &png, ImageDecoder &decoder) {
  std::vector<uint8_t> next;
  if (!png.empty() && (png.size() > 24 * 1024 || !decoder.decode(png.data(), png.size(), 64, 64, false, next)))
    return false;
  if (next != state().custom) {
    state().custom = std::move(next);
    ++state().revision;
  }
  return true;
}
inline std::vector<uint8_t> resize(const std::vector<uint8_t> &source, int sourceEdge, int edge) {
  std::vector<uint8_t> out(size_t(edge) * edge * 4);
  if (source.size() != size_t(sourceEdge) * sourceEdge * 4) return out;
  for (int y = 0; y < edge; ++y) for (int x = 0; x < edge; ++x) {
    double sx = std::clamp((x + .5) * sourceEdge / edge - .5, 0.0, double(sourceEdge - 1));
    double sy = std::clamp((y + .5) * sourceEdge / edge - .5, 0.0, double(sourceEdge - 1));
    int x0 = int(sx), y0 = int(sy), x1 = std::min(sourceEdge - 1, x0 + 1), y1 = std::min(sourceEdge - 1, y0 + 1);
    double fx = sx - x0, fy = sy - y0;
    for (int c = 0; c < 4; ++c) {
      double top = source[(y0 * sourceEdge + x0) * 4 + c] * (1 - fx) + source[(y0 * sourceEdge + x1) * 4 + c] * fx;
      double bottom = source[(y1 * sourceEdge + x0) * 4 + c] * (1 - fx) + source[(y1 * sourceEdge + x1) * 4 + c] * fx;
      out[(y * edge + x) * 4 + c] = uint8_t(std::lround(top * (1 - fy) + bottom * fy));
    }
  }
  return out;
}
inline void over(uint8_t *target, const uint8_t *source) {
  unsigned inverse = 255 - source[3];
  for (int c = 0; c < 4; ++c) target[c] = uint8_t(source[c] + (target[c] * inverse + 127) / 255);
}
inline std::vector<uint8_t> compose(const std::vector<uint8_t> &master, int edge) {
  edge = std::clamp(edge, 8, 128);
  auto out = resize(master.empty() ? defaultPixels(true) : master, 128, edge);
  int size = std::clamp(int(std::lround(edge * .40)), 6, edge - 2);
  auto badge = resize(state().custom.empty() ? defaultPixels() : state().custom, 64, size);
  int left = edge - size - 1, top = 1;
  // A one-pixel separator follows alpha, so custom non-circular badges retain
  // their own shape. Source pixels and cached masters are never changed.
  for (int y = -1; y <= size; ++y) for (int x = -1; x <= size; ++x) {
    uint8_t alpha = 0;
    for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
      int sx = x + dx, sy = y + dy;
      if (sx >= 0 && sy >= 0 && sx < size && sy < size) alpha = std::max(alpha, badge[(sy * size + sx) * 4 + 3]);
    }
    uint8_t white[] = {alpha, alpha, alpha, alpha};
    over(out.data() + ((y + top) * edge + x + left) * 4, white);
  }
  for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x)
    over(out.data() + ((y + top) * edge + x + left) * 4, badge.data() + (y * size + x) * 4);
  return out;
}
inline std::vector<uint8_t> png(const std::vector<uint8_t> &pixels, UINT edge) {
  if (pixels.size() != size_t(edge) * edge * 4) return {};
  Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
  Microsoft::WRL::ComPtr<IStream> stream;
  Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
  Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
      FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) ||
      FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
      FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
      FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
      FAILED(frame->SetSize(edge, edge))) return {};
  auto straight = pixels;
  for (size_t i = 0; i < straight.size(); i += 4) for (int c = 0; c < 3; ++c)
    straight[i + c] = pixels[i + 3] ? uint8_t(std::min(255u, (unsigned(pixels[i + c]) * 255 + pixels[i + 3] / 2) / pixels[i + 3])) : 0;
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  if (FAILED(frame->SetPixelFormat(&format)) || format != GUID_WICPixelFormat32bppBGRA ||
      FAILED(frame->WritePixels(edge, edge * 4, UINT(straight.size()), straight.data())) ||
      FAILED(frame->Commit()) || FAILED(encoder->Commit())) return {};
  STATSTG stat{};
  if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart > 96 * 1024) return {};
  LARGE_INTEGER start{};
  if (FAILED(stream->Seek(start, STREAM_SEEK_SET, nullptr))) return {};
  std::vector<uint8_t> result(size_t(stat.cbSize.QuadPart));
  ULONG read = 0;
  if (FAILED(stream->Read(result.data(), ULONG(result.size()), &read)) || read != result.size()) return {};
  return result;
}
}
