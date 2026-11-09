#pragma once
#include <windows.h>
#include "image_decoder.h"
#include "third_party/json.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// One normalized 128px master, two owned icons per remote toplevel.
class NativeIcon {
  std::vector<uint8_t> pixels;
  HICON large = nullptr, small = nullptr;
  bool ownLarge = false, ownSmall = false;
  UINT lastDpi = 0;

  HICON create(int edge) const {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = edge;
    bi.bmiHeader.biHeight = -edge;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HBITMAP color = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color || !bits) {
      if (color)
        DeleteObject(color);
      return nullptr;
    }
    size_t stride = size_t((edge + 15) / 16) * 2;
    std::vector<uint8_t> andMask(stride * edge, 0);
    auto out = static_cast<uint8_t *>(bits);
    for (int y = 0; y < edge; ++y)
      for (int x = 0; x < edge; ++x) {
        double sx = std::clamp((x + .5) * 128 / edge - .5, 0.0, 127.0),
               sy = std::clamp((y + .5) * 128 / edge - .5, 0.0, 127.0);
        int x0 = int(sx), y0 = int(sy), x1 = std::min(127, x0 + 1), y1 = std::min(127, y0 + 1);
        double fx = sx - x0, fy = sy - y0;
        for (int c = 0; c < 4; ++c) {
          double top =
              pixels[(y0 * 128 + x0) * 4 + c] * (1 - fx) + pixels[(y0 * 128 + x1) * 4 + c] * fx;
          double bottom =
              pixels[(y1 * 128 + x0) * 4 + c] * (1 - fx) + pixels[(y1 * 128 + x1) * 4 + c] * fx;
          out[(y * edge + x) * 4 + c] = uint8_t(std::lround(top * (1 - fy) + bottom * fy));
        }
        if (!out[(y * edge + x) * 4 + 3])
          andMask[size_t(y) * stride + x / 8] |= uint8_t(0x80 >> (x % 8));
      }
    HBITMAP mask = CreateBitmap(edge, edge, 1, 1, andMask.data());
    ICONINFO icon{};
    icon.fIcon = TRUE;
    icon.hbmColor = color;
    icon.hbmMask = mask;
    HICON result = mask ? CreateIconIndirect(&icon) : nullptr;
    DeleteObject(color);
    if (mask)
      DeleteObject(mask);
    return result;
  }
  void release() {
    if (ownLarge && large)
      DestroyIcon(large);
    if (ownSmall && small && small != large)
      DestroyIcon(small);
    large = small = nullptr;
    ownLarge = ownSmall = false;
  }

public:
  NativeIcon() = default;
  NativeIcon(const NativeIcon &) = delete;
  NativeIcon &operator=(const NativeIcon &) = delete;
  ~NativeIcon() { release(); }
  static std::string appId(const std::string &id) {
    if (id.empty() || id.size() > 255 || id == "." || id == "..")
      return {};
    for (unsigned char c : id)
      if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == '-'))
        return {};
    return id;
  }
  static bool matches(const nlohmann::json &message, uint32_t id, const std::string &appId,
                      uint64_t revision) {
    if (!message.is_object() || !message.contains("id") || !message.at("id").is_number_unsigned() ||
        message.at("id").get<uint64_t>() != id || !message.contains("revision") ||
        !message.at("revision").is_number_unsigned() || !revision ||
        message.at("revision").get<uint64_t>() != revision || !message.contains("app_id") ||
        !message.at("app_id").is_string() ||
        message.at("app_id").get_ref<const std::string &>() != appId || !message.contains("png"))
      return false;
    if (message.at("png").is_null())
      return true;
    return message.at("png").is_string() &&
           message.at("png").get_ref<const std::string &>().size() <= 128 * 1024 &&
           message.contains("width") && message.at("width").is_number_integer() &&
           message.at("width") == 128 && message.contains("height") &&
           message.at("height").is_number_integer() && message.at("height") == 128;
  }
  bool update(const std::vector<uint8_t> &png, ImageDecoder &decoder) {
    if (png.empty() || png.size() > 96 * 1024)
      return false;
    std::vector<uint8_t> decoded;
    if (!decoder.decode(png.data(), png.size(), 128, 128, false, decoded))
      return false;
    pixels = std::move(decoded);
    lastDpi = 0;
    return true;
  }
  void clear() {
    pixels.clear();
    lastDpi = 0;
  }
  void apply(HWND hwnd, UINT dpi) {
    dpi = std::clamp<UINT>(dpi, 96, 384);
    if (lastDpi == dpi)
      return;
    int bigSize = std::clamp(GetSystemMetricsForDpi(SM_CXICON, dpi), 1, 128);
    int smallSize = std::clamp(GetSystemMetricsForDpi(SM_CXSMICON, dpi), 1, 128);
    HICON nextLarge = pixels.empty() ? nullptr : create(bigSize),
          nextSmall = pixels.empty() ? nullptr : create(smallSize);
    bool nextOwnLarge = nextLarge != nullptr, nextOwnSmall = nextSmall != nullptr;
    if (!nextLarge)
      nextLarge = static_cast<HICON>(
          LoadImageW(nullptr, IDI_APPLICATION, IMAGE_ICON, bigSize, bigSize, LR_SHARED));
    if (!nextSmall)
      nextSmall = static_cast<HICON>(
          LoadImageW(nullptr, IDI_APPLICATION, IMAGE_ICON, smallSize, smallSize, LR_SHARED));
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(nextLarge));
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(nextSmall));
    if (GetEnvironmentVariableW(L"WAYBRIDGE_ICON_TRACE", nullptr, 0))
      std::fprintf(stderr, "Icon apply dpi=%u large=%p small=%p getLarge=%p getSmall=%p\n", dpi,
                   static_cast<void *>(nextLarge), static_cast<void *>(nextSmall),
                   reinterpret_cast<void *>(SendMessageW(hwnd, WM_GETICON, ICON_BIG, 0)),
                   reinterpret_cast<void *>(SendMessageW(hwnd, WM_GETICON, ICON_SMALL, 0)));
    release();
    large = nextLarge;
    small = nextSmall;
    ownLarge = nextOwnLarge;
    ownSmall = nextOwnSmall;
    lastDpi = dpi;
  }
};
