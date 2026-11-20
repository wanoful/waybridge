// Exercise the production adapter with SDL virtual controllers and motor callbacks.
#include "gamepad.h"
#include <cstdlib>
#include <iostream>
#include <vector>

using Json = nlohmann::json;

static void check(bool condition, const char *failure) {
  if (!condition) {
    std::cerr << failure << " (SDL: " << SDL_GetError() << ")\n";
    std::exit(1);
  }
}
struct Motors {
  std::vector<std::pair<Uint16, Uint16>> writes;
};
struct FixturePad {
  SDL_JoystickID id = 0;
  SDL_Joystick *joystick = nullptr;
  Motors *motors = nullptr;
  // An SDL virtual joystick emits only dirty axes by default. Repeat the
  // trigger values so they model a device delivering a fresh state report.
  Sint16 leftTrigger = -32768, rightTrigger = -32768;

  static void SDLCALL update(void *userdata) {
    auto &pad = *static_cast<FixturePad *>(userdata);
    if (!pad.joystick) return;
    SDL_SetJoystickVirtualAxis(pad.joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, pad.leftTrigger);
    SDL_SetJoystickVirtualAxis(pad.joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, pad.rightTrigger);
  }
  static bool SDLCALL rumble(void *userdata, Uint16 low, Uint16 high) {
    static_cast<FixturePad *>(userdata)->motors->writes.emplace_back(low, high);
    return true;
  }

  void attach(const char *name, Motors *feedback = nullptr) {
    motors = feedback;
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = name;
    desc.userdata = this;
    desc.Update = update;
    if (motors) desc.Rumble = rumble;
    id = SDL_AttachVirtualJoystick(&desc);
    check(id && SDL_IsGamepad(id), "fixture did not enumerate as a mapped gamepad");
    joystick = SDL_OpenJoystick(id);
    check(joystick != nullptr, "fixture joystick open failed");
  }
  void detach() {
    if (!id) return;
    check(SDL_DetachVirtualJoystick(id), "fixture detach failed");
    SDL_CloseJoystick(joystick);
    joystick = nullptr;
    id = 0;
  }
  void button(SDL_GamepadButton button, bool down) {
    check(SDL_SetJoystickVirtualButton(joystick, button, down), "fixture button update failed");
  }
  void axis(SDL_GamepadAxis axis, Sint16 value) {
    check(SDL_SetJoystickVirtualAxis(joystick, axis, value), "fixture axis update failed");
  }
};
static Json lease(uint64_t generation, uint32_t id, bool enabled = true) {
  return {{"type", "gamepad_state"}, {"generation", generation}, {"id", id},
          {"enabled", enabled}, {"supported", true}, {"error", ""}, {"devices", Json::array()}};
}
static Json haptic(uint64_t generation, uint32_t slot, uint64_t incarnation) {
  return {{"generation", generation}, {"slot", slot}, {"device_generation", incarnation},
          {"strong", uint32_t(1234)}, {"weak", uint32_t(5678)}, {"duration_ms", uint32_t(750)}};
}

int main() {
  std::vector<Json> messages;
  auto latest = [&](size_t slot) -> Json {
    for (auto it = messages.rbegin(); it != messages.rend(); ++it)
      if ((*it)["type"] == "gamepad" && (*it)["slot"] == slot) return *it;
    return Json::object();
  };
  SDL_JoystickID reconnectId=0;
  NativeGamepad pad([&](const Json &message) { messages.push_back(message); },
                    [&](SDL_JoystickID id) { return id == reconnectId ? "fixture-a" : ""; });
  ULONGLONG now = GetTickCount64();
  pad.configure(true, false, true, 0);
  pad.focus(7);
  pad.state(lease(1, 7));
  pad.poll(now);
  check(SDL_WasInit(SDL_INIT_GAMEPAD) == 0 && latest(0).empty(), "opt-out started SDL or forwarded input");

  // Attach only private devices in this process. SDL handles discovery,
  // normalization and feedback just as in production.
  check(SDL_InitSubSystem(SDL_INIT_GAMEPAD), "could not initialize SDL in the fixture");
  Motors firstMotors, secondMotors, replacementMotors;
  FixturePad a, b, c, d, overflow, replacement;
  a.attach("Waybridge SDL test A", &firstMotors);
  reconnectId=a.id;
  b.attach("Waybridge SDL test B", &secondMotors);
  c.attach("Waybridge SDL test C");
  d.attach("Waybridge SDL test D");
  overflow.attach("Waybridge SDL test overflow");
  a.button(SDL_GAMEPAD_BUTTON_SOUTH, true);
  a.button(SDL_GAMEPAD_BUTTON_DPAD_UP, true);
  a.axis(SDL_GAMEPAD_AXIS_LEFTY, -32768);
  a.leftTrigger = 32767;
  b.button(SDL_GAMEPAD_BUTTON_EAST, true);
  pad.configure(true, true, true, 0);
  pad.poll(now += 100);
  check(pad.status()["gamepad_local_count"] == 4 && pad.status()["gamepad_backend"] == "SDL3" &&
        latest(4).empty() && !latest(3).empty(), "SDL slots or four-device limit wrong");
  check(latest(0)["buttons"] == (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_DPAD_UP) &&
        latest(0)["ly"] == 32767 && latest(0)["lt"] == 255 && latest(0)["rt"] == 0 &&
        latest(1)["buttons"] == XINPUT_GAMEPAD_B, "SDL button, stick or trigger mapping wrong");
  auto aInc = latest(0)["device_generation"].get<uint64_t>();
  auto bInc = latest(1)["device_generation"].get<uint64_t>();
  auto cInc = latest(2)["device_generation"].get<uint64_t>();
  size_t before = messages.size();
  b.button(SDL_GAMEPAD_BUTTON_NORTH, true);
  pad.poll(now + 1);
  check(messages.size() == before, "poll exceeded 125 Hz");
  pad.poll(now += 8);
  check(latest(1)["sequence"] == 2 && latest(0)["sequence"] == 1, "SDL changes lost or rebroadcast in other slots");
  check(pad.status()["rumble_supported"] == true, "SDL rumble capability missing");
  pad.feedback(haptic(1, 2, cInc));
  pad.feedback(haptic(9, 0, aInc));
  pad.feedback(haptic(1, 0, aInc + 1));
  check(firstMotors.writes.empty() && secondMotors.writes.empty(), "stale or unsupported feedback escaped");
  pad.feedback(haptic(1, 1, bInc));
  check(firstMotors.writes.empty() && secondMotors.writes == decltype(secondMotors.writes){{1234, 5678}},
        "SDL rumble went to the wrong physical instance");
  pad.poll(GetTickCount64() + 1000);
  check(secondMotors.writes.back() == std::make_pair(Uint16(0), Uint16(0)), "rumble watchdog did not stop SDL motor");

  overflow.detach();
  b.detach();
  pad.poll(now += 1500);
  check(latest(1)["connected"] == false && latest(2)["device_generation"] == cInc &&
        latest(0)["device_generation"] == aInc, "unplug renumbered remaining devices or left held input");
  replacement.attach("Waybridge SDL test replacement", &replacementMotors);
  replacement.button(SDL_GAMEPAD_BUTTON_WEST, true);
  pad.poll(now += 500);
  auto newInc = latest(1)["device_generation"].get<uint64_t>();
  check(newInc > bInc && latest(1)["buttons"] == XINPUT_GAMEPAD_X, "replacement did not reuse vacant slot with fresh identity");
  pad.feedback(haptic(1, 1, bInc));
  check(replacementMotors.writes.empty(), "feedback crossed physical replacement");
  pad.feedback(haptic(1, 1, newInc));
  check(replacementMotors.writes.size() == 1, "replacement feedback discarded");
  before = messages.size();
  pad.focus(0);
  check(replacementMotors.writes.back() == std::make_pair(Uint16(0), Uint16(0)) &&
        firstMotors.writes.empty() && messages.size() == before + 1 &&
        messages.back()["type"] == "gamepad_focus" && !pad.status()["gamepad_active"].get<bool>(),
        "focus loss failed to stop the motor without disconnecting remote devices");
  pad.state(lease(2, 0, false));
  before = messages.size();
  pad.poll(now += 500);
  check(messages.size() == before && pad.status()["gamepad_local_count"] == 4,
        "blur confused local inventory with forwarded devices");
  d.detach();
  pad.poll(now += 500);
  check(latest(3)["connected"] == false && latest(3)["generation"] == 2 && latest(3)["id"] == 0,
        "physical unplug during blur did not remove the remote slot");
  pad.focus(7);
  before = messages.size();
  pad.state(lease(2, 7));
  pad.poll(now += 8);
  check(messages.size() == before && !pad.status()["gamepad_active"].get<bool>(),
        "focus recovery permitted input on a stale lease");
  pad.state(lease(3, 7));
  pad.poll(now += 8);
  check(latest(0)["generation"] == 3 && latest(0)["device_generation"] == aInc &&
        latest(0)["sequence"].get<uint64_t>() > 1 && latest(1)["device_generation"] == newInc &&
        latest(1)["buttons"] == XINPUT_GAMEPAD_X && latest(3)["connected"] == false,
        "focus recovery failed to preserve slot identity and send fresh samples");
  before = replacementMotors.writes.size();
  pad.feedback(haptic(1, 1, newInc));
  check(replacementMotors.writes.size() == before, "old feedback was replayed after focus recovery");
  pad.configure(true, true, false, 50);
  a.axis(SDL_GAMEPAD_AXIS_LEFTY, -1000);
  pad.poll(now += 8);
  check(latest(0)["ly"] == 0, "live deadzone not applied");
  pad.allowReconnect(true);
  a.detach();
  pad.poll(now += 500);
  check(latest(0)["connected"] == false && latest(0)["suspended"] == true && latest(0)["device_generation"] == aInc &&
        latest(1)["device_generation"] == newInc && latest(2)["device_generation"] == cInc,
        "SDL reconnect did not reserve only the missing slot");
  a.attach("Waybridge SDL test A reconnected", &firstMotors);
  reconnectId=a.id;
  a.button(SDL_GAMEPAD_BUTTON_SOUTH,true);
  pad.configure(true,true,true,50);
  pad.poll(now += 500);
  auto resumed=latest(0)["device_generation"].get<uint64_t>();
  check(latest(0)["connected"] == true && latest(0)["reconnect_from"] == aInc &&
        resumed > aInc && latest(0)["buttons"] == XINPUT_GAMEPAD_A && latest(1)["device_generation"] == newInc,
        "SDL reconnect lost the original slot or did not refresh its physical identity");
  pad.feedback(haptic(3,0,aInc));
  check(firstMotors.writes.empty(),"stale motor command reached the reconnected device");
  pad.feedback(haptic(3,0,resumed));
  check(firstMotors.writes.size()==1,"new motor command did not reach the reconnected device");
  a.detach();
  pad.poll(now += 500);
  check(latest(0)["suspended"] == true,"second detach did not suspend the reconnected slot");
  pad.poll(now += 5001);
  check(latest(0)["connected"] == false && !latest(0).contains("suspended"),
        "empty reserved slot did not expire");
  pad.configure(true, false, false, 15);
  check(latest(0)["connected"] == false && pad.status()["gamepad_local_count"].is_null(),
        "disable did not clear active input and local inventory");
  pad.disconnect();
  c.detach(); replacement.detach();
  SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
  check(SDL_WasInit(SDL_INIT_GAMEPAD) == 0, "SDL gamepad subsystem leaked a reference");

  XboxBluetoothGamepad::Report legacyReport = {1, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff,
      0xff, 0x03, 0x00, 0x02, 2, 0x49, 0x2c, 0x00};
  XINPUT_GAMEPAD legacyState{};
  check(XboxBluetoothGamepad::decode(legacyReport, legacyState) && legacyState.sThumbLX == 32767 &&
        legacyState.sThumbLY == 32767 && legacyState.bLeftTrigger == 255 && legacyState.bRightTrigger == 128 &&
        legacyState.wButtons == (XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_A |
            XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_BACK |
            XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_LEFT_THUMB), "temporary Xbox fallback report regressed");
  legacyReport[0] = 4;
  check(!XboxBluetoothGamepad::decode(legacyReport, legacyState), "temporary fallback accepted a battery report");
  std::cout << "PASS: SDL virtual pads, four slots and hotplug, buttons/axes/triggers, lease and deadzone, exact rumble/stop and stale feedback, bounded same-slot reconnect\n";
}
