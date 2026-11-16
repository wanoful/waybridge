// Native Windows viewer for the Waybridge prototype (MIT).
// Windows SDK headers require Winsock and COM declarations before GDI+.
// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>
#include <wincrypt.h>
#include <shellapi.h>
#include <shlobj.h>
// clang-format on

#include "presentation.h"
#include "h264_decoder.h"
#include "h264_decoder_cache.h"
#include "image_decoder.h"
#include "hdr_capabilities.h"
#include "audio.h"
#include "native_cursor.h"
#include "native_icon.h"
#include "application_identity.h"
#include "remote_tray.h"
#include "window_semantics.h"
#include "window_activation.h"
#include "drag_visual.h"
#include "relative_input.h"
#include "ime.h"
#include "third_party/json.hpp"
#include "native_transport.h"
#include "host_control.h"
#include "remote_notifications.h"
#include "ordered_sender.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <chrono>
#include <set>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Json = nlohmann::json;
HostControl hostControl;
NativeActivation nativeActivation;
DragVisual dragVisual;
Json hostedSettings = Json::object();
bool hostedReadOnly = false, hostedIme = true, hostedFiles = true, hostedImages = true, hostedDragText = true;
std::string hostedClipboard = "both";
bool streamConfigNegotiated = false;
std::atomic<uint64_t> networkBytes{0}, presentedFrames{0};
uint64_t streamRequest = 0;
void applyHostedSettings(const Json &settings, bool live);
void sendStreamConfiguration();
void sendJson(const Json &value);
RemoteNotifications remoteNotifications([](Json message) { hostControl.emit(std::move(message)); },
                                      [](Json message) { sendJson(message); });
constexpr size_t MaxPacket = 72 * 1024 * 1024;
constexpr UINT NetworkReady = WM_APP + 1;
SOCKET connection = INVALID_SOCKET;
HWND controller = nullptr;
std::atomic<bool> running{true};
std::mutex inboxMutex;
OrderedSender controlSender;
DWORD uiThread = 0;
std::condition_variable inboxWake, frameWake;
std::deque<Json> controls;
Json pendingCursor;
Json pendingDragVisual;
NativeCursor nativeCursor;
RemoteTray remoteTray;
bool trayNegotiated = false;
bool cursorNegotiated = false;
struct EncodedFrame {
  uint8_t kind = 0;
  std::vector<uint8_t> bytes;
  std::chrono::steady_clock::time_point received;
};
std::map<uint32_t, std::deque<EncodedFrame>> frames;
std::vector<uint32_t> decoderGarbage;
std::atomic<SOCKET> mediaConnection{INVALID_SOCKET};
std::thread mediaReceiver;
std::thread audioReceiver;
std::atomic<SOCKET> audioConnection{INVALID_SOCKET};
AudioPlayback audioPlayback;
std::string serverHost, serverPort, serverToken;
std::set<uint8_t> supportedVideoKinds;
using Clock = std::chrono::steady_clock;
struct ClientStats {
  struct Stage {
    double total = 0, max = 0;
    uint64_t count = 0;
  };
  std::mutex mutex;
  std::map<std::string, Stage> stages;
  bool enabled = GetEnvironmentVariableW(L"WAYBRIDGE_STATS", nullptr, 0) != 0;
  Clock::time_point last = Clock::now();
  void record(const char *name, double ms) {
    if (!enabled)
      return;
    std::lock_guard<std::mutex> guard(mutex);
    auto &s = stages[name];
    s.total += ms;
    s.max = std::max(s.max, ms);
    ++s.count;
  }
  void report() {
    if (!enabled || Clock::now() - last < std::chrono::seconds(2))
      return;
    last = Clock::now();
    std::lock_guard<std::mutex> guard(mutex);
    Json snapshot = Json::object();
    for (auto &[name, s] : stages)
    {
      snapshot[name] = {{"avg_ms", s.total / s.count}, {"max_ms", s.max}, {"samples", s.count}};
      std::cout << "Client timing " << name << " avg_ms=" << s.total / s.count
                << " max_ms=" << s.max << " samples=" << s.count << '\n';
    }
    hostControl.emit({{"type","timings"},{"stages",snapshot}});
    stages.clear();
  }
} clientStats;
double millis(Clock::duration duration) {
  return std::chrono::duration<double, std::milli>(duration).count();
}
struct DecodedFrame {
  int width = 0, height = 0, logicalWidth = 0, logicalHeight = 0, originX = 0, originY = 0;
  uint64_t sequence = 0;
  uint8_t codec = 0;
  std::vector<uint8_t> bgra, alpha;
  VideoSurface video;
  Clock::time_point decoded, received;
};
std::map<uint32_t, std::shared_ptr<DecodedFrame>> decodedFrames;
DWORD ownClipboardSequence = 0;
UINT pngFormat;
std::vector<std::thread> fileWorkers;
std::atomic<uint64_t> nextUpload{GetTickCount64() * 1000000};

struct IncomingFile {
  HANDLE handle = INVALID_HANDLE_VALUE;
  std::wstring path;
  uint64_t remaining = 0;
};
std::map<uint32_t, IncomingFile> incomingFiles;
uint64_t incomingBatch = 0;
uint64_t incomingSelectionEpoch = 0;
DWORD incomingClipboardSequence = 0;
std::wstring incomingDirectory;

struct RemoteWindow {
  // Drop callbacks and virtual-file workers may outlive the HWND. Never keep
  // the RemoteWindow alive merely to finish a transfer to a closed window.
  std::shared_ptr<void> dropLifetime = std::make_shared<char>();
  NativeIcon icon;
  std::wstring taskbarIdentity;
  std::string appId;
  uint64_t iconRevision = 0;
  NativeWindowState state;
  NativeIme ime;
  uint32_t id;
  HWND hwnd = nullptr;
  int width = 960, height = 640;
  std::shared_ptr<DecodedFrame> frame;
  std::unique_ptr<Presentation> presentation;
  UINT dpi = 96;
  int originX = 0, originY = 0;
  bool shown = false;
  bool popup = false;
  uint32_t root = 0, parent = 0;
  int x = 0, y = 0;
  uint64_t lastAck = 0;
  bool tracking = false;
  bool clientDecorated = false;
  int lastResizeWidth = -1, lastResizeHeight = -1;
  bool lastMaximized = false;
  bool sizing = false, resizePending = false;
  Clock::time_point resizeSent;
  UINT lastDpi = 0;
  int lastHdr = -1;
  int lastWorkWidth = 0, lastWorkHeight = 0, lastWorkX = INT_MIN, lastWorkY = INT_MIN;
};
std::map<uint32_t, std::unique_ptr<RemoteWindow>> windows;
Json imeStatus;
void updateIme();
uint32_t grabbedRoot = 0;
std::string remoteApplicationSource;
bool iconsNegotiated = false;
bool dataDragNegotiated = false;
std::unique_ptr<ImageDecoder> iconDecoder;
bool semanticsNegotiated = false;
void reconcileWindows();
RelativeInput relativeInput;
struct WindowDrag {
  bool active = false, released = false, cancelled = false;
  uint32_t source = 0, attached = 0, target = 0;
  int offsetX = 0, offsetY = 0;
} windowDrag;
constexpr UINT_PTR WindowDragTimer = 5;

DWORD windowStyle(bool clientDecorated) {
  // Client-decorated applications already supply their own title bar and controls.
  return clientDecorated ? WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX
                         : WS_OVERLAPPEDWINDOW;
}

void setDecoration(RemoteWindow *w, bool clientDecorated) {
  if (w->state.fullscreen) {
    w->clientDecorated = clientDecorated;
    return;
  }
  if (w->clientDecorated == clientDecorated)
    return;
  RECT client;
  GetClientRect(w->hwnd, &client);
  POINT origin{0, 0};
  ClientToScreen(w->hwnd, &origin);
  w->clientDecorated = clientDecorated;
  DWORD style = windowStyle(clientDecorated);
  SetWindowLongPtrW(w->hwnd, GWL_STYLE, style);
  if (!clientDecorated)
    AdjustWindowRectExForDpi(&client, style, FALSE, WS_EX_APPWINDOW, w->dpi);
  SetWindowPos(w->hwnd, nullptr, origin.x + client.left, origin.y + client.top,
               client.right - client.left, client.bottom - client.top,
               SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOZORDER);
}

void sendResize(RemoteWindow *w);

std::wstring wide(const std::string &s) {
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()),
                              nullptr, 0);
  std::wstring out(n, L'\0');
  if (n)
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()),
                        out.data(), n);
  return out;
}
std::string utf8(const std::wstring &s) {
  int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr,
                              nullptr);
  std::string out(n, '\0');
  if (n)
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr,
                        nullptr);
  return out;
}
std::string base64(const uint8_t *bytes, size_t size) {
  DWORD n = 0;
  CryptBinaryToStringA(bytes, static_cast<DWORD>(size), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                       nullptr, &n);
  std::string out(n, '\0');
  if (!CryptBinaryToStringA(bytes, static_cast<DWORD>(size),
                            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(), &n))
    return {};
  out.resize(n);
  if (!out.empty() && out.back() == '\0')
    out.pop_back();
  return out;
}
std::vector<uint8_t> unbase64(const std::string &text) {
  if (text.size() > 16 * 1024 * 1024)
    return {};
  DWORD n = 0;
  if (!CryptStringToBinaryA(text.data(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64,
                            nullptr, &n, nullptr, nullptr))
    return {};
  std::vector<uint8_t> out(n);
  if (!CryptStringToBinaryA(text.data(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64,
                            out.data(), &n, nullptr, nullptr))
    return {};
  out.resize(n);
  return out;
}

bool sendAll(const void *bytes, size_t n) {
  auto p = static_cast<const char *>(bytes);
  while (n && running) {
    int count = NativeTransport::write(connection, p, std::min(n, size_t(65536)));
    if (count <= 0)
      return false;
    p += count;
    n -= count;
  }
  return n == 0;
}
void sendJson(const Json &value) {
  auto text = value.dump();
  if (text.size() + 1 > MaxPacket)
    return;
  uint32_t size = htonl(static_cast<uint32_t>(text.size() + 1));
  std::vector<uint8_t> packet(5 + text.size());
  memcpy(packet.data(), &size, 4);
  packet[4] = 0;
  memcpy(packet.data() + 5, text.data(), text.size());
  if (!controlSender.enqueue(std::move(packet), GetCurrentThreadId() != uiThread)) {
    // Input edges and control transitions cannot be silently dropped. Abort an
    // overloaded connection rather than blocking the STA or applying stale input.
    shutdown(connection, SD_BOTH);
    if (controller) PostMessageW(controller, WM_CLOSE, 0, 0);
  }
}
HGLOBAL globalBytes(const void *data, size_t size);
void cancelIncomingFiles();
void cancelIncomingFilesExcept(uint64_t generation);
#include "drag_trace.h"
#include "file_tree.h"
#include "file_drag.h"
#include "text_drag.h"
#include "virtual_files.h"

bool readAll(SOCKET socket, void *bytes, size_t n) {
  auto p = static_cast<char *>(bytes);
  while (n && running) {
    int count = NativeTransport::read(socket, p, std::min(n, size_t(65536)));
    if (count <= 0)
      return false;
    networkBytes.fetch_add(uint64_t(count), std::memory_order_relaxed);
    p += count;
    n -= count;
  }
  return n == 0;
}
uint32_t u32(const uint8_t *p) {
  uint32_t n;
  memcpy(&n, p, 4);
  return ntohl(n);
}

void queueMedia(uint8_t kind, std::vector<uint8_t> bytes) {
  if ((kind != 1 || bytes.size() < 36) && (!VideoDecoder::videoKind(kind) || bytes.size() < 60))
    throw std::runtime_error("invalid media frame");
  uint32_t id = u32(bytes.data());
  std::unique_lock<std::mutex> guard(inboxMutex);
  if (frames.size() >= 128 && !frames.count(id))
    throw std::runtime_error("too many media windows");
  if (VideoDecoder::videoKind(kind))
    frameWake.wait(guard,
                   [&] { return !running || !frames.count(id) || frames.at(id).size() < 4; });
  if (!running)
    return;
  auto &queue = frames[id];
  if (kind == 1)
    queue.clear();
  queue.push_back({kind, std::move(bytes), Clock::now()});
  frameWake.notify_all();
}
#include "udp_media.h"
UdpMedia udpMedia;

void receiveLoop(SOCKET socket) {
  try {
    while (running) {
      uint32_t length;
      if (!readAll(socket, &length, 4))
        break;
      size_t size = ntohl(length);
      if (!size || size > MaxPacket)
        break;
      std::vector<uint8_t> packet(size);
      if (!readAll(socket, packet.data(), size))
        break;
      if (packet[0] == 6) {
        udpMedia.repair(packet.data() + 1, packet.size() - 1);
        continue;
      }
      if (packet[0] == 1 || VideoDecoder::videoKind(packet[0])) {
        uint8_t kind = packet[0];
        packet.erase(packet.begin());
        queueMedia(kind, std::move(packet));
        continue;
      }
      if (packet[0] == 0) {
        auto message = Json::parse(packet.begin() + 1, packet.end());
        if (message.value("type", "") == "udp_start") {
          udpMedia.activate();
          continue;
        }
      }
      bool control = packet[0] == 0;
      bool notifyUI = control;
      Json incoming;
      if (control) {
        incoming = Json::parse(packet.begin() + 1, packet.end());
        if (FileTree::uploadResponse(incoming) || FileDrag::receive(incoming)) continue;
        auto type = incoming.value("type", "");
        if (type == "tree_begin" || type == "tree_chunk" || type == "tree_done" || type == "tree_error" ||
            type == "files_begin" || type == "file_chunk" || type == "files_done" || type == "files_error")
          incoming["_file_io_bytes"] = size; // receiver-derived; never trust a peer-supplied cost
      }
      {
        std::unique_lock<std::mutex> guard(inboxMutex);
        if (control) {
          // Never stop the receiver behind a UI queue: IStream may be waiting
          // on this thread while the STA is inside an OLE data callback.
          if (controls.size() >= 128) throw std::runtime_error("Control inbox full");
          if (!running) break;
          auto message = std::move(incoming);
          if ((message.value("type", "") == "data_drag" || message.value("type", "") == "toplevel_drag") &&
              !message.value("active", true))
            pendingDragVisual = {{"type","drag_visual_reset"}};
          if (message.value("type", "") == "window_closed") {
            decoderGarbage.push_back(message.at("id").get<uint32_t>());
            frameWake.notify_all();
          }
          if (message.value("type", "") == "drag_visual") {
            if (size > 352 * 1024) throw std::runtime_error("DND visual exceeds limit");
            notifyUI = pendingDragVisual.is_null();
            pendingDragVisual = std::move(message);
          } else if (message.value("type", "") == "cursor")
            pendingCursor = std::move(message); // bounded latest-only animation state
          else
            controls.push_back(std::move(message));
        } else
          break;
      }
      if (control && notifyUI)
        PostMessageW(controller, NetworkReady, 0, 0);
    }
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
  }
  FileTree::cancelUploads();
  FileDrag::disconnect();
  PostMessageW(controller, WM_CLOSE, 0, 0);
}

void startMedia(const std::string &key) {
  if (mediaReceiver.joinable())
    return;
  mediaReceiver = std::thread([key] {
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    addrinfo *addresses = nullptr;
    SOCKET socket = INVALID_SOCKET;
    if (getaddrinfo(serverHost.c_str(), serverPort.c_str(), &hints, &addresses) == 0) {
      for (auto a = addresses; a && running; a = a->ai_next) {
        socket = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (socket == INVALID_SOCKET)
          continue;
        u_long nonblocking = 1;
        ioctlsocket(socket, FIONBIO, &nonblocking);
        int result = connect(socket, a->ai_addr, int(a->ai_addrlen));
        if (result && WSAGetLastError() == WSAEWOULDBLOCK) {
          fd_set writes, errors;
          FD_ZERO(&writes);
          FD_ZERO(&errors);
          FD_SET(socket, &writes);
          FD_SET(socket, &errors);
          timeval timeout{3, 0};
          result = select(0, nullptr, &writes, &errors, &timeout) > 0 && FD_ISSET(socket, &writes)
                       ? 0
                       : -1;
        }
        nonblocking = 0;
        ioctlsocket(socket, FIONBIO, &nonblocking);
        if (!result)
          break;
        closesocket(socket);
        socket = INVALID_SOCKET;
      }
      freeaddrinfo(addresses);
    }
    if (socket == INVALID_SOCKET || !running) {
      if (socket != INVALID_SOCKET)
        closesocket(socket);
      PostMessageW(controller, WM_CLOSE, 0, 0);
      return;
    }
    mediaConnection = socket;
    int one = 1;
    setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char *>(&one), sizeof(one));
    try { NativeTransport::attach(socket, serverHost); }
    catch (const std::exception &error) {
      std::cerr << "Media connection: " << error.what() << '\n';
      mediaConnection = INVALID_SOCKET; closesocket(socket);
      PostMessageW(controller, WM_CLOSE, 0, 0); return;
    }
    auto text = Json({{"type", "hello"},
                      {"version", 3},
                      {"token", serverToken},
                      {"channel", "media"},
                      {"media_key", key}})
                    .dump();
    uint32_t length = htonl(uint32_t(text.size() + 1));
    std::vector<uint8_t> wire(5 + text.size());
    memcpy(wire.data(), &length, 4);
    wire[4] = 0;
    memcpy(wire.data() + 5, text.data(), text.size());
    size_t sent = 0;
    while (running && sent < wire.size()) {
      int n = NativeTransport::write(socket, wire.data() + sent, wire.size() - sent);
      if (n <= 0)
        break;
      sent += n;
    }
    if (sent == wire.size())
      receiveLoop(socket);
    else
      PostMessageW(controller, WM_CLOSE, 0, 0);
    mediaConnection = INVALID_SOCKET;
    NativeTransport::detach(socket);
    closesocket(socket);
  });
}

void receiveAudio(SOCKET socket) {
  bool ready = false;
  uint64_t lastSequence = 0;
  try {
    uint8_t header[4];
    while (running && readAll(socket, header, 4)) {
      uint32_t size = u32(header);
      if (!size || size > 19229)
        break;
      std::vector<uint8_t> packet(size);
      if (!readAll(socket, packet.data(), size))
        break;
      if (!ready) {
        if (packet[0] != 0 ||
            Json::parse(packet.begin() + 1, packet.end()).value("type", "") != "audio_ready")
          break;
        ready = true;
        audioPlayback.start();
        continue;
      }
      if (packet[0] != 3 || size < 29)
        break;
      const uint8_t *p = packet.data() + 1;
      uint32_t frames = u32(p + 8);
      if (u32(p) != 48000 || p[4] != 0 || p[5] != 2 || p[6] != 0 || p[7] != 16 || !frames ||
          frames > 4800 || size != 29 + frames * 4)
        break;
      uint64_t sequence = (uint64_t(u32(p + 12)) << 32) | u32(p + 16);
      if (sequence <= lastSequence)
        continue;
      lastSequence = sequence;
      audioPlayback.push(p + 28, size_t(frames) * 4);
      auto ack = Json({{"type", "audio_ack"}, {"sequence", sequence}}).dump();
      uint32_t length = htonl(uint32_t(ack.size() + 1));
      std::vector<uint8_t> wire(5 + ack.size());
      memcpy(wire.data(), &length, 4);
      wire[4] = 0;
      memcpy(wire.data() + 5, ack.data(), ack.size());
      size_t sent = 0;
      while (running && sent < wire.size()) {
        int n = NativeTransport::write(socket, wire.data() + sent, wire.size() - sent);
        if (n <= 0)
          return;
        sent += size_t(n);
      }
    }
  } catch (const std::exception &e) {
    std::cerr << "Audio channel stopped: " << e.what() << '\n';
  }
}

void startAudio(const std::string &key) {
  if (audioReceiver.joinable())
    return;
  audioReceiver = std::thread([key] {
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    addrinfo *addresses = nullptr;
    SOCKET socket = INVALID_SOCKET;
    if (getaddrinfo(serverHost.c_str(), serverPort.c_str(), &hints, &addresses) == 0) {
      for (auto a = addresses; a && running; a = a->ai_next) {
        socket = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (socket == INVALID_SOCKET)
          continue;
        u_long nonblocking = 1;
        ioctlsocket(socket, FIONBIO, &nonblocking);
        int result = connect(socket, a->ai_addr, int(a->ai_addrlen));
        if (result && WSAGetLastError() == WSAEWOULDBLOCK) {
          fd_set writes, errors;
          FD_ZERO(&writes);
          FD_ZERO(&errors);
          FD_SET(socket, &writes);
          FD_SET(socket, &errors);
          timeval timeout{3, 0};
          result = select(0, nullptr, &writes, &errors, &timeout) > 0 && FD_ISSET(socket, &writes)
                       ? 0
                       : -1;
        }
        nonblocking = 0;
        ioctlsocket(socket, FIONBIO, &nonblocking);
        if (!result)
          break;
        closesocket(socket);
        socket = INVALID_SOCKET;
      }
      freeaddrinfo(addresses);
    }
    if (socket == INVALID_SOCKET || !running) {
      if (socket != INVALID_SOCKET)
        closesocket(socket);
      std::cerr << "Audio channel unavailable; video continues\n";
      return;
    }
    audioConnection = socket;
    int one = 1;
    setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char *>(&one), sizeof(one));
    try { NativeTransport::attach(socket, serverHost); }
    catch (const std::exception &error) {
      std::cerr << "Audio connection: " << error.what() << '\n';
      audioConnection = INVALID_SOCKET; closesocket(socket); return;
    }
    auto text = Json({{"type", "hello"},
                      {"version", 3},
                      {"token", serverToken},
                      {"channel", "audio"},
                      {"media_key", key}})
                    .dump();
    uint32_t length = htonl(uint32_t(text.size() + 1));
    std::vector<uint8_t> wire(5 + text.size());
    memcpy(wire.data(), &length, 4);
    wire[4] = 0;
    memcpy(wire.data() + 5, text.data(), text.size());
    size_t sent = 0;
    while (running && sent < wire.size()) {
      int n = NativeTransport::write(socket, wire.data() + sent, wire.size() - sent);
      if (n <= 0)
        break;
      sent += n;
    }
    if (sent == wire.size())
      receiveAudio(socket);
    else
      std::cerr << "Audio channel unavailable; video continues\n";
    audioConnection = INVALID_SOCKET;
    NativeTransport::detach(socket);
    closesocket(socket);
  });
}

std::unique_ptr<Gdiplus::Bitmap> decodeImage(const uint8_t *data, size_t size) {
  HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, size);
  if (!mem)
    return {};
  void *ptr = GlobalLock(mem);
  if (!ptr) {
    GlobalFree(mem);
    return {};
  }
  memcpy(ptr, data, size);
  GlobalUnlock(mem);
  IStream *stream = nullptr;
  if (FAILED(CreateStreamOnHGlobal(mem, TRUE, &stream))) {
    GlobalFree(mem);
    return {};
  }
  std::unique_ptr<Gdiplus::Bitmap> decoded(Gdiplus::Bitmap::FromStream(stream));
  std::unique_ptr<Gdiplus::Bitmap> result;
  if (decoded && decoded->GetLastStatus() == Gdiplus::Ok && decoded->GetWidth() > 0 &&
      decoded->GetHeight() > 0 && decoded->GetWidth() <= 8192 && decoded->GetHeight() <= 8192 &&
      uint64_t(decoded->GetWidth()) * decoded->GetHeight() <= 16777216) {
    result = std::make_unique<Gdiplus::Bitmap>(decoded->GetWidth(), decoded->GetHeight(),
                                               PixelFormat32bppARGB);
    Gdiplus::Graphics g(result.get());
    g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    g.DrawImage(decoded.get(), 0, 0);
  }
  decoded.reset();
  stream->Release();
  return result;
}

// Encoded reference frames are decoded in order. Only completed display frames may be replaced.
void decodeLoop() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  auto images = std::make_unique<ImageDecoder>();
  struct State {
    uint32_t epoch = 0;
    uint8_t kind = 0;
    std::unique_ptr<H264Decoder> decoder;
    std::map<uint64_t, std::shared_ptr<DecodedFrame>> pending;
  };
  std::map<uint32_t, State> videoStates;
  H264DecoderCache decoderCache;
  std::set<uint32_t> disabled, closed;
  uint32_t lastId = 0;
  while (running) {
    EncodedFrame encoded;
    uint32_t id = 0;
    std::vector<uint32_t> garbage;
    {
      std::unique_lock<std::mutex> lock(inboxMutex);
      frameWake.wait_for(lock, std::chrono::milliseconds(200),
                         [] { return !frames.empty() || !decoderGarbage.empty() || !running; });
      if (!running)
        break;
      garbage.swap(decoderGarbage);
      if (!frames.empty()) {
        auto entry = frames.upper_bound(lastId);
        if (entry == frames.end())
          entry = frames.begin();
        id = entry->first;
        lastId = id;
        encoded = std::move(entry->second.front());
        entry->second.pop_front();
        if (entry->second.empty())
          frames.erase(entry);
      }
    }
    frameWake.notify_all();
    for (auto removed : garbage) {
      videoStates.erase(removed);
      disabled.erase(removed);
      closed.insert(removed);
    }
    std::vector<uint32_t> stalled;
    for (auto &[window, state] : videoStates)
      if (!state.pending.empty() &&
          Clock::now() - state.pending.begin()->second->received > std::chrono::milliseconds(1500))
        stalled.push_back(window);
    for (auto window : stalled) {
      std::cerr << "Video decoder produced no frame; requesting PNG fallback for window " << window
                << '\n';
      auto kind = videoStates.at(window).kind;
      videoStates.erase(window);
      disabled.insert(window);
      sendJson(
          {{"type", "codec_fallback"}, {"id", window}, {"codec", VideoDecoder::capability(kind)}});
    }
    if (!id || closed.count(id))
      continue;
    auto &bytes = encoded.bytes;
    if (bytes.size() < 36)
      continue;
    clientStats.record("decode_queue", millis(Clock::now() - encoded.received));
    auto started = Clock::now();
    auto frame = std::make_shared<DecodedFrame>();
    frame->received = encoded.received;
    uint32_t width = u32(bytes.data() + 4), height = u32(bytes.data() + 8);
    if (!width || !height || width > 8192 || height > 8192 || uint64_t(width) * height > 16777216)
      continue;
    frame->width = int(width);
    frame->height = int(height);
    frame->sequence = (uint64_t(u32(bytes.data() + 12)) << 32) | u32(bytes.data() + 16);
    frame->logicalWidth = int32_t(u32(bytes.data() + 20));
    frame->logicalHeight = int32_t(u32(bytes.data() + 24));
    frame->originX = int32_t(u32(bytes.data() + 28));
    frame->originY = int32_t(u32(bytes.data() + 32));
    if (frame->logicalWidth <= 0 || frame->logicalHeight <= 0 || frame->logicalWidth > 8192 ||
        frame->logicalHeight > 8192)
      continue;
    std::vector<std::shared_ptr<DecodedFrame>> completed;
    if (VideoDecoder::videoKind(encoded.kind)) {
      frame->codec = encoded.kind;
      if (disabled.count(id))
        continue;
      try {
        if (!supportedVideoKinds.count(encoded.kind))
          throw std::runtime_error("Server selected an unadvertised video codec");
        if (bytes.size() < 60)
          throw std::runtime_error("Invalid video header");
        uint32_t epoch = u32(bytes.data() + 36), cw = u32(bytes.data() + 40),
                 ch = u32(bytes.data() + 44), key = u32(bytes.data() + 48),
                 color = u32(bytes.data() + 52), alpha = u32(bytes.data() + 56);
        if (cw < width || ch < height || cw > 4096 || ch > 2304 || (cw & 1) || (ch & 1) ||
            key > 1 || uint64_t(color) + alpha + 60 != bytes.size() || !color)
          throw std::runtime_error("Invalid video dimensions/length");
        auto &state = videoStates[id];
        if (!state.decoder || state.epoch != epoch || state.kind != encoded.kind ||
            !state.decoder->matches(cw, ch, encoded.kind)) {
          if (!key)
            throw std::runtime_error("New video epoch did not start with an independent keyframe");
          state.pending.clear();
          auto initStarted = Clock::now();
          decoderCache.recycle(std::move(state.decoder));
          bool reused = false;
          state.decoder = decoderCache.acquire(cw, ch, reused, encoded.kind);
          clientStats.record(reused ? "decoder_reuse" : "decoder_create",
                             millis(Clock::now() - initStarted));
          state.epoch = epoch;
          state.kind = encoded.kind;
          std::cout << "D3D11 hardware " << VideoDecoder::name(encoded.kind)
                    << " decoder active for window " << id << '\n';
        }
        if (alpha) {
          auto maskStarted = Clock::now();
          if (!images->decode(bytes.data() + 60 + color, alpha, width, height, true, frame->alpha,
                              VideoDecoder::hdr(encoded.kind)))
            throw std::runtime_error("Invalid video alpha mask");
          if (VideoDecoder::hdr(encoded.kind)) {
            for (size_t p = 0; p + 1 < frame->alpha.size(); p += 2) {
              uint16_t a = uint16_t(frame->alpha[p]) | (uint16_t(frame->alpha[p + 1]) << 8);
              if (a > 0x3c00 && a != 0x8000)
                throw std::runtime_error("Invalid FP16 alpha");
            }
          }
          clientStats.record("alpha_decode", millis(Clock::now() - maskStarted));
        }
        state.pending[frame->sequence] = frame;
        if (state.pending.size() > 8)
          throw std::runtime_error("Hardware decoder buffered too many frames");
        auto videoStarted = Clock::now();
        auto surfaces = state.decoder->decode(bytes.data() + 60, color, frame->sequence);
        clientStats.record(encoded.kind == 2                          ? "h264_transform"
                           : (encoded.kind == 4 || encoded.kind == 7) ? "hevc_transform"
                                                                      : "av1_transform",
                           millis(Clock::now() - videoStarted));
        for (auto &surface : surfaces) {
          auto found = state.pending.find(surface.sequence);
          if (found == state.pending.end())
            throw std::runtime_error("Decoder output sequence mismatch");
          auto ready = found->second;
          ready->video = std::move(surface);
          completed.push_back(ready);
          state.pending.erase(found);
        }
      } catch (const std::exception &e) {
        std::cerr << VideoDecoder::name(encoded.kind) << " decode failed for window " << id << ": "
                  << e.what() << "; requesting PNG fallback\n";
        videoStates.erase(id);
        disabled.insert(id);
        sendJson({{"type", "codec_fallback"},
                  {"id", id},
                  {"codec", VideoDecoder::capability(encoded.kind)}});
        continue;
      }
    } else {
      videoStates.erase(id);
      const uint8_t magic[] = {137, 80, 78, 71, 13, 10, 26, 10};
      if (bytes.size() < 69 || memcmp(bytes.data() + 36, magic, 8) ||
          u32(bytes.data() + 52) != width || u32(bytes.data() + 56) != height)
        continue;
      if (!images->decode(bytes.data() + 36, bytes.size() - 36, width, height, false, frame->bgra))
        continue;
      completed.push_back(frame);
    }
    clientStats.record(encoded.kind == 2                          ? "h264_decode"
                       : (encoded.kind == 4 || encoded.kind == 7) ? "hevc_decode"
                       : (encoded.kind == 5 || encoded.kind == 8) ? "av1_decode"
                                                                  : "png_decode",
                       millis(Clock::now() - started));
    for (auto &ready : completed) {
      ready->decoded = Clock::now();
      std::lock_guard<std::mutex> lock(inboxMutex);
      if (decodedFrames.size() >= 128 && !decodedFrames.count(id))
        continue;
      decodedFrames[id] = ready;
    }
    if (!completed.empty())
      PostMessageW(controller, NetworkReady, 0, 0);
  }
  videoStates.clear();
  decoderCache.clear();
  images.reset();
  CoUninitialize();
}

UINT effectiveDpi(HWND hwnd = nullptr) {
  wchar_t overrideValue[16]{};
  if (GetEnvironmentVariableW(L"WAYBRIDGE_DPI", overrideValue, 16))
    return std::clamp(UINT(_wtoi(overrideValue)), 96u, 384u);
  return hwnd ? GetDpiForWindow(hwnd) : GetDpiForSystem();
}

HGLOBAL globalBytes(const void *data, size_t size) {
  HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, size);
  if (!mem)
    return nullptr;
  void *p = GlobalLock(mem);
  if (!p) {
    GlobalFree(mem);
    return nullptr;
  }
  memcpy(p, data, size);
  GlobalUnlock(mem);
  return mem;
}

bool uploadFiles(std::vector<std::wstring> paths, Json drop = Json::object(),
                 std::function<bool()> current = {}) {
  if (!hostedFiles || (current && !current())) return false;
  if (paths.empty() || paths.size() > FileTree::MaxRoots) return false;
  // Tree sources perform metadata checks and enumeration on their upload worker.
  if (FileTree::supported) return FileTree::upload(std::move(paths), std::move(drop), std::move(current));
  for (const auto &path : paths) {
    WIN32_FILE_ATTRIBUTE_DATA metadata{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &metadata) || (metadata.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
      std::cerr << "File source is unavailable or is a reparse point\n"; return false;
    }
    if ((metadata.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !FileTree::supported) {
      std::cerr << "The server does not support file_tree; directories were not transferred\n"; return false;
    }
  }
  fileWorkers.emplace_back([paths = std::move(paths), drop = std::move(drop),
                            current = std::move(current)] {
    auto valid = [&] { return running && (!current || current()); };
    Json transfers = Json::array();
    for (const auto &path : paths) {
      if (!valid())
        return;
      HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file == INVALID_HANDLE_VALUE)
        continue;
      LARGE_INTEGER size{};
      if (!GetFileSizeEx(file, &size) || size.QuadPart > 1024LL * 1024 * 1024) {
        CloseHandle(file);
        continue;
      }
      size_t slash = path.find_last_of(L"/\\");
      std::string name = utf8(path.substr(slash == std::wstring::npos ? 0 : slash + 1));
      if (!valid()) {
        CloseHandle(file);
        return;
      }
      uint64_t id = ++nextUpload;
      sendJson(
          {{"type", "upload_begin"}, {"transfer", id}, {"name", name}, {"size", size.QuadPart}});
      std::vector<uint8_t> buffer(65536);
      DWORD n = 0;
      bool ok = true;
      while (valid()) {
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &n, nullptr)) {
          ok = false;
          break;
        }
        if (!n || !valid())
          break;
        sendJson({{"type", "upload_chunk"}, {"transfer", id}, {"data", base64(buffer.data(), n)}});
      }
      CloseHandle(file);
      if (!ok || !valid())
        return;
      sendJson({{"type", "upload_end"}, {"transfer", id}});
      transfers.push_back(id);
    }
    if (!transfers.empty() && valid()) {
      Json commit = drop;
      commit["type"] = "upload_commit";
      commit["transfers"] = transfers;
      sendJson(commit);
    }
  });
  return true;
}

RECT imageRect(RemoteWindow *w);
void dispatchNetwork();
RemoteWindow *windowDragTarget(POINT point);
void pumpDragMessages() {
  MSG message{};
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
    if (message.message == WM_QUIT) {
      PostQuitMessage(int(message.wParam));
      TextDrag::reset();
      break;
    }
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
}

class RemoteDropTarget final : public IDropTarget {
  LONG refs = 1;
  uint32_t windowId;
  std::weak_ptr<void> lifetime;
  bool accepted = false;
  uint64_t textDrag = 0, textTarget = 0;

  Json position(POINTL point) const {
    auto *window = findWindow();
    if (!window)
      return Json::object();
    POINT local{point.x, point.y};
    ScreenToClient(window->hwnd, &local);
    RECT image = imageRect(window);
    if (image.right <= image.left || image.bottom <= image.top)
      return Json::object();
    return {{"id", window->id},
            {"origin_x", window->originX},
            {"origin_y", window->originY},
            {"x", double(local.x - image.left) * window->width / (image.right - image.left)},
            {"y", double(local.y - image.top) * window->height / (image.bottom - image.top)}};
  }
  RemoteWindow *findWindow() const {
    if (lifetime.expired())
      return nullptr;
    auto found = windows.find(windowId);
    if (found == windows.end() || found->second->dropLifetime != lifetime.lock())
      return nullptr;
    return found->second.get();
  }

public:
  explicit RemoteDropTarget(RemoteWindow *w) : windowId(w->id), lifetime(w->dropLifetime) {}
  ~RemoteDropTarget() { TextDrag::leaveTarget(textTarget); }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    if (iid == IID_IUnknown || iid == IID_IDropTarget) {
      *out = static_cast<IDropTarget *>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
  ULONG STDMETHODCALLTYPE Release() override {
    auto count = InterlockedDecrement(&refs);
    if (!count)
      delete this;
    return count;
  }
  HRESULT STDMETHODCALLTYPE DragEnter(IDataObject *data, DWORD, POINTL point,
                                      DWORD *effect) override {
    // Reentrant OLE calls can close the window and revoke its registration.
    AddRef();
    VirtualFiles::Com<IDropTarget> self(this);
    DragTrace::event("ole.enter", {{"window", windowId},
                                   {"effects", *effect},
                                   {"files", hostedFiles},
                                   {"read_only", hostedReadOnly},
                                   {"data_drag", dataDragNegotiated}});
    const uint64_t target = TextDrag::enterTarget();
    textTarget = target;
    textDrag = 0;
    accepted = false;
    const auto current = [this, target] {
      return running && textTarget == target && TextDrag::currentTarget(target) && findWindow() &&
             !hostedReadOnly;
    };
    if (dataDragNegotiated && current() && FileDrag::returnToRemote(data, position(point))) {
      *effect = DROPEFFECT_NONE;
      return S_OK;
    }
    DragTrace::formats(data);
    FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    bool files = hostedFiles && current() && (*effect & DROPEFFECT_COPY) &&
                 !FileDrag::isOwn(data) &&
                 (SUCCEEDED(data->QueryGetData(&format)) || VirtualFiles::supports(data));
    uint64_t candidate = 0;
    if (!files && current() && hostedDragText && dataDragNegotiated &&
        (*effect & DROPEFFECT_COPY) && !FileDrag::isOwn(data) && TextDrag::supports(data)) {
      auto location = position(point);
      if (location.contains("id"))
        candidate = TextDrag::begin(data, location, target,
                                    [&current] { return current() && dataDragNegotiated; });
    }
    // External providers can pump DragLeave/DragEnter, close a window or apply
    // new sharing settings. Do not overwrite state committed by that reentry.
    if (!current()) {
      if (TextDrag::currentTarget(target))
        TextDrag::cancel(candidate);
      *effect = DROPEFFECT_NONE;
      return S_OK;
    }
    if (candidate) {
      bool inside = true;
      const auto pump = [&] {
        pumpDragMessages();
        if (!current() || TextDrag::active != candidate)
          return;
        POINT cursor{};
        GetCursorPos(&cursor);
        auto actual = windowDragTarget(cursor);
        inside = actual && actual->id == windowId;
        if (inside)
          TextDrag::refresh(candidate, position({cursor.x, cursor.y}));
      };
      TextDrag::awaitAcceptance(candidate, target, [&] { return inside && current(); }, pump);
    }
    if (!current()) {
      if (TextDrag::currentTarget(target))
        TextDrag::cancel(candidate);
      *effect = DROPEFFECT_NONE;
      return S_OK;
    }
    accepted = files;
    textDrag = candidate;
    DragTrace::event("ole.enter.result", {{"window", windowId},
                                          {"files", files},
                                          {"text_drag", textDrag},
                                          {"accepted", TextDrag::allowed(textDrag)}});
    *effect = accepted || TextDrag::allowed(textDrag) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL point, DWORD *effect) override {
    if (textDrag) {
      if (!TextDrag::currentTarget(textTarget) || !findWindow() || hostedReadOnly ||
          !hostedDragText) {
        TextDrag::leaveTarget(textTarget);
        textDrag = textTarget = 0;
      } else {
        auto location = position(point);
        if (location.contains("id"))
          TextDrag::motion(textDrag, location);
      }
      *effect = TextDrag::allowed(textDrag) && (*effect & DROPEFFECT_COPY) ? DROPEFFECT_COPY
                                                                           : DROPEFFECT_NONE;
      return S_OK;
    }
    *effect = accepted && TextDrag::currentTarget(textTarget) && findWindow() &&
                      (*effect & DROPEFFECT_COPY)
                  ? DROPEFFECT_COPY
                  : DROPEFFECT_NONE;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DragLeave() override {
    TextDrag::leaveTarget(textTarget);
    textDrag = textTarget = 0;
    accepted = false;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Drop(IDataObject *data, DWORD, POINTL point, DWORD *effect) override {
    AddRef();
    VirtualFiles::Com<IDropTarget> self(this);
    DragTrace::event("ole.drop", {{"window", windowId},
                                  {"text_drag", textDrag},
                                  {"effects", *effect},
                                  {"accepted", TextDrag::allowed(textDrag)}});
    bool copyAllowed = (*effect & DROPEFFECT_COPY) != 0;
    *effect = DROPEFFECT_NONE;
    accepted = false;
    if (textDrag) {
      auto location = position(point);
      auto gesture = textDrag, target = textTarget;
      const auto current = [this, target] {
        return running && textTarget == target && TextDrag::currentTarget(target) &&
               !hostedReadOnly && findWindow();
      };
      if (copyAllowed && current() && location.contains("id")) {
        TextDrag::refresh(gesture, location, true);
        if (TextDrag::awaitAcceptance(gesture, target, current, pumpDragMessages) &&
            TextDrag::drop(gesture, location) &&
            TextDrag::awaitCompletion(gesture, target, current, pumpDragMessages))
          *effect = DROPEFFECT_COPY;
      }
      if (*effect != DROPEFFECT_COPY)
        TextDrag::leaveTarget(target);
      if (textTarget == target)
        textDrag = textTarget = 0;
      return S_OK;
    }
    TextDrag::leaveTarget(textTarget);
    textTarget = 0;
    if (!copyAllowed || !hostedFiles || !findWindow() || FileDrag::isOwn(data))
      return S_OK;
    FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM storage{};
    if (FAILED(data->GetData(&format, &storage))) {
      auto *window = findWindow();
      if (!window)
        return S_OK;
      POINT position{point.x, point.y};
      ScreenToClient(window->hwnd, &position);
      RECT r = imageRect(window);
      if (r.right <= r.left || r.bottom <= r.top)
        return S_OK;
      Json drop{{"drop", true},
                {"id", window->id},
                {"origin_x", window->originX},
                {"origin_y", window->originY},
                {"x", double(position.x - r.left) * window->width / (r.right - r.left)},
                {"y", double(position.y - r.top) * window->height / (r.bottom - r.top)}};
      auto request = VirtualFiles::submit(data, std::move(drop), window->hwnd, lifetime);
      if (request && (request->asyncStarted || VirtualFiles::wait(request)) && findWindow())
        *effect = DROPEFFECT_COPY;
      return S_OK;
    }
    auto drop = static_cast<HDROP>(storage.hGlobal);
    std::vector<std::wstring> paths;
    UINT count = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
    if (!count || count > FileTree::MaxRoots) {
      ReleaseStgMedium(&storage); std::cerr << "Too many file roots; transfer rejected\n"; return S_OK;
    }
    for (UINT i = 0; i < count; ++i) {
      UINT n = DragQueryFileW(drop, i, nullptr, 0);
      std::wstring path(n + 1, L'\0');
      DragQueryFileW(drop, i, path.data(), n + 1);
      path.resize(n);
      paths.push_back(std::move(path));
    }
    ReleaseStgMedium(&storage);
    auto *window = findWindow();
    if (!window)
      return S_OK;
    POINT position{point.x, point.y};
    ScreenToClient(window->hwnd, &position);
    RECT r = imageRect(window);
    if (!paths.empty() && r.right > r.left && r.bottom > r.top) {
      bool scheduled = uploadFiles(std::move(paths),
                  {{"drop", true},
                   {"id", window->id},
                   {"origin_x", window->originX},
                   {"origin_y", window->originY},
                   {"x", double(position.x - r.left) * window->width / (r.right - r.left)},
                   {"y", double(position.y - r.top) * window->height / (r.bottom - r.top)}},
                  [lifetime = lifetime] { return !lifetime.expired(); });
      *effect = scheduled ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    }
    return S_OK;
  }
};

void cancelIncomingFilesNow() {
  for (auto &[id, file] : incomingFiles) if (file.handle != INVALID_HANDLE_VALUE) CloseHandle(file.handle);
  incomingFiles.clear();
  if (!incomingDirectory.empty()) FileTree::removeTree(incomingDirectory);
  incomingDirectory.clear();
  incomingBatch = 0;
  incomingClipboardSequence = 0;
}
void cancelIncomingFiles() {
  FileIo::cleanup(cancelIncomingFilesNow, &incomingFiles);
}
void cancelIncomingFilesExcept(uint64_t) {
  FileIo::cleanup([] {
    if (incomingBatch < FileTree::serverGeneration) cancelIncomingFilesNow();
  }, &incomingBatch);
}

void receiveFilesNow(const Json &message) {
  const std::string type = message.at("type");
  if (type == "files_begin") {
    uint64_t batch = message.at("batch").get<uint64_t>();
    if ((message.contains("epoch") && message["epoch"].get<uint64_t>() != FileTree::epoch) ||
        batch <= FileTree::latestIncoming || batch < FileTree::serverGeneration || batch <= FileTree::blockedGeneration) return;
    FileTree::latestIncoming = batch;
    FileTree::cancelIncoming();
    cancelIncomingFiles();
    incomingBatch = message.at("batch").get<uint64_t>();
    incomingSelectionEpoch = FileTree::epoch.load();
    incomingClipboardSequence = GetClipboardSequenceNumber();
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    std::wstring directory = std::wstring(temp) + L"Waybridge-" +
                             std::to_wstring(GetCurrentProcessId()) + L"-" +
                             std::to_wstring(GetTickCount64());
    if (!CreateDirectoryW(directory.c_str(), nullptr))
      throw std::runtime_error("Cannot create incoming file directory");
    incomingDirectory = directory;
    if (message.at("files").empty() || message.at("files").size() > 128)
      throw std::runtime_error("Too many incoming files");
    for (const auto &metadata : message.at("files")) {
      const auto fileId = metadata.at("id").get<uint32_t>();
      if (incomingFiles.count(fileId)) throw std::runtime_error("Duplicate incoming file ID");
      std::wstring name = wide(metadata.at("name").get<std::string>());
      if (!FileTree::validName(name))
        throw std::runtime_error("Invalid incoming filename");
      IncomingFile file;
      file.path = directory + L"\\" + name;
      file.remaining = metadata.at("size").get<uint64_t>();
      if (file.remaining > 1024ULL * 1024 * 1024)
        throw std::runtime_error("Incoming file too large");
      file.handle = CreateFileW(file.path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file.handle == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot create incoming file");
      incomingFiles.emplace(fileId, std::move(file));
    }
  } else if (message.value("batch", uint64_t(0)) == incomingBatch) {
    if (type == "file_chunk") {
      auto &file = incomingFiles.at(message.at("file").get<uint32_t>());
      auto bytes = unbase64(message.at("data").get<std::string>());
      DWORD written = 0;
      if (bytes.size() > 65536 || bytes.size() > file.remaining ||
          !WriteFile(file.handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                     nullptr) ||
          written != bytes.size())
        throw std::runtime_error("Invalid incoming file chunk");
      file.remaining -= written;
    } else if (type == "files_done") {
      const auto batch = incomingBatch;
      const auto expectedClipboard = incomingClipboardSequence;
      const auto selectionEpoch = incomingSelectionEpoch;
      std::vector<wchar_t> paths;
      for (auto &[id, file] : incomingFiles) {
        CloseHandle(file.handle);
        file.handle = INVALID_HANDLE_VALUE;
        if (file.remaining)
          throw std::runtime_error("Incomplete incoming file");
        paths.insert(paths.end(), file.path.begin(), file.path.end());
        paths.push_back(L'\0');
      }
      paths.push_back(L'\0');
      ReadOnlyFiles::protectTree(incomingDirectory);
      auto directory = std::shared_ptr<std::wstring>(new std::wstring(std::move(incomingDirectory)),
          [](std::wstring *path) {
            FileIo::cleanup([path] { if (!path->empty()) FileTree::removeTree(*path); delete path; });
          });
      incomingFiles.clear(); incomingDirectory.clear(); incomingBatch = 0;
      FileIo::postUi([batch, expectedClipboard, selectionEpoch, paths = std::move(paths), directory] {
      // A newer local clipboard must win over a file download that finishes
      // late.
      bool published = false;
      if (FileTree::downloadsEnabled && selectionEpoch == FileTree::epoch && batch >= FileTree::serverGeneration &&
          batch > FileTree::blockedGeneration && batch >= FileTree::latestIncoming &&
          GetClipboardSequenceNumber() == FileTree::publicationSequence(batch, expectedClipboard) &&
          OpenClipboard(controller)) {
        EmptyClipboard();
        std::vector<uint8_t> data(sizeof(DROPFILES) + paths.size() * sizeof(wchar_t));
        DROPFILES header{};
        header.pFiles = sizeof(header);
        header.fWide = TRUE;
        memcpy(data.data(), &header, sizeof(header));
        memcpy(data.data() + sizeof(header), paths.data(), paths.size() * sizeof(wchar_t));
        if (auto mem = globalBytes(data.data(), data.size())) {
          if (!SetClipboardData(CF_HDROP, mem))
            GlobalFree(mem);
          else published = true;
        }
        CloseClipboard();
        ownClipboardSequence = GetClipboardSequenceNumber();
      }
      if (published) {
        hostControl.emit({{"type","transfer"},{"event","download_complete"},
                          {"batch",batch},{"directory",utf8(*directory)}});
        FileTree::publishedGeneration = batch;
        directory->clear(); // committed paths outlive the viewer connection
      }
      });
    } else if (type == "files_error") {
      cancelIncomingFiles();
      std::cerr << message.value("message", "File transfer failed") << '\n';
    }
  }
}
void receiveFiles(const Json &message) {
  size_t bytes = message.value("_file_io_bytes", size_t(65536));
  FileTree::require(FileIo::submit([message] {
    try { receiveFilesNow(message); }
    catch (const std::exception &error) {
      cancelIncomingFilesNow();
      std::cerr << "File download: " << error.what() << '\n';
      hostControl.emit({{"type", "transfer"}, {"event", "download_error"}, {"message", error.what()}});
    }
  }, bytes), "File I/O queue full");
}
void setClipboard(const Json &items) {
  if (hostedClipboard == "off" || hostedClipboard == "send") return;
  if (!items.is_object() || !OpenClipboard(controller))
    return;
  EmptyClipboard();
  for (const auto *mime : {"text/plain;charset=utf-8", "text/plain"}) {
    if (!items.contains(mime) || !items[mime].is_string())
      continue;
    auto bytes = unbase64(items[mime].get<std::string>());
    std::wstring text = wide(std::string(bytes.begin(), bytes.end()));
    if (auto mem = globalBytes(text.c_str(), (text.size() + 1) * sizeof(wchar_t))) {
      if (!SetClipboardData(CF_UNICODETEXT, mem))
        GlobalFree(mem);
    }
    break;
  }
  if (hostedImages && items.contains("image/png") && items["image/png"].is_string()) {
    auto bytes = unbase64(items["image/png"].get<std::string>());
    if (!bytes.empty()) {
      if (auto mem = globalBytes(bytes.data(), bytes.size())) {
        if (!SetClipboardData(pngFormat, mem))
          GlobalFree(mem);
      }
      auto bitmap = decodeImage(bytes.data(), bytes.size());
      if (bitmap) {
        Gdiplus::BitmapData pixels{};
        Gdiplus::Rect rect(0, 0, bitmap->GetWidth(), bitmap->GetHeight());
        if (bitmap->LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &pixels) ==
            Gdiplus::Ok) {
          BITMAPINFOHEADER header{};
          header.biSize = sizeof(header);
          header.biWidth = rect.Width;
          header.biHeight = -rect.Height;
          header.biPlanes = 1;
          header.biBitCount = 32;
          header.biCompression = BI_RGB;
          header.biSizeImage = rect.Width * rect.Height * 4;
          std::vector<uint8_t> dib(sizeof(header) + header.biSizeImage);
          memcpy(dib.data(), &header, sizeof(header));
          for (int y = 0; y < rect.Height; ++y)
            memcpy(dib.data() + sizeof(header) + y * rect.Width * 4,
                   static_cast<uint8_t *>(pixels.Scan0) + y * pixels.Stride, rect.Width * 4);
          bitmap->UnlockBits(&pixels);
          if (auto mem = globalBytes(dib.data(), dib.size())) {
            if (!SetClipboardData(CF_DIB, mem))
              GlobalFree(mem);
          }
        }
      }
    }
  }
  CloseClipboard();
  ownClipboardSequence = GetClipboardSequenceNumber();
}

void publishClipboard() {
  if (hostedClipboard == "off" || hostedClipboard == "receive") return;
  if (hostedFiles && VirtualFiles::clipboard())
    return;
  if (!OpenClipboard(controller))
    return;
  Json items = Json::object();
  std::vector<std::wstring> files;
  if (auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP)); hostedFiles && drop) {
    UINT count = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
    if (!count || count > FileTree::MaxRoots) {
      CloseClipboard(); std::cerr << "Too many file roots; clipboard transfer rejected\n"; return;
    }
    for (UINT i = 0; i < count; ++i) {
      UINT n = DragQueryFileW(drop, i, nullptr, 0);
      std::wstring name(n + 1, L'\0');
      DragQueryFileW(drop, i, name.data(), n + 1);
      name.resize(n);
      files.push_back(std::move(name));
    }
  }
  if (auto mem = GetClipboardData(CF_UNICODETEXT)) {
    auto p = static_cast<const wchar_t *>(GlobalLock(mem));
    if (p) {
      size_t maxChars = std::min(GlobalSize(mem) / sizeof(wchar_t), size_t(4 * 1024 * 1024));
      size_t n = 0;
      while (n < maxChars && p[n])
        ++n;
      auto text = utf8(std::wstring(p, n));
      GlobalUnlock(mem);
      items["text/plain;charset=utf-8"] =
          base64(reinterpret_cast<const uint8_t *>(text.data()), text.size());
    }
  }
  if (auto mem = GetClipboardData(pngFormat); hostedImages && mem) {
    auto p = static_cast<const uint8_t *>(GlobalLock(mem));
    if (p) {
      size_t n = GlobalSize(mem);
      if (n <= 12 * 1024 * 1024)
        items["image/png"] = base64(p, n);
      GlobalUnlock(mem);
    }
  } else if (auto handle = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP)); hostedImages && handle) {
    Gdiplus::Bitmap bitmap(handle, nullptr);
    IStream *stream = nullptr;
    // Standard PNG encoder CLSID supplied by GDI+.
    CLSID pngEncoder{0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
    if (SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) {
      if (bitmap.Save(stream, &pngEncoder, nullptr) == Gdiplus::Ok) {
        HGLOBAL mem = nullptr;
        STATSTG stat{};
        GetHGlobalFromStream(stream, &mem);
        stream->Stat(&stat, STATFLAG_NONAME);
        auto p = static_cast<const uint8_t *>(GlobalLock(mem));
        if (p) {
          if (stat.cbSize.QuadPart <= 12 * 1024 * 1024)
            items["image/png"] = base64(p, static_cast<size_t>(stat.cbSize.QuadPart));
          GlobalUnlock(mem);
        }
      }
      stream->Release();
    }
  }
  CloseClipboard();
  if (!files.empty()) {
    uploadFiles(std::move(files));
    return;
  }
  if (!items.empty())
    sendJson({{"type", "clipboard"}, {"items", items}});
}

RECT imageRect(RemoteWindow *w) {
  RECT r;
  GetClientRect(w->hwnd, &r);
  if (w->width <= 0 || w->height <= 0)
    return r;
  double scale = std::min(double(r.right) / w->width, double(r.bottom) / w->height);
  int width = static_cast<int>(w->width * scale), height = static_cast<int>(w->height * scale);
  return {(r.right - width) / 2, (r.bottom - height) / 2, (r.right + width) / 2,
          (r.bottom + height) / 2};
}
void refreshCursor();
void releasePointer(bool notify = true) {
  if (!relativeInput.active())
    return;
  auto id = relativeInput.id();
  auto generation = relativeInput.generation();
  relativeInput.release();
  refreshCursor();
  if (notify)
    sendJson({{"type", "pointer_lock_ack"}, {"id", id},
              {"generation", generation}, {"active", false}});
}
void pointer(RemoteWindow *w, LPARAM param) {
  if (relativeInput.active())
    return;
  RECT r = imageRect(w);
  if (r.right <= r.left || r.bottom <= r.top)
    return;
  double x = double(GET_X_LPARAM(param) - r.left) * w->width / (r.right - r.left);
  double y = double(GET_Y_LPARAM(param) - r.top) * w->height / (r.bottom - r.top);
  sendJson({{"type", "pointer"},
            {"id", w->id},
            {"x", x},
            {"y", y},
            {"origin_x", w->originX},
            {"origin_y", w->originY}});
}
RemoteWindow *windowDragTarget(POINT point) {
  struct Search { POINT point; RemoteWindow *target = nullptr; } search{point};
  EnumWindows([](HWND hwnd, LPARAM data) -> BOOL {
    auto &s = *reinterpret_cast<Search *>(data);
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || hwnd == controller || dragVisual.owns(hwnd))
      return TRUE;
    auto dragged = windows.find(windowDrag.attached);
    if (dragged != windows.end() && hwnd == dragged->second->hwnd)
      return TRUE;
    RECT rect{};
    if (!GetWindowRect(hwnd, &rect) || !PtInRect(&rect, s.point))
      return TRUE;
    for (auto &[id, window] : windows) {
      if (window->hwnd != hwnd) continue;
      POINT local = s.point;
      ScreenToClient(hwnd, &local);
      RECT image = imageRect(window.get());
      if (PtInRect(&image, local)) s.target = window.get();
      break;
    }
    return FALSE; // Another local application's window occludes the remote target.
  }, reinterpret_cast<LPARAM>(&search));
  return search.target;
}
void moveDraggedWindow() {
  auto found = windows.find(windowDrag.attached);
  if (!windowDrag.active || windowDrag.released || found == windows.end()) return;
  auto *w = found->second.get();
  POINT cursor{};
  if (!GetCursorPos(&cursor)) return;
  POINT client{0, 0};
  ClientToScreen(w->hwnd, &client);
  RECT outer{};
  GetWindowRect(w->hwnd, &outer);
  SetWindowPos(w->hwnd, nullptr,
               cursor.x - MulDiv(windowDrag.offsetX, w->dpi, 96) - (client.x - outer.left),
               cursor.y - MulDiv(windowDrag.offsetY, w->dpi, 96) - (client.y - outer.top),
               0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}
void sendWindowDragMotion(bool release = false) {
  if (!windowDrag.active || windowDrag.released) return;
  if (dataDragNegotiated) {
    FileDrag::maybeClaim();
    if (FileDrag::ownsInput()) return;
  }
  moveDraggedWindow();
  POINT point{};
  if (!GetCursorPos(&point)) return;
  auto *target = windowDragTarget(point);
  Json motion = {{"type", "pointer"}, {"id", 0}, {"x", 0}, {"y", 0}};
  windowDrag.target = target ? target->id : 0;
  if (target) {
    ScreenToClient(target->hwnd, &point);
    RECT r = imageRect(target);
    motion["id"] = target->id;
    motion["x"] = double(point.x - r.left) * target->width / (r.right - r.left);
    motion["y"] = double(point.y - r.top) * target->height / (r.bottom - r.top);
    motion["origin_x"] = target->originX;
    motion["origin_y"] = target->originY;
  }
  sendJson(motion);
  if (release && FileDrag::waitForReturn()) return;
  if (release) {
    motion["type"] = "button";
    motion["button"] = 0x110;
    motion["down"] = false;
    sendJson(motion);
    windowDrag.released = true;
    dragVisual.reset();
  }
}
void cancelWindowDrag() {
  dragVisual.reset();
  if (!windowDrag.active || windowDrag.cancelled) return;
  windowDrag.cancelled = true;
  windowDrag.released = true;
  sendJson({{"type", "toplevel_drag_cancel"}});
}
void endWindowDrag() {
  if (!windowDrag.active) return;
  auto target = windowDrag.attached ? windowDrag.attached : windowDrag.target;
  bool cancelled = windowDrag.cancelled;
  windowDrag = {};
  KillTimer(controller, WindowDragTimer);
  if (GetCapture() == controller) ReleaseCapture();
  auto found = windows.find(target);
  if (!cancelled && found != windows.end() && IsWindowVisible(found->second->hwnd))
    SetForegroundWindow(found->second->hwnd);
}
uint32_t linuxKey(LPARAM param, UINT virtualKey = 0) {
  uint32_t code = (param >> 16) & 0xff;
  bool extended = (param & (1LL << 24)) != 0;
  if (!code && virtualKey && virtualKey != VK_PROCESSKEY && virtualKey != VK_PACKET) {
    UINT scan = MapVirtualKeyExW(virtualKey, MAPVK_VK_TO_VSC_EX, GetKeyboardLayout(0));
    code = scan & 0xff;
    extended |= (scan & 0xff00) == 0xe000;
  }
  if (!extended)
    return code;
  switch (code) {
  case 0x1c:
    return 96;
  case 0x1d:
    return 97;
  case 0x35:
    return 98;
  case 0x37:
    return 99;
  case 0x38:
    return 100;
  case 0x47:
    return 102;
  case 0x48:
    return 103;
  case 0x49:
    return 104;
  case 0x4b:
    return 105;
  case 0x4d:
    return 106;
  case 0x4f:
    return 107;
  case 0x50:
    return 108;
  case 0x51:
    return 109;
  case 0x52:
    return 110;
  case 0x53:
    return 111;
  case 0x5b:
    return 125;
  case 0x5c:
    return 126;
  case 0x5d:
    return 127;
  default:
    return 0;
  }
}
void positionPopup(RemoteWindow *w) {
  auto root = windows.find(w->root);
  if (root == windows.end())
    return;
  w->dpi = root->second->dpi;
  POINT origin{};
  ClientToScreen(root->second->hwnd, &origin);
  SetWindowPos(w->hwnd, nullptr, origin.x + MulDiv(w->x, w->dpi, 96),
               origin.y + MulDiv(w->y, w->dpi, 96), std::max(1, MulDiv(w->width, w->dpi, 96)),
               std::max(1, MulDiv(w->height, w->dpi, 96)), SWP_NOACTIVATE | SWP_NOZORDER);
}

void setPopupGrab(uint32_t root, bool active) {
  if (active) {
    grabbedRoot = root;
    auto found = windows.find(root);
    if (found != windows.end() && found->second->shown)
      SetCapture(found->second->hwnd);
  } else if (grabbedRoot == root) {
    grabbedRoot = 0;
    ReleaseCapture();
  }
}

void sendColorMode(RemoteWindow *w) {
  bool capable = supportedVideoKinds.count(7) || supportedVideoKinds.count(8);
  bool active = capable && w->presentation && HdrCapabilities::active(w->hwnd);
  if (w->lastHdr != int(active)) {
    w->lastHdr = int(active);
    sendJson({{"type", "color_mode"},
              {"id", w->id},
              {"hdr", active},
              {"color_space", active ? "bt2020_pq" : "bt709_sdr"}});
  }
}
void sendResize(RemoteWindow *w) {
  sendColorMode(w);
  if (w->popup || IsIconic(w->hwnd) ||
      (semanticsNegotiated && (!w->state.metadataReady || w->state.applying)))
    return;
  RECT rect;
  GetClientRect(w->hwnd, &rect);
  bool maximized = !w->state.fullscreen && IsZoomed(w->hwnd) != FALSE;
  MONITORINFO monitor{};
  monitor.cbSize = sizeof(monitor);
  GetMonitorInfoW(MonitorFromWindow(w->hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
  int workWidth = monitor.rcWork.right - monitor.rcWork.left,
      workHeight = monitor.rcWork.bottom - monitor.rcWork.top;
  POINT origin{};
  ClientToScreen(w->hwnd, &origin);
  int workX = MulDiv(monitor.rcWork.left - origin.x, 96, w->dpi),
      workY = MulDiv(monitor.rcWork.top - origin.y, 96, w->dpi);
  bool resized =
      w->lastResizeWidth != rect.right || w->lastResizeHeight != rect.bottom ||
      w->lastMaximized != maximized || w->lastDpi != w->dpi ||
      (semanticsNegotiated && (w->state.lastFullscreen != w->state.fullscreen ||
                               w->state.lastFullscreenEpoch != w->state.fullscreenEpoch));
  if (rect.right <= 0 || rect.bottom <= 0 ||
      (!resized && w->lastWorkWidth == workWidth && w->lastWorkHeight == workHeight &&
       w->lastWorkX == workX && w->lastWorkY == workY))
    return;
  w->lastWorkX = workX;
  w->lastWorkY = workY;
  w->lastResizeWidth = rect.right;
  w->lastResizeHeight = rect.bottom;
  w->lastMaximized = maximized;
  w->state.lastFullscreen = w->state.fullscreen;
  w->state.lastFullscreenEpoch = w->state.fullscreenEpoch;
  w->lastDpi = w->dpi;
  w->lastWorkWidth = workWidth;
  w->lastWorkHeight = workHeight;
  if (resized) {
    w->resizePending = true;
    w->resizeSent = Clock::now();
  }
  sendJson({{"type", resized ? "resize" : "viewport"},
            {"id", w->id},
            {"width", MulDiv(rect.right, 96, w->dpi)},
            {"height", MulDiv(rect.bottom, 96, w->dpi)},
            {"dpi", w->dpi},
            {"work_width", workWidth},
            {"work_height", workHeight},
            {"work_x", workX},
            {"work_y", workY},
            {"maximized", maximized},
            {"fullscreen", w->state.fullscreen},
            {"fullscreen_epoch", w->state.fullscreenEpoch}});
}
void acknowledge(RemoteWindow *w) {
  if (w->frame && w->frame->sequence > w->lastAck) {
    w->lastAck = w->frame->sequence;
    sendJson({{"type", "frame_ack"}, {"id", w->id}, {"sequence", w->lastAck}});
  }
}

uint32_t cursorRoot(RemoteWindow *w) { return w->popup ? w->root : w->id; }
void refreshCursor() {
  if (relativeInput.active()) { SetCursor(nullptr); return; }
  POINT point{};
  if (!cursorNegotiated) { SetCursor(LoadCursorW(nullptr, IDC_ARROW)); return; }
  if (!GetCursorPos(&point)) return;
  HWND under = WindowFromPoint(point);
  for (auto &[id, w] : windows) {
    if (w->hwnd == under && SendMessageW(under, WM_NCHITTEST, 0, MAKELPARAM(point.x, point.y)) == HTCLIENT) {
      SetCursor(nativeCursor.handle(cursorRoot(w.get()), w->dpi));
      break;
    }
  }
}
void sendWindowState(RemoteWindow *w) {
  if (!semanticsNegotiated || w->popup || !w->shown || w->state.applying)
    return;
  bool minimized = IsIconic(w->hwnd) != FALSE;
  bool visible = IsWindowVisible(w->hwnd) && !minimized && w->state.mapped;
  if (w->state.stateSent && w->state.lastVisible == visible && w->state.lastMinimized == minimized)
    return;
  w->state.stateSent = true;
  w->state.lastVisible = visible;
  w->state.lastMinimized = minimized;
  sendJson({{"type", "window_state"},
            {"id", w->id},
            {"visible", visible},
            {"minimized", minimized},
            {"mapping_epoch", w->state.mappingEpoch}});
}
void setFullscreen(RemoteWindow *w, bool fullscreen) {
  w->state.setFullscreen(w->hwnd, fullscreen, w->clientDecorated);
  sendResize(w);
  sendWindowState(w);
}
void reconcileWindows() {
  std::set<uint32_t> blocked;
  HWND foreground = GetForegroundWindow(), modalFocus = nullptr;
  for (auto &[id, w] : windows) {
    if (w->popup)
      continue;
    uint32_t owner = w->parent;
    std::set<uint32_t> seen{id};
    for (uint32_t current = owner; current;) {
      if (!seen.insert(current).second) {
        owner = 0;
        break;
      }
      auto found = windows.find(current);
      if (found == windows.end() || found->second->popup) {
        if (current == owner)
          owner = 0;
        break;
      }
      current = found->second->parent;
    }
    HWND hwnd = owner && windows.count(owner) ? windows.at(owner)->hwnd : nullptr;
    if (GetWindow(w->hwnd, GW_OWNER) != hwnd)
      SetWindowLongPtrW(w->hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(hwnd));
    auto style = GetWindowLongPtrW(w->hwnd, GWL_EXSTYLE);
    auto updated = hwnd ? style & ~WS_EX_APPWINDOW : style | WS_EX_APPWINDOW;
    if (style != updated) {
      SetWindowLongPtrW(w->hwnd, GWL_EXSTYLE, updated);
      SetWindowPos(w->hwnd, nullptr, 0, 0, 0, 0,
                   SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
    }
    if (owner && w->state.modal && w->state.mapped && w->shown) {
      blocked.insert(owner);
      if (hwnd == foreground)
        modalFocus = w->hwnd;
    }
  }
  for (auto &[id, w] : windows)
    if (!w->popup && IsWindowEnabled(w->hwnd) == (blocked.count(id) != 0))
      EnableWindow(w->hwnd, !blocked.count(id));
  if (modalFocus && IsWindowEnabled(modalFocus))
    SetForegroundWindow(modalFocus);
}
bool acceptsInput(RemoteWindow *w, LPARAM lp) {
  POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
  ScreenToClient(w->hwnd, &point);
  RECT r = imageRect(w);
  if (r.right <= r.left || r.bottom <= r.top)
    return true;
  double x = double(point.x - r.left) * w->width / (r.right - r.left),
         y = double(point.y - r.top) * w->height / (r.bottom - r.top);
  return w->state.accepts(x, y, w->originX, w->originY, w->width, w->height);
}
bool imeOleSuspending = false;
void suspendImeForOle();
void updateIme() {
  if (FileDrag::ownsInput()) {
    suspendImeForOle();
    return;
  }
  RemoteWindow *target = nullptr, *host = nullptr;
  if (hostedIme && !hostedReadOnly && imeStatus.is_object() && imeStatus.value("enabled", false)) {
    auto found = windows.find(imeStatus.value("id", 0u));
    if (found != windows.end()) {
      target = found->second.get();
      host = target;
      if (target->popup) {
        auto root = windows.find(target->root);
        host = root == windows.end() ? nullptr : root->second.get();
      }
    }
  }
  uint64_t generation = imeStatus.is_object() ? imeStatus.value("generation", uint64_t(0)) : 0;
  for (auto &[id, window] : windows) {
    if (!window->popup)
      window->ime.associate(window->hwnd, window.get() == host && GetFocus() == window->hwnd,
                            target ? target->id : 0, generation);
  }
  if (!host || !target || !host->ime.enabled || !imeStatus.contains("rectangle")) return;
  const auto &caret = imeStatus.at("rectangle");
  if (!caret.is_array() || caret.size() != 4) return;
  RECT image = imageRect(target);
  double sx = double(image.right - image.left) / std::max(1, target->width);
  double sy = double(image.bottom - image.top) / std::max(1, target->height);
  RECT rect{LONG(image.left + (caret[0].get<int>() - target->originX) * sx),
            LONG(image.top + (caret[1].get<int>() - target->originY) * sy), 0, 0};
  rect.right = rect.left + std::max(1L, LONG(caret[2].get<int>() * sx));
  rect.bottom = rect.top + std::max(1L, LONG(caret[3].get<int>() * sy));
  if (target != host) MapWindowPoints(target->hwnd, host->hwnd, reinterpret_cast<POINT *>(&rect), 2);
  host->ime.caret(host->hwnd, rect);
}

void sendIme(RemoteWindow *window, const std::optional<std::wstring> &commit,
             const std::optional<std::wstring> &preedit, LONG cursor = 0) {
  auto &ime = window->ime;
  if (!ime.enabled) return;
  Json packet{{"type", "ime"}, {"id", ime.target},
              {"generation", ime.composing ? ime.compositionGeneration : ime.generation}};
  if (commit) packet["commit"] = utf8(*commit);
  if (preedit) {
    size_t pos = size_t(std::clamp<LONG>(cursor, 0, LONG(preedit->size())));
    if (pos && pos < preedit->size() && (*preedit)[pos] >= 0xdc00 && (*preedit)[pos] <= 0xdfff &&
        (*preedit)[pos-1] >= 0xd800 && (*preedit)[pos-1] <= 0xdbff) --pos;
    packet["preedit"] = utf8(*preedit);
    packet["cursor_begin"] = packet["cursor_end"] = utf8(preedit->substr(0, pos)).size();
  }
  sendJson(packet);
}

void synchronizeImeModifiers(RemoteWindow *window, uint32_t except = 0) {
  auto &ime = window->ime;
  if (FileDrag::ownsInput() || ime.composing || ime.cancelling || GetFocus() != window->hwnd) return;
  if (ime.enabled && (!imeStatus.value("enabled", false) ||
                      imeStatus.value("generation", uint64_t(0)) != ime.generation ||
                      imeStatus.value("id", 0u) != ime.target)) return;
  static const std::pair<int, uint32_t> modifiers[] = {
      {VK_LSHIFT, 42}, {VK_RSHIFT, 54}, {VK_LCONTROL, 29}, {VK_RCONTROL, 97},
      {VK_LMENU, 56}, {VK_RMENU, 100}, {VK_LWIN, 125}, {VK_RWIN, 126}};
  for (const auto &[native, key] : modifiers) {
    if (key == except) continue;
    bool held = (GetKeyState(native) & 0x8000) != 0;
    bool forwarded = ime.forwardedKeys.count(key) != 0;
    if (held == forwarded) continue;
    if (held) ime.forwardedKeys.insert(key);
    else ime.forwardedKeys.erase(key);
    sendJson({{"type", "key"}, {"id", window->id}, {"key", key}, {"down", held}});
  }
}

// OLE owns keyboard processing while its native drag loop runs. Cancel the
// remote preedit and forget raw routes before entering that nested pump.
void suspendImeForOle() {
  if (imeOleSuspending) return;
  imeOleSuspending = true;
  for (auto &[id, window] : windows) {
    auto &ime = window->ime;
    if (ime.composing) sendIme(window.get(), {}, std::wstring());
    ime.cancel(window->hwnd);
    for (auto key : ime.forwardedKeys)
      sendJson({{"type", "key"}, {"id", id}, {"key", key}, {"down", false}});
    ime.forwardedKeys.clear();
    ime.rawCharacterKeys.clear();
    ime.compositionGeneration = 0;
    if (!window->popup) ime.associate(window->hwnd, false, 0, 0);
  }
  DestroyCaret();
  imeOleSuspending = false;
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
  if (hostedReadOnly && (message == WM_INPUT || (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ||
      (message >= WM_KEYFIRST && message <= WM_KEYLAST) ||
      message == WM_IME_STARTCOMPOSITION || message == WM_IME_COMPOSITION ||
      message == WM_IME_ENDCOMPOSITION)) return 0;
  auto w = reinterpret_cast<RemoteWindow *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    w = static_cast<RemoteWindow *>(reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams);
    w->hwnd = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(w));
  }
  if (!w)
    return DefWindowProcW(hwnd, message, wp, lp);
  if (GetEnvironmentVariableW(L"WAYBRIDGE_IME_TRACE", nullptr, 0) &&
      (message == WM_KEYDOWN || message == WM_KEYUP || message == WM_CHAR ||
       message == WM_IME_STARTCOMPOSITION || message == WM_IME_ENDCOMPOSITION ||
       message == WM_IME_COMPOSITION))
    std::cerr << "IME event window=" << w->id << " message=" << message
              << " key=" << ((message == WM_KEYDOWN || message == WM_KEYUP ||
                                (message == WM_CHAR && wp < 32)) ? wp : 0)
              << " flags=" << lp << " composing=" << w->ime.composing
              << " enabled=" << w->ime.enabled << " generation=" << w->ime.generation << '\n';
  switch (message) {
  case WM_INPUT: {
    LONG dx = 0, dy = 0;
    if (relativeInput.active(hwnd) && relativeInput.motion(reinterpret_cast<HRAWINPUT>(lp), dx, dy))
      sendJson({{"type", "relative_pointer"}, {"id", relativeInput.id()},
                {"generation", relativeInput.generation()}, {"dx", dx}, {"dy", dy}});
    // Foreground Raw Input requires DefWindowProc to release system resources.
    return DefWindowProcW(hwnd, message, wp, lp);
  }
  case WM_SETCURSOR:
    if (relativeInput.active()) {
      SetCursor(nullptr);
      return TRUE;
    }
    if (cursorNegotiated && LOWORD(lp) == HTCLIENT) {
      SetCursor(nativeCursor.handle(cursorRoot(w), w->dpi));
      return TRUE;
    }
    break;
  case WM_ACTIVATE:
    if (LOWORD(wp) == WA_INACTIVE && relativeInput.active(hwnd))
      releasePointer();
    break;
  case WM_CANCELMODE:
    if (relativeInput.active(hwnd))
      releasePointer();
    break;
  case WM_MOUSEACTIVATE:
    if (w->popup)
      return MA_NOACTIVATE;
    break;
  case WM_CAPTURECHANGED:
    if (grabbedRoot == w->id && reinterpret_cast<HWND>(lp) != hwnd) {
      grabbedRoot = 0;
      sendJson({{"type", "popup_cancel"}, {"id", w->id}});
    }
    break;
  case WM_WINDOWPOSCHANGING:
    if (semanticsNegotiated && !w->popup)
      w->state.clamp(hwnd, w->dpi, w->clientDecorated, reinterpret_cast<WINDOWPOS *>(lp));
    break;
  case WM_SHOWWINDOW:
    sendWindowState(w);
    break;
  case WM_SYSCOMMAND:
    if (semanticsNegotiated && w->state.fullscreen &&
        ((wp & 0xfff0) == SC_RESTORE || (wp & 0xfff0) == SC_MAXIMIZE)) {
      setFullscreen(w, false);
      ShowWindow(hwnd, (wp & 0xfff0) == SC_MAXIMIZE ? SW_MAXIMIZE : SW_RESTORE);
      sendResize(w);
      return 0;
    }
    break;
  case WM_WINDOWPOSCHANGED:
    if (relativeInput.active(hwnd))
      releasePointer();
    if (!w->popup) {
      // WM_SHOWWINDOW arrives before WS_VISIBLE changes. Observe the final
      // flags here as well, including hide/show without a size change.
      sendWindowState(w);
      for (auto &[id, popup] : windows)
        if (popup->popup && popup->root == w->id)
          positionPopup(popup.get());
      // DefWindowProc dispatches WM_SIZE after this message. Let WM_SIZE
      // coalesce drag sizes; movement alone still updates popup constraints.
      if (reinterpret_cast<WINDOWPOS *>(lp)->flags & SWP_NOSIZE)
        sendResize(w);
    }
    break;
  case WM_GETMINMAXINFO: {
    DefWindowProcW(hwnd, message, wp, lp);
    auto limits = reinterpret_cast<MINMAXINFO *>(lp);
    if (w->clientDecorated || w->state.fullscreen) {
      MONITORINFO info{};
      info.cbSize = sizeof(info);
      if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info)) {
        RECT area = w->state.fullscreen ? info.rcMonitor : info.rcWork;
        limits->ptMaxPosition = {area.left - info.rcMonitor.left, area.top - info.rcMonitor.top};
        limits->ptMaxSize = {area.right - area.left, area.bottom - area.top};
        limits->ptMinTrackSize = {64, 64};
      }
    }
    if (semanticsNegotiated)
      w->state.minMax(hwnd, w->dpi, w->clientDecorated, limits);
    return 0;
  }
  case WM_NCCALCSIZE:
    if (w->popup || w->clientDecorated || w->state.fullscreen)
      return 0;
    break;
  case WM_NCHITTEST:
    if (!w->popup && !w->clientDecorated && !w->state.fullscreen) {
      auto native = DefWindowProcW(hwnd, message, wp, lp);
      if (native != HTCLIENT)
        return native;
    }
    if (!w->popup && w->clientDecorated && !w->state.fullscreen) {
      if (!IsZoomed(hwnd)) {
        RECT r;
        GetWindowRect(hwnd, &r);
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        int edge =
            std::max(MulDiv(5, w->dpi, 96), GetSystemMetricsForDpi(SM_CXSIZEFRAME, w->dpi) +
                                                GetSystemMetricsForDpi(SM_CXPADDEDBORDER, w->dpi));
        bool left = x < r.left + edge, right = x >= r.right - edge, top = y < r.top + edge,
             bottom = y >= r.bottom - edge;
        if (top)
          return left ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP;
        if (bottom)
          return left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM;
        if (left)
          return HTLEFT;
        if (right)
          return HTRIGHT;
      }
    }
    return semanticsNegotiated && !acceptsInput(w, lp) ? HTTRANSPARENT : HTCLIENT;
  case WM_ENTERSIZEMOVE:
    if (relativeInput.active(hwnd))
      releasePointer();
    w->sizing = true;
    sendJson({{"type", "cancel_input"}, {"id", w->id}});
    return 0;
  case WM_EXITSIZEMOVE:
    w->sizing = false;
    KillTimer(hwnd, 1);
    sendResize(w);
    return 0;
  case WM_DESTROY:
    ApplicationIdentity::clear(hwnd);
    w->dropLifetime.reset();
    if (relativeInput.active(hwnd))
      releasePointer();
    RevokeDragDrop(hwnd);
    return 0;
  case WM_PAINT: {
    auto paintStarted = Clock::now();
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    if (!w->presentation && w->frame) {
      RECT client;
      GetClientRect(hwnd, &client);
      if (client.right > 0 && client.bottom > 0) {
        HDC memory = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
        auto old = SelectObject(memory, bitmap);
        FillRect(memory, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        Gdiplus::Bitmap image(w->frame->width, w->frame->height, w->frame->width * 4,
                              PixelFormat32bppPARGB, w->frame->bgra.data());
        {
          Gdiplus::Graphics g(memory);
          RECT r = imageRect(w);
          g.DrawImage(&image, int(r.left), int(r.top), int(r.right - r.left),
                      int(r.bottom - r.top));
        }
        BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
        SelectObject(memory, old);
        DeleteObject(bitmap);
        DeleteDC(memory);
      }
    }
    EndPaint(hwnd, &ps);
    if (!w->presentation) {
      clientStats.record("gdi_paint", millis(Clock::now() - paintStarted));
      acknowledge(w);
    }
    return 0;
  }
  case WM_ERASEBKGND:
    return 1;
  case WM_SETFOCUS:
    if (w->popup)
      return 0;
    sendJson({{"type", "focus"}, {"id", w->id}, {"focused", true}});
    updateIme();
    return 0;
  case WM_KILLFOCUS:
    if (relativeInput.active(hwnd))
      releasePointer();
    if (w->popup)
      return 0;
    if (w->ime.composing) sendIme(w, {}, std::wstring());
    w->ime.cancel(hwnd);
    w->ime.forwardedKeys.clear();
    DestroyCaret();
    sendJson({{"type", "focus"}, {"id", w->id}, {"focused", false}});
    return 0;
  case WM_MOVE:
    if (!w->popup)
      sendColorMode(w);
    break;
  case WM_IME_SETCONTEXT:
    return DefWindowProcW(hwnd, message, wp,
                         w->ime.enabled ? lp & ~ISC_SHOWUICOMPOSITIONWINDOW : lp);
  case WM_IME_REQUEST:
    if (w->ime.enabled && wp == IMR_QUERYCHARPOSITION && lp) {
      auto position = reinterpret_cast<IMECHARPOSITION *>(lp);
      if (position->dwSize < sizeof(*position)) return 0;
      RECT caret = w->ime.lastCaret;
      POINT point{caret.left, caret.top};
      ClientToScreen(hwnd, &point);
      position->pt = point;
      position->cLineHeight = UINT(std::max(1L, caret.bottom - caret.top));
      GetWindowRect(hwnd, &position->rcDocument);
      return TRUE;
    }
    break;
  case WM_IME_STARTCOMPOSITION:
    if (FileDrag::ownsInput()) return 0;
    if (!w->ime.enabled) return DefWindowProcW(hwnd, message, wp, lp);
    w->ime.composing = true;
    w->ime.compositionGeneration = w->ime.generation;
    for (auto key : w->ime.forwardedKeys)
      sendJson({{"type", "key"}, {"id", w->id}, {"key", key}, {"down", false}});
    w->ime.forwardedKeys.clear();
    w->ime.rawCharacterKeys.clear();
    updateIme();
    return 0;
  case WM_IME_COMPOSITION:
    if (FileDrag::ownsInput()) return 0;
    if (w->ime.enabled) {
      if (auto context = ImmGetContext(hwnd)) {
        std::optional<std::wstring> result, preedit;
        LONG cursor = 0;
        if (lp & GCS_RESULTSTR) { result = NativeIme::string(context, GCS_RESULTSTR); preedit = L""; }
        if (lp & (GCS_COMPSTR | GCS_CURSORPOS)) {
          preedit = NativeIme::string(context, GCS_COMPSTR);
          cursor = ImmGetCompositionStringW(context, GCS_CURSORPOS, nullptr, 0);
          if (cursor < 0 && preedit) cursor = LONG(preedit->size());
        }
        constexpr LPARAM compositionFlags = GCS_COMPSTR | GCS_COMPATTR | GCS_COMPCLAUSE |
            GCS_COMPREADSTR | GCS_COMPREADATTR | GCS_COMPREADCLAUSE | GCS_CURSORPOS |
            GCS_DELTASTART | GCS_RESULTSTR | GCS_RESULTCLAUSE | GCS_RESULTREADSTR |
            GCS_RESULTREADCLAUSE;
        if (!(lp & compositionFlags)) preedit = L"";
        ImmReleaseContext(hwnd, context);
        sendIme(w, result, preedit, cursor);
        // A result, an empty preedit, or cancellation can complete composition
        // before (or without) WM_IME_ENDCOMPOSITION. Do not keep swallowing
        // Enter/Backspace until an unrelated focus change resets this flag.
        if (preedit) {
          bool wasComposing = w->ime.composing;
          if (!wasComposing && !preedit->empty())
            w->ime.compositionGeneration = w->ime.generation;
          w->ime.composing = !preedit->empty();
          if (wasComposing && !w->ime.composing) synchronizeImeModifiers(w);
        }
      }
      return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
  case WM_IME_ENDCOMPOSITION: {
    bool current = w->ime.composing && w->ime.compositionGeneration == w->ime.generation;
    if (current) sendIme(w, {}, std::wstring());
    w->ime.composing = false;
    if (current) synchronizeImeModifiers(w);
    return 0;
  }
  case WM_IME_CHAR:
    return 0; // Result strings are committed exactly once through WM_IME_COMPOSITION.
  case WM_CHAR:
    if (FileDrag::ownsInput()) return 0;
    if (w->ime.enabled && !w->ime.composing) {
      UINT virtualKey = wp == L'\b' ? VK_BACK : wp == L'\r' ? VK_RETURN : wp == L'\t' ? VK_TAB : 0;
      if (!virtualKey && wp >= 32 && wp != 127) {
        SHORT mapped = VkKeyScanExW(wchar_t(wp), GetKeyboardLayout(0));
        if (mapped != -1) virtualKey = UINT(mapped) & 0xff;
      }
      uint32_t key = linuxKey(lp, virtualKey);
      if (key && w->ime.rawCharacterKeys.count(key)) return 0;
      if (wp == L'\b' || wp == L'\r' || wp == L'\t') {
        // Ctrl+H/I/M also translate to these characters. Their shortcut was
        // already forwarded, even when WM_CHAR carries no physical scan code.
        if (w->ime.shortcut(virtualKey, lp)) return 0;
        // An IME may translate VK_PROCESSKEY into a control WM_CHAR. Send
        // balanced raw events only if the physical key route did not send it.
        if (key) {
          synchronizeImeModifiers(w);
          sendJson({{"type", "key"}, {"id", w->id}, {"key", key}, {"down", true}});
          sendJson({{"type", "key"}, {"id", w->id}, {"key", key}, {"down", false}});
        }
      } else if (wp >= 32 && wp != 127) {
        if (auto text = w->ime.character(wchar_t(wp))) sendIme(w, text, std::wstring());
      }
    }
    return 0;
  case WM_DISPLAYCHANGE:
  case WM_SETTINGCHANGE:
    sendResize(w);
    return 0;
  case WM_DPICHANGED: {
    if (w->popup) {
      positionPopup(w);
      return 0;
    }
    w->dpi = effectiveDpi(hwnd);
    w->icon.apply(hwnd, w->dpi);
    auto r = reinterpret_cast<const RECT *>(lp);
    RECT fullscreenRect{};
    if (w->state.fullscreen) {
      MONITORINFO monitor{};
      monitor.cbSize = sizeof(monitor);
      if (GetMonitorInfoW(MonitorFromRect(r, MONITOR_DEFAULTTONEAREST), &monitor)) {
        fullscreenRect = monitor.rcMonitor;
        r = &fullscreenRect;
      }
    }
    SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    sendResize(w);
    return 0;
  }
  case WM_SIZE:
    sendWindowState(w);
    if (relativeInput.active(hwnd))
      releasePointer();
    if (w->presentation)
      w->presentation->layout(imageRect(w));
    if (!w->popup && wp != SIZE_MINIMIZED) {
      if (w->sizing)
        SetTimer(hwnd, 1, 120, nullptr);
      else {
        KillTimer(hwnd, 1);
        sendResize(w);
      }
    }
    updateIme();
    return 0;
  case WM_TIMER:
    KillTimer(hwnd, 1);
    sendResize(w);
    return 0;
  case WM_MOUSEMOVE:
    if (windowDrag.active) { sendWindowDragMotion(); return 0; }
    if (relativeInput.active(hwnd))
      return 0;
    FileDrag::maybeClaim();
    if (FileDrag::ownsInput()) {
      suspendImeForOle();
      return 0;
    }
    pointer(w, lp);
    refreshCursor();
    if (!w->tracking) {
      TRACKMOUSEEVENT e{sizeof(e), TME_LEAVE, hwnd, 0};
      TrackMouseEvent(&e);
      w->tracking = true;
    }
    return 0;
  case WM_MOUSELEAVE:
    w->tracking = false;
    if (relativeInput.active(hwnd))
      return 0;
    // A captured root may receive this after a motion has already entered an
    // overflowing popup. Captured motion supplies the real Wayland hit test;
    // forwarding the stale leave can clear the pressed menu item before up.
    if (grabbedRoot || windowDrag.active) return 0;
    sendJson({{"type", "leave"}, {"id", w->id}});
    return 0;
  case WM_DROPFILES: {
    auto drop = reinterpret_cast<HDROP>(wp);
    POINT position{};
    DragQueryPoint(drop, &position);
    std::vector<std::wstring> paths;
    UINT count = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
    if (!count || count > FileTree::MaxRoots) {
      DragFinish(drop); std::cerr << "Too many file roots; transfer rejected\n"; return 0;
    }
    for (UINT i = 0; i < count; ++i) {
      UINT n = DragQueryFileW(drop, i, nullptr, 0);
      std::wstring path(n + 1, L'\0');
      DragQueryFileW(drop, i, path.data(), n + 1);
      path.resize(n);
      paths.push_back(std::move(path));
    }
    DragFinish(drop);
    RECT r = imageRect(w);
    if (r.right > r.left && r.bottom > r.top)
      uploadFiles(std::move(paths),
                  {{"drop", true},
                   {"id", w->id},
                   {"origin_x", w->originX},
                   {"origin_y", w->originY},
                   {"x", double(position.x - r.left) * w->width / (r.right - r.left)},
                   {"y", double(position.y - r.top) * w->height / (r.bottom - r.top)}});
    return 0;
  }
  case WM_LBUTTONDOWN:
  case WM_RBUTTONDOWN:
  case WM_MBUTTONDOWN:
  case WM_LBUTTONUP:
  case WM_RBUTTONUP:
  case WM_MBUTTONUP: {
    if (windowDrag.active) {
      if (message == WM_LBUTTONUP) sendWindowDragMotion(true);
      return 0;
    }
    if (FileDrag::ownsInput()) {
      if (message == WM_LBUTTONUP) {
        FileDrag::releasedBeforeStart();
        updateIme();
      }
      return 0;
    }
    bool down = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN;
    uint32_t button = (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP)   ? 0x110
                      : (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP) ? 0x111
                                                                               : 0x112;
    if (down) {
      if (w->ime.composing) {
        sendIme(w, {}, std::wstring());
        w->ime.cancel(hwnd);
      }
      if (!w->popup)
        SetFocus(hwnd);
      if (!grabbedRoot && !relativeInput.active())
        SetCapture(hwnd);
    } else if (!grabbedRoot && !relativeInput.active())
      ReleaseCapture();
    pointer(w, lp);
    sendJson({{"type", "button"}, {"id", w->id}, {"button", button}, {"down", down}});
    return 0;
  }
  case WM_MOUSEWHEEL:
  case WM_MOUSEHWHEEL: {
    double delta = -double(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA * 15;
    sendJson({{"type", "scroll"},
              {"id", w->id},
              {"x", message == WM_MOUSEHWHEEL ? delta : 0},
              {"y", message == WM_MOUSEWHEEL ? delta : 0}});
    return 0;
  }
  case WM_KEYDOWN:
    if (windowDrag.active && wp == VK_ESCAPE) { cancelWindowDrag(); return 0; }
    if (wp == VK_ESCAPE && VirtualFiles::synchronousWaits) {
      VirtualFiles::cancelDrops();
      return 0;
    }
    [[fallthrough]];
  case WM_KEYUP:
  case WM_SYSKEYDOWN:
  case WM_SYSKEYUP:
    if (relativeInput.active(hwnd) && wp == VK_F12 &&
        (GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_MENU) & 0x8000) &&
        (GetKeyState(VK_SHIFT) & 0x8000)) {
      releasePointer();
      return 0;
    }
    if (FileDrag::ownsInput()) return 0;
    if (auto key = linuxKey(lp, UINT(wp))) {
      bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
      if (down) {
        w->ime.rawCharacterKeys.erase(key);
        if (wp == VK_PROCESSKEY || wp == VK_PACKET || w->ime.composing ||
            (w->ime.enabled && NativeIme::printable(wp) && !w->ime.shortcut(wp, lp))) return 0;
        synchronizeImeModifiers(w, key);
        w->ime.forwardedKeys.insert(key);
        w->ime.rawCharacterKeys.insert(key);
      } else if (!w->ime.forwardedKeys.erase(key)) return 0;
      sendJson({{"type", "key"}, {"id", w->id}, {"key", key}, {"down", down}});
    }
    return 0;
  case WM_CLOSE:
    if (relativeInput.active(hwnd))
      releasePointer();
    sendJson({{"type", "close"}, {"id", w->id}});
    return 0;
  }
  return DefWindowProcW(hwnd, message, wp, lp);
}

void destroyRemote(uint32_t id) {
  nativeActivation.closed(id);
  std::vector<uint32_t> children;
  for (auto &[child, w] : windows)
    if (w->popup && w->parent == id)
      children.push_back(child);
  for (auto child : children)
    destroyRemote(child);
  auto found = windows.find(id);
  if (found == windows.end())
    return;
  found->second->dropLifetime.reset();
  if (grabbedRoot == id)
    setPopupGrab(id, false);
  HWND oldOwner = GetWindow(found->second->hwnd, GW_OWNER);
  bool restoreFocus = GetForegroundWindow() == found->second->hwnd;
  // Owned HWNDs otherwise die with their owner even if their Wayland toplevel survives.
  for (auto &[child, w] : windows)
    if (!w->popup && w->parent == id) {
      w->parent = 0;
      SetWindowLongPtrW(w->hwnd, GWLP_HWNDPARENT, 0);
    }
  found->second->state.modal = false;
  reconcileWindows();
  if (restoreFocus) NativeActivation::restoreOwner(found->second->hwnd, oldOwner);
  DestroyWindow(found->second->hwnd);
  windows.erase(found);
  reconcileWindows();
}

void dispatchNetwork() {
  std::deque<Json> pending;
  Json cursor;
  Json visual;
  std::map<uint32_t, std::shared_ptr<DecodedFrame>> pendingFrames;
  {
    std::lock_guard<std::mutex> guard(inboxMutex);
    pending.swap(controls);
    cursor.swap(pendingCursor);
    visual.swap(pendingDragVisual);
    pendingFrames.swap(decodedFrames);
  }
  inboxWake.notify_all();
  for (auto &m : pending) {
    std::string type = m.value("type", "");
    HWND sourceWindow = nullptr;
    if (type == "drag_offer") {
      auto found = windows.find(m.value("id", uint32_t(0)));
      if (found != windows.end()) sourceWindow = found->second->hwnd;
    }
    if ((type == "files_begin" || type == "tree_begin" || type == "files_done" || type == "files_error" ||
         type == "upload_ready" || type == "upload_rejected" || type == "tree_done") && hostControl.active()) {
      Json event = {{"type","transfer"},{"event",type}};
      for (const auto *key : {"id","transfer","batch","name","size","total","message"})
        if (m.contains(key)) event[key] = m[key];
      hostControl.emit(event);
    }
    if ((type == "clipboard" && (hostedClipboard=="off" || hostedClipboard=="send")) ||
        ((!hostedFiles || hostedClipboard=="off" || hostedClipboard=="send") &&
                        (type == "files_begin" || type == "file_chunk" || type == "files_done" ||
                         type == "tree_begin" || type == "tree_entry" || type == "tree_chunk" ||
                         type == "tree_done")) || (!hostedFiles && type=="drag_offer" && !m.value("text",Json()).is_string())) continue;
    if(type=="drag_offer" && m.value("text",Json()).is_string() && !hostedDragText){sendJson({{"type","drag_result"},{"drag",m.value("drag",uint64_t(0))},{"success",false}});continue;}
    if (FileTree::receive(m) || FileDrag::control(m, controller, sourceWindow)) continue;
    if (TextDrag::control(m) || remoteNotifications.receive(m)) continue;
    if (type.rfind("tray_",0)==0) {
      if (!iconDecoder) iconDecoder=std::make_unique<ImageDecoder>();
      if (remoteTray.receive(m,*iconDecoder)) continue;
    }
    if (type == "hello") {
      dragVisual.configure(m.value("data_drag_v1", false));
      nativeActivation.hello(m.value("activation_v1", false));
      std::cout << "Connected, protocol " << m.value("version", 0) << '\n';
      if (m.value("version", 0) != 3 || m.value("codec", "") != "mixed") {
        std::cerr << "Incompatible server; update both ends to 0.3.0\n";
        PostQuitMessage(1);
        return;
      }
      if (m.value("audio_pcm", false))
        startAudio(m.at("media_key").get<std::string>());
      if (m.contains("udp_fec_v1"))
        udpMedia.start(m["udp_fec_v1"], queueMedia, sendJson);
      semanticsNegotiated = m.value("window_semantics_v1", false);
      cursorNegotiated = m.value("cursor_v1", false);
      iconsNegotiated = m.value("window_icon_v1", false);
      dataDragNegotiated = m.value("data_drag_v1", false);
      TextDrag::configure(m.value("data_drag_result_v1",false));
      trayNegotiated = m.value("tray_v1", false);
      remoteTray.configure(trayNegotiated,hostedSettings.value("tray",true),hostedReadOnly);
      if (!trayNegotiated) hostControl.emit({{"type","tray_status"},{"available",false},{"message","Linux 服务端未协商 tray_v1，请更新服务端"},{"xembed",false}});
      if (m.contains("features") && m["features"].is_array())
      {
        FileTree::supported = std::find(m["features"].begin(), m["features"].end(), "file_tree") != m["features"].end();
        streamConfigNegotiated = std::find(m["features"].begin(),m["features"].end(),"stream_config_v1") != m["features"].end();
      }
      if (m.value("split_media", false))
        startMedia(m.at("media_key").get<std::string>());
      publishClipboard();
      hostControl.emit({{"type","connected"},{"version",3},{"features",m.value("features",Json::array())},
                        {"activation_v1",nativeActivation.enabled()},
                        {"hardware_codecs",supportedVideoKinds},{"stream_config",streamConfigNegotiated}});
      remoteNotifications.hello(m);
      if (hostControl.active()) sendStreamConfiguration();
    } else if (type == "activation_prepared") {
      hostControl.emit(m);
    } else if (type == "window_activate") {
      auto target = windows.find(m.value("id", 0u));
      auto source = windows.find(m.value("origin", Json()).is_number_unsigned() ? m["origin"].get<uint32_t>() : 0u);
      if (target != windows.end() && !target->second->popup)
        nativeActivation.request(m, target->second->hwnd, source != windows.end() ? source->second->hwnd : nullptr, target->second->shown);
    } else if (type == "stream_config") {
      hostControl.emit(m);
    } else if (type == "toplevel_drag") {
      if (!m.value("active", false)) { endWindowDrag(); continue; }
      if (!windowDrag.active) {
        releasePointer();
        windowDrag = {};
        windowDrag.active = true;
        SetCapture(controller);
        SetTimer(controller, WindowDragTimer, 10, nullptr);
      }
      windowDrag.source = m.value("source", 0u);
      windowDrag.attached = m.value("id", 0u);
      windowDrag.offsetX = m.value("offset_x", 0);
      windowDrag.offsetY = m.value("offset_y", 0);
      sendWindowDragMotion(!(GetAsyncKeyState(VK_LBUTTON) & 0x8000));
    } else if (type == "window_icon" && iconsNegotiated) {
      if (!m.contains("id") || !m.at("id").is_number_unsigned() || m.at("id").get<uint64_t>() > UINT32_MAX) continue;
      auto found = windows.find(m.at("id").get<uint32_t>());
      if (found == windows.end() || found->second->popup) continue;
      auto *w = found->second.get();
      if (!NativeIcon::matches(m, w->id, w->appId, w->iconRevision)) continue;
      if (m.at("png").is_null()) w->icon.clear();
      else {
        auto png = unbase64(m.at("png").get_ref<const std::string &>());
        if (!iconDecoder) iconDecoder = std::make_unique<ImageDecoder>();
        if (!w->icon.update(png, *iconDecoder)) continue;
      }
      w->icon.apply(w->hwnd, w->dpi);
    } else if (type == "window_visibility" && semanticsNegotiated) {
      auto found = windows.find(m.value("id", 0u));
      if (found == windows.end() || found->second->popup)
        continue;
      auto *w = found->second.get();
      auto epoch = m.value("mapping_epoch", uint64_t(0));
      if (epoch < w->state.mappingEpoch)
        continue;
      w->state.mappingEpoch = epoch;
      w->state.mapped = m.value("mapped", true);
      w->state.stateSent = false;
      if (!w->state.mapped) {
        nativeActivation.closed(w->id);
        // Unmapping usually precedes destruction. Re-enable and restore the
        // owner before hiding its foreground modal, while we still own focus.
        reconcileWindows();
        NativeActivation::restoreOwner(w->hwnd, GetWindow(w->hwnd, GW_OWNER));
        ShowWindow(w->hwnd, SW_HIDE);
        auto fullscreenEpoch = m.value("fullscreen_epoch", w->state.fullscreenEpoch);
        if (fullscreenEpoch >= w->state.fullscreenEpoch) {
          w->state.fullscreenEpoch = fullscreenEpoch;
          setFullscreen(w, m.value("fullscreen", false));
        }
      } else if (w->shown)
        ShowWindow(w->hwnd, SW_SHOWNOACTIVATE);
      reconcileWindows();
      sendWindowState(w);
    } else if (type == "ime_state") {
      imeStatus = m;
    } else if (type == "pong") {
      clientStats.record("control_rtt", double(GetTickCount64() - m.at("nonce").get<uint64_t>()));
    } else if (type == "window") {
      uint32_t id = m.at("id").get<uint32_t>();
      if (!windows.count(id)) {
        if (windows.size() >= 128)
          continue;
        auto w = std::make_unique<RemoteWindow>();
        w->id = id;
        w->width = m.value("width", 960);
        w->height = m.value("height", 640);
        HWND owner = nullptr;
        if (m.contains("parent") && m["parent"].is_number_unsigned() &&
            windows.count(m["parent"].get<uint32_t>()))
          owner = windows.at(m["parent"].get<uint32_t>())->hwnd;
        w->dpi = effectiveDpi(owner);
        w->clientDecorated = m.value("decoration", "server") == "client";
        DWORD style = windowStyle(w->clientDecorated);
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        POINT point{100, 100};
        if (owner) {
          RECT parentRect;
          GetWindowRect(owner, &parentRect);
          point = {parentRect.left + 40, parentRect.top + 40};
        }
        GetMonitorInfoW(owner ? MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST)
                              : MonitorFromPoint(point, MONITOR_DEFAULTTOPRIMARY),
                        &monitor);
        int width =
            std::min(MulDiv(w->width, w->dpi, 96), int(monitor.rcWork.right - monitor.rcWork.left));
        int height =
            std::min(MulDiv(w->height, w->dpi, 96),
                     int(monitor.rcWork.bottom - monitor.rcWork.top) - MulDiv(64, w->dpi, 96));
        RECT rect{0, 0, std::max(width, 64), std::max(height, 64)};
        if (!w->clientDecorated)
          AdjustWindowRectExForDpi(&rect, style, FALSE, WS_EX_APPWINDOW, w->dpi);
        HWND hwnd = CreateWindowExW(
            WS_EX_APPWINDOW | WS_EX_NOREDIRECTIONBITMAP, L"WaybridgeRemote",
            wide(m.value("title", "Linux application")).c_str(), style,
            owner ? point.x : (w->clientDecorated ? 100 + int((id - 1) % 8) * 28 : CW_USEDEFAULT),
            owner ? point.y : (w->clientDecorated ? 100 + int((id - 1) % 8) * 28 : CW_USEDEFAULT),
            rect.right - rect.left, rect.bottom - rect.top, owner, nullptr,
            GetModuleHandleW(nullptr), w.get());
        if (!hwnd)
          continue;
        w->dpi = effectiveDpi(hwnd);
        w->presentation = std::make_unique<Presentation>();
        if (!w->presentation->initialize(hwnd)) {
          w->presentation.reset();
          SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
                            GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & ~WS_EX_NOREDIRECTIONBITMAP);
          SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
          std::cerr << "GDI fallback: buffered presentation; per-pixel transparency unavailable\n";
        } else
          std::cout << "D3D11/DirectComposition presentation active\n";
        windows[id] = std::move(w);
        auto target = new RemoteDropTarget(windows.at(id).get());
        HRESULT registration = RegisterDragDrop(hwnd, target);
        target->Release();
        if (FAILED(registration))
          std::cerr << "OLE drop registration failed\n";
        DragAcceptFiles(hwnd, TRUE);
        if (!semanticsNegotiated)
          sendResize(windows.at(id).get());
      } else {
        setDecoration(windows.at(id).get(), m.value("decoration", "server") == "client");
        SetWindowTextW(windows.at(id)->hwnd, wide(m.value("title", "Linux application")).c_str());
      }
      auto *iconWindow = windows.at(id).get();
      auto appId = NativeIcon::appId(m.value("app_id", ""));
      uint64_t revision = m.value("icon_revision", uint64_t(0));
      if (iconWindow->appId != appId || iconWindow->iconRevision != revision) {
        iconWindow->appId = std::move(appId); iconWindow->iconRevision = revision;
        iconWindow->icon.clear();
      }
      if (!iconWindow->popup) {
        auto identity = ApplicationIdentity::id(remoteApplicationSource, iconWindow->appId, id);
        if (iconWindow->taskbarIdentity != identity && ApplicationIdentity::apply(iconWindow->hwnd, remoteApplicationSource, iconWindow->appId, id))
          iconWindow->taskbarIdentity = std::move(identity);
      }
      iconWindow->icon.apply(iconWindow->hwnd, iconWindow->dpi);
      auto *updated = windows.at(id).get();
      auto parent = m.find("parent");
      updated->parent =
          parent != m.end() && parent->is_number_unsigned() ? parent->get<uint32_t>() : 0;
      if (semanticsNegotiated) {
        updated->state.constraints(m);
        updated->state.input(m.value("input_region", Json()));
        updated->state.mappingEpoch = m.value("mapping_epoch", uint64_t(1));
        updated->state.metadataReady = true;
        auto epoch = m.value("fullscreen_epoch", uint64_t(0));
        if (epoch >= updated->state.fullscreenEpoch) {
          updated->state.fullscreenEpoch = epoch;
          setFullscreen(updated, m.value("fullscreen", false));
        }
        WINDOWPOS position{};
        position.flags = SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER;
        RECT rect;
        GetWindowRect(updated->hwnd, &rect);
        position.cx = rect.right - rect.left;
        position.cy = rect.bottom - rect.top;
        auto old = position;
        updated->state.clamp(updated->hwnd, updated->dpi, updated->clientDecorated, &position);
        if (position.cx != old.cx || position.cy != old.cy)
          SetWindowPos(updated->hwnd, nullptr, 0, 0, position.cx, position.cy, position.flags);
      }
      reconcileWindows();
      sendResize(updated);
    } else if (type == "popup") {
      uint32_t id = m.at("id").get<uint32_t>(), root = m.at("root").get<uint32_t>(),
               parent = m.at("parent").get<uint32_t>();
      if (!windows.count(root) || !windows.count(parent))
        continue;
      if (!windows.count(id)) {
        if (windows.size() >= 128)
          continue;
        auto w = std::make_unique<RemoteWindow>();
        w->id = id;
        w->root = root;
        w->parent = parent;
        w->popup = true;
        w->clientDecorated = true;
        w->dpi = windows.at(root)->dpi;
        w->width = m.at("width");
        w->height = m.at("height");
        w->x = m.at("x");
        w->y = m.at("y");
        HWND hwnd =
            CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
                            L"WaybridgePopup", L"Waybridge popup", WS_POPUP, 0, 0, 1, 1,
                            windows.at(parent)->hwnd, nullptr, GetModuleHandleW(nullptr), w.get());
        if (!hwnd)
          continue;
        w->presentation = std::make_unique<Presentation>();
        if (!w->presentation->initialize(hwnd)) {
          w->presentation.reset();
          SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
                            GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & ~WS_EX_NOREDIRECTIONBITMAP);
        }
        windows[id] = std::move(w);
      }
      auto w = windows.at(id).get();
      w->x = m.at("x");
      w->y = m.at("y");
      w->width = m.at("width");
      w->height = m.at("height");
      positionPopup(w);
      if (semanticsNegotiated)
        w->state.input(m.value("input_region", Json()));
    } else if (type == "pointer_lock") {
      uint32_t id = m.at("id").get<uint32_t>();
      uint64_t generation = m.at("generation").get<uint64_t>();
      if (relativeInput.stale(generation))
        continue;
      if (!m.at("active").get<bool>()) {
        if (relativeInput.id() == id && relativeInput.generation() == generation)
          releasePointer(false);
      } else {
        auto found = windows.find(id);
        bool locked = found != windows.end() && !found->second->popup && !grabbedRoot &&
                      !FileDrag::ownsInput() &&
                      relativeInput.lock(found->second->hwnd, id, generation);
        if (!locked) refreshCursor();
        std::cout << "Pointer lock " << (locked ? "active" : "unavailable")
                  << " for window " << id << " generation " << generation << '\n';
        sendJson({{"type", "pointer_lock_ack"}, {"id", id},
                  {"generation", generation}, {"active", locked}});
      }
    } else if (type == "popup_grab") {
      if (m.at("active").get<bool>())
        releasePointer();
      setPopupGrab(m.at("root").get<uint32_t>(), m.at("active").get<bool>());
    } else if (type == "window_action") {
      auto found = windows.find(m.at("id").get<uint32_t>());
      if (found == windows.end())
        continue;
      HWND hwnd = found->second->hwnd;
      auto *native = found->second.get();
      if (semanticsNegotiated && m.contains("fullscreen_epoch"))
        native->state.fullscreenEpoch = m.at("fullscreen_epoch").get<uint64_t>();
      auto action = m.value("action", "");
      if (semanticsNegotiated && (action == "fullscreen" || action == "unfullscreen")) {
        setFullscreen(native, action == "fullscreen");
        continue;
      }
      if (semanticsNegotiated && native->state.fullscreen &&
          (action == "maximize" || action == "restore"))
        setFullscreen(native, false);
      if (action == "move") {
        ReleaseCapture();
        PostMessageW(hwnd, WM_SYSCOMMAND, SC_MOVE | HTCAPTION, 0);
      } else if (action == "resize") {
        UINT edge = 0;
        switch (m.value("edge", 0u)) {
        case 1:
          edge = WMSZ_TOP;
          break;
        case 2:
          edge = WMSZ_BOTTOM;
          break;
        case 4:
          edge = WMSZ_LEFT;
          break;
        case 5:
          edge = WMSZ_TOPLEFT;
          break;
        case 6:
          edge = WMSZ_BOTTOMLEFT;
          break;
        case 8:
          edge = WMSZ_RIGHT;
          break;
        case 9:
          edge = WMSZ_TOPRIGHT;
          break;
        case 10:
          edge = WMSZ_BOTTOMRIGHT;
          break;
        }
        if (edge) {
          ReleaseCapture();
          PostMessageW(hwnd, WM_SYSCOMMAND, SC_SIZE | edge, 0);
        }
      } else if (action == "minimize")
        ShowWindow(hwnd, SW_MINIMIZE);
      else if (action == "maximize")
        ShowWindow(hwnd, SW_MAXIMIZE);
      else if (action == "restore")
        ShowWindow(hwnd, SW_RESTORE);
    } else if (type == "window_closed") {
      uint32_t id = m.at("id").get<uint32_t>();
      destroyRemote(id);
    } else if (type == "clipboard") {
      auto generation = m.value("generation", uint64_t(0));
      if (FileTree::serverClipboard(generation)) {
        setClipboard(m.at("items"));
        FileTree::clipboardApplied(generation, ownClipboardSequence);
      }
    }
    else if (type == "files_begin" || type == "file_chunk" || type == "files_done" ||
             type == "files_error")
      receiveFiles(m);
    else if (type == "error")
      std::cerr << m.value("message", "Remote error") << '\n';
  }
  if (cursorNegotiated && !cursor.is_null() && nativeCursor.update(cursor))
    refreshCursor();
  if (!visual.is_null()) {
    if (visual.value("type", "") == "drag_visual_reset") dragVisual.reset();
    else dragVisual.receive(visual);
  }
  for (auto &[id, frame] : pendingFrames) {
    auto it = windows.find(id);
    if (it == windows.end())
      continue;
    auto *w = it->second.get();
    if (w->frame && frame->sequence <= w->frame->sequence)
      continue;
    w->width = frame->logicalWidth;
    w->height = frame->logicalHeight;
    w->originX = frame->originX;
    w->originY = frame->originY;
    clientStats.record("ui_queue", millis(Clock::now() - frame->decoded));
    if (semanticsNegotiated &&
        (!w->state.mapped || (w->shown && (!IsWindowVisible(w->hwnd) || IsIconic(w->hwnd))))) {
      w->frame = frame;
      acknowledge(w);
      continue; // retain/decode in order, return hidden-frame credit
    }
    auto presentStart = Clock::now();
    if (frame->video.texture) {
      if (!w->presentation ||
          !w->presentation->presentVideo(frame->video.texture.Get(), frame->video.subresource,
                                         frame->width, frame->height, frame->alpha, imageRect(w),
                                         VideoDecoder::hdr(frame->codec))) {
        std::cerr << "Video GPU presentation failed; requesting PNG fallback\n";
        sendJson({{"type", "codec_fallback"},
                  {"id", id},
                  {"codec", VideoDecoder::capability(frame->codec)}});
        continue;
      }
    } else if (w->presentation && !w->presentation->present(frame->bgra.data(), frame->width,
                                                            frame->height, imageRect(w))) {
      std::cerr << "Composition presentation failed; switching to buffered GDI (no transparency)\n";
      w->presentation.reset();
      SetWindowLongPtrW(w->hwnd, GWL_EXSTYLE,
                        GetWindowLongPtrW(w->hwnd, GWL_EXSTYLE) & ~WS_EX_NOREDIRECTIONBITMAP);
      SetWindowPos(w->hwnd, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
    }
    clientStats.record("present", millis(Clock::now() - presentStart));
    if (w->resizePending && frame->logicalWidth == MulDiv(w->lastResizeWidth, 96, w->dpi) &&
        frame->logicalHeight == MulDiv(w->lastResizeHeight, 96, w->dpi)) {
      clientStats.record("resize_to_present", millis(Clock::now() - w->resizeSent));
      w->resizePending = false;
    }
    clientStats.report();
    w->frame = frame;
    presentedFrames.fetch_add(1, std::memory_order_relaxed);
    if (w->presentation)
      acknowledge(w);
    if (!w->presentation)
      InvalidateRect(w->hwnd, nullptr, FALSE);
    if (!w->shown) {
      w->shown = true;
      if (w->state.mapped)
        ShowWindow(w->hwnd, w->popup || windowDrag.active || nativeActivation.enabled() ? SW_SHOWNOACTIVATE : SW_SHOW);
      reconcileWindows();
      if (!w->popup) nativeActivation.shown(w->id, w->hwnd);
      sendWindowState(w);
      if (!w->popup && grabbedRoot == w->id)
        SetCapture(w->hwnd);
    }
  }
  moveDraggedWindow();
  updateIme();
}
void sendStreamConfiguration() {
  if (!hostControl.active()) return;
  if (!streamConfigNegotiated) {
    hostControl.emit({{"type","settings_warning"},{"message","服务端不支持实时画面设置，请更新 Linux 服务端；本地设置仍可使用"}});
    return;
  }
  sendJson({{"type","stream_config"},{"request_id",++streamRequest},
            {"fps",hostedSettings.value("fps",60)},{"bitrate",hostedSettings.value("bitrate",30000)},
            {"codec",hostedSettings.value("codec","auto")},{"hdr",hostedSettings.value("hdr",false)},
            {"read_only",hostedReadOnly}});
}
void applyHostedSettings(const Json &settings, bool live) {
  if (!settings.is_object()) throw std::runtime_error("Settings must be an object");
  int fps=settings.value("fps",60), bitrate=settings.value("bitrate",30000), dpi=settings.value("dpi",0);
  int volume=settings.value("volume",100);
  std::string codec=settings.value("codec","auto"), clipboard=settings.value("clipboard","both");
  if (fps<1 || fps>120 || bitrate<1000 || bitrate>200000 || (dpi && (dpi<96 || dpi>384)) ||
      volume<0 || volume>100 ||
      (codec!="auto" && codec!="png" && codec!="h264" && codec!="hevc" && codec!="av1") ||
      (clipboard!="both" && clipboard!="send" && clipboard!="receive" && clipboard!="off"))
    throw std::runtime_error("Invalid settings range");
  std::string badge = settings.value("badge_png", "");
  if (badge.size() > 32768) throw std::runtime_error("Custom badge is too large");
  auto badgeBytes = unbase64(badge);
  if (!badge.empty() && badgeBytes.empty()) throw std::runtime_error("Invalid custom badge encoding");
  ImageDecoder badgeDecoder;
  if (!IconBadge::configure(badgeBytes, badgeDecoder))
    throw std::runtime_error("Custom badge must be a 64x64 PNG (at most 24 KiB)");
  if (live) {
    for (auto &[id, window] : windows) window->icon.apply(window->hwnd, window->dpi);
    for (const char *key : {"hdr", "udp", "udp_host", "audio_device"})
      if (hostedSettings.value(key, Json()) != settings.value(key, Json())) {
        hostControl.emit({{"type","settings_warning"},{"message","HDR、UDP 或声音设备已保存；请断开并重新连接使其生效"}});
        break;
      }
    TextDrag::reset();
    releasePointer(); cancelWindowDrag(); suspendImeForOle();
    for (auto &[id,window] : windows) sendJson({{"type","cancel_input"},{"id",id}});
  }
  hostedSettings=settings;
  remoteNotifications.settings(settings.value("notifications",true));
  remoteTray.configure(trayNegotiated,settings.value("tray",true),settings.value("read_only",false));
  remoteTray.repaint();
  hostedReadOnly=settings.value("read_only",false); hostedIme=settings.value("ime",true);
  remoteNotifications.inputReadOnly(hostedReadOnly);
  hostedDragText=settings.value("drag_text",true);
  hostedFiles=settings.value("files",true); hostedImages=settings.value("clipboard_images",true);
  if (hostedReadOnly || !hostedDragText) TextDrag::reset();
  hostedClipboard=clipboard;
  FileTree::setDownloadsEnabled(hostedFiles && hostedClipboard != "off" && hostedClipboard != "send");
  std::wstring scale=dpi?std::to_wstring(dpi):L"";
  SetEnvironmentVariableW(L"WAYBRIDGE_DPI",dpi?scale.c_str():nullptr);
  SetEnvironmentVariableW(L"WAYBRIDGE_HDR",settings.value("hdr",false)?L"1":nullptr);
  SetEnvironmentVariableW(L"WAYBRIDGE_NO_AUDIO",settings.value("audio",true)?nullptr:L"1");
  std::wstring device=wide(settings.value("audio_device",""));
  SetEnvironmentVariableW(L"WAYBRIDGE_AUDIO_DEVICE",device.empty()?nullptr:device.c_str());
  std::wstring udp=wide(settings.value("udp_host",""));
  SetEnvironmentVariableW(L"WAYBRIDGE_UDP_HOST",settings.value("udp",false)&&!udp.empty()?udp.c_str():nullptr);
  audioPlayback.setVolume(settings.value("audio",true)?float(volume)/100.0f:0.0f);
  if (live) {
    if (!hostedFiles) { FileDrag::disconnect(); FileTree::cancelUploads(); FileTree::cancelIncoming(); VirtualFiles::cancel(); }
    if (hostedClipboard=="off" || hostedClipboard=="send") { FileTree::cancelIncoming(); VirtualFiles::cancel(); }
    sendJson({{"type","file_drop_out_enabled"},{"enabled",hostedFiles}});
    sendJson({{"type","text_drag_enabled"},{"enabled",hostedDragText}});
    sendStreamConfiguration();
    for (auto &[id,window] : windows) if (!window->popup) { window->dpi=effectiveDpi(window->hwnd); sendResize(window.get()); }
    updateIme();
    hostControl.emit({{"type","local_settings_applied"},{"settings",settings}});
  }
}
void publishHostedStatus() {
  if (!hostControl.active()) return;
  Json list=Json::array();
  for (auto &[id,w] : windows) if (!w->popup) {
    wchar_t title[256]{}; GetWindowTextW(w->hwnd,title,256);
    list.push_back({{"id",id},{"title",utf8(title)},{"width",w->width},{"height",w->height},
      {"codec",w->frame?(VideoDecoder::videoKind(w->frame->codec)?VideoDecoder::name(w->frame->codec):"PNG"):"等待画面"},
      {"hardware_decode",bool(w->frame&&w->frame->video.texture)},{"presentation",w->presentation?"D3D11 / DirectComposition":"GDI"}});
  }
  hostControl.emit({{"type","status"},{"uptime_ms",GetTickCount64()},{"tcp_bytes",networkBytes.load()},
                    {"presented_updates",presentedFrames.load()},{"windows",list},{"background",remoteTray.background()}});
}
LRESULT CALLBACK controlProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
  try {
    if(message==WM_TIMER && wp==TextDrag::Timer){TextDrag::tick();return 0;}
    if (message == WM_TIMER && wp == DragVisual::Timer) { dragVisual.tick(); return 0; }
    if (remoteTray.message(message,wp,lp)) return 0;
    if (message == HostControl::Message) {
      for (const auto &request : hostControl.take()) {
        try {
          std::string type=request.value("type","");
          if (type=="disconnect") PostMessageW(hwnd,WM_CLOSE,0,0);
          else if (type=="activation_prepare") {
            auto requestId=request.value("request_id", "");
            if (nativeActivation.prepare(requestId,request.value("foreground",false))) {
              Json command={{"type","activation_prepare"},{"request_id",requestId},{"app",request.value("app","")}};
              sendJson(command);
            }
          }
          else if (type=="activation_launch") sendJson(request);
          else if (remoteNotifications.command(request)) {}
          else if (type=="settings") applyHostedSettings(request.at("settings"),true);
          else if (type=="tray_action") {
            if (!(request.value("epoch",uint64_t(0)) && request.value("generation",uint64_t(0)) && remoteTray.request(request.value("id",std::string()),request.value("action",std::string()),request.value("epoch",uint64_t(0)),request.value("generation",uint64_t(0)))))
              throw std::runtime_error("Tray action is unavailable or input is read-only");
          }
          else if (type=="send_files") {
            if (!hostedFiles) throw std::runtime_error("File transfers disabled");
            std::vector<std::wstring> paths;
            for (const auto &path : request.at("paths")) {
              if (!path.is_string() || paths.size()>=FileTree::MaxRoots) throw std::runtime_error("Invalid file roots");
              paths.push_back(wide(path.get<std::string>()));
            }
            if (paths.empty() || !uploadFiles(std::move(paths))) throw std::runtime_error("Cannot start file transfer");
            hostControl.emit({{"type","transfer"},{"event","upload_started"}});
          } else if (type=="status") publishHostedStatus();
          else if (type=="focus_window") {
            auto found=windows.find(request.value("id",0u));
            if (found!=windows.end()) NativeActivation::user(found->second->hwnd, request.value("foreground",false));
          }
        } catch (const std::exception &error) {
          hostControl.emit({{"type","error"},{"message",error.what()}});
        }
      }
      return 0;
    }
    if (windowDrag.active) {
      if (message == WM_MOUSEMOVE) { sendWindowDragMotion(); return 0; }
      if (message == WM_LBUTTONUP) { sendWindowDragMotion(true); return 0; }
      if (message == WM_CANCELMODE || (message == WM_CAPTURECHANGED && reinterpret_cast<HWND>(lp) != hwnd)) {
        // OLE deliberately releases capture while retaining the original
        // Wayland source. Its QueryContinueDrag/result owns cancellation now.
        if (!FileDrag::ownsInput()) cancelWindowDrag();
        return 0;
      }
      if (message == WM_TIMER && wp == WindowDragTimer) {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) cancelWindowDrag();
        else sendWindowDragMotion(!(GetAsyncKeyState(VK_LBUTTON) & 0x8000));
        return 0;
      }
    }
    if (message == VirtualFiles::CompletedMessage) {
      VirtualFiles::drain();
      return 0;
    }
    if (message == FileDrag::ResumeMessage) {
      if (windowDrag.active && !windowDrag.released) {
        SetCapture(controller);sendWindowDragMotion(!(GetAsyncKeyState(VK_LBUTTON)&0x8000));
      }
      return 0;
    }
    if (message == FileDrag::StartMessage) {
      if (FileDrag::ownsInput()) {
        releasePointer();
        suspendImeForOle();
      }
      if (FileDrag::start(hwnd)) dragVisual.reset();
      updateIme();
      return 0;
    }
    if (message == NetworkReady) {
      dispatchNetwork();
      FileIo::drain();
      return 0;
    }
    if (message == WM_CLIPBOARDUPDATE) {
      if (GetClipboardSequenceNumber() != ownClipboardSequence) {
        FileTree::localClipboardChanged();
        cancelIncomingFiles();
        publishClipboard();
      }
      return 0;
    }
    if (message == WM_TIMER) {
      remoteTray.tick();
      for (auto &entry : windows)
        if (!entry.second->popup)
          sendColorMode(entry.second.get());
      VirtualFiles::tick();
      sendJson({{"type", "ping"}, {"nonce", GetTickCount64()}});
      clientStats.report();
      publishHostedStatus();
      return 0;
    }
    if (message == WM_CLOSE) {
      TextDrag::reset();
      dragVisual.reset();
      remoteTray.clear();
      windowDrag.cancelled = true;
      endWindowDrag();
      releasePointer(false);
      running = false;
      FileTree::cancelUploads();
      VirtualFiles::cancel();
      PostQuitMessage(0);
      return 0;
    }
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    releasePointer(false);
    PostQuitMessage(1);
    return 0;
  }
  return DefWindowProcW(hwnd, message, wp, lp);
}
BOOL WINAPI consoleControl(DWORD) {
  if (controller)
    PostMessageW(controller, WM_CLOSE, 0, 0);
  return TRUE;
}

int wmain(int argc, wchar_t **argv) {
  uiThread = GetCurrentThreadId();
  std::wstring hostedHost, hostedPort, hostedToken;
  wchar_t *hostedArguments[4]{};
  if (argc==3 && std::wstring(argv[1])==L"--control-pipe") {
    try {
      Json start=hostControl.connect(argv[2]);
      NativeTransport::configuration = NativeTransport::Configuration::parse(start.value("transport", Json()));
      std::string host=start.at("host").get<std::string>(), token=start.at("token").get<std::string>();
      int port=start.at("port").get<int>();
      if (host.empty() || host.size()>255 || port<1 || port>65535 || token.size()<16 || token.size()>4096 ||
          token.find_first_of(" \r\n\t")!=std::string::npos) throw std::runtime_error("Invalid GUI connection parameters");
      remoteApplicationSource = start.value("source_id", "");
      if (remoteApplicationSource.size() > 128) throw std::runtime_error("Invalid application source identity");
      hostedHost=wide(host); hostedPort=std::to_wstring(port); hostedToken=wide(token);
      hostedArguments[0]=argv[0]; hostedArguments[1]=hostedHost.data();
      hostedArguments[2]=hostedPort.data(); hostedArguments[3]=hostedToken.data();
      argv=hostedArguments; argc=4;
      // Bootstrap settings may decode a custom badge before the normal OLE
      // initialization below. Balance this temporary apartment on every exit.
      struct BootstrapOle {
        HRESULT result = OleInitialize(nullptr);
        ~BootstrapOle() { if (SUCCEEDED(result)) OleUninitialize(); }
      } bootstrapOle;
      if (FAILED(bootstrapOle.result)) throw std::runtime_error("Cannot initialize custom icon decoder");
      applyHostedSettings(start.value("settings",Json::object()),false);
      clientStats.enabled=true;
      SetEnvironmentVariableW(L"WAYBRIDGE_IME_TRACE",nullptr);
      SetEnvironmentVariableW(L"WAYBRIDGE_ICON_TRACE",nullptr);
    } catch (const std::exception &error) { std::cerr<<error.what()<<'\n'; return 2; }
  }
  if (argc == 2 && std::wstring(argv[1]) == L"--check-hdr-shaders") {
    auto report = Presentation::validateHdrPixels();
    std::cout << report.dump() << '\n';
    return report.value("verified", false) ? 0 : 2;
  }
  if (argc == 2 && std::wstring(argv[1]) == L"--check-hdr") {
    std::cout << HdrCapabilities::inspect().dump() << '\n';
    return 0;
  }
  if (argc == 2 && std::wstring(argv[1]) == L"--check-codecs") {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto &mf = MediaFoundation::get();
    bool started = mf.available() && SUCCEEDED(mf.Startup(MF_VERSION, MFSTARTUP_FULL));
    for (uint8_t kind : {2, 4, 5, 7, 8})
      std::cout << VideoDecoder::name(kind) << " capability="
                << (started && VideoDecoder::available(kind) ? "available" : "unavailable") << '\n';
    if (started)
      mf.Shutdown();
    CoUninitialize();
    return 0;
  }
  if (argc == 2 && std::wstring(argv[1]) == L"--version") {
    std::cout << "Waybridge client " << WAYBRIDGE_VERSION << '\n';
    return 0;
  }
  if (argc == 2 && std::wstring(argv[1]) == L"--check-shaders")
    return Presentation::validateShaders() ? 0 : 1;
  if (argc != 4) {
    std::cerr << "Usage: waybridge-client.exe HOST PORT TOKEN\nUse an SSH "
                 "tunnel to the Linux loopback listener.\n";
    return 1;
  }
  if (remoteApplicationSource.empty()) remoteApplicationSource = utf8(argv[1]) + ":" + utf8(argv[2]);
  if (!SetProcessDpiAwarenessContext(reinterpret_cast<DPI_AWARENESS_CONTEXT>(-4)))
    SetProcessDPIAware();
  WSADATA ws;
  if (WSAStartup(MAKEWORD(2, 2), &ws))
    return 1;
  Gdiplus::GdiplusStartupInput startup;
  ULONG_PTR gdiplus;
  if (Gdiplus::GdiplusStartup(&gdiplus, &startup, nullptr) != Gdiplus::Ok)
    return 1;
  OleInitialize(nullptr);
  bool audioSupported = AudioPlayback::supported();
  bool mfStarted = MediaFoundation::get().available() &&
                   SUCCEEDED(MediaFoundation::get().Startup(MF_VERSION, MFSTARTUP_FULL));
  bool videoEnabled = !GetEnvironmentVariableW(L"WAYBRIDGE_PNG", nullptr, 0);
  bool hardwareDecoder = videoEnabled && mfStarted && H264Decoder::available();
  if (hardwareDecoder)
    supportedVideoKinds.insert(2);
  bool hdrEnabled = GetEnvironmentVariableW(L"WAYBRIDGE_HDR", nullptr, 0) != 0;
  bool hdrPresentation = false;
  if (hdrEnabled) {
    auto report = HdrCapabilities::inspect();
    if (report.value("hardware_device", false) &&
        report.value("fp16_composition_swapchain", false) &&
        report.value("scrgb_color_space", false) && report.value("fp16_values_preserved", false)) {
      auto pixels = Presentation::validateHdrPixels();
      hdrPresentation = pixels.value("verified", false) && pixels.value("p010_plane_views", false);
    }
  }
  for (uint8_t kind : {4, 5, 7, 8}) {
    bool available = videoEnabled && mfStarted && (!VideoDecoder::hdr(kind) || hdrPresentation) &&
                     VideoDecoder::available(kind);
    if (available)
      supportedVideoKinds.insert(kind);
    std::cout << VideoDecoder::name(kind) << " GPU decoder capability: "
              << (available ? (VideoDecoder::hdr(kind) ? "verified P010 texture"
                                                       : "verified NV12 texture")
                            : "unavailable; not advertised")
              << '\n';
  }
  std::cout << "H.264 hardware decoder capability: "
            << (hardwareDecoder ? "available" : "unavailable; using PNG") << '\n';
  pngFormat = RegisterClipboardFormatW(L"PNG");
  HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSW wc{};
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpfnWndProc = windowProc;
  wc.lpszClassName = L"WaybridgeRemote";
  RegisterClassW(&wc);
  wc.lpfnWndProc = windowProc;
  wc.lpszClassName = L"WaybridgePopup";
  RegisterClassW(&wc);
  wc.lpfnWndProc = controlProc;
  wc.lpszClassName = L"WaybridgeControl";
  RegisterClassW(&wc);
  controller = CreateWindowExW(0, wc.lpszClassName, L"Waybridge", 0, 0, 0, 0, 0, HWND_MESSAGE,
                               nullptr, instance, nullptr);
  hostControl.start(controller);
  dragVisual.attach(controller);
  TextDrag::attach(controller);
  FileTree::activity = [](const Json &event) { hostControl.emit(event); };
  FileDrag::remoteHitTest = [](POINT point) { return windowDragTarget(point)!=nullptr; };
  remoteNotifications.decorate = [](Json &message) {
    NativeIcon icon;
    if (message.contains("png") && message["png"].is_string()) {
      if (!iconDecoder) iconDecoder = std::make_unique<ImageDecoder>();
      icon.update(unbase64(message["png"].get<std::string>()), *iconDecoder);
    }
    auto png = icon.badgedPng(64);
    message["png"] = png.empty() ? Json(nullptr) : Json(base64(png.data(), png.size()));
  };
  remoteTray.attach(controller,sendJson,[](const Json &event){hostControl.emit(event);});
  remoteTray.userActivation=[]()->std::string {
    GUID guid{};if(FAILED(CoCreateGuid(&guid)))return {};
    const char hex[]="0123456789abcdef";std::string request;request.reserve(32);
    for(size_t i=0;i<sizeof(guid);++i){uint8_t b=reinterpret_cast<const uint8_t*>(&guid)[i];request+=hex[b>>4];request+=hex[b&15];}
    return nativeActivation.prepare(request,true)?request:std::string();
  };
  AddClipboardFormatListener(controller);
  SetTimer(controller, 1, 2000, nullptr);
  SetConsoleCtrlHandler(consoleControl, TRUE);
  addrinfo hints{};
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_family = AF_UNSPEC;
  addrinfo *addresses = nullptr;
  if (getaddrinfo(utf8(argv[1]).c_str(), utf8(argv[2]).c_str(), &hints, &addresses)) {
    std::cerr << "Cannot resolve host\n";
    return 1;
  }
  for (auto a = addresses; a; a = a->ai_next) {
    connection = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (connection != INVALID_SOCKET &&
        connect(connection, a->ai_addr, static_cast<int>(a->ai_addrlen)) == 0)
      break;
    if (connection != INVALID_SOCKET)
      closesocket(connection);
    connection = INVALID_SOCKET;
  }
  freeaddrinfo(addresses);
  if (connection == INVALID_SOCKET) {
    std::cerr << "Connection failed; check your SSH tunnel\n";
    hostControl.emit({{"type","error"},{"message","无法连接串流服务，请检查 SSH 隧道与服务端"}});
    hostControl.flush();
    return 1;
  }
  int one = 1;
  setsockopt(connection, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char *>(&one), sizeof(one));
  DWORD sendTimeout = 3000;
  setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char *>(&sendTimeout),
             sizeof(sendTimeout));
  try { NativeTransport::attach(connection, utf8(argv[1])); }
  catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    hostControl.emit({{"type","error"},{"message",error.what()}});
    hostControl.flush();
    hostControl.stop(); closesocket(connection); return 1;
  }
  FileIo::start([] { PostMessageW(controller, NetworkReady, 0, 0); }, [] {
    std::cerr << "File I/O queue failed; incomplete transfers cancelled\n";
    hostControl.emit({{"type","error"},{"message","文件工作队列失败，连接已关闭以取消未完成传输"}});
    shutdown(connection, SD_BOTH);
    PostMessageW(controller, WM_CLOSE, 0, 0);
  });
  controlSender.start([](const OrderedSender::Packet &packet) {
    return sendAll(packet.data(), packet.size());
  }, [] {
    shutdown(connection, SD_BOTH);
    if (running && controller) PostMessageW(controller, WM_CLOSE, 0, 0);
  });
  MONITORINFO monitor{};
  monitor.cbSize = sizeof(monitor);
  POINT point{};
  GetMonitorInfoW(MonitorFromPoint(point, MONITOR_DEFAULTTOPRIMARY), &monitor);
  serverHost = utf8(argv[1]);
  serverPort = utf8(argv[2]);
  serverToken = utf8(argv[3]);
  Json codecs = Json::array({"png_rgba"});
  for (auto kind : supportedVideoKinds)
    codecs.push_back(VideoDecoder::capability(kind));
  sendJson({{"type", "hello"},
            {"version", 3},
            {"split_media", true},
            {"audio_pcm", audioSupported},
            {"udp_fec_v1", UdpMedia::requested()},
            {"frame_ack", true},
            {"hdr", supportedVideoKinds.count(7) > 0 || supportedVideoKinds.count(8) > 0},
            {"color_spaces", Json::array({"bt709_sdr", "bt2020_pq"})},
            {"cursor_v1", true},
            {"window_icon_v1", true},
            {"tray_v1", true},
            {"window_semantics_v1", true},
            {"activation_v1", true},
            {"data_drag_v1", true},
            {"notifications_v1", hostControl.active()},
            {"notifications_enabled", false},
            {"pointer_lock", true},
            {"toplevel_drag_v1", true},
            {"data_drag_v1", true},
            {"file_drop_out", hostedFiles},
            {"drag_text", hostedDragText},
            {"file_tree", true},
            {"codecs", codecs},
            {"token", utf8(argv[3])},
            {"dpi", effectiveDpi()},
            {"monitor_width", monitor.rcMonitor.right - monitor.rcMonitor.left},
            {"monitor_height", monitor.rcMonitor.bottom - monitor.rcMonitor.top},
            {"work_width", monitor.rcWork.right - monitor.rcWork.left},
            {"work_height", monitor.rcWork.bottom - monitor.rcWork.top}});
  std::thread decoder(decodeLoop);
  std::thread receiver(receiveLoop, connection);
  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  remoteTray.clear();
  windowDrag.cancelled = true;
  endWindowDrag();
  releasePointer(false);
  running = false;
  hostControl.stop();
  FileTree::cancelUploads();
  FileTree::cancelIncoming();
  FileDrag::disconnect();
  inboxWake.notify_all();
  frameWake.notify_all();
  shutdown(connection, SD_BOTH);
  controlSender.stop();
  udpMedia.stop();
  SOCKET media = mediaConnection.load();
  if (media != INVALID_SOCKET)
    shutdown(media, SD_BOTH);
  SOCKET audio = audioConnection.load();
  if (audio != INVALID_SOCKET)
    shutdown(audio, SD_BOTH);
  receiver.join();
  if (audioReceiver.joinable())
    audioReceiver.join();
  audioPlayback.stop();
  if (mediaReceiver.joinable())
    mediaReceiver.join();
  decoder.join();
  VirtualFiles::finishWorkers();
  for (auto &worker : fileWorkers) {
    worker.join();
  }
  VirtualFiles::shutdown();
  NativeTransport::detach(connection);
  closesocket(connection);
  cancelIncomingFiles();
  FileIo::stop();
  for (auto &[id, w] : windows)
    DestroyWindow(w->hwnd);
  windows.clear();
  iconDecoder.reset();
  decodedFrames.clear();
  frames.clear();
  controls.clear();
  RemoveClipboardFormatListener(controller);
  DestroyWindow(controller);
  if (mfStarted)
    MediaFoundation::get().Shutdown();
  TextDrag::reset();
  NativeDragGesture::observer.stop();
  OleUninitialize();
  Gdiplus::GdiplusShutdown(gdiplus);
  WSACleanup();
  return 0;
}
