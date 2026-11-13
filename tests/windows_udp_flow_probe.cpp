// Reuse the real socket/CNG receiver harness; this file adds its reorder-capacity regression.
#define wmain original_udp_probe_main
#include "windows_udp_probe.cpp"
#undef wmain

static int headRepair(unsigned port) {
  WSADATA data{};
  if (WSAStartup(MAKEWORD(2, 2), &data))
    return 2;
  SetEnvironmentVariableW(L"WAYBRIDGE_UDP_HOST", L"127.0.0.1");
  SetEnvironmentVariableW(L"WAYBRIDGE_UDP_PORT", nullptr);
  int result = 0;
  {
    UdpMedia udp;
    std::vector<uint64_t> order;
    try {
      const std::string key = "AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE=";
      udp.start(
          {{"version", 1},
           {"session", "77"},
           {"port", port},
           {"chunk", 1024},
           {"group", 8},
           {"tx_key", key},
           {"rx_key", key}},
          [&](uint8_t kind, std::vector<uint8_t> body) {
            if (kind != 1 || body.size() < 36)
              throw std::runtime_error("invalid delivered PNG body");
            order.push_back((uint64_t(u32(body.data() + 12)) << 32) | u32(body.data() + 16));
          },
          [](const Json &) {});
      udp.activate();
      auto png = unbase64("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/"
                          "iZk9HQAAAABJRU5ErkJggg==");
      auto repair = [&](uint64_t message) {
        std::vector<uint8_t> packet(9 + 36);
        for (int i = 0; i < 8; ++i)
          packet[i] = uint8_t(message >> (56 - 8 * i));
        packet[8] = 1;
        packet[9 + 3] = packet[9 + 7] = packet[9 + 11] = 1; // ID and physical 1x1.
        packet[9 + 19] = uint8_t(message);
        packet[9 + 23] = packet[9 + 27] = 1; // Logical 1x1, zero origin.
        packet.insert(packet.end(), png.begin(), png.end());
        udp.repair(packet.data(), packet.size());
      };
      for (uint64_t message = 2; message <= 17; ++message)
        repair(message);
      if (!order.empty())
        throw std::runtime_error("future messages escaped the ordering barrier");
      repair(1);
      if (order.size() != 17)
        throw std::runtime_error("missing repaired messages");
      for (size_t i = 0; i < order.size(); ++i)
        if (order[i] != i + 1)
          throw std::runtime_error("repair delivery order changed");
      std::cout
          << "PASS: head repair bypasses 16 future slots; delivered messages 1..17 in order\n";
    } catch (const std::exception &error) {
      std::cerr << error.what() << '\n';
      result = 7;
    }
    running = false;
    udp.stop();
  }
  WSACleanup();
  return result;
}

int wmain(int argc, wchar_t **argv) {
  if (argc == 3 && std::wstring(argv[1]) == L"head")
    return headRepair(unsigned(std::stoul(argv[2])));
  return original_udp_probe_main(argc, argv);
}
