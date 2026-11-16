// Isolated Win32 focus probe. Only its own test HWNDs are clicked/destroyed.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "window_activation.h"
#include <iostream>
#include <stdexcept>
static void require(bool ok,const char *why) { if(!ok) throw std::runtime_error(why); }
static void pump() { MSG m{}; while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);} Sleep(30); }
static void click(HWND hwnd) {
  ShowWindow(hwnd,SW_SHOW); SetForegroundWindow(hwnd);
  SetWindowPos(hwnd,HWND_TOP,120,120,520,360,SWP_NOACTIVATE);
  for(int i=0;i<10;i++)pump();RECT r{};GetClientRect(hwnd,&r);POINT p{r.right/2,r.bottom/2};ClientToScreen(hwnd,&p);
  require(GetAncestor(WindowFromPoint(p),GA_ROOT)==hwnd,"probe click is not over its own HWND");
  SetCursorPos(p.x,p.y);INPUT input[2]{};input[0].type=input[1].type=INPUT_MOUSE;
  input[0].mi.dwFlags=MOUSEEVENTF_LEFTDOWN;input[1].mi.dwFlags=MOUSEEVENTF_LEFTUP;
  require(SendInput(2,input,sizeof(INPUT))==2,"probe self-click failed");pump();
  require(GetForegroundWindow()==hwnd,"self-click did not activate probe");
}
int main() {
  POINT previous{};GetCursorPos(&previous);
  HWND initialForeground=GetForegroundWindow();
  HWND a=nullptr,b=nullptr,dialog=nullptr;
  try {
    WNDCLASSW cls{};cls.lpfnWndProc=DefWindowProcW;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"WaybridgeFocusProbe";RegisterClassW(&cls);
    auto create=[&](const wchar_t *title,HWND owner) { return CreateWindowExW(0,cls.lpszClassName,title,WS_OVERLAPPEDWINDOW,120,120,520,360,owner,nullptr,cls.hInstance,nullptr); };
    a=create(L"Waybridge isolated focus A",nullptr);b=create(L"Waybridge isolated focus B",nullptr);dialog=create(L"Waybridge isolated modal",a);
    require(a&&b&&dialog,"create probe windows");
    ShowWindow(b,SW_SHOWNOACTIVATE);click(a);
    NativeActivation activation;activation.hello(true);std::string first(32,'a');
    require(activation.prepare(first,true),"prepare user intent");
    activation.request({{"id",2u},{"request_id",first}},b,nullptr,false);
    activation.shown(2,b);pump();require(GetForegroundWindow()==b,"explicit deferred activation did not foreground target");
    NativeActivation::user(a,true);pump();
    activation.request({{"id",2u},{"request_id",first}},b,nullptr,true);pump();require(GetForegroundWindow()==a,"consumed intent replayed");
    std::string switched(32,'b');activation.prepare(switched,true);
    click(b);
    activation.request({{"id",1u},{"request_id",switched}},a,nullptr,true);pump();require(GetForegroundWindow()==b,"user switch was overridden by old launch");
    activation.request({{"id",3u}},dialog,a,true);pump();require(GetForegroundWindow()==b,"background parent request stole focus");
    NativeActivation::user(a,true);pump();EnableWindow(a,FALSE);NativeActivation::user(dialog,true);pump();require(GetForegroundWindow()==dialog,"modal did not activate");
    EnableWindow(a,TRUE);require(NativeActivation::restoreOwner(dialog,a),"modal owner restore refused");ShowWindow(dialog,SW_HIDE);pump();require(GetForegroundWindow()==a,"hiding modal lost owner focus");
    EnableWindow(a,FALSE);NativeActivation::user(dialog,true);pump();EnableWindow(a,TRUE);require(NativeActivation::restoreOwner(dialog,a),"destroy owner restore refused");DestroyWindow(dialog);dialog=nullptr;pump();require(GetForegroundWindow()==a,"destroying modal lost owner focus");
    std::string old(32,'c');activation.prepare(old,true);activation.hello(true);
    activation.request({{"id",2u},{"request_id",old}},b,nullptr,true);pump();require(GetForegroundWindow()==a,"old connection intent survived");
    if(IsWindow(initialForeground))SetForegroundWindow(initialForeground);
    DestroyWindow(b);b=nullptr;DestroyWindow(a);a=nullptr;SetCursorPos(previous.x,previous.y);
    std::cout<<"PASS: deferred explicit activation, consumed intent and user-switch rejection, background parent refusal, modal hide/destroy owner focus restoration, connection reset\n";return 0;
  } catch(const std::exception &error) {
    if((GetForegroundWindow()==a||GetForegroundWindow()==b||GetForegroundWindow()==dialog)&&IsWindow(initialForeground))SetForegroundWindow(initialForeground);
    if(dialog)DestroyWindow(dialog);if(b)DestroyWindow(b);if(a)DestroyWindow(a);SetCursorPos(previous.x,previous.y);
    std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;
  }
}
