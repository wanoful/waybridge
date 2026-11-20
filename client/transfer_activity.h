#pragma once
#include <algorithm>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace TransferActivity {

struct Progress {
  std::string name;
  uint64_t total, bytes = 0;
  ULONGLONG emitted = 0;
  bool cancellable = true;
};

using Key = std::pair<std::string, uint64_t>;
inline std::mutex mutex;
inline std::map<Key, Progress> current;
inline std::function<void(const Json &)> report;

inline void emit(const Key &key, const Progress &progress, const char *state, const std::string &detail = {}) {
  if (!report) return;
  Json message = {{"type", "transfer"}, {"event", "progress"}, {"kind", key.first},
                  {"id", key.second}, {"state", state}, {"name", progress.name},
                  {"bytes", progress.bytes}, {"total", progress.total},
                  {"can_cancel", progress.cancellable}};
  if (!detail.empty()) message[std::string(state) == "done" ? "directory" : "message"] = detail;
  report(message);
}

inline void begin(const char *kind, uint64_t id, std::string name, uint64_t total) {
  std::lock_guard<std::mutex> lock(mutex);
  Key key{kind, id};
  auto [entry, added] = current.try_emplace(key, Progress{std::move(name), total});
  if (added) emit(key, entry->second, "active");
}

inline void describe(const char *kind, uint64_t id, std::string name, uint64_t total) {
  std::lock_guard<std::mutex> lock(mutex);
  auto entry = current.find({kind, id});
  if (entry == current.end()) return;
  entry->second.name = std::move(name);
  entry->second.total = total;
  emit(entry->first, entry->second, "active");
}

inline void committing(const char *kind, uint64_t id) {
  std::lock_guard<std::mutex> lock(mutex);
  auto entry = current.find({kind, id});
  if (entry == current.end()) return;
  entry->second.cancellable = false;
  emit(entry->first, entry->second, "active");
}

inline void advance(const char *kind, uint64_t id, uint64_t bytes) {
  std::lock_guard<std::mutex> lock(mutex);
  auto it = current.find({kind, id});
  if (it == current.end()) return;
  auto &progress = it->second;
  progress.bytes += std::min(bytes, progress.total - progress.bytes);
  auto now = GetTickCount64();
  if (now - progress.emitted >= 250) {
    progress.emitted = now;
    emit(it->first, progress, "active");
  }
}

inline bool finish(const char *kind, uint64_t id, const char *state, const std::string &detail = {}) {
  std::lock_guard<std::mutex> lock(mutex);
  auto entry = current.find({kind, id});
  if (entry == current.end()) return false;
  if (std::string_view(state) == "done") entry->second.bytes = entry->second.total;
  emit(entry->first, entry->second, state, detail);
  current.erase(entry);
  return true;
}

inline bool active(const char *kind, uint64_t id) {
  std::lock_guard<std::mutex> lock(mutex);
  return current.count({kind, id}) != 0;
}

inline void failAll(const std::string &reason) {
  std::lock_guard<std::mutex> lock(mutex);
  for (const auto &[key, progress] : current)
    emit(key, progress, "failed", reason);
  current.clear();
}

}
