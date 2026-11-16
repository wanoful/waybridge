#pragma once
#include <optional>
#include <memory>
// OLE provider for a bounded normalized UTF-8 representation. It owns no drag
// state, writes no temporary file and revalidates the target after COM reentry.
namespace NativeTextOffer {
struct Source {
  std::string text;
  uint64_t gesture = 0, press = 0;
  ULONGLONG expires = 0;
};

inline std::optional<std::string> read(IDataObject *data, const std::function<bool()> &current) {
  FORMATETC format{CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  STGMEDIUM medium{};
  HRESULT read = data->GetData(&format, &medium);
  DragTrace::event("text.read", {{"hresult", read}, {"tymed", medium.tymed}});
  if (FAILED(read))
    return std::nullopt;
  std::string text;
  if (medium.tymed == TYMED_HGLOBAL && medium.hGlobal) {
    SIZE_T length = GlobalSize(medium.hGlobal);
    if (length >= sizeof(wchar_t) && length <= 2 * 1024 * 1024 + sizeof(wchar_t)) {
      auto value = static_cast<const wchar_t *>(GlobalLock(medium.hGlobal));
      if (value) {
        size_t count = 0, limit = length / sizeof(wchar_t);
        while (count < limit && value[count])
          ++count;
        if (count < limit) {
          int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, int(count), nullptr,
                                          0, nullptr, nullptr);
          if (bytes >= 0 && bytes <= 1024 * 1024 && (count == 0 || bytes > 0)) {
            text.resize(size_t(bytes));
            if (bytes)
              WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, int(count), text.data(),
                                  bytes, nullptr, nullptr);
          }
        }
        GlobalUnlock(medium.hGlobal);
      }
    }
  }
  ReleaseStgMedium(&medium);
  if (!current() || text.empty())
    return std::nullopt;
  return text;
}
} // namespace NativeTextOffer
