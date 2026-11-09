#pragma once
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>

// Keep this object on the initialized COM decode thread. WIC writes directly to
// the final upload buffer: no GDI+ bitmap, DrawImage, or second pixel conversion.
class ImageDecoder {
  Microsoft::WRL::ComPtr<IWICImagingFactory> factory;

public:
  ImageDecoder() {
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(&factory));
  }
  bool decode(const uint8_t *bytes, size_t length, UINT width, UINT height,
              bool grayscale, std::vector<uint8_t> &pixels) {
    if (!factory || !length || length > MAXDWORD || !width || !height || width > 8192 ||
        height > 8192 || uint64_t(width) * height > 16777216)
      return false;
    Microsoft::WRL::ComPtr<IWICStream> stream;
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE *>(bytes), DWORD(length))) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), &GUID_VendorMicrosoft,
                                                WICDecodeMetadataCacheOnDemand, &decoder)))
      return false;
    GUID container{};
    if (FAILED(decoder->GetContainerFormat(&container)) || container != GUID_ContainerFormatPng ||
        FAILED(decoder->GetFrame(0, &frame)))
      return false;
    UINT actualWidth = 0, actualHeight = 0;
    if (FAILED(frame->GetSize(&actualWidth, &actualHeight)) || actualWidth != width ||
        actualHeight != height)
      return false;
    UINT stride = width * (grayscale ? 1 : 4);
    pixels.resize(size_t(stride) * height);
    GUID format{};
    if (FAILED(frame->GetPixelFormat(&format)))
      return false;
    const GUID &wanted = grayscale ? GUID_WICPixelFormat8bppGray : GUID_WICPixelFormat32bppPBGRA;
    if (format == wanted)
      return SUCCEEDED(frame->CopyPixels(nullptr, stride, UINT(pixels.size()), pixels.data()));
    Microsoft::WRL::ComPtr<IWICFormatConverter> converted;
    return SUCCEEDED(factory->CreateFormatConverter(&converted)) &&
           SUCCEEDED(converted->Initialize(frame.Get(), wanted, WICBitmapDitherTypeNone, nullptr,
                                            0, WICBitmapPaletteTypeCustom)) &&
           SUCCEEDED(converted->CopyPixels(nullptr, stride, UINT(pixels.size()), pixels.data()));
  }
};
