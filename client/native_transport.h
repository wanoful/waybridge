#pragma once
// Raw legacy TCP or a pinned Schannel TLS stream. One reader and one writer
// may operate concurrently; SSPI state is never shared between sockets.
#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif
#include <security.h>
#include <schannel.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace NativeTransport {
using Json = nlohmann::json;
struct Configuration {
  bool tls = false;
  std::array<unsigned char, 32> fingerprint{};
  std::string clientId, clientToken, session;
  static Configuration parse(const Json &value) {
    Configuration result;
    if (value.is_null())
      return result;
    if (!value.is_object() || value.value("type", "") != "tls")
      throw std::runtime_error("Unsupported connection transport");
    auto pin = value.at("fingerprint").get<std::string>();
    result.clientId = value.at("client_id").get<std::string>();
    result.clientToken = value.at("client_token").get<std::string>();
    result.session = value.at("session").get<std::string>();
    if (pin.size() != 64 || result.clientId.empty() || result.clientId.size() > 256 ||
        result.clientToken.size() < 16 || result.clientToken.size() > 4096 ||
        result.session.empty() || result.session.size() > 128)
      throw std::runtime_error("Invalid TLS connection configuration");
    auto digit = [](char c) -> unsigned {
      if (c >= '0' && c <= '9')
        return unsigned(c - '0');
      if (c >= 'a' && c <= 'f')
        return unsigned(c - 'a' + 10);
      throw std::runtime_error("Invalid TLS certificate fingerprint");
    };
    for (size_t i = 0; i < 32; ++i)
      result.fingerprint[i] =
          static_cast<unsigned char>((digit(pin[2 * i]) << 4) | digit(pin[2 * i + 1]));
    result.tls = true;
    return result;
  }
};
inline Configuration configuration;

class Channel {
  SOCKET socket;
  CredHandle credentials{};
  CtxtHandle context{};
  SecPkgContext_StreamSizes sizes{};
  std::vector<unsigned char> encrypted, plaintext;
  size_t plainOffset = 0;
  using Clock = std::chrono::steady_clock;
  Clock::time_point deadline = Clock::now() + std::chrono::seconds(10);
  bool initializing = true;

  void checkDeadline() {
    if (initializing && Clock::now() >= deadline)
      throw std::runtime_error("TLS connection handshake timed out");
  }
  bool rawWrite(const void *data, size_t count) {
    auto p = static_cast<const char *>(data);
    while (count) {
      checkDeadline();
      int written = ::send(socket, p, int(std::min(count, size_t(65536))), 0);
      if (written <= 0)
        return false;
      p += written;
      count -= size_t(written);
    }
    return true;
  }
  bool receiveCipher(size_t limit) {
    checkDeadline();
    if (encrypted.size() >= limit)
      throw std::runtime_error("TLS receive buffer limit exceeded");
    if (initializing) {
      auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
      DWORD timeout = DWORD(std::max<int64_t>(1, remaining));
      setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout),
                 sizeof(timeout));
    }
    unsigned char buffer[16384];
    int count = ::recv(socket, reinterpret_cast<char *>(buffer),
                       int(std::min(sizeof(buffer), limit - encrypted.size())), 0);
    if (count <= 0)
      return false;
    encrypted.insert(encrypted.end(), buffer, buffer + count);
    return true;
  }
  void preserveExtra(const SecBuffer *buffers, size_t count) {
    size_t extra = 0;
    for (size_t i = 0; i < count; ++i)
      if (buffers[i].BufferType == SECBUFFER_EXTRA)
        extra = buffers[i].cbBuffer;
    if (extra > encrypted.size())
      throw std::runtime_error("Invalid TLS record remainder");
    if (extra)
      encrypted.erase(encrypted.begin(), encrypted.end() - extra);
    else
      encrypted.clear();
  }

public:
  explicit Channel(SOCKET value) : socket(value) {
    SecInvalidateHandle(&credentials);
    SecInvalidateHandle(&context);
  }
  Channel(const Channel &) = delete;
  ~Channel() {
    if (SecIsValidHandle(&context))
      DeleteSecurityContext(&context);
    if (SecIsValidHandle(&credentials))
      FreeCredentialsHandle(&credentials);
  }
  void connect(const std::string &host, const Configuration &config) {
    SCHANNEL_CRED options{};
    options.dwVersion = SCHANNEL_CRED_VERSION;
    options.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT;
    // Trust comes exclusively from the user-approved DER fingerprint. Never
    // select a local client certificate or ambient Windows credentials.
    options.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS;
    TimeStamp expiry{};
    if (AcquireCredentialsHandleW(nullptr, const_cast<wchar_t *>(UNISP_NAME_W),
                                  SECPKG_CRED_OUTBOUND, nullptr, &options, nullptr, nullptr,
                                  &credentials, &expiry) != SEC_E_OK)
      throw std::runtime_error("Cannot initialize Windows TLS credentials");
    int characters = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, host.data(),
                                         int(host.size()), nullptr, 0);
    if (!characters)
      throw std::runtime_error("Invalid TLS server hostname");
    std::wstring target(size_t(characters), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, host.data(), int(host.size()), target.data(),
                        characters);
    constexpr DWORD flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT |
                            ISC_REQ_CONFIDENTIALITY | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM |
                            ISC_REQ_MANUAL_CRED_VALIDATION;
    bool first = true;
    for (;;) {
      checkDeadline();
      SecBuffer input[2]{{ULONG(encrypted.size()), SECBUFFER_TOKEN, encrypted.data()},
                         {0, SECBUFFER_EMPTY, nullptr}};
      SecBufferDesc in{SECBUFFER_VERSION, 2, input};
      SecBuffer token{0, SECBUFFER_TOKEN, nullptr};
      SecBufferDesc out{SECBUFFER_VERSION, 1, &token};
      DWORD attributes = 0;
      SECURITY_STATUS status = InitializeSecurityContextW(
          &credentials, first ? nullptr : &context, target.data(), flags, 0, SECURITY_NATIVE_DREP,
          first ? nullptr : &in, 0, &context, &out, &attributes, &expiry);
      if (token.pvBuffer) {
        bool sent = !token.cbBuffer || rawWrite(token.pvBuffer, token.cbBuffer);
        FreeContextBuffer(token.pvBuffer);
        if (!sent)
          throw std::runtime_error("TLS handshake write failed");
      }
      first = false;
      if (status == SEC_E_INCOMPLETE_MESSAGE) {
        if (!receiveCipher(1024 * 1024))
          throw std::runtime_error("TLS handshake read failed");
        continue;
      }
      if (status != SEC_E_OK && status != SEC_I_CONTINUE_NEEDED)
        throw std::runtime_error("TLS handshake was rejected");
      preserveExtra(input, 2);
      if (status == SEC_E_OK)
        break;
      if (encrypted.empty() && !receiveCipher(1024 * 1024))
        throw std::runtime_error("TLS handshake read failed");
    }
    PCCERT_CONTEXT certificate = nullptr;
    if (QueryContextAttributesW(&context, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &certificate) !=
            SEC_E_OK ||
        !certificate)
      throw std::runtime_error("TLS server did not provide a certificate");
    std::array<unsigned char, 32> digest{};
    DWORD count = DWORD(digest.size());
    bool hashed =
        CryptHashCertificate2(BCRYPT_SHA256_ALGORITHM, 0, nullptr, certificate->pbCertEncoded,
                              certificate->cbCertEncoded, digest.data(), &count) != FALSE;
    CertFreeCertificateContext(certificate);
    unsigned difference = 0;
    for (size_t i = 0; i < digest.size(); ++i)
      difference |= digest[i] ^ config.fingerprint[i];
    if (!hashed || count != digest.size() || difference)
      throw std::runtime_error("TLS certificate fingerprint mismatch");
    if (QueryContextAttributesW(&context, SECPKG_ATTR_STREAM_SIZES, &sizes) != SEC_E_OK ||
        !sizes.cbMaximumMessage || sizes.cbMaximumMessage > 65536 || sizes.cbHeader > 65536 ||
        sizes.cbTrailer > 65536)
      throw std::runtime_error("Invalid TLS stream sizes");
    auto request = Json({{"id", 1},
                         {"op", "stream"},
                         {"client_id", config.clientId},
                         {"client_token", config.clientToken},
                         {"session", config.session}})
                       .dump() +
                   "\n";
    if (!writeAll(request.data(), request.size()))
      throw std::runtime_error("TLS gateway authorization write failed");
    std::string line;
    for (;;) {
      checkDeadline();
      char byte = 0;
      if (read(&byte, 1) != 1)
        throw std::runtime_error("TLS gateway authorization read failed");
      if (byte == '\n')
        break;
      if (line.size() >= 65536)
        throw std::runtime_error("TLS gateway reply exceeded limit");
      line += byte;
    }
    auto response = Json::parse(line, nullptr, false);
    if (!response.is_object() || response.value("id", 0) != 1 || !response.value("ok", false) ||
        !response.contains("result") || !response["result"].is_object() ||
        !response["result"].value("stream_ready", false) ||
        response["result"].value("version", 0) != 3)
      throw std::runtime_error("TLS gateway authorization rejected");
    initializing = false;
    DWORD timeout = 0;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout),
               sizeof(timeout));
  }
  int write(const void *data, size_t length) {
    size_t count = std::min(length, size_t(sizes.cbMaximumMessage));
    std::vector<unsigned char> record(size_t(sizes.cbHeader) + count + sizes.cbTrailer);
    memcpy(record.data() + sizes.cbHeader, data, count);
    SecBuffer buffers[4]{
        {sizes.cbHeader, SECBUFFER_STREAM_HEADER, record.data()},
        {ULONG(count), SECBUFFER_DATA, record.data() + sizes.cbHeader},
        {sizes.cbTrailer, SECBUFFER_STREAM_TRAILER, record.data() + sizes.cbHeader + count},
        {0, SECBUFFER_EMPTY, nullptr}};
    SecBufferDesc message{SECBUFFER_VERSION, 4, buffers};
    if (EncryptMessage(&context, 0, &message, 0) != SEC_E_OK)
      return SOCKET_ERROR;
    for (size_t i = 0; i < 3; ++i)
      if (buffers[i].cbBuffer && !rawWrite(buffers[i].pvBuffer, buffers[i].cbBuffer))
        return SOCKET_ERROR;
    return int(count);
  }
  bool writeAll(const void *data, size_t count) {
    auto bytes = static_cast<const unsigned char *>(data);
    while (count) {
      int n = write(bytes, count);
      if (n <= 0)
        return false;
      bytes += n;
      count -= size_t(n);
    }
    return true;
  }
  int read(void *data, size_t length) {
    while (plainOffset == plaintext.size()) {
      plaintext.clear();
      plainOffset = 0;
      if (encrypted.empty() && !receiveCipher(256 * 1024))
        return 0;
      SecBuffer buffers[4]{{ULONG(encrypted.size()), SECBUFFER_DATA, encrypted.data()},
                           {0, SECBUFFER_EMPTY, nullptr},
                           {0, SECBUFFER_EMPTY, nullptr},
                           {0, SECBUFFER_EMPTY, nullptr}};
      SecBufferDesc message{SECBUFFER_VERSION, 4, buffers};
      SECURITY_STATUS status = DecryptMessage(&context, &message, 0, nullptr);
      if (status == SEC_E_INCOMPLETE_MESSAGE) {
        if (!receiveCipher(256 * 1024))
          return 0;
        continue;
      }
      if (status == SEC_I_CONTEXT_EXPIRED)
        return 0;
      if (status != SEC_E_OK)
        return SOCKET_ERROR;
      for (const auto &buffer : buffers)
        if (buffer.BufferType == SECBUFFER_DATA && buffer.cbBuffer) {
          auto bytes = static_cast<const unsigned char *>(buffer.pvBuffer);
          plaintext.insert(plaintext.end(), bytes, bytes + buffer.cbBuffer);
        }
      preserveExtra(buffers, 4);
    }
    size_t count = std::min(length, plaintext.size() - plainOffset);
    memcpy(data, plaintext.data() + plainOffset, count);
    plainOffset += count;
    return int(count);
  }
};

inline std::mutex mutex;
inline std::map<SOCKET, std::shared_ptr<Channel>> channels;
inline void attach(SOCKET socket, const std::string &host) {
  if (!configuration.tls)
    return;
  auto channel = std::make_shared<Channel>(socket);
  channel->connect(host, configuration);
  std::lock_guard<std::mutex> lock(mutex);
  channels.emplace(socket, std::move(channel));
}
inline std::shared_ptr<Channel> find(SOCKET socket) {
  std::lock_guard<std::mutex> lock(mutex);
  auto found = channels.find(socket);
  return found == channels.end() ? nullptr : found->second;
}
inline int read(SOCKET socket, void *data, size_t count) {
  try {
    if (auto channel = find(socket))
      return channel->read(data, count);
    return configuration.tls ? SOCKET_ERROR
                             : ::recv(socket, static_cast<char *>(data), int(count), 0);
  } catch (const std::exception &) {
    return SOCKET_ERROR;
  }
}
inline int write(SOCKET socket, const void *data, size_t count) {
  try {
    if (auto channel = find(socket))
      return channel->write(data, count);
    return configuration.tls ? SOCKET_ERROR
                             : ::send(socket, static_cast<const char *>(data), int(count), 0);
  } catch (const std::exception &) {
    return SOCKET_ERROR;
  }
}
inline void detach(SOCKET socket) {
  std::lock_guard<std::mutex> lock(mutex);
  channels.erase(socket);
}
} // namespace NativeTransport
