#pragma once
// A negotiated ordered connection for file bytes. Only the upload worker
// writes file data here, and only the file receiver can wait for disk capacity.

class FileChannel {
  using Clock = std::chrono::steady_clock;
  using Receive = std::function<void(Json, size_t)>;
  std::mutex state, writing;
  std::condition_variable wake;
  std::thread worker;
  SOCKET socket = INVALID_SOCKET;
  bool ready = false, closed = false, offered = false;
  uint64_t rate = 0;
  Clock::time_point next{};
  static constexpr size_t PacketLimit = 2 * 1024 * 1024;

  static SOCKET connectTo(const std::string &host, const std::string &port) {
    addrinfo hints{}, *addresses = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &addresses) != 0)
      return INVALID_SOCKET;
    SOCKET result = INVALID_SOCKET;
    for (auto *address = addresses; address && running; address = address->ai_next) {
      auto candidate = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
      if (candidate == INVALID_SOCKET) continue;
      u_long nonblocking = 1;
      ioctlsocket(candidate, FIONBIO, &nonblocking);
      int connected = connect(candidate, address->ai_addr, int(address->ai_addrlen));
      if (connected && WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set writes, errors;
        FD_ZERO(&writes);
        FD_ZERO(&errors);
        FD_SET(candidate, &writes);
        FD_SET(candidate, &errors);
        timeval timeout{3, 0};
        connected = select(0, nullptr, &writes, &errors, &timeout) > 0 && FD_ISSET(candidate, &writes) ? 0 : -1;
      }
      nonblocking = 0;
      ioctlsocket(candidate, FIONBIO, &nonblocking);
      if (!connected) {
        result = candidate;
        break;
      }
      closesocket(candidate);
    }
    freeaddrinfo(addresses);
    return result;
  }

  static bool sendPacket(SOCKET fd, const std::string &text) {
    if (text.size() >= PacketLimit) return false;
    uint32_t size = htonl(static_cast<uint32_t>(text.size() + 1));
    std::vector<uint8_t> packet(5 + text.size());
    memcpy(packet.data(), &size, 4);
    packet[4] = 0;
    memcpy(packet.data() + 5, text.data(), text.size());
    size_t sent = 0;
    while (running && sent < packet.size()) {
      int count = NativeTransport::write(fd, packet.data() + sent, std::min<size_t>(65536, packet.size() - sent));
      if (count <= 0) return false;
      sent += size_t(count);
    }
    return sent == packet.size();
  }

  static bool readPacket(SOCKET fd, Json &message, size_t &size) {
    uint32_t header;
    if (!readAll(fd, &header, sizeof(header))) return false;
    size = ntohl(header);
    if (size <= 1 || size > PacketLimit) return false;
    std::vector<uint8_t> packet(size);
    if (!readAll(fd, packet.data(), packet.size()) || packet[0] != 0) return false;
    message = Json::parse(packet.begin() + 1, packet.end());
    return message.is_object();
  }

  void close() {
    std::lock_guard<std::mutex> guard(state);
    ready = false;
    closed = true;
    if (socket != INVALID_SOCKET) shutdown(socket, SD_BOTH);
    wake.notify_all();
  }

public:
  void start(const std::string &host, const std::string &port, const std::string &token,
             const std::string &key, Receive receive, std::function<void()> lost) {
    {
      std::lock_guard<std::mutex> guard(state);
      if (offered) return;
      offered = true;
    }
    worker = std::thread([this, host, port, token, key, receive = std::move(receive), lost = std::move(lost)] {
      SOCKET fd = connectTo(host, port);
      try {
        if (fd != INVALID_SOCKET) {
          {
            std::lock_guard<std::mutex> guard(state);
            if (closed || !running) throw std::runtime_error("File channel stopped");
            socket = fd;
          }
          DWORD timeout = 5000;
          setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
          NativeTransport::attach(fd, host);
          setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
          Json hello = {{"type", "hello"}, {"version", 3}, {"channel", "files"},
                        {"token", token}, {"media_key", key}};
          Json message;
          size_t bytes;
          if (sendPacket(fd, hello.dump()) && readPacket(fd, message, bytes) && message.value("type", "") == "files_ready") {
            timeout = 0;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
            {
              std::lock_guard<std::mutex> guard(state);
              if (!closed) ready = true;
              wake.notify_all();
            }
            while (readPacket(fd, message, bytes))
              receive(std::move(message), bytes);
          }
        }
      } catch (const std::exception &error) {
        if (running) std::cerr << "File channel: " << error.what() << '\n';
      }
      close();
      if (fd != INVALID_SOCKET) {
        std::lock_guard<std::mutex> writer(writing);
        {
          std::lock_guard<std::mutex> guard(state);
          socket = INVALID_SOCKET;
        }
        NativeTransport::detach(fd);
        closesocket(fd);
      }
      if (running) lost();
    });
  }
  void setRate(uint64_t kib) {
    std::lock_guard<std::mutex> guard(state);
    auto updated = std::min<uint64_t>(kib, 1048576) * 1024;
    if (updated != rate) {
      rate = updated;
      next = {};
      wake.notify_all();
    }
  }
  bool negotiated() {
    std::lock_guard<std::mutex> guard(state);
    return offered;
  }
  bool send(const Json &message, const std::function<bool()> &valid) {
    auto text = message.dump();
    std::lock_guard<std::mutex> writer(writing);
    SOCKET fd;
    {
      std::unique_lock<std::mutex> lock(state);
      const auto deadline = Clock::now() + std::chrono::seconds(15);
      while (!ready && !closed && running && valid() && Clock::now() < deadline)
        wake.wait_for(lock, std::chrono::milliseconds(100));
      if (!ready || closed || !running || !valid()) return false;
      if (auto data = message.find("data"); data != message.end() && data->is_string()) {
        auto now = Clock::now();
        while (rate && now < next && !closed && running && valid()) {
          wake.wait_for(lock, std::min(next - now, Clock::duration(std::chrono::milliseconds(100))));
          now = Clock::now();
        }
        if (closed || !running || !valid()) return false;
        if (rate) next = now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>((text.size() + 5) / double(rate)));
      }
      fd = socket;
    }
    try {
      if (sendPacket(fd, text)) return true;
    } catch (const std::exception &error) {
      std::cerr << "File upload: " << error.what() << '\n';
    }
    close();
    return false;
  }
  void stop() {
    close();
    if (worker.joinable()) worker.join();
  }
};
