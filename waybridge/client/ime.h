#pragma once
#include <windows.h>
#include <imm.h>
#include <algorithm>
#include <cstdint>
#include <optional>
#include <set>
#include <string>

struct NativeIme {
  bool initialized = false, enabled = false, composing = false, cancelling = false;
  uint32_t target = 0;
  uint64_t generation = 0, compositionGeneration = 0;
  wchar_t surrogate = 0;
  RECT lastCaret{0, 0, 1, 20};
  std::set<uint32_t> forwardedKeys;
  // Keep the last keydown route after keyup: TranslateMessage can queue a
  // character after the physical release. Raw shortcuts must not also commit
  // that translated character through text-input-v3.
  std::set<uint32_t> rawCharacterKeys;
  void cancel(HWND hwnd) {
    bool previous = cancelling;
    cancelling = true;
    if (composing) {
      if (auto context = ImmGetContext(hwnd)) {
        ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
        ImmReleaseContext(hwnd, context);
      }
    }
    composing = false;
    surrogate = 0;
    cancelling = previous;
  }
  void associate(HWND hwnd, bool active, uint32_t id, uint64_t serial) {
    if (initialized && enabled == active && target == id && generation == serial) return;
    cancel(hwnd);
    initialized = true;
    enabled = active;
    target = id;
    generation = serial;
    ImmAssociateContextEx(hwnd, nullptr, active ? IACE_DEFAULT : 0);
  }
  static std::optional<std::wstring> string(HIMC context, DWORD kind) {
    LONG length = ImmGetCompositionStringW(context, kind, nullptr, 0);
    if (length < 0 || length > 131072 || length % sizeof(wchar_t)) return {};
    std::wstring text(size_t(length) / sizeof(wchar_t), L'\0');
    if (length && ImmGetCompositionStringW(context, kind, text.data(), DWORD(length)) != length) return {};
    return text;
  }
  std::optional<std::wstring> character(wchar_t c) {
    if (c >= 0xd800 && c <= 0xdbff) { surrogate = c; return {}; }
    std::wstring text;
    if (c >= 0xdc00 && c <= 0xdfff) {
      if (!surrogate) return {};
      text.push_back(surrogate);
    }
    surrogate = 0;
    text.push_back(c);
    return text;
  }
  static bool printable(WPARAM key) {
    return key == VK_SPACE || (key >= '0' && key <= 'Z') ||
           (key >= VK_NUMPAD0 && key <= VK_DIVIDE) ||
           (key >= VK_OEM_1 && key <= VK_OEM_3) ||
           (key >= VK_OEM_4 && key <= VK_OEM_8) || key == VK_OEM_102 || key == VK_PACKET;
  }
  bool shortcut(WPARAM key, LPARAM param) const {
    bool win = (GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000);
    // AltGr synthesizes Ctrl+RightAlt but produces text on the local layout.
    // Query without mutating dead-key state (Windows 10 1607+, also required by
    // the DPI APIs). Ctrl+RightAlt remains a shortcut on layouts with no text.
    if (!win && (GetKeyState(VK_RMENU) & 0x8000) && (GetKeyState(VK_CONTROL) & 0x8000)) {
      BYTE state[256]{};
      wchar_t text[8]{};
      if (GetKeyboardState(state)) {
        int count = ToUnicodeEx(UINT(key), UINT((param >> 16) & 0xff), state, text, 8, 4,
                                GetKeyboardLayout(0));
        if (count < 0 || (count > 0 && text[0] >= 32 && text[0] != 127)) return false;
      }
    }
    if (win || (GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000))
      return true;
    for (auto key : {29u, 97u, 56u, 100u, 125u, 126u})
      if (forwardedKeys.count(key)) return true;
    return false;
  }
  void caret(HWND hwnd, const RECT &rect) {
    if (!enabled || GetFocus() != hwnd) return;
    lastCaret = rect;
    CreateCaret(hwnd, nullptr, 1, std::max(1L, rect.bottom - rect.top));
    SetCaretPos(rect.left, rect.top);
    if (auto context = ImmGetContext(hwnd)) {
      COMPOSITIONFORM composition{};
      composition.dwStyle = CFS_POINT;
      composition.ptCurrentPos = {rect.left, rect.top};
      ImmSetCompositionWindow(context, &composition);
      CANDIDATEFORM candidate{};
      candidate.dwStyle = CFS_EXCLUDE;
      candidate.ptCurrentPos = {rect.left, rect.bottom};
      candidate.rcArea = rect;
      ImmSetCandidateWindow(context, &candidate);
      ImmReleaseContext(hwnd, context);
    }
  }
};
