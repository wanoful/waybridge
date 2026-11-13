#include <windows.h>
#include <wincrypt.h>
#include "native_cursor.h"
#include <iostream>
#include <stdexcept>
using Json = nlohmann::json;
static void require(bool ok, const char *why) {
  if (!ok)
    throw std::runtime_error(why);
}
static Json bitmap(uint64_t generation, int width = 32, int height = 24) {
  std::vector<uint8_t> rgba(size_t(width) * height * 4);
  for (size_t p = 0; p < rgba.size(); p += 4) {
    rgba[p] = 255;
    rgba[p + 3] = 255;
  }
  DWORD size = 0;
  CryptBinaryToStringA(rgba.data(), DWORD(rgba.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                       nullptr, &size);
  std::string text(size, '\0');
  CryptBinaryToStringA(rgba.data(), DWORD(rgba.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                       text.data(), &size);
  text.resize(size);
  if (!text.empty() && text.back() == '\0')
    text.pop_back();
  return {{"id", 1u},
          {"generation", generation},
          {"kind", "bitmap"},
          {"format", "rgba8-premultiplied"},
          {"width", width},
          {"height", height},
          {"scale", 2.0},
          {"hotspot_x", std::min(6, width - 1)},
          {"hotspot_y", std::min(10, height - 1)},
          {"pixels", text}};
}
static void geometry(HCURSOR cursor, int width, int height, int x, int y) {
  ICONINFO info{};
  require(GetIconInfo(cursor, &info), "GetIconInfo");
  BITMAP color{};
  GetObjectW(info.hbmColor, sizeof(color), &color);
  DeleteObject(info.hbmColor);
  DeleteObject(info.hbmMask);
  std::cout << "CURSOR " << color.bmWidth << ' ' << color.bmHeight << ' ' << info.xHotspot << ' '
            << info.yHotspot << '\n';
  require(color.bmWidth == width && color.bmHeight == height && int(info.xHotspot) == x &&
              int(info.yHotspot) == y,
          "cursor geometry/hotspot");
}
int main(int argc, char **argv) {
  try {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (argc < 2)
      return 1;
    if (std::string(argv[1]) == "self") {
      NativeCursor cursor;
      auto m = bitmap(1);
      require(cursor.update(m), "valid bitmap rejected");
      geometry(cursor.handle(1, 96), 16, 12, 3, 5);
      geometry(cursor.handle(1, 168), 28, 21, 5, 9);
      require(cursor.handle(2, 168) == LoadCursorW(nullptr, IDC_ARROW), "cross-window cursor leak");
      require(!cursor.update(m), "stale generation accepted");
      auto bad = bitmap(2);
      bad["width"] = uint64_t(1) << 32;
      require(!cursor.update(bad), "oversized bitmap accepted");
      bad = bitmap(2);
      bad["pixels"] = "AAAA";
      require(!cursor.update(bad), "truncated pixels accepted");
      bad = bitmap(2);
      bad["hotspot_x"] = 32;
      require(!cursor.update(bad), "invalid hotspot accepted");
      bad = bitmap(2);
      bad["scale"] = 0;
      require(!cursor.update(bad), "invalid scale accepted");
      DWORD beforeGdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS),
            beforeUser = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
      for (uint64_t i = 2; i < 502; ++i) {
        require(cursor.update(bitmap(i)), "animation update");
        require(cursor.handle(1, i & 1 ? 96 : 168) != nullptr, "bitmap handle");
      }
      require(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= beforeGdi + 4,
              "GDI resource leak");
      require(GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS) <= beforeUser + 4,
              "cursor handle leak");
      require(cursor.update({{"id", 1u}, {"generation", 502u}, {"kind", "hidden"}}),
              "hidden update");
      require(cursor.handle(1, 96) == nullptr, "hidden cursor");
      require(
          cursor.update({{"id", 1u}, {"generation", 503u}, {"kind", "named"}, {"name", "text"}}),
          "text update");
      require(cursor.handle(1, 96) == LoadCursorW(nullptr, IDC_IBEAM), "named cursor");
      require(cursor.update(bitmap(504, 1, 1)), "base64 double padding");
      geometry(cursor.handle(1, 96), 1, 1, 0, 0);
      require(cursor.update(bitmap(505, 2, 1)), "base64 single padding");
      geometry(cursor.handle(1, 96), 1, 1, 0, 0);
      auto noncanonical = bitmap(506, 1, 1);
      auto text = noncanonical.at("pixels").get<std::string>();
      text[text.size() - 3] = 'x';
      noncanonical["pixels"] = text;
      require(!cursor.update(noncanonical), "noncanonical base64 padding bits");
      std::cout << "PASS: Win32 cursor construction, DPI/hotspot, hidden/named, stale/malformed "
                   "rejection and bounded GDI/USER resources\n";
      return 0;
    }
    if (std::string(argv[1]) == "reference") {
      require(argc == 4, "reference NAME DPI");
      NativeCursor cursor;
      require(cursor.update({{"id", 1u}, {"generation", 1u}, {"kind", "named"}, {"name", argv[2]}}),
              "reference update");
      WNDCLASSW wc{};
      wc.hInstance = GetModuleHandleW(nullptr);
      wc.hCursor = std::string(argv[2]) == "native-resize"
                       ? LoadCursorW(nullptr, IDC_SIZEWE)
                       : cursor.handle(1, UINT(std::stoi(argv[3])));
      wc.lpfnWndProc = DefWindowProcW;
      wc.lpszClassName = L"CursorReference";
      RegisterClassW(&wc);
      HWND reference = CreateWindowExW(0, wc.lpszClassName, L"cursor", WS_POPUP | WS_VISIBLE, 0, 0,
                                       100, 100, nullptr, nullptr, wc.hInstance, nullptr);
      SetForegroundWindow(reference);
      SetCursorPos(20, 20);
      SetCursor(wc.hCursor);
      // Flush Wine's X11 cursor update before the external XFixes snapshot.
      for (int i = 0; i < 10; ++i) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
          TranslateMessage(&message);
          DispatchMessageW(&message);
        }
        Sleep(10);
      }
      std::cout << "READY" << std::endl;
      Sleep(1000);
      DestroyWindow(reference);
      return 0;
    }
    HWND hwnd = FindWindowW(L"WaybridgeRemote", L"Waybridge fixture 1");
    require(hwnd != nullptr, "remote window missing");
    SetForegroundWindow(hwnd);
    if (std::string(argv[1]) == "edge") {
      // Deliver a non-client hit while over the existing client X window.
      // Without an X window manager Wine's outer frame may sit on X's root
      // window, whose cursor cannot reflect a Windows SetCursor call.
      SendMessageW(hwnd, WM_SETCURSOR, reinterpret_cast<WPARAM>(hwnd),
                   MAKELPARAM(HTRIGHT, WM_MOUSEMOVE));
      Sleep(100);

      return 0;
    }
    require(argc >= 4, "hover X kind [geometry]");
    int x = std::stoi(argv[2]);
    POINT point{MulDiv(x, 168, 96), 175};
    ClientToScreen(hwnd, &point);
    SetCursorPos(point.x, point.y);
    SendMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(MulDiv(x, 168, 96), 175));
    Sleep(250);
    SendMessageW(hwnd, WM_SETCURSOR, reinterpret_cast<WPARAM>(hwnd),
                 MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 2;
  }
}
