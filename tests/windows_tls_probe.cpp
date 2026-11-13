// Real Windows Schannel with pinned certificate and pipe-only gateway credentials.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iostream>
#include "../client/third_party/json.hpp"
#include "../client/native_transport.h"

static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
int main(int argc, char **argv) {
  WSADATA data{};
  if (argc != 2 || WSAStartup(MAKEWORD(2, 2), &data))
    return 1;
  SOCKET socket = INVALID_SOCKET;
  try {
    std::string line;
    std::getline(std::cin, line);
    NativeTransport::configuration =
        NativeTransport::Configuration::parse(nlohmann::json::parse(line));
    for (int channel = 0; channel < 3; ++channel) {
      socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
      require(socket != INVALID_SOCKET, "socket creation failed");
      sockaddr_in address{};
      address.sin_family = AF_INET;
      address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      address.sin_port = htons(static_cast<unsigned short>(std::stoi(argv[1])));
      require(connect(socket, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0,
              "connect failed");
      DWORD timeout = 5000;
      setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char *>(&timeout),
                 sizeof(timeout));
      NativeTransport::attach(socket, "localhost");
      std::vector<unsigned char> greeting(17);
      size_t received = 0;
      while (received < greeting.size()) {
        int n =
            NativeTransport::read(socket, greeting.data() + received, greeting.size() - received);
        require(n > 0, "encrypted greeting failed");
        received += size_t(n);
      }
      require(std::string(greeting.begin(), greeting.end()) == "opaque-v3-ready!!",
              "gateway remainder lost");
      std::vector<unsigned char> payload(262144);
      for (size_t i = 0; i < payload.size(); ++i)
        payload[i] = static_cast<unsigned char>(i + channel);
      size_t sent = 0;
      while (sent < payload.size()) {
        int n = NativeTransport::write(socket, payload.data() + sent, payload.size() - sent);
        require(n > 0, "encrypted payload write failed");
        sent += size_t(n);
      }
      std::vector<unsigned char> echo(payload.size());
      received = 0;
      while (received < echo.size()) {
        int n = NativeTransport::read(socket, echo.data() + received,
                                      std::min(size_t(7919), echo.size() - received));
        require(n > 0, "encrypted echo failed");
        received += size_t(n);
      }
      require(echo == payload, "TLS stream bytes changed");
      NativeTransport::detach(socket);
      closesocket(socket);
      socket = INVALID_SOCKET;
    }
    std::cout << "PASS: three independently authenticated TLS streams, pinned certificate, "
                 "fragmented records and opaque binary framing\n";
    WSACleanup();
    return 0;
  } catch (const std::exception &error) {
    if (socket != INVALID_SOCKET) {
      NativeTransport::detach(socket);
      closesocket(socket);
    }
    std::cerr << error.what() << '\n';
    WSACleanup();
    return 2;
  }
}
