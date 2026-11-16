#pragma once
// Native outbound COPY drags. File contents stay on Linux until IStream::Read.
// Included after the viewer's Json, wide(), unbase64(), and sendJson() helpers.
#include <shldisp.h>
#include <functional>
#include <limits>
#include <optional>

namespace FileDrag {
inline void trace(const char *event) { if (GetEnvironmentVariableW(L"WAYBRIDGE_DND_TRACE",nullptr,0)) std::cerr << "DND " << event << std::endl; }
constexpr UINT StartMessage = WM_APP + 17;
constexpr UINT ResumeMessage = WM_APP + 23;
inline std::function<bool(POINT)> remoteHitTest;
constexpr size_t Chunk = 65536;
struct File {
  std::wstring name;
  uint64_t size;
  bool directory = false;
};
struct Response {
  bool ready = false, failed = false;
  std::vector<uint8_t> bytes;
};
struct Transfer {
  uint64_t id = 0, nextRequest = 0;
  HWND source = nullptr;
  std::vector<File> files;
  std::optional<std::wstring> text;
  bool returnRemote = false, waitingReturn = false, returnAccepted = false;
  uint64_t returnToken = 0;
  ULONGLONG returnDeadline = 0;
  uint64_t attempt = 0;
  std::mutex mutex;
  std::condition_variable wake;
  std::map<uint64_t, std::shared_ptr<Response>> requests;
  bool cancelled = false, done = false, claimed = false, ready = false, inOle = false;
  bool dropSent = false, oleDone = false, oleSuccess = false;
  bool operation = false, asyncEnded = false, asyncSuccess = false;

  void finish(bool success) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (done)
        return;
      done = true;
      cancelled = !success;
    }
    wake.notify_all();
    sendJson({{"type", "drag_result"},
              {"drag", id},
              {"success", success},
              {"effect", success ? "copy" : "none"}});
  }
  void cancel() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      cancelled = true;
    }
    wake.notify_all();
  }
  HRESULT read(size_t file, uint64_t offset, size_t length, std::vector<uint8_t> &bytes, uint64_t lease) {
    auto response = std::make_shared<Response>();
    uint64_t request;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (cancelled || done || lease != attempt)
        return STG_E_REVERTED;
      if (requests.size() >= 8)
        return STG_E_INSUFFICIENTMEMORY;
      request = ++nextRequest;
      requests.emplace(request, response);
    }
    sendJson({{"type", "drag_read"},
              {"drag", id},
              {"request", request},
              {"file", file},
              {"offset", offset},
              {"length", length}});
    std::unique_lock<std::mutex> lock(mutex);
    bool ready = wake.wait_for(lock, std::chrono::seconds(30),
                               [&] { return cancelled || done || lease != attempt || response->ready; });
    requests.erase(request);
    if (lease != attempt) return STG_E_REVERTED;
    if (!ready || cancelled || done || response->failed || response->bytes.size() != length) {
      lock.unlock();
      finish(false);
      return STG_E_READFAULT;
    }
    bytes = std::move(response->bytes);
    return S_OK;
  }
};
inline IDataObject *ownObject = nullptr; // UI thread, valid only inside DoDragDrop
inline bool isOwn(IDataObject *object) {
  if (!ownObject || !object)
    return false;
  IUnknown *a = nullptr, *b = nullptr;
  object->QueryInterface(IID_IUnknown, reinterpret_cast<void **>(&a));
  ownObject->QueryInterface(IID_IUnknown, reinterpret_cast<void **>(&b));
  bool same = a && a == b;
  if (a)
    a->Release();
  if (b)
    b->Release();
  return same;
}
inline std::mutex activeMutex;
inline std::shared_ptr<Transfer> active;
inline std::shared_ptr<Transfer> current() {
  std::lock_guard<std::mutex> lock(activeMutex);
  return active;
}
inline bool ownsInput() {
  auto transfer = current();
  if (!transfer)
    return false;
  std::lock_guard<std::mutex> lock(transfer->mutex);
  return transfer->claimed && !transfer->oleDone && !transfer->done;
}
inline bool returnToRemote(IDataObject *object, const Json &position) {
  if (!isOwn(object) || !position.contains("id")) return false;
  auto transfer=current();if(!transfer)return false;
  { std::lock_guard<std::mutex> lock(transfer->mutex);
    if(!transfer->inOle || transfer->dropSent || transfer->cancelled || transfer->done)return false;
    if(transfer->returnRemote)return true;
    transfer->returnRemote=true;transfer->returnAccepted=false;++transfer->returnToken;
  }
  Json message=position;message["type"]="drag_return";message["drag"]=transfer->id;message["return_token"]=transfer->returnToken;sendJson(message);return true;
}
inline bool waitForReturn() {
  auto transfer=current();if(!transfer)return false;
  std::lock_guard<std::mutex> lock(transfer->mutex);
  return transfer->waitingReturn && !transfer->done && !transfer->cancelled && GetTickCount64()<transfer->returnDeadline;
}
inline void disconnect() {
  if (auto transfer = current())
    transfer->cancel();
}

// Called by the socket receiver BEFORE the bounded UI inbox. A synchronous
// IStream read must never depend on the STA dispatching its own response.
inline bool receive(const Json &message) {
  auto type = message.value("type", "");
  if (type != "drag_data" && type != "drag_end")
    return false;
  auto transfer = current();
  if (!transfer || message.value("drag", uint64_t(0)) != transfer->id)
    return type == "drag_data";
  if (type == "drag_end") {
    {
      std::lock_guard<std::mutex> lock(transfer->mutex);
      transfer->done = true;
      transfer->cancelled = !message.value("success", false);
    }
    transfer->wake.notify_all();
    return false;
  }
  uint64_t request = message.at("request").get<uint64_t>();
  std::lock_guard<std::mutex> lock(transfer->mutex);
  auto found = transfer->requests.find(request);
  if (found == transfer->requests.end())
    return true;
  auto &response = *found->second;
  if (message.contains("error") || !message.contains("data") || !message["data"].is_string() ||
      message["data"].get_ref<const std::string &>().size() > 90000) {
    response.failed = true;
  } else {
    response.bytes = unbase64(message["data"].get<std::string>());
    response.failed = response.bytes.size() > Chunk;
  }
  response.ready = true;
  transfer->wake.notify_all();
  return true;
}

class Stream final : public IStream {
  std::atomic<ULONG> refs{1};
  std::shared_ptr<Transfer> transfer;
  size_t file;
  uint64_t position;
  uint64_t lease;
  std::mutex positionMutex;
  IUnknown *marshaler = nullptr;

public:
  Stream(std::shared_ptr<Transfer> state, size_t index, uint64_t offset = 0, uint64_t attempt = 0)
      : transfer(std::move(state)), file(index), position(offset), lease(attempt) {
    CoCreateFreeThreadedMarshaler(static_cast<IStream *>(this), &marshaler);
  }
  ~Stream() {
    if (marshaler)
      marshaler->Release();
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (iid == IID_IUnknown || iid == IID_ISequentialStream || iid == IID_IStream) {
      *out = static_cast<IStream *>(this);
      AddRef();
      return S_OK;
    }
    if (iid == IID_IMarshal && marshaler)
      return marshaler->QueryInterface(iid, out);
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override {
    auto count = --refs;
    if (!count)
      delete this;
    return count;
  }
  HRESULT STDMETHODCALLTYPE Read(void *data, ULONG length, ULONG *count) override {
    if (count)
      *count = 0;
    if (!data && length)
      return STG_E_INVALIDPOINTER;
    std::lock_guard<std::mutex> lock(positionMutex);
    ULONG total = 0;
    while (total < length && position < transfer->files[file].size) {
      auto n = static_cast<size_t>(std::min<uint64_t>(std::min<size_t>(length - total, Chunk),
                                                      transfer->files[file].size - position));
      std::vector<uint8_t> bytes;
      HRESULT hr = transfer->read(file, position, n, bytes, lease);
      if (FAILED(hr)) {
        if (count)
          *count = total;
        return hr;
      }
      memcpy(static_cast<uint8_t *>(data) + total, bytes.data(), bytes.size());
      position += bytes.size();
      total += static_cast<ULONG>(bytes.size());
    }
    if (count)
      *count = total;
    return total == length ? S_OK : S_FALSE;
  }
  HRESULT STDMETHODCALLTYPE Write(const void *, ULONG, ULONG *) override {
    return STG_E_ACCESSDENIED;
  }
  HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *out) override {
    std::lock_guard<std::mutex> lock(positionMutex);
    uint64_t base = origin == STREAM_SEEK_SET   ? 0
                    : origin == STREAM_SEEK_CUR ? position
                                                : transfer->files[file].size;
    if (origin > STREAM_SEEK_END)
      return STG_E_INVALIDFUNCTION;
    if ((move.QuadPart < 0 && static_cast<uint64_t>(-(move.QuadPart + 1)) + 1 > base) ||
        (move.QuadPart > 0 &&
         static_cast<uint64_t>(move.QuadPart) > std::numeric_limits<uint64_t>::max() - base))
      return STG_E_INVALIDFUNCTION;
    position = move.QuadPart < 0 ? base - (static_cast<uint64_t>(-(move.QuadPart + 1)) + 1)
                                 : base + static_cast<uint64_t>(move.QuadPart);
    if (out)
      out->QuadPart = position;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return STG_E_ACCESSDENIED; }
  HRESULT STDMETHODCALLTYPE CopyTo(IStream *destination, ULARGE_INTEGER length,
                                   ULARGE_INTEGER *read, ULARGE_INTEGER *written) override {
    if (read)
      read->QuadPart = 0;
    if (written)
      written->QuadPart = 0;
    if (!destination)
      return STG_E_INVALIDPOINTER;
    std::vector<uint8_t> bytes(Chunk);
    uint64_t totalRead = 0, totalWritten = 0;
    while (totalRead < length.QuadPart) {
      ULONG n = 0, output = 0;
      HRESULT hr =
          Read(bytes.data(),
               static_cast<ULONG>(std::min<uint64_t>(Chunk, length.QuadPart - totalRead)), &n);
      totalRead += n;
      if (n) {
        HRESULT result = destination->Write(bytes.data(), n, &output);
        totalWritten += output;
        if (written)
          written->QuadPart = totalWritten;
        if (read)
          read->QuadPart = totalRead;
        if (FAILED(result) || output != n)
          return FAILED(result) ? result : STG_E_MEDIUMFULL;
      }
      if (read)
        read->QuadPart = totalRead;
      if (FAILED(hr) || hr == S_FALSE)
        return hr;
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE Revert() override { return STG_E_INVALIDFUNCTION; }
  HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
    return STG_E_INVALIDFUNCTION;
  }
  HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
    return STG_E_INVALIDFUNCTION;
  }
  HRESULT STDMETHODCALLTYPE Stat(STATSTG *stat, DWORD flags) override {
    if (!stat)
      return STG_E_INVALIDPOINTER;
    *stat = {};
    stat->type = STGTY_STREAM;
    stat->cbSize.QuadPart = transfer->files[file].size;
    stat->grfMode = STGM_READ;
    if (!(flags & STATFLAG_NONAME)) {
      const auto &name = transfer->files[file].name;
      stat->pwcsName = static_cast<wchar_t *>(CoTaskMemAlloc((name.size() + 1) * sizeof(wchar_t)));
      if (!stat->pwcsName)
        return E_OUTOFMEMORY;
      memcpy(stat->pwcsName, name.c_str(), (name.size() + 1) * sizeof(wchar_t));
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Clone(IStream **out) override {
    if (!out)
      return E_POINTER;
    std::lock_guard<std::mutex> lock(positionMutex);
    *out = new Stream(transfer, file, position, lease);
    return S_OK;
  }
};

class DataObject final : public IDataObject, public IDataObjectAsyncCapability {
  std::atomic<ULONG> refs{1};
  std::shared_ptr<Transfer> transfer;
  IUnknown *marshaler = nullptr;
  std::atomic<bool> asyncMode{false};
  CLIPFORMAT descriptor = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW));
  CLIPFORMAT contents = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILECONTENTS));
  CLIPFORMAT preferred =
      static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT));

  uint64_t lease = 0;
  bool currentAttempt() const {std::lock_guard<std::mutex> lock(transfer->mutex);return lease==transfer->attempt;}
public:
  explicit DataObject(std::shared_ptr<Transfer> state) : transfer(std::move(state)), lease(transfer->attempt) {
    CoCreateFreeThreadedMarshaler(static_cast<IDataObject *>(this), &marshaler);
  }
  ~DataObject() {
    if (marshaler)
      marshaler->Release();
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (iid == IID_IUnknown || iid == IID_IDataObject)
      *out = static_cast<IDataObject *>(this);
    else if (iid == IID_IDataObjectAsyncCapability)
      *out = static_cast<IDataObjectAsyncCapability *>(this);
    else if (iid == IID_IMarshal && marshaler)
      return marshaler->QueryInterface(iid, out);
    else
      return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override {
    auto count = --refs;
    if (!count)
      delete this;
    return count;
  }
  HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC *format) override {
    if (!currentAttempt()) return STG_E_REVERTED;
    if (!format)
      return E_POINTER;
    if (format->dwAspect != DVASPECT_CONTENT)
      return DV_E_DVASPECT;
    if (format->ptd)
      return DV_E_DVTARGETDEVICE;
    if (transfer->text) {
      if (format->cfFormat != CF_UNICODETEXT && format->cfFormat != preferred) return DV_E_FORMATETC;
      if (format->lindex != -1) return DV_E_LINDEX;
      return format->tymed & TYMED_HGLOBAL ? S_OK : DV_E_TYMED;
    }
    if (format->cfFormat == contents) {
      if (format->lindex < 0 || size_t(format->lindex) >= transfer->files.size())
        return DV_E_LINDEX;
      if (transfer->files[size_t(format->lindex)].directory)
        return DV_E_FORMATETC;
      return format->tymed & TYMED_ISTREAM ? S_OK : DV_E_TYMED;
    }
    if (format->cfFormat != descriptor && format->cfFormat != preferred)
      return DV_E_FORMATETC;
    if (format->lindex != -1)
      return DV_E_LINDEX;
    return format->tymed & TYMED_HGLOBAL ? S_OK : DV_E_TYMED;
  }
  HRESULT STDMETHODCALLTYPE GetData(FORMATETC *format, STGMEDIUM *medium) override {
    if (!medium)
      return E_POINTER;
    *medium = {};
    HRESULT hr = QueryGetData(format);
    if (FAILED(hr))
      return hr;
    if (format->cfFormat == contents) {
      medium->tymed = TYMED_ISTREAM;
      medium->pstm = new Stream(transfer, size_t(format->lindex), 0, lease);
      return S_OK;
    }
    size_t size = format->cfFormat == CF_UNICODETEXT
                      ? (transfer->text->size() + 1) * sizeof(wchar_t)
                      : format->cfFormat == descriptor
                      ? sizeof(UINT) + transfer->files.size() * sizeof(FILEDESCRIPTORW)
                      : sizeof(DWORD);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size);
    if (!memory)
      return E_OUTOFMEMORY;
    void *bytes = GlobalLock(memory);
    if (!bytes) {
      GlobalFree(memory);
      return E_OUTOFMEMORY;
    }
    if (format->cfFormat == CF_UNICODETEXT) {
      memcpy(bytes, transfer->text->c_str(), size);
    } else if (format->cfFormat == descriptor) {
      auto group = static_cast<FILEGROUPDESCRIPTORW *>(bytes);
      group->cItems = static_cast<UINT>(transfer->files.size());
      for (size_t i = 0; i < transfer->files.size(); ++i) {
        auto &d = group->fgd[i];
        const auto &file = transfer->files[i];
        d.dwFlags = FD_UNICODE | FD_FILESIZE | FD_ATTRIBUTES | FD_PROGRESSUI;
        d.dwFileAttributes = file.directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        d.nFileSizeHigh = static_cast<DWORD>(file.size >> 32);
        d.nFileSizeLow = static_cast<DWORD>(file.size);
        memcpy(d.cFileName, file.name.c_str(), (file.name.size() + 1) * sizeof(wchar_t));
      }
    } else
      *static_cast<DWORD *>(bytes) = DROPEFFECT_COPY;
    GlobalUnlock(memory);
    medium->tymed = TYMED_HGLOBAL;
    medium->hGlobal = memory;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC *, STGMEDIUM *) override {
    return DATA_E_FORMATETC;
  }
  HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC *, FORMATETC *out) override {
    if (out)
      out->ptd = nullptr;
    return DATA_S_SAMEFORMATETC;
  }
  HRESULT STDMETHODCALLTYPE SetData(FORMATETC *, STGMEDIUM *, BOOL) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC **out) override {
    if (direction != DATADIR_GET)
      return E_NOTIMPL;
    if (transfer->text) {
      FORMATETC formats[] = {{CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
                              {preferred, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}};
      return SHCreateStdEnumFmtEtc(2, formats, out);
    }
    FORMATETC formats[] = {{descriptor, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
                           {contents, nullptr, DVASPECT_CONTENT, -1, TYMED_ISTREAM},
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
  HRESULT STDMETHODCALLTYPE SetAsyncMode(BOOL mode) override {
    asyncMode = mode != FALSE;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetAsyncMode(BOOL *mode) override {
    if (!mode)
      return E_POINTER;
    *mode = asyncMode;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE StartOperation(IBindCtx *) override {
    std::lock_guard<std::mutex> lock(transfer->mutex);
    if (lease!=transfer->attempt || !asyncMode || transfer->cancelled || transfer->done || transfer->operation ||
        transfer->asyncEnded)
      return E_UNEXPECTED;
    transfer->operation = true;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE InOperation(BOOL *operation) override {
    if (!operation)
      return E_POINTER;
    std::lock_guard<std::mutex> lock(transfer->mutex);
    *operation = transfer->operation;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE EndOperation(HRESULT result, IBindCtx *, DWORD effect) override {
    bool finish = false, success = false;
    {
      std::lock_guard<std::mutex> lock(transfer->mutex);
      if (lease!=transfer->attempt) return STG_E_REVERTED;
      if (transfer->returnRemote) return S_OK;
      if (!transfer->operation)
        return E_UNEXPECTED;
      transfer->operation = false;
      transfer->asyncEnded = true;
      transfer->asyncSuccess = SUCCEEDED(result) && (effect & DROPEFFECT_COPY);
      finish = transfer->oleDone;
      success = transfer->oleSuccess && transfer->asyncSuccess;
    }
    if (finish)
      transfer->finish(success);
    return S_OK;
  }
};

class DropSource final : public IDropSource {
  std::atomic<ULONG> refs{1};
  std::shared_ptr<Transfer> transfer;

public:
  explicit DropSource(std::shared_ptr<Transfer> state) : transfer(std::move(state)) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (iid != IID_IUnknown && iid != IID_IDropSource)
      return E_NOINTERFACE;
    *out = static_cast<IDropSource *>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override {
    auto count = --refs;
    if (!count)
      delete this;
    return count;
  }
  HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override {
    bool drop = false;
    {
      std::lock_guard<std::mutex> lock(transfer->mutex);
      if (escape || transfer->cancelled || transfer->done || transfer->returnRemote)
        return DRAGDROP_S_CANCEL;
      if (!(keys & MK_LBUTTON) && !transfer->dropSent) {
        transfer->dropSent = true;
        drop = true;
      }
    }
    if (drop)
      sendJson({{"type", "drag_drop"}, {"drag", transfer->id}});
    return keys & MK_LBUTTON ? S_OK : DRAGDROP_S_DROP;
  }
  HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
};

inline bool remoteAt(POINT point) {
  if (remoteHitTest) return remoteHitTest(point);
  HWND window = GetAncestor(WindowFromPoint(point), GA_ROOT);
  wchar_t name[64]{};
  GetClassNameW(window, name, 64);
  return std::wstring(name) == L"WaybridgeRemote" || std::wstring(name) == L"WaybridgePopup";
}
inline void maybeClaim() {
  auto transfer = current();
  if (!transfer)
    return;
  POINT point{};
  GetCursorPos(&point);
  {
    std::lock_guard<std::mutex> lock(transfer->mutex);
    if (transfer->claimed || transfer->done || transfer->cancelled)
      return;
  }
  if (remoteAt(point)) return;
  if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
    transfer->finish(false);
    return;
  }
  {
    std::lock_guard<std::mutex> lock(transfer->mutex);
    transfer->claimed = true;
  }
  trace("claim");
  sendJson({{"type", "drag_claim"}, {"drag", transfer->id}});
}
inline void releasedBeforeStart() {
  auto transfer = current();
  if (!transfer)
    return;
  bool cancel;
  {
    std::lock_guard<std::mutex> lock(transfer->mutex);
    cancel = !transfer->inOle && !transfer->oleDone;
  }
  if (cancel)
    transfer->finish(false);
}
inline void start(HWND controllerWindow) {
  trace("start");
  auto transfer = current();
  if (!transfer)
    return;
  {
    std::lock_guard<std::mutex> lock(transfer->mutex);
    if (!transfer->claimed || !transfer->ready || transfer->inOle || transfer->oleDone ||
        transfer->done || transfer->cancelled)
      return;
  }
  if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
    transfer->finish(false);
    return;
  }
  { std::lock_guard<std::mutex> lock(transfer->mutex);++transfer->attempt; }
  auto data = new DataObject(transfer);
  data->SetAsyncMode(TRUE);
  auto source = new DropSource(transfer);
  {
    std::lock_guard<std::mutex> lock(transfer->mutex);
    transfer->inOle = true;
  }
  trace("release-capture");
  ReleaseCapture();
  trace("enter-ole");
  DWORD effect = 0;
  ownObject = data;
  HRESULT result = DoDragDrop(data, source, DROPEFFECT_COPY, &effect);
  ownObject = nullptr;
  trace("leave-ole");
  source->Release();
  bool returned=false;
  { std::lock_guard<std::mutex> lock(transfer->mutex);
    if (transfer->returnRemote && !transfer->cancelled && !transfer->done) {
      returned=true;transfer->returnRemote=false;transfer->waitingReturn=!transfer->returnAccepted;transfer->returnDeadline=GetTickCount64()+1500;
      transfer->claimed=transfer->ready=transfer->inOle=transfer->oleDone=transfer->dropSent=false;
      transfer->operation=transfer->asyncEnded=transfer->asyncSuccess=false;++transfer->attempt;transfer->requests.clear();
    }
  }
  if(returned){transfer->wake.notify_all();data->Release();PostMessageW(controllerWindow,ResumeMessage,0,0);return;}
  bool finish, success;
  {
    std::lock_guard<std::mutex> lock(transfer->mutex);
    transfer->inOle = false;
    transfer->oleDone = true;
    transfer->oleSuccess = result == DRAGDROP_S_DROP && (effect & DROPEFFECT_COPY);
    finish = !transfer->operation;
    success = transfer->oleSuccess && (!transfer->asyncEnded || transfer->asyncSuccess);
  }
  if (finish)
    transfer->finish(success);
  data->Release();
  PostMessageW(controllerWindow, WM_NULL, 0, 0);
}
inline bool control(const Json &message, HWND controllerWindow, HWND sourceWindow = nullptr) {
  auto type = message.value("type", "");
  if (type == "drag_offer") {
    auto state = std::make_shared<Transfer>();
    state->id = message.at("drag");
    state->source = sourceWindow;
    if (auto old = current()) {
      std::lock_guard<std::mutex> lock(old->mutex);
      if (!old->done && !old->cancelled) {
        state->finish(false);
        return true;
      }
    }
    bool text = message.contains("text") && message["text"].is_string();
    if (!sourceWindow || !message.at("files").is_array() || (!text && message["files"].empty()) ||
        message["files"].size() > (message.value("tree", false) ? FileTree::MaxEntries : 128)) {
      state->finish(false);
      return true;
    }
    if (text) {
      const auto &encoded = message["text"].get_ref<const std::string &>();
      if (encoded.size() > 1400000) { state->finish(false); return true; }
      auto bytes = unbase64(encoded);
      int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<const char *>(bytes.data()), int(bytes.size()), nullptr, 0);
      if (bytes.size() > 1024 * 1024 || (!bytes.empty() && !n) || std::find(bytes.begin(), bytes.end(), 0) != bytes.end()) { state->finish(false); return true; }
      std::wstring value(size_t(n), L'\0');
      if (n) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<const char *>(bytes.data()), int(bytes.size()), value.data(), n);
      state->text = std::move(value);
    } else if (message.value("tree", false)) {
      try {
        FileTree::require(FileTree::supported, "file_tree capability required");
        auto entries = FileTree::parse(message["files"]);
        for (const auto &entry : entries)
          state->files.push_back({FileTree::nativePath(entry.name), entry.size, entry.directory});
      } catch (const std::exception &error) {
        std::cerr << "Native tree drag: " << error.what() << '\n';
        state->finish(false);
        return true;
      }
    } else {
      for (const auto &entry : message["files"]) {
        auto name = wide(entry.at("name").get<std::string>());
        auto size = entry.at("size").get<uint64_t>();
        if (entry.at("id").get<size_t>() != state->files.size() || !FileTree::validName(name) ||
            size > FileTree::MaxFile) {
          state->finish(false);
          return true;
        }
        for (const auto &other : state->files) {
          if (CompareStringOrdinal(name.c_str(), -1, other.name.c_str(), -1, TRUE) == CSTR_EQUAL) {
            state->finish(false);
            return true;
          }
        }
        state->files.push_back({std::move(name), size});
      }
    }
    {
      std::lock_guard<std::mutex> lock(activeMutex);
      active = state;
    }
    maybeClaim();
    return true;
  }
  if (type == "drag_return_ready") {
    auto state=current();if(state && state->id==message.value("drag",uint64_t(0))) {
      std::lock_guard<std::mutex> lock(state->mutex);
      if(message.value("return_token",uint64_t(0))==state->returnToken && message.value("accepted",false)) {state->returnAccepted=true;state->waitingReturn=false;}
    }
    return true;
  }
  if (type != "drag_ready" && type != "drag_end")
    return false;
  auto transfer = current();
  if (!transfer || message.value("drag", uint64_t(0)) != transfer->id)
    return true;
  if (type == "drag_ready") {
    trace("ready");
    {
      std::lock_guard<std::mutex> lock(transfer->mutex);
      transfer->ready = true;
    }
    PostMessageW(controllerWindow, StartMessage, 0, 0);
  } else {
    bool captured;
    {
      std::lock_guard<std::mutex> lock(transfer->mutex);
      captured = transfer->inOle;
    }
    if (captured)
      ReleaseCapture();
  }
  return true;
}
} // namespace FileDrag
