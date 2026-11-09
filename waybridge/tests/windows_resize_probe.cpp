#include "presentation.h"
#include "h264_decoder.h"
#include "h264_decoder_cache.h"
#include "third_party/json.hpp"
#include <chrono>
#include <fstream>
#include <iomanip>
#include <thread>
#include <string>

using Clock = std::chrono::steady_clock;
static double ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }
int main(int argc, char **argv) {
  if (argc != 4) { std::cerr << "Usage: resize-probe H264 JSON recreate|reuse|cache\n"; return 2; }
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  auto &mf = MediaFoundation::get();
  if (!mf.available() || FAILED(mf.Startup(MF_VERSION, MFSTARTUP_FULL))) return 3;
  int result = 0;
  try {
    std::ifstream metadata(argv[2]), data(argv[1], std::ios::binary);
    nlohmann::json manifest; metadata >> manifest;
    WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"WaybridgeResizeProbe"; RegisterClassW(&wc);
    HWND window = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, L"Waybridge resize probe",
                                  WS_OVERLAPPEDWINDOW, 80, 80, 1000, 700, nullptr, nullptr, wc.hInstance, nullptr);
    if (!window) throw std::runtime_error("window creation failed");
    {
      Presentation presentation;
      if (!presentation.initialize(window)) throw std::runtime_error("presentation initialization failed");
      std::unique_ptr<H264Decoder> decoder;
      H264DecoderCache cache;
      VideoSurface previous;
      UINT previousWidth = 0, previousHeight = 0;
      std::cout << "sequence,width,height,changed,decoder_ms,decode_ms,present_ms,total_ms,pixel_hash\n";
      double totalSwitch = 0; unsigned switches = 0;
      for (const auto &row : manifest.at("frames")) {
        UINT width = row.at("width"), height = row.at("height");
        uint64_t sequence = row.at("sequence"); size_t length = row.at("length");
        std::vector<uint8_t> bytes(length);
        data.seekg(row.at("offset").get<std::streamoff>());
        if (!data.read(reinterpret_cast<char *>(bytes.data()), std::streamsize(length))) throw std::runtime_error("fixture truncated");
        bool changed = width != previousWidth || height != previousHeight;
        auto started = Clock::now();
        if (changed && decoder && std::string(argv[3]) == "cache") {
          cache.recycle(std::move(decoder));
          bool reused = false; decoder = cache.acquire(width, height, reused);
        } else if (!decoder || (changed && std::string(argv[3]) == "recreate"))
          decoder = std::make_unique<H264Decoder>(width, height);
        auto initialized = Clock::now();
        auto frames = decoder->decode(bytes.data(), bytes.size(), sequence);
        auto decoded = Clock::now();
        if (frames.size() != 1 || frames[0].sequence != sequence) throw std::runtime_error("output/sequence mismatch");
        D3D11_TEXTURE2D_DESC desc{}; frames[0].texture->GetDesc(&desc);
        if (desc.Width < width || desc.Height < height) throw std::runtime_error("output texture too small");
        RECT rect{0, 0, 960, 640};
        if (!presentation.presentVideo(frames[0].texture.Get(), frames[0].subresource, width, height, {}, rect))
          throw std::runtime_error("presentation failed");
        auto presented = Clock::now();
        auto gpu = GraphicsDevice::get();
        D3D11_TEXTURE2D_DESC staging = desc;
        staging.ArraySize = staging.MipLevels = 1; staging.BindFlags = staging.MiscFlags = 0;
        staging.Usage = D3D11_USAGE_STAGING; staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> readback;
        if (FAILED(gpu->device->CreateTexture2D(&staging, nullptr, &readback))) throw std::runtime_error("staging allocation failed");
        gpu->context->CopySubresourceRegion(readback.Get(), 0, 0, 0, 0, frames[0].texture.Get(), frames[0].subresource, nullptr);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(gpu->context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped))) throw std::runtime_error("readback map failed");
        uint64_t hash = 14695981039346656037ULL;
        const auto *pixels = static_cast<const uint8_t *>(mapped.pData);
        for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x) { hash ^= pixels[size_t(y)*mapped.RowPitch+x]; hash *= 1099511628211ULL; }
        const auto *uv = pixels + size_t(mapped.RowPitch)*desc.Height;
        for (UINT y = 0; y < height/2; ++y) for (UINT x = 0; x < width; ++x) { hash ^= uv[size_t(y)*mapped.RowPitch+x]; hash *= 1099511628211ULL; }
        gpu->context->Unmap(readback.Get(), 0);
        double total = ms(presented-started);
        if (changed) { totalSwitch += total; ++switches; }
        std::cout << sequence << ',' << width << ',' << height << ',' << changed << ','
                  << ms(initialized-started) << ',' << ms(decoded-initialized) << ','
                  << ms(presented-decoded) << ',' << total << ',' << hash << '\n';
        previous = std::move(frames[0]); previousWidth = width; previousHeight = height;
        MSG message; while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        std::this_thread::sleep_for(std::chrono::milliseconds(17));
      }
      std::cout << "SUMMARY switches=" << switches << " avg_switch_ms=" << totalSwitch/switches << '\n';
    }
    DestroyWindow(window);
  } catch (const std::exception &e) { std::cerr << "FAIL: " << e.what() << '\n'; result = 1; }
  mf.Shutdown(); CoUninitialize(); return result;
}
