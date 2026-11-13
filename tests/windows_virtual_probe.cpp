// A real STA OLE provider with synthetic bytes and NO CF_HDROP/filesystem source.
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <shldisp.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static HWND sourceWindow, targetWindow;
struct State {
  std::wstring command, variant;
  bool async = true;
  std::atomic<bool> operation{false}, ended{false}, success{false}, replaced{false};
  std::atomic<unsigned> reads{0}, clones{0}, seeks{0}, eof{0};
  std::atomic<unsigned> globals{0}, descriptions{0};
  std::atomic<ULONGLONG> lastActivity{GetTickCount64()};
  unsigned gets[4]{};
  bool is(const wchar_t *name) const { return variant == name; }
  bool memory() const { return is(L"hglobal") || is(L"hglobal-big") || is(L"hglobal-unknown"); }
  bool slow() const { return is(L"replace") || is(L"cancel") || is(L"disconnect"); }
  uint64_t length(size_t index) const {
    return index == 0   ? (is(L"hglobal-big") ? 13 * 1024 * 1024
                           : slow()           ? 4000000
                                              : 180000)
           : index == 1 ? 0
           : index == 2 ? 1234
                        : 0;
  }
  uint8_t byte(size_t index, uint64_t position) const {
    return static_cast<uint8_t>(position * 37 + index * 19 + 11);
  }
  bool fail() const {
    return is(L"read-fail") || is(L"size-bad") || is(L"badname") || is(L"truncated") ||
           is(L"oversize") || is(L"nostat") || is(L"replace") || is(L"cancel") ||
           is(L"disconnect") || is(L"hglobal-big") || is(L"hglobal-unknown") || is(L"bad-utf16");
  }
};
static std::shared_ptr<State> state;
static void check(HRESULT hr) {
  if (FAILED(hr))
    throw hr;
}

class Stream final : public IStream {
  std::atomic<ULONG> refs{1};
  std::shared_ptr<State> s;
  size_t index;
  uint64_t position;
  unsigned pending = 0;

public:
  Stream(std::shared_ptr<State> data, size_t file, uint64_t offset)
      : s(std::move(data)), index(file), position(offset) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    *out = nullptr;
    if (iid != IID_IUnknown && iid != IID_ISequentialStream && iid != IID_IStream)
      return E_NOINTERFACE;
    *out = static_cast<IStream *>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override {
    auto n = --refs;
    if (!n)
      delete this;
    return n;
  }
  HRESULT STDMETHODCALLTYPE Read(void *buffer, ULONG length, ULONG *read) override {
    s->lastActivity = GetTickCount64();
    if (read)
      *read = 0;
    if (s->is(L"pending") && pending++ < 2) {
      std::cout << "PENDING_READ " << index << std::endl;
      return E_PENDING;
    }
    if (index == 0 && position >= 65536 && s->is(L"die"))
      ExitProcess(43);
    if (index == 0 && position >= 65536 && s->is(L"read-fail"))
      return STG_E_READFAULT;
    if (s->slow()) {
      for (int i = 0; i < 10; ++i) {
        Sleep(10);
        static bool wine =
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
        if (!wine && CoTestCancel() == RPC_E_CALL_CANCELED)
          return E_ABORT;
      }
    }
    uint64_t size = s->length(index);
    ULONG n =
        static_cast<ULONG>(position >= size ? 0 : std::min<uint64_t>(length, size - position));
    for (ULONG i = 0; i < n; ++i)
      static_cast<uint8_t *>(buffer)[i] = s->byte(index, position + i);
    position += n;
    if (read)
      *read = n;
    ++s->reads;
    std::cout << "READ " << index << " " << n << std::endl;
    if (n < length) {
      ++s->eof;
      return S_FALSE;
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Write(const void *, ULONG, ULONG *) override {
    return STG_E_ACCESSDENIED;
  }
  HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *out) override {
    if (s->is(L"noseek")) {
      std::cout << "NO_SEEK" << std::endl;
      return STG_E_INVALIDFUNCTION;
    }
    uint64_t base = origin == STREAM_SEEK_SET   ? 0
                    : origin == STREAM_SEEK_CUR ? position
                                                : s->length(index);
    if (move.QuadPart < 0 && uint64_t(-move.QuadPart) > base)
      return STG_E_INVALIDFUNCTION;
    position = base + move.QuadPart;
    if (out)
      out->QuadPart = position;
    ++s->seeks;
    std::cout << "SEEK " << index << " " << position << std::endl;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return STG_E_ACCESSDENIED; }
  HRESULT STDMETHODCALLTYPE CopyTo(IStream *, ULARGE_INTEGER, ULARGE_INTEGER *,
                                   ULARGE_INTEGER *) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE Revert() override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE Stat(STATSTG *stat, DWORD) override {
    if (s->is(L"nostat"))
      return E_NOTIMPL;
    *stat = {};
    stat->type = STGTY_STREAM;
    stat->cbSize.QuadPart = s->length(index);
    std::cout << "STAT " << index << std::endl;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Clone(IStream **out) override {
    *out = nullptr;
    if (s->is(L"noseek")) {
      std::cout << "NO_CLONE" << std::endl;
      return E_NOTIMPL;
    }
    *out = new Stream(s, index, position);
    ++s->clones;
    std::cout << "CLONE " << index << std::endl;
    return S_OK;
  }
};

class Object final : public IDataObject, public IDataObjectAsyncCapability {
  std::atomic<ULONG> refs{1};
  std::shared_ptr<State> s;
  CLIPFORMAT descriptor = static_cast<CLIPFORMAT>(
      RegisterClipboardFormatW(state->is(L"ansi") ? CFSTR_FILEDESCRIPTORA : CFSTR_FILEDESCRIPTORW));
  CLIPFORMAT contents = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILECONTENTS));
  CLIPFORMAT preferred =
      static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT));
  std::wstring name(size_t i) const {
    if (i == 0 && s->is(L"bad-utf16"))
      return std::wstring(1, wchar_t(0xd800)) + L".bin";
    if (i == 0)
      return s->is(L"badname") ? L"..\\escape.bin"
             : s->is(L"ansi")  ? L"attachment.bin"
                               : L"\u9644\u4ef6 \u56fe.bin";
    if (i == 1)
      return L"empty.bin";
    if (i == 2)
      return L"archive\\folder\\inside.bin";
    return L"archive\\empty";
  }

public:
  explicit Object(std::shared_ptr<State> data) : s(std::move(data)) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    *out = nullptr;
    if (iid == IID_IUnknown || iid == IID_IDataObject)
      *out = static_cast<IDataObject *>(this);
    else if (iid == IID_IDataObjectAsyncCapability && s->async)
      *out = static_cast<IDataObjectAsyncCapability *>(this);
    else
      return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override {
    auto n = --refs;
    if (!n)
      delete this;
    return n;
  }
  HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC *format) override {
    if (format->dwAspect != DVASPECT_CONTENT)
      return DV_E_DVASPECT;
    if (format->cfFormat == descriptor || format->cfFormat == preferred)
      return format->tymed & TYMED_HGLOBAL ? S_OK : DV_E_TYMED;
    if (format->cfFormat != contents)
      return DV_E_FORMATETC; // CF_HDROP is never offered.
    if (format->lindex < 0 || format->lindex > 2)
      return DV_E_LINDEX;
    return format->tymed & (s->memory() ? TYMED_HGLOBAL : TYMED_ISTREAM) ? S_OK : DV_E_TYMED;
  }
  HRESULT STDMETHODCALLTYPE GetData(FORMATETC *format, STGMEDIUM *medium) override {
    *medium = {};
    HRESULT hr = QueryGetData(format);
    if (FAILED(hr))
      return hr;
    if (format->cfFormat == descriptor) {
      bool ansi = s->is(L"ansi");
      size_t itemSize = ansi ? sizeof(FILEDESCRIPTORA) : sizeof(FILEDESCRIPTORW);
      size_t size = s->is(L"truncated") ? sizeof(UINT) : sizeof(UINT) + 4 * itemSize;
      HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size);
      if (!memory)
        return E_OUTOFMEMORY;
      auto bytes = static_cast<uint8_t *>(GlobalLock(memory));
      *reinterpret_cast<UINT *>(bytes) = 4;
      if (!s->is(L"truncated"))
        for (size_t i = 0; i < 4; ++i) {
          FILEDESCRIPTORW item{};
          item.dwFlags = FD_ATTRIBUTES | FD_FILESIZE | (ansi ? 0 : FD_UNICODE);
          if (i == 0 && (s->is(L"unknown") || s->is(L"nostat") || s->is(L"hglobal-unknown")))
            item.dwFlags &= ~FD_FILESIZE;
          item.dwFileAttributes = i == 3 ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
          uint64_t n = s->length(i) + (i == 0 && s->is(L"size-bad") ? 7 : 0);
          if (i == 0 && s->is(L"oversize"))
            n = 1024ULL * 1024 * 1024 + 1;
          item.nFileSizeHigh = DWORD(n >> 32);
          item.nFileSizeLow = DWORD(n);
          auto filename = name(i);
          if (ansi) {
            FILEDESCRIPTORA narrow{};
            memcpy(&narrow, &item, offsetof(FILEDESCRIPTORA, cFileName));
            WideCharToMultiByte(CP_ACP, 0, filename.c_str(), -1, narrow.cFileName, MAX_PATH,
                                nullptr, nullptr);
            memcpy(bytes + sizeof(UINT) + i * itemSize, &narrow, sizeof(narrow));
          } else {
            memcpy(item.cFileName, filename.c_str(), (filename.size() + 1) * sizeof(wchar_t));
            memcpy(bytes + sizeof(UINT) + i * itemSize, &item, sizeof(item));
          }
        }
      GlobalUnlock(memory);
      medium->tymed = TYMED_HGLOBAL;
      medium->hGlobal = memory;
      std::cout << "DESCRIPTORS" << std::endl;
      ++s->descriptions;
      s->lastActivity = GetTickCount64();
      return S_OK;
    }
    if (format->cfFormat == preferred) {
      HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
      *static_cast<DWORD *>(GlobalLock(memory)) = DROPEFFECT_COPY;
      GlobalUnlock(memory);
      medium->tymed = TYMED_HGLOBAL;
      medium->hGlobal = memory;
      return S_OK;
    }
    size_t index = static_cast<size_t>(format->lindex);
    if (s->is(L"pending") && ++s->gets[index] <= 2) {
      std::cout << "PENDING_GET " << index << std::endl;
      return E_PENDING;
    }
    if (s->memory()) {
      size_t n = static_cast<size_t>(s->length(index));
      HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, n ? n : 1);
      if (!memory)
        return E_OUTOFMEMORY;
      auto bytes = static_cast<uint8_t *>(GlobalLock(memory));
      for (size_t i = 0; i < n; ++i)
        bytes[i] = s->byte(index, i);
      GlobalUnlock(memory);
      medium->tymed = TYMED_HGLOBAL;
      medium->hGlobal = memory;
      std::cout << "GLOBAL " << index << std::endl;
      ++s->globals;
      s->lastActivity = GetTickCount64();
      return S_OK;
    }
    // A seekable provider deliberately returns a stream at EOF. Import must
    // clone/rewind rather than accidentally publish an empty/truncated file.
    medium->tymed = TYMED_ISTREAM;
    medium->pstm = new Stream(s, index, s->is(L"noseek") ? 0 : s->length(index));
    std::cout << "CONTENTS " << index << std::endl;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC *, STGMEDIUM *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC *, FORMATETC *out) override {
    out->ptd = nullptr;
    return DATA_S_SAMEFORMATETC;
  }
  HRESULT STDMETHODCALLTYPE SetData(FORMATETC *, STGMEDIUM *, BOOL) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC **out) override {
    if (direction != DATADIR_GET)
      return E_NOTIMPL;
    FORMATETC formats[] = {{descriptor, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
                           {contents, nullptr, DVASPECT_CONTENT, -1,
                            DWORD(s->memory() ? TYMED_HGLOBAL : TYMED_ISTREAM)},
                           {preferred, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}};
    return SHCreateStdEnumFmtEtc(3, formats, out);
  }
  HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC *, DWORD, IAdviseSink *, DWORD *) override {
    return OLE_E_ADVISENOTSUPPORTED;
  }
  HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
  HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA **) override {
    return OLE_E_ADVISENOTSUPPORTED;
  }
  HRESULT STDMETHODCALLTYPE SetAsyncMode(BOOL) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE GetAsyncMode(BOOL *mode) override {
    *mode = s->async;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE StartOperation(IBindCtx *) override {
    if (s->operation.exchange(true))
      return E_UNEXPECTED;
    std::cout << "ASYNC_START" << std::endl;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE InOperation(BOOL *operation) override {
    *operation = s->operation;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE EndOperation(HRESULT result, IBindCtx *, DWORD effect) override {
    s->operation = false;
    s->success = SUCCEEDED(result) && (effect & DROPEFFECT_COPY);
    s->ended = true;
    std::cout << (s->success ? "END_SUCCESS" : "END_FAILURE") << std::endl;
    PostMessageW(sourceWindow, WM_CLOSE, 0, 0);
    return S_OK;
  }
};

class DropSource final : public IDropSource {
  std::atomic<ULONG> refs{1};

public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    *out = nullptr;
    if (iid != IID_IUnknown && iid != IID_IDropSource)
      return E_NOINTERFACE;
    *out = this;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override {
    auto n = --refs;
    if (!n)
      delete this;
    return n;
  }
  HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override {
    return escape ? DRAGDROP_S_CANCEL : keys & MK_LBUTTON ? S_OK : DRAGDROP_S_DROP;
  }
  HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
};
static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
  if (message == WM_CLOSE) {
    PostQuitMessage(0);
    return 0;
  }
  if (message == WM_TIMER) {
    if (wp == 2) {
      std::cout << "TIMEOUT" << std::endl;
      PostQuitMessage(1);
    }
    // OleGetClipboard may wrap the provider without exposing its async
    // interface. Retain this STA until consumption settles; the Python side
    // independently checks Linux commit, exact bytes and failed-tree cleanup.
    if (state->command == L"clipboard" && !state->operation && state->descriptions &&
        GetTickCount64() - state->lastActivity.load() > 1000 &&
        (state->fail() || state->eof >= 3 || state->globals == 3)) {
      state->success = !state->fail();
      state->ended = true;
      PostQuitMessage(0);
    }
    return 0;
  }
  return DefWindowProcW(hwnd, message, wp, lp);
}
int wmain(int argc, wchar_t **argv) {
  if (argc < 3)
    return 2;
  state = std::make_shared<State>();
  state->command = argv[1];
  state->variant = argv[2];
  state->async = state->command != L"drop-sync";
  bool clipboard = state->command == L"clipboard";
  OleInitialize(nullptr);
  WNDCLASSW wc{};
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"VirtualFileProviderProbe";
  wc.lpfnWndProc = windowProc;
  RegisterClassW(&wc);
  sourceWindow = CreateWindowExW(
      0, wc.lpszClassName, L"Virtual source", clipboard ? 0 : WS_OVERLAPPEDWINDOW | WS_VISIBLE, 800,
      100, 300, 350, clipboard ? HWND_MESSAGE : nullptr, nullptr, wc.hInstance, nullptr);
  SetTimer(sourceWindow, 1, 10, nullptr);
  SetTimer(sourceWindow, 2, 25000, nullptr);
  auto object = new Object(state);
  FORMATETC hdrop{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  if (SUCCEEDED(object->QueryGetData(&hdrop)))
    return 3;
  std::cout << "NO_HDROP" << std::endl;
  DWORD effect = 0;
  HRESULT dragResult = E_FAIL;
  std::thread driver;
  if (clipboard) {
    check(OleSetClipboard(object));
    std::cout << "CLIPBOARD" << std::endl;
  } else {
    targetWindow = FindWindowW(L"WaybridgeRemote", nullptr);
    if (!targetWindow)
      return 4;
    SetWindowPos(targetWindow, nullptr, 100, 100, 450, 350, SWP_NOZORDER);
    SetForegroundWindow(targetWindow);
    POINT start{100, 70};
    ClientToScreen(targetWindow, &start);
    SetCursorPos(start.x, start.y);
    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
    driver = std::thread([] {
      Sleep(500);
      POINT target{100, 70};
      ClientToScreen(targetWindow, &target);
      SetCursorPos(target.x + 1, target.y);
      Sleep(500);
      mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
      if (state->is(L"cancel") || state->is(L"disconnect")) {
        for (int i = 0; i < 500 && !state->reads; ++i)
          Sleep(10);
        if (state->is(L"cancel"))
          PostMessageW(targetWindow, WM_KEYDOWN, VK_ESCAPE, 1);
        else {
          HWND control = FindWindowExW(HWND_MESSAGE, nullptr, L"WaybridgeControl", nullptr);
          PostMessageW(control, WM_CLOSE, 0, 0);
        }
      }
    });
    auto source = new DropSource;
    dragResult = DoDragDrop(object, source, DROPEFFECT_COPY, &effect);
    source->Release();
    driver.join();
    std::cout << "DROP_RETURN " << std::hex << uint32_t(dragResult) << " " << effect << std::dec
              << " reads=" << state->reads << std::endl;
  }
  if (clipboard || state->operation) {
    MSG msg{};
    while (!state->ended && GetMessageW(&msg, nullptr, 0, 0) > 0) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  bool success = state->async ? state->ended && state->success
                              : dragResult == DRAGDROP_S_DROP && (effect & DROPEFFECT_COPY);
  bool expected = state->fail() ? !success : success;
  if (clipboard)
    OleSetClipboard(nullptr);
  object->Release();
  DestroyWindow(sourceWindow);
  OleUninitialize();
  std::cout << "RESULT " << success << " CLONES " << state->clones << " SEEKS " << state->seeks
            << std::endl;
  return expected ? 0 : 7;
}
