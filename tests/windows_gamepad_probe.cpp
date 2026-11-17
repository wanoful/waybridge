// Deterministic XInput adapter checks. All reads/motor writes use the fake API;
// this executable never opens a real controller or manipulates the desktop.
#include "gamepad.h"
#include <cstdlib>
#include <iostream>
#include <vector>
using Json = nlohmann::json;
static std::array<bool, 4> connected{};
static std::array<XINPUT_GAMEPAD, 4> physical{};
static std::array<unsigned, 4> reads{};
struct Motor {
  DWORD slot;
  WORD strong, weak;
};
static std::vector<Motor> motors;
static DWORD WINAPI readState(DWORD slot, XINPUT_STATE *out) {
  ++reads.at(slot);
  if (!connected.at(slot))
    return ERROR_DEVICE_NOT_CONNECTED;
  out->Gamepad = physical.at(slot);
  return ERROR_SUCCESS;
}
static DWORD WINAPI setState(DWORD slot, XINPUT_VIBRATION *value) {
  motors.push_back({slot, value->wLeftMotorSpeed, value->wRightMotorSpeed});
  return connected.at(slot) ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}
static void check(bool condition, const char *reason) {
  if (!condition) {
    std::cerr << reason << '\n';
    std::exit(1);
  }
}
static Json lease(uint64_t generation, uint32_t id, bool enabled = true) {
  return {{"type", "gamepad_state"}, {"generation", generation}, {"id", id},
          {"enabled", enabled},      {"supported", true},        {"error", ""},
          {"devices", Json::array()}};
}
static Json feedback(uint64_t generation, uint32_t slot, uint64_t incarnation) {
  return {{"generation", generation}, {"slot", slot},           {"device_generation", incarnation},
          {"strong", uint32_t(1234)}, {"weak", uint32_t(5678)}, {"duration_ms", uint32_t(750)}};
}
int main() {
  std::vector<Json> packets;
  NativeGamepad pad([&](const Json &v) { packets.push_back(v); }, readState, setState);
  ULONGLONG now = GetTickCount64() + 10;
  check(motors.empty(), "constructor touched unrelated motors");
  pad.configure(true, false, true, 15);
  pad.focus(7);
  pad.state(lease(1, 7));
  pad.poll(now);
  check(reads[0] == 0 && motors.empty(), "default OFF captured input");
  check(pad.status()["gamepad_enabled"] == false, "OFF preference status");
  connected[0] = connected[3] = true;
  physical[0].wButtons = XINPUT_GAMEPAD_A;
  physical[0].sThumbLY = 32767;
  pad.configure(true, true, true, 15);
  pad.poll(now);
  check(reads[0] == 1 && reads[3] == 1, "four-slot discovery");
  check(packets.back()["slot"] == 3 && packets.back()["device_generation"] == 1,
        "slot/incarnation identity");
  auto packetCount = packets.size();
  pad.poll(now + 1);
  check(packets.size() == packetCount && reads[0] == 1, "125Hz upper bound");
  physical[0].wButtons |= XINPUT_GAMEPAD_B;
  pad.poll(now + 8);
  check(reads[0] == 2 && reads[1] == 1 && packets.back()["sequence"] == 2,
        "changed sample / bounded empty-slot probes");
  pad.state(Json::object());
  pad.feedback(Json::object());
  pad.feedback(feedback(0, 3, 1));
  pad.feedback(feedback(1, 4, 1));
  pad.feedback(feedback(1, 3, 99));
  check(motors.empty(), "stale/malformed feedback drove motors");
  pad.feedback(feedback(1, 3, 1));
  check(motors.size() == 1 && motors.back().slot == 3 && motors.back().strong == 1234,
        "rumble went to wrong physical slot");
  pad.focus(0);
  pad.poll(now + 16);
  check(motors.size() == 2 && motors.back().slot == 3 && motors.back().strong == 0,
        "focus loss did not stop only our owned motors");
  check(reads[0] == 2 && !pad.status()["gamepad_active"].get<bool>() &&
            pad.status()["gamepad_enabled"] == true,
        "blur changed preference or polled background");
  pad.feedback(feedback(1, 3, 1));
  check(motors.size() == 2, "background rumble accepted");
  pad.focus(7);
  pad.state(lease(3, 7));
  pad.poll(now + 24);
  check(packets.back()["device_generation"] == 2, "new lease reused device incarnation");
  connected[3] = false;
  pad.poll(now + 32);
  check(packets.back()["slot"] == 3 && packets.back()["connected"] == false, "hot-unplug not sent");
  connected[3] = true;
  pad.poll(now + 544);
  check(packets.back()["slot"] == 3 && packets.back()["device_generation"] == 3,
        "replug did not advance incarnation");
  pad.feedback(feedback(3, 3, 2));
  check(motors.size() == 2, "old device feedback crossed hotplug");
  pad.feedback(feedback(3, 3, 3));
  check(motors.size() == 3, "current device feedback rejected");
  pad.poll(GetTickCount64() + 1001);
  check(motors.size() == 4 && motors.back().strong == 0, "rumble watchdog failed");
  pad.feedback(feedback(3, 3, 3));
  pad.configure(true, true, false, 15);
  check(motors.size() == 6 && motors.back().strong == 0, "rumble OFF failed to neutralize");
  unsigned before = reads[0];
  pad.configure(true, false, false, 15);
  pad.poll(now + 2000);
  check(reads[0] == before, "disabled adapter read physical state");
  pad.disconnect();
  check(motors.size() == 6, "cleanup zeroed unrelated motors");
  XINPUT_GAMEPAD raw{};
  raw.sThumbLX = 1000;
  raw.sThumbLY = -1000;
  raw.bLeftTrigger = 10;
  auto neutral = NativeGamepad::filtered(raw, 15);
  check(neutral.sThumbLX == 0 && neutral.sThumbLY == 0 && neutral.bLeftTrigger == 0,
        "deadzone failed");
  raw = {};
  raw.sThumbLX = -32768;
  raw.bRightTrigger = 255;
  auto extreme = NativeGamepad::filtered(raw, 15);
  check(extreme.sThumbLX == -32768 && extreme.bRightTrigger == 255, "analog endpoints lost");
  std::cout
      << "gamepad: opt-in/focus/125Hz/deadzones/4-slots/hotplug/lease/exact-rumble/watchdog PASS\n";
}
