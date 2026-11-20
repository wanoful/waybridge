#pragma once
#include <charconv>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace RichClipboard {
constexpr size_t MaxBytes = 12 * 1024 * 1024;
constexpr size_t MaxHtmlHeader = 4096;
constexpr std::string_view FragmentStart = "<!--StartFragment-->";
constexpr std::string_view FragmentEnd = "<!--EndFragment-->";

inline std::optional<size_t> field(std::string_view header, std::string_view name) {
  while (!header.empty()) {
    auto end = header.find_first_of("\r\n");
    auto line = header.substr(0, end);
    if (line.compare(0, name.size(), name) == 0) {
      auto value = line.substr(name.size());
      size_t begin = value.find_first_not_of(" \t");
      if (begin == std::string_view::npos) return {};
      value.remove_prefix(begin);
      size_t number = 0;
      auto [rest, error] = std::from_chars(value.data(), value.data() + value.size(), number);
      if (error == std::errc{} && rest == value.data() + value.size()) return number;
      return {};
    }
    if (end == std::string_view::npos) break;
    header.remove_prefix(end + 1);
  }
  return {};
}

// CF_HTML offsets are byte positions, even when the fragment contains Unicode.
inline std::string_view decodeHtml(std::string_view data) {
  if (data.size() > MaxBytes + MaxHtmlHeader || data.compare(0, 8, "Version:") != 0) return {};
  const auto header = data.substr(0, MaxHtmlHeader);
  auto first = field(header, "StartFragment:");
  auto last = field(header, "EndFragment:");
  if (!first || !last || *first >= *last || *last > data.size() || *last - *first > MaxBytes) return {};
  auto htmlBegin = field(header, "StartHTML:");
  auto htmlEnd = field(header, "EndHTML:");
  // Keep the HTML document when the producer marked the selection in it.
  // Some producers supply only fragment offsets, with StartHTML=-1.
  if (htmlBegin && htmlEnd && *htmlBegin <= *first && *last <= *htmlEnd &&
      *htmlEnd <= data.size() && *htmlEnd - *htmlBegin <= MaxBytes) {
    auto document = data.substr(*htmlBegin, *htmlEnd - *htmlBegin);
    size_t start = document.find(FragmentStart);
    size_t end = document.find(FragmentEnd);
    if (start != std::string_view::npos && end != std::string_view::npos &&
        *htmlBegin + start + FragmentStart.size() == *first && *htmlBegin + end == *last)
      return document;
  }
  return data.substr(*first, *last - *first);
}

inline std::string encodeHtml(std::string_view html) {
  if (html.empty() || html.size() > MaxBytes || html.find('\0') != std::string_view::npos) return {};
  std::string wrapped;
  size_t first = html.find(FragmentStart);
  size_t last = html.find(FragmentEnd);
  if (first == std::string_view::npos || last == std::string_view::npos || first + FragmentStart.size() > last) {
    wrapped.reserve(html.size() + FragmentStart.size() + FragmentEnd.size() + 26);
    wrapped.append("<html><body>").append(FragmentStart).append(html).append(FragmentEnd).append("</body></html>");
    html = wrapped;
    first = html.find(FragmentStart);
    last = html.rfind(FragmentEnd);
  }
  first += FragmentStart.size();
  constexpr char Header[] = "Version:1.0\r\nStartHTML:%010u\r\nEndHTML:%010u\r\nStartFragment:%010u\r\nEndFragment:%010u\r\n";
  char header[160];
  unsigned headerBytes = unsigned(std::snprintf(header, sizeof(header), Header, 0u, 0u, 0u, 0u));
  std::snprintf(header, sizeof(header), Header, headerBytes, headerBytes + unsigned(html.size()),
                headerBytes + unsigned(first), headerBytes + unsigned(last));
  return std::string(header, headerBytes).append(html);
}

inline bool validRtf(std::string_view rtf) {
  return rtf.size() <= MaxBytes && rtf.compare(0, 5, "{\\rtf") == 0;
}
} // namespace RichClipboard
