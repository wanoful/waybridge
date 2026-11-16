#pragma once
// Bounded OLE Unicode text <-> Wayland MIME data. Never creates a file or
// changes the clipboard. All state belongs to the controller STA.
namespace TextDrag {
inline uint64_t sequence = 0, active = 0;
inline bool accepted = false;
inline bool supports(IDataObject *data) {
  FORMATETC format{CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  return data && SUCCEEDED(data->QueryGetData(&format));
}
inline void cancel(uint64_t id) {
  if (!id || active != id) return;
  sendJson({{"type","text_drag_cancel"},{"drag",id}}); active=0;accepted=false;
}
inline uint64_t begin(IDataObject *data, Json position, const std::function<bool()> &current) {
  FORMATETC format{CF_UNICODETEXT,nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};
  STGMEDIUM medium{};
  if (FAILED(data->GetData(&format,&medium))) return 0;
  std::string text;
  if (medium.tymed==TYMED_HGLOBAL && medium.hGlobal) {
    SIZE_T length=GlobalSize(medium.hGlobal);
    if (length>=sizeof(wchar_t) && length<=2*1024*1024+sizeof(wchar_t)) {
      auto value=static_cast<const wchar_t *>(GlobalLock(medium.hGlobal));
      if (value) {
        size_t count=0,limit=length/sizeof(wchar_t);
        while(count<limit && value[count])++count;
        if(count<limit) {
          int bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value,int(count),nullptr,0,nullptr,nullptr);
          if(bytes>=0 && bytes<=1024*1024 && (count==0 || bytes>0)) {
            text.resize(size_t(bytes));
            if(bytes)WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value,int(count),text.data(),bytes,nullptr,nullptr);
          }
        }
        GlobalUnlock(medium.hGlobal);
      }
    }
  }
  ReleaseStgMedium(&medium);
  if (!current() || text.empty()) return 0;
  cancel(active);active=++sequence;accepted=false;
  position["type"]="text_drag_enter";position["drag"]=active;
  position["data"]=base64(reinterpret_cast<const uint8_t *>(text.data()),text.size());
  sendJson(position);return active;
}
inline void motion(uint64_t id, Json position) {
  if(!id || active!=id)return;
  position["type"]="text_drag_motion";position["drag"]=id;sendJson(position);
}
inline bool allowed(uint64_t id) {return id && active==id && accepted;}
inline bool drop(uint64_t id, Json position) {
  if(!allowed(id)){cancel(id);return false;}
  position["type"]="text_drag_drop";position["drag"]=id;sendJson(position);
  active=0;accepted=false;return true;
}
inline bool control(const Json &message) {
  if(message.value("type","")!="text_drag_status")return false;
  if(message.value("drag",uint64_t(0))==active)accepted=message.value("accepted",false);
  return true;
}
}
