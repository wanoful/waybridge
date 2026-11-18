#include <windows.h>
#include "native_touch.h"
#include <iostream>
#include <stdexcept>
#include <vector>
static void require(bool value,const char *message){if(!value)throw std::runtime_error(message);}
int main(){
 try{
  std::vector<NativeTouch::Json> sent;
  NativeTouch touch([&](const auto &packet){sent.push_back(packet);});
  HWND window=CreateWindowExW(0,L"STATIC",L"Touch probe",WS_OVERLAPPEDWINDOW,10,10,800,600,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
  require(window!=nullptr,"window");ShowWindow(window,SW_SHOW);SetFocus(window);
  NativeTouch::Target target{window,7,400,300,12,8,{20,30,620,480},true};
  POINT first{170,180};ClientToScreen(window,&first);
  require(!touch.sample(target,WM_POINTERDOWN,1,first),"legacy input consumed");
  touch.hello(true);
  touch.sample(target,WM_POINTERDOWN,1,first);touch.sample(target,WM_POINTERUP,1,first);
  require(sent.empty(),"touch crossed unknown lease");
  touch.state({{"generation",1},{"id",7},{"enabled",true}});
  require(sent.size()==2&&sent[0]["points"][0]["phase"]=="down"&&sent[1]["points"][0]["phase"]=="up","first tap buffering");
  require(sent[0]["points"][0]["x"]==100.&&sent[0]["points"][0]["y"]==100.,"DPI/image transform");
  require(sent[0]["origin_x"]==12&&sent[0]["generation"]==1,"origin/generation");
  // An explicit handoff preserves a first tap while focus negotiation completes.
  std::vector<NativeTouch::Json> queued;
  NativeTouch acquisition([&](const auto &p){queued.push_back(p);});
  acquisition.hello(true);
  acquisition.sample(target,WM_POINTERDOWN,1,first);acquisition.sample(target,WM_POINTERUP,1,first);
  acquisition.state({{"generation",1},{"id",0},{"enabled",false},{"pending_target",7}});
  acquisition.state({{"generation",2},{"id",7},{"enabled",true}});
  require(queued.size()==2&&queued[0]["generation"]==2,"marked initial focus acquisition lost tap");
  // A global policy revoke cannot leave an unsent first tap to replay on regrant.
  queued.clear();
  NativeTouch revoked([&](const auto &p){queued.push_back(p);});
  revoked.hello(true);revoked.sample(target,WM_POINTERDOWN,1,first);revoked.sample(target,WM_POINTERUP,1,first);
  revoked.state({{"generation",1},{"id",0},{"enabled",false}});
  revoked.state({{"generation",2},{"id",7},{"enabled",true}});
  require(queued.empty(),"revoked queued tap replayed on same-window regrant");
  revoked.sample(target,WM_POINTERUPDATE,1,first);revoked.sample(target,WM_POINTERUP,1,first);
  require(queued.empty(),"revoked queued contact survived");
  revoked.sample(target,WM_POINTERDOWN,2,first);revoked.sample(target,WM_POINTERUP,2,first);
  require(queued.size()==2,"fresh tap failed after revocation");
  queued.clear();
  NativeTouch retargeted([&](const auto &p){queued.push_back(p);});
  retargeted.hello(true);retargeted.sample(target,WM_POINTERDOWN,1,first);retargeted.sample(target,WM_POINTERUP,1,first);
  retargeted.state({{"generation",1},{"id",8},{"enabled",true}});
  retargeted.state({{"generation",2},{"id",7},{"enabled",true}});
  require(queued.empty(),"other-target lease retained queued tap");
  NativeTouch wrongHandoff([&](const auto &p){queued.push_back(p);});
  wrongHandoff.hello(true);wrongHandoff.sample(target,WM_POINTERDOWN,1,first);wrongHandoff.sample(target,WM_POINTERUP,1,first);
  wrongHandoff.state({{"generation",1},{"id",0},{"enabled",false},{"pending_target",8}});
  wrongHandoff.state({{"generation",2},{"id",7},{"enabled",true}});
  require(queued.empty(),"wrong-target handoff retained queued tap");
  queued.clear();
  NativeTouch settings([&](const auto &p){queued.push_back(p);});
  settings.enabled(false); settings.hello(true);
  settings.state({{"generation",1},{"id",7},{"enabled",true}});
  require(!settings.sample(target,WM_POINTERDOWN,1,first)&&queued.empty(),"startup preference off consumed touch");
  settings.enabled(true);settings.state({{"generation",2},{"id",7},{"enabled",true}});
  settings.sample(target,WM_POINTERDOWN,1,first);
  require(queued.size()==1&&queued.back()["generation"]==2,"live touch enable");
  settings.enabled(false);
  require(queued.size()==2&&queued.back()["action"]=="cancel","live disable did not cancel contact");
  require(!settings.promotedMouse(window,0xff515780),"disabled preference suppressed compatibility mouse");
  settings.enabled(true);settings.state({{"generation",2},{"id",7},{"enabled",true}});
  size_t disabledSize=queued.size();settings.sample(target,WM_POINTERUPDATE,1,first);
  require(queued.size()==disabledSize,"reenable resumed stale held contact");
  settings.sample(target,WM_POINTERDOWN,2,first);settings.sample(target,WM_POINTERUP,2,first);
  settings.enabled(false);settings.enabled(true);
  settings.state({{"generation",3},{"id",7},{"enabled",true}});
  require(queued.size()==disabledSize,"preference toggle replayed unsent queued tap");
  sent.clear();
  for(UINT i=1;i<=11;++i)touch.sample(target,WM_POINTERDOWN,i,first);
  require(sent.size()==10,"ten contact bound");
  for(unsigned i=0;i<10;++i)require(sent[i]["points"][0]["slot"]==i,"independent slots");
  POINT moved{320,255};ClientToScreen(window,&moved);
  touch.sample(target,WM_POINTERUPDATE,2,moved);
  require(sent.back()["points"][0]["slot"]==1&&sent.back()["points"][0]["x"]==200.,"motion slot");
  touch.cancel(window);require(sent.back()["action"]=="cancel","focus cancellation");
  size_t size=sent.size();touch.sample(target,WM_POINTERUPDATE,1,first);
  require(sent.size()==size,"canceled motion delivered");
  for(UINT i=1;i<=11;++i)touch.sample(target,WM_POINTERUP,i,first);
  touch.state({{"generation",2},{"id",7},{"enabled",true}});
  touch.sample(target,WM_POINTERDOWN,1,first);
  require(sent.back()["generation"]==2,"fresh generation");
  size=sent.size();touch.state({{"generation",1},{"id",7},{"enabled",true}});
  require(sent.size()==size,"stale lease changed input");
  touch.sample(target,WM_POINTERCAPTURECHANGED,1,first);
  require(sent.back()["action"]=="cancel","capture loss");
  require(touch.promotedMouse(window,0xff515780),"promoted touch mouse not suppressed");
  require(!touch.promotedMouse(window,0xff515700)&&!touch.promotedMouse(window,0),"pen/mouse suppressed");
  touch.hello(false);require(!touch.promotedMouse(window,0xff515780),"legacy mouse suppression");
  // Server takeover is bound to a physical gesture and owning slot, rather
  // than a focus HWND or a slot number that can already have been reused.
  SetFocus(window);
  std::vector<NativeTouch::Json> wire;
  std::vector<NativeTouch::InteractionSample> events;
  NativeTouch takeover([&](const auto &p){wire.push_back(p);});
  bool ackBeforeBegin=false;
  takeover.onInteraction([&](const auto &s){
    if(s.phase=="begin")ackBeforeBegin=!wire.empty()&&wire.back().value("type",std::string())=="touch_interaction_ack"&&wire.back().value("accepted",false);
    events.push_back(s);
  });
  takeover.hello(true);takeover.state({{"generation",10},{"id",7},{"enabled",true}});
  takeover.sample(target,WM_POINTERDOWN,101,first);
  uint64_t firstGesture=wire.back()["gesture"];
  require(firstGesture!=0,"missing physical gesture identity");
  takeover.sample(target,WM_POINTERDOWN,102,first);
  takeover.sample(target,WM_POINTERUPDATE,101,moved);
  RECT originalWindow{};GetWindowRect(window,&originalWindow);
  auto start=[&](uint64_t operation,uint64_t physical,uint32_t slot=0,const char *kind="move"){
    // Match serde_json Option::None on the actual server wire: move/DND carry null.
    NativeTouch::Json edge=std::string(kind)=="resize"?NativeTouch::Json(8):NativeTouch::Json(nullptr);
    return NativeTouch::Json{{"operation",operation},{"kind",kind},{"id",7},{"generation",10},{"gesture",physical},{"slot",slot},{"edge",edge}};
  };
  require(takeover.beginInteraction(start(1,firstGesture),window),"valid operation rejected");
  require(ackBeforeBegin&&events.size()==1&&events.back().pointerId==101&&events.back().edge==0,"null-edge move ACK/owner ordering");
  require(events.back().initialScreen.x==first.x&&events.back().screen.x==moved.x&&
          events.back().initialWindow.left==originalWindow.left&&events.back().initialWindow.right==originalWindow.right,
          "takeover lost physical press geometry");
  require(takeover.ownsInteraction(window)&&takeover.ownsPointer(101),"operation ownership unavailable");
  require(takeover.beginInteraction(start(1,firstGesture),window)&&events.size()==1,"duplicate begin replayed callback");
  require(!takeover.beginInteraction(start(1,firstGesture+1),window)&&takeover.ownsInteraction(),"duplicate operation accepted wrong gesture");
  require(!takeover.beginInteraction(start(1,firstGesture,1),window)&&takeover.ownsInteraction(),"duplicate operation accepted another slot");
  HWND elsewhere=CreateWindowExW(0,L"STATIC",L"Other focus",WS_OVERLAPPEDWINDOW,850,10,200,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
  require(elsewhere!=nullptr,"other focus window");ShowWindow(elsewhere,SW_SHOW);SetFocus(elsewhere);
  NativeTouch::Target ineligible{};
  size_t wireCount=wire.size();
  require(takeover.sample(ineligible,WM_POINTERUPDATE,101,first),"captured owner rejected after eligibility change");
  require(events.back().phase=="motion"&&events.back().pointerId==101&&wire.size()==wireCount,"operation emitted ordinary touch");
  size_t eventCount=events.size();
  takeover.sample(ineligible,WM_POINTERUP,102,first);
  takeover.sample(ineligible,WM_POINTERCAPTURECHANGED,102,first);
  takeover.sample(ineligible,WM_POINTERDOWN,103,first);
  require(events.size()==eventCount&&takeover.ownsInteraction()&&!takeover.ownsPointer(103),"second finger altered operation");
  require(!takeover.endInteraction(99)&&takeover.ownsInteraction(),"stale end killed operation");
  require(takeover.promotedMouse(elsewhere,0xff515780)&&!takeover.promotedMouse(window,0xff515700),"takeover touch-only suppression");
  takeover.sample(ineligible,WM_POINTERUP,101,first);
  require(events.size()==eventCount+1&&events.back().phase=="up"&&!takeover.ownsInteraction()&&!takeover.ownsPointer(101),"owner release did not end operation");
  require(!takeover.beginInteraction(start(2,firstGesture),window)&&!wire.back()["accepted"].get<bool>(),"ended contact acknowledged");
  SetFocus(window);
  takeover.sample(target,WM_POINTERDOWN,104,first);
  uint64_t secondGesture=wire.back()["gesture"];
  require(secondGesture!=firstGesture,"new gesture reused old identity");
  require(!takeover.beginInteraction(start(2,firstGesture),window),"old gesture took over reused slot");
  auto withoutEdge=start(2,secondGesture);withoutEdge.erase("edge");
  require(takeover.beginInteraction(withoutEdge,window),"move with omitted edge rejected after stale start");
  require(!takeover.beginInteraction(start(1,firstGesture),window)&&takeover.activeInteraction()->operation==2,"stale operation replaced active operation");
  // External native-to-OLE handoff tolerates OS capture changes and keeps the
  // operation token after UP, without pretending a released pointer is held.
  require(!takeover.externalInteraction(1,true)&&takeover.externalInteraction(2,true),"external handoff token validation");
  require(!takeover.promotedMouse(window,0xff515780),"external handoff suppressed compatibility mouse");
  eventCount=events.size();
  takeover.sample(ineligible,WM_POINTERCAPTURECHANGED,104,first);
  require(takeover.ownsInteraction()&&events.size()==eventCount,"external OS capture transfer canceled operation");
  takeover.sample(ineligible,WM_POINTERUPDATE,104,moved);
  require(events.back().external&&events.back().phase=="motion","external motion lost identity");
  require(takeover.externalInteraction(2,false)&&takeover.promotedMouse(window,0xff515780),"failed external promotion could not restore native mode");
  require(takeover.externalInteraction(2,true),"second external handoff failed");
  takeover.sample(ineligible,WM_POINTERUP,104,moved);
  require(events.back().external&&events.back().phase=="up"&&takeover.ownsInteraction()&&!takeover.ownsPointer(104),"external UP lifetime");
  require(!takeover.externalInteraction(2,false),"released pointer restored native mode");
  require(!takeover.endInteraction(1)&&takeover.endInteraction(2)&&events.back().phase=="end"&&!takeover.ownsInteraction(),"external completion token");
  // Ordinary DOWN/UP before the control request must never be resurrected.
  takeover.sample(target,WM_POINTERDOWN,105,first);uint64_t releasedGesture=wire.back()["gesture"];
  takeover.sample(target,WM_POINTERUP,105,first);
  require(!takeover.beginInteraction(start(3,releasedGesture),window),"down/up before ACK resurrected contact");
  takeover.sample(target,WM_POINTERDOWN,106,first);uint64_t cancelGesture=wire.back()["gesture"];
  for(const auto &edge:NativeTouch::Json::array({nullptr,0,3,12,-1,8.5,"8",uint64_t(-1)})) {
    auto invalid=start(3,cancelGesture,0,"resize");invalid["edge"]=edge;
    require(!takeover.beginInteraction(invalid,window)&&!takeover.ownsInteraction()&&!wire.back().value("accepted",true),"malformed resize edge accepted");
  }
  auto missingResize=start(3,cancelGesture,0,"resize");missingResize.erase("edge");
  require(!takeover.beginInteraction(missingResize,window),"resize missing edge accepted");
  require(takeover.beginInteraction(start(3,cancelGesture,0,"resize"),window),"resize start");
  wireCount=wire.size();eventCount=events.size();takeover.enabled(false);
  require(events.size()==eventCount+1&&events.back().phase=="cancel"&&wire.size()==wireCount&&!takeover.ownsInteraction(),"policy cancel duplicated or lost operation cancel");
  takeover.cancel();require(events.size()==eventCount+1,"operation cancel replayed");
  takeover.enabled(true);takeover.state({{"generation",11},{"id",7},{"enabled",true}});
  takeover.sample(target,WM_POINTERDOWN,107,first);uint64_t revokedGesture=wire.back()["gesture"];
  auto revokedStart=start(4,revokedGesture);revokedStart["generation"]=11;
  require(takeover.beginInteraction(revokedStart,window),"post-disable recovery");
  eventCount=events.size();wireCount=wire.size();
  takeover.state({{"generation",12},{"id",0},{"enabled",false}});
  require(events.size()==eventCount+1&&events.back().phase=="end"&&wire.size()==wireCount&&!takeover.ownsInteraction(),"server revoke echoed operation cancel");
  takeover.state({{"generation",13},{"id",7},{"enabled",true}});
  takeover.sample(target,WM_POINTERDOWN,108,first);auto capturedStart=start(5,wire.back()["gesture"],0,"dnd");capturedStart["generation"]=13;
  require(takeover.beginInteraction(capturedStart,window)&&events.back().edge==0,"null-edge captured DND start");
  eventCount=events.size();wireCount=wire.size();takeover.sample(ineligible,WM_POINTERCAPTURECHANGED,108,first);
  require(events.size()==eventCount+1&&events.back().phase=="cancel"&&wire.size()==wireCount&&!takeover.ownsInteraction(),"owning native capture loss failed to cancel exactly once");
  takeover.sample(target,WM_POINTERDOWN,109,first);
  auto systemStart=start(6,wire.back()["gesture"]);systemStart["generation"]=13;
  require(takeover.beginInteraction(systemStart,window),"system move start rejected");
  require(!takeover.systemMove(5,true)&&takeover.systemMove(6,true),"system move did not bind to its operation");
  eventCount=events.size();
  takeover.sample(ineligible,WM_POINTERCAPTURECHANGED,109,first);
  require(takeover.ownsInteraction()&&events.size()==eventCount,"system move capture handoff canceled the owner");
  require(takeover.systemMove(6,false),"declined system move rejected fallback");
  takeover.sample(ineligible,WM_POINTERUPDATE,109,moved);
  require(events.back().phase=="motion"&&!events.back().systemMove,"declined system move did not restore local motion");
  require(takeover.systemMove(6,true),"system move could not take ownership");
  eventCount=events.size();
  takeover.finishSystemMove(5);
  require(takeover.ownsInteraction()&&events.size()==eventCount,"stale system move completion ended a new gesture");
  takeover.finishSystemMove(6);
  takeover.sample(ineligible,WM_POINTERUP,109,moved);
  require(!takeover.ownsInteraction()&&!takeover.ownsPointer(109)&&events.size()==eventCount+1&&
          events.back().systemMove&&events.back().phase=="up","system move did not report one release");
  // Concrete popup coordinates use the root lease, including a first queued
  // tap when the keyboard focus remains on the root window.
  HWND popup=CreateWindowExW(0,L"STATIC",L"Touch popup",WS_POPUP,200,200,220,180,window,nullptr,GetModuleHandleW(nullptr),nullptr);
  require(popup!=nullptr,"popup window");ShowWindow(popup,SW_SHOWNOACTIVATE);SetFocus(window);
  NativeTouch::Target popupTarget{popup,7,200,160,4,6,{0,0,200,160},true,9,window};
  POINT popupPoint{50,60};ClientToScreen(popup,&popupPoint);
  std::vector<NativeTouch::Json> popupWire;
  NativeTouch popupTouch([&](const auto &p){popupWire.push_back(p);});popupTouch.hello(true);
  popupTouch.sample(popupTarget,WM_POINTERDOWN,201,popupPoint);popupTouch.sample(popupTarget,WM_POINTERUP,201,popupPoint);
  popupTouch.sample(popupTarget,WM_POINTERDOWN,202,popupPoint);popupTouch.sample(popupTarget,WM_POINTERUP,202,popupPoint);
  popupTouch.state({{"generation",1},{"id",0},{"enabled",false},{"pending_target",7}});
  popupTouch.state({{"generation",2},{"id",7},{"enabled",true}});
  require(popupWire.size()==4&&popupWire[0]["id"]==7&&popupWire[0]["target"]==9&&popupWire[0]["generation"]==2&&
          popupWire[0]["origin_x"]==4&&popupWire[0]["points"][0]["x"]==50.,"popup root lease/transform");
  require(popupWire[0]["gesture"]==popupWire[1]["gesture"]&&popupWire[0]["gesture"]!=popupWire[2]["gesture"],"queued taps reused gesture identity");
  DestroyWindow(popup);DestroyWindow(elsewhere);
  DestroyWindow(window);
  std::cout<<"PASS: native touch lease/popup geometry, distinct queued gestures, real null/omitted move/DND edges, malformed resize rejection, takeover ACK ordering, exact owning contact, second-finger isolation, focus-independent motion/up, stale operations, down/up-before-ACK, external capture/UP/completion, system move capture/fallback/release, policy/server cancel and touch-only mouse suppression\n";
  return 0;
 }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
