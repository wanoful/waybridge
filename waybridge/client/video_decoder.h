#pragma once
#include "graphics.h"
#include "media_foundation.h"
#include "h264_bitstream.h"
#include "video_capability_samples.h"
#include "hdr_capability_samples.h"
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
class VideoDecoder {
  std::shared_ptr<GraphicsDevice> gpu;
  ComPtr<IMFTransform> transform;
  ComPtr<IMFDXGIDeviceManager> manager;
  UINT width, height;
  uint8_t kind;
  void check(HRESULT hr, const char *where) {
    if (FAILED(hr))
      throw std::runtime_error(std::string(where) + " failed: " + std::to_string(unsigned(hr)));
  }
  void outputType() {
    for (DWORD i = 0;; ++i) {
      ComPtr<IMFMediaType> type;
      check(transform->GetOutputAvailableType(0, i, &type), "Video output types");
      GUID subtype{};
      type->GetGUID(MF_MT_SUBTYPE, &subtype);
      if (subtype == (hdr(kind) ? MFVideoFormat_P010 : MFVideoFormat_NV12)) {
        check(transform->SetOutputType(0, type.Get(), 0), "Video texture output");
        return;
      }
    }
  }
  void drain(std::vector<VideoSurface> &frames) {
    for (unsigned i = 0; i < 8; ++i) {
      MFT_OUTPUT_STREAM_INFO info{};
      check(transform->GetOutputStreamInfo(0, &info), "Video output info");
      if (!(info.dwFlags &
            (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)))
        throw std::runtime_error("Video decoder did not provide GPU samples");
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
      check(hr, "Video ProcessOutput");
      if (!sample)
        continue;
      ComPtr<IMFMediaBuffer> buffer;
      check(sample->GetBufferByIndex(0, &buffer), "Video sample buffer");
      ComPtr<IMFDXGIBuffer> dxgi;
      check(buffer.As(&dxgi), "Video hardware texture");
      VideoSurface frame;
      frame.sample = sample;
      check(dxgi->GetResource(IID_PPV_ARGS(&frame.texture)), "Video texture resource");
      check(dxgi->GetSubresourceIndex(&frame.subresource), "Video texture slice");
      LONGLONG time = 0;
      check(sample->GetSampleTime(&time), "Video sample sequence");
      frame.sequence = uint64_t(time) / 10000;
      frames.push_back(std::move(frame));
    }
  }

public:
  bool matches(UINT w, UINT h, uint8_t codec = 2) const {
    return width == w && height == h && kind == codec;
  }
  uint64_t pixels() const { return uint64_t(width) * height; }
  void restart() {
    check(transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0), "Video flush epoch");
    check(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "Video restart epoch");
  }
  static const char *name(uint8_t codec) {
    return codec == 2   ? "H.264"
           : codec == 4 ? "HEVC"
           : codec == 5 ? "AV1"
           : codec == 7 ? "HEVC Main10 PQ"
           : codec == 8 ? "AV1 Main10 PQ"
                        : "invalid";
  }
  static bool hdr(uint8_t codec) { return codec == 7 || codec == 8; }
  static bool videoKind(uint8_t codec) {
    return codec == 2 || codec == 4 || codec == 5 || hdr(codec);
  }
  static const char *capability(uint8_t codec) {
    return codec == 2   ? "h264_alpha"
           : codec == 4 ? "hevc_alpha"
           : codec == 5 ? "av1_alpha"
           : codec == 7 ? "hevc_main10_pq_alpha16"
                        : "av1_main10_pq_alpha16";
  }
  static GUID subtype(uint8_t codec) {
    // Standard Media Foundation subtype GUIDs derive from the registered FOURCC.
    if (codec == 2)
      return MFVideoFormat_H264;
    if (codec == 4 || codec == 7)
      return MFVideoFormat_HEVC;
    if (codec == 5 || codec == 8)
      return MFVideoFormat_AV1;
    throw std::runtime_error("Unknown video codec");
  }
  static bool available(uint8_t codec = 2) {
    auto gpu = GraphicsDevice::get();
    if (!gpu || !gpu->hardware || !videoKind(codec))
      return false;
    if (codec == 2) {
      // Preserve established H.264 capability behavior.
      ComPtr<IMFTransform> test;
      if (FAILED(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&test))))
        return false;
      ComPtr<IMFAttributes> attributes;
      UINT aware = 0;
      return SUCCEEDED(test->GetAttributes(&attributes)) &&
             SUCCEEDED(attributes->GetUINT32(MF_SA_D3D11_AWARE, &aware)) && aware;
    }
    // Do not advertise an installed software decoder or merely an awareness flag.
    // A real access unit must yield a correctly timestamped NV12/P010 GPU texture.
    try {
      VideoDecoder test(256, 256, codec);
      auto output = codec == 4 ? test.decode(HevcCapabilitySample, sizeof(HevcCapabilitySample), 1)
                    : codec == 5 ? test.decode(Av1CapabilitySample, sizeof(Av1CapabilitySample), 1)
                    : codec == 7
                        ? test.decode(HevcHdrCapabilitySample, sizeof(HevcHdrCapabilitySample), 1)
                        : test.decode(Av1HdrCapabilitySample, sizeof(Av1HdrCapabilitySample), 1);
      if (output.size() != 1 || output[0].sequence != 1 || !output[0].texture)
        return false;
      D3D11_TEXTURE2D_DESC desc{};
      output[0].texture->GetDesc(&desc);
      return desc.Format == (hdr(codec) ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12) &&
             desc.Width >= 256 && desc.Height >= 256;
    } catch (...) {
      return false;
    }
  }
  VideoDecoder(UINT w, UINT h, uint8_t codec = 2)
      : gpu(GraphicsDevice::get()), width(w), height(h), kind(codec) {
    if (!gpu || !gpu->hardware)
      throw std::runtime_error("D3D11 hardware device unavailable");
    if (kind == 2) {
      check(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                             IID_PPV_ARGS(&transform)),
            "Video decoder");
      initialize();
      return;
    }
    auto enumerate = MediaFoundation::get().EnumTransforms;
    if (!videoKind(kind) || !enumerate)
      throw std::runtime_error("Video decoder enumeration unavailable");
    MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, subtype(kind)};
    MFT_REGISTER_TYPE_INFO output{MFMediaType_Video,
                                  hdr(kind) ? MFVideoFormat_P010 : MFVideoFormat_NV12};
    IMFActivate **found = nullptr;
    UINT count = 0;
    check(enumerate(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                    &input, &output, &found, &count),
          "MFTEnumEx video decoder");
    std::vector<ComPtr<IMFActivate>> candidates;
    for (UINT i = 0; i < count; ++i) {
      ComPtr<IMFActivate> activation;
      activation.Attach(found[i]);
      candidates.push_back(activation);
    }
    CoTaskMemFree(found);
    for (auto &candidate : candidates) {
      try {
        check(candidate->ActivateObject(IID_PPV_ARGS(&transform)), "Activate video decoder");
        initialize();
        candidate->DetachObject();
        return;
      } catch (...) {
        candidate->ShutdownObject();
        transform.Reset();
        manager.Reset();
      }
    }
    throw std::runtime_error("No synchronous D3D11 texture decoder for selected codec");
  }

private:
  void initialize() {
    ComPtr<IMFAttributes> attributes;
    check(transform->GetAttributes(&attributes), "Video attributes");
    UINT aware = 0;
    check(attributes->GetUINT32(MF_SA_D3D11_AWARE, &aware), "Video D3D11 awareness");
    if (!aware)
      throw std::runtime_error("Video decoder is not D3D11 aware");
    UINT asynchronous = 0;
    attributes->GetUINT32(MF_TRANSFORM_ASYNC, &asynchronous);
    if (asynchronous)
      throw std::runtime_error("Asynchronous MFT requires a different processing contract");
    check(attributes->SetUINT32(MF_LOW_LATENCY, TRUE), "Video low latency attribute");
    ComPtr<ICodecAPI> codec;
    if (SUCCEEDED(transform->QueryInterface(IID_ICodecAPI,
                                            reinterpret_cast<void **>(codec.GetAddressOf())))) {
      VARIANT value;
      VariantInit(&value);
      // The Microsoft H.264 decoder requires VT_UI4, unlike other codecs.
      // VT_BOOL fails with E_INVALIDARG.
      value.vt = VT_UI4;
      value.ulVal = 1;
      HRESULT latency = codec->SetValue(&CODECAPI_AVLowLatencyMode, &value);
      if (kind == 2)
        check(latency, "H264 low latency mode");
      else if (FAILED(latency)) {
        value.vt = VT_BOOL;
        value.boolVal = VARIANT_TRUE;
        codec->SetValue(&CODECAPI_AVLowLatencyMode, &value);
      }
    }
    UINT token = 0;
    check(MediaFoundation::get().CreateDXGIDeviceManager(&token, &manager), "DXGI manager");
    check(manager->ResetDevice(gpu->device.Get(), token), "DXGI decoder device");
    check(transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                    reinterpret_cast<ULONG_PTR>(manager.Get())),
          "Video D3D manager");
    ComPtr<IMFMediaType> input;
    check(MediaFoundation::get().CreateMediaType(&input), "Video input type");
    input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    input->SetGUID(MF_MT_SUBTYPE, subtype(kind));
    if (kind == 4 || kind == 7)
      input->SetUINT32(MF_MT_MPEG2_PROFILE,
                       kind == 7 ? 2 : 1); // HEVC Main=1 / Main10=2 profile_idc.
    if (hdr(kind)) {
      input->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT2020);
      input->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_2084);
      input->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT2020_10);
      input->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    }
    MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(input.Get(), MF_MT_FRAME_RATE, 60, 1);
    MFSetAttributeRatio(input.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    check(transform->SetInputType(0, input.Get(), 0), "Video input");
    outputType();
    check(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0),
          "Video begin streaming");
    check(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "Video start stream");
  }

public:
  std::vector<VideoSurface> decode(const uint8_t *data, size_t length, uint64_t sequence) {
    if (length > 32 * 1024 * 1024)
      throw std::runtime_error("Video access unit too large");
    std::vector<VideoSurface> frames;
    ComPtr<IMFSample> sample;
    check(MediaFoundation::get().CreateSample(&sample), "Video input sample");
    ComPtr<IMFMediaBuffer> buffer;
    check(MediaFoundation::get().CreateMemoryBuffer(DWORD(length), &buffer), "Video input buffer");
    BYTE *dest = nullptr;
    check(buffer->Lock(&dest, nullptr, nullptr), "Video input lock");
    size_t payload = length;
    if (kind == 2)
      payload = copyH264WithoutFiller(dest, data, length);
    else
      memcpy(dest, data, length); // HEVC Annex-B and AV1 OBUs are distinct formats.
    buffer->Unlock();
    if (!payload)
      throw std::runtime_error("Video access unit contains only padding");
    check(buffer->SetCurrentLength(DWORD(payload)), "Video input length");
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(LONGLONG(sequence) * 10000);
    sample->SetSampleDuration(166667);
    HRESULT input = transform->ProcessInput(0, sample.Get(), 0);
    if (input == MF_E_NOTACCEPTING) {
      drain(frames);
      input = transform->ProcessInput(0, sample.Get(), 0);
    }
    check(input, "Video ProcessInput");
    drain(frames);
    return frames;
  }
  ~VideoDecoder() {
    if (transform)
      transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  }
};
