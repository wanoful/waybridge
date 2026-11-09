#pragma once
#include <windows.h>
#include <d3d11.h>
#include <d3d10.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <wrl/client.h>
#include <memory>
#include <mutex>
#include <iostream>

template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
struct GraphicsDevice {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<IDXGIFactory2> factory;
  ComPtr<IDCompositionDevice> composition;
  ComPtr<ID3D10Multithread> protection;
  bool hardware = false;
  static std::shared_ptr<GraphicsDevice> get() {
    static std::once_flag once;
    static std::shared_ptr<GraphicsDevice> shared;
    std::call_once(once, [] {
      if (GetEnvironmentVariableW(L"WAYBRIDGE_GDI", nullptr, 0))
        return;
      auto gpu = std::make_shared<GraphicsDevice>();
      HRESULT hr =
          D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                            D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                            nullptr, 0, D3D11_SDK_VERSION, &gpu->device, nullptr, &gpu->context);
      gpu->hardware = SUCCEEDED(hr);
      if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                               &gpu->device, nullptr, &gpu->context);
      if (FAILED(hr))
        return;
      if (FAILED(gpu->device.As(&gpu->protection)))
        gpu->context.As(&gpu->protection);
      if (gpu->protection)
        gpu->protection->SetMultithreadProtected(TRUE);
      else
        gpu->hardware = false;
      ComPtr<IDXGIDevice> dxgi;
      ComPtr<IDXGIAdapter> adapter;
      if (FAILED(gpu->device.As(&dxgi)) || FAILED(dxgi->GetAdapter(&adapter)) ||
          FAILED(adapter->GetParent(IID_PPV_ARGS(&gpu->factory))) ||
          FAILED(DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&gpu->composition))))
        return;
      shared = gpu;
    });
    return shared;
  }
};
struct GraphicsLock {
  std::shared_ptr<GraphicsDevice> gpu;
  explicit GraphicsLock(std::shared_ptr<GraphicsDevice> g) : gpu(std::move(g)) {
    if (gpu->protection)
      gpu->protection->Enter();
  }
  ~GraphicsLock() {
    if (gpu->protection)
      gpu->protection->Leave();
  }
};
