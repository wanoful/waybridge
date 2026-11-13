#include <windows.h>
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>
#include "image_decoder.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>

// Reproduce the former streaming path for a same-process, same-image comparison.
static bool legacy(const std::vector<uint8_t> &bytes, bool gray, UINT w, UINT h,
                    std::vector<uint8_t> &pixels) {
  HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
  void *ptr = GlobalLock(mem);
  if (!ptr) { GlobalFree(mem); return false; }
  memcpy(ptr, bytes.data(), bytes.size()); GlobalUnlock(mem);
  Microsoft::WRL::ComPtr<IStream> stream;
  if (FAILED(CreateStreamOnHGlobal(mem, TRUE, &stream))) { GlobalFree(mem); return false; }
  std::unique_ptr<Gdiplus::Bitmap> source(Gdiplus::Bitmap::FromStream(stream.Get()));
  if (!source || source->GetLastStatus() != Gdiplus::Ok) return false;
  Gdiplus::Bitmap bitmap(w, h, PixelFormat32bppARGB);
  { Gdiplus::Graphics g(&bitmap); g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    if (g.DrawImage(source.get(), 0, 0) != Gdiplus::Ok) return false; }
  Gdiplus::Rect rect(0, 0, w, h); Gdiplus::BitmapData data{};
  if (bitmap.LockBits(&rect, Gdiplus::ImageLockModeRead,
                      gray ? PixelFormat32bppARGB : PixelFormat32bppPARGB, &data) != Gdiplus::Ok)
    return false;
  pixels.resize(size_t(w) * h * (gray ? 1 : 4));
  for (UINT y = 0; y < h; ++y) {
    auto row = static_cast<const uint8_t *>(data.Scan0) + ptrdiff_t(y) * data.Stride;
    if (!gray) memcpy(pixels.data() + size_t(y) * w * 4, row, size_t(w) * 4);
    else for (UINT x = 0; x < w; ++x) pixels[size_t(y) * w + x] = row[x * 4];
  }
  bitmap.UnlockBits(&data); return true;
}

int main(int argc, char **argv) {
  if (argc != 5) { std::cerr << "Usage: image-probe RGBA_PNG GRAY_PNG WIDTH HEIGHT\n"; return 2; }
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  Gdiplus::GdiplusStartupInput startup; ULONG_PTR token = 0;
  Gdiplus::GdiplusStartup(&token, &startup, nullptr);
  int result = 0;
  try {
    ImageDecoder images;
    UINT w = UINT(std::stoul(argv[3])), h = UINT(std::stoul(argv[4]));
    for (bool gray : {false, true}) {
      std::ifstream file(argv[gray ? 2 : 1], std::ios::binary);
      std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {}), pixels;
      if (!images.decode(bytes.data(), bytes.size(), w, h, gray, pixels))
        throw std::runtime_error("WIC decode failed");
      // Fixture uses a gray ramp, or RGBA=(233,127,47,x mod 256).
      for (UINT y = 0; y < h; ++y) for (UINT x = 0; x < w; ++x) {
        auto i = size_t(y) * w + x;
        if (gray) {
          if (pixels[i] != uint8_t(x)) throw std::runtime_error("gray/alpha fidelity mismatch");
        } else {
          unsigned a = x % 256;
          if (pixels[4*i+3] != a) throw std::runtime_error("RGBA alpha mismatch");
          for (unsigned c = 0; c < 3; ++c) {
            unsigned expected = (c == 0 ? 47 : c == 1 ? 127 : 233) * a / 255;
            if (std::abs(int(pixels[4*i+c]) - int(expected)) > 1)
              throw std::runtime_error("premultiplied color mismatch");
          }
        }
      }
      if (images.decode(bytes.data(), bytes.size(), w+1, h, gray, pixels) ||
          images.decode(bytes.data(), 8, w, h, gray, pixels))
        throw std::runtime_error("invalid PNG/dimensions accepted");
      for (bool old : {true, false}) {
        double total = 0, maximum = 0;
        for (int i = 0; i < 40; ++i) {
          auto started = std::chrono::steady_clock::now();
          bool ok = old ? legacy(bytes, gray, w, h, pixels)
                        : images.decode(bytes.data(), bytes.size(), w, h, gray, pixels);
          if (!ok) throw std::runtime_error("benchmark decode failed");
          double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-started).count();
          if (i >= 5) { total += elapsed; maximum = std::max(maximum, elapsed); }
        }
        std::cout << (gray ? "alpha" : "rgba") << (old ? " GDI+" : " WIC")
                  << " samples=35 avg_ms=" << total/35 << " max_ms=" << maximum << '\n';
      }
    }
    std::cout << "PASS: exact 8-bit alpha, premultiplied BGRA, invalid image/dimensions rejected\n";
  } catch (const std::exception &e) { std::cerr << "FAIL: " << e.what() << '\n'; result = 1; }
  Gdiplus::GdiplusShutdown(token); CoUninitialize(); return result;
}
