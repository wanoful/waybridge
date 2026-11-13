#pragma once
#include <windows.h>
#include "third_party/json.hpp"
#include <algorithm>
#include <cstdint>
#include <vector>

struct NativeWindowState {
  using Json = nlohmann::json;
  bool fullscreen = false, applying = false, metadataReady = false, mapped = true, modal = false;
  bool lastVisible = true, lastMinimized = false, stateSent = false;
  uint64_t fullscreenEpoch = 0, lastFullscreenEpoch = UINT64_MAX, mappingEpoch = 1;
  bool lastFullscreen = false;
  int minWidth = 0, minHeight = 0, maxWidth = 0, maxHeight = 0;
  WINDOWPLACEMENT placement{};
  struct Operation {
    bool add;
    int x, y, width, height;
  };
  struct Layer {
    int x, y, width, height;
    bool all;
    std::vector<Operation> operations;
  };
  std::vector<Layer> layers;
  bool regionValid = false;
  int inputOriginX = 0, inputOriginY = 0, inputWidth = 0, inputHeight = 0;
  NativeWindowState() { placement.length = sizeof(placement); }
  static bool integer(const Json &v, int &out) {
    if (!v.is_number_integer())
      return false;
    if (v.is_number_unsigned() && v.get<uint64_t>() > INT32_MAX)
      return false;
    auto number = v.get<int64_t>();
    if (number < INT32_MIN || number > INT32_MAX)
      return false;
    out = int(number);
    return true;
  }
  static int dimension(const Json &m, size_t index) {
    int value = 0;
    return m.is_array() && m.size() == 2 && integer(m[index], value) && value >= 0 && value <= 8192
               ? value
               : 0;
  }
  void constraints(const Json &m) {
    minWidth = dimension(m.value("min_size", Json()), 0);
    minHeight = dimension(m.value("min_size", Json()), 1);
    maxWidth = dimension(m.value("max_size", Json()), 0);
    maxHeight = dimension(m.value("max_size", Json()), 1);
    if (maxWidth)
      maxWidth = std::max(maxWidth, minWidth);
    if (maxHeight)
      maxHeight = std::max(maxHeight, minHeight);
    modal = m.value("modal", false);
  }
  bool setFullscreen(HWND hwnd, bool value, bool clientDecorated) {
    if (fullscreen == value)
      return false;
    applying = true;
    if (value) {
      GetWindowPlacement(hwnd, &placement);
      fullscreen = true;
      if (IsZoomed(hwnd) || IsIconic(hwnd))
        ShowWindow(hwnd, SW_RESTORE);
      DWORD style = DWORD(GetWindowLongPtrW(hwnd, GWL_STYLE));
      SetWindowLongPtrW(hwnd, GWL_STYLE, (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
      MONITORINFO monitor{};
      monitor.cbSize = sizeof(monitor);
      if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor))
        SetWindowPos(hwnd, nullptr, monitor.rcMonitor.left, monitor.rcMonitor.top,
                     monitor.rcMonitor.right - monitor.rcMonitor.left,
                     monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOZORDER);
    } else {
      fullscreen = false;
      DWORD style = clientDecorated
                        ? WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX
                        : WS_OVERLAPPEDWINDOW;
      bool visible = IsWindowVisible(hwnd) != FALSE, minimized = IsIconic(hwnd) != FALSE;
      style |= DWORD(GetWindowLongPtrW(hwnd, GWL_STYLE)) & (WS_VISIBLE | WS_DISABLED);
      SetWindowLongPtrW(hwnd, GWL_STYLE, style);
      auto restore = placement;
      if (!visible)
        restore.showCmd = SW_HIDE;
      else if (minimized) {
        restore.showCmd = SW_SHOWMINNOACTIVE;
        if (placement.showCmd == SW_SHOWMAXIMIZED)
          restore.flags |= WPF_RESTORETOMAXIMIZED;
      }
      SetWindowPlacement(hwnd, &restore);
      SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                   SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
    }
    applying = false;
    return true;
  }
  static SIZE outer(HWND hwnd, UINT dpi, bool clientDecorated, int width, int height) {
    RECT r{0, 0, width ? MulDiv(width, dpi, 96) : 0, height ? MulDiv(height, dpi, 96) : 0};
    if (!clientDecorated)
      AdjustWindowRectExForDpi(&r, DWORD(GetWindowLongPtrW(hwnd, GWL_STYLE)), FALSE,
                               DWORD(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)), dpi);
    return {r.right - r.left, r.bottom - r.top};
  }
  void minMax(HWND hwnd, UINT dpi, bool clientDecorated, MINMAXINFO *info) const {
    if (fullscreen)
      return;
    auto min = outer(hwnd, dpi, clientDecorated, minWidth, minHeight),
         max = outer(hwnd, dpi, clientDecorated, maxWidth, maxHeight);
    if (minWidth)
      info->ptMinTrackSize.x = min.cx;
    if (minHeight)
      info->ptMinTrackSize.y = min.cy;
    if (maxWidth) {
      info->ptMaxTrackSize.x = max.cx;
      info->ptMaxSize.x = std::min(info->ptMaxSize.x, max.cx);
    }
    if (maxHeight) {
      info->ptMaxTrackSize.y = max.cy;
      info->ptMaxSize.y = std::min(info->ptMaxSize.y, max.cy);
    }
    info->ptMinTrackSize.x = std::min(info->ptMinTrackSize.x, info->ptMaxTrackSize.x);
    info->ptMinTrackSize.y = std::min(info->ptMinTrackSize.y, info->ptMaxTrackSize.y);
  }
  void clamp(HWND hwnd, UINT dpi, bool clientDecorated, WINDOWPOS *position) const {
    if (fullscreen || applying || !metadataReady || (position->flags & SWP_NOSIZE))
      return;
    auto min = outer(hwnd, dpi, clientDecorated, minWidth, minHeight),
         max = outer(hwnd, dpi, clientDecorated, maxWidth, maxHeight);
    if (minWidth)
      position->cx = std::max(position->cx, int(min.cx));
    if (minHeight)
      position->cy = std::max(position->cy, int(min.cy));
    if (maxWidth)
      position->cx = std::min(position->cx, int(max.cx));
    if (maxHeight)
      position->cy = std::min(position->cy, int(max.cy));
  }
  void input(const Json &m) {
    regionValid = false;
    layers.clear();
    if (!m.is_object())
      return;
    auto number = [&](const Json &object, const char *key, int &out) {
      return object.contains(key) && integer(object.at(key), out);
    };
    if (!number(m, "origin_x", inputOriginX) || !number(m, "origin_y", inputOriginY) ||
        !number(m, "width", inputWidth) || !number(m, "height", inputHeight) || inputWidth < 1 ||
        inputHeight < 1 || inputWidth > 8192 || inputHeight > 8192 || !m.contains("layers") ||
        !m.at("layers").is_array() || m.at("layers").size() > 64)
      return;
    size_t operations = 0;
    for (auto &entry : m.at("layers")) {
      Layer layer{};
      if (!entry.is_object() || !number(entry, "x", layer.x) || !number(entry, "y", layer.y) ||
          !number(entry, "width", layer.width) || !number(entry, "height", layer.height) ||
          layer.width < 1 || layer.height < 1 || layer.width > 8192 || layer.height > 8192 ||
          !entry.contains("rects"))
        return;
      auto &rects = entry.at("rects");
      layer.all = rects.is_null();
      if (!layer.all) {
        if (!rects.is_array() || rects.size() > 256 - operations)
          return;
        operations += rects.size();
        for (auto &rect : rects) {
          Operation op{};
          if (!rect.is_array() || rect.size() != 5 || !rect[0].is_boolean() ||
              !integer(rect[1], op.x) || !integer(rect[2], op.y) || !integer(rect[3], op.width) ||
              !integer(rect[4], op.height) || op.width < 0 || op.height < 0)
            return;
          op.add = rect[0].get<bool>();
          layer.operations.push_back(op);
        }
      }
      layers.push_back(std::move(layer));
    }
    regionValid = true;
  }
  bool accepts(double x, double y, int originX, int originY, int width, int height) const {
    if (!regionValid || originX != inputOriginX || originY != inputOriginY || width != inputWidth ||
        height != inputHeight)
      return true;
    x += originX;
    y += originY;
    for (auto &layer : layers) {
      double lx = x - layer.x, ly = y - layer.y;
      if (lx < 0 || ly < 0 || lx >= layer.width || ly >= layer.height)
        continue;
      bool hit = layer.all;
      for (auto &op : layer.operations)
        if (lx >= op.x && ly >= op.y && lx < double(op.x) + op.width &&
            ly < double(op.y) + op.height)
          hit = op.add;
      if (hit)
        return true;
    }
    return false;
  }
};
