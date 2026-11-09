// Native virtual-file drop target/driver. Only run on the isolated Wine desktop.
#include <windows.h>
#include <shlobj.h>
#include <shldisp.h>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

static HWND targetWindow;
static std::wstring outputDirectory, mode;
static std::atomic<bool> entered{false}, done{false};
static int outcome = 1;
static std::thread worker;
static void check(HRESULT hr) {
  if (FAILED(hr))
    throw hr;
}

static void extract(IDataObject *data) {
  FORMATETC format{static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW)),
                   nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  STGMEDIUM descriptors{};
  check(data->GetData(&format, &descriptors));
  auto group = static_cast<FILEGROUPDESCRIPTORW *>(GlobalLock(descriptors.hGlobal));
  if (!group || !group->cItems || group->cItems > 128)
    throw E_FAIL;
  std::vector<FILEDESCRIPTORW> files(group->fgd, group->fgd + group->cItems);
  GlobalUnlock(descriptors.hGlobal);
  ReleaseStgMedium(&descriptors);
  for (size_t i = 0; i < files.size(); ++i) {
    format.cfFormat = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILECONTENTS));
    format.lindex = static_cast<LONG>(i);
    format.tymed = TYMED_ISTREAM;
    STGMEDIUM content{};
    check(data->GetData(&format, &content));
    if (content.tymed != TYMED_ISTREAM)
      throw E_FAIL;
    STATSTG stat{};
    check(content.pstm->Stat(&stat, STATFLAG_NONAME));
    uint64_t size = (uint64_t(files[i].nFileSizeHigh) << 32) | files[i].nFileSizeLow;
    if (stat.cbSize.QuadPart != size)
      throw E_FAIL;
    std::vector<unsigned char> sample;
    if (size > 40) {
      LARGE_INTEGER position{};
      position.QuadPart = 17;
      check(content.pstm->Seek(position, STREAM_SEEK_SET, nullptr));
      IStream *clone = nullptr;
      check(content.pstm->Clone(&clone));
      sample.resize(19);
      ULONG n = 0;
      check(clone->Read(sample.data(), static_cast<ULONG>(sample.size()), &n));
      clone->Release();
      if (n != sample.size())
        throw E_FAIL;
    }
    LARGE_INTEGER zero{};
    check(content.pstm->Seek(zero, STREAM_SEEK_SET, nullptr));
    std::vector<unsigned char> bytes;
    for (;;) {
      unsigned char buffer[4093];
      ULONG n = 0;
      HRESULT hr = content.pstm->Read(buffer, sizeof(buffer), &n);
      check(hr);
      bytes.insert(bytes.end(), buffer, buffer + n);
      if (hr == S_FALSE)
        break;
      if (!n || bytes.size() > size)
        throw E_FAIL;
    }
    ReleaseStgMedium(&content);
    if (bytes.size() != size ||
        (!sample.empty() && !std::equal(sample.begin(), sample.end(), bytes.begin() + 17)))
      throw E_FAIL;
    auto path = outputDirectory + L"\\" + files[i].cFileName;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
      throw E_FAIL;
    DWORD written = 0;
    bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(file);
    if (!ok || written != bytes.size())
      throw E_FAIL;
  }
  std::cout << "EXTRACTED " << files.size() << std::endl;
}

class Target final : public IDropTarget {
  LONG refs = 1;

public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
    *out = nullptr;
    if (id != IID_IUnknown && id != IID_IDropTarget)
      return E_NOINTERFACE;
    *out = this;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
  ULONG STDMETHODCALLTYPE Release() override {
    auto n = InterlockedDecrement(&refs);
    if (!n)
      delete this;
    return n;
  }
  HRESULT STDMETHODCALLTYPE DragEnter(IDataObject *data, DWORD, POINTL, DWORD *effect) override {
    FORMATETC format{static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW)),
                     nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    bool supported = SUCCEEDED(data->QueryGetData(&format));
    *effect = supported && mode != L"reject" ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    entered = true;
    std::cout << "ENTER " << supported << std::endl;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD *effect) override {
    *effect = mode == L"reject" ? DROPEFFECT_NONE : DROPEFFECT_COPY;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DragLeave() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE Drop(IDataObject *data, DWORD, POINTL, DWORD *effect) override {
    *effect = DROPEFFECT_NONE;
    if (mode == L"async") {
      IDataObjectAsyncCapability *async = nullptr;
      HRESULT hr = data->QueryInterface(IID_PPV_ARGS(&async));
      if (FAILED(hr)) {
        std::cout << "NO_ASYNC " << std::hex << hr << std::endl;
        return S_OK;
      }
      BOOL supported = FALSE;
      check(async->GetAsyncMode(&supported));
      if (!supported)
        throw E_FAIL;
      check(async->StartOperation(nullptr));
      IStream *marshaled = nullptr;
      check(CoMarshalInterThreadInterfaceInStream(IID_IDataObject, data, &marshaled));
      async->Release();
      worker = std::thread([marshaled] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IDataObject *object = nullptr;
        HRESULT result = CoGetInterfaceAndReleaseStream(marshaled, IID_PPV_ARGS(&object));
        if (SUCCEEDED(result)) {
          Sleep(400); // DoDragDrop must return before asynchronous extraction.
          try {
            extract(object);
            outcome = 0;
          } catch (HRESULT hr) {
            result = hr;
            std::cout << "READ_ERROR " << std::hex << hr << std::endl;
          }
          IDataObjectAsyncCapability *operation = nullptr;
          if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&operation)))) {
            operation->EndOperation(result, nullptr,
                                    SUCCEEDED(result) ? DROPEFFECT_COPY : DROPEFFECT_NONE);
            operation->Release();
          }
          object->Release();
        }
        done = true;
        PostMessageW(targetWindow, WM_CLOSE, 0, 0);
        CoUninitialize();
      });
      *effect = DROPEFFECT_COPY;
      std::cout << "ASYNC_STARTED" << std::endl;
      return S_OK;
    }
    try {
      extract(data);
      outcome = 0;
      *effect = DROPEFFECT_COPY;
    } catch (HRESULT hr) {
      std::cout << "READ_ERROR " << std::hex << hr << std::endl;
    }
    done = true;
    PostMessageW(targetWindow, WM_CLOSE, 0, 0);
    return S_OK;
  }
};
static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
  if (message == WM_CLOSE) {
    PostQuitMessage(0);
    return 0;
  }
  if (message == WM_TIMER) {
    std::cout << "TIMEOUT" << std::endl;
    done = true;
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(hwnd, message, wp, lp);
}
int wmain(int argc, wchar_t **argv) {
  if (argc < 3)
    return 2;
  outputDirectory = argv[1];
  mode = argv[2];
  OleInitialize(nullptr);
  WNDCLASSW wc{};
  wc.lpfnWndProc = windowProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"WaybridgeDragTest";
  RegisterClassW(&wc);
  targetWindow =
      CreateWindowExW(0, wc.lpszClassName, L"Native drop target", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                      800, 100, 300, 400, nullptr, nullptr, wc.hInstance, nullptr);
  auto target = new Target;
  check(RegisterDragDrop(targetWindow, target));
  target->Release();
  SetTimer(targetWindow, 1, 20000, nullptr);
  std::thread driver([] {
    HWND source = FindWindowW(L"WaybridgeRemote", nullptr);
    if (!source) {
      done = true;
      PostMessageW(targetWindow, WM_CLOSE, 0, 0);
      return;
    }
    SetWindowPos(source, nullptr, 100, 100, 450, 350, SWP_NOZORDER);
    // A newly created target can still own activation. Wait for the source
    // activation/resize messages before injecting its first button press.
    for (int i = 0; i < 50 && GetForegroundWindow() != source; ++i) {
      SetForegroundWindow(source);
      Sleep(20);
    }
    Sleep(100);
    POINT point{50, 70};
    ClientToScreen(source, &point);
    SetCursorPos(point.x, point.y);
    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
    Sleep(mode == L"early" ? 50 : 500);
    if (mode == L"early") {
      mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
      Sleep(600);
      outcome = 0;
      done = true;
      PostMessageW(targetWindow, WM_CLOSE, 0, 0);
      return;
    }
    POINT destination{80, 120};
    ClientToScreen(targetWindow, &destination);
    for (int i = 1; i <= 10; ++i) {
      SetCursorPos(point.x + (destination.x - point.x) * i / 10,
                   point.y + (destination.y - point.y) * i / 10);
      Sleep(35);
    }
    for (int i = 0; i < 100 && !entered && !done; ++i)
      Sleep(20);
    if (mode == L"disconnect") {
      Sleep(5000);
      mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
      outcome = entered ? 0 : 1;
      done = true;
      PostMessageW(targetWindow, WM_CLOSE, 0, 0);
      return;
    }
    if (mode == L"cancel") {
      keybd_event(VK_ESCAPE, 0, 0, 0);
      keybd_event(VK_ESCAPE, 0, KEYEVENTF_KEYUP, 0);
    }
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    if (mode == L"reject" || mode == L"cancel") {
      Sleep(400);
      outcome = entered ? 0 : 1;
      done = true;
      PostMessageW(targetWindow, WM_CLOSE, 0, 0);
    }
  });
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  done = true;
  driver.join();
  if (worker.joinable())
    worker.join();
  RevokeDragDrop(targetWindow);
  DestroyWindow(targetWindow);
  OleUninitialize();
  return outcome;
}
