#pragma once
#include <windows.h>
#include "third_party/json.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>

// Controller-thread PT_PEN -> leased tablet-v2 samples. Unsupported applications
// return false so their existing Windows pen-to-mouse compatibility keeps working.
class NativePen {
public:
  using Json = nlohmann::json;
  struct Target {
    HWND hwnd=nullptr; uint32_t id=0;
    int width=0,height=0,originX=0,originY=0; RECT image{}; bool enabled=false;
    uint32_t targetId=0; HWND rootWindow=nullptr;
  };
  struct WindowInteractionSample {
    uint64_t operation=0,generation=0,gesture=0,tool=0;
    bool eraser=false, external=false;
    uint32_t id=0,edge=0;
    UINT pointerId=0;
    std::string kind,phase;
    HWND hwnd=nullptr,rootWindow=nullptr;
    POINT screen{},initialScreen{};
    RECT initialWindow{};
  };
private:
  std::function<void(const Json&)> emit;
  std::function<void(const WindowInteractionSample&)> windowCallback;
  std::optional<WindowInteractionSample> windowInteraction;
  uint64_t lastWindowOperation=0, lastDragOperation=0;
  POINT lastScreen{},downScreen{};
  RECT downWindow{};
  bool negotiated=false,requested=false,sent=false,inRange=false,tip=false;
  double gamma=1.;
  Target owner;
  UINT pointer=0,blockedPointer=0;
  uint64_t generation=0,lastGeneration=0,gesture=0,lastGesture=0,sequence=0;
  uint64_t tool=0,lastTool=0,pendingStarted=0,suppressUntil=0;
  uint32_t leaseId=0;
  bool eraser=false;
  HWND suppressedWindow=nullptr;
  std::deque<Json> pending;
  struct ToolIdentity { uint64_t id; uint32_t axes; };
  uint32_t toolCapabilities=0;
  std::map<std::pair<uintptr_t,uint32_t>,ToolIdentity> tools;
  static HWND root(const Target&t){return t.rootWindow?t.rootWindow:t.hwnd;}
  static uint32_t destination(const Target&t){return t.targetId?t.targetId:t.id;}
  static bool focused(const Target&t){HWND f=GetFocus(),w=root(t);return f==t.hwnd||f==w||(f&&w&&IsChild(w,f));}
  void clear(){
    if(tip)blockedPointer=pointer;
    if(owner.hwnd){suppressedWindow=root(owner);suppressUntil=GetTickCount64()+500;}
    owner={};pointer=0;sent=false;inRange=false;tip=false;gesture=sequence=tool=0;
    pending.clear();pendingStarted=0;
  }
  bool packet(Json value){
    if(leaseId==owner.id&&generation){value["generation"]=generation;if(emit)emit(value);sent=true;}
    else{
      if(!pendingStarted)pendingStarted=GetTickCount64();
      if(pending.size()>=64||GetTickCount64()-pendingStarted>500){cancel();return false;}
      pending.push_back(std::move(value));
    }
    return true;
  }
  Json envelope(const char*phase){return {{"type","pen"},{"id",owner.id},{"target",destination(owner)},
    {"gesture",gesture},{"sequence",++sequence},{"tool",tool},{"eraser",eraser},{"phase",phase},{"capabilities",toolCapabilities},
    {"origin_x",owner.originX},{"origin_y",owner.originY}};}
  bool windowSample(UINT message,const POINTER_PEN_INFO&info){
    if(!windowInteraction||windowInteraction->pointerId!=info.pointerInfo.pointerId)return false;
    bool canceled=(info.pointerInfo.pointerFlags&POINTER_FLAG_CANCELED)!=0;
    if(message==WM_POINTERDOWN||windowInteraction->phase=="up"||
       (message==WM_POINTERCAPTURECHANGED&&windowInteraction->external&&!canceled))return true;
    auto sample=*windowInteraction;sample.screen=info.pointerInfo.ptPixelLocation;
    sample.phase=message==WM_POINTERCAPTURECHANGED||canceled?"cancel":
      message==WM_POINTERUP?"up":message==WM_POINTERLEAVE&&!(info.pointerInfo.pointerFlags&POINTER_FLAG_INCONTACT)?"cancel":"motion";
    if(sample.phase=="up"||sample.phase=="cancel"){
      if(sample.phase=="up"&&sample.external)windowInteraction=sample;
      else windowInteraction.reset();
      clear();
      blockedPointer=sample.phase=="cancel"?sample.pointerId:0;
    }else *windowInteraction=sample;
    suppressUntil=GetTickCount64()+500;suppressedWindow=sample.rootWindow;
    if(windowCallback)windowCallback(sample);
    return true;
  }
  bool heldTip(HWND sourceRoot) const {
    return negotiated && requested && !windowInteraction && inRange && tip && sent && pending.empty() &&
      generation && IsWindow(owner.hwnd) && IsWindow(root(owner)) && sourceRoot == root(owner);
  }
  void beginInteraction(uint64_t operation, std::string kind, uint32_t edge=0) {
    WindowInteractionSample sample;
    sample.operation=operation;sample.generation=generation;sample.gesture=gesture;sample.tool=tool;sample.eraser=eraser;
    sample.id=owner.id;sample.edge=edge;sample.pointerId=pointer;sample.kind=std::move(kind);sample.phase="begin";
    sample.hwnd=owner.hwnd;sample.rootWindow=root(owner);sample.screen=lastScreen;sample.initialScreen=downScreen;sample.initialWindow=downWindow;
    windowInteraction=sample;
    // Park the live physical owner while the server retires its tablet tip.
    owner={};pointer=0;sent=false;inRange=false;tip=false;gesture=sequence=tool=0;pending.clear();pendingStarted=0;
  }
public:
  explicit NativePen(std::function<void(const Json&)>sender={}):emit(std::move(sender)){}
  void configure(bool enabled,double pressureGamma=1.){
    double next=std::isfinite(pressureGamma)?std::clamp(pressureGamma,.25,4.):1.;
    if(requested!=enabled||(inRange&&gamma!=next))cancel();
    requested=enabled;gamma=next;
    if(!requested){suppressedWindow=nullptr;suppressUntil=0;}
  }
  void hello(bool available){cancel(nullptr,false);negotiated=available;leaseId=0;generation=lastGeneration=0;tools.clear();lastTool=lastGesture=lastWindowOperation=lastDragOperation=0;}
  bool active()const{return negotiated&&requested&&((inRange&&sent)||windowInteraction.has_value());}
  void onWindowInteraction(std::function<void(const WindowInteractionSample&)>callback){windowCallback=std::move(callback);}
  bool ownsWindowInteraction(HWND window=nullptr)const{return windowInteraction&&(!window||windowInteraction->hwnd==window||windowInteraction->rootWindow==window);}
  std::optional<WindowInteractionSample> activeDrag() const {
    return windowInteraction && windowInteraction->kind=="dnd" ? windowInteraction : std::nullopt;
  }
  bool ownsDragPointer(UINT id) const {
    return windowInteraction && windowInteraction->kind=="dnd" &&
           windowInteraction->pointerId==id && windowInteraction->phase!="up";
  }
  bool externalDrag(uint64_t operation,bool external) {
    if(!windowInteraction || windowInteraction->kind!="dnd" || windowInteraction->operation!=operation ||
       windowInteraction->phase=="up")return false;
    windowInteraction->external=external;
    return true;
  }
  bool ownsExternalDrag(HWND window=nullptr) const {
    return ownsWindowInteraction(window) && windowInteraction->kind=="dnd" && windowInteraction->external;
  }
  bool beginDrag(const Json &value, HWND sourceRoot) {
    uint64_t operation=value.value("operation",uint64_t(0));
    if(!operation || operation<=lastDragOperation || !heldTip(sourceRoot) ||
       value.value("id",uint32_t(0))!=owner.id || value.value("generation",uint64_t(0))!=generation ||
       value.value("gesture",uint64_t(0))!=gesture || value.value("pen_tool",uint64_t(0))!=tool ||
       !value.contains("pen_eraser") || value["pen_eraser"]!=eraser || value.value("slot",uint32_t(1))!=0)
      return false;
    lastDragOperation=operation;
    beginInteraction(operation,"dnd");
    return true;
  }
  void endDrag(uint64_t operation) {
    if(windowInteraction && windowInteraction->kind=="dnd" && windowInteraction->operation==operation)
      cancel(nullptr,false);
  }
  bool beginWindowInteraction(const Json&value,HWND sourceRoot){
    uint64_t operation=value.value("pen_operation",uint64_t(0));
    std::string kind=value.value("action",std::string());
    uint32_t edge=value.contains("edge")&&value["edge"].is_number_integer()?value["edge"].get<uint32_t>():0;
    if(!heldTip(sourceRoot)||!operation||operation<=lastWindowOperation||
      (kind!="move"&&kind!="resize")||(kind=="resize"&&(!edge||(edge&~15u)||(edge&3u)==3u||(edge&12u)==12u))||
      value.value("id",uint32_t(0))!=owner.id||value.value("pen_generation",uint64_t(0))!=generation||
      value.value("pen_gesture",uint64_t(0))!=gesture||value.value("pen_tool",uint64_t(0))!=tool||value.value("pen_eraser",false)!=eraser)return false;
    lastWindowOperation=operation;
    beginInteraction(operation,std::move(kind),edge);
    auto sample=*windowInteraction;
    if(windowCallback)windowCallback(sample);
    return true;
  }
  void cancel(HWND window=nullptr,bool notify=true){
    if(window&&owner.hwnd!=window&&root(owner)!=window&&!ownsWindowInteraction(window))return;
    auto operation=windowInteraction;windowInteraction.reset();
    bool send=notify&&sent&&owner.id&&generation;
    Json value=send?envelope("cancel"):Json();
    if(send)value["generation"]=generation;
    clear();leaseId=0;generation=0;
    if(send&&emit)emit(value);
    if(operation){
      blockedPointer=operation->phase=="up"?0:operation->pointerId;
      suppressedWindow=operation->hwnd;suppressUntil=GetTickCount64()+500;
      operation->phase=notify?"cancel":"end";if(windowCallback)windowCallback(*operation);
    }
  }
  void state(const Json&value){
    uint64_t next=value.value("generation",uint64_t(0));
    if(!negotiated||!next||next<=lastGeneration)return;
    if(!requested){cancel(nullptr,false);lastGeneration=next;return;}
    if(windowInteraction){
      auto field=windowInteraction->kind=="dnd"?"drag_operation":"window_operation";
      if(value.value(field,uint64_t(0))==windowInteraction->operation){lastGeneration=generation=next;leaseId=0;return;}
      cancel(nullptr,false);
    }
    bool enabled=value.value("enabled",false);uint32_t id=enabled?value.value("id",uint32_t(0)):0;
    bool acquiring=!enabled&&owner.id&&!pending.empty()&&value.value("pending_target",uint32_t(0))==owner.id&&
      IsWindow(owner.hwnd)&&focused(owner)&&GetTickCount64()-pendingStarted<=500;
    if((sent&&(id!=leaseId||next!=generation))||(!pending.empty()&&((!enabled&&!acquiring)||(enabled&&id!=owner.id))))cancel(nullptr,false);
    lastGeneration=generation=next;leaseId=id;
    if(!pending.empty()&&id==owner.id){
      if(!IsWindow(owner.hwnd)||!focused(owner)||GetTickCount64()-pendingStarted>500){cancel();return;}
      while(!pending.empty()){auto item=std::move(pending.front());pending.pop_front();item["generation"]=generation;if(emit)emit(item);sent=true;}
      pendingStarted=0;if(!inRange){owner={};sent=false;gesture=0;}
    }
  }
  // Production and the standalone probe use the same Windows mask validation,
  // coordinate transform, device identity and proximity/lease state machine.
  bool sample(const Target&target,UINT message,const POINTER_PEN_INFO&info,uint32_t cursorId=0){
    if(windowSample(message,info))return true;
    if(windowInteraction||!negotiated||!requested||!target.enabled)return false;
    UINT id=info.pointerInfo.pointerId;
    bool contact=(info.pointerInfo.pointerFlags&POINTER_FLAG_INCONTACT)!=0&&message!=WM_POINTERUP;
    bool range=(info.pointerInfo.pointerFlags&POINTER_FLAG_INRANGE)!=0&&message!=WM_POINTERLEAVE;
    if(blockedPointer==id){if(contact)return true;blockedPointer=0;}
    if((message==WM_POINTERCAPTURECHANGED||(info.pointerInfo.pointerFlags&POINTER_FLAG_CANCELED))){
      if(pointer==id)cancel();
      return true;
    }
    if(owner.hwnd&&pointer!=id)return true;
    if(!range){
      if(owner.hwnd&&pointer==id){packet(envelope("out"));inRange=false;tip=false;if(pending.empty()){owner={};sent=false;gesture=0;}}
      return true;
    }
    POINT imagePoint=info.pointerInfo.ptPixelLocation;ScreenToClient(target.hwnd,&imagePoint);
    if(!contact&&!tip&&!PtInRect(&target.image,imagePoint)){
      if(inRange){packet(envelope("out"));inRange=false;sent=false;owner={};gesture=0;}
      return true;
    }
    bool nextEraser=(info.penFlags&(PEN_FLAG_INVERTED|PEN_FLAG_ERASER))!=0;
    auto identity=std::make_pair(reinterpret_cast<uintptr_t>(info.pointerInfo.sourceDevice),cursorId);
    uint32_t validAxes=0;
    if((info.penMask&PEN_MASK_PRESSURE)&&info.pressure<=1024)validAxes|=PEN_MASK_PRESSURE;
    if((info.penMask&PEN_MASK_TILT_X)&&info.tiltX>=-90&&info.tiltX<=90)validAxes|=PEN_MASK_TILT_X;
    if((info.penMask&PEN_MASK_TILT_Y)&&info.tiltY>=-90&&info.tiltY<=90)validAxes|=PEN_MASK_TILT_Y;
    if((info.penMask&PEN_MASK_ROTATION)&&info.rotation<=359)validAxes|=PEN_MASK_ROTATION;
    auto known=tools.find(identity);
    if(inRange&&(owner.id!=target.id||root(owner)!=root(target)||known==tools.end()||tool!=known->second.id||eraser!=nextEraser)){
      packet(envelope("out"));inRange=false;tip=false;sent=false;owner={};
    }
    if(!inRange){
      if(!IsWindow(target.hwnd)||!IsWindow(root(target))||!focused(target)||target.width<=0||target.height<=0||
        target.image.right<=target.image.left||target.image.bottom<=target.image.top||!PtInRect(&target.image,imagePoint))return false;
      auto found=tools.find(identity);
      if(found==tools.end()){if(tools.size()>=16)return false;found=tools.emplace(identity,ToolIdentity{++lastTool,validAxes}).first;}
      found->second.axes|=validAxes;toolCapabilities=found->second.axes;
      owner=target;pointer=id;tool=found->second.id;eraser=nextEraser;gesture=++lastGesture;sequence=0;inRange=true;sent=false;
    }else {owner=target;auto &knownTool=tools.at(identity);knownTool.axes|=validAxes;toolCapabilities=knownTool.axes;}
    POINT local=info.pointerInfo.ptPixelLocation;ScreenToClient(target.hwnd,&local);
    if(target.image.right<=target.image.left||target.image.bottom<=target.image.top){cancel();return true;}
    lastScreen=info.pointerInfo.ptPixelLocation;
    if(contact&&!tip){downScreen=lastScreen;GetWindowRect(root(target),&downWindow);}
    Json value=envelope(sequence?"motion":"in");
    value["x"]=double(local.x-target.image.left)*target.width/(target.image.right-target.image.left);
    value["y"]=double(local.y-target.image.top)*target.height/(target.image.bottom-target.image.top);
    value["tip"]=contact;value["barrel"]=(info.penFlags&PEN_FLAG_BARREL)!=0;
    if((info.penMask&PEN_MASK_PRESSURE)&&info.pressure<=1024)value["pressure"]=std::pow(double(info.pressure)/1024.,gamma);
    if((info.penMask&PEN_MASK_TILT_X)&&info.tiltX>=-90&&info.tiltX<=90)value["tilt_x"]=info.tiltX;
    if((info.penMask&PEN_MASK_TILT_Y)&&info.tiltY>=-90&&info.tiltY<=90)value["tilt_y"]=info.tiltY;
    if((info.penMask&PEN_MASK_ROTATION)&&info.rotation<=359)value["rotation"]=info.rotation;
    if(!packet(std::move(value))){if(contact)blockedPointer=id;return true;}
    tip=contact;suppressedWindow=root(target);suppressUntil=GetTickCount64()+500;
    return true;
  }
  bool handle(const Target&target,UINT message,WPARAM wp){
    if(message!=WM_POINTERENTER&&message!=WM_POINTERUPDATE&&message!=WM_POINTERDOWN&&message!=WM_POINTERUP&&
      message!=WM_POINTERLEAVE&&message!=WM_POINTERCAPTURECHANGED)return false;
    UINT id=GET_POINTERID_WPARAM(wp);
    if(windowInteraction){
      if(windowInteraction->pointerId!=id)return false;
      if(message==WM_POINTERCAPTURECHANGED&&windowInteraction->external){
        POINTER_INFO data{};
        if(GetPointerInfo(id,&data)&&(data.pointerFlags&POINTER_FLAG_CANCELED))cancel();
        return true;
      }
      if(message==WM_POINTERCAPTURECHANGED){cancel();return true;}
      POINTER_PEN_INFO info{};
      if(!GetPointerPenInfo(id,&info)){if(windowInteraction->phase!="up")cancel();return true;}
      return windowSample(message,info);
    }
    if(message==WM_POINTERCAPTURECHANGED&&pointer==id&&owner.hwnd){cancel();return true;}
    POINTER_INPUT_TYPE type=PT_POINTER;
    if(!GetPointerType(id,&type)||type!=PT_PEN)return false;
    POINTER_PEN_INFO info{};if(!GetPointerPenInfo(id,&info)){if(pointer==id)cancel();return negotiated&&requested&&target.enabled;}
    UINT32 cursor=0;GetPointerCursorId(id,&cursor);
    return sample(target,message,info,cursor);
  }
  bool promotedMouse(HWND window,ULONG_PTR extra)const{
    return negotiated&&requested&&(extra&0xffffff80u)==0xff515700u&&
      (((window==suppressedWindow||window==owner.hwnd)&&GetTickCount64()<=suppressUntil)||
       ownsWindowInteraction(window));
  }
};
