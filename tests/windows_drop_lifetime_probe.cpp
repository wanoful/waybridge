// Exercise production OLE callbacks with ordinary target closure/replacement.
// Assertions concern cancellation and transfer scheduling, never freed memory.
#define wmain waybridge_viewer_main
#define WAYBRIDGE_VERSION "drop-lifetime-probe"
#include "../client/main.cpp"
#undef wmain

namespace {
constexpr uint32_t Id = 91;
enum class CloseAt { None, Query, GetFailure, GetSuccess, ReleaseMedium, AsyncMode, AsyncStart };
unsigned operationsEnded = 0;
bool replaceWindow = false;

void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
void createTarget() {
  auto window = std::make_unique<RemoteWindow>();
  window->id = Id;
  HWND hwnd = CreateWindowW(L"DropLifetimeProbe", L"Drop target", WS_OVERLAPPEDWINDOW,
                             0, 0, 400, 300, nullptr, nullptr, GetModuleHandleW(nullptr), window.get());
  require(hwnd != nullptr, "CreateWindow failed");
  windows[Id] = std::move(window);
}
void closeTarget() {
  destroyRemote(Id);
  if (replaceWindow)
    createTarget();
}

class MediumOwner final : public IUnknown {
  ULONG refs = 1;
  HGLOBAL memory;
public:
  explicit MediumOwner(HGLOBAL value) : memory(value) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    *out = nullptr;
    if (iid != IID_IUnknown) return E_NOINTERFACE;
    *out = this;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG count = --refs;
    if (!count) {
      closeTarget();
      GlobalFree(memory);
      delete this;
    }
    return count;
  }
};

class ClosingData final : public IDataObject, public IDataObjectAsyncCapability {
  ULONG refs = 1;
  CloseAt closeAt;
public:
  explicit ClosingData(CloseAt value) : closeAt(value) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    *out = nullptr;
    if (iid == IID_IUnknown || iid == IID_IDataObject)
      *out = static_cast<IDataObject *>(this);
    else if (iid == IID_IDataObjectAsyncCapability)
      *out = static_cast<IDataObjectAsyncCapability *>(this);
    else
      return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG count = --refs;
    if (!count) delete this;
    return count;
  }
  HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC *format) override {
    if (closeAt == CloseAt::Query)
      closeTarget();
    if (format->cfFormat == CF_HDROP)
      return closeAt == CloseAt::Query ? S_OK : DV_E_FORMATETC;
    return format->cfFormat == VirtualFiles::descriptorsW() ? S_OK : DV_E_FORMATETC;
  }
  HRESULT STDMETHODCALLTYPE GetData(FORMATETC *format, STGMEDIUM *out) override {
    *out = {};
    if (format->cfFormat != CF_HDROP)
      return DV_E_FORMATETC;
    if (closeAt == CloseAt::GetFailure) {
      closeTarget();
      return DV_E_FORMATETC;
    }
    if (closeAt != CloseAt::GetSuccess && closeAt != CloseAt::ReleaseMedium)
      return DV_E_FORMATETC;
    const wchar_t path[] = L"C:\\drop-lifetime-fixture.txt\0";
    std::vector<uint8_t> bytes(sizeof(DROPFILES) + sizeof(path));
    auto header = reinterpret_cast<DROPFILES *>(bytes.data());
    header->pFiles = sizeof(DROPFILES);
    header->fWide = TRUE;
    memcpy(bytes.data() + sizeof(DROPFILES), path, sizeof(path));
    out->tymed = TYMED_HGLOBAL;
    out->hGlobal = globalBytes(bytes.data(), bytes.size());
    if (closeAt == CloseAt::ReleaseMedium)
      out->pUnkForRelease = new MediumOwner(out->hGlobal);
    else
      closeTarget();
    return out->hGlobal ? S_OK : E_OUTOFMEMORY;
  }
  HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC *, STGMEDIUM *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC *, FORMATETC *out) override {
    out->ptd = nullptr; return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE SetData(FORMATETC *, STGMEDIUM *, BOOL) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD, IEnumFORMATETC **) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC *, DWORD, IAdviseSink *, DWORD *) override {
    return OLE_E_ADVISENOTSUPPORTED;
  }
  HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
  HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA **) override { return OLE_E_ADVISENOTSUPPORTED; }
  HRESULT STDMETHODCALLTYPE SetAsyncMode(BOOL) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE GetAsyncMode(BOOL *mode) override {
    *mode = TRUE;
    if (closeAt == CloseAt::AsyncMode) closeTarget();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE StartOperation(IBindCtx *) override {
    if (closeAt == CloseAt::AsyncStart) closeTarget();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE InOperation(BOOL *active) override { *active = FALSE; return S_OK; }
  HRESULT STDMETHODCALLTYPE EndOperation(HRESULT result, IBindCtx *, DWORD effect) override {
    require(FAILED(result) && effect == DROPEFFECT_NONE, "closed target reported successful transfer");
    ++operationsEnded;
    return S_OK;
  }
};
}

int wmain() {
  HRESULT initialized = OleInitialize(nullptr);
  if (FAILED(initialized)) return 1;
  int result = 0;
  try {
    WNDCLASSW type{};
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"DropLifetimeProbe";
    type.lpfnWndProc = windowProc;
    require(RegisterClassW(&type) != 0, "RegisterClass failed");
    FileTree::supported = true;
    for (bool replacement : {false, true}) {
      replaceWindow = replacement;
      for (CloseAt closeAt : {CloseAt::Query, CloseAt::GetFailure, CloseAt::GetSuccess,
                              CloseAt::ReleaseMedium, CloseAt::AsyncMode, CloseAt::AsyncStart}) {
        createTarget();
        HWND original = windows.at(Id)->hwnd;
        auto target = new FileDropTarget(windows.at(Id).get());
        require(SUCCEEDED(RegisterDragDrop(original, target)), "RegisterDragDrop failed");
        auto data = new ClosingData(closeAt);
        DWORD effect = DROPEFFECT_COPY;
        POINTL point{20, 20};
        auto available = new ClosingData(CloseAt::None);
        target->DragEnter(available, 0, point, &effect);
        require(effect == DROPEFFECT_COPY, "live target rejected supported files");
        target->DragLeave();
        effect = DROPEFFECT_COPY;
        target->DragOver(0, point, &effect);
        require(effect == DROPEFFECT_NONE, "DragLeave did not clear acceptance");
        available->Release();
        effect = DROPEFFECT_COPY;
        if (closeAt == CloseAt::Query)
          target->DragEnter(data, 0, point, &effect);
        else
          target->Drop(data, 0, point, &effect);
        require(effect == DROPEFFECT_NONE, "closed/replaced target accepted drop");
        effect = DROPEFFECT_COPY;
        target->DragOver(0, point, &effect);
        require(effect == DROPEFFECT_NONE, "closed/replaced target accepted DragOver");
        require(VirtualFiles::requests.empty() && FileTree::requests.empty() && fileWorkers.empty(),
                "closed target scheduled a file transfer");
        require(!IsWindow(original) || replacement, "original target was not closed");
        data->Release();
        target->Release();
        replaceWindow = false;
        destroyRemote(Id);
        replaceWindow = replacement;
      }
    }
    require(operationsEnded == 2, "async operation was not ended after target closure");
    std::cout << "PASS: OLE target closure/replacement during query, data read, medium release, async setup; cancelled without scheduling uploads\n";
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    result = 1;
  }
  replaceWindow = false;
  destroyRemote(Id);
  OleUninitialize();
  return result;
}
