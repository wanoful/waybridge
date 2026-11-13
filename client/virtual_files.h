#pragma once
// FILEDESCRIPTOR/FILECONTENTS consumers. No filesystem path or CF_HDROP is used.
// OLE objects remain on their apartment; a marshaled reference feeds the bounded
// file-tree worker. Linux sees paths only after the complete byte tree commits.
#include <shldisp.h>
#include <sstream>

namespace VirtualFiles {
constexpr UINT CompletedMessage = WM_APP + 19;
constexpr uint64_t MaxMemoryFile = 12 * 1024 * 1024;
template <class T> class Com {
  T *pointer = nullptr;

public:
  Com() = default;
  explicit Com(T *p) : pointer(p) {}
  Com(const Com &) = delete;
  Com &operator=(const Com &) = delete;
  Com(Com &&other) noexcept : pointer(other.pointer) { other.pointer = nullptr; }
  Com &operator=(Com &&other) noexcept {
    if (pointer)
      pointer->Release();
    pointer = other.pointer;
    other.pointer = nullptr;
    return *this;
  }
  ~Com() {
    if (pointer)
      pointer->Release();
  }
  T *get() const { return pointer; }
  T *operator->() const { return pointer; }
  T **out() {
    if (pointer)
      pointer->Release();
    pointer = nullptr;
    return &pointer;
  }
  T *release() {
    auto p = pointer;
    pointer = nullptr;
    return p;
  }
};
inline void check(HRESULT hr, const char *operation) {
  if (FAILED(hr)) {
    std::ostringstream text;
    text << operation << " (0x" << std::hex << uint32_t(hr) << ')';
    throw std::runtime_error(text.str());
  }
}
struct Medium {
  STGMEDIUM value{};
  ~Medium() {
    if (value.tymed)
      ReleaseStgMedium(&value);
  }
};
inline CLIPFORMAT descriptorsW() {
  return static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW));
}
inline CLIPFORMAT descriptorsA() {
  return static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORA));
}
inline CLIPFORMAT contents() {
  return static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILECONTENTS));
}
inline bool supports(IDataObject *object) {
  if (!FileTree::supported || !object)
    return false;
  FORMATETC format{descriptorsW(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  if (SUCCEEDED(object->QueryGetData(&format)))
    return true;
  format.cfFormat = descriptorsA();
  return SUCCEEDED(object->QueryGetData(&format));
}

struct Request {
  uint64_t id;
  IDataObject *original = nullptr;             // UI apartment only
  IDataObjectAsyncCapability *async = nullptr; // UI apartment only
  IStream *marshaled = nullptr;
  HANDLE finished = nullptr;
  HWND target = nullptr;
  uint64_t selectionEpoch = 0;
  bool clipboard = true;
  bool asyncStarted = false, uiReleased = false;
  std::atomic<bool> cancelled{false}, done{false}, success{false}, started{false};
  std::atomic<ULONGLONG> progress{GetTickCount64()};
  std::mutex marshalMutex;
  explicit Request(uint64_t number)
      : id(number), finished(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
  ~Request() {
    if (finished)
      CloseHandle(finished);
  }
  void touch() { progress = GetTickCount64(); }
  bool current() const {
    return running && !FileTree::uploadStopping && !cancelled &&
           (!clipboard || selectionEpoch == FileTree::epoch.load()) &&
           (!target || IsWindow(target));
  }
  void ensureCurrent() const {
    FileTree::require(current(), "Virtual import cancelled");
    FileTree::require(GetTickCount64() - progress.load() <= 30000, "Virtual source idle timeout");
  }
  void complete(bool result) {
    success = result && !cancelled;
    done.store(true, std::memory_order_release);
    SetEvent(finished);
    PostMessageW(controller, CompletedMessage, 0, 0);
  }
  void cancel() {
    cancelled = true;
    FileTree::cancelRequest(id);
  }
  // Called on the UI apartment, including rejection and final shutdown paths.
  void releaseUi() {
    if (uiReleased)
      return;
    uiReleased = true;
    if (asyncStarted && async) {
      HRESULT hr = async->EndOperation(success     ? S_OK
                                       : current() ? E_FAIL
                                                   : E_ABORT,
                                       nullptr, success ? DROPEFFECT_COPY : DROPEFFECT_NONE);
      if (FAILED(hr))
        std::cerr << "Virtual file source completion failed: 0x" << std::hex << uint32_t(hr)
                  << std::dec << '\n';
    }
    if (async) {
      async->Release();
      async = nullptr;
    }
    if (original) {
      original->Release();
      original = nullptr;
    }
    std::lock_guard<std::mutex> lock(marshalMutex);
    if (marshaled) {
      LARGE_INTEGER zero{};
      marshaled->Seek(zero, STREAM_SEEK_SET, nullptr);
      CoReleaseMarshalData(marshaled);
      marshaled->Release();
      marshaled = nullptr;
    }
  }
};
inline uint64_t nextRequest = 0;
inline std::map<uint64_t, std::shared_ptr<Request>> requests; // UI only

inline HRESULT getData(IDataObject *object, FORMATETC &format, Medium &medium,
                       const std::shared_ptr<Request> &request) {
  for (;;) {
    request->ensureCurrent();
    medium.value = {};
    HRESULT result = object->GetData(&format, &medium.value);
    if (result != E_PENDING) {
      if (FAILED(result))
        medium.value = {};
      return result;
    }
    // A failed/pending call does not transfer storage ownership.
    medium.value = {};
    Sleep(10);
  }
}
class Reader {
  Medium medium;
  Com<IStream> stream;
  const uint8_t *memory = nullptr;
  uint64_t size = 0, position = 0;
  bool memoryMode = false;
  std::shared_ptr<Request> request;

public:
  Reader(IDataObject *object, size_t index, std::optional<uint64_t> expected,
         std::shared_ptr<Request> state)
      : request(std::move(state)) {
    FORMATETC format{contents(), nullptr, DVASPECT_CONTENT, static_cast<LONG>(index),
                     TYMED_ISTREAM};
    HRESULT hr = getData(object, format, medium, request);
    if (FAILED(hr) && expected && *expected <= MaxMemoryFile) {
      format.tymed = TYMED_HGLOBAL;
      hr = getData(object, format, medium, request);
    }
    check(hr, "GetData(FileContents)");
    if (medium.value.tymed == TYMED_ISTREAM && medium.value.pstm) {
      // Clone keeps a provider's shared stream position intact; otherwise seek
      // the returned, owned stream. Non-seekable streams can be read sequentially.
      if (FAILED(medium.value.pstm->Clone(stream.out())) || !stream.get()) {
        medium.value.pstm->AddRef();
        stream = Com<IStream>(medium.value.pstm);
      }
      LARGE_INTEGER zero{};
      HRESULT seek;
      do {
        request->ensureCurrent();
        seek = stream->Seek(zero, STREAM_SEEK_SET, nullptr);
        if (seek == E_PENDING)
          Sleep(10);
      } while (seek == E_PENDING);
      if (FAILED(seek) && seek != STG_E_INVALIDFUNCTION && seek != E_NOTIMPL)
        check(seek, "Seek virtual file");
      if (expected)
        size = *expected;
      else {
        STATSTG stat{};
        HRESULT status;
        do {
          request->ensureCurrent();
          status = stream->Stat(&stat, STATFLAG_NONAME);
          if (status == E_PENDING)
            Sleep(10);
        } while (status == E_PENDING);
        check(status, "Stat unknown-size virtual file");
        size = stat.cbSize.QuadPart;
      }
    } else if (medium.value.tymed == TYMED_HGLOBAL && expected && *expected <= MaxMemoryFile &&
               (medium.value.hGlobal || !*expected)) {
      size = *expected;
      FileTree::require(GlobalSize(medium.value.hGlobal) >= size,
                        "Virtual file memory is shorter than its descriptor");
      memoryMode = true;
      if (size) {
        memory = static_cast<const uint8_t *>(GlobalLock(medium.value.hGlobal));
        FileTree::require(memory != nullptr, "Cannot lock virtual file memory");
      }
    } else
      throw std::runtime_error(
          "Unsupported virtual-file medium (IStream or bounded HGLOBAL required)");
    FileTree::require(size <= FileTree::MaxFile, "Virtual file exceeds 1 GiB");
  }
  ~Reader() {
    if (memory)
      GlobalUnlock(medium.value.hGlobal);
  }
  uint64_t length() const { return size; }
  size_t read(uint8_t *bytes, size_t length) {
    if (memoryMode) {
      size_t n = static_cast<size_t>(std::min<uint64_t>(length, size - position));
      if (n)
        memcpy(bytes, memory + position, n);
      position += n;
      return n;
    }
    for (;;) {
      request->ensureCurrent();
      ULONG n = 0;
      HRESULT result = stream->Read(bytes, static_cast<ULONG>(length), &n);
      FileTree::require(n <= length, "Invalid virtual stream read count");
      if (result == E_PENDING && !n) {
        Sleep(10);
        continue;
      }
      if (result != E_PENDING)
        check(result, "Read virtual file");
      return n;
    }
  }
};

struct Descriptor {
  std::wstring path;
  bool directory;
  std::optional<uint64_t> size;
  size_t index;
};
inline std::vector<Descriptor> describe(IDataObject *object,
                                        const std::shared_ptr<Request> &request) {
  FORMATETC format{descriptorsW(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  Medium medium;
  HRESULT hr = getData(object, format, medium, request);
  bool unicode = true;
  if (FAILED(hr)) {
    format.cfFormat = descriptorsA();
    unicode = false;
    hr = getData(object, format, medium, request);
  }
  check(hr, "GetData(FileGroupDescriptor)");
  FileTree::require(medium.value.tymed == TYMED_HGLOBAL && medium.value.hGlobal,
                    "Invalid virtual descriptor medium");
  size_t bytes = GlobalSize(medium.value.hGlobal);
  FileTree::require(bytes >= sizeof(UINT) &&
                        bytes <=
                            sizeof(UINT) + FileTree::MaxEntries * sizeof(FILEDESCRIPTORW) + 1024,
                    "Invalid virtual descriptor allocation length");
  auto memory = static_cast<const uint8_t *>(GlobalLock(medium.value.hGlobal));
  FileTree::require(memory != nullptr, "Cannot lock virtual descriptors");
  std::vector<Descriptor> result;
  try {
    UINT count;
    memcpy(&count, memory, sizeof(count));
    size_t itemSize = unicode ? sizeof(FILEDESCRIPTORW) : sizeof(FILEDESCRIPTORA);
    FileTree::require(count && count <= FileTree::MaxEntries &&
                          count <= (bytes - sizeof(UINT)) / itemSize,
                      "Invalid virtual descriptor count/length");
    for (size_t i = 0; i < count; ++i) {
      DWORD flags, attributes, high, low;
      std::wstring name;
      if (unicode) {
        FILEDESCRIPTORW item;
        memcpy(&item, memory + sizeof(UINT) + i * itemSize, sizeof(item));
        auto end = std::find(std::begin(item.cFileName), std::end(item.cFileName), L'\0');
        FileTree::require(end != std::end(item.cFileName), "Unterminated virtual filename");
        name.assign(item.cFileName, end);
        flags = item.dwFlags;
        attributes = item.dwFileAttributes;
        high = item.nFileSizeHigh;
        low = item.nFileSizeLow;
      } else {
        FILEDESCRIPTORA item;
        memcpy(&item, memory + sizeof(UINT) + i * itemSize, sizeof(item));
        auto end = std::find(std::begin(item.cFileName), std::end(item.cFileName), '\0');
        FileTree::require(end != std::end(item.cFileName), "Unterminated virtual filename");
        int length = static_cast<int>(end - item.cFileName);
        int n =
            MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, item.cFileName, length, nullptr, 0);
        FileTree::require(n > 0, "Invalid ANSI virtual filename");
        name.resize(n);
        if (n)
          MultiByteToWideChar(CP_ACP, 0, item.cFileName, length, name.data(), n);
        flags = item.dwFlags;
        attributes = item.dwFileAttributes;
        high = item.nFileSizeHigh;
        low = item.nFileSizeLow;
      }
      FileTree::require(wide(utf8(name)) == name, "Invalid Unicode virtual filename");
      std::replace(name.begin(), name.end(), L'\\', L'/');
      FileTree::require(!(flags & FD_ATTRIBUTES) ||
                            !(attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)),
                        "Virtual links/special files are unsupported");
      bool directory = (flags & FD_ATTRIBUTES) && (attributes & FILE_ATTRIBUTE_DIRECTORY);
      std::optional<uint64_t> size = directory ? std::optional<uint64_t>(0)
                                     : (flags & FD_FILESIZE)
                                         ? std::optional<uint64_t>((uint64_t(high) << 32) | low)
                                         : std::nullopt;
      if (size)
        FileTree::require(*size <= FileTree::MaxFile, "Virtual file exceeds 1 GiB");
      result.push_back({std::move(name), directory, size, i});
    }
  } catch (...) {
    GlobalUnlock(medium.value.hGlobal);
    throw;
  }
  GlobalUnlock(medium.value.hGlobal);
  return result;
}

class Input final : public FileTree::Input {
  Com<IDataObject> object;
  std::shared_ptr<Request> request;
  std::vector<std::optional<Descriptor>> mapping;
  std::unique_ptr<Reader> reader;

public:
  explicit Input(std::shared_ptr<Request> state) : request(std::move(state)) {
    request->started = true;
    request->touch();
    IStream *marshaled;
    {
      std::lock_guard<std::mutex> lock(request->marshalMutex);
      marshaled = request->marshaled;
      request->marshaled = nullptr;
    }
    FileTree::require(marshaled != nullptr, "Virtual source already consumed");
    check(CoGetInterfaceAndReleaseStream(marshaled, IID_PPV_ARGS(object.out())),
          "Unmarshal virtual source");
    auto descriptors = describe(object.get(), request);
    request->touch();
    // Resolve directory paths before sorting. A provider may omit implicit
    // parents or put children before directories; original lindex is retained.
    std::map<std::wstring, Descriptor> declared;
    for (auto descriptor : descriptors) {
      FileTree::require(declared.emplace(descriptor.path, descriptor).second,
                        "Duplicate virtual path");
    }
    for (const auto &descriptor : descriptors) {
      auto path = descriptor.path;
      for (auto slash = path.find_last_of(L'/'); slash != std::wstring::npos;
           slash = path.find_last_of(L'/')) {
        path.resize(slash);
        auto found = declared.find(path);
        if (found == declared.end())
          declared.emplace(path, Descriptor{path, true, 0, 0});
        else
          FileTree::require(found->second.directory, "Virtual path has a file parent");
      }
    }
    Json manifest = Json::array();
    for (auto &[path, descriptor] : declared) {
      // A preliminary zero size validates all paths/types/counts without
      // requesting any contents, including adversarial absolute/ADS paths.
      manifest.push_back({{"id", manifest.size()},
                          {"name", utf8(path)},
                          {"kind", descriptor.directory ? "directory" : "file"},
                          {"size", descriptor.size.value_or(0)}});
    }
    FileTree::parse(manifest);
    for (auto &[path, descriptor] : declared) {
      if (!descriptor.size) {
        Reader probe(object.get(), descriptor.index, std::nullopt, request);
        descriptor.size = probe.length();
        request->touch();
      }
      entries.push_back({entries.size(), path, descriptor.directory, *descriptor.size});
      mapping.push_back(descriptor.directory ? std::nullopt
                                             : std::optional<Descriptor>(descriptor));
    }
    FileTree::parse(FileTree::metadata(entries));
    request->touch();
  }
  void begin(size_t id) override {
    FileTree::require(id < mapping.size() && mapping[id].has_value(), "Invalid virtual-file index");
    auto &entry = *mapping[id];
    reader = std::make_unique<Reader>(object.get(), entry.index, entry.size, request);
    request->touch();
  }
  size_t read(uint8_t *bytes, size_t length) override {
    auto n = reader->read(bytes, length);
    request->touch();
    return n;
  }
  void end() override {
    reader.reset();
    request->touch();
  }
};

inline void drain() {
  std::vector<std::shared_ptr<Request>> completed;
  for (auto it = requests.begin(); it != requests.end();) {
    if (it->second->done.load(std::memory_order_acquire)) {
      completed.push_back(it->second);
      it = requests.erase(it);
    } else
      ++it;
  }
  for (auto &request : completed)
    request->releaseUi();
}
inline void tick() {
  for (const auto &[id, request] : requests)
    if (!request->done && !request->cancelled && request->started &&
        GetTickCount64() - request->progress.load() > 30000) {
      std::cerr << "Virtual file source timed out\n";
      request->cancel();
    }
  drain();
}
inline void cancel() {
  for (const auto &[id, request] : requests)
    if (!request->done)
      request->cancel();
}
inline std::shared_ptr<Request> submit(IDataObject *data, Json drop = Json::object(),
                                       HWND target = nullptr) {
  drain();
  if (!supports(data) || requests.size() >= 3)
    return {};
  auto request = std::make_shared<Request>(++nextRequest);
  request->target = target;
  request->clipboard = !drop.value("drop", false);
  request->selectionEpoch = FileTree::epoch.load();
  if (!request->finished)
    return {};
  data->AddRef();
  request->original = data;
  try {
    if (SUCCEEDED(data->QueryInterface(IID_PPV_ARGS(&request->async)))) {
      BOOL mode = FALSE;
      check(request->async->GetAsyncMode(&mode), "Get virtual async mode");
      if (mode) {
        check(request->async->StartOperation(nullptr), "Start virtual-file operation");
        request->asyncStarted = true;
      }
    }
    check(CoMarshalInterThreadInterfaceInStream(IID_IDataObject, data, &request->marshaled),
          "Marshal virtual source");
    requests.emplace(request->id, request);
    FileTree::UploadRequest upload{{},
                                   std::move(drop),
                                   FileTree::epoch.load(),
                                   [request] { return std::make_unique<Input>(request); },
                                   [request](bool success) { request->complete(success); },
                                   [request] {
                                     return !request->cancelled.load() &&
                                            (!request->target || IsWindow(request->target));
                                   },
                                   request->id};
    if (!FileTree::enqueue(std::move(upload))) {
      requests.erase(request->id);
      request->releaseUi();
      return {};
    }
    return request;
  } catch (const std::exception &error) {
    std::cerr << "Virtual file import: " << error.what() << '\n';
    request->releaseUi();
    return {};
  }
}
inline unsigned synchronousWaits = 0;
inline void cancelDrops() {
  for (const auto &[id, request] : requests)
    if (!request->clipboard && !request->done)
      request->cancel();
}
inline bool wait(const std::shared_ptr<Request> &request) {
  if (!request)
    return false;
  struct Waiting {
    Waiting() { ++synchronousWaits; }
    ~Waiting() { --synchronousWaits; }
  } waiting;
  while (!request->done.load(std::memory_order_acquire)) {
    DWORD index = 0;
    HANDLE event = request->finished;
    HRESULT hr = CoWaitForMultipleHandles(COWAIT_DISPATCH_CALLS | COWAIT_DISPATCH_WINDOW_MESSAGES |
                                              COWAIT_INPUTAVAILABLE,
                                          50, 1, &event, &index);
    if (FAILED(hr) && hr != RPC_S_CALLPENDING) {
      request->cancel();
    }
    // Some OLE modal loops (including Wine) leave ordinary keyboard/window
    // messages queued despite COWAIT_DISPATCH_WINDOW_MESSAGES. Pump a bounded
    // batch so Escape and target closure remain responsive during a sync Drop.
    MSG message{};
    for (unsigned i = 0; i < 64 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++i) {
      if (message.message == WM_QUIT) {
        request->cancel();
        PostQuitMessage(static_cast<int>(message.wParam));
        break;
      }
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    tick();
  }
  drain();
  return request->success;
}
inline bool clipboard() {
  if (!FileTree::supported)
    return false;
  Com<IDataObject> object;
  if (FAILED(OleGetClipboard(object.out())) || !supports(object.get()))
    return false;
  FORMATETC drop{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  if (SUCCEEDED(object->QueryGetData(&drop)))
    return false;
  return submit(object.get()) != nullptr;
}
inline void finishWorkers() {
  // Marshaled clipboard wrappers may call this STA while a worker unwinds.
  // Keep COM dispatch alive until every source/medium has been released;
  // blocking thread joins here would deadlock such providers.
  cancel();
  std::vector<std::shared_ptr<Request>> pending;
  for (const auto &[id, request] : requests)
    pending.push_back(request);
  for (const auto &request : pending)
    wait(request);
}
inline void shutdown() {
  cancel();
  drain();
  // Called after the worker joined and before OleUninitialize.
  for (auto &[id, request] : requests) {
    request->success = false;
    request->releaseUi();
  }
  requests.clear();
}
} // namespace VirtualFiles
