// Adversarial tree validation and actual Win32 staging/cleanup on private Wine.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincrypt.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iostream>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "../client/third_party/json.hpp"
using Json = nlohmann::json;
static std::atomic<bool> running{true};
static std::atomic<uint64_t> nextUpload{1};
static std::vector<std::thread> fileWorkers;
static HWND controller;
static DWORD ownClipboardSequence;
static std::wstring wide(const std::string &s) {
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()),
                              nullptr, 0);
  std::wstring out(n, 0);
  if (n)
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()),
                        out.data(), n);
  return out;
}
static std::string utf8(const std::wstring &s) {
  int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr,
                              nullptr);
  std::string out(n, 0);
  if (n)
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr,
                        nullptr);
  return out;
}
static std::vector<uint8_t> unbase64(const std::string &s) {
  DWORD n = 0;
  if (!CryptStringToBinaryA(s.data(), static_cast<DWORD>(s.size()), CRYPT_STRING_BASE64, nullptr,
                            &n, nullptr, nullptr))
    return {};
  std::vector<uint8_t> bytes(n);
  if (!CryptStringToBinaryA(s.data(), static_cast<DWORD>(s.size()), CRYPT_STRING_BASE64,
                            bytes.data(), &n, nullptr, nullptr))
    return {};
  return bytes;
}
static std::string base64(const uint8_t *bytes, size_t n) {
  DWORD size = 0;
  CryptBinaryToStringA(bytes, static_cast<DWORD>(n), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                       nullptr, &size);
  std::string s(size, 0);
  CryptBinaryToStringA(bytes, static_cast<DWORD>(n), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                       s.data(), &size);
  s.resize(size);
  if (!s.empty() && !s.back())
    s.pop_back();
  return s;
}
static void sendJson(const Json &) {}
static void cancelIncomingFiles() {}
static void cancelIncomingFilesExcept(uint64_t) {}
static HGLOBAL globalBytes(const void *bytes, size_t size) {
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);
  if (!memory)
    return nullptr;
  auto p = GlobalLock(memory);
  if (!p) {
    GlobalFree(memory);
    return nullptr;
  }
  memcpy(p, bytes, size);
  GlobalUnlock(memory);
  return memory;
}
#include "../client/file_tree.h"

static Json entry(size_t id, const std::string &name, const char *kind, uint64_t size = 0) {
  return {{"id", id}, {"name", name}, {"kind", kind}, {"size", size}};
}
static bool exists(const std::wstring &path) {
  return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}
static void reject(const Json &entries) {
  bool rejected = false;
  try {
    FileTree::parse(entries);
  } catch (const std::exception &) {
    rejected = true;
  }
  FileTree::require(rejected, "Accepted adversarial tree manifest");
}
int wmain() {
  try {
    controller = CreateWindowExW(0, L"STATIC", L"Tree probe", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                 nullptr, nullptr);
    FileTree::require(controller != nullptr, "Cannot create clipboard owner");
    FileTree::supported = true;
    for (const auto *name : {"../escape", "/absolute", "C:x", "r\\x", "x:stream", "CON .txt", "NUL",
                             "LPT1", "x.", "x ", "r//x"})
      reject(Json::array({entry(0, name, "file")}));
    reject(Json::array(
        {entry(0, "r", "directory"), entry(1, "r/A", "file"), entry(2, "r/a", "file")}));
    reject(Json::array({entry(0, "r", "file"), entry(1, "r/x", "file")}));
    reject(Json::array({entry(0, "r/x", "file")}));
    reject(Json::array({entry(1, "r", "directory")}));
    reject(Json::array({entry(0, "r", "directory", 1)}));
    reject(Json::array({entry(0, "r", "file", FileTree::MaxFile + 1)}));
    Json total = Json::array();
    for (size_t i = 0; i < 9; ++i)
      total.push_back(entry(i, "f" + std::to_string(i), "file", FileTree::MaxFile));
    reject(total);
    Json count = Json::array();
    for (size_t i = 0; i <= FileTree::MaxEntries; ++i)
      count.push_back(entry(i, "f" + std::to_string(i), "file"));
    reject(count);
    Json depth = Json::array();
    std::string path;
    for (size_t i = 0; i < 17; ++i) {
      if (i)
        path += "/";
      path += "d";
      depth.push_back(entry(i, path, "directory"));
    }
    reject(depth);

    auto manifest = Json::array({entry(0, "r", "directory"), entry(1, "r/empty", "directory"),
                                 entry(2, "r/zero", "file"), entry(3, "r/data", "file", 3)});
    auto begin = [&](uint64_t batch) {
      FileTree::receive({{"type", "tree_begin"},
                         {"batch", batch},
                         {"epoch", FileTree::epoch.load()},
                         {"entries", manifest}});
      FileTree::require(FileTree::incoming != nullptr, "Tree begin failed");
      return FileTree::incoming->directory;
    };
    auto partial = begin(1);
    FileTree::receive({{"type", "tree_done"}, {"batch", 1}});
    FileTree::require(!exists(partial), "Incomplete tree leaked");
    auto superseded = begin(2);
    FileTree::localClipboardChanged();
    FileTree::require(!exists(superseded), "Superseded tree leaked");
    auto failed = begin(3);
    FileTree::receive(
        {{"type", "tree_error"}, {"batch", 3}, {"message", "expected fixture failure"}});
    FileTree::require(!exists(failed), "Failed tree leaked");
    auto complete = begin(4);
    FileTree::receive({{"type", "tree_chunk"}, {"batch", 4}, {"file", 3}, {"data", "YWJj"}});
    FileTree::receive({{"type", "tree_done"}, {"batch", 4}});
    FileTree::require(!FileTree::incoming && exists(complete + L"\\r\\empty") &&
                          exists(complete + L"\\r\\zero"),
                      "Completed tree not retained");
    FileTree::require(OpenClipboard(controller), "Cannot inspect clipboard");
    auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
    FileTree::require(drop && DragQueryFileW(drop, 0xffffffff, nullptr, 0) == 1,
                      "Clipboard exposed children rather than root");
    CloseClipboard();
    {
      FileTree::Source source({complete + L"\\r"});
      FileTree::require(source.entries.size() == 4 &&
                            source.entries[1].name.find(L'/') != std::wstring::npos,
                        "Win32 source tree lost hierarchy");
    }
    auto disconnected = begin(5);
    FileTree::cancelIncoming();
    FileTree::require(!exists(disconnected) && exists(complete),
                      "Disconnect cleanup removed committed files");
    auto invalidChunk = begin(6);
    FileTree::receive({{"type", "tree_chunk"}, {"batch", 6}, {"file", 0}, {"data", "YWJj"}});
    FileTree::require(!exists(invalidChunk), "Directory accepted file bytes");
    FileTree::localClipboardChanged();
    FileTree::receive({{"type", "tree_begin"},
                       {"batch", 7},
                       {"epoch", FileTree::epoch.load() - 1},
                       {"entries", manifest}});
    FileTree::require(!FileTree::incoming, "Accepted stale clipboard epoch");
    if (OpenClipboard(controller)) {
      EmptyClipboard();
      CloseClipboard();
    }
    FileTree::removeTree(complete);

    // The production path stages on the file worker and publishes only on this
    // STA. Stall that worker to exercise responsiveness and late cancellation.
    FileIo::start([] {});
    auto barrier = [] {
      auto promise = std::make_shared<std::promise<void>>();
      auto future = promise->get_future();
      FileIo::cleanup([promise] { promise->set_value(); });
      FileTree::require(future.wait_for(std::chrono::seconds(3)) == std::future_status::ready,
                        "File worker did not complete");
    };
    auto flush = [&] { barrier(); FileIo::drain(); barrier(); };
    FileTree::receive({{"type", "tree_begin"}, {"batch", 8},
                       {"epoch", FileTree::epoch.load()}, {"entries", manifest}});
    flush();
    FileTree::require(FileTree::incoming != nullptr, "Async staging failed");
    auto committed = FileTree::incoming->directory;
    FileTree::receive({{"type", "tree_chunk"}, {"batch", 8}, {"file", 3}, {"data", "YWJj"}});
    FileTree::receive({{"type", "tree_done"}, {"batch", 8}});
    flush();
    FileTree::require(FileTree::publishedGeneration == 8 && exists(committed + L"\\r\\data"),
                      "Async clipboard publication failed");
    FileTree::receive({{"type", "tree_begin"}, {"batch", 9},
                       {"epoch", FileTree::epoch.load()}, {"entries", manifest}});
    flush();
    auto cancelled = FileTree::incoming->directory;
    auto stalled = std::make_shared<std::promise<void>>();
    auto entered = stalled->get_future();
    FileIo::submit([stalled] {
      stalled->set_value();
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
    });
    FileTree::require(entered.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
                      "Stalled I/O fixture did not start");
    auto started = std::chrono::steady_clock::now();
    FileTree::receive({{"type", "tree_chunk"}, {"batch", 9}, {"file", 3}, {"data", "YWJj"}});
    FileTree::receive({{"type", "tree_done"}, {"batch", 9}});
    FileTree::localClipboardChanged();
    for (unsigned i = 0; i < 10000; ++i) FileTree::cancelIncoming();
    {
      std::lock_guard<std::mutex> lock(FileIo::mutex);
      FileTree::require(FileIo::tasks.size() < 10, "Repeated cancellation grew an unbounded queue");
    }
    FileTree::require(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(100),
                      "UI waited for filesystem worker");
    flush();
    FileTree::require(!exists(cancelled) && exists(committed),
                      "Async supersession leaked staging or removed committed files");
    FileTree::receive({{"type", "tree_begin"}, {"batch", 10},
                       {"epoch", FileTree::epoch.load()}, {"entries", manifest}});
    flush();
    auto late = FileTree::incoming->directory;
    FileTree::receive({{"type", "tree_chunk"}, {"batch", 10}, {"file", 3}, {"data", "YWJj"}});
    FileTree::receive({{"type", "tree_done"}, {"batch", 10}});
    barrier(); // staged result exists, but its UI completion has not run
    FileTree::localClipboardChanged();
    flush();
    FileTree::require(!exists(late) && FileTree::publishedGeneration != 10,
                      "Late completion overwrote a newer clipboard");
    FileTree::receive({{"type", "tree_begin"}, {"batch", 11},
                       {"epoch", FileTree::epoch.load()}, {"entries", manifest}});
    flush();
    auto announced = FileTree::incoming->directory;
    FileTree::require(FileTree::serverClipboard(11), "New remote clipboard was rejected");
    FileTree::require(OpenClipboard(controller), "Cannot update remote clipboard announcement");
    EmptyClipboard(); CloseClipboard();
    ownClipboardSequence = GetClipboardSequenceNumber();
    FileTree::clipboardApplied(11, ownClipboardSequence);
    FileTree::receive({{"type", "tree_chunk"}, {"batch", 11}, {"file", 3}, {"data", "YWJj"}});
    FileTree::receive({{"type", "tree_done"}, {"batch", 11}});
    flush();
    FileTree::require(FileTree::publishedGeneration == 11 && exists(announced + L"\\r\\data"),
                      "Matching remote clipboard announcement cancelled its own download");
    FileTree::receive({{"type", "tree_begin"}, {"batch", 12},
                       {"epoch", FileTree::epoch.load()}, {"entries", manifest}});
    flush();
    auto disabled = FileTree::incoming->directory;
    FileTree::receive({{"type", "tree_chunk"}, {"batch", 12}, {"file", 3}, {"data", "YWJj"}});
    FileTree::receive({{"type", "tree_done"}, {"batch", 12}});
    barrier();
    FileTree::setDownloadsEnabled(false);
    FileTree::setDownloadsEnabled(true);
    flush();
    FileTree::require(!exists(disabled) && FileTree::publishedGeneration != 12,
                      "Disabling sharing did not invalidate an already staged result");
    FileTree::cancelIncoming();
    FileIo::stop();
    std::atomic<unsigned> failures{0};
    FileIo::start([] {}, [&] { ++failures; });
    FileIo::submit([] {
      for (unsigned i = 0; i < 130; ++i) FileIo::postUi([] {});
    });
    barrier();
    FileTree::require(failures == 1 && !FileIo::submit([] {}),
                      "Completion overflow did not explicitly fail and stop admission");
    FileIo::stop();
    FileTree::removeTree(committed);
    FileTree::removeTree(announced);
    DestroyWindow(controller);
    FileTree::cancelUploads();
    std::cout << "PASS: Win32 tree path/type/count/depth/byte validation; "
                 "partial/error/supersession/disconnect cleanup; complete roots retained, "
                 "directory clipboard and source enumeration; async staging, responsive STA, "
                 "late-completion cancellation and committed-file lifetime"
              << std::endl;
    return 0;
  } catch (const std::exception &error) {
    running = false;
    FileTree::cancelIncoming();
    FileIo::stop();
    std::cerr << error.what() << std::endl;
    return 1;
  }
}
