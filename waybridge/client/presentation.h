#pragma once
#include "graphics.h"
#include <d3dcompiler.h>
#include <cstdint>
#include <vector>

class Presentation {
  inline static const char *vertexCode =
      R"(struct Out{float4 position:SV_Position;float2 uv:TEXCOORD0;};Out main(uint id:SV_VertexID){Out o;o.uv=float2((id<<1)&2,id&2);o.position=float4(o.uv.x*2-1,1-o.uv.y*2,0,1);return o;})";
  inline static const char *pixelCode =
      R"(Texture2D Y:register(t0);Texture2D UV:register(t1);Texture2D A:register(t2);SamplerState S:register(s0);cbuffer C:register(b0){float4 crop;};float4 main(float4 pos:SV_Position,float2 uv:TEXCOORD0):SV_Target{float2 tc=uv*crop.xy;float y=(Y.Sample(S,tc).r-16.0/255.0)*(255.0/219.0);float2 c=(UV.Sample(S,tc).rg-128.0/255.0)*(255.0/224.0);float3 rgb=float3(y+1.5748*c.y,y-0.187324*c.x-0.468124*c.y,y+1.8556*c.x);float a=A.Sample(S,uv).r;return float4(saturate(rgb)*a,a);})";
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
  bool surface(UINT w, UINT h) {
    if (swapchain && width == w && height == h)
      return true;
    ComPtr<IDXGISwapChain1> next;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = w;
    desc.Height = h;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    if (FAILED(
            gpu->factory->CreateSwapChainForComposition(gpu->device.Get(), &desc, nullptr, &next)))
      return false;
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
  bool initialize(HWND hwnd) {
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
                    const std::vector<uint8_t> &alpha, const RECT &rect) {
    GraphicsLock lock(gpu);
    if (!surface(w, h) || !shaders())
      return false;
    D3D11_TEXTURE2D_DESC source{};
    texture->GetDesc(&source);
    if (source.Format != DXGI_FORMAT_NV12 || source.Width < w || source.Height < h)
      return false;
    D3D11_TEXTURE2D_DESC previous{};
    if (videoTexture)
      videoTexture->GetDesc(&previous);
    if (!videoTexture || previous.Width != source.Width || previous.Height != source.Height) {
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
      view.Format = DXGI_FORMAT_R8_UNORM;
      if (FAILED(gpu->device->CreateShaderResourceView(videoTexture.Get(), &view, &luma)))
        return false;
      view.Format = DXGI_FORMAT_R8G8_UNORM;
      if (FAILED(gpu->device->CreateShaderResourceView(videoTexture.Get(), &view, &chroma)))
        return false;
    }
    gpu->context->CopySubresourceRegion(videoTexture.Get(), 0, 0, 0, 0, texture, subresource,
                                        nullptr);
    D3D11_TEXTURE2D_DESC a{};
    a.Width = alpha.empty() ? 1 : w;
    a.Height = alpha.empty() ? 1 : h;
    a.MipLevels = a.ArraySize = 1;
    a.Format = DXGI_FORMAT_R8_UNORM;
    a.SampleDesc.Count = 1;
    a.Usage = D3D11_USAGE_IMMUTABLE;
    a.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    uint8_t opaque = 255;
    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = alpha.empty() ? &opaque : alpha.data();
    data.SysMemPitch = a.Width;
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
    float crop[] = {float(w) / source.Width, float(h) / source.Height, 0, 0};
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
