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

static LRESULT CALLBACK policyProc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp){
  if(message==WM_APP+99){TextDrag::control({{"type","text_drag_status"},{"drag",TextDrag::active},{"accepted",true}});return 0;}
  return windowProc(hwnd,message,wp,lp);
}

int wmain() {
  HRESULT initialized = OleInitialize(nullptr);
  if (FAILED(initialized)) return 1;
  int result = 0;
  try {
    WNDCLASSW type{};
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"DropLifetimeProbe";
    type.lpfnWndProc = policyProc;
    require(RegisterClassW(&type) != 0, "RegisterClass failed");
    FileTree::supported = true;
    for (bool replacement : {false, true}) {
      replaceWindow = replacement;
      for (CloseAt closeAt : {CloseAt::Query, CloseAt::GetFailure, CloseAt::GetSuccess,
                              CloseAt::ReleaseMedium, CloseAt::AsyncMode, CloseAt::AsyncStart}) {
        createTarget();
        HWND original = windows.at(Id)->hwnd;
        auto target = new RemoteDropTarget(windows.at(Id).get());
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
    // Text and files have independent policy, through the actual target adapter.
    createTarget();auto *window=windows.at(Id).get();window->width=400;window->height=300;window->clientDecorated=true;
    ShowWindow(window->hwnd,SW_SHOW);POINT cursor{20,20};ClientToScreen(window->hwnd,&cursor);SetCursorPos(cursor.x,cursor.y);
    hostedFiles=false;dataDragNegotiated=true;
    auto adapter=new RemoteDropTarget(window);
    auto textState=std::make_shared<FileDrag::Transfer>();textState->text=L"text with files disabled";
    auto textData=new FileDrag::DataObject(textState);
    PostMessageW(window->hwnd,WM_APP+99,0,0);
    DWORD effect=DROPEFFECT_COPY;POINTL point{cursor.x,cursor.y};
    adapter->DragEnter(textData,0,point,&effect);require(effect==DROPEFFECT_COPY,"file sharing switch blocked plain text");
    effect=DROPEFFECT_COPY;adapter->Drop(textData,0,point,&effect);require(effect==DROPEFFECT_COPY,"text drop blocked with files disabled");textData->Release();
    hostedDragText=false;TextDrag::reset();
    auto blockedState=std::make_shared<FileDrag::Transfer>();blockedState->text=L"blocked text";auto blockedData=new FileDrag::DataObject(blockedState);
    effect=DROPEFFECT_COPY;adapter->DragEnter(blockedData,0,point,&effect);require(effect==DROPEFFECT_NONE,"disabled text drag policy was bypassed");adapter->DragLeave();blockedData->Release();hostedDragText=true;
    auto fileState=std::make_shared<FileDrag::Transfer>();fileState->files.push_back({L"blocked.txt",0,false});auto fileData=new FileDrag::DataObject(fileState);
    effect=DROPEFFECT_COPY;adapter->DragEnter(fileData,0,point,&effect);require(effect==DROPEFFECT_NONE,"file disabled policy was bypassed");adapter->DragLeave();fileData->Release();adapter->Release();destroyRemote(Id);hostedFiles=true;
    require(operationsEnded == 2, "async operation was not ended after target closure");
    // Exercise the actual viewer callback and windowProc, not just geometry.
    createTarget();
    auto *touchTarget=windows.at(Id).get(); touchTarget->width=400; touchTarget->height=300;
    touchTarget->clientDecorated=true; touchTarget->touchInput=true; touchTarget->appId="fixture.touch";
    ShowWindow(touchTarget->hwnd,SW_SHOW); SetFocus(touchTarget->hwnd);
    std::vector<Json> touchWire;
    nativeTouch=NativeTouch([&](const Json &message){touchWire.push_back(message);});
    nativeTouch.onInteraction(touchInteractionSample); nativeTouch.hello(true); nativeTouch.enabled(true);
    nativeTouch.state({{"generation",11},{"id",Id},{"enabled",true}});
    RECT image=imageRect(touchTarget); POINT first{image.left+60,image.top+60};
    ClientToScreen(touchTarget->hwnd,&first);
    NativeTouch::Target input{touchTarget->hwnd,Id,400,300,0,0,image,true,Id,touchTarget->hwnd};
    nativeTouch.sample(input,WM_POINTERDOWN,501,first);
    auto gesture=touchWire.back().at("gesture").get<uint64_t>();
    RECT before{}; GetWindowRect(touchTarget->hwnd,&before);
    require(nativeTouch.beginInteraction({{"operation",31},{"kind","resize"},{"edge",10},
      {"id",Id},{"generation",11},{"gesture",gesture},{"slot",0}},touchTarget->hwnd),"production touch resize rejected");
    POINT after{first.x+70,first.y+40}; input.enabled=false;
    nativeTouch.sample(input,WM_POINTERUPDATE,501,after);
    RECT resized{}; GetWindowRect(touchTarget->hwnd,&resized);
    require(nativeTouch.ownsInteraction(touchTarget->hwnd) && touchTarget->sizing &&
      resized.right-resized.left == before.right-before.left+70 &&
      resized.bottom-resized.top == before.bottom-before.top+40,
      "production WM_SIZE revoked its owning touch resize");
    nativeTouch.sample(input,WM_POINTERUP,501,after);
    require(!nativeTouch.ownsInteraction() && !touchTarget->sizing,"production resize did not release ownership");
    nativeTouch.state({{"generation",12},{"id",Id},{"enabled",true}}); input.enabled=true;
    SetFocus(touchTarget->hwnd); nativeTouch.sample(input,WM_POINTERDOWN,502,first);
    gesture=touchWire.back().at("gesture").get<uint64_t>();
    require(nativeTouch.beginInteraction({{"operation",32},{"kind","dnd"},{"id",Id},
      {"generation",12},{"gesture",gesture},{"slot",0}},touchTarget->hwnd),"production touch DND rejected");
    require(nativeTouch.externalInteraction(32,true),"external owner not retained");
    auto returned=std::make_shared<FileDrag::Transfer>(); returned->id=51; returned->touchOperation=32;
    { std::lock_guard<std::mutex> lock(FileDrag::activeMutex); FileDrag::active=returned; }
    touchOlePromotedOperation=32; touchOleMouseReady=true;
    FileDrag::remoteHitTest=[](POINT point){return windowDragTarget(point) != nullptr;};
    SetCursorPos(first.x,first.y);
    auto started=GetTickCount64();
    require(!touchOleMouse(touchTarget->hwnd,WM_MOUSEMOVE,MK_LBUTTON,0xff515780) &&
      GetTickCount64()-started < 500 && nativeTouch.ownsInteraction(),
      "returned remote motion was consumed by an unclaimed OLE wait");
    require(!returned->done, "remote return was misclassified as a local OLE target");
    returned->claimed=true; // Real claim is sent, but remote ready has not arrived.
    nativeTouch.sample(input,WM_POINTERUP,502,first);
    require(!nativeTouch.ownsInteraction(), "UP before ready retained native owner");
    require(returned->done, "UP before ready did not cancel the pending transfer");
    require(!touchOlePromotedOperation, "UP before ready retained promotion state");
    { std::lock_guard<std::mutex> lock(FileDrag::activeMutex); FileDrag::active.reset(); }
    nativeTouch=NativeTouch{}; resetTouchOle(); FileDrag::remoteHitTest={}; destroyRemote(Id);
    std::cout << "PASS: production touch resize survives WM_SIZE and releases sizing; returned motion bypasses unclaimed OLE wait and UP-before-ready cancels promptly\n";

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
