#include <windows.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <iostream>
#include <string>

// Resolve only the named test/output endpoint. Never changes the default device.
int wmain(int argc, wchar_t **argv) {
  if (argc != 2) return 2;
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  int result = 3;
  {
    using Microsoft::WRL::ComPtr;
    ComPtr<IMMDeviceEnumerator> devices;
    ComPtr<IMMDeviceCollection> collection;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&devices))) &&
        SUCCEEDED(devices->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))) {
      UINT count = 0; collection->GetCount(&count);
      for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        ComPtr<IPropertyStore> properties;
        PROPVARIANT name{};
        if (FAILED(collection->Item(i, &device)) ||
            FAILED(device->OpenPropertyStore(STGM_READ, &properties)) ||
            FAILED(properties->GetValue(PKEY_Device_FriendlyName, &name))) continue;
        bool match = name.vt == VT_LPWSTR && std::wstring(name.pwszVal).find(argv[1]) != std::wstring::npos;
        PropVariantClear(&name);
        if (!match) continue;
        LPWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id))) {
          std::wstring wide(id); CoTaskMemFree(id);
          int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), nullptr, 0, nullptr, nullptr);
          std::string utf8(n, '\0');
          WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), utf8.data(), n, nullptr, nullptr);
          std::cout << utf8 << '\n'; result = 0; break;
        }
      }
    }
  }
  CoUninitialize(); return result;
}
