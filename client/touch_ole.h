#pragma once
#include <windows.h>
#include <array>
#include <cstdint>
#include <cstring>

// A system-only bridge for one current primary touch or pen contact. No SendInput,
// posted fake mouse buttons, ordinal calls or clipboard publication occur here.
// Promote inside the pointer-owning UI thread while its WM_POINTER data is live,
// then enter DoDragDrop only when takeMouseReady() accepts a synthesized mouse
// down/move in that same window. Keep active() until DoDragDrop returns/reset().
// Reset pending promotion on contact UP/CANCEL, owner destruction or a bounded
// no-mouse timeout. A missing named export or rejected conversion leaves the
// caller responsible for keeping its native interaction and offering mouse
// compatibility; this helper never fabricates a substitute mouse sequence.
// https://learn.microsoft.com/windows/win32/winmsg/winuser/nf-winuser-convertprimarypointertomousedrag
// https://learn.microsoft.com/windows/win32/api/ole2/nf-ole2-dodragdrop
namespace TouchOle {
enum class Code {
  AwaitingMouse,
  Unsupported,
  WrongThread,
  InvalidWindow,
  PointerUnavailable,
  InvalidContact,
  NotPrimary,
  MultipleContacts,
  AlreadyActive,
  SystemRejected,
};
struct Result {
  Code code = Code::Unsupported;
  DWORD error = ERROR_NOT_SUPPORTED;
  bool promoted() const { return code == Code::AwaitingMouse; }
};
inline const char *name(Code code) {
  switch (code) {
  case Code::AwaitingMouse:
    return "awaiting_system_mouse";
  case Code::Unsupported:
    return "unsupported_system_bridge";
  case Code::WrongThread:
    return "wrong_pointer_thread";
  case Code::InvalidWindow:
    return "invalid_source_window";
  case Code::PointerUnavailable:
    return "pointer_data_unavailable";
  case Code::InvalidContact:
    return "contact_not_active_touch";
  case Code::NotPrimary:
    return "contact_not_primary";
  case Code::MultipleContacts:
    return "multiple_contacts";
  case Code::AlreadyActive:
    return "promotion_already_active";
  case Code::SystemRejected:
    return "system_rejected_promotion";
  }
  return "unknown";
}
class Bridge {
  using Convert = BOOL(WINAPI *)();
  HMODULE library = nullptr;
  Convert convert = nullptr;
  HWND owner = nullptr;
  UINT pointer = 0;
  POINTER_INPUT_TYPE type = PT_TOUCH;
  bool awaitingMouse = false;

public:
  Bridge() {
    library = LoadLibraryExW(L"user32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (library) {
      // Preview docs identify ordinal 2811, whose meaning is not safe to assume
      // on an older DLL. Named resolution identifies the exact API instead.
      auto address = GetProcAddress(library, "ConvertPrimaryPointerToMouseDrag");
      static_assert(sizeof(address) == sizeof(convert));
      std::memcpy(&convert, &address, sizeof(convert));
    }
  }
  ~Bridge() {
    if (library)
      FreeLibrary(library);
  }
  Bridge(const Bridge &) = delete;
  Bridge &operator=(const Bridge &) = delete;
  bool supported() const { return convert != nullptr; }
  bool active(HWND window = nullptr) const { return owner && (!window || window == owner); }
  UINT pointerId() const { return pointer; }
  void reset() {
    owner = nullptr;
    pointer = 0;
    awaitingMouse = false;
  }
  static bool matchesMouse(POINTER_INPUT_TYPE source, ULONG_PTR extra) {
    if ((extra & 0xffffff00) != 0xff515700) return false;
    return source == PT_TOUCH ? (extra & 0x80) != 0 : source == PT_PEN && (extra & 0x80) == 0;
  }
  Result promote(HWND window, UINT pointerId, POINTER_INPUT_TYPE source = PT_TOUCH) {
    if (!convert)
      return {Code::Unsupported, ERROR_NOT_SUPPORTED};
    if (owner)
      return {Code::AlreadyActive, ERROR_BUSY};
    if (!window || !IsWindow(window))
      return {Code::InvalidWindow, ERROR_INVALID_WINDOW_HANDLE};
    if (GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId())
      return {Code::WrongThread, ERROR_ACCESS_DENIED};
    POINTER_INFO info{};
    SetLastError(ERROR_SUCCESS);
    if (!GetPointerInfo(pointerId, &info))
      return {Code::PointerUnavailable, GetLastError()};
    if ((source != PT_TOUCH && source != PT_PEN) || info.pointerType != source || info.hwndTarget != window ||
        !(info.pointerFlags & POINTER_FLAG_INCONTACT) ||
        (info.pointerFlags & (POINTER_FLAG_UP | POINTER_FLAG_CANCELED)))
      return {Code::InvalidContact, ERROR_INVALID_PARAMETER};
    if (!(info.pointerFlags & POINTER_FLAG_PRIMARY))
      return {Code::NotPrimary, ERROR_INVALID_PARAMETER};
    std::array<POINTER_INFO, MAX_TOUCH_COUNT> frame{};
    UINT32 count = UINT32(frame.size());
    SetLastError(ERROR_SUCCESS);
    if (!GetPointerFrameInfo(pointerId, &count, frame.data()))
      return {count > frame.size() ? Code::MultipleContacts : Code::PointerUnavailable,
              GetLastError()};
    UINT32 active = 0;
    for (UINT32 i = 0; i < count; ++i)
      if ((frame[i].pointerFlags & POINTER_FLAG_INCONTACT) &&
          !(frame[i].pointerFlags & (POINTER_FLAG_UP | POINTER_FLAG_CANCELED)))
        ++active;
    if (active != 1)
      return {Code::MultipleContacts, ERROR_INVALID_PARAMETER};
    // A frame reports contacts owned by this window. The system call below is
    // the final authority and also rejects extra contacts on the calling thread.
    // Publish ownership before the system call in case it dispatches a native
    // message reentrantly. Failed conversion rolls the local state back.
    owner = window;
    pointer = pointerId;
    type = source;
    awaitingMouse = true;
    SetLastError(ERROR_SUCCESS);
    if (!convert()) {
      DWORD error = GetLastError();
      reset();
      return {Code::SystemRejected, error};
    }
    return {Code::AwaitingMouse, ERROR_SUCCESS};
  }
  bool takeMouseReady(HWND window, UINT message, WPARAM keys, ULONG_PTR extra) {
    if (!awaitingMouse || owner != window || !IsWindow(window))
      return false;
    if (message != WM_LBUTTONDOWN && message != WM_MOUSEMOVE)
      return false;
    if (!(keys & MK_LBUTTON) || !matchesMouse(type,extra))
      return false;
    awaitingMouse = false;
    return true;
  }
};
} // namespace TouchOle
