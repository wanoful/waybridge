#pragma once
#include <windows.h>
#include <directmanipulation.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include "third_party/json.hpp"

// Direct Manipulation supplies native Precision Touchpad pan and pinch on the
// window thread. The polling viewport has no content of its own: Wayland
// receives the gestures instead.
class NativeTouchpad {
  friend struct NativeTouchpadProbe;
  using Json = nlohmann::json;
  template <class Interface> using ComPtr = Microsoft::WRL::ComPtr<Interface>;
  static constexpr GUID ManagerClass = {
      0x54e211b6, 0x3650, 0x4f75, {0x83, 0x34, 0xfa, 0x35, 0x95, 0x98, 0xe1, 0xc5}};
  std::function<void(const Json &)> send;
  ComPtr<IDirectManipulationManager> manager;
  ComPtr<IDirectManipulationUpdateManager> updater;
  ComPtr<IDirectManipulationViewport> viewport;
  ComPtr<IDirectManipulationContent> content;
  HWND owner = nullptr;
  RECT bounds{};
  uint32_t windowId = 0;
  uint64_t gesture = 0, nextGesture = 0;
  bool polling = false, started = false;
  ULONGLONG pendingUntil = 0;
  double dpi = 96., lastX = 0., lastY = 0., baseScale = 1., lastScale = 1.;
  enum class Kind { None, Scroll, Pinch };
  Kind kind = Kind::None;

  void report(const char *phase, double dx = 0., double dy = 0., double scale = 1.) {
    send({{"type", "touchpad"},
          {"id", windowId},
          {"gesture", gesture},
          {"phase", phase},
          {"kind", kind == Kind::Pinch ? "pinch" : "scroll"},
          {"dx", dx},
          {"dy", dy},
          {"scale", scale}});
  }
  void endGesture(bool cancelled) {
    if (kind != Kind::None)
      report(cancelled ? "cancel" : "end");
    kind = Kind::None;
  }
  void beginGesture(Kind next) {
    kind = next;
    gesture = ++nextGesture;
    report("begin");
  }
  void transform(const float (&matrix)[6]) {
    double scale = matrix[0] / baseScale;
    double x = matrix[4], y = matrix[5];
    if (!std::isfinite(scale) || scale < .01 || scale > 100. || !std::isfinite(x) ||
        !std::isfinite(y))
      return;
    // A zoom transform also translates to preserve the zoom anchor. It must
    // not be forwarded as scroll. Compare to the gesture's initial scale.
    if (kind != Kind::Pinch && std::abs(scale - 1.) >= .002) {
      endGesture(false);
      beginGesture(Kind::Pinch);
    }
    if (kind == Kind::Pinch) {
      if (std::abs(scale - lastScale) >= .0001) {
        lastScale = scale;
        report("update", 0., 0., scale);
      }
      return;
    }
    double dx = (lastX - x) * 96. / dpi, dy = (lastY - y) * 96. / dpi;
    if (std::abs(dx) < .01 && std::abs(dy) < .01)
      return;
    lastX = x;
    lastY = y;
    if (kind == Kind::None)
      beginGesture(Kind::Scroll);
    report("update", std::clamp(dx, -1000., 1000.), std::clamp(dy, -1000., 1000.));
  }
  void stop(bool cancelled) {
    if (!polling)
      return;
    polling = started = false;
    KillTimer(owner, Timer);
    endGesture(cancelled);
    // Restore the transform between gestures so zoom limits cannot accumulate.
    viewport->ReleaseAllContacts();
    viewport->Stop();
    viewport->ZoomToRect(0.f, 0.f, float(bounds.right), float(bounds.bottom), FALSE);
  }

public:
  static constexpr UINT_PTR Timer = 0x57425450;
  explicit NativeTouchpad(std::function<void(const Json &)> output) : send(std::move(output)) {}
  bool initialize() {
    if (FAILED(
            CoCreateInstance(ManagerClass, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&manager))))
      return false;
    if (FAILED(manager->GetUpdateManager(IID_PPV_ARGS(&updater))))
      manager.Reset();
    return bool(updater);
  }
  bool available() const { return bool(updater); }
  bool owns(HWND hwnd) const { return hwnd && owner == hwnd && polling; }
  void focus(HWND hwnd, uint32_t id) {
    if (owner == hwnd || !available())
      return;
    detach();
    RECT rect{};
    if (!GetClientRect(hwnd, &rect) || rect.right <= 0 || rect.bottom <= 0 ||
        FAILED(manager->CreateViewport(nullptr, hwnd, IID_PPV_ARGS(&viewport))))
      return;
    const auto configuration = static_cast<DIRECTMANIPULATION_CONFIGURATION>(
        DIRECTMANIPULATION_CONFIGURATION_INTERACTION |
        DIRECTMANIPULATION_CONFIGURATION_TRANSLATION_X |
        DIRECTMANIPULATION_CONFIGURATION_TRANSLATION_Y | DIRECTMANIPULATION_CONFIGURATION_SCALING |
        DIRECTMANIPULATION_CONFIGURATION_RAILS_X | DIRECTMANIPULATION_CONFIGURATION_RAILS_Y);
    if (FAILED(viewport->ActivateConfiguration(configuration)) ||
        FAILED(viewport->SetViewportOptions(DIRECTMANIPULATION_VIEWPORT_OPTIONS_MANUALUPDATE)) ||
        FAILED(viewport->SetViewportRect(&rect)) ||
        FAILED(viewport->GetPrimaryContent(IID_PPV_ARGS(&content))) ||
        FAILED(manager->Activate(hwnd))) {
      detach();
      return;
    }
    owner = hwnd;
    windowId = id;
    bounds = rect;
    if (FAILED(viewport->Enable()))
      detach();
  }
  void resize(HWND hwnd) {
    if (!viewport || owner != hwnd)
      return;
    RECT rect{};
    if (!GetClientRect(hwnd, &rect) || rect.right <= 0 || rect.bottom <= 0)
      return;
    if (EqualRect(&bounds, &rect))
      return;
    stop(true);
    if (SUCCEEDED(viewport->SetViewportRect(&rect)))
      bounds = rect;
    else
      detach(hwnd);
  }
  void cancel(HWND hwnd = nullptr) {
    if (!hwnd || owner == hwnd)
      stop(true);
  }
  void detach(HWND hwnd = nullptr) {
    if (hwnd && owner != hwnd)
      return;
    stop(true);
    auto old = owner;
    owner = nullptr;
    if (old)
      manager->Deactivate(old);
    if (viewport)
      viewport->Abandon();
    content.Reset();
    viewport.Reset();
  }
  void shutdown() {
    detach();
    updater.Reset();
    manager.Reset();
  }
  bool hit(HWND hwnd, WPARAM param, UINT windowDpi) {
    if (owner != hwnd || !viewport)
      return false;
    UINT pointer = GET_POINTERID_WPARAM(param);
    POINTER_INPUT_TYPE type{};
    if (!GetPointerType(pointer, &type) || type != PT_TOUCHPAD)
      return false;
    if (polling)
      return SUCCEEDED(viewport->SetContact(pointer));
    float matrix[6]{};
    if (FAILED(content->GetContentTransform(matrix, 6)) || !std::isfinite(matrix[0]) ||
        matrix[0] <= 0.f || !std::isfinite(matrix[4]) || !std::isfinite(matrix[5]))
      return false;
    if (!SetTimer(hwnd, Timer, 8, nullptr))
      return false;
    if (FAILED(viewport->SetContact(pointer))) {
      KillTimer(hwnd, Timer);
      return false;
    }
    lastX = matrix[4];
    lastY = matrix[5];
    baseScale = matrix[0];
    lastScale = 1.;
    dpi = windowDpi ? windowDpi : 96;
    polling = true;
    pendingUntil = GetTickCount64() + 1000;
    return true;
  }
  void tick(HWND hwnd, bool eligible) {
    if (!owns(hwnd))
      return;
    if (!eligible) {
      stop(true);
      return;
    }
    DIRECTMANIPULATION_STATUS status{};
    float matrix[6]{};
    if (FAILED(updater->Update(nullptr)) || FAILED(viewport->GetStatus(&status)) ||
        FAILED(content->GetContentTransform(matrix, 6))) {
      stop(true);
      return;
    }
    if (status == DIRECTMANIPULATION_RUNNING) {
      started = true;
      transform(matrix);
    } else if (status == DIRECTMANIPULATION_READY || status == DIRECTMANIPULATION_INERTIA) {
      transform(matrix);
      if (started || kind != Kind::None)
        stop(false);
    } else if (started || status == DIRECTMANIPULATION_DISABLED ||
               status == DIRECTMANIPULATION_SUSPENDED) {
      stop(true);
    }
    if (polling && !started && GetTickCount64() >= pendingUntil)
      stop(true);
  }
};
