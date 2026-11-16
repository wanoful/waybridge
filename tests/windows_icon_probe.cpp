#include <windows.h>
#include <objbase.h>
#include "native_icon.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
static void require(bool ok, const char *why) {
  if (!ok)
    throw std::runtime_error(why);
}
static void checkIcon(HWND hwnd, WPARAM kind, int size) {
  auto icon = reinterpret_cast<HICON>(SendMessageW(hwnd, WM_GETICON, kind, 0));
  require(icon != nullptr, "WM_GETICON");
  ICONINFO info{};
  require(GetIconInfo(icon, &info), "GetIconInfo");
  BITMAP bitmap{};
  GetObjectW(info.hbmColor, sizeof(bitmap), &bitmap);
  require(info.fIcon && bitmap.bmWidth == size && bitmap.bmHeight == size,
          "native icon DPI geometry");
  DeleteObject(info.hbmColor);
  DeleteObject(info.hbmMask);
}
int main(int argc, char **argv) {
  try {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (argc < 2)
      return 1;
    if (std::string(argv[1]) == "self") {
      require(argc == 3, "self PNG");
      OleInitialize(nullptr);
      std::ifstream file(argv[2], std::ios::binary);
      std::vector<uint8_t> png((std::istreambuf_iterator<char>(file)), {});
      ImageDecoder decoder;
      NativeIcon icon;
      WNDCLASSW wc{};
      wc.hInstance = GetModuleHandleW(nullptr);
      wc.lpfnWndProc = DefWindowProcW;
      wc.lpszClassName = L"NativeIconProbe";
      RegisterClassW(&wc);
      HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"probe", WS_OVERLAPPEDWINDOW, 0, 0, 300,
                                  200, nullptr, nullptr, wc.hInstance, nullptr);
      require(hwnd != nullptr, "probe window");
      require(icon.update(png, decoder), "valid PNG rejected");
      for (UINT dpi : {96u, 168u, 120u, 384u}) {
        icon.apply(hwnd, dpi);
        checkIcon(hwnd, ICON_BIG, GetSystemMetricsForDpi(SM_CXICON, dpi));
        checkIcon(hwnd, ICON_SMALL, GetSystemMetricsForDpi(SM_CXSMICON, dpi));
      }
      auto defaultBadge = icon.badgedPng(64);
      std::vector<uint8_t> rendered;
      require(decoder.decode(defaultBadge.data(), defaultBadge.size(), 64, 64, false, rendered), "badged PNG roundtrip");
      auto blue = [&](const std::vector<uint8_t> &image) {
        size_t at = (16 * 64 + 59) * 4;
        return image[at] == 0xeb && image[at + 1] == 0x63 && image[at + 2] == 0x25 && image[at + 3] == 255;
      };
      require(blue(rendered), "upper-right blue Waybridge badge missing");
      require(rendered[(50 * 64 + 10) * 4 + 2] == 255, "badge changed original lower-left pixels");
      std::vector<uint8_t> green(64 * 64 * 4, 0);
      for (size_t i = 0; i < green.size(); i += 4) { green[i + 1] = 255; green[i + 3] = 255; }
      auto custom = IconBadge::png(green, 64);
      auto oldHandle = SendMessageW(hwnd, WM_GETICON, ICON_SMALL, 0);
      require(IconBadge::configure(custom, decoder), "custom badge rejected");
      icon.apply(hwnd, 384);
      require(SendMessageW(hwnd, WM_GETICON, ICON_SMALL, 0) != oldHandle, "same-DPI badge change not applied");
      auto customPng = icon.badgedPng(64);
      require(decoder.decode(customPng.data(), customPng.size(), 64, 64, false, rendered), "custom PNG roundtrip");
      require(rendered[(16 * 64 + 59) * 4 + 1] == 255, "custom badge not composed");
      auto revision = IconBadge::state().revision;
      require(!IconBadge::configure(png, decoder), "128px custom badge accepted");
      require(!IconBadge::configure({1, 2, 3}, decoder), "invalid custom badge accepted");
      require(IconBadge::state().revision == revision, "invalid custom badge changed active settings");
      require(IconBadge::configure({}, decoder), "badge reset");
      require(icon.badgedPng(64) == defaultBadge, "custom badge permanently changed original master");
      auto owned = icon.createOwnedIcon(144);
      require(owned != nullptr, "caller-owned tray icon");
      DestroyIcon(owned);
      require(!icon.update({1, 2, 3}, decoder), "invalid PNG accepted");
      require(!icon.update(std::vector<uint8_t>(96 * 1024 + 1), decoder), "oversized PNG accepted");
      nlohmann::json message = {
          {"id", 1u}, {"revision", 2u}, {"app_id", "org.example.App"}, {"png", nullptr}};
      require(NativeIcon::matches(message, 1, "org.example.App", 2), "current icon response");
      require(!NativeIcon::matches(message, 2, "org.example.App", 2), "wrong window accepted");
      require(!NativeIcon::matches(message, 1, "other.App", 2), "wrong app accepted");
      require(!NativeIcon::matches(message, 1, "org.example.App", 3), "stale icon accepted");
      auto bad = message;
      bad["revision"] = "2";
      require(!NativeIcon::matches(bad, 1, "org.example.App", 2), "wrong revision type");
      bad = message;
      bad["png"] = nlohmann::json::array();
      require(!NativeIcon::matches(bad, 1, "org.example.App", 2), "wrong PNG type");
      bad = message;
      bad["png"] = "AA==";
      bad["width"] = 1024;
      bad["height"] = 128;
      require(!NativeIcon::matches(bad, 1, "org.example.App", 2), "wrong icon geometry");
      DWORD beforeGdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS),
            beforeUser = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
      for (int i = 0; i < 300; ++i) {
        auto oldLarge = reinterpret_cast<HICON>(SendMessageW(hwnd, WM_GETICON, ICON_BIG, 0));
        auto oldSmall = reinterpret_cast<HICON>(SendMessageW(hwnd, WM_GETICON, ICON_SMALL, 0));
        require(icon.update(png, decoder), "repeated PNG update");
        icon.apply(hwnd, i & 1 ? 96 : 168);
        ICONINFO stale{};
        require(!GetIconInfo(oldLarge, &stale) && !GetIconInfo(oldSmall, &stale),
                "old HICON was not destroyed");
      }
      require(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= beforeGdi + 4, "GDI leak");
      require(GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS) <= beforeUser + 4, "HICON leak");
      icon.clear();
      icon.apply(hwnd, 168);
      checkIcon(hwnd, ICON_BIG, GetSystemMetricsForDpi(SM_CXICON, 168));
      auto fallbackPng = icon.badgedPng(64);
      require(decoder.decode(fallbackPng.data(), fallbackPng.size(), 64, 64, false, rendered) && blue(rendered), "fallback missing badge");
      checkIcon(hwnd, ICON_SMALL, GetSystemMetricsForDpi(SM_CXSMICON, 168));
      require(NativeIcon::appId("../../app").empty() &&
                  NativeIcon::appId("org.example.App") == "org.example.App",
              "app-id normalization");
      DestroyWindow(hwnd);
      std::cout << "PASS: native WM_SETICON/WM_GETICON large/small, 100/125/175/400% DPI, invalid "
                   "PNG/size rejection, default/custom/reset badges, generic fallback and 300 resource replacements\n";
      return 0;
    }
    HWND hwnd = FindWindowW(L"WaybridgeRemote", L"Waybridge fixture 1");
    require(hwnd && IsWindowVisible(hwnd), "remote window not ready");
    if (std::string(argv[1]) == "ready")
      return 0;
    if (std::string(argv[1]) == "key") {
      require(argc == 3, "key CODE");
      int code = std::stoi(argv[2]);
      SetForegroundWindow(hwnd);
      Sleep(50);
      SendMessageW(hwnd, WM_KEYDOWN, 0, 1 | (code << 16));
      SendMessageW(hwnd, WM_KEYUP, 0,
                   LPARAM(1) | (code << 16) | (LPARAM(1) << 30) | (LPARAM(1) << 31));
      return 0;
    }
    auto large = SendMessageW(hwnd, WM_GETICON, ICON_BIG, 0),
         small = SendMessageW(hwnd, WM_GETICON, ICON_SMALL, 0);
    require(large && small, "remote icons missing");
    HDC dc = GetWindowDC(hwnd);
    int red = 0, blue = 0, green = 0;
    for (int y = 5; y < 25; ++y)
      for (int x = 5; x < 55; ++x) {
        COLORREF p = GetPixel(dc, x, y);
        int r = GetRValue(p), g = GetGValue(p), b = GetBValue(p);
        if (r > 220 && g < 40 && b < 40)
          ++red;
        if (b > 220 && r < 40 && g < 40)
          ++blue;
        if (g > 220 && r < 40 && b < 40)
          ++green;
      }
    ReleaseDC(hwnd, dc);
    std::cout << "ICON " << large << ' ' << small << ' ' << red << ' ' << blue << ' ' << green
              << '\n';
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 2;
  }
}
