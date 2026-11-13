#pragma once
#include "graphics.h"
#include "hdr_capabilities.h"
#include <d3dcompiler.h>
#include <cstdint>
#include <vector>
#include <set>
#include <cmath>

class Presentation {
  inline static const char *vertexCode =
      R"(struct Out{float4 position:SV_Position;float2 uv:TEXCOORD0;};Out main(uint id:SV_VertexID){Out o;o.uv=float2((id<<1)&2,id&2);o.position=float4(o.uv.x*2-1,1-o.uv.y*2,0,1);return o;})";
  inline static const char *pixelCode = R"(
Texture2D Y:register(t0);Texture2D UV:register(t1);Texture2D A:register(t2);
SamplerState S:register(s0);cbuffer C:register(b0){float4 crop;};
float3 inversePQ(float3 c){float3 p=pow(max(c,0),1.0/(2523.0/32.0));return pow(max(p-3424.0/4096.0,0)/(2413.0/128.0-2392.0/128.0*p),1.0/(2610.0/16384.0))*125.0;}
float4 main(float4 pos:SV_Position,float2 uv:TEXCOORD0):SV_Target {
 float2 tc=uv*crop.xy;float y=Y.Sample(S,tc).r;float2 c=UV.Sample(S,tc).rg;float3 rgb;
 if(crop.z>0.5){
  // P010 UNORM views expose high 10 bits in 16-bit channels, not /1023.
  y=(y*65535.0/64.0-64.0)/876.0;c=(c*65535.0/64.0-512.0)/896.0;
  float3 pq=float3(y+1.4746*c.y,y-.1645531*c.x-.5713531*c.y,y+1.8814*c.x);
  float3 b=inversePQ(saturate(pq));
  rgb=float3(1.660491*b.x-.5876411*b.y-.0728499*b.z,-.1245505*b.x+1.1328999*b.y-.0083494*b.z,-.0181508*b.x-.1005789*b.y+1.1187297*b.z);
 }else{
  y=(y-16.0/255.0)*(255.0/219.0);c=(c-128.0/255.0)*(255.0/224.0);
  rgb=saturate(float3(y+1.5748*c.y,y-.187324*c.x-.468124*c.y,y+1.8556*c.x));
 }
 float a=A.Sample(S,uv).r;return float4(rgb*a,a);
})";
  std::shared_ptr<GraphicsDevice> gpu;
  ComPtr<IDCompositionTarget> target;
  ComPtr<IDCompositionVisual> visual;
  ComPtr<IDXGISwapChain1> swapchain;
  ComPtr<ID3D11VertexShader> vertex;
  ComPtr<ID3D11PixelShader> pixel;
  ComPtr<ID3D11SamplerState> sampler;
  ComPtr<ID3D11Buffer> constants;
  ComPtr<ID3D11Texture2D> videoTexture;
  ComPtr<ID3D11ShaderResourceView> luma, chroma;
  UINT width = 0, height = 0;
  bool hdrSurface = false;
  HWND window = nullptr;
  bool surface(UINT w, UINT h, bool hdr = false) {
    if (swapchain && width == w && height == h && hdrSurface == hdr)
      return true;
    ComPtr<IDXGISwapChain1> next;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = w;
    desc.Height = h;
    desc.Format = hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    if (FAILED(
            gpu->factory->CreateSwapChainForComposition(gpu->device.Get(), &desc, nullptr, &next)))
      return false;
    if (hdr) {
      ComPtr<IDXGISwapChain3> color;
      UINT support = 0;
      if (FAILED(next.As(&color)) ||
          FAILED(
              color->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, &support)) ||
          !(support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) ||
          FAILED(color->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709)))
        return false;
    }
    hdrSurface = hdr;
    swapchain = next;
    width = w;
    height = h;
    return true;
  }
  bool finish(const RECT &rect) {
    return SUCCEEDED(swapchain->Present(0, 0)) && SUCCEEDED(visual->SetContent(swapchain.Get())) &&
           layout(rect);
  }
  bool shaders() {
    if (vertex && pixel && sampler && constants)
      return true;
    const char *vs = vertexCode, *ps = pixelCode;
    ComPtr<ID3DBlob> code, error;
    if (FAILED(D3DCompile(vs, strlen(vs), nullptr, nullptr, nullptr, "main", "vs_4_0", 0, 0, &code,
                          &error)) ||
        FAILED(gpu->device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(),
                                               nullptr, &vertex)))
      return false;
    code.Reset();
    error.Reset();
    if (FAILED(D3DCompile(ps, strlen(ps), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &code,
                          &error)) ||
        FAILED(gpu->device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(),
                                              nullptr, &pixel)))
      return false;
    D3D11_SAMPLER_DESC sample{};
    sample.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sample.AddressU = sample.AddressV = sample.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sample.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(gpu->device->CreateSamplerState(&sample, &sampler)))
      return false;
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = 16;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    return SUCCEEDED(gpu->device->CreateBuffer(&desc, nullptr, &constants));
  }

public:
  static bool validateShaders() {
    for (auto pair :
         {std::pair<const char *, const char *>{vertexCode, "vs_4_0"}, {pixelCode, "ps_4_0"}}) {
      ComPtr<ID3DBlob> code, error;
      HRESULT hr = D3DCompile(pair.first, strlen(pair.first), nullptr, nullptr, nullptr, "main",
                              pair.second, 0, 0, &code, &error);
      if (FAILED(hr)) {
        if (error)
          std::cerr.write(static_cast<const char *>(error->GetBufferPointer()),
                          error->GetBufferSize());
        return false;
      }
    }
    std::cout << "H264 NV12/alpha shaders compiled successfully\n";
    return true;
  }
  // Offscreen numeric check of the exact presentation shader. WARP/Wine may
  // validate arithmetic, but this does not advertise a hardware HDR decoder.
  static nlohmann::json validateHdrPixels() {
    using Json = nlohmann::json;
    Json result = {{"verified", false}, {"hardware", false}, {"p010_plane_views", false}};
    Presentation p;
    p.gpu = GraphicsDevice::get();
    if (!p.gpu) {
      auto diagnostic = std::make_shared<GraphicsDevice>();
      if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &diagnostic->device, nullptr,
                                   &diagnostic->context))) {
        result["reason"] = "no D3D11 device";
        return result;
      }
      p.gpu = diagnostic;
    }
    result["hardware"] = p.gpu->hardware;
    GraphicsLock lock(p.gpu);
    if (!p.shaders()) {
      result["reason"] = "shader creation failed";
      return result;
    }
    constexpr UINT w = 1024, h = 1;
    std::vector<uint16_t> y(w), uv(w * 2), alpha(w);
    for (UINT x = 0; x < w; ++x) {
      y[x] = uint16_t((64 + x * 876 / (w - 1)) << 6);
      uv[x * 2] = uint16_t((x % 16 == 0 ? 600 : 512) << 6);
      uv[x * 2 + 1] = uint16_t((x % 16 == 0 ? 440 : 512) << 6);
      alpha[x] = uint16_t(0x3000 + x);
    }
    auto plane = [&](DXGI_FORMAT format, const void *data, UINT pitch,
                     ComPtr<ID3D11ShaderResourceView> &view) {
      D3D11_TEXTURE2D_DESC d{};
      d.Width = w;
      d.Height = h;
      d.MipLevels = d.ArraySize = 1;
      d.Format = format;
      d.SampleDesc.Count = 1;
      d.Usage = D3D11_USAGE_IMMUTABLE;
      d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      D3D11_SUBRESOURCE_DATA source{};
      source.pSysMem = data;
      source.SysMemPitch = pitch;
      ComPtr<ID3D11Texture2D> texture;
      return SUCCEEDED(p.gpu->device->CreateTexture2D(&d, &source, &texture)) &&
             SUCCEEDED(p.gpu->device->CreateShaderResourceView(texture.Get(), nullptr, &view));
    };
    ComPtr<ID3D11ShaderResourceView> Y, UV, A;
    if (!plane(DXGI_FORMAT_R16_UNORM, y.data(), w * 2, Y) ||
        !plane(DXGI_FORMAT_R16G16_UNORM, uv.data(), w * 4, UV) ||
        !plane(DXGI_FORMAT_R16_FLOAT, alpha.data(), w * 2, A)) {
      result["reason"] = "plane texture creation failed";
      return result;
    }
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> output, staging;
    ComPtr<ID3D11RenderTargetView> target;
    if (FAILED(p.gpu->device->CreateTexture2D(&d, nullptr, &output)) ||
        FAILED(p.gpu->device->CreateRenderTargetView(output.Get(), nullptr, &target))) {
      result["reason"] = "FP16 target creation failed";
      return result;
    }
    auto ctx = p.gpu->context.Get();
    ID3D11RenderTargetView *rt = target.Get();
    ctx->OMSetRenderTargets(1, &rt, nullptr);
    D3D11_VIEWPORT viewport{0, 0, float(w), float(h), 0, 1};
    ctx->RSSetViewports(1, &viewport);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(p.vertex.Get(), nullptr, 0);
    ctx->PSSetShader(p.pixel.Get(), nullptr, 0);
    float crop[] = {1, 1, 1, 0};
    ctx->UpdateSubresource(p.constants.Get(), 0, nullptr, crop, 0, 0);
    ID3D11Buffer *cb = p.constants.Get();
    ctx->PSSetConstantBuffers(0, 1, &cb);
    ID3D11SamplerState *sampler = p.sampler.Get();
    ctx->PSSetSamplers(0, 1, &sampler);
    ID3D11ShaderResourceView *views[] = {Y.Get(), UV.Get(), A.Get()};
    ctx->PSSetShaderResources(0, 3, views);
    ctx->Draw(3, 0);
    ctx->ClearState();
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(p.gpu->device->CreateTexture2D(&d, nullptr, &staging))) {
      result["reason"] = "FP16 staging creation failed";
      return result;
    }
    ctx->CopyResource(staging.Get(), output.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
      result["reason"] = "FP16 map failed";
      return result;
    }
    auto half = [](uint16_t v) {
      int e = (v >> 10) & 31;
      float c = std::ldexp(float(v & 1023), -24);
      if (e)
        c = std::ldexp(1.f + float(v & 1023) / 1024.f, e - 15);
      return v & 0x8000 ? -c : c;
    };
    auto inverse = [](double value) {
      double q = std::pow(value, 1. / (2523. / 32.));
      return std::pow(std::fmax(q - 3424. / 4096., 0.) / (2413. / 128. - 2392. / 128. * q),
                      1. / (2610. / 16384.)) *
             125.;
    };
    auto values = static_cast<const uint16_t *>(mapped.pData);
    std::set<uint16_t> distinct;
    double error = 0, maximum = 0;
    bool exactAlpha = true;
    for (UINT x = 0; x < w; ++x) {
      double a = half(alpha[x]), Y = double((y[x] >> 6) - 64) / 876.,
             U = double((uv[x * 2] >> 6) - 512) / 896.,
             V = double((uv[x * 2 + 1] >> 6) - 512) / 896.;
      auto clamp = [](double v) { return std::fmax(0., std::fmin(1., v)); };
      double R = inverse(clamp(Y + 1.4746 * V)),
             G = inverse(clamp(Y - .1645531 * U - .5713531 * V)),
             B = inverse(clamp(Y + 1.8814 * U));
      double expected[] = {1.660491 * R - .5876411 * G - .0728499 * B,
                           -.1245505 * R + 1.1328999 * G - .0083494 * B,
                           -.0181508 * R - .1005789 * G + 1.1187297 * B};
      for (int c = 0; c < 3; ++c) {
        double value = half(values[x * 4 + c]) / a;
        maximum = std::fmax(maximum, value);
        error = std::fmax(error,
                          std::fabs(value - expected[c]) / std::fmax(std::fabs(expected[c]), .1));
      }
      distinct.insert(values[x * 4]);
      exactAlpha &= values[x * 4 + 3] == alpha[x];
    }
    ctx->Unmap(staging.Get(), 0);
    result["distinct_red_values"] = distinct.size();
    result["max_scrgb"] = maximum;
    result["max_relative_error"] = error;
    result["exact_fp16_alpha"] = exactAlpha;
    result["verified"] = distinct.size() > 256 && maximum > 100 && error < .02 && exactAlpha;
    // Also require real P010 plane SRVs before capability advertisement. This
    // arithmetic test uses separate planes so software implementations can run it.
    D3D11_TEXTURE2D_DESC planar{};
    planar.Width = planar.Height = 4;
    planar.MipLevels = planar.ArraySize = 1;
    planar.Format = DXGI_FORMAT_P010;
    planar.SampleDesc.Count = 1;
    planar.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> luma, chroma;
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels = 1;
    view.Format = DXGI_FORMAT_R16_UNORM;
    bool planes = SUCCEEDED(p.gpu->device->CreateTexture2D(&planar, nullptr, &texture)) &&
                  SUCCEEDED(p.gpu->device->CreateShaderResourceView(texture.Get(), &view, &luma));
    view.Format = DXGI_FORMAT_R16G16_UNORM;
    result["p010_plane_views"] =
        planes && SUCCEEDED(p.gpu->device->CreateShaderResourceView(texture.Get(), &view, &chroma));
    return result;
  }
  bool initialize(HWND hwnd) {
    window = hwnd;
    gpu = GraphicsDevice::get();
    if (!gpu)
      return false;
    return SUCCEEDED(gpu->composition->CreateTargetForHwnd(hwnd, TRUE, &target)) &&
           SUCCEEDED(gpu->composition->CreateVisual(&visual)) &&
           SUCCEEDED(target->SetRoot(visual.Get())) && SUCCEEDED(gpu->composition->Commit());
  }
  bool present(const uint8_t *bgra, UINT w, UINT h, const RECT &rect) {
    GraphicsLock lock(gpu);
    if (!surface(w, h))
      return false;
    ComPtr<ID3D11Texture2D> buffer;
    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&buffer))))
      return false;
    gpu->context->UpdateSubresource(buffer.Get(), 0, nullptr, bgra, w * 4, 0);
    return finish(rect);
  }
  bool presentVideo(ID3D11Texture2D *texture, UINT subresource, UINT w, UINT h,
                    const std::vector<uint8_t> &alpha, const RECT &rect, bool hdr = false) {
    GraphicsLock lock(gpu);
    if ((hdr && !HdrCapabilities::active(window)) || !surface(w, h, hdr) || !shaders())
      return false;
    D3D11_TEXTURE2D_DESC source{};
    texture->GetDesc(&source);
    if (source.Format != (hdr ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12) || source.Width < w ||
        source.Height < h)
      return false;
    D3D11_TEXTURE2D_DESC previous{};
    if (videoTexture)
      videoTexture->GetDesc(&previous);
    if (!videoTexture || previous.Width != source.Width || previous.Height != source.Height ||
        previous.Format != source.Format) {
      videoTexture.Reset();
      luma.Reset();
      chroma.Reset();
      source.ArraySize = 1;
      source.MipLevels = 1;
      source.Usage = D3D11_USAGE_DEFAULT;
      source.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      source.CPUAccessFlags = 0;
      source.MiscFlags = 0;
      if (FAILED(gpu->device->CreateTexture2D(&source, nullptr, &videoTexture)))
        return false;
      D3D11_SHADER_RESOURCE_VIEW_DESC view{};
      view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
      view.Texture2D.MipLevels = 1;
      view.Format = hdr ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
      if (FAILED(gpu->device->CreateShaderResourceView(videoTexture.Get(), &view, &luma)))
        return false;
      view.Format = hdr ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
      if (FAILED(gpu->device->CreateShaderResourceView(videoTexture.Get(), &view, &chroma)))
        return false;
    }
    gpu->context->CopySubresourceRegion(videoTexture.Get(), 0, 0, 0, 0, texture, subresource,
                                        nullptr);
    D3D11_TEXTURE2D_DESC a{};
    a.Width = alpha.empty() ? 1 : w;
    a.Height = alpha.empty() ? 1 : h;
    a.MipLevels = a.ArraySize = 1;
    a.Format = hdr ? DXGI_FORMAT_R16_FLOAT : DXGI_FORMAT_R8_UNORM;
    a.SampleDesc.Count = 1;
    a.Usage = D3D11_USAGE_IMMUTABLE;
    a.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    uint16_t opaque = hdr ? 0x3c00 : 255;
    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = alpha.empty() ? static_cast<const void *>(&opaque)
                                 : static_cast<const void *>(alpha.data());
    data.SysMemPitch = a.Width * (hdr ? 2 : 1);
    ComPtr<ID3D11Texture2D> mask;
    ComPtr<ID3D11ShaderResourceView> alphaView;
    if (FAILED(gpu->device->CreateTexture2D(&a, &data, &mask)) ||
        FAILED(gpu->device->CreateShaderResourceView(mask.Get(), nullptr, &alphaView)))
      return false;
    ComPtr<ID3D11Texture2D> back;
    ComPtr<ID3D11RenderTargetView> targetView;
    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&back))) ||
        FAILED(gpu->device->CreateRenderTargetView(back.Get(), nullptr, &targetView)))
      return false;
    float crop[] = {float(w) / source.Width, float(h) / source.Height, hdr ? 1.f : 0.f, 0};
    gpu->context->UpdateSubresource(constants.Get(), 0, nullptr, crop, 0, 0);
    auto ctx = gpu->context.Get();
    ID3D11RenderTargetView *rt = targetView.Get();
    ctx->OMSetRenderTargets(1, &rt, nullptr);
    D3D11_VIEWPORT viewport{0, 0, float(w), float(h), 0, 1};
    ctx->RSSetViewports(1, &viewport);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vertex.Get(), nullptr, 0);
    ctx->PSSetShader(pixel.Get(), nullptr, 0);
    ID3D11Buffer *cb = constants.Get();
    ctx->PSSetConstantBuffers(0, 1, &cb);
    ID3D11SamplerState *ss = sampler.Get();
    ctx->PSSetSamplers(0, 1, &ss);
    ID3D11ShaderResourceView *views[] = {luma.Get(), chroma.Get(), alphaView.Get()};
    ctx->PSSetShaderResources(0, 3, views);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView *empty[] = {nullptr, nullptr, nullptr};
    ctx->PSSetShaderResources(0, 3, empty);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    return finish(rect);
  }
  bool layout(const RECT &r) {
    if (!width || !height)
      return true;
    D2D_MATRIX_3X2_F matrix{};
    matrix._11 = float(r.right - r.left) / width;
    matrix._22 = float(r.bottom - r.top) / height;
    matrix._31 = float(r.left);
    matrix._32 = float(r.top);
    return SUCCEEDED(visual->SetTransform(matrix)) && SUCCEEDED(gpu->composition->Commit());
  }
};
