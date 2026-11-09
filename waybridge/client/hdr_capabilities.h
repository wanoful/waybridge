#pragma once
#include "graphics.h"
#include "third_party/json.hpp"
#include <dxgi1_6.h>
#include <cstdint>

// Read-only probes. No display mode/HDR setting is changed and nothing is presented.
struct HdrCapabilities {
  static bool active(HWND window) {
    auto gpu = GraphicsDevice::get();
    if (!gpu || !gpu->hardware)
      return false;
    HMONITOR wanted = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    for (UINT adapterIndex = 0;; ++adapterIndex) {
      ComPtr<IDXGIAdapter1> adapter;
      if (gpu->factory->EnumAdapters1(adapterIndex, &adapter) == DXGI_ERROR_NOT_FOUND)
        break;
      if (!adapter)
        break;
      for (UINT outputIndex = 0;; ++outputIndex) {
        ComPtr<IDXGIOutput> output;
        if (adapter->EnumOutputs(outputIndex, &output) == DXGI_ERROR_NOT_FOUND)
          break;
        if (!output)
          break;
        ComPtr<IDXGIOutput6> extended;
        DXGI_OUTPUT_DESC1 desc{};
        if (SUCCEEDED(output.As(&extended)) && SUCCEEDED(extended->GetDesc1(&desc)) &&
            desc.Monitor == wanted && desc.AttachedToDesktop)
          return desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 &&
                 desc.BitsPerColor >= 10;
      }
    }
    return false;
  }
  static nlohmann::json inspect() {
    using Json = nlohmann::json;
    Json result = {{"streaming_hdr", false},
                   {"status", "partial-preflight"},
                   {"current_decoder_format", "NV12"},
                   {"current_swapchain_format", "BGRA8"},
                   {"current_wire_color_spaces", Json::array({"bt709_sdr"})},
                   {"hardware_device", false},
                   {"outputs", Json::array()},
                   {"fp16_composition_swapchain", false},
                   {"scrgb_color_space", false},
                   {"fp16_values_preserved", false}};
    auto gpu = GraphicsDevice::get();
    if (!gpu || !gpu->hardware) {
      result["reason"] = "no hardware D3D11/DirectComposition device";
      return result;
    }
    result["hardware_device"] = true;
    GraphicsLock lock(gpu);
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = description.Height = 2;
    description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    description.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    ComPtr<IDXGISwapChain1> swapchain;
    HRESULT status = gpu->factory->CreateSwapChainForComposition(gpu->device.Get(), &description,
                                                                 nullptr, &swapchain);
    result["fp16_composition_swapchain"] = SUCCEEDED(status);
    if (SUCCEEDED(status)) {
      ComPtr<IDXGISwapChain3> color;
      UINT support = 0;
      result["scrgb_color_space"] =
          SUCCEEDED(swapchain.As(&color)) &&
          SUCCEEDED(
              color->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, &support)) &&
          (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) &&
          SUCCEEDED(color->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709));
      ComPtr<ID3D11Texture2D> buffer, staging;
      ComPtr<ID3D11RenderTargetView> target;
      if (SUCCEEDED(swapchain->GetBuffer(0, IID_PPV_ARGS(&buffer))) &&
          SUCCEEDED(gpu->device->CreateRenderTargetView(buffer.Get(), nullptr, &target))) {
        const float sample[] = {2.f, .25f, 0.f, .5f};
        gpu->context->ClearRenderTargetView(target.Get(), sample);
        D3D11_TEXTURE2D_DESC desc{};
        buffer->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        if (SUCCEEDED(gpu->device->CreateTexture2D(&desc, nullptr, &staging))) {
          gpu->context->CopyResource(staging.Get(), buffer.Get());
          D3D11_MAPPED_SUBRESOURCE mapped{};
          if (SUCCEEDED(gpu->context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            const auto *value = static_cast<const uint16_t *>(mapped.pData);
            result["fp16_values_preserved"] =
                value[0] == 0x4000 && value[1] == 0x3400 && value[3] == 0x3800;
            gpu->context->Unmap(staging.Get(), 0);
          }
        }
      }
    }
    for (UINT ai = 0;; ++ai) {
      ComPtr<IDXGIAdapter1> adapter;
      if (gpu->factory->EnumAdapters1(ai, &adapter) == DXGI_ERROR_NOT_FOUND)
        break;
      if (!adapter)
        break;
      for (UINT oi = 0;; ++oi) {
        ComPtr<IDXGIOutput> output;
        if (adapter->EnumOutputs(oi, &output) == DXGI_ERROR_NOT_FOUND)
          break;
        if (!output)
          break;
        ComPtr<IDXGIOutput6> extended;
        DXGI_OUTPUT_DESC1 desc{};
        if (FAILED(output.As(&extended)) || FAILED(extended->GetDesc1(&desc))) {
          result["outputs"].push_back({{"capability_known", false}});
          continue;
        }
        // DXGI reports the active color mode, not HDR supported-but-disabled.
        result["outputs"].push_back(
            {{"capability_known", true},
             {"attached", bool(desc.AttachedToDesktop)},
             {"bits_per_color", desc.BitsPerColor},
             {"active_color_space", unsigned(desc.ColorSpace)},
             {"hdr10_active", desc.AttachedToDesktop && desc.BitsPerColor >= 10 &&
                                  desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020},
             {"max_luminance_nits", desc.MaxLuminance},
             {"max_full_frame_nits", desc.MaxFullFrameLuminance}});
      }
    }
    result["reason"] =
        "render/monitor capabilities do not yet enable a color-managed Main10/P010 streaming path";
    return result;
  }
};
