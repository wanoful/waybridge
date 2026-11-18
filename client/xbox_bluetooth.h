#pragma once
#include <windows.h>
#include <xinput.h>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Xbox Series controller (045e:0b13), 17-byte Bluetooth LE input report.
// See SDL's Xbox Bluetooth state packet implementation for button/axis layout.
// This backend opens just the matching gamepad collection for shared reads.
class XboxBluetoothGamepad {
public:
  using Report = std::array<BYTE, 17>;

  XboxBluetoothGamepad() = default;
  XboxBluetoothGamepad(const XboxBluetoothGamepad &) = delete;
  XboxBluetoothGamepad &operator=(const XboxBluetoothGamepad &) = delete;
  ~XboxBluetoothGamepad() { close(); }

  static std::wstring find() {
    UINT count = 0;
    if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) == UINT(-1) ||
        !count || count > 4096)
      return {};
    std::vector<RAWINPUTDEVICELIST> devices(count);
    UINT found = GetRawInputDeviceList(devices.data(), &count, sizeof(RAWINPUTDEVICELIST));
    if (found == UINT(-1)) return {};
    for (UINT i = 0; i < found; ++i) {
      const auto &device = devices[i];
      if (device.dwType != RIM_TYPEHID) continue;
      RID_DEVICE_INFO info{};
      info.cbSize = sizeof(info);
      UINT size = sizeof(info);
      if (GetRawInputDeviceInfoW(device.hDevice, RIDI_DEVICEINFO, &info, &size) == UINT(-1) ||
          info.hid.dwVendorId != 0x045e || info.hid.dwProductId != 0x0b13 ||
          info.hid.usUsagePage != 1 || info.hid.usUsage != 5)
        continue;
      UINT length = 0;
      if (GetRawInputDeviceInfoW(device.hDevice, RIDI_DEVICENAME, nullptr, &length) == UINT(-1) ||
          !length || length > 4096)
        continue;
      std::wstring path(length + 1, L'\0');
      if (GetRawInputDeviceInfoW(device.hDevice, RIDI_DEVICENAME, path.data(), &length) == UINT(-1))
        continue;
      path.resize(path.find(L'\0'));
      return path;
    }
    return {};
  }

  static bool decode(const Report &report, XINPUT_GAMEPAD &state) {
    if (report[0] != 1 || report[13] > 8) return false;
    auto word = [&](size_t offset) { return int(report[offset]) | (int(report[offset + 1]) << 8); };
    int lt = word(9), rt = word(11);
    if (lt > 1023 || rt > 1023) return false;
    constexpr WORD up = XINPUT_GAMEPAD_DPAD_UP, down = XINPUT_GAMEPAD_DPAD_DOWN;
    constexpr WORD left = XINPUT_GAMEPAD_DPAD_LEFT, right = XINPUT_GAMEPAD_DPAD_RIGHT;
    constexpr WORD hat[] = {0, up, up | right, right, down | right, down, down | left, left, up | left};
    constexpr WORD buttons[] = {
      XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, 0, XINPUT_GAMEPAD_X,
      XINPUT_GAMEPAD_Y, 0, XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER,
      0, 0, XINPUT_GAMEPAD_BACK, XINPUT_GAMEPAD_START, 0,
      XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB,
    };
    WORD mask = hat[report[13]];
    int pressed = word(14);
    for (size_t i = 0; i < std::size(buttons); ++i)
      if (pressed & (1 << i)) mask |= buttons[i];
    state = {mask, BYTE((lt * 255 + 511) / 1023), BYTE((rt * 255 + 511) / 1023),
      SHORT(word(1) - 32768), SHORT(32767 - word(3)),
      SHORT(word(5) - 32768), SHORT(32767 - word(7))};
    return true;
  }

  bool read(const std::wstring &path, XINPUT_GAMEPAD &state) {
    if (file == INVALID_HANDLE_VALUE) {
      file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
      if (file == INVALID_HANDLE_VALUE) return false;
      io.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      if (!io.hEvent) { close(); return false; }
    }
    // Bound the number of queued reports consumed per timer tick.
    for (int i = 0; i < 8; ++i) {
      DWORD bytes = 0;
      if (!pending) {
        ResetEvent(io.hEvent);
        if (!ReadFile(file, report.data(), DWORD(report.size()), &bytes, &io)) {
          if (GetLastError() != ERROR_IO_PENDING) { close(); return false; }
          pending = true;
        }
      }
      if (pending) {
        if (!GetOverlappedResult(file, &io, &bytes, FALSE)) {
          if (GetLastError() == ERROR_IO_INCOMPLETE) break;
          pending = false;
          close();
          return false;
        }
        pending = false;
      }
      if (!bytes) { close(); return false; }
      if (bytes == report.size()) decode(report, latest);
    }
    state = latest;
    return true;
  }

  void close() {
    if (file != INVALID_HANDLE_VALUE) {
      if (pending) {
        CancelIoEx(file, &io);
        DWORD ignored = 0;
        GetOverlappedResult(file, &io, &ignored, TRUE);
      }
      CloseHandle(file);
      file = INVALID_HANDLE_VALUE;
    }
    if (io.hEvent) CloseHandle(io.hEvent);
    io = {};
    pending = false;
    latest = {};
  }

private:
  HANDLE file = INVALID_HANDLE_VALUE;
  OVERLAPPED io{};
  bool pending = false;
  Report report{};
  XINPUT_GAMEPAD latest{};
};
