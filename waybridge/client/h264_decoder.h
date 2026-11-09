#pragma once
#include "graphics.h"
#include "media_foundation.h"
#include "h264_bitstream.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <wmcodecdsp.h>
#include <codecapi.h>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

struct VideoSurface {
  ComPtr<IMFSample> sample;
  ComPtr<ID3D11Texture2D> texture;
  UINT subresource = 0;
  uint64_t sequence = 0;
};
class H264Decoder {
  std::shared_ptr<GraphicsDevice> gpu;
  ComPtr<IMFTransform> transform;
  ComPtr<IMFDXGIDeviceManager> manager;
  UINT width, height;
  void check(HRESULT hr, const char *where) {
    if (FAILED(hr))
      throw std::runtime_error(std::string(where) + " failed: " + std::to_string(unsigned(hr)));
  }
  void outputType() {
    for (DWORD i = 0;; ++i) {
      ComPtr<IMFMediaType> type;
      check(transform->GetOutputAvailableType(0, i, &type), "H264 output types");
      GUID subtype{};
      type->GetGUID(MF_MT_SUBTYPE, &subtype);
      if (subtype == MFVideoFormat_NV12) {
        check(transform->SetOutputType(0, type.Get(), 0), "H264 NV12 output");
        return;
      }
    }
  }
  void drain(std::vector<VideoSurface> &frames) {
    for (unsigned i = 0; i < 8; ++i) {
      MFT_OUTPUT_STREAM_INFO info{};
      check(transform->GetOutputStreamInfo(0, &info), "H264 output info");
      if (!(info.dwFlags &
            (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)))
        throw std::runtime_error("H264 decoder did not provide GPU samples");
      MFT_OUTPUT_DATA_BUFFER output{};
      DWORD status = 0;
      HRESULT hr = transform->ProcessOutput(0, 1, &output, &status);
      if (output.pEvents)
        output.pEvents->Release();
      ComPtr<IMFSample> sample;
      sample.Attach(output.pSample);
      if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
        return;
      if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        outputType();
        continue;
      }
      check(hr, "H264 ProcessOutput");
      if (!sample)
        continue;
      ComPtr<IMFMediaBuffer> buffer;
      check(sample->GetBufferByIndex(0, &buffer), "H264 sample buffer");
      ComPtr<IMFDXGIBuffer> dxgi;
      check(buffer.As(&dxgi), "H264 hardware texture");
      VideoSurface frame;
      frame.sample = sample;
      check(dxgi->GetResource(IID_PPV_ARGS(&frame.texture)), "H264 texture resource");
      check(dxgi->GetSubresourceIndex(&frame.subresource), "H264 texture slice");
      LONGLONG time = 0;
      check(sample->GetSampleTime(&time), "H264 sample sequence");
      frame.sequence = uint64_t(time) / 10000;
      frames.push_back(std::move(frame));
    }
  }

public:
  bool matches(UINT w, UINT h) const { return width == w && height == h; }
  uint64_t pixels() const { return uint64_t(width) * height; }
  void restart() {
    check(transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0), "H264 flush epoch");
    check(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "H264 restart epoch");
  }
  static bool available() {
    auto gpu = GraphicsDevice::get();
    if (!gpu || !gpu->hardware)
      return false;
    ComPtr<IMFTransform> test;
    if (FAILED(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&test))))
      return false;
    ComPtr<IMFAttributes> attributes;
    UINT aware = 0;
    return SUCCEEDED(test->GetAttributes(&attributes)) &&
           SUCCEEDED(attributes->GetUINT32(MF_SA_D3D11_AWARE, &aware)) && aware;
  }
  H264Decoder(UINT w, UINT h) : gpu(GraphicsDevice::get()), width(w), height(h) {
    if (!gpu || !gpu->hardware)
      throw std::runtime_error("D3D11 hardware device unavailable");
    check(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                           IID_PPV_ARGS(&transform)),
          "H264 decoder");
    ComPtr<IMFAttributes> attributes;
    check(transform->GetAttributes(&attributes), "H264 attributes");
    UINT aware = 0;
    check(attributes->GetUINT32(MF_SA_D3D11_AWARE, &aware), "H264 D3D11 awareness");
    if (!aware)
      throw std::runtime_error("H264 decoder is not D3D11 aware");
    check(attributes->SetUINT32(MF_LOW_LATENCY, TRUE), "H264 low latency attribute");
    ComPtr<ICodecAPI> codec;
    if (SUCCEEDED(transform->QueryInterface(IID_ICodecAPI,
                                            reinterpret_cast<void **>(codec.GetAddressOf())))) {
      VARIANT value;
      VariantInit(&value);
      // The Microsoft H.264 decoder requires VT_UI4, unlike other codecs.
      // VT_BOOL fails with E_INVALIDARG.
      value.vt = VT_UI4;
      value.ulVal = 1;
      check(codec->SetValue(&CODECAPI_AVLowLatencyMode, &value), "H264 low latency mode");
    }
    UINT token = 0;
    check(MediaFoundation::get().CreateDXGIDeviceManager(&token, &manager), "DXGI manager");
    check(manager->ResetDevice(gpu->device.Get(), token), "DXGI decoder device");
    check(transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                    reinterpret_cast<ULONG_PTR>(manager.Get())),
          "H264 D3D manager");
    ComPtr<IMFMediaType> input;
    check(MediaFoundation::get().CreateMediaType(&input), "H264 input type");
    input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(input.Get(), MF_MT_FRAME_RATE, 60, 1);
    MFSetAttributeRatio(input.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    check(transform->SetInputType(0, input.Get(), 0), "H264 input");
    outputType();
    check(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "H264 begin streaming");
    check(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "H264 start stream");
  }
  std::vector<VideoSurface> decode(const uint8_t *data, size_t length, uint64_t sequence) {
    if (length > 32 * 1024 * 1024)
      throw std::runtime_error("H264 access unit too large");
    std::vector<VideoSurface> frames;
    ComPtr<IMFSample> sample;
    check(MediaFoundation::get().CreateSample(&sample), "H264 input sample");
    ComPtr<IMFMediaBuffer> buffer;
    check(MediaFoundation::get().CreateMemoryBuffer(DWORD(length), &buffer), "H264 input buffer");
    BYTE *dest = nullptr;
    check(buffer->Lock(&dest, nullptr, nullptr), "H264 input lock");
    size_t payload = copyH264WithoutFiller(dest, data, length);
    buffer->Unlock();
    if (!payload)
      throw std::runtime_error("H264 access unit contains only padding");
    check(buffer->SetCurrentLength(DWORD(payload)), "H264 input length");
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(LONGLONG(sequence) * 10000);
    sample->SetSampleDuration(166667);
    HRESULT input = transform->ProcessInput(0, sample.Get(), 0);
    if (input == MF_E_NOTACCEPTING) {
      drain(frames);
      input = transform->ProcessInput(0, sample.Get(), 0);
    }
    check(input, "H264 ProcessInput");
    drain(frames);
    return frames;
  }
  ~H264Decoder() {
    if (transform)
      transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  }
};
