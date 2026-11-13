#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>

// UI-thread owned. Raw Input supplies unaccelerated relative counts; ClipCursor
// keeps the hidden pointer in the focused client area. SetCapture is unrelated.
class RelativeInput {
  HWND owner = nullptr;
  uint32_t window = 0;
  uint64_t epoch = 0;
  uint64_t newest = 0;
  RECT previousClip{};
  POINT previousPosition{};
  int hideCalls = 0;
  bool registered = false, clipped = false;

public:
  ~RelativeInput() { release(); }
  bool active(HWND hwnd = nullptr) const { return owner && (!hwnd || hwnd == owner); }
  uint32_t id() const { return window; }
  uint64_t generation() const { return epoch; }
  bool stale(uint64_t generation) const { return generation < newest; }

  void release() {
    HWND old = owner;
    owner = nullptr;
    if (registered) {
      RAWINPUTDEVICE device{1, 2, RIDEV_REMOVE, nullptr};
      RegisterRawInputDevices(&device, 1, sizeof(device));
      registered = false;
    }
    if (clipped) {
      ClipCursor(&previousClip);
      clipped = false;
      if (GetForegroundWindow() == old)
        SetCursorPos(previousPosition.x, previousPosition.y);
    }
    while (hideCalls > 0) {
      ShowCursor(TRUE);
      --hideCalls;
    }
    window = 0;
    epoch = 0;
  }

  bool lock(HWND hwnd, uint32_t id, uint64_t generation) {
    if (active(hwnd) && epoch == generation && window == id)
      return true;
    if (!generation || generation <= newest)
      return false;
    newest = generation;
    release();
    if (!IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd) ||
        GetForegroundWindow() != hwnd || GetFocus() != hwnd)
      return false;
    RECT area{};
    if (!GetClientRect(hwnd, &area) || area.right <= 0 || area.bottom <= 0)
      return false;
    POINT first{area.left, area.top}, last{area.right, area.bottom};
    if (!ClientToScreen(hwnd, &first) || !ClientToScreen(hwnd, &last))
      return false;
    area = {first.x, first.y, last.x, last.y};
    if (!GetClipCursor(&previousClip) || !GetCursorPos(&previousPosition))
      return false;
    POINT position{std::clamp(previousPosition.x, area.left, area.right - 1),
                   std::clamp(previousPosition.y, area.top, area.bottom - 1)};
    RECT point{position.x, position.y, position.x + 1, position.y + 1};
    owner = hwnd;
    window = id;
    epoch = generation;
    RAWINPUTDEVICE device{1, 2, 0, hwnd};
    if (!RegisterRawInputDevices(&device, 1, sizeof(device))) {
      release();
      return false;
    }
    registered = true;
    if (!ClipCursor(&point)) {
      release();
      return false;
    }
    clipped = true;
    for (int i = 0; i < 16; ++i) {
      ++hideCalls;
      if (ShowCursor(FALSE) < 0)
        return true;
    }
    release();
    return false;
  }

  bool motion(HRAWINPUT handle, LONG &dx, LONG &dy) const {
    if (!active() || GetFocus() != owner || GetForegroundWindow() != owner)
      return false;
    RAWINPUT input{};
    UINT bytes = sizeof(input);
    if (GetRawInputData(handle, RID_INPUT, &input, &bytes, sizeof(RAWINPUTHEADER)) == UINT(-1) ||
        bytes < sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE) || input.header.dwType != RIM_TYPEMOUSE ||
        (input.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
      return false;
    dx = input.data.mouse.lLastX;
    dy = input.data.mouse.lLastY;
    return dx || dy;
  }
};
