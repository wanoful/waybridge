// Real OLE + physical mouse driver; only manipulates its own fixture windows.
#include <windows.h>
#include <ole2.h>
#include <shlobj.h>
#include <atomic>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

static std::atomic<bool> entered{false}, oleStarted{false};
static std::atomic<int> outcome{1};
static HWND targetWindow;
static const wchar_t Text[] = L"Windows \x4f60\x597d";
class TextData final : public IDataObject {
  LONG refs=1;
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void **out) override {
    *out=nullptr;if(iid!=IID_IUnknown && iid!=IID_IDataObject)return E_NOINTERFACE;
    *out=this;AddRef();return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {return InterlockedIncrement(&refs);}
  ULONG STDMETHODCALLTYPE Release() override {auto n=InterlockedDecrement(&refs);if(!n)delete this;return n;}
  HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC *f) override {return f && f->cfFormat==CF_UNICODETEXT && f->dwAspect==DVASPECT_CONTENT && (f->tymed&TYMED_HGLOBAL) ? S_OK : DV_E_FORMATETC;}
  HRESULT STDMETHODCALLTYPE GetData(FORMATETC *f,STGMEDIUM *m) override {
    std::cout<<"GET_TEXT"<<std::endl;*m={};auto hr=QueryGetData(f);if(FAILED(hr))return hr;
    m->hGlobal=GlobalAlloc(GMEM_MOVEABLE,sizeof(Text));if(!m->hGlobal)return E_OUTOFMEMORY;
    auto p=GlobalLock(m->hGlobal);memcpy(p,Text,sizeof(Text));GlobalUnlock(m->hGlobal);m->tymed=TYMED_HGLOBAL;return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*,STGMEDIUM*) override{return E_NOTIMPL;}
  HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*,FORMATETC *f) override{f->ptd=nullptr;return E_NOTIMPL;}
  HRESULT STDMETHODCALLTYPE SetData(FORMATETC*,STGMEDIUM*,BOOL) override{return E_NOTIMPL;}
  HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD d,IEnumFORMATETC **out) override {
    FORMATETC f{CF_UNICODETEXT,nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};return d==DATADIR_GET?SHCreateStdEnumFmtEtc(1,&f,out):E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*,DWORD,IAdviseSink*,DWORD*) override{return OLE_E_ADVISENOTSUPPORTED;}
  HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override{return OLE_E_ADVISENOTSUPPORTED;}
  HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override{return OLE_E_ADVISENOTSUPPORTED;}
};
class Source final : public IDropSource {
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void **out) override{*out=nullptr;if(iid!=IID_IUnknown && iid!=IID_IDropSource)return E_NOINTERFACE;*out=this;return S_OK;}
  ULONG STDMETHODCALLTYPE AddRef() override{return 2;}ULONG STDMETHODCALLTYPE Release() override{return 1;}
  HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape,DWORD keys) override{return escape?DRAGDROP_S_CANCEL:keys&MK_LBUTTON?S_OK:DRAGDROP_S_DROP;}
  HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override{return DRAGDROP_S_USEDEFAULTCURSORS;}
};
class Target final : public IDropTarget {
  LONG refs=1;
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void **out) override{*out=nullptr;if(iid!=IID_IUnknown && iid!=IID_IDropTarget)return E_NOINTERFACE;*out=this;AddRef();return S_OK;}
  ULONG STDMETHODCALLTYPE AddRef() override{return InterlockedIncrement(&refs);}ULONG STDMETHODCALLTYPE Release() override{auto n=InterlockedDecrement(&refs);if(!n)delete this;return n;}
  HRESULT STDMETHODCALLTYPE DragEnter(IDataObject *d,DWORD,POINTL,DWORD *effect) override{
    FORMATETC f{CF_UNICODETEXT,nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};
    bool supported=SUCCEEDED(d->QueryGetData(&f));*effect=supported?DROPEFFECT_COPY:DROPEFFECT_NONE;
    entered=supported;std::cout<<"ENTER "<<supported<<std::endl;return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DragOver(DWORD,POINTL,DWORD *effect) override{*effect=DROPEFFECT_COPY;return S_OK;}
  HRESULT STDMETHODCALLTYPE DragLeave() override{return S_OK;}
  HRESULT STDMETHODCALLTYPE Drop(IDataObject *d,DWORD,POINTL,DWORD *effect) override{
    *effect=DROPEFFECT_NONE;FORMATETC f{CF_UNICODETEXT,nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};STGMEDIUM m{};
    if(SUCCEEDED(d->GetData(&f,&m))){auto p=static_cast<const wchar_t*>(GlobalLock(m.hGlobal));
      if(p && std::wstring(p)==L"Waybridge drag text"){outcome=0;*effect=DROPEFFECT_COPY;std::cout<<"TEXT_MATCH"<<std::endl;}
      if(p)GlobalUnlock(m.hGlobal);ReleaseStgMedium(&m);}
    PostMessageW(targetWindow,WM_CLOSE,0,0);return S_OK;
  }
};
static LRESULT CALLBACK proc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp){
  if(message==WM_CLOSE || message==WM_TIMER){PostQuitMessage(0);return 0;}return DefWindowProcW(hwnd,message,wp,lp);
}
static POINT point(HWND hwnd,int x,int y){POINT p{x,y};ClientToScreen(hwnd,&p);return p;}
static void move(POINT a,POINT b){for(int i=1;i<=16;++i){SetCursorPos(a.x+(b.x-a.x)*i/16,a.y+(b.y-a.y)*i/16);Sleep(30);}}
int wmain(int argc,wchar_t **argv){
  if(argc!=2)return 2;std::wstring mode=argv[1];
  HWND source=FindWindowW(L"WaybridgeRemote",L"Waybridge drag source");
  HWND remoteTarget=FindWindowW(L"WaybridgeRemote",L"Waybridge drag target");
  if(!source || !remoteTarget)return 3;
  POINT original{};GetCursorPos(&original);HWND foreground=GetForegroundWindow();
  OleInitialize(nullptr);
  WNDCLASSW wc{};wc.lpfnWndProc=proc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"WaybridgeTextTest";RegisterClassW(&wc);
  targetWindow=CreateWindowExW(0,wc.lpszClassName,L"Waybridge native text test",WS_OVERLAPPEDWINDOW|WS_VISIBLE,1050,500,400,250,nullptr,nullptr,wc.hInstance,nullptr);
  auto target=new Target;RegisterDragDrop(targetWindow,target);target->Release();
  SetTimer(targetWindow,1,18000,nullptr);
  SetWindowPos(source,nullptr,80,80,600,430,SWP_NOZORDER|SWP_NOACTIVATE);
  SetWindowPos(remoteTarget,nullptr,750,80,600,380,SWP_NOZORDER|SWP_NOACTIVATE);
  SetForegroundWindow(mode==L"in"?targetWindow:source);Sleep(400);
  std::thread driver([&]{
    POINT start=mode==L"in"?point(targetWindow,50,50):point(source,60,40);
    SetCursorPos(start.x,start.y);mouse_event(MOUSEEVENTF_LEFTDOWN,0,0,0,0);Sleep(150);
    if(mode==L"in"){while(!oleStarted)Sleep(10);Sleep(100);}
    if(mode!=L"in"){
      auto next=point(source,100,40);move(start,next);start=next;Sleep(350);
    }
    POINT destination=point(mode==L"out"?targetWindow:remoteTarget,100,120);
    if(mode==L"return"){
      auto gap=point(targetWindow,100,120);move(start,gap);
      for(int i=0;i<80 && !entered;++i)Sleep(20);
      if(!entered){mouse_event(MOUSEEVENTF_LEFTUP,0,0,0,0);PostMessageW(targetWindow,WM_CLOSE,0,0);return;}
      start=gap;Sleep(200);
    }
    move(start,destination);wchar_t hit[128]{};GetClassNameW(WindowFromPoint(destination),hit,128);std::wcout<<L"DEST "<<destination.x<<L" "<<destination.y<<L" "<<hit<<std::endl;Sleep(700);mouse_event(MOUSEEVENTF_LEFTUP,0,0,0,0);
    if(mode==L"internal" || mode==L"return"){outcome=0;Sleep(400);PostMessageW(targetWindow,WM_CLOSE,0,0);}
  });
  if(mode==L"in"){
    // OLE consults this STA's key state. Dispatch the real injected press
    // before entering its modal loop, then allow the driver to leave the source.
    for(int i=0;i<200 && !(GetKeyState(VK_LBUTTON)&0x8000);++i){
      MSG pending{};while(PeekMessageW(&pending,nullptr,0,0,PM_REMOVE)){TranslateMessage(&pending);DispatchMessageW(&pending);}Sleep(5);
    }
    oleStarted=true;TextData *data=new TextData;Source input;DWORD effect=0;
    auto hr=DoDragDrop(data,&input,DROPEFFECT_COPY,&effect);data->Release();
    if(hr==DRAGDROP_S_DROP && effect==DROPEFFECT_COPY)outcome=0;
    std::cout<<"IN_RESULT "<<std::hex<<hr<<" "<<effect<<std::endl;PostMessageW(targetWindow,WM_CLOSE,0,0);
  }
  MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}
  driver.join();RevokeDragDrop(targetWindow);DestroyWindow(targetWindow);
  SetCursorPos(original.x,original.y);if(IsWindow(foreground))SetForegroundWindow(foreground);
  OleUninitialize();return outcome.load();
}
