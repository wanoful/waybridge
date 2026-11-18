#include <windows.h>
#include <windowsx.h>
#include "window_semantics.h"
#include "image_geometry.h"
#include <iostream>
#include <stdexcept>
using Json = nlohmann::json;
static void require(bool ok, const char *why) {
  if (!ok)
    throw std::runtime_error(why);
}
static HWND window(int id) {
  std::wstring title = L"Waybridge semantics " + std::to_wstring(id);
  return FindWindowW(L"WaybridgeRemote", title.c_str());
}
static Json state(HWND hwnd) {
  require(hwnd != nullptr, "missing window");
  RECT rect, client;
  GetWindowRect(hwnd, &rect);
  GetClientRect(hwnd, &client);
  MONITORINFO monitor{};
  monitor.cbSize = sizeof(monitor);
  GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
  DWORD style = DWORD(GetWindowLongPtrW(hwnd, GWL_STYLE));
  int owner = 0;
  for (int i = 1; i <= 3; ++i)
    if (window(i) == GetWindow(hwnd, GW_OWNER) && window(i))
      owner = i;
  MINMAXINFO limits{};
  limits.ptMaxTrackSize = {20000, 20000};
  limits.ptMaxSize = {20000, 20000};
  SendMessageW(hwnd, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&limits));
  return {{"rect", {rect.left, rect.top, rect.right, rect.bottom}},
          {"client", {client.right, client.bottom}},
          {"monitor",
           {monitor.rcMonitor.left, monitor.rcMonitor.top, monitor.rcMonitor.right,
            monitor.rcMonitor.bottom}},
          {"work",
           {monitor.rcWork.left, monitor.rcWork.top, monitor.rcWork.right, monitor.rcWork.bottom}},
          {"maximized", IsZoomed(hwnd) != FALSE},
          {"minimized", IsIconic(hwnd) != FALSE},
          {"visible", IsWindowVisible(hwnd) != FALSE},
          {"enabled", IsWindowEnabled(hwnd) != FALSE},
          {"foreground", GetForegroundWindow() == hwnd},
          {"owner", owner},
          {"caption", (style & WS_CAPTION) != 0},
          {"appwindow", (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_APPWINDOW) != 0},
          {"min_track", {limits.ptMinTrackSize.x, limits.ptMinTrackSize.y}},
          {"max_track", {limits.ptMaxTrackSize.x, limits.ptMaxTrackSize.y}}};
}
int main(int argc, char **argv) {
  try {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (argc < 2)
      return 1;
    std::string mode = argv[1];
    if (mode == "self") {
      RECT viewport{0,0,2560,1600};
      for (auto size : {SIZE{2559,1600}, SIZE{2559,1599}, SIZE{2561,1601}}) {
        auto fitted=fitImageRect(viewport,size.cx,size.cy,true,144);
        require(EqualRect(&fitted,&viewport), "fractional-DPI fullscreen must fill every pixel");
      }
      auto letterbox=fitImageRect(viewport,1920,1080,true,144);
      require(letterbox.top==80 && letterbox.bottom==1520 && letterbox.right==2560,
              "different fullscreen aspect ratio must retain letterboxing");
      auto odd=fitImageRect({0,0,1001,701},700,450,false,120);
      require(odd.right-odd.left==1001 && odd.bottom-odd.top==644,
              "aspect fit uses rounded extents without losing a centered pixel");
      NativeWindowState s;
      s.metadataReady = true;
      s.constraints({{"min_size", {320, 200}}, {"max_size", {640, 480}}});
      WNDCLASSW wc{};
      wc.hInstance = GetModuleHandleW(nullptr);
      wc.lpfnWndProc = DefWindowProcW;
      wc.lpszClassName = L"SemanticsProbe";
      RegisterClassW(&wc);
      HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"probe", WS_OVERLAPPEDWINDOW, 100, 100, 500,
                                  350, nullptr, nullptr, wc.hInstance, nullptr);
      require(hwnd != nullptr, "probe create");
      for (UINT dpi : {96u, 168u, 120u}) {
        MINMAXINFO m{};
        m.ptMaxTrackSize = {20000, 20000};
        m.ptMaxSize = {20000, 20000};
        s.minMax(hwnd, dpi, false, &m);
        auto min = NativeWindowState::outer(hwnd, dpi, false, 320, 200),
             max = NativeWindowState::outer(hwnd, dpi, false, 640, 480);
        require(m.ptMinTrackSize.x == min.cx && m.ptMinTrackSize.y == min.cy &&
                    m.ptMaxTrackSize.x == max.cx && m.ptMaxTrackSize.y == max.cy,
                "DPI min/max");
        WINDOWPOS p{};
        p.cx = 1;
        p.cy = 20000;
        s.clamp(hwnd, dpi, false, &p);
        require(p.cx == min.cx && p.cy == max.cy, "DPI constrained WINDOWPOS");
      }
      Json input = {
          {"origin_x", 0},
          {"origin_y", 0},
          {"width", 100},
          {"height", 100},
          {"layers",
           Json::array(
               {{{"x", 0},
                 {"y", 0},
                 {"width", 100},
                 {"height", 100},
                 {"rects", Json::array({Json::array({true, 0, 0, 50, 100}),
                                        Json::array({false, 20, 20, 20, 20})})}},
                {{"x", 70}, {"y", 70}, {"width", 10}, {"height", 10}, {"rects", nullptr}}})}};
      s.input(input);
      require(s.accepts(10, 10, 0, 0, 100, 100) && !s.accepts(25, 25, 0, 0, 100, 100) &&
                  !s.accepts(90, 10, 0, 0, 100, 100) && s.accepts(75, 75, 0, 0, 100, 100),
              "input union/subtraction");
      require(s.accepts(25, 25, 1, 0, 100, 100), "stale origin must be conservative");
      input["layers"] = Json::array();
      for (int i = 0; i < 65; ++i)
        input["layers"].push_back(
            {{"x", 0}, {"y", 0}, {"width", 1}, {"height", 1}, {"rects", nullptr}});
      s.input(input);
      require(!s.regionValid && s.accepts(99, 99, 0, 0, 100, 100), "bounded input layers");
      RECT before;
      GetWindowRect(hwnd, &before);
      s.setFullscreen(hwnd, true, false);
      auto full = state(hwnd);
      require(full["rect"] == full["monitor"] && !full["caption"].get<bool>() &&
                  !full["maximized"].get<bool>(),
              "true fullscreen");
      s.setFullscreen(hwnd, false, false);
      RECT after;
      GetWindowRect(hwnd, &after);
      require(EqualRect(&before, &after) && !IsWindowVisible(hwnd),
              "hidden normal placement restore");
      ShowWindow(hwnd, SW_MAXIMIZE);
      s.setFullscreen(hwnd, true, false);
      s.setFullscreen(hwnd, false, false);
      require(IsZoomed(hwnd), "maximized placement restore");
      DestroyWindow(hwnd);
      std::cout << "PASS: native fullscreen/restore, 100/125/175% DPI constraints, input "
                   "union/subtraction/stale geometry/resource bound\n";
      return 0;
    }
    int id = argc > 2 ? std::stoi(argv[2]) : 1;
    HWND hwnd = window(id);
    if (mode == "exists") {
      return hwnd && IsWindow(hwnd) ? 0 : 2;
    }
    require(hwnd && IsWindow(hwnd), "missing live window");
    if (mode == "state") {
      std::cout << state(hwnd).dump() << '\n';
      return 0;
    }
    if (mode == "sysrestore") {
      SendMessageW(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
      return 0;
    }
    if (mode == "key") {
      require(argc > 3, "key id code");
      int code = std::stoi(argv[3]);
      SetForegroundWindow(hwnd);
      SendMessageW(hwnd, WM_ACTIVATE, WA_ACTIVE, 0);
      Sleep(100);
      SendMessageW(hwnd, WM_KEYDOWN, 0, LPARAM(1) | (code << 16));
      SendMessageW(hwnd, WM_KEYUP, 0,
                   LPARAM(1) | (code << 16) | (LPARAM(1) << 30) | (LPARAM(1) << 31));
      return 0;
    }
    if (mode == "resize") {
      require(argc > 4, "resize id clientWidth clientHeight");
      RECT r{0, 0, std::stoi(argv[3]), std::stoi(argv[4])};
      DWORD style = DWORD(GetWindowLongPtrW(hwnd, GWL_STYLE));
      if (style & WS_CAPTION)
        AdjustWindowRectExForDpi(&r, style, FALSE, DWORD(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)), 96);
      SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                   SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER);
      return 0;
    }
    if (mode == "size-begin" || mode == "size-end") {
      SendMessageW(hwnd, mode == "size-begin" ? WM_ENTERSIZEMOVE : WM_EXITSIZEMOVE, 0, 0);
      return 0;
    }
    if (mode == "move") {
      require(argc > 4, "move id x y");
      SetWindowPos(hwnd, nullptr, std::stoi(argv[3]), std::stoi(argv[4]), 0, 0,
                   SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
      return 0;
    }
    if (mode == "move-burst") {
      const auto move = [&](int x, int y) {
        require(SetWindowPos(hwnd, nullptr, x, y, 0, 0,
                             SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER) != FALSE,
                "move-burst placement");
      };
      const auto origin = [&]() {
        POINT p{};
        require(ClientToScreen(hwnd, &p) != FALSE, "move-burst client origin");
        return Json::array({p.x, p.y});
      };
      // Drive the production window's position-change messages without system
      // pointer input. The caller runs this only in a private Wine desktop.
      SendMessageW(hwnd, WM_ENTERSIZEMOVE, 0, 0);
      for (int i = 0; i < 48; ++i) {
        move(450 + 5 * i, 320 + 3 * i);
        Sleep(2);
      }
      auto paused = origin();
      Sleep(100); // Pending viewport must arrive even while the drag remains held.
      move(800, 480);
      auto released = origin();
      SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);
      std::cout << Json::array({paused, released}).dump() << '\n';
      return 0;
    }
    if (mode == "pixel") {
      require(argc > 4, "pixel id x y");
      SetForegroundWindow(hwnd);
      SendMessageW(hwnd, WM_ACTIVATE, WA_ACTIVE, 0);
      SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
      RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
      Sleep(100);
      POINT point{std::stoi(argv[3]), std::stoi(argv[4])};
      ClientToScreen(hwnd, &point);
      HDC dc = GetDC(nullptr);
      auto value = GetPixel(dc, point.x, point.y);
      ReleaseDC(nullptr, dc);
      std::cout << int(GetRValue(value)) << ' ' << int(GetGValue(value)) << ' '
                << int(GetBValue(value)) << '\n';
      return 0;
    }
    if (mode == "hide" || mode == "show") {
      ShowWindow(hwnd, mode == "hide" ? SW_HIDE : SW_SHOWNOACTIVATE);
      return 0;
    }
    if (mode == "minimize") {
      ShowWindow(hwnd, SW_MINIMIZE);
      return 0;
    }
    if (mode == "restore") {
      ShowWindow(hwnd, SW_RESTORE);
      return 0;
    }
    if (mode == "hit") {
      require(argc > 4, "hit id x y");
      POINT p{std::stoi(argv[3]), std::stoi(argv[4])};
      ClientToScreen(hwnd, &p);
      std::cout << SendMessageW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(p.x, p.y)) << '\n';
      return 0;
    }
    if (mode == "close") {
      PostMessageW(hwnd, WM_CLOSE, 0, 0);
      return 0;
    }
    return 1;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 2;
  }
}
