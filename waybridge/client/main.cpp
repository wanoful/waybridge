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
#include "native_cursor.h"
#include "third_party/json.hpp"
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
constexpr size_t MaxPacket = 72 * 1024 * 1024;
constexpr UINT NetworkReady = WM_APP + 1;
SOCKET connection = INVALID_SOCKET;
HWND controller = nullptr;
std::atomic<bool> running{true};
std::mutex sendMutex, inboxMutex;
std::condition_variable inboxWake, frameWake;
std::deque<Json> controls;
Json pendingCursor;
NativeCursor nativeCursor;
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
std::string serverHost, serverPort, serverToken;
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
    for (auto &[name, s] : stages)
      std::cout << "Client timing " << name << " avg_ms=" << s.total / s.count
                << " max_ms=" << s.max << " samples=" << s.count << '\n';
    stages.clear();
  }
} clientStats;
double millis(Clock::duration duration) {
  return std::chrono::duration<double, std::milli>(duration).count();
}
struct DecodedFrame {
  int width = 0, height = 0, logicalWidth = 0, logicalHeight = 0, originX = 0, originY = 0;
  uint64_t sequence = 0;
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
DWORD incomingClipboardSequence = 0;

struct RemoteWindow {
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
  int lastWorkWidth = 0, lastWorkHeight = 0, lastWorkX = INT_MIN, lastWorkY = INT_MIN;
};
std::map<uint32_t, std::unique_ptr<RemoteWindow>> windows;
uint32_t grabbedRoot = 0;

DWORD windowStyle(bool clientDecorated) {
  // Client-decorated applications already supply their own title bar and controls.
  return clientDecorated ? WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX
                         : WS_OVERLAPPEDWINDOW;
}

void setDecoration(RemoteWindow *w, bool clientDecorated) {
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
    int count = send(connection, p, static_cast<int>(std::min(n, size_t(65536))), 0);
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
  uint8_t type = 0;
  std::lock_guard<std::mutex> guard(sendMutex);
  if (!sendAll(&size, 4) || !sendAll(&type, 1) || !sendAll(text.data(), text.size())) {
    shutdown(connection, SD_BOTH);
  }
}
bool readAll(SOCKET socket, void *bytes, size_t n) {
  auto p = static_cast<char *>(bytes);
  while (n && running) {
    int count = recv(socket, p, static_cast<int>(std::min(n, size_t(65536))), 0);
    if (count <= 0)
      return false;
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
      bool control = packet[0] == 0;
      {
        std::unique_lock<std::mutex> guard(inboxMutex);
        if (control) {
          inboxWake.wait(guard, [] { return controls.size() < 128 || !running; });
          if (!running)
            break;
          auto message = Json::parse(packet.begin() + 1, packet.end());
          if (message.value("type", "") == "window_closed") {
            decoderGarbage.push_back(message.at("id").get<uint32_t>());
            frameWake.notify_all();
          }
          if (message.value("type", "") == "cursor")
            pendingCursor = std::move(message); // bounded latest-only animation state
          else
            controls.push_back(std::move(message));
        } else if ((packet[0] == 1 && size >= 37) || (packet[0] == 2 && size >= 61)) {
          uint32_t id = u32(packet.data() + 1);
          if (frames.size() >= 128 && !frames.count(id))
            break;
          if (packet[0] == 2)
            frameWake.wait(
                guard, [&] { return !running || !frames.count(id) || frames.at(id).size() < 4; });
          if (!running)
            break;
          uint8_t kind = packet[0];
          packet.erase(packet.begin());
          auto &queue = frames[id];
          if (kind == 1)
            queue.clear();
          queue.push_back({kind, std::move(packet), Clock::now()});
          frameWake.notify_all();
        } else
          break;
      }
      if (control)
        PostMessageW(controller, NetworkReady, 0, 0);
    }
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
  }
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
      int n = send(socket, reinterpret_cast<const char *>(wire.data() + sent),
                   int(wire.size() - sent), 0);
      if (n <= 0)
        break;
      sent += n;
    }
    if (sent == wire.size())
      receiveLoop(socket);
    else
      PostMessageW(controller, WM_CLOSE, 0, 0);
    mediaConnection = INVALID_SOCKET;
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
      std::cerr << "H264 decoder produced no frame; requesting PNG fallback for window " << window
                << '\n';
      videoStates.erase(window);
      disabled.insert(window);
      sendJson({{"type", "codec_fallback"}, {"id", window}});
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
    if (encoded.kind == 2) {
      if (disabled.count(id))
        continue;
      try {
        if (bytes.size() < 60)
          throw std::runtime_error("Invalid H264 header");
        uint32_t epoch = u32(bytes.data() + 36), cw = u32(bytes.data() + 40),
                 ch = u32(bytes.data() + 44), key = u32(bytes.data() + 48),
                 color = u32(bytes.data() + 52), alpha = u32(bytes.data() + 56);
        if (cw < width || ch < height || cw > 4096 || ch > 2304 ||
            uint64_t(color) + alpha + 60 != bytes.size() || !color)
          throw std::runtime_error("Invalid H264 dimensions/length");
        auto &state = videoStates[id];
        if (!state.decoder || state.epoch != epoch) {
          if (!key)
            throw std::runtime_error("New H264 epoch did not start with an IDR");
          state.pending.clear();
          auto initStarted = Clock::now();
          decoderCache.recycle(std::move(state.decoder));
          bool reused = false;
          state.decoder = decoderCache.acquire(cw, ch, reused);
          clientStats.record(reused ? "decoder_reuse" : "decoder_create",
                             millis(Clock::now() - initStarted));
          state.epoch = epoch;
          std::cout << "D3D11 hardware H.264 decoder active for window " << id << '\n';
        }
        if (alpha) {
          auto maskStarted = Clock::now();
          if (!images->decode(bytes.data() + 60 + color, alpha, width, height, true, frame->alpha))
            throw std::runtime_error("Invalid H264 alpha mask");
          clientStats.record("alpha_decode", millis(Clock::now() - maskStarted));
        }
        state.pending[frame->sequence] = frame;
        if (state.pending.size() > 8)
          throw std::runtime_error("Hardware decoder buffered too many frames");
        auto videoStarted = Clock::now();
        auto surfaces = state.decoder->decode(bytes.data() + 60, color, frame->sequence);
        clientStats.record("h264_transform", millis(Clock::now() - videoStarted));
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
        std::cerr << "H264 decode failed for window " << id << ": " << e.what()
                  << "; requesting PNG fallback\n";
        videoStates.erase(id);
        disabled.insert(id);
        sendJson({{"type", "codec_fallback"}, {"id", id}});
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
    clientStats.record(encoded.kind == 2 ? "h264_decode" : "png_decode",
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

void uploadFiles(std::vector<std::wstring> paths, Json drop = Json::object()) {
  fileWorkers.emplace_back([paths = std::move(paths), drop = std::move(drop)] {
    Json transfers = Json::array();
    for (const auto &path : paths) {
      if (!running)
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
      uint64_t id = ++nextUpload;
      sendJson(
          {{"type", "upload_begin"}, {"transfer", id}, {"name", name}, {"size", size.QuadPart}});
      std::vector<uint8_t> buffer(65536);
      DWORD n = 0;
      bool ok = true;
      while (running) {
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &n, nullptr)) {
          ok = false;
          break;
        }
        if (!n)
          break;
        sendJson({{"type", "upload_chunk"}, {"transfer", id}, {"data", base64(buffer.data(), n)}});
      }
      CloseHandle(file);
      if (!ok || !running)
        return;
      sendJson({{"type", "upload_end"}, {"transfer", id}});
      transfers.push_back(id);
    }
    if (!transfers.empty()) {
      Json commit = drop;
      commit["type"] = "upload_commit";
      commit["transfers"] = transfers;
      sendJson(commit);
    }
  });
}

RECT imageRect(RemoteWindow *w);

class FileDropTarget final : public IDropTarget {
  LONG refs = 1;
  RemoteWindow *window;
  bool accepted = false;

public:
  explicit FileDropTarget(RemoteWindow *w) : window(w) {}
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
  HRESULT STDMETHODCALLTYPE DragEnter(IDataObject *data, DWORD, POINTL, DWORD *effect) override {
    FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    accepted = SUCCEEDED(data->QueryGetData(&format));
    *effect = accepted ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD *effect) override {
    *effect = accepted ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DragLeave() override {
    accepted = false;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Drop(IDataObject *data, DWORD, POINTL point, DWORD *effect) override {
    *effect = DROPEFFECT_NONE;
    FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM storage{};
    if (FAILED(data->GetData(&format, &storage)))
      return S_OK;
    auto drop = static_cast<HDROP>(storage.hGlobal);
    std::vector<std::wstring> paths;
    UINT count = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
    for (UINT i = 0; i < std::min(count, 128u); ++i) {
      UINT n = DragQueryFileW(drop, i, nullptr, 0);
      std::wstring path(n + 1, L'\0');
      DragQueryFileW(drop, i, path.data(), n + 1);
      path.resize(n);
      paths.push_back(std::move(path));
    }
    ReleaseStgMedium(&storage);
    POINT position{point.x, point.y};
    ScreenToClient(window->hwnd, &position);
    RECT r = imageRect(window);
    if (!paths.empty() && r.right > r.left && r.bottom > r.top) {
      uploadFiles(std::move(paths),
                  {{"drop", true},
                   {"id", window->id},
                   {"origin_x", window->originX},
                   {"origin_y", window->originY},
                   {"x", double(position.x - r.left) * window->width / (r.right - r.left)},
                   {"y", double(position.y - r.top) * window->height / (r.bottom - r.top)}});
      *effect = DROPEFFECT_COPY;
    }
    return S_OK;
  }
};

void receiveFiles(const Json &message) {
  const std::string type = message.at("type");
  if (type == "files_begin") {
    for (auto &[id, file] : incomingFiles)
      if (file.handle != INVALID_HANDLE_VALUE)
        CloseHandle(file.handle);
    incomingFiles.clear();
    incomingBatch = message.at("batch").get<uint64_t>();
    incomingClipboardSequence = GetClipboardSequenceNumber();
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    std::wstring directory = std::wstring(temp) + L"Waybridge-" +
                             std::to_wstring(GetCurrentProcessId()) + L"-" +
                             std::to_wstring(GetTickCount64());
    if (!CreateDirectoryW(directory.c_str(), nullptr))
      throw std::runtime_error("Cannot create incoming file directory");
    if (message.at("files").size() > 128)
      throw std::runtime_error("Too many incoming files");
    for (const auto &metadata : message.at("files")) {
      std::wstring name = wide(metadata.at("name").get<std::string>());
      if (name.empty() || name == L"." || name == L".." ||
          name.find_first_of(L"/\\:\0") != std::wstring::npos)
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
      incomingFiles.emplace(metadata.at("id").get<uint32_t>(), std::move(file));
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
      // A newer local clipboard must win over a file download that finishes
      // late.
      if (GetClipboardSequenceNumber() == incomingClipboardSequence && OpenClipboard(controller)) {
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
        }
        CloseClipboard();
        ownClipboardSequence = GetClipboardSequenceNumber();
      }
      incomingFiles.clear();
    } else if (type == "files_error") {
      for (auto &[id, file] : incomingFiles) {
        if (file.handle != INVALID_HANDLE_VALUE)
          CloseHandle(file.handle);
        DeleteFileW(file.path.c_str());
      }
      incomingFiles.clear();
      std::cerr << message.value("message", "File transfer failed") << '\n';
    }
  }
}
void setClipboard(const Json &items) {
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
  if (items.contains("image/png") && items["image/png"].is_string()) {
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
  if (!OpenClipboard(controller))
    return;
  Json items = Json::object();
  std::vector<std::wstring> files;
  if (auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP))) {
    UINT count = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
    for (UINT i = 0; i < std::min(count, 128u); ++i) {
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
  if (auto mem = GetClipboardData(pngFormat)) {
    auto p = static_cast<const uint8_t *>(GlobalLock(mem));
    if (p) {
      size_t n = GlobalSize(mem);
      if (n <= 12 * 1024 * 1024)
        items["image/png"] = base64(p, n);
      GlobalUnlock(mem);
    }
  } else if (auto handle = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP))) {
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
void pointer(RemoteWindow *w, LPARAM param) {
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
uint32_t linuxKey(LPARAM param) {
  uint32_t code = (param >> 16) & 0xff;
  if (!(param & (1LL << 24)))
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

void sendResize(RemoteWindow *w) {
  if (w->popup || IsIconic(w->hwnd))
    return;
  RECT rect;
  GetClientRect(w->hwnd, &rect);
  bool maximized = IsZoomed(w->hwnd) != FALSE;
  MONITORINFO monitor{};
  monitor.cbSize = sizeof(monitor);
  GetMonitorInfoW(MonitorFromWindow(w->hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
  int workWidth = monitor.rcWork.right - monitor.rcWork.left,
      workHeight = monitor.rcWork.bottom - monitor.rcWork.top;
  POINT origin{};
  ClientToScreen(w->hwnd, &origin);
  int workX = MulDiv(monitor.rcWork.left - origin.x, 96, w->dpi),
      workY = MulDiv(monitor.rcWork.top - origin.y, 96, w->dpi);
  bool resized = w->lastResizeWidth != rect.right || w->lastResizeHeight != rect.bottom ||
                 w->lastMaximized != maximized || w->lastDpi != w->dpi;
  if (rect.right <= 0 || rect.bottom <= 0 ||
      (!resized && w->lastWorkWidth == workWidth && w->lastWorkHeight == workHeight &&
       w->lastWorkX == workX && w->lastWorkY == workY))
    return;
  w->lastWorkX = workX;
  w->lastWorkY = workY;
  w->lastResizeWidth = rect.right;
  w->lastResizeHeight = rect.bottom;
  w->lastMaximized = maximized;
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
            {"maximized", maximized}});
}
void acknowledge(RemoteWindow *w) {
  if (w->frame && w->frame->sequence > w->lastAck) {
    w->lastAck = w->frame->sequence;
    sendJson({{"type", "frame_ack"}, {"id", w->id}, {"sequence", w->lastAck}});
  }
}

uint32_t cursorRoot(RemoteWindow *w) { return w->popup ? w->root : w->id; }
void refreshCursor() {
  POINT point{};
  if (!cursorNegotiated || !GetCursorPos(&point)) return;
  HWND under = WindowFromPoint(point);
  for (auto &[id, w] : windows) {
    if (w->hwnd == under && SendMessageW(under, WM_NCHITTEST, 0, MAKELPARAM(point.x, point.y)) == HTCLIENT) {
      SetCursor(nativeCursor.handle(cursorRoot(w.get()), w->dpi));
      break;
    }
  }
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
  auto w = reinterpret_cast<RemoteWindow *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    w = static_cast<RemoteWindow *>(reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams);
    w->hwnd = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(w));
  }
  if (!w)
    return DefWindowProcW(hwnd, message, wp, lp);
  switch (message) {
  case WM_SETCURSOR:
    // A relative-pointer lock must hide the cursor before applying remote shapes.
    if (cursorNegotiated && LOWORD(lp) == HTCLIENT) {
      SetCursor(nativeCursor.handle(cursorRoot(w), w->dpi));
      return TRUE;
    }
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
  case WM_WINDOWPOSCHANGED:
    if (!w->popup) {
      for (auto &[id, popup] : windows)
        if (popup->popup && popup->root == w->id)
          positionPopup(popup.get());
      // DefWindowProc dispatches WM_SIZE after this message. Let WM_SIZE
      // coalesce drag sizes; movement alone still updates popup constraints.
      if (reinterpret_cast<WINDOWPOS *>(lp)->flags & SWP_NOSIZE)
        sendResize(w);
    }
    break;
  case WM_GETMINMAXINFO:
    if (w->clientDecorated) {
      MONITORINFO info{};
      info.cbSize = sizeof(info);
      if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info)) {
        auto limits = reinterpret_cast<MINMAXINFO *>(lp);
        limits->ptMaxPosition = {info.rcWork.left - info.rcMonitor.left,
                                 info.rcWork.top - info.rcMonitor.top};
        limits->ptMaxSize = {info.rcWork.right - info.rcWork.left,
                             info.rcWork.bottom - info.rcWork.top};
        limits->ptMinTrackSize = {64, 64};
        return 0;
      }
    }
    break;
  case WM_NCCALCSIZE:
    if (w->popup || w->clientDecorated)
      return 0;
    break;
  case WM_NCHITTEST:
    if (w->popup)
      return HTCLIENT;
    if (w->clientDecorated) {
      if (IsZoomed(hwnd))
        return HTCLIENT;
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
      return HTCLIENT;
    }
    break;
  case WM_ENTERSIZEMOVE:
    w->sizing = true;
    sendJson({{"type", "cancel_input"}, {"id", w->id}});
    return 0;
  case WM_EXITSIZEMOVE:
    w->sizing = false;
    KillTimer(hwnd, 1);
    sendResize(w);
    return 0;
  case WM_DESTROY:
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
    return 0;
  case WM_KILLFOCUS:
    if (w->popup)
      return 0;
    sendJson({{"type", "focus"}, {"id", w->id}, {"focused", false}});
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
    auto r = reinterpret_cast<const RECT *>(lp);
    SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    sendResize(w);
    return 0;
  }
  case WM_SIZE:
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
    return 0;
  case WM_TIMER:
    KillTimer(hwnd, 1);
    sendResize(w);
    return 0;
  case WM_MOUSEMOVE:
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
    sendJson({{"type", "leave"}, {"id", w->id}});
    return 0;
  case WM_DROPFILES: {
    auto drop = reinterpret_cast<HDROP>(wp);
    POINT position{};
    DragQueryPoint(drop, &position);
    std::vector<std::wstring> paths;
    UINT count = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
    for (UINT i = 0; i < std::min(count, 128u); ++i) {
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
    bool down = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN;
    uint32_t button = (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP)   ? 0x110
                      : (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP) ? 0x111
                                                                               : 0x112;
    if (down) {
      if (!w->popup)
        SetFocus(hwnd);
      if (!grabbedRoot)
        SetCapture(hwnd);
    } else if (!grabbedRoot)
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
  case WM_KEYUP:
  case WM_SYSKEYDOWN:
  case WM_SYSKEYUP:
    if (auto key = linuxKey(lp))
      sendJson({{"type", "key"},
                {"id", w->id},
                {"key", key},
                {"down", message == WM_KEYDOWN || message == WM_SYSKEYDOWN}});
    return 0;
  case WM_CLOSE:
    sendJson({{"type", "close"}, {"id", w->id}});
    return 0;
  }
  return DefWindowProcW(hwnd, message, wp, lp);
}

void destroyRemote(uint32_t id) {
  std::vector<uint32_t> children;
  for (auto &[child, w] : windows)
    if (w->popup && w->parent == id)
      children.push_back(child);
  for (auto child : children)
    destroyRemote(child);
  auto found = windows.find(id);
  if (found == windows.end())
    return;
  if (grabbedRoot == id)
    setPopupGrab(id, false);
  DestroyWindow(found->second->hwnd);
  windows.erase(found);
}

void dispatchNetwork() {
  std::deque<Json> pending;
  Json cursor;
  std::map<uint32_t, std::shared_ptr<DecodedFrame>> pendingFrames;
  {
    std::lock_guard<std::mutex> guard(inboxMutex);
    pending.swap(controls);
    cursor.swap(pendingCursor);
    pendingFrames.swap(decodedFrames);
  }
  inboxWake.notify_all();
  for (auto &m : pending) {
    std::string type = m.value("type", "");
    if (type == "hello") {
      std::cout << "Connected, protocol " << m.value("version", 0) << '\n';
      if (m.value("version", 0) != 3 || m.value("codec", "") != "mixed") {
        std::cerr << "Incompatible server; update both ends to 0.3.0\n";
        PostQuitMessage(1);
        return;
      }
      cursorNegotiated = m.value("cursor_v1", false);
      if (m.value("split_media", false))
        startMedia(m.at("media_key").get<std::string>());
      publishClipboard();
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
        auto target = new FileDropTarget(windows.at(id).get());
        HRESULT registration = RegisterDragDrop(hwnd, target);
        target->Release();
        if (FAILED(registration))
          std::cerr << "OLE drop registration failed\n";
        DragAcceptFiles(hwnd, TRUE);
        sendResize(windows.at(id).get());
      } else {
        setDecoration(windows.at(id).get(), m.value("decoration", "server") == "client");
        SetWindowTextW(windows.at(id)->hwnd, wide(m.value("title", "Linux application")).c_str());
      }
      auto parent = m.find("parent");
      if (parent != m.end() && parent->is_number_unsigned() &&
          windows.count(parent->get<uint32_t>()))
        SetWindowLongPtrW(windows.at(id)->hwnd, GWLP_HWNDPARENT,
                          reinterpret_cast<LONG_PTR>(windows.at(parent->get<uint32_t>())->hwnd));
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
    } else if (type == "popup_grab") {
      setPopupGrab(m.at("root").get<uint32_t>(), m.at("active").get<bool>());
    } else if (type == "window_action") {
      auto found = windows.find(m.at("id").get<uint32_t>());
      if (found == windows.end())
        continue;
      HWND hwnd = found->second->hwnd;
      auto action = m.value("action", "");
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
    } else if (type == "clipboard")
      setClipboard(m.at("items"));
    else if (type == "files_begin" || type == "file_chunk" || type == "files_done" ||
             type == "files_error")
      receiveFiles(m);
    else if (type == "error")
      std::cerr << m.value("message", "Remote error") << '\n';
  }
  if (cursorNegotiated && !cursor.is_null() && nativeCursor.update(cursor))
    refreshCursor();
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
    auto presentStart = Clock::now();
    if (frame->video.texture) {
      if (!w->presentation ||
          !w->presentation->presentVideo(frame->video.texture.Get(), frame->video.subresource,
                                         frame->width, frame->height, frame->alpha, imageRect(w))) {
        std::cerr << "H264 GPU presentation failed; requesting PNG fallback\n";
        sendJson({{"type", "codec_fallback"}, {"id", id}});
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
    if (w->presentation)
      acknowledge(w);
    if (!w->presentation)
      InvalidateRect(w->hwnd, nullptr, FALSE);
    if (!w->shown) {
      w->shown = true;
      ShowWindow(w->hwnd, w->popup ? SW_SHOWNOACTIVATE : SW_SHOW);
      if (!w->popup && grabbedRoot == w->id)
        SetCapture(w->hwnd);
    }
  }
}
LRESULT CALLBACK controlProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
  try {
    if (message == NetworkReady) {
      dispatchNetwork();
      return 0;
    }
    if (message == WM_CLIPBOARDUPDATE) {
      if (GetClipboardSequenceNumber() != ownClipboardSequence)
        publishClipboard();
      return 0;
    }
    if (message == WM_TIMER) {
      sendJson({{"type", "ping"}, {"nonce", GetTickCount64()}});
      clientStats.report();
      return 0;
    }
    if (message == WM_CLOSE) {
      PostQuitMessage(0);
      return 0;
    }
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
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
  bool mfStarted = MediaFoundation::get().available() &&
                   SUCCEEDED(MediaFoundation::get().Startup(MF_VERSION, MFSTARTUP_FULL));
  bool hardwareDecoder = mfStarted && H264Decoder::available();
  if (GetEnvironmentVariableW(L"WAYBRIDGE_PNG", nullptr, 0))
    hardwareDecoder = false;
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
    return 1;
  }
  int one = 1;
  setsockopt(connection, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char *>(&one), sizeof(one));
  DWORD sendTimeout = 3000;
  setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char *>(&sendTimeout),
             sizeof(sendTimeout));
  MONITORINFO monitor{};
  monitor.cbSize = sizeof(monitor);
  POINT point{};
  GetMonitorInfoW(MonitorFromPoint(point, MONITOR_DEFAULTTOPRIMARY), &monitor);
  serverHost = utf8(argv[1]);
  serverPort = utf8(argv[2]);
  serverToken = utf8(argv[3]);
  Json codecs = Json::array({"png_rgba"});
  if (hardwareDecoder)
    codecs.push_back("h264_alpha");
  sendJson({{"type", "hello"},
            {"version", 3},
            {"split_media", true},
            {"frame_ack", true},
            {"cursor_v1", true},
            {"codecs", codecs},
            {"token", utf8(argv[3])},
            {"dpi", effectiveDpi()},
            {"work_width", monitor.rcWork.right - monitor.rcWork.left},
            {"work_height", monitor.rcWork.bottom - monitor.rcWork.top}});
  std::thread decoder(decodeLoop);
  std::thread receiver(receiveLoop, connection);
  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  running = false;
  inboxWake.notify_all();
  frameWake.notify_all();
  shutdown(connection, SD_BOTH);
  SOCKET media = mediaConnection.load();
  if (media != INVALID_SOCKET)
    shutdown(media, SD_BOTH);
  receiver.join();
  if (mediaReceiver.joinable())
    mediaReceiver.join();
  decoder.join();
  for (auto &worker : fileWorkers) {
    worker.join();
  }
  closesocket(connection);
  for (auto &[id, file] : incomingFiles)
    if (file.handle != INVALID_HANDLE_VALUE)
      CloseHandle(file.handle);
  for (auto &[id, w] : windows)
    DestroyWindow(w->hwnd);
  windows.clear();
  decodedFrames.clear();
  frames.clear();
  controls.clear();
  RemoveClipboardFormatListener(controller);
  DestroyWindow(controller);
  if (mfStarted)
    MediaFoundation::get().Shutdown();
  OleUninitialize();
  Gdiplus::GdiplusShutdown(gdiplus);
  WSACleanup();
  return 0;
}
