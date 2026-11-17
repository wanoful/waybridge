#pragma once
#include <windows.h>
#include <xinput.h>
#include "third_party/json.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>

// Controller-thread XInput adapter. It never calls XInputEnable (which changes
// process-wide reporting), and does not poll an unfocused/background session.
// The optional API pair is for the deterministic device-free probe only.
class NativeGamepad {
public:
  using Json = nlohmann::json;
  using GetState = DWORD(WINAPI *)(DWORD, XINPUT_STATE *);
  using SetState = DWORD(WINAPI *)(DWORD, XINPUT_VIBRATION *);

private:
  struct Slot {
    bool connected = false, sent = false, motorOwned = false;
    uint64_t incarnation = 0, sequence = 0;
    ULONGLONG nextProbe = 0, motorDeadline = 0;
    XINPUT_GAMEPAD last{};
  };
  HMODULE library = nullptr;
  GetState get = nullptr;
  SetState set = nullptr;
  std::function<void(const Json &)> emit;
  std::array<Slot, 4> slots{};
  bool negotiated = false, requested = false, rumble = true;
  bool serverSupported = false, leaseEnabled = false, haveState = false;
  int deadzone = 15;
  uint32_t focusId = 0, leaseId = 0;
  uint64_t generation = 0;
  ULONGLONG nextPoll = 0;
  std::string backend, error;
  Json serverDevices = Json::array();
  void send(const Json &message) {
    if (emit)
      emit(message);
  }
  bool active() const {
    return negotiated && requested && get && set && serverSupported && leaseEnabled && focusId &&
           leaseId == focusId && generation;
  }
  void stopMotor(DWORD slot) {
    auto &device = slots[slot];
    if (device.motorOwned && set) {
      XINPUT_VIBRATION zero{};
      set(slot, &zero);
    }
    device.motorOwned = false;
    device.motorDeadline = 0;
  }
  void packet(DWORD slot, bool connected, const XINPUT_GAMEPAD &sample) {
    auto &device = slots[slot];
    send({{"type", "gamepad"},
          {"generation", generation},
          {"id", leaseId},
          {"slot", slot},
          {"device_generation", device.incarnation},
          {"sequence", ++device.sequence},
          {"connected", connected},
          {"buttons", sample.wButtons},
          {"lx", sample.sThumbLX},
          {"ly", sample.sThumbLY},
          {"rx", sample.sThumbRX},
          {"ry", sample.sThumbRY},
          {"lt", sample.bLeftTrigger},
          {"rt", sample.bRightTrigger}});
  }
  void reset(bool notify) {
    for (DWORD i = 0; i < slots.size(); ++i) {
      auto &device = slots[i];
      if (notify && device.connected && device.sent && generation && leaseId)
        packet(i, false, {});
      stopMotor(i);
      device.connected = device.sent = false;
      device.last = {};
      device.nextProbe = 0;
      device.sequence = 0;
    }
    nextPoll = 0;
  }
  static bool same(const XINPUT_GAMEPAD &a, const XINPUT_GAMEPAD &b) {
    return a.wButtons == b.wButtons && a.bLeftTrigger == b.bLeftTrigger &&
           a.bRightTrigger == b.bRightTrigger && a.sThumbLX == b.sThumbLX &&
           a.sThumbLY == b.sThumbLY && a.sThumbRX == b.sThumbRX && a.sThumbRY == b.sThumbRY;
  }
  static void stick(SHORT &x, SHORT &y, int percent) {
    double nx = double(x) / (x < 0 ? 32768.0 : 32767.0);
    double ny = double(y) / (y < 0 ? 32768.0 : 32767.0);
    double magnitude = std::hypot(nx, ny), threshold = percent / 100.0;
    if (magnitude <= threshold) {
      x = y = 0;
      return;
    }
    double scaled = ((std::min)(1.0, magnitude) - threshold) / (1.0 - threshold);
    nx = nx / magnitude * scaled;
    ny = ny / magnitude * scaled;
    x = SHORT(std::clamp(std::lround(nx * (nx < 0 ? 32768.0 : 32767.0)), -32768l, 32767l));
    y = SHORT(std::clamp(std::lround(ny * (ny < 0 ? 32768.0 : 32767.0)), -32768l, 32767l));
  }
  static BYTE trigger(BYTE value, int percent) {
    int threshold = 255 * percent / 100;
    return value <= threshold ? 0 : BYTE((int(value) - threshold) * 255 / (255 - threshold));
  }
  template <class T> static T address(HMODULE module, const char *name) {
    auto raw = GetProcAddress(module, name);
    T result = nullptr;
    static_assert(sizeof(raw) == sizeof(result));
    std::memcpy(&result, &raw, sizeof(result));
    return result;
  }

public:
  explicit NativeGamepad(std::function<void(const Json &)> sender = {}, GetState mockGet = nullptr,
                         SetState mockSet = nullptr)
      : emit(std::move(sender)) {
    if (mockGet || mockSet) {
      get = mockGet;
      set = mockSet;
      backend = "test-api";
      return;
    }
    for (const wchar_t *name : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
      HMODULE module = LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
      if (!module)
        continue;
      auto read = address<GetState>(module, "XInputGetState");
      auto write = address<SetState>(module, "XInputSetState");
      if (read && write) {
        library = module;
        get = read;
        set = write;
        while (*name)
          backend.push_back(char(*name++));
        break;
      }
      FreeLibrary(module);
    }
    if (!get || !set)
      error = "系统没有可用的 XInput；手柄转发不可用";
  }
  ~NativeGamepad() {
    reset(false);
    if (library)
      FreeLibrary(library);
  }
  NativeGamepad(const NativeGamepad &) = delete;
  NativeGamepad &operator=(const NativeGamepad &) = delete;
  bool supported() const { return get && set; }
  void configure(bool capability, bool enabled, bool motors, int percent) {
    bool changed = negotiated != capability || requested != enabled;
    if (changed)
      reset(true);
    if (rumble && !motors)
      for (DWORD i = 0; i < slots.size(); ++i)
        stopMotor(i);
    int nextDeadzone = std::clamp(percent, 0, 50);
    if (deadzone != nextDeadzone)
      for (auto &device : slots)
        device.sent = false;
    bool newlyNegotiated = !negotiated && capability;
    negotiated = capability;
    requested = enabled;
    rumble = motors;
    deadzone = nextDeadzone;
    if (newlyNegotiated && focusId)
      send({{"type", "gamepad_focus"}, {"id", focusId}});
  }
  void focus(uint32_t id) {
    if (focusId == id)
      return;
    reset(true);
    focusId = id;
    if (negotiated)
      send({{"type", "gamepad_focus"}, {"id", id}});
  }
  void state(const Json &message) {
    if (!message.is_object() || !message.contains("generation") || !message.contains("id") ||
        !message.contains("enabled") || !message.contains("supported") ||
        !message["generation"].is_number_unsigned() || !message["id"].is_number_unsigned() ||
        !message["enabled"].is_boolean() || !message["supported"].is_boolean())
      return;
    uint64_t next = message["generation"].get<uint64_t>();
    uint64_t id = message["id"].get<uint64_t>();
    if (!next || next < generation || id > UINT32_MAX)
      return;
    bool enabled = message["enabled"].get<bool>();
    if (next != generation || leaseId != id || !enabled)
      reset(false);
    generation = next;
    leaseId = uint32_t(id);
    leaseEnabled = enabled;
    serverSupported = message["supported"].get<bool>();
    haveState = true;
    if (message.contains("error") && message["error"].is_string())
      error = message["error"].get<std::string>();
    if (message.contains("devices") && message["devices"].is_array())
      serverDevices = message["devices"];
  }
  void feedback(const Json &message) {
    if (!active() || !rumble)
      return;
    auto integer = [&](const char *field, uint64_t maximum, uint64_t &out) {
      if (!message.contains(field) || !message[field].is_number_unsigned())
        return false;
      out = message[field].get<uint64_t>();
      return out <= maximum;
    };
    uint64_t lease, slot, incarnation, strong, weak, duration;
    if (!integer("generation", UINT64_MAX, lease) || lease != generation ||
        !integer("slot", 3, slot) || !integer("device_generation", UINT64_MAX, incarnation) ||
        !integer("strong", 65535, strong) || !integer("weak", 65535, weak) ||
        !integer("duration_ms", 1000, duration) || !duration)
      return;
    auto &device = slots[slot];
    if (!device.connected || device.incarnation != incarnation)
      return;
    XINPUT_VIBRATION motors{WORD(strong), WORD(weak)};
    if (set(DWORD(slot), &motors) == ERROR_SUCCESS) {
      device.motorOwned = strong || weak;
      device.motorDeadline = device.motorOwned ? GetTickCount64() + duration : 0;
    }
  }
  void poll(ULONGLONG now = GetTickCount64()) {
    for (DWORD i = 0; i < slots.size(); ++i)
      if (slots[i].motorOwned && now >= slots[i].motorDeadline)
        stopMotor(i);
    if (!active() || now < nextPoll)
      return;
    nextPoll = now + 8; // At most 125 Hz; empty XInput slots are checked at 2 Hz.
    for (DWORD i = 0; i < slots.size(); ++i) {
      auto &device = slots[i];
      if (!device.connected && now < device.nextProbe)
        continue;
      XINPUT_STATE state{};
      DWORD result = get(i, &state);
      if (result != ERROR_SUCCESS) {
        if (device.connected) {
          packet(i, false, {});
          stopMotor(i);
        }
        device.connected = device.sent = false;
        device.nextProbe = now + 500;
        continue;
      }
      if (!device.connected) {
        device.connected = true;
        device.sent = false;
        device.sequence = 0;
        ++device.incarnation;
        if (!device.incarnation)
          ++device.incarnation;
      }
      XINPUT_GAMEPAD sample = filtered(state.Gamepad, deadzone);
      if (!device.sent || !same(sample, device.last)) {
        packet(i, true, sample);
        device.last = sample;
        device.sent = true;
      }
    }
  }
  static XINPUT_GAMEPAD filtered(XINPUT_GAMEPAD sample, int percent) {
    percent = std::clamp(percent, 0, 50);
    sample.wButtons &= 0xf3ff;
    stick(sample.sThumbLX, sample.sThumbLY, percent);
    stick(sample.sThumbRX, sample.sThumbRY, percent);
    sample.bLeftTrigger = trigger(sample.bLeftTrigger, percent);
    sample.bRightTrigger = trigger(sample.bRightTrigger, percent);
    return sample;
  }
  Json status() const {
    return {{"gamepad_supported", negotiated && supported() && (!haveState || serverSupported)},
            {"gamepad_enabled", negotiated && requested},
            {"gamepad_active", active()},
            {"gamepad_devices", serverDevices},
            {"rumble_supported", negotiated && supported() && serverSupported},
            {"gamepad_error", supported() ? error : "系统没有可用的 XInput；手柄转发不可用"},
            {"gamepad_backend", backend}};
  }
  void disconnect() {
    reset(true);
    negotiated = requested = serverSupported = leaseEnabled = haveState = false;
    generation = 0;
    focusId = leaseId = 0;
    serverDevices = Json::array();
  }
};
