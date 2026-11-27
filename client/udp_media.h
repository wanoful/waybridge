#pragma once
// Datagram authentication is Windows CNG AES-GCM; reassembly never drops dependent access units.
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <functional>
#include <stdexcept>
#include "udp_fec.h"

class UdpMedia {
  static constexpr size_t Chunk = 1024, Group = 8, RsGroup = 16, Budget = 96 * 1024 * 1024;
  static bool videoKind(uint8_t kind) {
    return kind == 2 || kind == 4 || kind == 5 || kind == 7 || kind == 8;
  }

  struct Route {
    uint32_t window = 0;
    uint64_t predecessor = 0;
    bool independent = false;
    bool operator==(const Route &other) const {
      return window == other.window && predecessor == other.predecessor &&
             independent == other.independent;
    }
  };
  struct Frame {
    uint8_t kind = 0;
    size_t total = 0, count = 0, have = 0, reserved = 0;
    std::vector<uint8_t> body, seen;
    std::map<size_t, std::array<uint8_t, Chunk>> parity;
    Route route;
    Clock::time_point firstSeen = Clock::now();
    Clock::time_point lastProgress = firstSeen;
  };
  struct Window {
    uint64_t delivered = 0, refresh = 0;
    Clock::time_point lastRetry{};
  };
  std::mutex mutex;
  std::map<uint64_t, Frame> frames;
  std::map<uint32_t, Window> windows;
  std::set<uint32_t> retiredWindows;
  std::set<uint64_t> replay;
  size_t reserved = 0;
  uint64_t session = 0, next = 1, counter = 0, highest = 0;
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_KEY_HANDLE decryptKey = nullptr, encryptKey = nullptr;
  SOCKET socket = INVALID_SOCKET;
  std::thread worker;
  std::atomic<bool> live{false};
  bool active = false;
  bool perWindow = false;
  bool reedSolomon = false;
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
  void eraseFrame(std::map<uint64_t, Frame>::iterator it) {
    reserved -= it->second.reserved;
    frames.erase(it);
  }
  bool acceptRoute(uint64_t message, const Route &route) {
    if (!message || !route.window || route.predecessor >= message || retiredWindows.count(route.window))
      return false;
    if (windows.size() >= 128 && !windows.count(route.window))
      return false;
    auto &window = windows[route.window];
    return message > window.delivered && message >= window.refresh;
  }
  void discardEarlier(uint32_t id, uint64_t message) {
    for (auto it = frames.begin(); it != frames.end() && it->first < message;) {
      if (it->second.route.window == id)
        eraseFrame(it++);
      else
        ++it;
    }
  }
  bool reserveFrame(size_t cost, uint64_t message, const Route &route) {
    auto full = [&] { return frames.size() >= 16 || reserved + cost > Budget; };
    if (perWindow && full()) {
      const auto cutoff = Clock::now() - std::chrono::milliseconds(250);
      for (auto it = frames.begin(); full() && it != frames.end();) {
        if (it->second.lastProgress < cutoff)
          eraseFrame(it++);
        else
          ++it;
      }
      if (full() && route.independent) {
        windows.at(route.window).refresh = message;
        discardEarlier(route.window, message);
      }
    }
    return !full();
  }
  void drainWindow(uint32_t id, Window &window) {
    while (active) {
      auto it = std::find_if(frames.begin(), frames.end(), [&](const auto &entry) {
        const auto &f = entry.second;
        return f.route.window == id && f.have == f.count &&
               (f.route.independent || f.route.predecessor == window.delivered);
      });
      if (it == frames.end())
        return;
      const auto message = it->first;
      auto kind = it->second.kind;
      auto body = std::move(it->second.body);
      bool independent = kind == 1 || (body.size() >= 60 && u32(body.data() + 48) == 1);
      const bool valid = body.size() >= (kind == 1 ? 36u : 60u) && u32(body.data()) == id &&
                         independent == it->second.route.independent;
      eraseFrame(it);
      if (!valid)
        return;
      if (!logged) {
        std::cout << "UDP per-window media active\n" << std::flush;
        logged = true;
      }
      deliver(kind, std::move(body));
      window.delivered = message;
      discardEarlier(id, message);
      feedback(Json{{"type", "udp_ack"}, {"message", message}});
    }
  }
  void drain() {
    if (perWindow) {
      for (auto &[id, window] : windows)
        drainWindow(id, window);
      return;
    }
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
    size_t header = perWindow ? 32 : 20;
    if (p.size() < header || (p[0] != 1 && p[0] != 2) ||
        (p[6] && !(reedSolomon && p[0] == 2 && p[6] == 1)) || p[7])
      return;
    uint8_t kind = perWindow ? p[1] & 0x7f : p[1];
    if (kind != 1 && !videoKind(kind))
      return;
    size_t index = (size_t(p[2]) << 8) | p[3], count = (size_t(p[4]) << 8) | p[5],
           total = u32(p.data() + 8);
    uint64_t message = read64(p.data() + 12);
    if (!total || total > 8 * 1024 * 1024 || count != (total + Chunk - 1) / Chunk || index >= count)
      return;
    size_t group = reedSolomon ? RsGroup : Group;
    if (p[0] == 2 ? index % group || p.size() != header + Chunk ||
                       (p[6] && count - index <= Group)
                  : p.size() != header + std::min(Chunk, total - index * Chunk))
      return;
    Route route;
    if (perWindow)
      route = {u32(p.data() + 20), read64(p.data() + 24), (p[1] & 0x80) != 0};
    std::lock_guard<std::mutex> guard(mutex);
    if (perWindow ? !acceptRoute(message, route) : message < next || message - next >= 32)
      return;
    auto it = frames.find(message);
    if (it == frames.end()) {
      size_t cost = total + count + ((count + Group - 1) / Group) * Chunk;
      if (!reserveFrame(cost, message, route))
        return; // TCP repair supplies the missing ordered message.
      Frame f;
      f.kind = kind;
      f.route = route;
      f.total = total;
      f.count = count;
      f.body.resize(total);
      f.seen.resize(count);
      f.reserved = cost;
      reserved += cost;
      it = frames.emplace(message, std::move(f)).first;
    }
    auto &f = it->second;
    if (f.kind != kind || f.total != total || f.count != count || !(f.route == route))
      return;
    auto shard = index * 2 + p[6];
    if (p[0] == 1 && !f.seen[index]) {
      memcpy(f.body.data() + index * Chunk, p.data() + header, p.size() - header);
      f.seen[index] = 1;
      ++f.have;
      f.lastProgress = Clock::now();
    } else if (p[0] == 2 && !f.parity.count(shard)) {
      std::array<uint8_t, Chunk> a{};
      memcpy(a.data(), p.data() + header, Chunk);
      f.parity[shard] = a;
      f.lastProgress = Clock::now();
    }
    size_t first = index / group * group, missing = count, absent = 0;
    for (size_t i = first; i < std::min(first + group, count); ++i)
      if (!f.seen[i]) {
        missing = i;
        ++absent;
      }
    auto parity = f.parity.find(first * 2);
    size_t recovered = 0;
    if (reedSolomon) {
      auto second = f.parity.find(first * 2 + 1);
      const uint8_t *p0 = parity == f.parity.end() ? nullptr : parity->second.data();
      const uint8_t *p1 = second == f.parity.end() ? nullptr : second->second.data();
      if (absent && absent <= size_t(p0 != nullptr) + size_t(p1 != nullptr))
        recovered = recoverReedSolomon(f.body.data(), total, f.seen.data(), first, p0, p1);
    } else if (absent == 1 && parity != f.parity.end()) {
      auto restored = parity->second;
      for (size_t i = first; i < std::min(first + group, count); ++i)
        if (i != missing)
          for (size_t j = 0; j < std::min(Chunk, total - i * Chunk); ++j)
            restored[j] ^= f.body[i * Chunk + j];
      memcpy(f.body.data() + missing * Chunk, restored.data(),
             std::min(Chunk, total - missing * Chunk));
      f.seen[missing] = 1;
      recovered = 1;
    }
    f.have += recovered;
    if (recovered && !loggedFec) {
      std::cout << (reedSolomon ? "UDP Reed–Solomon shards recovered\n" : "UDP XOR shard recovered\n")
                << std::flush;
      loggedFec = true;
    }
    if (perWindow)
      drainWindow(route.window, windows.at(route.window));
    else
      drain();
  }
  void requestWindowRepairs(Clock::time_point now) {
    unsigned requested = 0;
    for (const auto &[message, frame] : frames) {
      auto &window = windows.at(frame.route.window);
      if (now - frame.firstSeen < std::chrono::milliseconds(55) ||
          now - window.lastRetry < std::chrono::milliseconds(50))
        continue;
      uint64_t missing = 0;
      if (!frame.route.independent && frame.route.predecessor > window.delivered)
        missing = frame.route.predecessor;
      else if (frame.have != frame.count)
        missing = message;
      if (!missing)
        continue;
      feedback(Json{{"type", "udp_retry"}, {"message", missing}});
      window.lastRetry = now;
      if (++requested == 4)
        return;
    }
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
      if (now - retry >= std::chrono::milliseconds(perWindow ? 25 : 100)) {
        std::lock_guard<std::mutex> guard(mutex);
        if (active) {
          if (perWindow)
            requestWindowRepairs(now);
          else
            feedback(Json{{"type", "udp_retry"}, {"message", next}});
        }
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
      auto fec = offer.value("fec", "xor");
      const bool rs = fec == "rs";
      require((offer.at("version") == 1 || offer.at("version") == 2) && offer.at("chunk") == Chunk &&
              (fec == "xor" || (rs && ReedSolomonAvailable && offer.at("version") == 2)) &&
              offer.at("group") == (rs ? RsGroup : Group));
      {
        std::lock_guard<std::mutex> guard(mutex);
        perWindow = offer.at("version") == 2;
        reedSolomon = rs;
      }
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
  void forgetWindow(uint32_t id) {
    std::lock_guard<std::mutex> guard(mutex);
    if (!perWindow)
      return;
    windows.erase(id);
    retiredWindows.insert(id);
    if (retiredWindows.size() > 256)
      retiredWindows.erase(retiredWindows.begin());
    for (auto it = frames.begin(); it != frames.end();) {
      if (it->second.route.window == id)
        eraseFrame(it++);
      else
        ++it;
    }
  }
  void repair(const uint8_t *p, size_t size) {
    std::lock_guard<std::mutex> guard(mutex);
    size_t header = perWindow ? 21 : 9;
    require(size >= header);
    uint64_t message = read64(p);
    uint8_t kind = perWindow ? p[8] & 0x7f : p[8];
    size_t total = size - header;
    require((kind == 1 && total >= 36) || (videoKind(kind) && total >= 60));
    if (perWindow) {
      Route route{u32(p + 9), read64(p + 13), (p[8] & 0x80) != 0};
      if (!acceptRoute(message, route))
        return;
      auto old = frames.find(message);
      if (old != frames.end())
        eraseFrame(old);
      auto &window = windows.at(route.window);
      if (!reserveFrame(total, message, route)) {
        // A complete repair that can be consumed immediately needs no
        // reassembly credit.
        if (active && (route.independent || route.predecessor == window.delivered)) {
          require(u32(p + header) == route.window &&
                  route.independent == (kind == 1 || u32(p + header + 48) == 1));
          deliver(kind, std::vector<uint8_t>(p + header, p + size));
          window.delivered = message;
          discardEarlier(route.window, message);
          feedback(Json{{"type", "udp_ack"}, {"message", message}});
          drainWindow(route.window, window);
        }
        return;
      }
      Frame f;
      f.kind = kind;
      f.total = total;
      f.count = f.have = 1;
      f.reserved = total;
      f.route = route;
      f.body.assign(p + header, p + size);
      reserved += total;
      frames.emplace(message, std::move(f));
      drainWindow(route.window, window);
      return;
    }
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
