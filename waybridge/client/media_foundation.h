#pragma once
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <cstring>

// Media Foundation is optional (for example, Windows N without its media feature pack).
// PNG and DirectComposition must remain usable when mfplat.dll or the decoder is absent.
struct MediaFoundation {
  HMODULE library = nullptr;
  decltype(&MFStartup) Startup = nullptr;
  decltype(&MFShutdown) Shutdown = nullptr;
  decltype(&MFCreateDXGIDeviceManager) CreateDXGIDeviceManager = nullptr;
  decltype(&MFCreateMediaType) CreateMediaType = nullptr;
  decltype(&MFCreateSample) CreateSample = nullptr;
  decltype(&MFCreateMemoryBuffer) CreateMemoryBuffer = nullptr;
  template <class Function> Function procedure(const char *name) {
    FARPROC address = GetProcAddress(library, name);
    Function function = nullptr;
    static_assert(sizeof(function) == sizeof(address));
    std::memcpy(&function, &address, sizeof(function));
    return function;
  }
  MediaFoundation() {
    library = LoadLibraryW(L"mfplat.dll");
    if (!library)
      return;
    Startup = procedure<decltype(Startup)>("MFStartup");
    Shutdown = procedure<decltype(Shutdown)>("MFShutdown");
    CreateDXGIDeviceManager =
        procedure<decltype(CreateDXGIDeviceManager)>("MFCreateDXGIDeviceManager");
    CreateMediaType = procedure<decltype(CreateMediaType)>("MFCreateMediaType");
    CreateSample = procedure<decltype(CreateSample)>("MFCreateSample");
    CreateMemoryBuffer = procedure<decltype(CreateMemoryBuffer)>("MFCreateMemoryBuffer");
  }
  bool available() const {
    return Startup && Shutdown && CreateDXGIDeviceManager && CreateMediaType && CreateSample &&
           CreateMemoryBuffer;
  }
  static MediaFoundation &get() {
    static MediaFoundation api;
    return api;
  }
};
