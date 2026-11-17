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
  DestroyWindow(window);
  std::cout<<"PASS: native touch first-tap and marked handoff buffering, revocation/retarget no-replay, startup/live preference cancel and recovery, fractional image geometry, ten slots, motion/up/cancel, stale leases, capture loss and touch-only mouse suppression\n";
  return 0;
 }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
