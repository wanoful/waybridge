#include "h264_decoder.h"
#include <gdiplus.h>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <thread>

using Clock = std::chrono::steady_clock;
static double ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

int main(int argc, char **argv) {
  if (argc != 5) { std::cerr << "Usage: decode-probe H264 SIZES WIDTH HEIGHT\n"; return 2; }
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  auto &mf = MediaFoundation::get();
  if (!mf.available() || FAILED(mf.Startup(MF_VERSION, MFSTARTUP_FULL))) return 3;
  int result = 0;
  try {
    ComPtr<IMFTransform> test;
    HRESULT hr = CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&test));
    if (FAILED(hr)) throw std::runtime_error("decoder creation failed");
    ComPtr<ICodecAPI> codec;
    test->QueryInterface(IID_ICodecAPI, reinterpret_cast<void **>(codec.GetAddressOf()));
    if (codec) {
      for (auto type : {VT_BOOL, VT_UI4}) {
        VARIANT value{}; value.vt = VARTYPE(type);
        if (type == VT_BOOL) value.boolVal = VARIANT_TRUE; else value.ulVal = 1;
        hr = codec->SetValue(&CODECAPI_AVLowLatencyMode, &value);
        std::cout << "Set low latency type=" << type << " hr=0x" << std::hex << unsigned(hr) << std::dec << '\n';
        VARIANT actual{};
        hr = codec->GetValue(&CODECAPI_AVLowLatencyMode, &actual);
        std::cout << "Read low latency hr=0x" << std::hex << unsigned(hr) << std::dec << " type=" << actual.vt << " value=" << actual.ulVal << '\n';
        VariantClear(&actual);
      }
    }
    std::ifstream data(argv[1], std::ios::binary), sizes(argv[2]);
    if (!data || !sizes) throw std::runtime_error("fixture missing");
    H264Decoder decoder(unsigned(std::stoul(argv[3])), unsigned(std::stoul(argv[4])));
    size_t length; uint64_t submitted = 0, outputs = 0; double total = 0, peak = 0;
    while (sizes >> length) {
      std::vector<uint8_t> bytes(length);
      if (!data.read(reinterpret_cast<char *>(bytes.data()), std::streamsize(length))) throw std::runtime_error("truncated fixture");
      auto start = Clock::now();
      auto frames = decoder.decode(bytes.data(), bytes.size(), ++submitted);
      double elapsed = ms(Clock::now() - start);
      total += elapsed; peak = std::max(peak, elapsed);
      outputs += frames.size();
      if (submitted <= 6 || frames.empty()) std::cout << "input=" << submitted << " outputs=" << frames.size() << " time_ms=" << elapsed << '\n';
      for (auto &f : frames) {
        if (!f.texture || f.sequence != outputs - frames.size() + (&f - frames.data()) + 1) throw std::runtime_error("wrong output sequence/texture");
      }
      if (frames.size() != 1) throw std::runtime_error("each complete access unit must output immediately");
      std::this_thread::sleep_for(std::chrono::milliseconds(17));
    }
    if (!submitted || data.peek() != std::char_traits<char>::eof()) throw std::runtime_error("fixture sizes do not cover the complete bitstream");
    std::cout << "SUMMARY submitted=" << submitted << " outputs=" << outputs << " avg_ms=" << total/submitted << " max_ms=" << peak << '\n';
    if (outputs != submitted) result = 1;
  } catch (const std::exception &e) { std::cerr << "FAIL: " << e.what() << '\n'; result = 1; }
  mf.Shutdown(); CoUninitialize(); return result;
}
