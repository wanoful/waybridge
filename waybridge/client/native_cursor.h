#pragma once
#include <windows.h>
#include "third_party/json.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// One seat, one latest bitmap, one owned HCURSOR. System cursors are shared.
class NativeCursor {
  using Json = nlohmann::json;
  static constexpr int MaxEdge = 256;
  uint32_t root = 0;
  uint64_t generation = 0;
  std::string kind = "named", name = "default";
  int width = 0, height = 0, hotspotX = 0, hotspotY = 0;
  double sourceScale = 1;
  std::vector<uint8_t> rgba;
  HCURSOR cached = nullptr;
  bool owned = false;
  UINT cachedDpi = 0;

  void release() {
    if (owned && cached) {
      if (GetCursor() == cached)
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
      DestroyCursor(cached);
    }
    cached = nullptr;
    owned = false;
    cachedDpi = 0;
  }
  void replace(HCURSOR next, bool nextOwned, UINT dpi) {
    HCURSOR previous = cached;
    bool previousOwned = owned;
    // Keep the displayed bitmap alive until its replacement is ready, so an
    // animation update never briefly substitutes the system arrow.
    if (previousOwned && GetCursor() == previous)
      SetCursor(next);
    cached = next;
    owned = nextOwned;
    cachedDpi = dpi;
    if (previousOwned && previous && previous != next)
      DestroyCursor(previous);
  }
  static LPCWSTR systemName(const std::string &name) {
    if (name == "text" || name == "vertical-text")
      return IDC_IBEAM;
    if (name == "pointer" || name == "grab" || name == "grabbing")
      return IDC_HAND;
    if (name == "wait")
      return IDC_WAIT;
    if (name == "progress")
      return IDC_APPSTARTING;
    if (name == "crosshair" || name == "cell")
      return IDC_CROSS;
    if (name == "help")
      return IDC_HELP;
    if (name == "not-allowed" || name == "no-drop")
      return IDC_NO;
    if (name == "move" || name == "all-scroll")
      return IDC_SIZEALL;
    if (name == "e-resize" || name == "w-resize" || name == "ew-resize" || name == "col-resize")
      return IDC_SIZEWE;
    if (name == "n-resize" || name == "s-resize" || name == "ns-resize" || name == "row-resize")
      return IDC_SIZENS;
    if (name == "ne-resize" || name == "sw-resize" || name == "nesw-resize")
      return IDC_SIZENESW;
    if (name == "nw-resize" || name == "se-resize" || name == "nwse-resize")
      return IDC_SIZENWSE;
    return IDC_ARROW; // No exact Win32 equivalents for zoom, copy, alias, etc.
  }

public:
  NativeCursor() = default;
  NativeCursor(const NativeCursor &) = delete;
  NativeCursor &operator=(const NativeCursor &) = delete;
  ~NativeCursor() { release(); }

  bool update(const Json &m) {
    try {
      if (!m.contains("id") || !m.at("id").is_number_unsigned() ||
          m.at("id").get<uint64_t>() > UINT32_MAX || !m.contains("generation") ||
          !m.at("generation").is_number_unsigned())
        return false;
      uint64_t next = m.at("generation").get<uint64_t>();
      if (next <= generation)
        return false;
      std::string nextKind = m.value("kind", "");
      std::string nextName = m.value("name", "default");
      std::vector<uint8_t> pixels;
      int w = 0, h = 0, hx = 0, hy = 0;
      double scale = 1;
      if (nextKind == "bitmap") {
        if (m.value("format", "") != "rgba8-premultiplied")
          return false;
        // Validate as wide integers before narrowing or multiplication.
        auto integer = [&](const char *key, int limit) {
          if (!m.contains(key) || !m.at(key).is_number_integer())
            return -1;
          int64_t v = m.at(key).get<int64_t>();
          return v >= 0 && v <= limit ? int(v) : -1;
        };
        w = integer("width", MaxEdge);
        h = integer("height", MaxEdge);
        hx = integer("hotspot_x", MaxEdge - 1);
        hy = integer("hotspot_y", MaxEdge - 1);
        scale = m.value("scale", 0.0);
        if (w <= 0 || h <= 0 || hx < 0 || hy < 0 || hx >= w || hy >= h || !std::isfinite(scale) ||
            scale < 1 || scale > 4)
          return false;
        const auto &text = m.at("pixels").get_ref<const std::string &>();
        size_t expected = size_t(w) * h * 4;
        if (text.size() != ((expected + 2) / 3) * 4)
          return false;
        auto digit = [](unsigned char c) {
          if (c >= 'A' && c <= 'Z')
            return int(c - 'A');
          if (c >= 'a' && c <= 'z')
            return int(c - 'a') + 26;
          if (c >= '0' && c <= '9')
            return int(c - '0') + 52;
          return c == '+' ? 62 : c == '/' ? 63 : -1;
        };
        pixels.resize(expected);
        size_t out = 0;
        for (size_t i = 0; i < text.size(); i += 4) {
          int a = digit(text[i]), b = digit(text[i + 1]);
          int c = text[i + 2] == '=' ? 0 : digit(text[i + 2]);
          int d = text[i + 3] == '=' ? 0 : digit(text[i + 3]);
          size_t count = std::min(size_t(3), expected - out);
          if (a < 0 || b < 0 || c < 0 || d < 0 ||
              (count == 3 && (text[i + 2] == '=' || text[i + 3] == '=')) ||
              (count == 2 && (text[i + 2] == '=' || text[i + 3] != '=' || (c & 3))) ||
              (count == 1 && (text[i + 2] != '=' || text[i + 3] != '=' || (b & 15))))
            return false;
          uint32_t value =
              (uint32_t(a) << 18) | (uint32_t(b) << 12) | (uint32_t(c) << 6) | uint32_t(d);
          pixels[out++] = uint8_t(value >> 16);
          if (count > 1)
            pixels[out++] = uint8_t(value >> 8);
          if (count > 2)
            pixels[out++] = uint8_t(value);
        }
      } else if (nextKind != "named" && nextKind != "hidden")
        return false;
      if (nextName.size() > 64)
        return false;
      cachedDpi = 0;
      root = m.at("id").get<uint32_t>();
      generation = next;
      kind = std::move(nextKind);
      name = std::move(nextName);
      width = w;
      height = h;
      hotspotX = hx;
      hotspotY = hy;
      sourceScale = scale;
      rgba = std::move(pixels);
      return true;
    } catch (const Json::exception &) {
      return false;
    }
  }

  HCURSOR handle(uint32_t windowRoot, UINT dpi) {
    if (!root || root != windowRoot)
      return LoadCursorW(nullptr, IDC_ARROW);
    if (kind == "hidden") {
      replace(nullptr, false, dpi);
      return nullptr;
    }
    dpi = std::clamp<UINT>(dpi, 96, 384);
    if (cached && cachedDpi == dpi)
      return cached;
    if (kind == "named") {
      HCURSOR next = static_cast<HCURSOR>(LoadImageW(
          nullptr, systemName(name), IMAGE_CURSOR, GetSystemMetricsForDpi(SM_CXCURSOR, dpi),
          GetSystemMetricsForDpi(SM_CYCURSOR, dpi), LR_SHARED));
      if (!next)
        next = LoadCursorW(nullptr, IDC_ARROW);
      replace(next, false, dpi);
      return cached;
    }
    double scale = double(dpi) / 96 / sourceScale;
    scale = std::min(scale, double(MaxEdge) / std::max(width, height));
    int w = std::clamp(int(std::lround(width * scale)), 1, MaxEdge);
    int h = std::clamp(int(std::lround(height * scale)), 1, MaxEdge);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HBITMAP color = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    std::vector<uint8_t> maskBytes(size_t((w + 15) / 16) * 2 * h, 0);
    HBITMAP mask = nullptr;
    HCURSOR next = nullptr;
    if (color && bits) {
      auto out = static_cast<uint8_t *>(bits);
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          size_t src = (size_t(y * height / h) * width + x * width / w) * 4;
          size_t dst = (size_t(y) * w + x) * 4;
          out[dst] = rgba[src + 2];
          out[dst + 1] = rgba[src + 1];
          out[dst + 2] = rgba[src];
          out[dst + 3] = rgba[src + 3];
          if (!rgba[src + 3])
            maskBytes[size_t(y) * ((w + 15) / 16) * 2 + x / 8] |= uint8_t(0x80 >> (x % 8));
        }
      // Win32 alpha cursors take premultiplied BGRA; also supply a valid AND
      // mask for fully transparent images and legacy rendering paths.
      // https://learn.microsoft.com/windows/win32/menurc/using-cursors#creating-an-alpha-blended-cursor
      mask = CreateBitmap(w, h, 1, 1, maskBytes.data());
      ICONINFO icon{};
      icon.fIcon = FALSE;
      icon.hbmColor = color;
      icon.hbmMask = mask;
      icon.xHotspot = std::min(w - 1, int(std::lround(double(hotspotX) * w / width)));
      icon.yHotspot = std::min(h - 1, int(std::lround(double(hotspotY) * h / height)));
      if (mask)
        next = static_cast<HCURSOR>(CreateIconIndirect(&icon));
    }
    if (color)
      DeleteObject(color);
    if (mask)
      DeleteObject(mask);
    bool nextOwned = next != nullptr;
    if (!next)
      next = LoadCursorW(nullptr, IDC_ARROW);
    replace(next, nextOwned, dpi);
    return cached;
  }
};
