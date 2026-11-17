#pragma once
#include <windows.h>
#include <algorithm>
#include <string>

// A touch-owned move/resize uses the contact, independently of mouse position.
// Windows still applies the production window's sizing constraints and DPI.
class TouchWindowManipulation {
  HWND window = nullptr;
  RECT initial{};
  POINT press{};
  unsigned edge = 0;
  bool resizing = false;
public:
  bool begin(HWND target, const std::string &kind, unsigned resizeEdge,
             RECT downWindow, POINT down, POINT current) {
    window = nullptr;
    if (!IsWindow(target) || IsIconic(target) || (kind != "move" && kind != "resize")) return false;
    resizing = kind == "resize"; edge = resizeEdge;
    if (resizing && (IsZoomed(target) || !edge || (edge & ~15u) ||
                     (edge & 3u) == 3u || (edge & 12u) == 12u)) return false;
    initial = downWindow; press = down;
    if (!resizing && IsZoomed(target)) {
      double fraction = std::clamp(double(down.x - initial.left) /
                                  std::max(1L, initial.right - initial.left), 0., 1.);
      LONG titleOffset = std::max(0L, down.y - initial.top);
      ShowWindow(target, SW_RESTORE);
      if (!GetWindowRect(target, &initial)) return false;
      LONG width = initial.right - initial.left, height = initial.bottom - initial.top;
      initial.left = current.x - LONG(width * fraction);
      initial.top = current.y - std::min(titleOffset, std::max(0L, height - 1));
      initial.right = initial.left + width; initial.bottom = initial.top + height;
      press = current;
    }
    window = target;
    return update(current);
  }
  bool update(POINT position) {
    if (!IsWindow(window)) return false;
    RECT next = initial;
    LONG dx = position.x - press.x, dy = position.y - press.y;
    if (!resizing) OffsetRect(&next, dx, dy);
    else {
      if (edge & 1u) next.top += dy;
      if (edge & 2u) next.bottom += dy;
      if (edge & 4u) next.left += dx;
      if (edge & 8u) next.right += dx;
      MINMAXINFO limits{};
      SendMessageW(window, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&limits));
      LONG width = std::max(std::max(1L, limits.ptMinTrackSize.x), next.right - next.left);
      LONG height = std::max(std::max(1L, limits.ptMinTrackSize.y), next.bottom - next.top);
      if (limits.ptMaxTrackSize.x > 0) width = std::min(width, limits.ptMaxTrackSize.x);
      if (limits.ptMaxTrackSize.y > 0) height = std::min(height, limits.ptMaxTrackSize.y);
      if (edge & 4u) next.left = initial.right - width; else next.right = next.left + width;
      if (edge & 1u) next.top = initial.bottom - height; else next.bottom = next.top + height;
    }
    return SetWindowPos(window, nullptr, next.left, next.top,
                        next.right - next.left, next.bottom - next.top,
                        SWP_NOACTIVATE | SWP_NOZORDER | (resizing ? 0 : SWP_NOSIZE)) != FALSE;
  }
  void end() { window = nullptr; }
};
