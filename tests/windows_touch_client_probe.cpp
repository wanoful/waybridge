// Inject only into an exact production client PID/title on an interactive desktop.
#include <windows.h>
#include "third_party/json.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
using Json=nlohmann::json;
static void require(bool ok,const char *message){if(!ok)throw std::runtime_error(message);}
static Json rect(RECT r){return Json::array({r.left,r.top,r.right,r.bottom});}
static POINTER_TOUCH_INFO contact(UINT id,POINT point,POINTER_FLAGS flags){
  POINTER_TOUCH_INFO value{};value.pointerInfo.pointerType=PT_TOUCH;value.pointerInfo.pointerId=id;
  value.pointerInfo.ptPixelLocation=point;value.pointerInfo.pointerFlags=flags;
  value.touchMask=TOUCH_MASK_CONTACTAREA|TOUCH_MASK_PRESSURE|TOUCH_MASK_ORIENTATION;
  value.rcContact={point.x-2,point.y-2,point.x+2,point.y+2};value.orientation=90;value.pressure=512;
  return value;
}
static bool belongs(HWND window,DWORD process){DWORD actual=0;GetWindowThreadProcessId(window,&actual);return actual==process;}
int wmain(int argc,wchar_t **argv){
  HWND previous=GetForegroundWindow(),window=nullptr;Json result;
  std::array<POINTER_TOUCH_INFO,2> held{};UINT heldCount=0;
  try{
    require(argc==3,"expected PID and exact fixture title");DWORD process=wcstoul(argv[1],nullptr,10);
    require(process!=0,"invalid fixture PID");
    for(int i=0;i<100&&!window;++i){window=FindWindowW(L"WaybridgeRemote",argv[2]);if(!window)Sleep(100);}
    require(window&&belongs(window,process),"exact production fixture window unavailable");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ShowWindow(window,SW_RESTORE);SetWindowPos(window,HWND_TOPMOST,160,120,0,0,SWP_NOSIZE|SWP_SHOWWINDOW);
    DWORD foregroundThread=GetWindowThreadProcessId(GetForegroundWindow(),nullptr);
    MSG message{};PeekMessageW(&message,nullptr,0,0,PM_NOREMOVE);
    bool attached=foregroundThread&&foregroundThread!=GetCurrentThreadId()&&AttachThreadInput(GetCurrentThreadId(),foregroundThread,TRUE);
    SetForegroundWindow(window);
    if(attached)AttachThreadInput(GetCurrentThreadId(),foregroundThread,FALSE);
    Sleep(400);result["foreground_before"]=GetForegroundWindow()==window;
    require(InitializeTouchInjection(2,TOUCH_FEEDBACK_NONE),"InitializeTouchInjection failed");
    auto inject=[&](std::initializer_list<POINTER_TOUCH_INFO> values){
      std::array<POINTER_TOUCH_INFO,2> frame{};std::copy(values.begin(),values.end(),frame.begin());
      if(!InjectTouchInput(UINT(values.size()),frame.data()))throw std::runtime_error("InjectTouchInput failed: "+std::to_string(GetLastError()));
      heldCount=0;
      for(const auto &value:values)if(!(value.pointerInfo.pointerFlags&POINTER_FLAG_UP))held[heldCount++]=value;
    };
    auto sample=[&](int fraction){
      RECT client{};GetClientRect(window,&client);POINT point{client.right*fraction/6,client.bottom/2};ClientToScreen(window,&point);
      HWND hit=WindowFromPoint(point);DWORD hitProcess=0;GetWindowThreadProcessId(hit,&hitProcess);
      result["hit_test"]={{"point",Json::array({point.x,point.y})},{"expected",uint64_t(uintptr_t(window))},{"actual",uint64_t(uintptr_t(hit))},{"pid",hitProcess}};
      require(WindowFromPoint(point)==window&&belongs(WindowFromPoint(point),process),"fixture touch point is covered by another window");
      return point;
    };
    const auto down=POINTER_FLAG_DOWN|POINTER_FLAG_INRANGE|POINTER_FLAG_INCONTACT;
    const auto motion=POINTER_FLAG_UPDATE|POINTER_FLAG_INRANGE|POINTER_FLAG_INCONTACT;
    POINT p=sample(1),q{p.x+12,p.y+10};
    inject({contact(71,p,down)});Sleep(150);inject({contact(71,q,motion)});Sleep(150);inject({contact(71,q,POINTER_FLAG_UP)});Sleep(250);
    RECT before{};GetWindowRect(window,&before);
    p=sample(3);q={p.x+90,p.y+45};POINT second{p.x+24,p.y+20};
    inject({contact(72,p,down)});Sleep(300);
    inject({contact(72,p,motion),contact(73,second,down)});Sleep(100);
    POINT intermediate{p.x+40,p.y+20};
    inject({contact(72,intermediate,motion),contact(73,second,motion)});Sleep(150);
    inject({contact(72,intermediate,motion),contact(73,second,POINTER_FLAG_UP)});Sleep(100);
    RECT afterSecondUp{};GetWindowRect(window,&afterSecondUp);
    inject({contact(72,q,motion)});Sleep(180);
    RECT moving{};GetWindowRect(window,&moving);
    inject({contact(72,q,POINTER_FLAG_UP)});Sleep(300);
    RECT moved{};GetWindowRect(window,&moved);
    result["move"]={{"before",rect(before)},{"after_second_up",rect(afterSecondUp)},{"while_held",rect(moving)},{"after",rect(moved)}};
    require(abs(moved.left-before.left-90)<=3&&abs(moved.top-before.top-45)<=3,"touch move did not follow original contact");
    require(moving.left>afterSecondUp.left+25,"second-finger release ended owning move");
    require(moved.right-moved.left==before.right-before.left,"touch move resized window");
    p=sample(5);q={p.x+70,p.y+50};GetWindowRect(window,&before);
    inject({contact(74,p,down)});Sleep(300);inject({contact(74,q,motion)});Sleep(200);
    inject({contact(74,q,POINTER_FLAG_UP)});Sleep(500);RECT resized{};GetWindowRect(window,&resized);
    result["resize"]={{"before",rect(before)},{"after",rect(resized)}};
    require(resized.left==before.left&&resized.top==before.top,"bottom-right resize moved anchored origin");
    require(abs((resized.right-resized.left)-(before.right-before.left)-70)<=3&&
            abs((resized.bottom-resized.top)-(before.bottom-before.top)-50)<=3,"touch resize dimensions incorrect");
    // The next independent normal touch proves the lease/contact state recovers.
    p=sample(1);inject({contact(75,p,down)});Sleep(100);inject({contact(75,p,POINTER_FLAG_UP)});Sleep(150);
    result["ok"]=true;result["production_pid"]=process;
  }catch(const std::exception &error){result["ok"]=false;result["error"]=error.what();result["win32_error"]=GetLastError();}
  if(heldCount){for(UINT i=0;i<heldCount;++i)held[i].pointerInfo.pointerFlags=POINTER_FLAG_UP|POINTER_FLAG_CANCELED;InjectTouchInput(heldCount,held.data());}
  if(previous&&IsWindow(previous))SetForegroundWindow(previous);
  std::cout<<result.dump()<<'\n';return result.value("ok",false)?0:1;
}
