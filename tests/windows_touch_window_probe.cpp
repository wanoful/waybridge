#include "touch_window.h"
#include <iostream>
#include <stdexcept>
static void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
  auto value = DefWindowProcW(hwnd, message, wp, lp);
  if (message == WM_GETMINMAXINFO) {
    auto limits = reinterpret_cast<MINMAXINFO *>(lp);
    limits->ptMinTrackSize = {180, 140}; limits->ptMaxTrackSize = {500, 400};
  }
  return value;
}
int main() {
  try {
    SetProcessDPIAware();
    WNDCLASSW klass{}; klass.lpfnWndProc = procedure; klass.hInstance = GetModuleHandleW(nullptr);
    klass.lpszClassName = L"WaybridgeTouchWindowFixture";
    require(RegisterClassW(&klass) != 0, "register fixture");
    auto hwnd = CreateWindowW(klass.lpszClassName, L"private touch move fixture", WS_OVERLAPPEDWINDOW,
                              200, 200, 320, 240, nullptr, nullptr, klass.hInstance, nullptr);
    require(hwnd != nullptr, "create fixture"); ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    RECT initial{}; GetWindowRect(hwnd, &initial);
    POINT press{initial.left + 90, initial.top + 15}, position{press.x + 75, press.y + 45};
    TouchWindowManipulation operation;
    require(operation.begin(hwnd, "move", 0, initial, press, position), "begin contact move");
    RECT moved{}; GetWindowRect(hwnd, &moved);
    require(moved.left == initial.left + 75 && moved.top == initial.top + 45 &&
            moved.right - moved.left == initial.right - initial.left, "contact move used wrong position or size");
    operation.end();
    initial = moved; press = {initial.left, initial.top + 80}; position = {initial.right + 100, press.y};
    require(operation.begin(hwnd, "resize", 4, initial, press, position), "begin left resize");
    RECT resized{}; GetWindowRect(hwnd, &resized);
    require(resized.right == initial.right && resized.right - resized.left == 180 && resized.top == initial.top,
            "left-edge minimum must retain opposite edge");
    operation.end(); initial = resized; press = {initial.right, initial.bottom};
    position = {press.x + 2000, press.y + 2000};
    require(operation.begin(hwnd, "resize", 10, initial, press, position), "begin bottom-right resize");
    GetWindowRect(hwnd, &resized);
    require(resized.left == initial.left && resized.top == initial.top &&
            resized.right - resized.left == 500 && resized.bottom - resized.top == 400,
            "resize exceeded application maximum");
    require(!operation.begin(hwnd, "resize", 3, resized, press, position), "accepted contradictory edges");
    operation.end();
    ShowWindow(hwnd, SW_MAXIMIZE); GetWindowRect(hwnd, &initial);
    press = {initial.left + (initial.right-initial.left)/2, initial.top + 20};
    position = {press.x + 35, press.y + 35};
    require(operation.begin(hwnd, "move", 0, initial, press, position) && !IsZoomed(hwnd),
            "touch move did not restore maximized window");
    DestroyWindow(hwnd);
    require(!operation.update(position), "dead target continued moving");
    std::cout << "PASS: contact-based real HWND move, anchored min/max edge resize, invalid edges, maximized restore and target destruction\n";
    return 0;
  } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
