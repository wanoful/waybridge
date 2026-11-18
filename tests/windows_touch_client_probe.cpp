// Inject only into an exact production client PID/title on an interactive desktop.
#include <windows.h>
#include "third_party/json.hpp"
#include <algorithm>
#include <array>
#include <cstring>
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
    bool expectSnap=argc==4&&std::wstring(argv[3])==L"--expect-snap";
    require(argc==3||expectSnap,"expected PID and exact fixture title");DWORD process=wcstoul(argv[1],nullptr,10);
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
    unsigned frameNumber=0;
    auto inject=[&](std::initializer_list<POINTER_TOUCH_INFO> values){
      std::array<POINTER_TOUCH_INFO,2> frame{};std::copy(values.begin(),values.end(),frame.begin());
      ++frameNumber;
      if(!InjectTouchInput(UINT(values.size()),frame.data())){
        DWORD error=GetLastError();
        Json contacts=Json::array();
        for(const auto &value:values){
          const auto &info=value.pointerInfo;
          contacts.push_back({{"id",info.pointerId},{"flags",info.pointerFlags},
                              {"point",Json::array({info.ptPixelLocation.x,info.ptPixelLocation.y})}});
        }
        result["failed_injection"]={{"frame",frameNumber},{"error",error},{"contacts",contacts}};
        throw std::runtime_error("InjectTouchInput failed: "+std::to_string(error));
      }
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
    // The injector has two slots. Reuse the slots after release; Windows
    // assigns its own pointer identities to the receiving process.
    inject({contact(0,p,down)});Sleep(150);inject({contact(0,q,motion)});Sleep(150);inject({contact(0,q,POINTER_FLAG_UP)});Sleep(250);
    RECT before{};GetWindowRect(window,&before);
    p=sample(3);q={p.x+90,p.y+45};POINT second{p.x+24,p.y+20};
    inject({contact(0,p,down)});Sleep(300);
    inject({contact(0,p,motion),contact(1,second,down)});Sleep(100);
    POINT intermediate{p.x+40,p.y+20};
    inject({contact(0,intermediate,motion),contact(1,second,motion)});Sleep(150);
    inject({contact(0,intermediate,motion),contact(1,second,POINTER_FLAG_UP)});Sleep(100);
    RECT afterSecondUp{};GetWindowRect(window,&afterSecondUp);
    inject({contact(0,q,motion)});Sleep(180);
    RECT moving{};GetWindowRect(window,&moving);
    inject({contact(0,q,POINTER_FLAG_UP)});Sleep(300);
    RECT moved{};GetWindowRect(window,&moved);
    result["move"]={{"before",rect(before)},{"after_second_up",rect(afterSecondUp)},{"while_held",rect(moving)},{"after",rect(moved)}};
    require(abs(moved.left-before.left-90)<=3&&abs(moved.top-before.top-45)<=3,"touch move did not follow original contact");
    require(moving.left>afterSecondUp.left+25,"second-finger release ended owning move");
    require(moved.right-moved.left==before.right-before.left,"touch move resized window");
    p=sample(5);q={p.x+70,p.y+50};GetWindowRect(window,&before);
    inject({contact(0,p,down)});Sleep(300);inject({contact(0,q,motion)});Sleep(200);
    inject({contact(0,q,POINTER_FLAG_UP)});Sleep(500);RECT resized{};GetWindowRect(window,&resized);
    result["resize"]={{"before",rect(before)},{"after",rect(resized)}};
    require(resized.left==before.left&&resized.top==before.top,"bottom-right resize moved anchored origin");
    require(abs((resized.right-resized.left)-(before.right-before.left)-70)<=3&&
            abs((resized.bottom-resized.top)-(before.bottom-before.top)-50)<=3,"touch resize dimensions incorrect");
    // The next independent normal touch proves the lease/contact state recovers.
    p=sample(1);inject({contact(0,p,down)});Sleep(100);inject({contact(0,p,POINTER_FLAG_UP)});Sleep(150);
    if(expectSnap){
      BOOL arranging=FALSE;
      require(SystemParametersInfoW(SPI_GETWINARRANGING,0,&arranging,0)&&arranging,"Windows Snap must be enabled for this opt-in probe");
      auto monitor=MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST);
      MONITORINFO bounds{};bounds.cbSize=sizeof(bounds);
      require(GetMonitorInfoW(monitor,&bounds),"could not find the fixture monitor");
      const auto work=bounds.rcWork;
      LONG middleY=(work.top+work.bottom)/2;
      bool left=!MonitorFromPoint({bounds.rcMonitor.left-1,middleY},MONITOR_DEFAULTTONULL);
      require(left||!MonitorFromPoint({bounds.rcMonitor.right,middleY},MONITOR_DEFAULTTONULL),"fixture monitor has no outer horizontal edge");
      require(SetWindowPos(window,nullptr,work.left+(work.right-work.left-400)/2,middleY-140,400,280,
                           SWP_NOZORDER|SWP_NOACTIVATE),"could not place the fixture away from the edge");
      Sleep(400);
      require(GetForegroundWindow()==window,"fixture lost the foreground before Snap");
      p=sample(3);
      LONG edge=left ? bounds.rcMonitor.left : bounds.rcMonitor.right-1;
      q={edge,p.y};
      GetWindowRect(window,&before);
      inject({contact(0,p,down)});Sleep(300);
      for(int step=1;step<=24;++step){
        const LONG x=p.x+(q.x-p.x)*step/24;
        inject({contact(0,{x,q.y},motion)});Sleep(20);
      }
      Sleep(300);
      inject({contact(0,q,POINTER_FLAG_UP)});
      using IsArranged=BOOL(WINAPI *)(HWND);
      IsArranged arranged=nullptr;
      const auto address=GetProcAddress(GetModuleHandleW(L"user32.dll"),"IsWindowArranged");
      static_assert(sizeof(arranged)==sizeof(address));
      std::memcpy(&arranged,&address,sizeof(arranged));
      RECT snapped{};
      bool geometry=false,systemState=false;
      for(int settle=0;settle<30;++settle){
        Sleep(50);GetWindowRect(window,&snapped);
        LONG halfWidth=(work.right-work.left)/2;
        geometry=abs((left?snapped.left-work.left:snapped.right-work.right))<=32&&
                 abs((snapped.right-snapped.left)-halfWidth)<=32&&
                 abs((snapped.bottom-snapped.top)-(work.bottom-work.top))<=32;
        systemState=arranged&&arranged(window);
        if(geometry&&(!arranged||systemState))break;
      }
      result["snap"]={{"before",rect(before)},{"after",rect(snapped)},{"work_area",rect(work)},
                      {"shell_reports_arranged",arranged?Json(systemState):Json(nullptr)}};
      require(geometry&&(!arranged||systemState),"touch move did not trigger Windows edge Snap");
    }
    result["ok"]=true;result["production_pid"]=process;
  }catch(const std::exception &error){result["ok"]=false;result["error"]=error.what();result["win32_error"]=GetLastError();}
  if(heldCount){for(UINT i=0;i<heldCount;++i)held[i].pointerInfo.pointerFlags=POINTER_FLAG_UP|POINTER_FLAG_CANCELED;InjectTouchInput(heldCount,held.data());}
  if(previous&&IsWindow(previous))SetForegroundWindow(previous);
  std::cout<<result.dump()<<'\n';return result.value("ok",false)?0:1;
}
