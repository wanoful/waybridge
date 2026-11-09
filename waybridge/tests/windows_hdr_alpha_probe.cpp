#include "image_decoder.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
int main(int argc, char **argv) {
  if (argc != 3)
    return 2;
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  int result = 0;
  try {
    ImageDecoder decoder;
    for (int sample = 0; sample < 2; ++sample) {
      std::ifstream file(argv[sample + 1], std::ios::binary);
      std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {}), pixels;
      bool okay = decoder.decode(bytes.data(), bytes.size(), 1024, 64, true, pixels, true);
      if (sample) {
        if (okay)
          throw std::runtime_error("8-bit alpha silently accepted as raw FP16");
        continue;
      }
      if (!okay || pixels.size() != 1024 * 64 * 2)
        throw std::runtime_error("raw 16-bit PNG decode failed");
      for (size_t i = 0; i < 1024 * 64; ++i) {
        uint16_t value = uint16_t(pixels[i * 2]) | (uint16_t(pixels[i * 2 + 1]) << 8);
        if (value != 0x3000 + i % 1024)
          throw std::runtime_error("FP16 alpha bits changed by WIC");
      }
      if (decoder.decode(bytes.data(), bytes.size(), 1023, 64, true, pixels, true))
        throw std::runtime_error("wrong alpha dimensions accepted");
    }
    std::cout << "PASS: actual WIC preserves all 1024 raw half-alpha values and rejects "
                 "8-bit/wrong dimensions\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    result = 1;
  }
  CoUninitialize();
  return result;
}
