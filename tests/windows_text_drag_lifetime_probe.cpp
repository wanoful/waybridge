#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <ole2.h>
#include <wincrypt.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <functional>
#include <iostream>
#include <string>
#include <vector>
#include "third_party/json.hpp"
using Json = nlohmann::json;
static std::vector<Json> sent;
static void sendJson(const Json &value) { sent.push_back(value); }
static std::string base64(const uint8_t *bytes, size_t length) {
  DWORD size = 0;
  if (!CryptBinaryToStringA(bytes, DWORD(length), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                            nullptr, &size))
    return {};
  std::string result(size, '\0');
  if (!CryptBinaryToStringA(bytes, DWORD(length), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                            result.data(), &size))
    return {};
  result.resize(size);
  if (!result.empty() && result.back() == 0)
    result.pop_back();
  return result;
}
#include "text_drag.h"
static void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
class MediumOwner final : public IUnknown {
  std::atomic<ULONG> refs{1};
  HGLOBAL memory;
  std::function<void()> callback;

public:
  MediumOwner(HGLOBAL value, std::function<void()> action)
      : memory(value), callback(std::move(action)) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (iid != IID_IUnknown)
      return E_NOINTERFACE;
    *out = this;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override {
    auto remaining = --refs;
    if (!remaining) {
      if (callback)
        callback();
      GlobalFree(memory);
      delete this;
    }
    return remaining;
  }
};
class Source final : public IDataObject {
  std::atomic<ULONG> refs{1};

public:
  std::wstring text;
  bool terminated = true;
  unsigned reads = 0;
  std::function<void()> query, get, release;
  explicit Source(std::wstring value) : text(std::move(value)) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (iid != IID_IUnknown && iid != IID_IDataObject)
      return E_NOINTERFACE;
    *out = this;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override {
    auto remaining = --refs;
    if (!remaining)
      delete this;
    return remaining;
  }
  HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC *format) override {
    if (query)
      query();
    return format && format->cfFormat == CF_UNICODETEXT ? S_OK : DV_E_FORMATETC;
  }
  HRESULT STDMETHODCALLTYPE GetData(FORMATETC *format, STGMEDIUM *medium) override {
    if (!format || !medium || format->cfFormat != CF_UNICODETEXT)
      return DV_E_FORMATETC;
    ++reads;
    if (get)
      get();
    size_t count = text.size() + (terminated ? 1 : 0);
    auto memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, count * sizeof(wchar_t));
    if (!memory)
      return E_OUTOFMEMORY;
    auto bytes = static_cast<wchar_t *>(GlobalLock(memory));
    if (!bytes) {
      GlobalFree(memory);
      return E_OUTOFMEMORY;
    }
    if (!terminated)
      std::fill_n(bytes, GlobalSize(memory) / sizeof(wchar_t), L'x');
    memcpy(bytes, text.data(), text.size() * sizeof(wchar_t));
    GlobalUnlock(memory);
    medium->tymed = TYMED_HGLOBAL;
    medium->hGlobal = memory;
    medium->pUnkForRelease = release ? new MediumOwner(memory, release) : nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC *, STGMEDIUM *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC *, FORMATETC *) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE SetData(FORMATETC *, STGMEDIUM *, BOOL) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD, IEnumFORMATETC **) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC *, DWORD, IAdviseSink *, DWORD *) override {
    return OLE_E_ADVISENOTSUPPORTED;
  }
  HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
  HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA **) override {
    return OLE_E_ADVISENOTSUPPORTED;
  }
};
static Json position = {{"id", 1u}, {"x", 10}, {"y", 20}};
static uint64_t begin(Source *source, uint64_t target) {
  return TextDrag::begin(source, position, target, [] { return true; });
}
static void fresh() {
  TextDrag::reset();
  sent.clear();
}
int main() {
  OleInitialize(nullptr);
  try {
    fresh();
    auto source = new Source(L"新值 ✓");
    auto target = TextDrag::enterTarget();
    auto id = begin(source, target);
    source->Release();
    std::string expected = u8"新值 ✓";
    require(id && sent.size() == 1 &&
                sent[0]["data"] ==
                    base64(reinterpret_cast<const uint8_t *>(expected.data()), expected.size()),
            "valid Unicode text rejected");
    fresh();
    source = new Source(L"old");
    target = TextDrag::enterTarget();
    source->get = [target] { TextDrag::leaveTarget(target); };
    require(begin(source, target) == 0 && !TextDrag::active && sent.empty(),
            "DragLeave resurrected a pending text enter");
    source->Release();
    fresh();
    source = new Source(L"old");
    target = TextDrag::enterTarget();
    source->get = [] { TextDrag::reset(); }; // Sharing off/on; the window remains alive.
    require(begin(source, target) == 0 && !TextDrag::active && sent.empty(),
            "policy reset failed to invalidate pending text");
    source->Release();
    for (int callback = 0; callback < 3; callback++) {
      fresh();
      source = new Source(L"old");
      target = TextDrag::enterTarget();
      uint64_t nested = 0, newer = 0;
      auto replacement = [&] {
        newer = TextDrag::enterTarget();
        auto next = new Source(L"replacement");
        nested = begin(next, newer);
        next->Release();
      };
      if (callback == 0)
        source->query = replacement;
      if (callback == 1)
        source->get = replacement;
      if (callback == 2)
        source->release = replacement;
      if (callback == 0)
        require(TextDrag::supports(source), "query format failed");
      require(begin(source, target) == 0 && nested && TextDrag::active == nested &&
                  sent.size() == 1,
              "old Query/GetData/ReleaseStgMedium callback overwrote newer text enter");
      TextDrag::leaveTarget(target);
      require(TextDrag::active == nested && TextDrag::currentTarget(newer),
              "old target leave canceled newer target");
      TextDrag::control({{"type", "text_drag_status"}, {"drag", nested}, {"accepted", true}});
      require(TextDrag::drop(nested, position) && !TextDrag::currentTarget(newer),
              "drop did not revoke target lease");
      source->Release();
    }
    for (int invalid = 0; invalid < 3; invalid++) {
      fresh();
      target = TextDrag::enterTarget();
      source = new Source(invalid == 0   ? std::wstring(1, wchar_t(0xd800))
                          : invalid == 1 ? std::wstring(400000, L'\u4e00')
                                         : L"unterminated");
      if (invalid == 2)
        source->terminated = false;
      require(begin(source, target) == 0 && !TextDrag::active && sent.empty(),
              "malformed/oversized/unterminated Unicode published");
      source->Release();
    }
    fresh();
    TextDrag::configure(true);
    source = new Source(L"coordinated");
    target = TextDrag::enterTarget();
    id = begin(source, target);
    source->Release();
    auto oldRevision = TextDrag::state.target;
    require(TextDrag::refresh(id, position, true), "offer refresh not issued");
    auto revision = TextDrag::state.target;
    TextDrag::control({{"type", "text_drag_status"},
                       {"drag", id},
                       {"target_epoch", oldRevision},
                       {"accepted", true}});
    require(!TextDrag::allowed(id), "old target revision accepted the new offer");
    unsigned pumps = 0;
    require(TextDrag::awaitAcceptance(
                id, target, [] { return true; },
                [&] {
                  ++pumps;
                  TextDrag::control({{"type", "text_drag_status"},
                                     {"drag", id},
                                     {"target_epoch", revision},
                                     {"accepted", pumps > 1},
                                     {"status", pumps > 1 ? "accepted" : "rejected"}});
                }),
            "transient asynchronous rejection prevented later acceptance");
    require(TextDrag::drop(id, position) && TextDrag::active == id,
            "source finished before target completion");
    TextDrag::control({{"type", "text_drag_finished"},
                       {"drag", id},
                       {"target_epoch", oldRevision},
                       {"success", true}});
    require(!TextDrag::state.terminal(), "stale completion finished current offer");
    require(TextDrag::awaitCompletion(
                id, target, [] { return true; },
                [&] {
                  TextDrag::control({{"type", "text_drag_finished"},
                                     {"drag", id},
                                     {"target_epoch", revision},
                                     {"success", true}});
                }),
            "target completion failed");
    fresh();
    source = new Source(L"cancel during wait");
    target = TextDrag::enterTarget();
    id = begin(source, target);
    source->Release();
    require(!TextDrag::awaitAcceptance(
                id, target, [] { return true; }, [] { TextDrag::reset(); }),
            "cancelled nested wait resurrected a session");
    require(!TextDrag::active, "cancelled session retained active ID");
    TextDrag::configure(false);
    // Different OLE wrappers still belong to the same physical press. A new
    // release/press must retire the snapshot even if the UI timer never ran.
    HWND owner = CreateWindowExW(0, L"STATIC", L"gesture probe", 0, 0, 0, 1, 1, HWND_MESSAGE,
                                 nullptr, GetModuleHandleW(nullptr), nullptr);
    require(owner != nullptr, "gesture owner creation failed");
    TextDrag::attach(owner);
    NativeDragGesture::observer.start();
    auto mouse = [](DWORD flags) {
      INPUT input{};
      input.type = INPUT_MOUSE;
      input.mi.dwFlags = flags;
      require(SendInput(1, &input, sizeof(input)) == 1, "gesture input failed");
    };
    auto waitPress = [](uint64_t previous) {
      auto until = GetTickCount64() + 2000;
      uint64_t press = 0;
      while (GetTickCount64() < until) {
        press = NativeDragGesture::observer.press();
        if (press && press != previous)
          return press;
        Sleep(10);
      }
      throw std::runtime_error("physical press was not observed independently of STA");
    };
    mouse(MOUSEEVENTF_LEFTDOWN);
    auto press = waitPress(0);
    target = TextDrag::enterTarget();
    source = new Source(L"immutable source");
    id = begin(source, target);
    require(id && source->reads == 1, "initial provider not read once");
    source->Release();
    TextDrag::leaveTarget(target);
    target = TextDrag::enterTarget();
    source = new Source(L"different native wrapper");
    require(begin(source, target) == id && source->reads == 0 &&
                TextDrag::source->text == "immutable source",
            "target handoff replaced the gesture snapshot");
    mouse(MOUSEEVENTF_LEFTUP);
    mouse(MOUSEEVENTF_LEFTDOWN);
    waitPress(press);
    TextDrag::leaveTarget(target);
    target = TextDrag::enterTarget();
    require(begin(source, target) != id && source->reads == 1 &&
                TextDrag::source->text == "different native wrapper",
            "new press reused old gesture data");
    source->Release();
    mouse(MOUSEEVENTF_LEFTUP);
    TextDrag::reset();
    NativeDragGesture::observer.stop();
    TextDrag::attach(nullptr);
    DestroyWindow(owner);
    fresh();
    OleUninitialize();
    std::cout << "PASS: native text target leases survive QueryGetData/GetData/ReleaseStgMedium "
                 "reentry; leave/policy/drop revoke old work, newer targets remain active, Unicode "
                 "bounds enforced\n";
    return 0;
  } catch (const std::exception &error) {
    TextDrag::reset();
    OleUninitialize();
    std::cerr << error.what() << '\n';
    return 1;
  }
}
