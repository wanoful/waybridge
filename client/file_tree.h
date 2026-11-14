#pragma once
// Shared portable tree manifest, staged clipboard download and directory upload.
// Included after Json/UTF-8/send helpers; callers own the UI thread inbox.
#include <functional>
#include <optional>
#include <set>
#include <stdexcept>
#include "file_io.h"

namespace FileTree {
constexpr size_t MaxEntries = 512, MaxDepth = 16, MaxRoots = 128, MaxPath = 240;
constexpr uint64_t MaxFile = 1024ULL * 1024 * 1024, MaxTotal = 8 * MaxFile;
inline bool supported = false;
inline std::function<void(const Json &)> activity;
inline std::atomic<uint64_t> epoch{0};
inline std::atomic<bool> downloadsEnabled{true};
inline std::atomic<uint64_t> latestIncoming{0}, serverGeneration{0}, blockedGeneration{0},
                             publishedGeneration{0};
inline uint64_t appliedGeneration = 0;
inline DWORD appliedClipboard = 0;
struct Entry {
  size_t id;
  std::wstring name;
  bool directory;
  uint64_t size;
};
inline void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
inline bool validName(const std::wstring &name) {
  if (name.empty() || name.size() > MaxPath || utf8(name).size() > MaxPath ||
      wide(utf8(name)) != name || name == L"." || name == L".." || name.back() == L'.' ||
      name.back() == L' ' || name.find_first_of(L"<>:\"/\\|?*") != std::wstring::npos ||
      std::any_of(name.begin(), name.end(),
                  [](wchar_t c) { return c < 32 || (c >= 0x7f && c <= 0x9f); }))
    return false;
  auto stem = name.substr(0, name.find(L'.'));
  while (!stem.empty() && stem.back() == L' ')
    stem.pop_back();
  std::transform(stem.begin(), stem.end(), stem.begin(),
                 [](wchar_t c) { return c >= L'a' && c <= L'z' ? c - L'a' + L'A' : c; });
  if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" || stem == L"CONIN$" ||
      stem == L"CONOUT$")
    return false;
  if (stem.size() == 4 && (stem.substr(0, 3) == L"COM" || stem.substr(0, 3) == L"LPT"))
    if ((stem[3] >= L'1' && stem[3] <= L'9') || stem[3] == L'\u00b9' || stem[3] == L'\u00b2' ||
        stem[3] == L'\u00b3')
      return false;
  return true;
}
inline std::vector<Entry> parse(const Json &metadata) {
  require(metadata.is_array() && !metadata.empty() && metadata.size() <= MaxEntries,
          "Invalid tree entry count");
  std::vector<Entry> entries;
  std::map<std::wstring, bool> paths;
  size_t roots = 0;
  uint64_t total = 0;
  for (const auto &item : metadata) {
    Entry e{item.at("id").get<size_t>(), wide(item.at("name").get<std::string>()),
            item.at("kind") == "directory", item.at("size").get<uint64_t>()};
    require(item.at("kind") == "file" || item.at("kind") == "directory", "Unknown tree entry type");
    require(e.id == entries.size() && e.name.size() <= MaxPath && e.size <= MaxFile &&
                (!e.directory || !e.size),
            "Invalid tree metadata");
    size_t start = 0, depth = 0;
    for (;;) {
      size_t slash = e.name.find(L'/', start);
      require(
          validName(e.name.substr(start, slash == std::wstring::npos ? slash : slash - start)) &&
              ++depth <= MaxDepth,
          "Invalid relative tree path");
      if (slash == std::wstring::npos)
        break;
      start = slash + 1;
    }
    auto slash = e.name.find_last_of(L'/');
    if (slash == std::wstring::npos)
      require(++roots <= MaxRoots, "Too many tree roots");
    else {
      auto parent = paths.find(e.name.substr(0, slash));
      require(parent != paths.end() && parent->second, "Missing directory parent");
    }
    for (const auto &other : entries)
      require(CompareStringOrdinal(e.name.c_str(), -1, other.name.c_str(), -1, TRUE) != CSTR_EQUAL,
              "Case-insensitive tree collision");
    require(e.size <= MaxTotal - total, "Tree byte limit exceeded");
    total += e.size;
    paths.emplace(e.name, e.directory);
    entries.push_back(std::move(e));
  }
  return entries;
}
inline std::wstring nativePath(std::wstring path) {
  std::replace(path.begin(), path.end(), L'/', L'\\');
  return path;
}
inline Json metadata(const std::vector<Entry> &entries) {
  Json result = Json::array();
  for (const auto &e : entries) {
    auto name = utf8(e.name);
    require(wide(name) == e.name, "Invalid Unicode tree path");
    result.push_back({{"id", e.id},
                      {"name", std::move(name)},
                      {"kind", e.directory ? "directory" : "file"},
                      {"size", e.size}});
  }
  return result;
}
struct Handle {
  HANDLE value = INVALID_HANDLE_VALUE;
  Handle() = default;
  explicit Handle(HANDLE h) : value(h) {}
  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;
  Handle(Handle &&other) noexcept : value(other.value) { other.value = INVALID_HANDLE_VALUE; }
  Handle &operator=(Handle &&other) noexcept {
    if (value != INVALID_HANDLE_VALUE)
      CloseHandle(value);
    value = other.value;
    other.value = INVALID_HANDLE_VALUE;
    return *this;
  }
  ~Handle() {
    if (value != INVALID_HANDLE_VALUE)
      CloseHandle(value);
  }
};
struct Source {
  std::vector<Entry> entries;
  std::vector<Handle> handles;
  uint64_t total = 0;
  void visit(const std::wstring &path, const std::wstring &relative) {
    require(entries.size() < MaxEntries && relative.size() <= MaxPath &&
                std::count(relative.begin(), relative.end(), L'/') < static_cast<long>(MaxDepth),
            "Tree path/entry/depth limit exceeded");
    // Denying delete access pins each directory while enumerating children.
    // Reparse points are opened, inspected, and rejected rather than followed.
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    require(file.value != INVALID_HANDLE_VALUE, "Cannot open tree source (it may be in use)");
    BY_HANDLE_FILE_INFORMATION info{};
    require(GetFileInformationByHandle(file.value, &info) &&
                GetFileType(file.value) == FILE_TYPE_DISK &&
                !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)),
            "Reparse points and special files are unsupported");
    bool directory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    uint64_t size = directory ? 0 : (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    require(size <= MaxFile && size <= MaxTotal - total, "Tree byte limit exceeded");
    total += size;
    for (const auto &other : entries)
      require(CompareStringOrdinal(relative.c_str(), -1, other.name.c_str(), -1, TRUE) !=
                  CSTR_EQUAL,
              "Case-insensitive tree collision");
    entries.push_back({entries.size(), relative, directory, size});
    handles.push_back(std::move(file));
    if (!directory)
      return;
    WIN32_FIND_DATAW data{};
    HANDLE found = FindFirstFileW((path + L"\\*").c_str(), &data);
    if (found == INVALID_HANDLE_VALUE) {
      require(GetLastError() == ERROR_FILE_NOT_FOUND, "Cannot enumerate directory");
      return;
    }
    try {
      do {
        std::wstring name = data.cFileName;
        if (name == L"." || name == L"..")
          continue;
        require(validName(name), "Unsupported Windows filename");
        visit(path + L"\\" + name, relative + L"/" + name);
      } while (FindNextFileW(found, &data));
      require(GetLastError() == ERROR_NO_MORE_FILES, "Directory enumeration failed");
    } catch (...) {
      FindClose(found);
      throw;
    }
    FindClose(found);
  }
  explicit Source(const std::vector<std::wstring> &paths) {
    require(!paths.empty() && paths.size() <= MaxRoots, "Invalid tree root count");
    for (auto path : paths) {
      while (path.size() > 3 && (path.back() == L'/' || path.back() == L'\\'))
        path.pop_back();
      auto slash = path.find_last_of(L"/\\");
      auto name = path.substr(slash == std::wstring::npos ? 0 : slash + 1);
      require(validName(name), "Invalid tree root name");
      visit(path, name);
    }
    parse(metadata(entries));
  }
};

// Importers provide a bounded sequential reader. The same worker/protocol owns
// filesystem handles and IDataObject streams, with publication only on commit.
struct Input {
  std::vector<Entry> entries;
  virtual ~Input() = default;
  virtual void begin(size_t id) = 0;
  virtual size_t read(uint8_t *bytes, size_t length) = 0;
  virtual void end() = 0;
};
struct FileInput final : Input {
  Source source;
  size_t current = 0;
  explicit FileInput(const std::vector<std::wstring> &paths) : source(paths) {
    entries = source.entries;
  }
  void begin(size_t id) override { current = id; }
  size_t read(uint8_t *bytes, size_t length) override {
    DWORD n = 0;
    require(ReadFile(source.handles[current].value, bytes, static_cast<DWORD>(length), &n, nullptr),
            "Cannot read file source");
    return n;
  }
  void end() override {}
};
struct Upload {
  std::mutex mutex;
  std::condition_variable wake;
  bool failed = false, clipboard = false;
  unsigned stage = 0;
  DWORD comThread = 0;
  uint64_t external = 0;
};
inline std::mutex uploadsMutex;
inline std::map<uint64_t, std::shared_ptr<Upload>> uploads;
struct UploadRequest {
  std::vector<std::wstring> paths;
  Json drop;
  uint64_t selectionEpoch;
  std::function<std::unique_ptr<Input>()> open;
  std::function<void(bool)> complete;
  std::function<bool()> current;
  uint64_t external = 0;
};
inline std::mutex requestsMutex;
inline std::condition_variable requestsWake;
inline std::deque<UploadRequest> requests;
inline std::atomic<bool> uploadStopping{false};
inline bool uploadWorkerStarted = false;
inline bool uploadResponse(const Json &message) {
  auto type = message.value("type", "");
  if (type != "tree_upload_ready" && type != "tree_upload_error")
    return false;
  if (activity) activity({{"type", "transfer"}, {"event", type},
                         {"batch", message.value("batch", uint64_t(0))},
                         {"stage", message.value("stage", "")},
                         {"message", message.value("message", "")}});
  std::shared_ptr<Upload> job;
  {
    std::lock_guard<std::mutex> lock(uploadsMutex);
    auto it = uploads.find(message.value("batch", uint64_t(0)));
    if (it != uploads.end())
      job = it->second;
  }
  if (job) {
    std::lock_guard<std::mutex> lock(job->mutex);
    if (type == "tree_upload_error")
      job->failed = true;
    if (!job->failed)
      job->stage = message.value("stage", "") == "commit" ? 2 : 1;
    job->wake.notify_all();
  }
  if (type == "tree_upload_error")
    std::cerr << "Tree upload: " << message.value("message", "failed") << '\n';
  return true;
}
inline void cancelComCall(DWORD thread) {
  // Wine currently exports an aborting stub for CoCancelCall. Let its finite
  // RPC reads finish and cancel at the next chunk boundary; Windows uses the
  // real API to interrupt an outstanding call as well.
  static bool wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
  if (!wine)
    CoCancelCall(thread, 0);
}
inline void cancelClipboardUploads() {
  std::vector<DWORD> calls;
  {
    std::lock_guard<std::mutex> lock(uploadsMutex);
    for (auto &[id, job] : uploads)
      if (job->clipboard) {
        std::lock_guard<std::mutex> guard(job->mutex);
        job->failed = true;
        job->wake.notify_all();
        if (job->comThread)
          calls.push_back(job->comThread);
      }
  }
  for (auto thread : calls)
    cancelComCall(thread);
  std::deque<UploadRequest> cancelled;
  {
    std::lock_guard<std::mutex> lock(requestsMutex);
    for (auto it = requests.begin(); it != requests.end();) {
      if (!it->drop.value("drop", false)) {
        cancelled.push_back(std::move(*it));
        it = requests.erase(it);
      } else
        ++it;
    }
  }
  for (auto &request : cancelled)
    if (request.complete)
      request.complete(false);
}
inline void cancelUploads() {
  uploadStopping = true;
  std::vector<DWORD> calls;
  {
    std::lock_guard<std::mutex> lock(uploadsMutex);
    for (auto &[id, job] : uploads) {
      std::lock_guard<std::mutex> guard(job->mutex);
      job->failed = true;
      job->wake.notify_all();
      if (job->comThread)
        calls.push_back(job->comThread);
    }
  }
  for (auto thread : calls)
    cancelComCall(thread);
  std::deque<UploadRequest> cancelled;
  {
    std::lock_guard<std::mutex> lock(requestsMutex);
    cancelled.swap(requests);
  }
  for (auto &request : cancelled)
    if (request.complete)
      request.complete(false);
  requestsWake.notify_all();
}
inline void cancelRequest(uint64_t external) {
  if (!external)
    return;
  std::vector<DWORD> calls;
  {
    std::lock_guard<std::mutex> lock(uploadsMutex);
    for (auto &[id, job] : uploads)
      if (job->external == external) {
        std::lock_guard<std::mutex> guard(job->mutex);
        job->failed = true;
        job->wake.notify_all();
        if (job->comThread)
          calls.push_back(job->comThread);
      }
  }
  std::deque<UploadRequest> cancelled;
  {
    std::lock_guard<std::mutex> lock(requestsMutex);
    for (auto it = requests.begin(); it != requests.end();) {
      if (it->external == external) {
        cancelled.push_back(std::move(*it));
        it = requests.erase(it);
      } else
        ++it;
    }
  }
  for (auto thread : calls)
    cancelComCall(thread);
  for (auto &request : cancelled)
    if (request.complete)
      request.complete(false);
}
inline void performUpload(UploadRequest request) {
  uint64_t batch = ++nextUpload;
  auto job = std::make_shared<Upload>();
  job->clipboard = !request.drop.value("drop", false);
  job->external = request.external;
  auto valid = [&] {
    return running && !uploadStopping && (!request.current || request.current()) &&
           (!job->clipboard || request.selectionEpoch == epoch.load());
  };
  bool began = false, success = false;
  auto wait = [&](unsigned stage) {
    std::unique_lock<std::mutex> lock(job->mutex);
    require(job->wake.wait_for(lock, std::chrono::seconds(10),
                               [&] { return job->failed || job->stage >= stage || !valid(); }) &&
                !job->failed && valid(),
            "Tree upload failed or timed out");
  };
  try {
    require(valid(), "Tree upload superseded");
    job->comThread = request.open ? GetCurrentThreadId() : 0;
    {
      std::lock_guard<std::mutex> lock(uploadsMutex);
      uploads.emplace(batch, job);
    }
    auto source = request.open ? request.open() : std::make_unique<FileInput>(request.paths);
    parse(metadata(source->entries));
    require(valid(), "Tree upload superseded");
    sendJson({{"type", "tree_upload_begin"},
              {"batch", batch},
              {"drop", !job->clipboard},
              {"epoch", request.selectionEpoch},
              {"entries", metadata(source->entries)}});
    began = true;
    wait(1);
    std::vector<uint8_t> bytes(65536);
    for (const auto &entry : source->entries) {
      if (entry.directory)
        continue;
      source->begin(entry.id);
      uint64_t total = 0;
      for (;;) {
        {
          std::lock_guard<std::mutex> lock(job->mutex);
          require(!job->failed && valid(), "Tree upload cancelled");
        }
        size_t n = source->read(bytes.data(), bytes.size());
        require(n <= bytes.size(), "Invalid source read length");
        if (!n)
          break;
        total += n;
        require(total <= entry.size, "Source size changed");
        sendJson({{"type", "tree_upload_chunk"},
                  {"batch", batch},
                  {"file", entry.id},
                  {"data", base64(bytes.data(), n)}});
      }
      require(total == entry.size, "Source size changed");
      source->end();
      sendJson({{"type", "tree_upload_end"}, {"batch", batch}, {"file", entry.id}});
    }
    require(valid(), "Tree upload superseded");
    Json commit = request.drop;
    commit["type"] = "tree_upload_commit";
    commit["batch"] = batch;
    sendJson(commit);
    wait(2);
    success = true;
  } catch (const std::exception &e) {
    std::cerr << "Tree upload: " << e.what() << '\n';
    if (began && running && !uploadStopping)
      sendJson({{"type", "tree_upload_cancel"}, {"batch", batch}});
  }
  {
    std::lock_guard<std::mutex> lock(uploadsMutex);
    uploads.erase(batch);
  }
  if (request.complete)
    request.complete(success);
}
inline bool enqueue(UploadRequest request) {
  std::lock_guard<std::mutex> lock(requestsMutex);
  if (uploadStopping || requests.size() >= 2) {
    std::cerr << "File upload queue is full; retry the transfer\n";
    return false;
  }
  requests.push_back(std::move(request));
  if (!uploadWorkerStarted) {
    uploadWorkerStarted = true;
    fileWorkers.emplace_back([] {
      HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
      CoEnableCallCancellation(nullptr);
      for (;;) {
        UploadRequest request;
        {
          std::unique_lock<std::mutex> lock(requestsMutex);
          requestsWake.wait(lock, [] { return uploadStopping || !running || !requests.empty(); });
          if (uploadStopping || !running)
            break;
          request = std::move(requests.front());
          requests.pop_front();
        }
        performUpload(std::move(request));
      }
      CoDisableCallCancellation(nullptr);
      if (SUCCEEDED(initialized))
        CoUninitialize();
    });
  }
  requestsWake.notify_one();
  return true;
}
inline bool upload(std::vector<std::wstring> paths, Json drop, std::function<bool()> current = {}) {
  return enqueue({std::move(paths), std::move(drop), epoch.load(), {}, {}, std::move(current), 0});
}

inline void removeTree(const std::wstring &path) {
  DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES)
    return;
  if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
    DeleteFileW(path.c_str());
    return;
  }
  if (!(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
    WIN32_FIND_DATAW data{};
    HANDLE found = FindFirstFileW((path + L"\\*").c_str(), &data);
    if (found != INVALID_HANDLE_VALUE) {
      do {
        std::wstring name = data.cFileName;
        if (name != L"." && name != L"..")
          removeTree(path + L"\\" + name);
      } while (FindNextFileW(found, &data));
      FindClose(found);
    }
  }
  RemoveDirectoryW(path.c_str());
}
struct Incoming {
  struct File {
    Handle handle;
    uint64_t remaining;
  };
  uint64_t batch, selectionEpoch;
  DWORD clipboard;
  std::wstring directory;
  std::vector<Entry> entries;
  std::map<size_t, File> files;
  bool published = false;
  ~Incoming() {
    files.clear();
    if (!published && !directory.empty())
      removeTree(directory);
  }
};
inline std::unique_ptr<Incoming> incoming;
inline void cancelIncoming() { FileIo::cleanup([] { incoming.reset(); }, &incoming); }
inline void setDownloadsEnabled(bool enabled) {
  if (downloadsEnabled.exchange(enabled) == enabled) return;
  if (!enabled) { ++epoch; cancelIncoming(); cancelIncomingFiles(); }
  if (supported) sendJson({{"type", "file_tree_epoch"}, {"epoch", epoch.load()}});
}
inline void localClipboardChanged() {
  cancelIncoming();
  blockedGeneration = serverGeneration.load();
  publishedGeneration = 0;
  cancelClipboardUploads();
  if (supported) {
    ++epoch;
    sendJson({{"type", "file_tree_epoch"}, {"epoch", epoch.load()}});
  }
}
inline bool serverClipboard(uint64_t generation) {
  if (!generation)
    return true;
  if (generation < serverGeneration || generation <= blockedGeneration)
    return false;
  if (generation > serverGeneration) {
    serverGeneration = generation;
    publishedGeneration = 0;
    FileIo::cleanup([] {
      if (incoming && incoming->batch < serverGeneration) incoming.reset();
    }, &serverGeneration);
    cancelIncomingFilesExcept(generation);
  }
  return publishedGeneration != generation;
}
inline void clipboardApplied(uint64_t generation, DWORD sequence) {
  appliedGeneration = generation;
  appliedClipboard = sequence;
}
inline DWORD publicationSequence(uint64_t generation, DWORD original) {
  return generation && generation == appliedGeneration ? appliedClipboard : original;
}
inline bool receiveNow(const Json &message) {
  auto type = message.value("type", "");
  if (type != "tree_begin" && type != "tree_chunk" && type != "tree_done" && type != "tree_error")
    return false;
  auto batch = message.at("batch").get<uint64_t>();
  if (type == "tree_begin") {
    if (!supported || message.value("epoch", uint64_t(0)) != epoch || batch <= latestIncoming ||
        batch < serverGeneration || batch <= blockedGeneration)
      return true;
    latestIncoming = batch;
    cancelIncomingFiles();
    incoming.reset();
    auto state = std::make_unique<Incoming>();
    state->batch = batch;
    state->selectionEpoch = epoch;
    state->clipboard = GetClipboardSequenceNumber();
    try {
      state->entries = parse(message.at("entries"));
      wchar_t temp[MAX_PATH]{};
      DWORD length = GetTempPathW(MAX_PATH, temp);
      require(length && length < MAX_PATH, "Unsupported temporary directory path");
      auto directory = std::wstring(temp) + L"Waybridge-tree-" +
                       std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(batch) +
                       L"-" + std::to_wstring(GetTickCount64());
      require(CreateDirectoryW(directory.c_str(), nullptr), "Cannot create tree inbox");
      state->directory = directory;
      for (const auto &entry : state->entries) {
        auto path = directory + L"\\" + nativePath(entry.name);
        require(path.size() < MAX_PATH, "Tree destination exceeds Win32 path limit");
        if (entry.directory)
          require(CreateDirectoryW(path.c_str(), nullptr), "Cannot create incoming directory");
        else {
          Handle handle(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
          require(handle.value != INVALID_HANDLE_VALUE, "Cannot create incoming file");
          state->files.emplace(entry.id, Incoming::File{std::move(handle), entry.size});
        }
      }
      incoming = std::move(state);
    } catch (const std::exception &error) {
      std::cerr << "Tree download: " << error.what() << '\n';
    }
  } else if (incoming && incoming->batch == batch) {
    try {
      if (type == "tree_chunk") {
        auto &file = incoming->files.at(message.at("file").get<size_t>());
        const auto &encoded = message.at("data").get_ref<const std::string &>();
        require(encoded.size() <= 90000, "Tree chunk too large");
        auto bytes = unbase64(encoded);
        DWORD written = 0;
        require(!bytes.empty() && bytes.size() <= 65536 && bytes.size() <= file.remaining &&
                    WriteFile(file.handle.value, bytes.data(), static_cast<DWORD>(bytes.size()),
                              &written, nullptr) &&
                    written == bytes.size(),
                "Invalid tree chunk");
        file.remaining -= written;
      } else if (type == "tree_done") {
        for (const auto &[id, file] : incoming->files)
          require(!file.remaining, "Incomplete tree");
        incoming->files.clear();
        auto state = std::shared_ptr<Incoming>(incoming.release(), [](Incoming *item) {
          FileIo::cleanup([item] { delete item; });
        });
        FileIo::postUi([state] {
        try {
        const auto batch = state->batch;
        if (!downloadsEnabled || state->selectionEpoch != epoch || batch < serverGeneration ||
            batch <= blockedGeneration || batch < latestIncoming ||
            publicationSequence(batch, state->clipboard) != GetClipboardSequenceNumber()) return;
        std::vector<wchar_t> paths;
        for (const auto &entry : state->entries)
          if (entry.name.find(L'/') == std::wstring::npos) {
            auto path = state->directory + L"\\" + entry.name;
            paths.insert(paths.end(), path.begin(), path.end());
            paths.push_back(L'\0');
          }
        paths.push_back(L'\0');
        std::vector<uint8_t> bytes(sizeof(DROPFILES) + paths.size() * sizeof(wchar_t));
        DROPFILES header{};
        header.pFiles = sizeof(header);
        header.fWide = TRUE;
        memcpy(bytes.data(), &header, sizeof(header));
        memcpy(bytes.data() + sizeof(header), paths.data(), paths.size() * sizeof(wchar_t));
        auto memory = globalBytes(bytes.data(), bytes.size());
        require(memory != nullptr, "Cannot allocate file clipboard");
        if (!OpenClipboard(controller)) {
          GlobalFree(memory);
          throw std::runtime_error("Clipboard is busy");
        }
        EmptyClipboard();
        bool set = SetClipboardData(CF_HDROP, memory) != nullptr;
        CloseClipboard();
        if (!set) {
          GlobalFree(memory);
          throw std::runtime_error("Cannot publish tree clipboard");
        }
        ownClipboardSequence = GetClipboardSequenceNumber();
        state->published = true;
        publishedGeneration = batch;
        if (activity) activity({{"type", "transfer"}, {"event", "download_complete"},
                               {"batch", batch}, {"directory", utf8(state->directory)}});
        } catch (const std::exception &error) {
          std::cerr << "Tree publication: " << error.what() << '\n';
        }
        });
      } else {
        std::cerr << "Tree download: " << message.value("message", "failed") << '\n';
        incoming.reset();
      }
    } catch (const std::exception &error) {
      std::cerr << "Tree download: " << error.what() << '\n';
      incoming.reset();
    }
  }
  return true;
}
inline bool receive(const Json &message) {
  auto type = message.value("type", "");
  if (type != "tree_begin" && type != "tree_chunk" && type != "tree_done" && type != "tree_error")
    return false;
  if (!downloadsEnabled) return true;
  size_t bytes = message.value("_file_io_bytes", size_t(65536));
  require(FileIo::submit([message] { receiveNow(message); }, bytes), "File I/O queue full");
  return true;
}
} // namespace FileTree
