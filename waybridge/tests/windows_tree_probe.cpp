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
    DestroyWindow(controller);
    FileTree::cancelUploads();
    std::cout << "PASS: Win32 tree path/type/count/depth/byte validation; "
                 "partial/error/supersession/disconnect cleanup; complete roots retained, "
                 "directory clipboard and source enumeration"
              << std::endl;
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << std::endl;
    return 1;
  }
}
