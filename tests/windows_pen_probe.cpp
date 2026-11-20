#include <windows.h>
#include "native_pen.h"
#include "touch_window.h"
#include "touch_ole.h"
#include <iostream>
#include <stdexcept>
#include <vector>
static void require(bool value,const char*message){if(!value)throw std::runtime_error(message);}
int main(){try{
 std::vector<NativePen::Json>wire;
 NativePen pen([&](const auto&sample){wire.push_back(sample);});
 HWND window=CreateWindowExW(0,L"STATIC",L"Pen probe",WS_OVERLAPPEDWINDOW,20,20,800,600,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
 require(window!=nullptr,"window");ShowWindow(window,SW_SHOW);SetFocus(window);
 NativePen::Target target{window,7,400,300,12,8,{20,30,620,480},true};
 POINTER_PEN_INFO info{};info.pointerInfo.pointerId=55;info.pointerInfo.pointerFlags=POINTER_FLAG_INRANGE;
 info.pointerInfo.ptPixelLocation={170,180};ClientToScreen(window,&info.pointerInfo.ptPixelLocation);POINT first=info.pointerInfo.ptPixelLocation;
 info.penMask=PEN_MASK_PRESSURE|PEN_MASK_TILT_X|PEN_MASK_TILT_Y|PEN_MASK_ROTATION;
 info.pressure=512;info.tiltX=-25;info.tiltY=40;info.rotation=270;
 require(!pen.sample(target,WM_POINTERENTER,info),"legacy consumed");
 pen.hello(true);require(!pen.sample(target,WM_POINTERENTER,info),"default preference consumed");
 pen.configure(true,2.);pen.sample(target,WM_POINTERENTER,info);
 require(wire.empty(),"unleased input escaped");
 pen.state({{"generation",1},{"enabled",false},{"pending_target",7}});
 pen.state({{"generation",2},{"enabled",true},{"id",7}});
 require(wire.size()==1&&wire[0]["phase"]=="in"&&wire[0]["generation"]==2,"first focus hover");
 require(wire[0]["x"]==100.&&wire[0]["y"]==100.&&wire[0]["pressure"]==.25,"DPI/gamma");
 require(wire[0]["tilt_x"]==-25&&wire[0]["tilt_y"]==40&&wire[0]["rotation"]==270,"valid axes");
 uint64_t gesture=wire.back()["gesture"],tool=wire.back()["tool"];
 info.pointerInfo.pointerFlags|=POINTER_FLAG_INCONTACT;info.penFlags=PEN_FLAG_BARREL;
 pen.sample(target,WM_POINTERDOWN,info);
 require(wire.back()["tip"]==true&&wire.back()["barrel"]==true&&wire.back()["gesture"]==gesture,"tip/barrel identity");
 info.penMask=0;info.pressure=999999;info.tiltX=1000;pen.sample(target,WM_POINTERUPDATE,info);
 require(!wire.back().contains("pressure")&&!wire.back().contains("tilt_x")&&!wire.back().contains("rotation"),"invalid masks leaked");
 info.penMask=PEN_MASK_PRESSURE|PEN_MASK_TILT_X|PEN_MASK_ROTATION;info.rotation=1000;
 pen.sample(target,WM_POINTERUPDATE,info);
 require(!wire.back().contains("pressure")&&!wire.back().contains("tilt_x")&&!wire.back().contains("rotation"),"invalid axis values leaked");
 pen.cancel(window);require(wire.back()["phase"]=="cancel","capture/focus cancel");
 size_t before=wire.size();pen.state({{"generation",3},{"enabled",true},{"id",7}});
 pen.sample(target,WM_POINTERUPDATE,info);require(wire.size()==before,"revoked held tip replayed");
 info.pointerInfo.pointerFlags=POINTER_FLAG_INRANGE;info.penMask=0;info.penFlags=0;
 pen.sample(target,WM_POINTERUP,info);require(wire.back()["phase"]=="in"&&wire.back()["tip"]==false,"hover recovery");
 pen.sample(target,WM_POINTERLEAVE,info);require(wire.back()["phase"]=="out","proximity out");
 info.penFlags=PEN_FLAG_INVERTED;pen.sample(target,WM_POINTERENTER,info);
 require(wire.back()["eraser"]==true&&wire.back()["tool"]==tool&&wire.back()["gesture"].get<uint64_t>()>gesture,"eraser/tool identity");
 require(pen.promotedMouse(window,0xff515700),"promoted pen not suppressed");
 require(!pen.promotedMouse(window,0xff515780)&&!pen.promotedMouse(window,0),"touch or mouse suppressed");
 pen.configure(false);require(!pen.promotedMouse(window,0xff515700)&&!pen.sample(target,WM_POINTERENTER,info),"preference fallback");
 pen.configure(true);pen.state({{"generation",4},{"enabled",true},{"id",7}});target.enabled=false;
 require(!pen.sample(target,WM_POINTERENTER,info),"unsubscribed app consumed");target.enabled=true;
 // Native hover on a background window preserves compatibility and never focuses it.
 HWND background=CreateWindowExW(0,L"STATIC",L"Background pen probe",WS_OVERLAPPEDWINDOW,30,30,400,300,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
 ShowWindow(background,SW_SHOW);SetFocus(background);
 NativePen unfocused;unfocused.configure(true);unfocused.hello(true);unfocused.state({{"generation",1},{"enabled",true},{"id",7}});
 require(!unfocused.sample(target,WM_POINTERENTER,info)&&GetFocus()==background,"unfocused hover consumed or focused");
 SetFocus(window);DestroyWindow(background);
 // A real revoke cannot preserve unsent first-hover/stroke packets.
 NativePen queued([&](const auto&p){wire.push_back(p);});queued.configure(true);queued.hello(true);
 before=wire.size();queued.sample(target,WM_POINTERENTER,info);
 queued.state({{"generation",1},{"enabled",false}});queued.state({{"generation",2},{"enabled",true},{"id",7}});
 require(wire.size()==before,"global revoke replayed pending pen");
 // Capacity is shared across pen/eraser identities and never grows indefinitely.
 NativePen bounded;bounded.configure(true);bounded.hello(true);bounded.state({{"generation",1},{"enabled",true},{"id",7}});
 for(uint32_t i=0;i<16;i++){require(bounded.sample(target,WM_POINTERENTER,info,i),"tool capacity early");bounded.sample(target,WM_POINTERLEAVE,info,i);}
 require(!bounded.sample(target,WM_POINTERENTER,info,16),"tool cache unbounded");
 // Pressure-less hardware does not claim pressure; newly valid axes keep the same stroke.
 std::vector<NativePen::Json>axesWire;NativePen axes([&](const auto&p){axesWire.push_back(p);});axes.configure(true);axes.hello(true);axes.state({{"generation",1},{"enabled",true},{"id",7}});
 info.penMask=0;info.penFlags=0;info.pointerInfo.pointerFlags=POINTER_FLAG_INRANGE|POINTER_FLAG_INCONTACT;
 axes.sample(target,WM_POINTERDOWN,info);require(axesWire.back()["capabilities"]==0&&axesWire.back()["tip"]==true,"pressure-less capability/tip");
 auto axesGesture=axesWire.back()["gesture"];info.penMask=PEN_MASK_PRESSURE;info.pressure=512;
 axes.sample(target,WM_POINTERUPDATE,info);require(axesWire.back()["capabilities"]==1&&axesWire.back()["gesture"]==axesGesture,"late mask restarted native stroke");
 axes.cancel();
 // Production held-tip takeover actually moves/resizes an HWND without any mouse input.
 std::vector<NativePen::Json>windowWire;std::vector<NativePen::WindowInteractionSample>operations;
 NativePen manipulation([&](const auto&p){windowWire.push_back(p);});TouchWindowManipulation geometry;
 bool geometryAccepted=false;
 manipulation.onWindowInteraction([&](const auto&s){operations.push_back(s);if(s.phase=="begin")geometryAccepted=geometry.begin(s.rootWindow,s.kind,s.edge,s.initialWindow,s.initialScreen,s.screen);else if(s.phase=="motion")geometry.update(s.screen);else geometry.end();});
 manipulation.configure(true);manipulation.hello(true);manipulation.state({{"generation",1},{"enabled",true},{"id",7}});
 info.pointerInfo.pointerFlags=POINTER_FLAG_INRANGE|POINTER_FLAG_INCONTACT;info.pointerInfo.ptPixelLocation=first;info.penFlags=0;info.penMask=0;
 manipulation.sample(target,WM_POINTERDOWN,info);
 auto request=[&](uint64_t operation,const char*action,unsigned edge){const auto&p=windowWire.back();return NativePen::Json{{"type","window_action"},{"input","pen"},{"id",7},{"action",action},{"edge",edge},{"pen_operation",operation},{"pen_generation",p["generation"]},{"pen_gesture",p["gesture"]},{"pen_tool",p["tool"]},{"pen_eraser",p["eraser"]}};};
 auto move=request(1,"move",0);auto wrong=move;wrong["pen_tool"]=999;
 require(!manipulation.beginWindowInteraction(wrong,window),"wrong pen tool took window");
 RECT initial{};GetWindowRect(window,&initial);
 require(manipulation.beginWindowInteraction(move,window)&&geometryAccepted,"held-tip move takeover");
 manipulation.state({{"generation",2},{"enabled",false},{"window_operation",1}});
 require(manipulation.ownsWindowInteraction(window),"lease retirement killed authorized move");
 target.enabled=false;info.pointerInfo.ptPixelLocation.x+=70;info.pointerInfo.ptPixelLocation.y+=50;
 require(manipulation.sample(target,WM_POINTERUPDATE,info),"captured move blocked by eligibility");
 RECT moved{};GetWindowRect(window,&moved);require(moved.left==initial.left+70&&moved.top==initial.top+50,"pen coordinates did not move HWND");
 auto other=info;other.pointerInfo.pointerId=56;
 require(!manipulation.sample(target,WM_POINTERUP,other)&&manipulation.ownsWindowInteraction(),"second pointer finished pen move");
 info.pointerInfo.pointerFlags=POINTER_FLAG_INRANGE;manipulation.sample(target,WM_POINTERUP,info);
 require(!manipulation.ownsWindowInteraction()&&operations.back().phase=="up","owner UP did not finish move");
 require(!manipulation.beginWindowInteraction(move,window),"late start after UP accepted");
 target.enabled=true;manipulation.state({{"generation",3},{"enabled",true},{"id",7}});
 info.pointerInfo.pointerFlags|=POINTER_FLAG_INCONTACT;manipulation.sample(target,WM_POINTERDOWN,info);
 auto resize=request(2,"resize",10);require(manipulation.beginWindowInteraction(resize,window)&&geometryAccepted,"held-tip resize takeover");
 manipulation.state({{"generation",4},{"enabled",false},{"window_operation",2}});
 info.pointerInfo.ptPixelLocation.x-=40;info.pointerInfo.ptPixelLocation.y-=30;manipulation.sample(target,WM_POINTERUPDATE,info);
 RECT resized{};GetWindowRect(window,&resized);require(resized.right-resized.left==moved.right-moved.left-40&&resized.bottom-resized.top==moved.bottom-moved.top-30,"pen resize geometry");
 manipulation.state({{"generation",5},{"enabled",false}});
 require(!manipulation.ownsWindowInteraction()&&operations.back().phase=="end","policy revoke preserved pen window operation");
 // Fresh connection may start its own operation numbering; held old tips still wait for UP.
 manipulation.hello(true);manipulation.state({{"generation",1},{"enabled",true},{"id",7}});
 info.pointerInfo.pointerFlags=POINTER_FLAG_INRANGE;manipulation.sample(target,WM_POINTERUP,info);
 info.pointerInfo.pointerFlags|=POINTER_FLAG_INCONTACT;manipulation.sample(target,WM_POINTERDOWN,info);
 require(manipulation.beginWindowInteraction(request(1,"move",0),window),"hello retained old window operation counter");
 manipulation.cancel(window);require(operations.back().phase=="cancel"&&!manipulation.ownsWindowInteraction(),"focus/capture policy cancel");
 std::vector<NativePen::Json> dragWire;
 std::vector<NativePen::WindowInteractionSample> dragEvents;
 NativePen drag([&](const auto&p){dragWire.push_back(p);});
 drag.onWindowInteraction([&](const auto&s){dragEvents.push_back(s);});
 drag.hello(true);drag.configure(true);drag.state({{"generation",1},{"enabled",true},{"id",7}});
 info.pointerInfo.pointerFlags=POINTER_FLAG_INRANGE|POINTER_FLAG_INCONTACT;info.pointerInfo.ptPixelLocation=first;
 require(drag.sample(target,WM_POINTERDOWN,info),"fresh pen drag tip");
 const auto &tip=dragWire.back();
 auto offer=NativePen::Json{{"operation",1},{"id",7},{"generation",tip["generation"]},
   {"gesture",tip["gesture"]},{"pen_tool",tip["tool"]},{"pen_eraser",tip["eraser"]},{"slot",0}};
 auto foreign=offer;foreign["gesture"]=99;
 require(!drag.beginDrag(foreign,window)&&drag.beginDrag(offer,window),"pen drag did not check held tool and gesture");
 drag.state({{"generation",2},{"enabled",false},{"drag_operation",1}});
 require(drag.activeDrag()&&drag.externalDrag(1,true),"marked pen lease retirement lost OLE owner");
 require(TouchOle::Bridge::matchesMouse(PT_PEN,0xff515700) &&
         !TouchOle::Bridge::matchesMouse(PT_PEN,0xff515780) &&
         !TouchOle::Bridge::matchesMouse(PT_PEN,0),"OLE promoted input signature mismatch");
 drag.sample(target,WM_POINTERCAPTURECHANGED,info);
 require(drag.activeDrag()&&drag.ownsDragPointer(info.pointerInfo.pointerId),"OLE capture lost the current pen");
 auto wrongUp=info;wrongUp.pointerInfo.pointerId++;
 require(!drag.sample(target,WM_POINTERUP,wrongUp)&&drag.activeDrag(),"second pointer released pen drag");
 info.pointerInfo.pointerFlags=POINTER_FLAG_INRANGE;
 drag.sample(target,WM_POINTERUP,info);
 require(drag.activeDrag()&&!drag.ownsDragPointer(info.pointerInfo.pointerId)&&dragEvents.back().phase=="up",
         "OLE must keep the release token without a held tip");
 drag.endDrag(0);require(drag.activeDrag().has_value(),"stale end lost the drag");
 drag.endDrag(1);require(!drag.activeDrag()&&dragEvents.back().phase=="end","valid OLE result lost pen drag");
 DestroyWindow(window);std::cout<<"PASS: production pen mask/range/pressure gamma, DPI transform, hover/tip/barrel/eraser identity, leases/focus handoff/revocation, capture recovery, automatic fallback, pressure-less/late axes, held-tip CSD move/resize actual HWNDs, native drag/OLE release ownership, stale/secondary-pointer/policy rejection and 16-device bound\n";return 0;
 }catch(const std::exception&error){std::cerr<<"FAIL: "<<error.what()<<"\n";return 1;}}
