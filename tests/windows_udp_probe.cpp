// Real Windows CNG + UdpMedia receiver, with decoded access units checked by the Linux harness.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "third_party/json.hpp"
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
std::atomic<bool> running{true};
HWND controller = nullptr;
uint32_t u32(const uint8_t *p) {
  uint32_t v;
  memcpy(&v, p, 4);
  return ntohl(v);
}
std::string utf8(const std::wstring &w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(n, 0);
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}
std::vector<uint8_t> unbase64(const std::string &s) {
  DWORD n = 0;
  if (!CryptStringToBinaryA(s.data(), DWORD(s.size()), CRYPT_STRING_BASE64, nullptr, &n, nullptr,
                            nullptr))
    return {};
  std::vector<uint8_t> v(n);
  if (!CryptStringToBinaryA(s.data(), DWORD(s.size()), CRYPT_STRING_BASE64, v.data(), &n, nullptr,
                            nullptr))
    return {};
  v.resize(n);
  return v;
}
#include "udp_media.h"
std::mutex sendMutex, outputMutex;
SOCKET control = INVALID_SOCKET;
bool all(SOCKET socket, void *data, size_t n, bool write = false) {
  auto p = static_cast<char *>(data);
  while (n) {
    int k = write ? send(socket, p, int(n), 0) : recv(socket, p, int(n), 0);
    if (k <= 0)
      return false;
    p += k;
    n -= size_t(k);
  }
  return true;
}
void sendPacket(SOCKET socket, uint8_t kind, const std::string &s) {
  uint32_t n = htonl(uint32_t(s.size() + 1));
  if (!all(socket, &n, 4, true) || !all(socket, &kind, 1, true) ||
      !all(socket, const_cast<char *>(s.data()), s.size(), true))
    running = false;
}
void sendJson(const Json &j) {
  std::lock_guard<std::mutex> g(sendMutex);
  sendPacket(control, 0, j.dump());
}
std::vector<uint8_t> readPacket(SOCKET socket) {
  uint32_t n = 0;
  if (!all(socket, &n, 4))
    return {};
  n = ntohl(n);
  if (!n || n > 72 * 1024 * 1024)
    return {};
  std::vector<uint8_t> p(n);
  if (!all(socket, p.data(), n))
    return {};
  return p;
}
SOCKET connectTo(const char *port) {
  addrinfo hints{};
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *a = nullptr;
  if (getaddrinfo("127.0.0.1", port, &hints, &a))
    return INVALID_SOCKET;
  SOCKET s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
  if (connect(s, a->ai_addr, int(a->ai_addrlen))) {
    closesocket(s);
    s = INVALID_SOCKET;
  } else {
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&one), sizeof(one));
  }
  freeaddrinfo(a);
  return s;
}
int wmain(int argc, wchar_t **argv) {
  if (argc != 5)
    return 2;
  WSADATA data{};
  WSAStartup(MAKEWORD(2, 2), &data);
  auto port = utf8(argv[1]), token = utf8(argv[2]), path = utf8(argv[3]);
  bool hdr = getenv("WAYBRIDGE_UDP_TEST_HDR") != nullptr;
  bool perWindow = getenv("WAYBRIDGE_UDP_TEST_WINDOWS") != nullptr;
  bool reedSolomon = perWindow && ReedSolomonAvailable && getenv("WAYBRIDGE_UDP_TEST_RS");
  bool refresh = getenv("WAYBRIDGE_UDP_TEST_NO_REFRESH") == nullptr;
  int target = std::stoi(argv[4]);
  control = connectTo(port.c_str());
  if (control == INVALID_SOCKET)
    return 3;
  sendJson({{"type", "hello"},
            {"version", 3},
            {"token", token},
            {"split_media", true},
            {"udp_fec_v1", true},
            {"udp_window_v2", perWindow},
            {"udp_rs_v1", reedSolomon},
            {"frame_ack", true},
            {"hdr", hdr},
            {"color_spaces", Json::array({"bt2020_pq", "bt709_sdr"})},
            {"codecs", Json::array({"png_rgba", "h264_alpha", "hevc_alpha", "av1_alpha",
                                    "hevc_main10_pq_alpha16", "av1_main10_pq_alpha16"})}});
  auto p = readPacket(control);
  if (p.empty() || p[0])
    return 4;
  auto hello = Json::parse(p.begin() + 1, p.end());
  std::ofstream output(path, std::ios::binary);
  std::atomic<int> count{0};
  auto frame = [&](uint8_t kind, std::vector<uint8_t> body) {
    std::lock_guard<std::mutex> g(outputMutex);
    uint32_t n = htonl(uint32_t(body.size() + 1));
    output.write(reinterpret_cast<char *>(&n), 4);
    output.put(char(kind));
    output.write(reinterpret_cast<char *>(body.data()), body.size());
    output.flush();
    uint64_t seq = (uint64_t(u32(body.data() + 12)) << 32) | u32(body.data() + 16);
    sendJson({{"type", "frame_ack"}, {"id", u32(body.data())}, {"sequence", seq}});
    if (++count < target && refresh)
      sendJson({{"type", "refresh"}});
  };
  UdpMedia udp;
  udp.start(hello.at("udp_fec_v1"), frame, sendJson);
  if (perWindow)
    Sleep(50);
  SOCKET media = connectTo(port.c_str());
  sendPacket(media, 0,
             Json{{"type", "hello"},
                  {"version", 3},
                  {"token", token},
                  {"channel", "media"},
                  {"media_key", hello.at("media_key")}}
                 .dump());
  std::thread controlReader([&] {
    while (running) {
      auto packet = readPacket(control);
      if (packet.empty())
        break;
      if (packet[0] != 0)
        continue;
      auto j = Json::parse(packet.begin() + 1, packet.end());
      if (hdr && j.value("type", "") == "window")
        sendJson({{"type", "color_mode"}, {"id", j.at("id")}, {"hdr", true}});
    }
  });
  std::thread reader([&] {
    try {
      while (running) {
        auto packet = readPacket(media);
        if (packet.empty())
          break;
        if (packet[0] == 0) {
          auto j = Json::parse(packet.begin() + 1, packet.end());
          if (j.value("type", "") == "udp_start")
            udp.activate();
        } else if (packet[0] == 6)
          udp.repair(packet.data() + 1, packet.size() - 1);
        else {
          uint8_t kind = packet[0];
          packet.erase(packet.begin());
          frame(kind, std::move(packet));
        }
      }
    } catch (const std::exception &e) {
      std::cerr << e.what() << '\n';
    }
    running = false;
  });
  auto deadline = Clock::now() + std::chrono::seconds(35);
  while (running && count < target && Clock::now() < deadline)
    Sleep(20);
  running = false;
  shutdown(control, SD_BOTH);
  shutdown(media, SD_BOTH);
  udp.stop();
  reader.join();
  controlReader.join();
  closesocket(media);
  closesocket(control);
  WSACleanup();
  std::cout << "received=" << count << '\n';
  return count >= target ? 0 : 5;
}
