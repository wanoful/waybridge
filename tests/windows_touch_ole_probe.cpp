// Isolated capability inventory. Ordinal addresses are inspected, never invoked.
#include <windows.h>
#include <winver.h>
#include <ole2.h>
#include <shlobj.h>
#include <atomic>
#include <thread>
#include <stdexcept>
#include "touch_ole.h"
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include "third_party/json.hpp"
using Json = nlohmann::json;

static Json version() {
  OSVERSIONINFOW info{};
  info.dwOSVersionInfoSize = sizeof(info);
  using GetVersion = LONG(WINAPI *)(OSVERSIONINFOW *);
  auto address = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
  GetVersion proc = nullptr;
  static_assert(sizeof(proc) == sizeof(address));
  std::memcpy(&proc, &address, sizeof(proc));
  if (!proc || proc(&info) != 0)
    return nullptr;
  return {{"major", info.dwMajorVersion},
          {"minor", info.dwMinorVersion},
          {"build", info.dwBuildNumber}};
}
static Json fileVersion(const wchar_t *path) {
  DWORD unused = 0, size = GetFileVersionInfoSizeW(path, &unused);
  std::vector<uint8_t> bytes(size);
  if (!size || !GetFileVersionInfoW(path, 0, size, bytes.data()))
    return nullptr;
  VS_FIXEDFILEINFO *info = nullptr;
  UINT count = 0;
  if (!VerQueryValueW(bytes.data(), L"\\", reinterpret_cast<void **>(&info), &count) ||
      count < sizeof(*info) || info->dwSignature != 0xfeef04bd)
    return nullptr;
  return Json::array({HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                      HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS)});
}
static Json inventory() {
  Json report = {{"mode", "inventory"}, {"os", version()}, {"ordinal_invoked", false}};
  HANDLE token = nullptr;
  TOKEN_ELEVATION elevation{};
  DWORD size = 0;
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size))
      report["elevated"] = elevation.TokenIsElevated != 0;
    CloseHandle(token);
  }
  auto user32 = LoadLibraryExW(L"user32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!user32) {
    report["error"] = GetLastError();
    return report;
  }
  wchar_t path[32768]{};
  GetModuleFileNameW(user32, path, 32768);
  report["user32_version"] = fileVersion(path);
  auto named = GetProcAddress(user32, "ConvertPrimaryPointerToMouseDrag");
  auto ordinal = GetProcAddress(user32, MAKEINTRESOURCEA(2811));
  report["named_export"] = named != nullptr;
  report["ordinal_export"] = ordinal != nullptr;
  report["named_matches_ordinal"] = named && named == ordinal;
  auto base = reinterpret_cast<const uint8_t *>(user32);
  auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
  auto nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
  report["user32_timestamp"] = nt->FileHeader.TimeDateStamp;
  report["user32_image_size"] = nt->OptionalHeader.SizeOfImage;
  const auto &directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  if (directory.VirtualAddress && directory.Size >= sizeof(IMAGE_EXPORT_DIRECTORY)) {
    auto exports =
        reinterpret_cast<const IMAGE_EXPORT_DIRECTORY *>(base + directory.VirtualAddress);
    report["export_base"] = exports->Base;
    report["export_functions"] = exports->NumberOfFunctions;
    Json names = Json::array();
    if (2811 >= exports->Base && 2811 - exports->Base < exports->NumberOfFunctions) {
      auto functions = reinterpret_cast<const DWORD *>(base + exports->AddressOfFunctions);
      auto nameRvas = reinterpret_cast<const DWORD *>(base + exports->AddressOfNames);
      auto ordinals = reinterpret_cast<const WORD *>(base + exports->AddressOfNameOrdinals);
      for (DWORD index = 0; index < exports->NumberOfNames; ++index)
        if (ordinals[index] == 2811 - exports->Base)
          names.push_back(reinterpret_cast<const char *>(base + nameRvas[index]));
      report["ordinal_function_rva"] = functions[2811 - exports->Base];
    }
    report["ordinal_names"] = names;
  }
  FreeLibrary(user32);
  return report;
}

static const wchar_t Payload[] = L"Waybridge native touch OLE \u4f60\u597d";
static constexpr UINT ReadyMessage = WM_APP + 79;
class Data final : public IDataObject {
  LONG refs = 1;

public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (iid == IID_IUnknown || iid == IID_IDataObject) {
      *out = static_cast<IDataObject *>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ULONG(InterlockedIncrement(&refs)); }
  ULONG STDMETHODCALLTYPE Release() override {
    auto count = InterlockedDecrement(&refs);
    if (!count)
      delete this;
    return ULONG(count);
  }
  HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC *format) override {
    return format && format->cfFormat == CF_UNICODETEXT && (format->tymed & TYMED_HGLOBAL) &&
                   format->dwAspect == DVASPECT_CONTENT && format->lindex == -1
               ? S_OK
               : DV_E_FORMATETC;
  }
  HRESULT STDMETHODCALLTYPE GetData(FORMATETC *format, STGMEDIUM *medium) override {
    if (!medium)
      return E_POINTER;
    if (QueryGetData(format) != S_OK)
      return DV_E_FORMATETC;
    *medium = {};
    medium->tymed = TYMED_HGLOBAL;
    medium->hGlobal = GlobalAlloc(GMEM_MOVEABLE, sizeof(Payload));
    if (!medium->hGlobal)
      return E_OUTOFMEMORY;
    auto target = GlobalLock(medium->hGlobal);
    if (!target) {
      GlobalFree(medium->hGlobal);
      *medium = {};
      return E_OUTOFMEMORY;
    }
    std::memcpy(target, Payload, sizeof(Payload));
    GlobalUnlock(medium->hGlobal);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC *, STGMEDIUM *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC *, FORMATETC *out) override {
    if (out)
      out->ptd = nullptr;
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE SetData(FORMATETC *, STGMEDIUM *, BOOL) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC **out) override {
    if (direction != DATADIR_GET)
      return E_NOTIMPL;
    FORMATETC f{CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    return SHCreateStdEnumFmtEtc(1, &f, out);
  }
  HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC *, DWORD, IAdviseSink *, DWORD *) override {
    return OLE_E_ADVISENOTSUPPORTED;
  }
  HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
  HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA **) override {
    return OLE_E_ADVISENOTSUPPORTED;
  }
};
struct Context;
class Source final : public IDropSource {
  LONG refs = 1;
  ULONGLONG started = GetTickCount64();

public:
  unsigned queries = 0;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (iid == IID_IUnknown || iid == IID_IDropSource) {
      *out = static_cast<IDropSource *>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ULONG(InterlockedIncrement(&refs)); }
  ULONG STDMETHODCALLTYPE Release() override {
    auto count = InterlockedDecrement(&refs);
    if (!count)
      delete this;
    return ULONG(count);
  }
  HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override {
    ++queries;
    if (escape || GetTickCount64() - started > 5000)
      return DRAGDROP_S_CANCEL;
    return keys & MK_LBUTTON ? S_OK : DRAGDROP_S_DROP;
  }
  HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
};
class Sink final : public IDropTarget {
  LONG refs = 1;

public:
  unsigned enters = 0, drops = 0;
  bool exact = false;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (iid == IID_IUnknown || iid == IID_IDropTarget) {
      *out = static_cast<IDropTarget *>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ULONG(InterlockedIncrement(&refs)); }
  ULONG STDMETHODCALLTYPE Release() override {
    auto count = InterlockedDecrement(&refs);
    if (!count)
      delete this;
    return ULONG(count);
  }
  HRESULT STDMETHODCALLTYPE DragEnter(IDataObject *, DWORD, POINTL, DWORD *effect) override {
    ++enters;
    *effect = DROPEFFECT_COPY;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD *effect) override {
    *effect = DROPEFFECT_COPY;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DragLeave() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE Drop(IDataObject *data, DWORD, POINTL, DWORD *effect) override {
    ++drops;
    exact = false;
    FORMATETC format{CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    if (SUCCEEDED(data->GetData(&format, &medium))) {
      auto bytes = static_cast<const wchar_t *>(GlobalLock(medium.hGlobal));
      exact = bytes && GlobalSize(medium.hGlobal) >= sizeof(Payload) &&
              std::memcmp(bytes, Payload, sizeof(Payload)) == 0;
      if (bytes)
        GlobalUnlock(medium.hGlobal);
      ReleaseStgMedium(&medium);
    }
    *effect = exact ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    return S_OK;
  }
};
struct Context {
  TouchOle::Bridge bridge;
  HWND source = nullptr, target = nullptr;
  Sink *sink = nullptr;
  bool multi = false, oleDone = false, promotionSucceeded = false;
  bool deferReady = false, ready = false;
  UINT readyDelay = 0;
  UINT lastPointer = 0, entryMessage = 0;
  ULONG_PTR entryExtra = 0;
  HRESULT dragResult = E_PENDING;
  DWORD effect = 0;
  unsigned pointerDowns = 0, promotedMessages = 0, queries = 0;
  Json outcomes = Json::array();
};
static LRESULT CALLBACK fixtureProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
  auto c = reinterpret_cast<Context *>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    c = static_cast<Context *>(reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(c));
  }
  if (!c)
    return DefWindowProcW(window, message, wp, lp);
  if (message == ReadyMessage && window == c->source) {
    c->ready = true;
    return 0;
  }
  if (message == WM_POINTERDOWN && window == c->source) {
    ++c->pointerDowns;
    c->lastPointer = GET_POINTERID_WPARAM(wp);
  }
  if (window == c->source &&
      ((message == WM_POINTERDOWN && c->multi) ||
       (message == WM_POINTERUPDATE && !c->multi && !c->promotionSucceeded))) {
    auto outcome = c->bridge.promote(window, c->lastPointer);
    c->outcomes.push_back({{"code", TouchOle::name(outcome.code)}, {"error", outcome.error}});
    c->promotionSucceeded |= outcome.promoted();
    return 0;
  }
  if (message == WM_POINTERDOWN || message == WM_POINTERUPDATE || message == WM_POINTERUP ||
      message == WM_POINTERCAPTURECHANGED)
    return 0;
  if (window == c->source && (message == WM_LBUTTONDOWN || message == WM_MOUSEMOVE)) {
    ULONG_PTR extra = ULONG_PTR(GetMessageExtraInfo());
    if (c->bridge.takeMouseReady(window, message, wp, extra)) {
      ++c->promotedMessages;
      c->entryMessage = message;
      c->entryExtra = extra;
      // Simulate an asynchronous server-ready acknowledgement, dispatching it
      // in a bounded wait while preserving the real synthesized mouse handler
      // as the caller of DoDragDrop. No fake button messages are posted.
      if (c->deferReady) {
        ULONGLONG began = GetTickCount64();
        std::thread acknowledge([window] {
          Sleep(35);
          PostMessageW(window, ReadyMessage, 0, 0);
        });
        while (!c->ready && GetTickCount64() - began < 1000 && IsWindow(window)) {
          MSG pending{};
          while (PeekMessageW(&pending, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&pending);
            DispatchMessageW(&pending);
          }
          Sleep(1);
        }
        acknowledge.join();
        c->readyDelay = UINT(GetTickCount64() - began);
        if (!c->ready || !(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
          c->bridge.reset();
          c->oleDone = true;
          return 0;
        }
      }
      auto data = new Data();
      auto source = new Source();
      c->dragResult = DoDragDrop(data, source, DROPEFFECT_COPY, &c->effect);
      c->queries = source->queries;
      data->Release();
      source->Release();
      c->bridge.reset();
      c->oleDone = true;
      return 0;
    }
  }
  if (message == WM_CLOSE)
    return 0;
  return DefWindowProcW(window, message, wp, lp);
}
static POINTER_TOUCH_INFO contact(UINT id, POINT point, POINTER_FLAGS flags) {
  POINTER_TOUCH_INFO info{};
  info.pointerInfo.pointerType = PT_TOUCH;
  info.pointerInfo.pointerId = id;
  info.pointerInfo.ptPixelLocation = point;
  info.pointerInfo.pointerFlags = flags;
  info.touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_PRESSURE | TOUCH_MASK_ORIENTATION;
  info.rcContact = {point.x - 2, point.y - 2, point.x + 2, point.y + 2};
  info.orientation = 90;
  info.pressure = 512;
  return info;
}
static Json runCase(Context &c, bool multi, bool deferred = false) {
  c.multi = multi;
  c.deferReady = deferred;
  c.ready = false;
  c.readyDelay = 0;
  c.oleDone = false;
  c.promotionSucceeded = false;
  c.outcomes = Json::array();
  c.bridge.reset();
  c.pointerDowns = c.promotedMessages = c.queries = 0;
  c.dragResult = E_PENDING;
  c.effect = 0;
  unsigned before = c.sink->drops;
  POINT first{90, 90}, last{90, 90};
  ClientToScreen(c.source, &first);
  ClientToScreen(c.target, &last);
  std::atomic<bool> done = false;
  std::atomic<DWORD> injectionError = ERROR_SUCCESS;
  std::atomic<DWORD> guardError = ERROR_SUCCESS;
  std::atomic<unsigned> injectionCalls = 0;
  std::atomic<bool> ownFirst = false, ownLast = false, foregroundBefore = false;
  std::atomic<ULONG_PTR> firstHit = 0, lastHit = 0;
  std::string firstClass, lastClass;
  DWORD firstProcess = 0, lastProcess = 0;
  std::atomic<int> injectionStage = 0;
  auto owned = [&](POINT p) {
    HWND hit = WindowFromPoint(p);
    return hit == c.source || hit == c.target;
  };
  std::thread injector([&] {
    // Wait while the owning thread dispatches show/paint/window-position work;
    // the hit-test guard must use the displayed fixture, not a stale surface.
    Sleep(200);
    injectionStage = 1;
    if (!InitializeTouchInjection(2, TOUCH_FEEDBACK_NONE)) {
      injectionError = GetLastError();
      done = true;
      return;
    }
    std::array<POINTER_TOUCH_INFO, 2> contacts{};
    UINT count = multi ? 2 : 1;
    bool held = false;
    POINT lastInjected = first;
    auto inject = [&](POINTER_FLAGS flags, POINT position) {
      for (UINT i = 0; i < count; ++i) {
        POINT p = position;
        p.x += LONG(i * 30);
        contacts[i] = contact(41 + i, p, flags);
      }
      SetLastError(ERROR_SUCCESS);
      injectionStage = 3;
      ++injectionCalls;
      if (!InjectTouchInput(count, contacts.data())) {
        injectionError = GetLastError();
        return false;
      }
      lastInjected = position;
      return true;
    };
    injectionStage = 2;
    ownFirst = owned(first);
    ownLast = owned(last);
    firstHit = ULONG_PTR(WindowFromPoint(first));
    lastHit = ULONG_PTR(WindowFromPoint(last));
    auto describe = [](HWND window, std::string &text, DWORD &process) {
      wchar_t name[256]{};
      GetClassNameW(window, name, 256);
      for (auto p = name; *p; ++p)
        text.push_back(char(*p));
      GetWindowThreadProcessId(window, &process);
    };
    describe(HWND(firstHit.load()), firstClass, firstProcess);
    describe(HWND(lastHit.load()), lastClass, lastProcess);
    foregroundBefore = GetForegroundWindow() == c.source;
    if (ownFirst && ownLast) {
      if (inject(POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT, first)) {
        held = true;
        Sleep(80);
        if (multi) {
          inject(POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT, first);
          Sleep(120);
        } else {
          POINT threshold = first;
          threshold.x += 20;
          inject(POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT, threshold);
          Sleep(120);
          if (owned(last)) {
            inject(POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT, last);
            Sleep(180);
          }
        }
        if (inject(POINTER_FLAG_UP, lastInjected))
          held = false;
      }
    } else
      guardError = ERROR_ACCESS_DENIED;
    if (held)
      inject(POINTER_FLAG_UP | POINTER_FLAG_CANCELED, lastInjected);
    Sleep(100);
    done = true;
  });
  ULONGLONG deadline = GetTickCount64() + 7000;
  while (GetTickCount64() < deadline && (!done || (!multi && c.promotionSucceeded && !c.oleDone))) {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    Sleep(1);
  }
  injector.join();
  MSG message{};
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  Json result = {{"multi", multi},
                 {"deferred_ready", deferred},
                 {"ready_delay_ms", c.readyDelay},
                 {"injection_error", injectionError.load()},
                 {"guard_error", guardError.load()},
                 {"injection_calls", injectionCalls.load()},
                 {"injection_stage", injectionStage.load()},
                 {"start_owned", ownFirst.load()},
                 {"target_owned", ownLast.load()},
                 {"foreground_before", foregroundBefore.load()},
                 {"source_hwnd", ULONG_PTR(c.source)},
                 {"target_hwnd", ULONG_PTR(c.target)},
                 {"start_hit", firstHit.load()},
                 {"target_hit", lastHit.load()},
                 {"start_hit_class", firstClass},
                 {"target_hit_class", lastClass},
                 {"start_hit_process", firstProcess},
                 {"target_hit_process", lastProcess},
                 {"source_visible", bool(IsWindowVisible(c.source))},
                 {"source_enabled", bool(IsWindowEnabled(c.source))},
                 {"first_point", Json::array({first.x, first.y})},
                 {"last_point", Json::array({last.x, last.y})},
                 {"pointer_downs", c.pointerDowns},
                 {"promotion_succeeded", c.promotionSucceeded},
                 {"outcomes", c.outcomes},
                 {"mouse_handler_entries", c.promotedMessages},
                 {"entry_message", c.entryMessage},
                 {"entry_extra", c.entryExtra},
                 {"drag_hresult", uint32_t(c.dragResult)},
                 {"effect", c.effect},
                 {"query_continue_calls", c.queries},
                 {"drop_count", c.sink->drops - before},
                 {"payload_exact", c.sink->exact}};
  if (guardError != ERROR_SUCCESS) {
    result["blocked"] = true;
    result["reason"] =
        firstClass == "LockScreenBackstopFrame" || lastClass == "LockScreenBackstopFrame"
            ? "lock_screen_covers_fixture; no contacts injected"
            : "private_fixture_not_exposed; no contacts injected";
  }
  bool success = guardError == ERROR_SUCCESS && injectionError == ERROR_SUCCESS &&
                 injectionCalls > 0 && c.pointerDowns == (multi ? 2u : 1u);
  if (multi)
    success &= !c.promotionSucceeded && c.promotedMessages == 0 && c.sink->drops == before;
  else
    success &= c.promotionSucceeded && c.promotedMessages == 1 && c.dragResult == DRAGDROP_S_DROP &&
               c.effect == DROPEFFECT_COPY && c.sink->drops == before + 1 && c.sink->exact &&
               c.queries > 0;
  result["pass"] = success;
  return result;
}
static Json exercise() {
  Context c;
  Json report = {{"supported_by_name", c.bridge.supported()},
                 {"mode", "injected_os_touch_to_native_ole"}};
  auto desktopName = [](HDESK desktop) {
    wchar_t name[256]{};
    DWORD needed = 0;
    if (!GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), &needed))
      return std::string();
    std::string result;
    for (auto p = name; *p; ++p)
      result.push_back(char(*p));
    return result;
  };
  report["thread_desktop"] = desktopName(GetThreadDesktop(GetCurrentThreadId()));
  DWORD session = 0;
  ProcessIdToSessionId(GetCurrentProcessId(), &session);
  report["process_session"] = session;
  report["active_console_session"] = WTSGetActiveConsoleSessionId();
  SetLastError(ERROR_SUCCESS);
  HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
  report["input_desktop_error"] = input ? 0 : GetLastError();
  report["input_desktop"] = input ? desktopName(input) : std::string();
  if (input)
    CloseDesktop(input);
  if (report["input_desktop"] != "Default") {
    report["pass"] = false;
    report["reason"] = "interactive Default desktop unavailable; no contacts injected";
    return report;
  }
  if (!c.bridge.supported()) {
    report["pass"] = false;
    report["reason"] = "named API unavailable; mouse compatibility required";
    return report;
  }
  HWND previous = GetForegroundWindow();
  POINT cursor{};
  GetCursorPos(&cursor);
  HRESULT initialized = OleInitialize(nullptr);
  if (FAILED(initialized)) {
    report["ole_initialize"] = uint32_t(initialized);
    report["pass"] = false;
    return report;
  }
  WNDCLASSW cls{};
  cls.lpfnWndProc = fixtureProc;
  cls.hInstance = GetModuleHandleW(nullptr);
  cls.lpszClassName = L"Waybridge.Private.TouchOle.Probe";
  cls.hbrBackground = static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
  RegisterClassW(&cls);
  RECT work{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  int width = (work.right - work.left - 90) / 2;
  if (width < 220) {
    report["pass"] = false;
    report["reason"] = "insufficient private fixture area";
    OleUninitialize();
    return report;
  }
  width = width > 420 ? 420 : width;
  c.source = CreateWindowExW(WS_EX_TOPMOST, cls.lpszClassName, L"Waybridge temporary touch source",
                             WS_OVERLAPPEDWINDOW, work.left + 20, work.top + 30, width, 300,
                             nullptr, nullptr, cls.hInstance, &c);
  c.target = CreateWindowExW(WS_EX_TOPMOST, cls.lpszClassName, L"Waybridge temporary OLE target",
                             WS_OVERLAPPEDWINDOW, work.left + width + 50, work.top + 30, width, 300,
                             nullptr, nullptr, cls.hInstance, &c);
  c.sink = new Sink();
  HRESULT registered = c.target ? RegisterDragDrop(c.target, c.sink) : E_FAIL;
  if (c.source && c.target && SUCCEEDED(registered)) {
    ShowWindow(c.target, SW_SHOWNOACTIVATE);
    ShowWindow(c.source, SW_SHOWNORMAL);
    SetForegroundWindow(c.source);
    SetFocus(c.source);
    SetWindowPos(c.target, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(c.source, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    UpdateWindow(c.target);
    UpdateWindow(c.source);
    report["no_contact"] = TouchOle::name(c.bridge.promote(c.source, 0).code);
    report["single"] = runCase(c, false);
    if (report["single"].value("blocked", false)) {
      report["blocked"] = true;
      report["reason"] = report["single"]["reason"];
      report["pass"] = false;
    } else {
      report["deferred"] = runCase(c, false, true);
      SetForegroundWindow(c.source);
      SetFocus(c.source);
      report["multi"] = runCase(c, true);
      report["pass"] = report["single"]["pass"] == true && report["deferred"]["pass"] == true &&
                       report["multi"]["pass"] == true;
    }
  } else {
    report["pass"] = false;
    report["registration_error"] = uint32_t(registered);
  }
  bool ownsForeground = GetForegroundWindow() == c.source || GetForegroundWindow() == c.target;
  POINT finalCursor{};
  GetCursorPos(&finalCursor);
  HWND cursorWindow = WindowFromPoint(finalCursor);
  bool ownsCursor = cursorWindow == c.source || cursorWindow == c.target;
  if (SUCCEEDED(registered))
    RevokeDragDrop(c.target);
  if (c.source)
    DestroyWindow(c.source);
  if (c.target)
    DestroyWindow(c.target);
  c.sink->Release();
  if (ownsForeground && IsWindow(previous))
    SetForegroundWindow(previous);
  if (ownsForeground && ownsCursor)
    SetCursorPos(cursor.x, cursor.y);
  OleUninitialize();
  return report;
}
int main(int argc, char **argv) {
  if (!SetProcessDpiAwarenessContext(reinterpret_cast<DPI_AWARENESS_CONTEXT>(-4)))
    SetProcessDPIAware();
  auto report = inventory();
  if (argc == 2 && std::string(argv[1]) == "--exercise")
    report["exercise"] = exercise();
  std::cout << report.dump() << '\n';
  return report.contains("exercise") && report["exercise"]["pass"] != true ? 1 : 0;
}
