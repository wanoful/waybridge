#pragma once
// Datagram authentication is Windows CNG AES-GCM; reassembly never drops dependent access units.
#include <bcrypt.h>
#include <array>
#include <functional>
#include <stdexcept>

class UdpMedia {
  static constexpr size_t Chunk = 1024, Group = 8, Budget = 96 * 1024 * 1024;
  struct Frame {
    uint8_t kind = 0;
    size_t total = 0, count = 0, have = 0, reserved = 0;
    std::vector<uint8_t> body, seen;
    std::map<size_t, std::array<uint8_t, Chunk>> parity;
  };
  std::mutex mutex;
  std::map<uint64_t, Frame> frames;
  std::set<uint64_t> replay;
  size_t reserved = 0;
  uint64_t session = 0, next = 1, counter = 0, highest = 0;
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_KEY_HANDLE decryptKey = nullptr, encryptKey = nullptr;
  SOCKET socket = INVALID_SOCKET;
  std::thread worker;
  std::atomic<bool> live{false};
  bool active = false;
  bool logged = false, loggedFec = false, loggedRepair = false;
  std::function<void(uint8_t, std::vector<uint8_t>)> deliver;
  std::function<void(const Json &)> feedback;
  static uint64_t read64(const uint8_t *p) { return (uint64_t(u32(p)) << 32) | u32(p + 4); }
  static void append64(std::vector<uint8_t> &p, uint64_t v) {
    for (int i = 7; i >= 0; --i)
      p.push_back(uint8_t(v >> (i * 8)));
  }
  static void require(bool ok) {
    if (!ok)
      throw std::runtime_error("invalid/bounded UDP media state");
  }
  bool crypt(bool encrypt, uint64_t number, const uint8_t *aad, const uint8_t *data, size_t count,
             std::vector<uint8_t> &out) {
    if (!encrypt && count < 16)
      return false;
    std::array<uint8_t, 12> nonce{};
    for (int i = 0; i < 8; ++i)
      nonce[4 + i] = uint8_t(number >> (56 - 8 * i));
    std::array<uint8_t, 16> tag{};
    size_t bytes = encrypt ? count : count - 16;
    if (!encrypt)
      memcpy(tag.data(), data + bytes, 16);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = nonce.data();
    info.cbNonce = ULONG(nonce.size());
    info.pbAuthData = const_cast<PUCHAR>(aad);
    info.cbAuthData = 20;
    info.pbTag = tag.data();
    info.cbTag = ULONG(tag.size());
    out.resize(bytes + 16);
    ULONG written = 0;
    NTSTATUS result = encrypt
                          ? BCryptEncrypt(encryptKey, const_cast<PUCHAR>(data), ULONG(bytes), &info,
                                          nullptr, 0, out.data(), ULONG(bytes), &written, 0)
                          : BCryptDecrypt(decryptKey, const_cast<PUCHAR>(data), ULONG(bytes), &info,
                                          nullptr, 0, out.data(), ULONG(bytes), &written, 0);
    if (result < 0 || written != bytes)
      return false;
    out.resize(bytes);
    if (encrypt)
      out.insert(out.end(), tag.begin(), tag.end());
    return true;
  }
  void hello() {
    if (counter == UINT64_MAX)
      return;
    std::vector<uint8_t> packet{'W', 'B', 'U', '1'}, body;
    append64(packet, session);
    append64(packet, ++counter);
    uint8_t zero = 0;
    if (crypt(true, counter, packet.data(), &zero, 1, body)) {
      packet.insert(packet.end(), body.begin(), body.end());
      ::send(socket, reinterpret_cast<const char *>(packet.data()), int(packet.size()), 0);
    }
  }
  void drain() {
    while (active) {
      auto it = frames.find(next);
      if (it == frames.end() || it->second.have != it->second.count)
        return;
      auto kind = it->second.kind;
      auto body = std::move(it->second.body);
      reserved -= it->second.reserved;
      frames.erase(it);
      if (!logged) {
        std::cout << "UDP ordered media active\n" << std::flush;
        logged = true;
      }
      deliver(kind, std::move(body));
      feedback(Json{{"type", "udp_ack"}, {"message", next}});
      require(++next != 0);
    }
  }
  void fragment(const std::vector<uint8_t> &p) {
    if (p.size() < 20 || (p[0] != 1 && p[0] != 2) || (p[1] != 1 && p[1] != 2) || p[6] || p[7])
      return;
    size_t index = (size_t(p[2]) << 8) | p[3], count = (size_t(p[4]) << 8) | p[5],
           total = u32(p.data() + 8);
    uint64_t message = read64(p.data() + 12);
    if (!total || total > 8 * 1024 * 1024 || count != (total + Chunk - 1) / Chunk || index >= count)
      return;
    if (p[0] == 2 ? index % Group || p.size() != 20 + Chunk
                  : p.size() != 20 + std::min(Chunk, total - index * Chunk))
      return;
    std::lock_guard<std::mutex> guard(mutex);
    if (message < next || message - next >= 32)
      return;
    auto it = frames.find(message);
    if (it == frames.end()) {
      size_t cost = total + count + ((count + Group - 1) / Group) * Chunk;
      if (frames.size() >= 16 || reserved + cost > Budget)
        return; // TCP repair supplies the missing ordered message.
      Frame f;
      f.kind = p[1];
      f.total = total;
      f.count = count;
      f.body.resize(total);
      f.seen.resize(count);
      f.reserved = cost;
      reserved += cost;
      it = frames.emplace(message, std::move(f)).first;
    }
    auto &f = it->second;
    if (f.kind != p[1] || f.total != total || f.count != count)
      return;
    if (p[0] == 1 && !f.seen[index]) {
      memcpy(f.body.data() + index * Chunk, p.data() + 20, p.size() - 20);
      f.seen[index] = 1;
      ++f.have;
    } else if (p[0] == 2 && !f.parity.count(index)) {
      std::array<uint8_t, Chunk> a{};
      memcpy(a.data(), p.data() + 20, Chunk);
      f.parity[index] = a;
    }
    size_t first = index / Group * Group, missing = count, absent = 0;
    for (size_t i = first; i < std::min(first + Group, count); ++i)
      if (!f.seen[i]) {
        missing = i;
        ++absent;
      }
    auto parity = f.parity.find(first);
    if (absent == 1 && parity != f.parity.end()) {
      auto restored = parity->second;
      for (size_t i = first; i < std::min(first + Group, count); ++i)
        if (i != missing)
          for (size_t j = 0; j < std::min(Chunk, total - i * Chunk); ++j)
            restored[j] ^= f.body[i * Chunk + j];
      memcpy(f.body.data() + missing * Chunk, restored.data(),
             std::min(Chunk, total - missing * Chunk));
      f.seen[missing] = 1;
      ++f.have;
      if (!loggedFec) {
        std::cout << "UDP XOR shard recovered\n" << std::flush;
        loggedFec = true;
      }
    }
    drain();
  }
  void receive() {
    auto keep = Clock::now() - std::chrono::seconds(2), retry = Clock::now();
    hello();
    while (live && running) {
      fd_set ready;
      FD_ZERO(&ready);
      FD_SET(socket, &ready);
      timeval wait{0, 20000};
      int result = select(0, &ready, nullptr, nullptr, &wait);
      if (result > 0) {
        std::array<uint8_t, 1201> packet{};
        int size = recv(socket, reinterpret_cast<char *>(packet.data()), int(packet.size()), 0);
        if (size >= 36 && size <= 1200 && memcmp(packet.data(), "WBU1", 4) == 0 &&
            read64(packet.data() + 4) == session) {
          uint64_t number = read64(packet.data() + 12);
          if (number && (highest < 4096 || number > highest - 4096) && !replay.count(number)) {
            std::vector<uint8_t> plain;
            if (crypt(false, number, packet.data(), packet.data() + 20, size_t(size) - 20, plain)) {
              replay.insert(number);
              highest = std::max(highest, number);
              while (!replay.empty() && highest - *replay.begin() >= 4096)
                replay.erase(replay.begin());
              fragment(plain);
            }
          }
        }
      } else if (result < 0)
        Sleep(20);
      auto now = Clock::now();
      if (now - keep >= std::chrono::seconds(1)) {
        hello();
        keep = now;
      }
      if (now - retry >= std::chrono::milliseconds(100)) {
        std::lock_guard<std::mutex> guard(mutex);
        if (active)
          feedback(Json{{"type", "udp_retry"}, {"message", next}});
        retry = now;
      }
    }
  }

public:
  static bool requested() {
    return GetEnvironmentVariableW(L"WAYBRIDGE_UDP_HOST", nullptr, 0) != 0;
  }
  void start(const Json &offer, std::function<void(uint8_t, std::vector<uint8_t>)> output,
             std::function<void(const Json &)> send) {
    if (worker.joinable() || !requested() || !offer.is_object())
      return;
    try {
      require(offer.at("version") == 1 && offer.at("chunk") == Chunk && offer.at("group") == Group);
      session = std::stoull(offer.at("session").get<std::string>());
      auto tx = unbase64(offer.at("rx_key")), rx = unbase64(offer.at("tx_key"));
      require(tx.size() == 32 && rx.size() == 32);
      require(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0) >= 0);
      require(
          BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE,
                            reinterpret_cast<PUCHAR>(const_cast<wchar_t *>(BCRYPT_CHAIN_MODE_GCM)),
                            sizeof(BCRYPT_CHAIN_MODE_GCM), 0) >= 0);
      require(BCryptGenerateSymmetricKey(algorithm, &encryptKey, nullptr, 0, tx.data(),
                                         ULONG(tx.size()), 0) >= 0);
      require(BCryptGenerateSymmetricKey(algorithm, &decryptKey, nullptr, 0, rx.data(),
                                         ULONG(rx.size()), 0) >= 0);
      wchar_t host[256]{}, port[16]{};
      require(GetEnvironmentVariableW(L"WAYBRIDGE_UDP_HOST", host, 256) > 0);
      auto portText = std::to_string(offer.at("port").get<unsigned>());
      if (GetEnvironmentVariableW(L"WAYBRIDGE_UDP_PORT", port, 16))
        portText = utf8(port);
      addrinfo hints{};
      hints.ai_socktype = SOCK_DGRAM;
      addrinfo *addresses = nullptr;
      require(getaddrinfo(utf8(host).c_str(), portText.c_str(), &hints, &addresses) == 0);
      for (auto a = addresses; a; a = a->ai_next) {
        socket = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (socket == INVALID_SOCKET)
          continue;
        if (connect(socket, a->ai_addr, int(a->ai_addrlen)) == 0)
          break;
        closesocket(socket);
        socket = INVALID_SOCKET;
      }
      freeaddrinfo(addresses);
      require(socket != INVALID_SOCKET);
      deliver = std::move(output);
      feedback = std::move(send);
      live = true;
      worker = std::thread([this] {
        try {
          receive();
        } catch (const std::exception &e) {
          std::cerr << "UDP receive: " << e.what() << '\n';
          PostMessageW(controller, WM_CLOSE, 0, 0);
        }
      });
      std::cout << "Encrypted UDP/FEC endpoint configured; TCP remains available\n" << std::flush;
    } catch (const std::exception &e) {
      std::cerr << "UDP unavailable, using TCP: " << e.what() << '\n';
    }
  }
  void activate() {
    std::lock_guard<std::mutex> guard(mutex);
    active = true;
    drain();
  }
  void repair(const uint8_t *p, size_t size) {
    require(size >= 9);
    uint64_t message = read64(p);
    uint8_t kind = p[8];
    size_t total = size - 9;
    require((kind == 1 && total >= 36) || (kind == 2 && total >= 60));
    std::lock_guard<std::mutex> guard(mutex);
    if (message < next)
      return;
    require(message - next < 32);
    auto old = frames.find(message);
    if (old != frames.end()) {
      reserved -= old->second.reserved;
      frames.erase(old);
    }
    if (message == next && active) {
      deliver(kind, std::vector<uint8_t>(p + 9, p + size));
      feedback(Json{{"type", "udp_ack"}, {"message", next}});
      require(++next != 0);
      drain();
      if (!loggedRepair) {
        std::cout << "UDP frame repaired over TCP\n" << std::flush;
        loggedRepair = true;
      }
      return;
    }
    require(frames.size() < 16 && reserved + total <= Budget);
    Frame f;
    f.kind = kind;
    f.total = total;
    f.count = f.have = 1;
    f.reserved = total;
    f.body.assign(p + 9, p + size);
    reserved += total;
    frames.emplace(message, std::move(f));
    drain();
    if (!loggedRepair) {
      std::cout << "UDP frame repaired over TCP\n" << std::flush;
      loggedRepair = true;
    }
  }
  void stop() {
    live = false;
    if (worker.joinable())
      worker.join();
    if (socket != INVALID_SOCKET) {
      closesocket(socket);
      socket = INVALID_SOCKET;
    }
  }
  ~UdpMedia() {
    stop();
    if (encryptKey)
      BCryptDestroyKey(encryptKey);
    if (decryptKey)
      BCryptDestroyKey(decryptKey);
    if (algorithm)
      BCryptCloseAlgorithmProvider(algorithm, 0);
  }
};
