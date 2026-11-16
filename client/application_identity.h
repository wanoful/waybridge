#pragma once
#include <windows.h>
#include <shellapi.h>
#include <propsys.h>
#include <propkey.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>

namespace ApplicationIdentity {
// These hashes are compact namespace labels, not authentication credentials.
// The GUI supplies its persistent profile ID, independent of ephemeral tunnels.
inline std::wstring hash(const std::string &text) {
  uint64_t value = 14695981039346656037ull;
  for (unsigned char byte : text) { value ^= byte; value *= 1099511628211ull; }
  const wchar_t digits[] = L"0123456789abcdef";
  std::wstring result(16, L'0');
  for (int i = 15; i >= 0; --i) { result[i] = digits[value & 15]; value >>= 4; }
  return result;
}
inline std::wstring id(const std::string &source, const std::string &app, uint32_t window) {
  return L"Waybridge.Remote." + hash(source) + L"." + hash(app.empty() ? "window:" + std::to_string(window) : "app:" + app);
}
inline void clear(HWND hwnd) {
  Microsoft::WRL::ComPtr<IPropertyStore> store;
  if (SUCCEEDED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&store)))) {
    PROPVARIANT empty{};
    store->SetValue(PKEY_AppUserModel_ID, empty);
    store->SetValue(PKEY_AppUserModel_PreventPinning, empty);
  }
}
inline bool apply(HWND hwnd, const std::string &source, const std::string &app, uint32_t window) {
  Microsoft::WRL::ComPtr<IPropertyStore> store;
  if (FAILED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&store)))) return false;
  auto identity = id(source, app, window);
  PROPVARIANT value{};
  // A generic worker shortcut cannot relaunch a particular remote application.
  // Set this before AppUserModelID, as required by the Windows Shell contract.
  value.vt = VT_BOOL; value.boolVal = VARIANT_TRUE;
  if (FAILED(store->SetValue(PKEY_AppUserModel_PreventPinning, value))) return false;
  value.vt = VT_LPWSTR; value.pwszVal = identity.data();
  return SUCCEEDED(store->SetValue(PKEY_AppUserModel_ID, value));
}
}
