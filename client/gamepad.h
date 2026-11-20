#pragma once
#include <windows.h>
#include <xinput.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_init.h>
#include "third_party/json.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

// SDL owns physical device discovery, mapping and feedback. Waybridge owns the
// foreground lease, protocol slots and the lifetime of remote input.
class NativeGamepad {
public:
  using Json = nlohmann::json;

private:
  static constexpr ULONGLONG ReconnectGraceMs = 5000;
  static constexpr WORD GuideButton = 0x0400;
  struct Slot {
    SDL_JoystickID instance = 0;
    SDL_Gamepad *controller = nullptr;
    bool sent = false, needSnapshot = false, rumbleCapable = false, motorOwned = false;
    uint64_t incarnation = 0, sequence = 0;
    ULONGLONG retryAfter = 0, motorDeadline = 0;
    ULONGLONG reconnectUntil = 0;
    std::string identity;
    XINPUT_GAMEPAD last{};
  };
  std::function<void(const Json &)> emit;
  std::function<std::string(SDL_JoystickID)> deviceIdentity;
  std::array<Slot, 4> slots{};
  bool initialized = false, negotiated = false, requested = false, rumble = true;
  bool reconnect = false;
  bool serverSupported = false, leaseEnabled = false, haveState = false;
  int deadzone = 15;
  uint32_t focusId = 0, leaseId = 0;
  uint64_t generation = 0, generationAtFocusChange = 0;
  ULONGLONG nextPoll = 0, nextInventory = 0;
  int localCount = -1;
  std::string serverError, localError;
  Json serverDevices = Json::array();

  void send(const Json &message) {
    if (emit) emit(message);
  }
  bool supported() const { return !requested || initialized; }
  bool active() const {
    return negotiated && requested && supported() && serverSupported && leaseEnabled && focusId &&
           leaseId == focusId && generation;
  }
  void packet(size_t slot, bool connected, const XINPUT_GAMEPAD &sample, bool suspended = false,
              uint64_t reconnectFrom = 0) {
    auto &device = slots[slot];
    Json value = {{"type", "gamepad"}, {"generation", generation}, {"id", leaseId}, {"slot", slot},
          {"device_generation", device.incarnation}, {"sequence", ++device.sequence},
          {"connected", connected}, {"buttons", sample.wButtons & ~GuideButton},
          {"guide", (sample.wButtons & GuideButton) != 0}, {"lx", sample.sThumbLX},
          {"ly", sample.sThumbLY}, {"rx", sample.sThumbRX}, {"ry", sample.sThumbRY},
          {"lt", sample.bLeftTrigger}, {"rt", sample.bRightTrigger}};
    if (suspended) value["suspended"] = true;
    if (reconnectFrom) value["reconnect_from"] = reconnectFrom;
    send(value);
  }
  static void stopMotor(Slot &device) {
    if (device.motorOwned && device.controller)
      SDL_RumbleGamepad(device.controller, 0, 0, 0);
    device.motorOwned = false;
    device.motorDeadline = 0;
  }
  static void closeController(Slot &device) {
    stopMotor(device);
    if (device.controller) SDL_CloseGamepad(device.controller);
    device.controller = nullptr;
    device.retryAfter = 0;
  }
  void closeSlot(size_t slot, bool notify) {
    auto &device = slots[slot];
    if (notify && device.sent && generation) packet(slot, false, {});
    closeController(device);
    device.sent = device.needSnapshot = device.rumbleCapable = false;
    device.sequence = 0;
    device.reconnectUntil = 0;
  }
  void detachPhysical(size_t slot, ULONGLONG now) {
    auto &device = slots[slot];
    if (reconnect && device.sent && generation && !device.identity.empty()) {
      closeController(device);
      device.rumbleCapable = false;
      if (!device.reconnectUntil) {
        packet(slot, false, {}, true);
        device.reconnectUntil = now + ReconnectGraceMs;
      }
    } else {
      closeSlot(slot, true);
      device.identity.clear();
    }
    device.instance = 0;
  }
  static std::string sdlIdentity(SDL_JoystickID id) {
    const auto *path = SDL_GetGamepadPathForID(id);
    if (!path || !*path || SDL_IsJoystickVirtual(id)) return {};
    char guid[33]{};
    SDL_GUIDToString(SDL_GetGamepadGUIDForID(id), guid, sizeof(guid));
    return std::string(guid) + ":" + path;
  }
  void pauseInput() {
    for (auto &device : slots) {
      closeController(device);
      device.needSnapshot = true;
    }
    nextPoll = 0;
  }
  void stopDiscovery(bool notify) {
    for (size_t i = 0; i < slots.size(); ++i) {
      closeSlot(i, notify);
      slots[i].instance = 0;
      slots[i].identity.clear();
    }
    if (initialized) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    initialized = false;
    nextPoll = nextInventory = 0;
    localCount = -1;
    localError.clear();
  }
  void refreshDevices(ULONGLONG now) {
    int count = 0;
    std::unique_ptr<SDL_JoystickID, decltype(&SDL_free)> instances(
        SDL_GetGamepads(&count), SDL_free);
    if (!instances) {
      localError = "Could not enumerate SDL controllers: " + std::string(SDL_GetError());
      return;
    }
    localError.clear();
    const auto *first = instances.get();
    for (size_t i = 0; i < slots.size(); ++i) {
      auto &device = slots[i];
      if (device.instance && (!count || std::find(first, first + count, device.instance) == first + count)) {
        detachPhysical(i, now);
      }
    }
    localCount = std::min(count, int(slots.size()));
    for (int n = 0; n < count; ++n) {
      const auto id = first[n];
      if (std::any_of(slots.begin(), slots.end(), [&](const Slot &s) { return s.instance == id; })) continue;
      std::string key = deviceIdentity ? deviceIdentity(id) : sdlIdentity(id);
      auto vacant = slots.end();
      if (!key.empty())
        vacant = std::find_if(slots.begin(), slots.end(), [&](const Slot &s) {
          return !s.instance && s.reconnectUntil && s.identity == key;
        });
      if (vacant == slots.end())
        vacant = std::find_if(slots.begin(), slots.end(), [](const Slot &s) { return !s.instance && !s.reconnectUntil; });
      if (vacant == slots.end()) continue;
      vacant->instance = id;
      vacant->identity = std::move(key);
    }
  }
  static XINPUT_GAMEPAD sample(SDL_Gamepad *controller) {
    // The v1 wire uses Xbox button masks and positive-up stick coordinates.
    static constexpr std::pair<SDL_GamepadButton, WORD> buttons[] = {
      {SDL_GAMEPAD_BUTTON_SOUTH, XINPUT_GAMEPAD_A}, {SDL_GAMEPAD_BUTTON_EAST, XINPUT_GAMEPAD_B},
      {SDL_GAMEPAD_BUTTON_WEST, XINPUT_GAMEPAD_X}, {SDL_GAMEPAD_BUTTON_NORTH, XINPUT_GAMEPAD_Y},
      {SDL_GAMEPAD_BUTTON_BACK, XINPUT_GAMEPAD_BACK}, {SDL_GAMEPAD_BUTTON_START, XINPUT_GAMEPAD_START},
      {SDL_GAMEPAD_BUTTON_GUIDE, GuideButton},
      {SDL_GAMEPAD_BUTTON_LEFT_STICK, XINPUT_GAMEPAD_LEFT_THUMB},
      {SDL_GAMEPAD_BUTTON_RIGHT_STICK, XINPUT_GAMEPAD_RIGHT_THUMB},
      {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, XINPUT_GAMEPAD_LEFT_SHOULDER},
      {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER},
      {SDL_GAMEPAD_BUTTON_DPAD_UP, XINPUT_GAMEPAD_DPAD_UP},
      {SDL_GAMEPAD_BUTTON_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_DOWN},
      {SDL_GAMEPAD_BUTTON_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_LEFT},
      {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT},
    };
    WORD mask = 0;
    for (auto [button, flag] : buttons)
      if (SDL_GetGamepadButton(controller, button)) mask |= flag;
    auto axis = [controller](SDL_GamepadAxis a) { return SDL_GetGamepadAxis(controller, a); };
    auto up = [](Sint16 value) { return SHORT(std::min(32767, -int(value))); };
    auto trigger = [](Sint16 value) { return BYTE((std::max(0, int(value)) * 255 + 16383) / 32767); };
    return {mask, trigger(axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER)), trigger(axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)),
            axis(SDL_GAMEPAD_AXIS_LEFTX), up(axis(SDL_GAMEPAD_AXIS_LEFTY)),
            axis(SDL_GAMEPAD_AXIS_RIGHTX), up(axis(SDL_GAMEPAD_AXIS_RIGHTY))};
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
    double scaled = (std::min(1.0, magnitude) - threshold) / (1.0 - threshold);
    nx = nx / magnitude * scaled;
    ny = ny / magnitude * scaled;
    x = SHORT(std::clamp(std::lround(nx * (nx < 0 ? 32768.0 : 32767.0)), -32768l, 32767l));
    y = SHORT(std::clamp(std::lround(ny * (ny < 0 ? 32768.0 : 32767.0)), -32768l, 32767l));
  }
  static BYTE trigger(BYTE value, int percent) {
    int threshold = 255 * percent / 100;
    return value <= threshold ? 0 : BYTE((int(value) - threshold) * 255 / (255 - threshold));
  }
  void forward(size_t slot, XINPUT_GAMEPAD raw) {
    auto &device = slots[slot];
    const auto state = filtered(raw, deadzone);
    uint64_t reconnectFrom = 0;
    if (device.reconnectUntil) {
      reconnectFrom = device.incarnation;
      device.reconnectUntil = 0;
      device.sequence = 0;
      if (!++device.incarnation) ++device.incarnation;
    } else if (!device.sent) {
      if (!++device.incarnation) ++device.incarnation;
    } else if (!device.needSnapshot && same(state, device.last)) {
      return;
    }
    packet(slot, true, state, false, reconnectFrom);
    device.last = state;
    device.sent = true;
    device.needSnapshot = false;
  }

public:
  explicit NativeGamepad(std::function<void(const Json &)> sender = {},
                         std::function<std::string(SDL_JoystickID)> identity = {})
      : emit(std::move(sender)), deviceIdentity(std::move(identity)) {}
  ~NativeGamepad() { stopDiscovery(false); }
  NativeGamepad(const NativeGamepad &) = delete;
  NativeGamepad &operator=(const NativeGamepad &) = delete;
  void allowReconnect(bool allowed) { reconnect = allowed; }

  void configure(bool capability, bool enabled, bool motors, int percent) {
    const bool newlyNegotiated = !negotiated && capability;
    const bool changed = negotiated != capability || requested != enabled;
    if (changed) stopDiscovery(true);
    negotiated = capability;
    requested = enabled;
    if (changed && negotiated && requested) {
      initialized = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
      if (initialized) {
        SDL_SetGamepadEventsEnabled(false);
        SDL_SetJoystickEventsEnabled(false);
      } else {
        localError = "Could not initialize SDL controllers: " + std::string(SDL_GetError());
      }
    }
    if (rumble && !motors)
      for (auto &device : slots) stopMotor(device);
    rumble = motors;
    deadzone = std::clamp(percent, 0, 50);
    if (newlyNegotiated && focusId) send({{"type", "gamepad_focus"}, {"id", focusId}});
  }
  void focus(uint32_t id) {
    if (focusId == id) return;
    pauseInput();
    leaseEnabled = false;
    generationAtFocusChange = generation;
    focusId = id;
    if (negotiated) send({{"type", "gamepad_focus"}, {"id", id}});
  }
  void state(const Json &message) {
    if (!message.is_object() || !message.contains("generation") || !message.contains("id") ||
        !message.contains("enabled") || !message.contains("supported") ||
        !message["generation"].is_number_unsigned() || !message["id"].is_number_unsigned() ||
        !message["enabled"].is_boolean() || !message["supported"].is_boolean()) return;
    uint64_t next = message["generation"].get<uint64_t>();
    uint64_t id = message["id"].get<uint64_t>();
    if (!next || next < generation || id > UINT32_MAX) return;
    bool enabled = message["enabled"].get<bool>();
    bool supportedByServer = message["supported"].get<bool>();
    if (next != generation || leaseId != id || (leaseEnabled && (!enabled || !supportedByServer)))
      pauseInput();
    generation = next;
    leaseId = uint32_t(id);
    leaseEnabled = enabled && next > generationAtFocusChange;
    serverSupported = supportedByServer;
    haveState = true;
    if (message.contains("error") && message["error"].is_string())
      serverError = message["error"].get<std::string>();
    if (message.contains("devices") && message["devices"].is_array())
      serverDevices = message["devices"];
  }
  void feedback(const Json &message) {
    if (!active() || !rumble) return;
    auto integer = [&](const char *field, uint64_t maximum, uint64_t &out) {
      if (!message.contains(field) || !message[field].is_number_unsigned()) return false;
      out = message[field].get<uint64_t>();
      return out <= maximum;
    };
    uint64_t lease, slot, incarnation, strong, weak, duration;
    if (!integer("generation", UINT64_MAX, lease) || lease != generation ||
        !integer("slot", slots.size() - 1, slot) ||
        !integer("device_generation", UINT64_MAX, incarnation) ||
        !integer("strong", 65535, strong) || !integer("weak", 65535, weak) ||
        !integer("duration_ms", 1000, duration) || !duration) return;
    auto &device = slots[slot];
    if (!device.sent || device.reconnectUntil || !device.controller || !device.rumbleCapable ||
        device.incarnation != incarnation) return;
    if (SDL_RumbleGamepad(device.controller, Uint16(strong), Uint16(weak), Uint32(duration))) {
      device.motorOwned = strong || weak;
      device.motorDeadline = device.motorOwned ? GetTickCount64() + duration : 0;
    }
  }
  void poll(ULONGLONG now = GetTickCount64()) {
    if (!initialized) return;
    for (size_t slot=0;slot<slots.size();++slot) {
      auto &device=slots[slot];
      if (device.motorOwned && now >= device.motorDeadline) stopMotor(device);
      if (device.reconnectUntil && now >= device.reconnectUntil) {
        closeSlot(slot, true);
        if (!device.instance) device.identity.clear();
      }
    }
    const bool inventoryDue = now >= nextInventory;
    const bool sampleDue = active() && now >= nextPoll;
    if (!sampleDue && !inventoryDue) return;
    SDL_UpdateGamepads();
    if (inventoryDue) {
      nextInventory = now + 500;
      refreshDevices(now);
    }
    if (!active() || now < nextPoll) return;
    nextPoll = now + 8;
    bool opened = false;
    for (size_t i = 0; i < slots.size(); ++i) {
      auto &device = slots[i];
      if (!device.instance || now < device.retryAfter) continue;
      if (device.controller && !SDL_GamepadConnected(device.controller)) {
        detachPhysical(i, now);
        continue;
      }
      if (!device.controller) {
        device.controller = SDL_OpenGamepad(device.instance);
        if (!device.controller) {
          device.retryAfter = now + 500;
          localError = "Could not open the SDL controller: " + std::string(SDL_GetError());
          continue;
        }
        device.rumbleCapable = SDL_GetBooleanProperty(SDL_GetGamepadProperties(device.controller),
                                                    SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
        opened = true;
      }
    }
    // Opening a mapped gamepad initializes the trigger zero point. Refresh it
    // before forwarding the first snapshot, including triggers already held.
    if (opened) SDL_UpdateGamepads();
    for (size_t i = 0; i < slots.size(); ++i)
      if (auto *controller = slots[i].controller; controller && SDL_GamepadConnected(controller))
        forward(i, sample(controller));
  }
  static XINPUT_GAMEPAD filtered(XINPUT_GAMEPAD sample, int percent) {
    percent = std::clamp(percent, 0, 50);
    sample.wButtons &= 0xf3ff | GuideButton;
    stick(sample.sThumbLX, sample.sThumbLY, percent);
    stick(sample.sThumbRX, sample.sThumbRY, percent);
    sample.bLeftTrigger = trigger(sample.bLeftTrigger, percent);
    sample.bRightTrigger = trigger(sample.bRightTrigger, percent);
    return sample;
  }
  Json status() const {
    bool motors = std::any_of(slots.begin(), slots.end(), [](const Slot &s) { return s.rumbleCapable; });
    return {{"gamepad_supported", negotiated && supported() && (!haveState || serverSupported)},
            {"gamepad_enabled", negotiated && requested}, {"gamepad_active", active()},
            {"gamepad_local_count", localCount < 0 ? Json(nullptr) : Json(localCount)},
            {"gamepad_devices", serverDevices}, {"rumble_supported", negotiated && serverSupported && motors},
            {"gamepad_error", serverError.empty() ? localError : serverError},
            {"gamepad_backend", "SDL3"}};
  }
  void disconnect() {
    stopDiscovery(true);
    negotiated = requested = serverSupported = leaseEnabled = haveState = false;
    reconnect = false;
    generation = generationAtFocusChange = 0;
    focusId = leaseId = 0;
    serverDevices = Json::array();
    serverError.clear();
  }
};
