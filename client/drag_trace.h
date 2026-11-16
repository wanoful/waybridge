#pragma once
// Opt-in protocol metadata only: never logs the transferred text/file contents.
namespace DragTrace {
inline bool enabled() {return GetEnvironmentVariableW(L"WAYBRIDGE_DND_TRACE",nullptr,0)!=0;}
inline void event(const char *name,Json fields=Json::object()) {
  if(!enabled())return;
  fields["event"]=name;fields["tick"]=GetTickCount64();
  std::cerr<<"DND "<<fields.dump()<<std::endl;
}
inline void formats(IDataObject *data) {
  if(!enabled() || !data)return;
  IEnumFORMATETC *enumerator=nullptr;
  if(FAILED(data->EnumFormatEtc(DATADIR_GET,&enumerator)) || !enumerator)return;
  Json offered=Json::array();FORMATETC format{};
  for(unsigned i=0;i<64 && enumerator->Next(1,&format,nullptr)==S_OK;++i){
    char name[256]{};GetClipboardFormatNameA(format.cfFormat,name,sizeof(name));
    offered.push_back({{"format",format.cfFormat},{"name",format.cfFormat==CF_UNICODETEXT?"CF_UNICODETEXT":name},{"tymed",format.tymed}});
    if(format.ptd)CoTaskMemFree(format.ptd);
    format={};
  }
  enumerator->Release();event("ole.formats",{{"offered",offered}});
}
}
