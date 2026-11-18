#include "native_touchpad.h"
#include <array>
#include <charconv>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

using Json = nlohmann::json;
static std::vector<Json> events;
static NativeTouchpad touchpad([](const Json &packet) { events.push_back(packet); });
static bool enabled;
static unsigned wheelEvents, hitEvents;

static void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

struct NativeTouchpadProbe {
  static void replay() {
    touchpad.dpi = 144;
    touchpad.windowId = 7;
    touchpad.transform({1.f, 0.f, 0.f, 1.f, 30.f, -12.f});
    touchpad.transform({1.f, 0.f, 0.f, 1.f, 30.f, -12.f});
    touchpad.transform({1.25f, 0.f, 0.f, 1.25f, -30.f, -60.f});
    touchpad.transform({1.5f, 0.f, 0.f, 1.5f, -60.f, -120.f});
    touchpad.transform({std::numeric_limits<float>::infinity(), 0.f, 0.f, 1.f, 0.f, 0.f});
    touchpad.endGesture(true);
    require(events.size() == 7, "lost or duplicated gesture transitions");
    require(events[0]["kind"] == "scroll" && events[0]["phase"] == "begin" &&
                events[1]["dx"] == -20. && events[1]["dy"] == 8. && events[2]["phase"] == "end",
            "pan direction, scale, or deduplication");
    require(events[3]["kind"] == "pinch" && events[3]["phase"] == "begin" &&
                events[3]["gesture"] == 2 && events[4]["scale"] == 1.25 &&
                events[5]["scale"] == 1.5 && events[6]["phase"] == "cancel",
            "pinch scale, token, or cancellation");
    events.clear();
    std::cout << "PASS native transform conversion, DPI, pan/pinch transitions, cancellation\n";
  }
  static bool attached(HWND window) { return touchpad.owner == window; }
};

static LRESULT CALLBACK fixtureWindow(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
  if (message == DM_POINTERHITTEST && enabled && GetForegroundWindow() == hwnd) {
    ++hitEvents;
    if (touchpad.hit(hwnd, wp, GetDpiForWindow(hwnd)))
      return 0;
  }
  if (message == WM_TIMER && wp == NativeTouchpad::Timer) {
    touchpad.tick(hwnd, enabled && GetForegroundWindow() == hwnd);
    return 0;
  }
  if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) {
    ++wheelEvents;
    return 0;
  }
  if (message == WM_KILLFOCUS || message == WM_DESTROY)
    touchpad.detach(hwnd);
  return DefWindowProcW(hwnd, message, wp, lp);
}

// The Microsoft SDK documents this ABI, but older MinGW headers omit it.
struct SyntheticTouchpadParameters {
  POINTER_INPUT_TYPE type;
  ULONG maxContacts;
  POINTER_FEEDBACK_MODE feedback;
  HMONITOR monitor;
  ULONG width, height, options;
};

static void pump(DWORD duration) {
  ULONGLONG end = GetTickCount64() + duration;
  do {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
      DispatchMessageW(&message);
    Sleep(1);
  } while (GetTickCount64() < end);
}

static void gesture(HWND hwnd, DWORD expectedOwner, HSYNTHETICPOINTERDEVICE device, bool pinch) {
  std::array<POINTER_TYPE_INFO, 2> contacts{};
  for (unsigned step = 0; step <= 25; ++step) {
    POINT cursor{};
    DWORD owner = 0;
    GetWindowThreadProcessId(hwnd, &owner);
    require(owner == expectedOwner && GetForegroundWindow() == hwnd && GetCursorPos(&cursor) &&
                GetAncestor(WindowFromPoint(cursor), GA_ROOT) == hwnd,
            "test window lost foreground or cursor ownership");
    for (unsigned i = 0; i < contacts.size(); ++i) {
      auto &contact = contacts[i];
      contact.type = PT_TOUCHPAD;
      auto &pointer = contact.touchInfo.pointerInfo;
      pointer.pointerId = i;
      pointer.pointerFlags = POINTER_FLAG_CONFIDENCE;
      if (step != 25)
        pointer.pointerFlags |= POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
      LONG progress = LONG(std::min(24u, step));
      pointer.ptHimetricLocation.x =
          i ? 5900 + (pinch ? progress * 90 : 0) : 4200 - (pinch ? progress * 90 : 0);
      pointer.ptHimetricLocation.y = 1750 + (pinch ? 0 : progress * 115);
      pointer.dwTime = GetTickCount();
    }
    require(InjectSyntheticPointerInput(device, contacts.data(), UINT32(contacts.size())),
            "Windows declined the synthetic Precision Touchpad frame");
    pump(16);
  }
  pump(400);
}

struct CursorRestore {
  POINT original{}, target{};
  bool moved = false;
  ~CursorRestore() {
    POINT cursor{};
    if (moved && GetCursorPos(&cursor) && cursor.x == target.x && cursor.y == target.y)
      SetCursorPos(original.x, original.y);
  }
};

static HSYNTHETICPOINTERDEVICE createDevice() {
  using Create = HSYNTHETICPOINTERDEVICE(WINAPI *)(const SyntheticTouchpadParameters *);
  FARPROC address =
      GetProcAddress(GetModuleHandleW(L"user32.dll"), "CreateSyntheticPointerDevice2");
  require(address != nullptr, "CreateSyntheticPointerDevice2 unavailable");
  Create create = nullptr;
  static_assert(sizeof(create) == sizeof(address));
  std::memcpy(&create, &address, sizeof(create));
  const SyntheticTouchpadParameters parameters{
      PT_TOUCHPAD, 2, POINTER_FEEDBACK_NONE, nullptr, 10000, 6000, 3};
  return create(&parameters);
}

static void native() {
  require(touchpad.initialize(), "Direct Manipulation unavailable");
  WNDCLASSW klass{};
  klass.lpfnWndProc = fixtureWindow;
  klass.hInstance = GetModuleHandleW(nullptr);
  klass.lpszClassName = L"WaybridgePrivateTouchpadProbe";
  klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  require(RegisterClassW(&klass) != 0, "register fixture window");
  HWND hwnd =
      CreateWindowExW(WS_EX_TOPMOST, klass.lpszClassName, L"Waybridge temporary touchpad probe",
                      WS_OVERLAPPEDWINDOW | WS_VISIBLE, 180, 180, 550, 410, nullptr, nullptr,
                      klass.hInstance, nullptr);
  require(hwnd != nullptr, "create fixture window");
  CursorRestore cursor;
  HSYNTHETICPOINTERDEVICE device = nullptr;
  auto owner = GetCurrentProcessId();
  try {
    device = createDevice();
    require(device != nullptr, "Windows declined a private synthetic touchpad");
    SetForegroundWindow(hwnd);
    pump(300);
    require(GetForegroundWindow() == hwnd && GetCursorPos(&cursor.original),
            "fixture window is not foreground");
    cursor.target = {200, 150};
    require(ClientToScreen(hwnd, &cursor.target) &&
                GetAncestor(WindowFromPoint(cursor.target), GA_ROOT) == hwnd &&
                SetCursorPos(cursor.target.x, cursor.target.y),
            "fixture window is covered");
    cursor.moved = true;
    enabled = true;
    touchpad.focus(hwnd, 7);
    require(NativeTouchpadProbe::attached(hwnd), "Direct Manipulation viewport setup failed");
    unsigned wheelBefore = wheelEvents;
    gesture(hwnd, owner, device, false);
    gesture(hwnd, owner, device, true);
    auto found = [](const char *kind, const char *phase) {
      return std::any_of(events.begin(), events.end(), [&](const auto &event) {
        return event["phase"] == phase && event["kind"] == kind;
      });
    };
    std::cout << "DM hit tests " << hitEvents << ", wheel events " << wheelEvents - wheelBefore
              << '\n';
    for (const auto &event : events)
      if (event["phase"] != "update")
        std::cout << event.dump() << '\n';
    require(found("scroll", "update") && found("scroll", "end"), "native pan did not complete");
    require(found("pinch", "update") && found("pinch", "end"), "native pinch did not complete");
    require(wheelEvents == wheelBefore, "native gestures also generated legacy wheel input");
    unsigned nativeEvents = unsigned(events.size());
    enabled = false;
    touchpad.detach(hwnd);
    gesture(hwnd, owner, device, false);
    require(events.size() == nativeEvents && wheelEvents > wheelBefore,
            "fallback did not restore wheel input");
  } catch (...) {
    if (device)
      DestroySyntheticPointerDevice(device);
    DestroyWindow(hwnd);
    throw;
  }
  DestroySyntheticPointerDevice(device);
  DestroyWindow(hwnd);
  std::cout << "PASS Windows Precision Touchpad injection through Direct Manipulation and wheel "
               "fallback\n";
}

struct ViewerTarget {
  DWORD pid;
  std::wstring title;
  HWND hwnd = nullptr;
};

static BOOL CALLBACK findViewer(HWND window, LPARAM context) {
  auto &target = *reinterpret_cast<ViewerTarget *>(context);
  DWORD owner = 0;
  GetWindowThreadProcessId(window, &owner);
  if (owner != target.pid || !IsWindowVisible(window))
    return TRUE;
  wchar_t title[128]{};
  GetWindowTextW(window, title, int(std::size(title)));
  if (target.title != title)
    return TRUE;
  target.hwnd = window;
  return FALSE;
}

static void productionViewer(const char *pid, const char *title) {
  const char *end = pid + std::strlen(pid);
  ViewerTarget target{0, std::wstring(title, title + std::strlen(title))};
  auto parsed = std::from_chars(pid, end, target.pid);
  require(parsed.ec == std::errc{} && parsed.ptr == end && target.pid != 0 &&
              target.title.rfind(L"Waybridge-touchpad-e2e-", 0) == 0,
          "expected a disposable fixture title and viewer PID");
  ULONGLONG deadline = GetTickCount64() + 12000;
  while (!target.hwnd && GetTickCount64() < deadline) {
    EnumWindows(findViewer, reinterpret_cast<LPARAM>(&target));
    if (!target.hwnd)
      pump(50);
  }
  require(target.hwnd != nullptr, "production viewer fixture window did not open");
  SetForegroundWindow(target.hwnd);
  pump(300);
  CursorRestore cursor;
  RECT client{};
  require(GetForegroundWindow() == target.hwnd && GetCursorPos(&cursor.original) &&
              GetClientRect(target.hwnd, &client),
          "production viewer fixture window is not foreground");
  cursor.target = {(client.left + client.right) / 2, (client.top + client.bottom) / 2};
  require(ClientToScreen(target.hwnd, &cursor.target) &&
              GetAncestor(WindowFromPoint(cursor.target), GA_ROOT) == target.hwnd &&
              SetCursorPos(cursor.target.x, cursor.target.y),
          "production viewer fixture window is covered");
  cursor.moved = true;
  pump(300);
  auto device = createDevice();
  require(device != nullptr, "Windows declined a private synthetic touchpad");
  try {
    gesture(target.hwnd, target.pid, device, false);
    gesture(target.hwnd, target.pid, device, true);
  } catch (...) {
    DestroySyntheticPointerDevice(device);
    throw;
  }
  DestroySyntheticPointerDevice(device);
  std::cout << "PASS injected synthetic pan and pinch into the foreground production viewer\n";
}

int main(int argc, char **argv) {
  if (FAILED(OleInitialize(nullptr)))
    return 1;
  int result = 0;
  try {
    NativeTouchpadProbe::replay();
    if (argc > 1 && std::string(argv[1]) == "--inject-into-private-window")
      native();
    if (argc == 4 && std::string(argv[1]) == "--production-viewer")
      productionViewer(argv[2], argv[3]);
  } catch (const std::exception &error) {
    std::cerr << error.what() << " (GetLastError " << GetLastError() << ")\n";
    result = 1;
  }
  touchpad.shutdown();
  OleUninitialize();
  return result;
}
